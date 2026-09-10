// SPDX-License-Identifier: GPL-2.0
/* Synchronous init_on_free offload using a dedicated kernel DSA work queue. */
#include <linux/dma-mapping.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/dsa.h>

#include "idxd.h"

#define DSA_CLEAR_TIMEOUT_NS NSEC_PER_SEC

static unsigned int min_pages = 1;
module_param(min_pages, uint, 0644);
MODULE_PARM_DESC(min_pages, "Minimum number of contiguous pages to offload");

static atomic64_t completed_pages = ATOMIC64_INIT(0);

static int completed_pages_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lld\n", atomic64_read(&completed_pages));
}

static const struct kernel_param_ops completed_pages_ops = {
	.get = completed_pages_get,
};
module_param_cb(completed_pages, &completed_pages_ops, NULL, 0444);
MODULE_PARM_DESC(completed_pages, "Number of pages successfully zeroed by DSA");

struct idxd_page_clear {
	struct dsa_page_clear_ops ops;
	struct idxd_wq *wq;
};

static bool idxd_clear_pages(struct dsa_page_clear_ops *ops, void *addr,
			     unsigned int npages)
{
	struct idxd_page_clear *clear = container_of(ops, struct idxd_page_clear, ops);
	struct idxd_wq *wq = clear->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	size_t len = (size_t)npages * PAGE_SIZE;
	struct idxd_desc *desc;
	dma_addr_t dma;
	u64 start;
	u8 status;
	bool cleared = false;

	if (!npages || npages < READ_ONCE(min_pages) ||
	    len > wq->max_xfer_bytes || len > U32_MAX ||
	    len > dma_max_mapping_size(dev) ||
	    READ_ONCE(wq->state) != IDXD_WQ_ENABLED)
		return false;

	/* Hold the WQ live until both completion and DMA unmapping finish. */
	if (!percpu_ref_tryget_live(&wq->wq_active))
		return false;

	desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
	if (IS_ERR(desc))
		goto out_ref;

	/* The caller supplies a contiguous, direct-mapped page range. */
	dma = dma_map_page(dev, virt_to_page(addr), 0, len, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dma))
		goto out_desc;

	desc->hw->opcode = DSA_OPCODE_MEMFILL;
	desc->hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	desc->hw->pattern = 0;
	desc->hw->dst_addr = dma;
	desc->hw->xfer_size = len;
	desc->hw->completion_addr = desc->compl_dma;
	/* Kernel DMA uses user privilege in the kernel's DMA address space. */
	desc->hw->priv = 0;

	if (idxd_submit_desc_nowait(wq, desc))
		goto out_unmap;

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
	return cleared;
}

static int idxd_page_clear_probe(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_page_clear *clear;
	int ret;

	mutex_lock(&wq->wq_lock);
	if (!idxd_wq_driver_name_match(wq, dev) ||
	    wq->idxd->data->type != IDXD_TYPE_DSA || !wq_dedicated(wq) ||
	    !test_bit(DSA_OPCODE_MEMFILL, wq->opcap_bmap)) {
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

	ret = dsa_register_page_clear(&clear->ops);
	if (ret) {
		__idxd_wq_quiesce(wq);
		idxd_drv_disable_wq(wq);
		goto out_free;
	}

	dev_set_drvdata(dev, clear);
	wq->idxd->cmd_status = 0;
	dev_info(dev, "registered synchronous init_on_free page clearing\n");
	mutex_unlock(&wq->wq_lock);
	return 0;

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
	mutex_lock(&wq->wq_lock);
	__idxd_wq_quiesce(wq);
	idxd_drv_disable_wq(wq);
	mutex_unlock(&wq->wq_lock);
	dev_set_drvdata(dev, NULL);
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
