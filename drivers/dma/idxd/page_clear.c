// SPDX-License-Identifier: GPL-2.0
/* Synchronous init_on_free offload using a dedicated kernel DSA work queue. */
#include <linux/dma-mapping.h>
#include <linux/kasan.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <asm/dsa.h>

#include "idxd.h"

#define DSA_CLEAR_TIMEOUT_NS NSEC_PER_SEC

static unsigned int min_pages = 1;
module_param(min_pages, uint, 0644);
MODULE_PARM_DESC(min_pages, "Minimum number of contiguous pages to offload");

static bool cache_control;
module_param(cache_control, bool, 0644);
MODULE_PARM_DESC(cache_control, "Set the DSA cache-control hint on new fills (default: false)");

static bool unsafe_async;
module_param(unsafe_async, bool, 0644);
MODULE_PARM_DESC(unsafe_async, "UNSAFE: return after submission; DMA may overwrite reused pages");

static atomic64_t completed_pages = ATOMIC64_INIT(0);
static atomic64_t submitted_pages = ATOMIC64_INIT(0);
static atomic64_t pending_pages = ATOMIC64_INIT(0);
static atomic64_t async_fallback_pages = ATOMIC64_INIT(0);
static atomic64_t async_error_pages = ATOMIC64_INIT(0);

static int page_count_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lld\n", atomic64_read(kp->arg));
}

static const struct kernel_param_ops page_count_ops = {
	.get = page_count_get,
};
module_param_cb(completed_pages, &page_count_ops, &completed_pages, 0444);
MODULE_PARM_DESC(completed_pages, "Pages in DSA fills reporting success (not a reuse-safety guarantee)");
module_param_cb(submitted_pages, &page_count_ops, &submitted_pages, 0444);
MODULE_PARM_DESC(submitted_pages, "Pages submitted with unsafe early return");
module_param_cb(pending_pages, &page_count_ops, &pending_pages, 0444);
MODULE_PARM_DESC(pending_pages, "Pages in unsafe requests awaiting resource retirement; not isolated");
module_param_cb(async_fallback_pages, &page_count_ops, &async_fallback_pages, 0444);
MODULE_PARM_DESC(async_fallback_pages, "Pages rejected by unsafe mode and returned to CPU fallback");
module_param_cb(async_error_pages, &page_count_ops, &async_error_pages, 0444);
MODULE_PARM_DESC(async_error_pages, "Pages in failed unsafe fills; no CPU repair is attempted");

struct idxd_page_clear_request {
	struct work_struct work;
	struct idxd_desc *desc;
	dma_addr_t dma;
	unsigned int npages;
	u64 start;
};

struct idxd_page_clear {
	struct dsa_page_clear_ops ops;
	struct idxd_wq *wq;
	struct workqueue_struct *retire_wq;
	struct idxd_page_clear_request *requests;
};

/* Retire DMA resources only. The allocator already owns the destination. */
static void idxd_retire_unsafe_fill(struct work_struct *work)
{
	struct idxd_page_clear_request *request =
		container_of(work, struct idxd_page_clear_request, work);
	struct idxd_desc *desc = request->desc;
	struct idxd_wq *wq = desc->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	unsigned int npages = request->npages;
	u8 status;

	/* Completion is needed to recycle the descriptor/mapping, not the page. */
	for (;;) {
		status = READ_ONCE(desc->completion->status);
		if (DSA_COMP_STATUS(status))
			break;
		if (ktime_get_mono_fast_ns() - request->start > DSA_CLEAR_TIMEOUT_NS)
			panic("DSA unsafe fill timed out on %s; DMA resources still active",
			      dev_name(wq_confdev(wq)));
		cpu_relax();
		cond_resched();
	}
	dma_rmb();
	kasan_disable_current();
	dma_unmap_page(dev, request->dma, (size_t)npages * PAGE_SIZE, DMA_FROM_DEVICE);
	kasan_enable_current();
	if (DSA_COMP_STATUS(status) == DSA_COMP_SUCCESS)
		atomic64_add(npages, &completed_pages);
	else
		atomic64_add(npages, &async_error_pages);
	/* Never touch/clear/free the destination here: it may have a new owner. */
	idxd_free_desc(wq, desc);
	percpu_ref_put(&wq->wq_active);
	atomic64_sub(npages, &pending_pages);
}

