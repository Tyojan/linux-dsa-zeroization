// SPDX-License-Identifier: GPL-2.0
/* Synchronous init_on_free offload using a dedicated kernel DSA work queue. */
#include <linux/dma-mapping.h>
#include <linux/kasan.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/dsa.h>

#include "idxd.h"

#define DSA_CLEAR_TIMEOUT_NS NSEC_PER_SEC
#define DSA_CLEAR_MAX_BATCH 1024U

static unsigned int min_pages = 1;
module_param(min_pages, uint, 0644);
MODULE_PARM_DESC(min_pages, "Minimum number of contiguous pages to offload");

static bool cache_control;
module_param(cache_control, bool, 0644);
MODULE_PARM_DESC(cache_control, "Set the DSA cache-control hint on new fills (default: false)");

static bool unsafe_async;
module_param(unsafe_async, bool, 0644);
MODULE_PARM_DESC(unsafe_async, "UNSAFE: return after queueing/submission; DMA may overwrite reused pages");

static unsigned int batch_size = 1;
static unsigned int batch_wait_us;
static unsigned int batch_capacity = 1;

static int batch_param_set(const char *val, const struct kernel_param *kp)
{
	unsigned int value;
	int ret = kstrtouint(val, 0, &value);

	if (ret)
		return ret;
	if (kp->arg == &batch_size) {
		if (!value || value > DSA_CLEAR_MAX_BATCH)
			return -EINVAL;
	} else if (value > 1000) {
		return -EINVAL;
	}
	WRITE_ONCE(*(unsigned int *)kp->arg, value);
	return 0;
}

static const struct kernel_param_ops batch_param_ops = {
	.set = batch_param_set,
	.get = param_get_uint,
};
module_param_cb(batch_size, &batch_param_ops, &batch_size, 0644);
MODULE_PARM_DESC(batch_size, "Maximum MEMFILLs per batch, 1 disables batching (1..1024)");
module_param_cb(batch_wait_us, &batch_param_ops, &batch_wait_us, 0644);
MODULE_PARM_DESC(batch_wait_us, "Collection window from oldest queued request, in us (0..1000)");
module_param(batch_capacity, uint, 0444);
MODULE_PARM_DESC(batch_capacity, "Bound queue's batch capacity; 1 means batching unavailable");

static atomic64_t completed_pages = ATOMIC64_INIT(0);
static atomic64_t submitted_pages = ATOMIC64_INIT(0);
static atomic64_t pending_pages = ATOMIC64_INIT(0);
static atomic64_t async_fallback_pages = ATOMIC64_INIT(0);
static atomic64_t async_error_pages = ATOMIC64_INIT(0);
static atomic64_t batch_submissions = ATOMIC64_INIT(0);
static atomic64_t batched_descriptors = ATOMIC64_INIT(0);
static atomic64_t single_submissions = ATOMIC64_INIT(0);

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
module_param_cb(batch_submissions, &page_count_ops, &batch_submissions, 0444);
MODULE_PARM_DESC(batch_submissions, "Successfully submitted BATCH descriptors");
module_param_cb(batched_descriptors, &page_count_ops, &batched_descriptors, 0444);
MODULE_PARM_DESC(batched_descriptors, "MEMFILL descriptors submitted inside batches");
module_param_cb(single_submissions, &page_count_ops, &single_submissions, 0444);
MODULE_PARM_DESC(single_submissions, "MEMFILL descriptors submitted directly");

struct idxd_page_clear_request {
	struct work_struct work;
	struct list_head node;
	struct idxd_desc *desc;
	dma_addr_t dma;
	unsigned int npages;
	u64 start;
	bool early_return;
	bool success;
	bool done;
};

struct idxd_page_clear {
	struct dsa_page_clear_ops ops;
	struct idxd_wq *wq;
	struct workqueue_struct *retire_wq;
	struct idxd_page_clear_request *requests;
	spinlock_t pending_lock;
	spinlock_t dispatch_lock;
	struct list_head pending;
	unsigned int nr_pending;
	struct work_struct batch_work;
	struct idxd_desc *batch_desc;
	struct dsa_hw_desc *batch_list;
	dma_addr_t batch_dma;
	unsigned int capacity;
};

static void idxd_finish_unsafe_fill(struct idxd_page_clear_request *request,
				   bool success)
{
	struct idxd_desc *desc = request->desc;
	struct idxd_wq *wq = desc->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	unsigned int npages = request->npages;

	kasan_disable_current();
	dma_unmap_page(dev, request->dma, (size_t)npages * PAGE_SIZE, DMA_FROM_DEVICE);
	kasan_enable_current();
	if (success)
		atomic64_add(npages, &completed_pages);
	else
		atomic64_add(npages, &async_error_pages);
	/* Never touch/clear/free the destination here: it may have a new owner. */
	idxd_free_desc(wq, desc);
	percpu_ref_put(&wq->wq_active);
	atomic64_sub(npages, &pending_pages);
}

/* Retire DMA resources only. The allocator already owns the destination. */
static void idxd_retire_unsafe_fill(struct work_struct *work)
{
	struct idxd_page_clear_request *request =
		container_of(work, struct idxd_page_clear_request, work);
	struct idxd_desc *desc = request->desc;
	u8 status;

	for (;;) {
		status = READ_ONCE(desc->completion->status);
		if (DSA_COMP_STATUS(status))
			break;
		if (ktime_get_mono_fast_ns() - request->start > DSA_CLEAR_TIMEOUT_NS)
			panic("DSA unsafe fill timed out on %s; DMA resources still active",
			      dev_name(wq_confdev(desc->wq)));
		cpu_relax();
		cond_resched();
	}
	dma_rmb();
	idxd_finish_unsafe_fill(request, DSA_COMP_STATUS(status) == DSA_COMP_SUCCESS);
}

/* The dispatcher may be a preemption-disabled synchronous caller. */
static void idxd_wait_batch_desc(struct idxd_desc *desc)
{
	u64 start = ktime_get_mono_fast_ns();

	while (!DSA_COMP_STATUS(READ_ONCE(desc->completion->status))) {
		if (ktime_get_mono_fast_ns() - start > DSA_CLEAR_TIMEOUT_NS)
			panic("DSA batch clear timed out on %s; DMA may still be active",
			      dev_name(wq_confdev(desc->wq)));
		cpu_relax();
	}
	dma_rmb();
}

/* One in-flight batch owns the coherent list until its parent completes. */
static void idxd_flush_page_batch(struct idxd_page_clear *clear)
{
	struct idxd_page_clear_request *request, *next;
	struct idxd_desc *desc;
	unsigned int count = 0, target;
	u64 window;
	bool submitted;
	LIST_HEAD(active);

	if (!spin_trylock(&clear->dispatch_lock))
		return;
	target = min(READ_ONCE(batch_size), clear->capacity);
	window = (u64)READ_ONCE(batch_wait_us) * NSEC_PER_USEC;
	for (;;) {
		spin_lock(&clear->pending_lock);
		if (list_empty(&clear->pending)) {
			spin_unlock(&clear->pending_lock);
			goto out_unlock;
		}
		request = list_first_entry(&clear->pending,
					  struct idxd_page_clear_request, node);
		if (clear->nr_pending >= target ||
		    ktime_get_mono_fast_ns() - request->start >= window)
			break;
		spin_unlock(&clear->pending_lock);
		cpu_relax();
	}
	list_for_each_entry_safe(request, next, &clear->pending, node) {
		list_move_tail(&request->node, &active);
		clear->batch_list[count++] = *request->desc->hw;
		clear->nr_pending--;
		if (count == target)
			break;
	}
	spin_unlock(&clear->pending_lock);

	if (count == 1) {
		request = list_first_entry(&active, struct idxd_page_clear_request, node);
		desc = request->desc;
	} else {
		desc = clear->batch_desc;
		/* Keep the PASID initialized by idxd_alloc_desc(). */
		memset(desc->completion, 0, clear->wq->idxd->data->compl_size);
		desc->hw->opcode = DSA_OPCODE_BATCH;
		desc->hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		desc->hw->desc_list_addr = clear->batch_dma;
		desc->hw->desc_count = count;
		desc->hw->completion_addr = desc->compl_dma;
		desc->hw->priv = 0;
	}
	submitted = !idxd_submit_desc_nowait(clear->wq, desc);
	if (submitted) {
		if (count > 1) {
			atomic64_inc(&batch_submissions);
			atomic64_add(count, &batched_descriptors);
		} else {
			atomic64_inc(&single_submissions);
		}
		list_for_each_entry(request, &active, node)
			if (request->early_return)
				atomic64_add(request->npages, &submitted_pages);
		/* Parent completion follows children and their completion writes. */
		idxd_wait_batch_desc(desc);
	}
	/* DMA no longer accesses the list; another dispatcher may reuse it. */
	spin_unlock(&clear->dispatch_lock);
	list_for_each_entry_safe(request, next, &active, node) {
		bool success = submitted &&
			DSA_COMP_STATUS(READ_ONCE(request->desc->completion->status)) ==
			DSA_COMP_SUCCESS;

		list_del_init(&request->node);
		if (request->early_return) {
			idxd_finish_unsafe_fill(request, success);
		} else {
			request->success = success;
			/* Caller may recycle now; no further access to this request. */
			smp_store_release(&request->done, true);
		}
	}
	return;
out_unlock:
	spin_unlock(&clear->dispatch_lock);
}