static bool idxd_clear_pages(struct dsa_page_clear_ops *ops, void *addr,
			     unsigned int npages)
{
	struct idxd_page_clear *clear = container_of(ops, struct idxd_page_clear, ops);
	struct idxd_wq *wq = clear->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	size_t len = (size_t)npages * PAGE_SIZE;
	struct idxd_desc *desc;
	struct idxd_page_clear_request *request;
	dma_addr_t dma;
	u64 start;
	u8 status;
	bool cleared = false;
	bool early_return = READ_ONCE(unsafe_async);

	if (!npages || npages < READ_ONCE(min_pages) ||
	    len > wq->max_xfer_bytes || len > U32_MAX ||
	    len > dma_max_mapping_size(dev) ||
	    READ_ONCE(wq->state) != IDXD_WQ_ENABLED)
		goto out_fallback;

	/* Hold the WQ live until both completion and DMA unmapping finish. */
	if (!percpu_ref_tryget_live(&wq->wq_active))
		goto out_fallback;

	desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
	if (IS_ERR(desc))
		goto out_ref;

	/* The caller supplies a contiguous, direct-mapped page range. */
	dma = dma_map_page(dev, virt_to_page(addr), 0, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dma))
		goto out_desc;

	desc->hw->opcode = DSA_OPCODE_MEMFILL;
	desc->hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (READ_ONCE(cache_control))
		desc->hw->flags |= IDXD_OP_FLAG_CC;
	desc->hw->pattern = 0;
	desc->hw->dst_addr = dma;
	desc->hw->xfer_size = len;
	desc->hw->completion_addr = desc->compl_dma;
	/* Kernel DMA uses user privilege in the kernel's DMA address space. */
	desc->hw->priv = 0;

	if (early_return) {
		request = &clear->requests[desc->id];
		request->desc = desc;
		request->dma = dma;
		request->npages = npages;
		request->start = ktime_get_mono_fast_ns();
	}

	if (idxd_submit_desc_nowait(wq, desc))
		goto out_unmap;

	if (early_return) {
		atomic64_add(npages, &submitted_pages);
		atomic64_add(npages, &pending_pages);
		queue_work(clear->retire_wq, &request->work);
		/*
		 * Deliberately violates the old synchronous provider contract.
		 * The page may be reused BEFORE this DMA fill finishes. This is
		 * an unsafe submission-cost experiment, not valid init_on_free.
		 */
		return true;
	}

	/*
	 * No completion interrupt: this caller owns and frees the descriptor.
	 * Never return on timeout: DMA could still overwrite a reused page.
	 */
	start = ktime_get_mono_fast_ns();
	for (;;) {
		status = READ_ONCE(desc->completion->status);
		if (DSA_COMP_STATUS(status))
			break;
		if (ktime_get_mono_fast_ns() - start > DSA_CLEAR_TIMEOUT_NS)
			panic("DSA page clear timed out on %s; DMA may still be active",
			      dev_name(wq_confdev(wq)));
		cpu_relax();
	}
	dma_rmb();
	cleared = DSA_COMP_STATUS(status) == DSA_COMP_SUCCESS;
	if (!cleared)
		dev_warn_ratelimited(dev, "page clear failed: status %#x; using CPU\n",
				     status);

out_unmap:
	/* Also handles bounce buffers before CPU fallback overwrites the range. */
	dma_unmap_page(dev, dma, len, DMA_FROM_DEVICE);
	if (cleared)
		atomic64_add(npages, &completed_pages);
out_desc:
	idxd_free_desc(wq, desc);
out_ref:
	percpu_ref_put(&wq->wq_active);
out_fallback:
	if (early_return)
		atomic64_add(npages, &async_fallback_pages);
	return cleared;
}

static int idxd_page_clear_probe(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_page_clear *clear;
	int ret, i;

	mutex_lock(&wq->wq_lock);
	if (!idxd_wq_driver_name_match(wq, dev) ||
	    wq->idxd->data->type != IDXD_TYPE_DSA || !wq_dedicated(wq) ||
	    !test_bit(DSA_OPCODE_MEMFILL, wq->idxd->opcap_bmap)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* The per-WQ bitmap exists only when WQCAP advertises op_config. */
	if (wq->opcap_bmap && !test_bit(DSA_OPCODE_MEMFILL, wq->opcap_bmap)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	clear = kzalloc(sizeof(*clear), GFP_KERNEL);
	if (!clear) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	clear->wq = wq;
	clear->ops.clear = idxd_clear_pages;
	wq->type = IDXD_WQT_KERNEL;
	ret = idxd_drv_enable_wq(wq);
	if (ret)
		goto out_free;

	clear->requests = kcalloc(wq->num_descs, sizeof(*clear->requests), GFP_KERNEL);
	if (!clear->requests) {
		ret = -ENOMEM;
		goto out_disable;
	}
	for (i = 0; i < wq->num_descs; i++)
		INIT_WORK(&clear->requests[i].work, idxd_retire_unsafe_fill);
	clear->retire_wq = alloc_workqueue("dsa_unsafe_retire",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!clear->retire_wq) {
		ret = -ENOMEM;
		goto out_requests;
	}

	ret = dsa_register_page_clear(&clear->ops);
	if (ret) {
		destroy_workqueue(clear->retire_wq);
		goto out_requests;
	}

	dev_set_drvdata(dev, clear);
	wq->idxd->cmd_status = 0;
	dev_info(dev, "registered page clearing (sync default, unsafe_async experiment available)\n");
	mutex_unlock(&wq->wq_lock);
	return 0;

out_requests:
	kfree(clear->requests);
out_disable:
	__idxd_wq_quiesce(wq);
	idxd_drv_disable_wq(wq);
out_free:
	wq->type = IDXD_WQT_NONE;
	kfree(clear);
out_unlock:
	mutex_unlock(&wq->wq_lock);
	return ret;
}

static void idxd_page_clear_remove(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_page_clear *clear = dev_get_drvdata(dev);
	struct idxd_wq *wq = clear->wq;

	/* Stop new callbacks and wait for all DMA before releasing resources. */
	dsa_unregister_page_clear(&clear->ops);
	destroy_workqueue(clear->retire_wq);
	mutex_lock(&wq->wq_lock);
	__idxd_wq_quiesce(wq);
	idxd_drv_disable_wq(wq);
	mutex_unlock(&wq->wq_lock);
	dev_set_drvdata(dev, NULL);
	kfree(clear->requests);
	kfree(clear);
}

static enum idxd_dev_type idxd_page_clear_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

static struct idxd_device_driver idxd_page_clear_driver = {
	.name = "dsa_page_clear",
	.type = idxd_page_clear_types,
	.probe = idxd_page_clear_probe,
	.remove = idxd_page_clear_remove,
};
module_idxd_driver(idxd_page_clear_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Intel DSA synchronous page clearing for init_on_free");
MODULE_IMPORT_NS("IDXD");