static void idxd_page_batch_work(struct work_struct *work)
{
	struct idxd_page_clear *clear =
		container_of(work, struct idxd_page_clear, batch_work);
	bool pending;

	for (;;) {
		spin_lock(&clear->pending_lock);
		pending = !list_empty(&clear->pending);
		spin_unlock(&clear->pending_lock);
		if (!pending)
			return;
		idxd_flush_page_batch(clear);
		cond_resched();
	}
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
	bool batching = READ_ONCE(batch_size) > 1 && clear->capacity > 1;

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

	if (early_return || batching) {
		request = &clear->requests[desc->id];
		request->desc = desc;
		request->dma = dma;
		request->npages = npages;
		request->start = ktime_get_mono_fast_ns();
	}

	if (batching) {
		request->early_return = early_return;
		request->done = false;
		if (early_return)
			atomic64_add(npages, &pending_pages);
		spin_lock(&clear->pending_lock);
		list_add_tail(&request->node, &clear->pending);
		clear->nr_pending++;
		spin_unlock(&clear->pending_lock);
		if (early_return) {
			queue_work(clear->retire_wq, &clear->batch_work);
			/* Unsafe: the destination may be reused even before submission. */
			return true;
		}
		/* Do not depend on a worker running while callers disable preemption. */
		while (!smp_load_acquire(&request->done)) {
			idxd_flush_page_batch(clear);
			cpu_relax();
		}
		cleared = request->success;
		if (!cleared)
			dev_warn_ratelimited(dev, "batched page clear failed; using CPU\n");
		goto out_unmap;
	}

	if (idxd_submit_desc_nowait(wq, desc))
		goto out_unmap;
	atomic64_inc(&single_submissions);

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

static void idxd_free_page_batch(struct idxd_page_clear *clear)
{
	if (!clear->batch_desc)
		return;
	dma_free_coherent(&clear->wq->idxd->pdev->dev,
			  clear->capacity * sizeof(*clear->batch_list),
			  clear->batch_list, clear->batch_dma);
	idxd_free_desc(clear->wq, clear->batch_desc);
}

static int idxd_alloc_page_batch(struct idxd_page_clear *clear)
{
	struct idxd_wq *wq = clear->wq;
	struct idxd_desc *desc;
	unsigned int capacity;

	clear->capacity = 1;
	spin_lock_init(&clear->pending_lock);
	spin_lock_init(&clear->dispatch_lock);
	INIT_LIST_HEAD(&clear->pending);
	INIT_WORK(&clear->batch_work, idxd_page_batch_work);
	if (!test_bit(DSA_OPCODE_BATCH, wq->idxd->opcap_bmap) ||
	    (wq->opcap_bmap && !test_bit(DSA_OPCODE_BATCH, wq->opcap_bmap)) ||
	    wq->num_descs < 3)
		return 0;
	capacity = min_t(unsigned int, wq->num_descs - 1, DSA_CLEAR_MAX_BATCH);
	capacity = min(capacity, wq->max_batch_size);
	capacity = min(capacity, wq->idxd->max_batch_size);
	if (capacity < 2)
		return 0;

	/* Reserve the parent so a full child pool cannot prevent submission. */
	desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
	if (IS_ERR(desc))
		return PTR_ERR(desc);
	clear->batch_list = dma_alloc_coherent(&wq->idxd->pdev->dev,
					     capacity * sizeof(*clear->batch_list),
					     &clear->batch_dma, GFP_KERNEL);
	if (!clear->batch_list) {
		idxd_free_desc(wq, desc);
		return -ENOMEM;
	}
	clear->batch_desc = desc;
	clear->capacity = capacity;
	return 0;
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
	ret = idxd_alloc_page_batch(clear);
	if (ret)
		goto out_requests;
	clear->retire_wq = alloc_workqueue("dsa_unsafe_retire",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!clear->retire_wq) {
		ret = -ENOMEM;
		goto out_batch;
	}

	ret = dsa_register_page_clear(&clear->ops);
	if (ret) {
		destroy_workqueue(clear->retire_wq);
		goto out_batch;
	}

	dev_set_drvdata(dev, clear);
	WRITE_ONCE(batch_capacity, clear->capacity);
	wq->idxd->cmd_status = 0;
	dev_info(dev, "registered page clearing (sync default, batch capacity %u)\n",
		 clear->capacity);
	mutex_unlock(&wq->wq_lock);
	return 0;

out_batch:
	idxd_free_page_batch(clear);
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
	/* Reset before unregister allows another queue to register its capacity. */
	WRITE_ONCE(batch_capacity, 1);
	dsa_unregister_page_clear(&clear->ops);
	destroy_workqueue(clear->retire_wq);
	idxd_free_page_batch(clear);
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
MODULE_DESCRIPTION("Intel DSA page clearing with synchronous and unsafe batched submission");
MODULE_IMPORT_NS("IDXD");
