// SPDX-License-Identifier: GPL-2.0
/* Synchronous and deferred init_on_free using a dedicated kernel DSA WQ. */
#include <linux/dma-mapping.h>
#include <linux/kasan.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <asm/dsa.h>

#include "idxd.h"

#define DSA_CLEAR_TIMEOUT_NS NSEC_PER_SEC
#define DSA_CLEAR_TIMEOUT_POLL_INTERVAL 1024U
#define DSA_CLEAR_MAX_BATCH 1024U

static unsigned int min_pages = 1;
module_param(min_pages, uint, 0644);
MODULE_PARM_DESC(min_pages, "Minimum number of contiguous pages to offload");

static bool cache_control;
module_param(cache_control, bool, 0644);
MODULE_PARM_DESC(cache_control, "Set the DSA cache-control hint on new fills (default: false)");

static bool async_mode;
module_param(async_mode, bool, 0644);
MODULE_PARM_DESC(async_mode, "Defer allocator reuse until DMA completion (default: false)");

static unsigned int max_pending_pages = 4096;
module_param(max_pending_pages, uint, 0644);
MODULE_PARM_DESC(max_pending_pages, "Maximum pages held for asynchronous zeroing; overflow uses CPU");

static unsigned int batch_size = 1;
static unsigned int batch_wait_us;
static unsigned int batch_capacity = 1;
static unsigned int batch_slots = 8;
static unsigned int batch_slots_active;
module_param(batch_slots, uint, 0444);
MODULE_PARM_DESC(batch_slots, "Requested concurrent batch slots, allocated at bind time (1..1024)");
module_param(batch_slots_active, uint, 0444);
MODULE_PARM_DESC(batch_slots_active, "Batch slots allocated on the bound WQ");

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
static atomic64_t inflight_batches = ATOMIC64_INIT(0);
static atomic64_t peak_inflight_batches = ATOMIC64_INIT(0);
static atomic64_t overlapped_batch_submissions = ATOMIC64_INIT(0);

static int page_count_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%lld\n", atomic64_read(kp->arg));
}

static const struct kernel_param_ops page_count_ops = {
	.get = page_count_get,
};
module_param_cb(completed_pages, &page_count_ops, &completed_pages, 0444);
MODULE_PARM_DESC(completed_pages, "Pages successfully zeroed by DSA and unmapped");
module_param_cb(submitted_pages, &page_count_ops, &submitted_pages, 0444);
MODULE_PARM_DESC(submitted_pages, "Pages submitted for deferred allocator release");
module_param_cb(pending_pages, &page_count_ops, &pending_pages, 0444);
MODULE_PARM_DESC(pending_pages, "Pages held outside allocator free lists, including admission reservations");
module_param_cb(async_fallback_pages, &page_count_ops, &async_fallback_pages, 0444);
MODULE_PARM_DESC(async_fallback_pages, "Pages rejected by async mode, including below min_pages");
module_param_cb(async_error_pages, &page_count_ops, &async_error_pages, 0444);
MODULE_PARM_DESC(async_error_pages, "Pages in failed deferred fills repaired by CPU before freeing");
module_param_cb(batch_submissions, &page_count_ops, &batch_submissions, 0444);
MODULE_PARM_DESC(batch_submissions, "Successfully submitted BATCH descriptors");
module_param_cb(batched_descriptors, &page_count_ops, &batched_descriptors, 0444);
MODULE_PARM_DESC(batched_descriptors, "MEMFILL descriptors submitted inside batches");
module_param_cb(single_submissions, &page_count_ops, &single_submissions, 0444);
MODULE_PARM_DESC(single_submissions, "MEMFILL descriptors submitted directly");
module_param_cb(inflight_batches, &page_count_ops, &inflight_batches, 0444);
MODULE_PARM_DESC(inflight_batches, "BATCH submissions whose completion has not yet been reaped");
module_param_cb(peak_inflight_batches, &page_count_ops, &peak_inflight_batches, 0444);
MODULE_PARM_DESC(peak_inflight_batches, "Maximum unreaped BATCH submissions since module load");
module_param_cb(overlapped_batch_submissions, &page_count_ops, &overlapped_batch_submissions, 0444);
MODULE_PARM_DESC(overlapped_batch_submissions, "BATCH submissions while another BATCH was unreaped");

static unsigned int latency_sample_every;
static DEFINE_PER_CPU(unsigned int, latency_sample_seq);

static int latency_sample_set(const char *val, const struct kernel_param *kp)
{
	unsigned int value;
	int ret = kstrtouint(val, 0, &value);

	if (ret)
		return ret;
	if (value && !is_power_of_2(value))
		return -EINVAL;
	WRITE_ONCE(*(unsigned int *)kp->arg, value);
	return 0;
}

static const struct kernel_param_ops latency_sample_ops = {
	.set = latency_sample_set,
	.get = param_get_uint,
};
module_param_cb(latency_sample_every, &latency_sample_ops, &latency_sample_every, 0644);
MODULE_PARM_DESC(latency_sample_every, "Sample every N direct synchronous submissions per CPU; 0 disables, N must be a power of two");

#define DSA_LATENCY_STAT(name, description) \
	static atomic64_t name = ATOMIC64_INIT(0); \
	module_param_cb(name, &page_count_ops, &name, 0444); \
	MODULE_PARM_DESC(name, description)

DSA_LATENCY_STAT(latency_samples, "Completed sampled direct synchronous submissions");
DSA_LATENCY_STAT(latency_sampled_pages, "Pages covered by latency samples");
DSA_LATENCY_STAT(latency_total_ns, "Sum of sampled submit-entry to completion-observed times, ns");
DSA_LATENCY_STAT(latency_submit_ns, "Sum of sampled time inside idxd_submit_desc_nowait, ns");
DSA_LATENCY_STAT(latency_poll_loops, "Sum of unsuccessful completion polls in samples");
DSA_LATENCY_STAT(latency_zero_polls, "Samples already complete at the first status check");
DSA_LATENCY_STAT(latency_errors, "Samples with unsuccessful hardware completion status");
DSA_LATENCY_STAT(latency_max_ns, "Maximum sampled total latency since module load, ns");
DSA_LATENCY_STAT(latency_max_polls, "Maximum sampled unsuccessful polls since module load");

/* Disjoint buckets with upper bounds 1, 2, 4, 8, 16, 32, 64 us, then >64 us. */
static atomic64_t latency_buckets[8];

static int latency_histogram_get(char *buffer, const struct kernel_param *kp)
{
	unsigned int i;
	int len = 0;

	for (i = 0; i < ARRAY_SIZE(latency_buckets); i++)
		len += sysfs_emit_at(buffer, len, "%lld%c",
				     atomic64_read(&latency_buckets[i]),
				     i == ARRAY_SIZE(latency_buckets) - 1 ? '\n' : ' ');
	return len;
}

static const struct kernel_param_ops latency_histogram_ops = {
	.get = latency_histogram_get,
};
module_param_cb(latency_histogram, &latency_histogram_ops, NULL, 0444);
MODULE_PARM_DESC(latency_histogram, "Disjoint sample counts: <=1us, (1,2], (2,4], (4,8], (8,16], (16,32], (32,64], >64us");

static bool idxd_sample_latency(void)
{
	unsigned int every = READ_ONCE(latency_sample_every);

	/* The provider calls us with preemption disabled. */
	return every && !(this_cpu_inc_return(latency_sample_seq) & (every - 1));
}

static void idxd_latency_max(atomic64_t *counter, s64 value)
{
	s64 old = atomic64_read(counter);

	while (value > old && !atomic64_try_cmpxchg(counter, &old, value))
		cpu_relax();
}

static void idxd_record_latency(u64 total, u64 submit, u64 polls,
				unsigned int npages, bool success)
{
	unsigned int bucket = 0;

	while (bucket < ARRAY_SIZE(latency_buckets) - 1 && total > (1000ULL << bucket))
		bucket++;
	atomic64_add(total, &latency_total_ns);
	atomic64_add(submit, &latency_submit_ns);
	atomic64_add(polls, &latency_poll_loops);
	atomic64_add(npages, &latency_sampled_pages);
	if (!polls)
		atomic64_inc(&latency_zero_polls);
	if (!success)
		atomic64_inc(&latency_errors);
	idxd_latency_max(&latency_max_ns, total);
	idxd_latency_max(&latency_max_polls, polls);
	atomic64_inc(&latency_buckets[bucket]);
	atomic64_inc(&latency_samples);
}

struct idxd_page_clear_request {
	struct page *page;
	struct list_head node;
	struct idxd_desc *desc;
	dma_addr_t dma;
	unsigned int npages;
	u64 start;
	bool success;
	bool done;
};

struct idxd_page_batch {
	struct list_head node;
	struct list_head requests;
	struct idxd_desc *parent;
	struct idxd_desc *submitted_desc;
	struct dsa_hw_desc *list;
	dma_addr_t dma;
	u64 start;
	unsigned int count;
	bool submitted;
};

struct idxd_page_clear {
	struct dsa_page_clear_ops ops;
	struct idxd_wq *wq;
	struct workqueue_struct *retire_wq;
	struct idxd_page_clear_request *requests;
	spinlock_t pending_lock;
	spinlock_t dispatch_lock;
	struct list_head pending;
	struct list_head running_direct;
	unsigned int nr_pending;
	struct work_struct batch_work;
	struct idxd_page_batch *batches;
	struct list_head free_batches;
	struct list_head running_batches;
	unsigned int nr_slots;
	/* Includes slots being retired outside dispatch_lock. */
	unsigned int nr_active;
	unsigned int capacity;
};

static void idxd_finish_deferred_fill(struct idxd_page_clear_request *request,
				      bool success)
{
	struct idxd_desc *desc = request->desc;
	struct idxd_wq *wq = desc->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	unsigned int npages = request->npages;
	struct page *page = request->page;

	/* Recursive frees must not wait on the batch slot we are retiring. */
	preempt_disable();
	kasan_disable_current();
	dma_unmap_page(dev, request->dma, (size_t)npages * PAGE_SIZE, DMA_FROM_DEVICE);
	kasan_enable_current();
	if (success)
		atomic64_add(npages, &completed_pages);
	else
		atomic64_add(npages, &async_error_pages);
	/* No DMA can touch these pages after unmapping, even on an error. */
	dsa_free_pages_complete(page, ilog2(npages), success);
	idxd_free_desc(wq, desc);
	percpu_ref_put(&wq->wq_active);
	atomic64_sub(npages, &pending_pages);
	preempt_enable();
}

/* Reap ready direct fills without waiting behind an earlier slow request. */
static void idxd_retire_direct_fills(struct idxd_page_clear *clear)
{
	struct idxd_page_clear_request *request, *next;
	u64 now = ktime_get_mono_fast_ns();
	LIST_HEAD(completed);

	preempt_disable();
	spin_lock(&clear->pending_lock);
	list_for_each_entry_safe(request, next, &clear->running_direct, node) {
		u8 status = DSA_COMP_STATUS(READ_ONCE(request->desc->completion->status));

		if (status) {
			dma_rmb();
			request->success = status == DSA_COMP_SUCCESS;
			list_move_tail(&request->node, &completed);
		} else if (now > request->start &&
			   now - request->start > DSA_CLEAR_TIMEOUT_NS) {
			panic("DSA deferred fill timed out on %s; pages still held",
			      dev_name(wq_confdev(clear->wq)));
		}
	}
	spin_unlock(&clear->pending_lock);
	list_for_each_entry_safe(request, next, &completed, node) {
		list_del_init(&request->node);
		idxd_finish_deferred_fill(request, request->success);
	}
	preempt_enable();
}

/*
 * Poll each submitted slot once, then try one new submission. Never wait for
 * hardware or the collection window while holding dispatch_lock. Each slot
 * owns its parent, child list and requests through completion and retirement.
 * Synchronous callers and the deferred worker both drive this progress engine.
 */
static void idxd_flush_page_batch(struct idxd_page_clear *clear)
{
	struct idxd_page_clear_request *request, *next;
	struct idxd_page_batch *batch, *next_batch;
	struct idxd_desc *desc;
	unsigned int count = 0, target;
	u64 window;
	LIST_HEAD(completed);

	/* A worker must publish synchronous results before it can be preempted. */
	preempt_disable();
	if (!spin_trylock(&clear->dispatch_lock)) {
		preempt_enable();
		return;
	}
	/* Reap out of order: a slow batch must not block later completions. */
	list_for_each_entry_safe(batch, next_batch, &clear->running_batches, node) {
		desc = batch->submitted_desc;
		if (!DSA_COMP_STATUS(READ_ONCE(desc->completion->status))) {
			if (ktime_get_mono_fast_ns() - batch->start > DSA_CLEAR_TIMEOUT_NS)
				panic("DSA batch clear timed out on %s; DMA may still be active",
				      dev_name(wq_confdev(clear->wq)));
			continue;
		}
		/* Parent completion orders all child completion records and writes. */
		dma_rmb();
		list_move_tail(&batch->node, &completed);
		if (batch->count > 1)
			atomic64_dec(&inflight_batches);
	}
	if (list_empty(&clear->free_batches))
		goto out_unlock;

	target = min(READ_ONCE(batch_size), clear->capacity);
	window = (u64)READ_ONCE(batch_wait_us) * NSEC_PER_USEC;
	spin_lock(&clear->pending_lock);
	if (list_empty(&clear->pending)) {
		spin_unlock(&clear->pending_lock);
		goto out_unlock;
	}
	request = list_first_entry(&clear->pending,
				  struct idxd_page_clear_request, node);
	if (clear->nr_pending < target &&
	    ktime_get_mono_fast_ns() - request->start < window) {
		spin_unlock(&clear->pending_lock);
		goto out_unlock;
	}
	batch = list_first_entry(&clear->free_batches, struct idxd_page_batch, node);
	list_del_init(&batch->node);
	clear->nr_active++;
	list_for_each_entry_safe(request, next, &clear->pending, node) {
		list_move_tail(&request->node, &batch->requests);
		batch->list[count++] = *request->desc->hw;
		clear->nr_pending--;
		if (count == target)
			break;
	}
	spin_unlock(&clear->pending_lock);
	batch->count = count;

	if (count == 1) {
		request = list_first_entry(&batch->requests, struct idxd_page_clear_request, node);
		desc = request->desc;
	} else {
		desc = batch->parent;
		/* Keep the PASID initialized by idxd_alloc_desc(). */
		memset(desc->completion, 0, clear->wq->idxd->data->compl_size);
		desc->hw->opcode = DSA_OPCODE_BATCH;
		desc->hw->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		desc->hw->desc_list_addr = batch->dma;
		desc->hw->desc_count = count;
		desc->hw->completion_addr = desc->compl_dma;
		desc->hw->priv = 0;
	}
	batch->submitted_desc = desc;
	batch->start = ktime_get_mono_fast_ns();
	batch->submitted = !idxd_submit_desc_nowait(clear->wq, desc);
	if (batch->submitted) {
		if (count > 1) {
			s64 inflight = atomic64_inc_return(&inflight_batches);
			s64 peak = atomic64_read(&peak_inflight_batches);

			if (inflight > 1)
				atomic64_inc(&overlapped_batch_submissions);
			while (inflight > peak &&
			       !atomic64_try_cmpxchg(&peak_inflight_batches, &peak, inflight))
				cpu_relax();
			atomic64_inc(&batch_submissions);
			atomic64_add(count, &batched_descriptors);
		} else {
			atomic64_inc(&single_submissions);
		}
		list_for_each_entry(request, &batch->requests, node)
			if (request->page)
				atomic64_add(request->npages, &submitted_pages);
		list_add_tail(&batch->node, &clear->running_batches);
	} else {
		/* No DMA started; publish failure using the same retirement path. */
		list_add_tail(&batch->node, &completed);
	}

out_unlock:
	spin_unlock(&clear->dispatch_lock);
	list_for_each_entry_safe(batch, next_batch, &completed, node) {
		list_del_init(&batch->node);
		list_for_each_entry_safe(request, next, &batch->requests, node) {
			bool success = batch->submitted &&
				DSA_COMP_STATUS(READ_ONCE(request->desc->completion->status)) ==
				DSA_COMP_SUCCESS;

			list_del_init(&request->node);
			if (request->page) {
				idxd_finish_deferred_fill(request, success);
			} else {
				request->success = success;
				/* Caller may recycle now; never access this request again. */
				smp_store_release(&request->done, true);
			}
		}
		/* No hardware or caller can still access this slot's DMA storage. */
		spin_lock(&clear->dispatch_lock);
		list_add_tail(&batch->node, &clear->free_batches);
		clear->nr_active--;
		spin_unlock(&clear->dispatch_lock);
	}
	preempt_enable();
}

static void idxd_page_batch_work(struct work_struct *work)
{
	struct idxd_page_clear *clear =
		container_of(work, struct idxd_page_clear, batch_work);
	bool busy;

	for (;;) {
		idxd_retire_direct_fills(clear);
		idxd_flush_page_batch(clear);
		spin_lock(&clear->dispatch_lock);
		spin_lock(&clear->pending_lock);
		busy = clear->nr_pending || clear->nr_active ||
			!list_empty(&clear->running_direct);
		spin_unlock(&clear->pending_lock);
		spin_unlock(&clear->dispatch_lock);
		if (!busy)
			return;
		cpu_relax();
		cond_resched();
	}
}

static bool idxd_clear_pages_common(struct dsa_page_clear_ops *ops, void *addr,
				    unsigned int npages, struct page *page)
{
	struct idxd_page_clear *clear = container_of(ops, struct idxd_page_clear, ops);
	struct idxd_wq *wq = clear->wq;
	struct device *dev = &wq->idxd->pdev->dev;
	size_t len = (size_t)npages * PAGE_SIZE;
	struct idxd_desc *desc;
	struct idxd_page_clear_request *request;
	dma_addr_t dma;
	unsigned int timeout_polls;
	u64 start;
	u64 sample_start = 0, sample_submit = 0, sample_polls = 0;
	u8 status;
	bool sampled;
	bool cleared = false;
	bool early_return = page != NULL;
	bool reserved = false;
	bool batching = READ_ONCE(batch_size) > 1 && clear->capacity > 1;

	if (!npages || npages < READ_ONCE(min_pages) ||
	    len > wq->max_xfer_bytes || len > U32_MAX ||
	    len > dma_max_mapping_size(dev) ||
	    READ_ONCE(wq->state) != IDXD_WQ_ENABLED)
		goto out_fallback;

	if (early_return) {
		s64 held = atomic64_read(&pending_pages);
		unsigned int limit = READ_ONCE(max_pending_pages);

		do {
			if (held + npages > limit)
				goto out_fallback;
		} while (!atomic64_try_cmpxchg(&pending_pages, &held, held + npages));
		reserved = true;
	}

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
		request->page = page;
		request->start = ktime_get_mono_fast_ns();
	}

	if (batching) {
		request->done = false;
		spin_lock(&clear->pending_lock);
		list_add_tail(&request->node, &clear->pending);
		clear->nr_pending++;
		spin_unlock(&clear->pending_lock);
		if (early_return) {
			queue_work(clear->retire_wq, &clear->batch_work);
			/* The allocator retains these pages outside its free lists. */
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

	sampled = !early_return && idxd_sample_latency();
	if (sampled)
		sample_start = ktime_get_mono_fast_ns();
	if (idxd_submit_desc_nowait(wq, desc))
		goto out_unmap;
	if (sampled)
		sample_submit = ktime_get_mono_fast_ns();
	atomic64_inc(&single_submissions);

	if (early_return) {
		atomic64_add(npages, &submitted_pages);
		spin_lock(&clear->pending_lock);
		list_add_tail(&request->node, &clear->running_direct);
		spin_unlock(&clear->pending_lock);
		queue_work(clear->retire_wq, &clear->batch_work);
		return true;
	}

	/*
	 * No completion interrupt: this caller owns and frees the descriptor.
	 * Never return on timeout: DMA could still overwrite a reused page.
	 */
	start = ktime_get_mono_fast_ns();
	timeout_polls = DSA_CLEAR_TIMEOUT_POLL_INTERVAL;
	for (;;) {
		status = READ_ONCE(desc->completion->status);
		if (DSA_COMP_STATUS(status))
			break;
		/* Poll completion every time, but amortize the clock read. */
		if (!--timeout_polls) {
			if (sampled)
				sample_polls += DSA_CLEAR_TIMEOUT_POLL_INTERVAL;
			if (ktime_get_mono_fast_ns() - start > DSA_CLEAR_TIMEOUT_NS)
				panic("DSA page clear timed out on %s; DMA may still be active",
				      dev_name(wq_confdev(wq)));
			timeout_polls = DSA_CLEAR_TIMEOUT_POLL_INTERVAL;
		}
		cpu_relax();
	}
	dma_rmb();
	cleared = DSA_COMP_STATUS(status) == DSA_COMP_SUCCESS;
	if (sampled) {
		u64 end = ktime_get_mono_fast_ns();

		/* Reuse the timeout countdown instead of incrementing every poll. */
		sample_polls += DSA_CLEAR_TIMEOUT_POLL_INTERVAL - timeout_polls;
		idxd_record_latency(end - sample_start, sample_submit - sample_start,
				    sample_polls, npages, cleared);
	}
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
	if (reserved)
		atomic64_sub(npages, &pending_pages);
	if (early_return)
		atomic64_add(npages, &async_fallback_pages);
	return cleared;
}

static bool idxd_clear_pages(struct dsa_page_clear_ops *ops, void *addr,
			     unsigned int npages)
{
	/* A rejected deferred request must use CPU, not retry synchronously. */
	if (READ_ONCE(async_mode))
		return false;
	return idxd_clear_pages_common(ops, addr, npages, NULL);
}

static bool idxd_defer_pages(struct dsa_page_clear_ops *ops, struct page *page,
			     unsigned int order)
{
	if (!READ_ONCE(async_mode))
		return false;
	return idxd_clear_pages_common(ops, page_address(page), 1U << order, page);
}

static void idxd_drain_pages(struct dsa_page_clear_ops *ops)
{
	struct idxd_page_clear *clear = container_of(ops, struct idxd_page_clear, ops);

	flush_workqueue(clear->retire_wq);
}

static void idxd_free_page_batch(struct idxd_page_clear *clear)
{
	unsigned int i;

	for (i = 0; i < clear->nr_slots; i++) {
		struct idxd_page_batch *batch = &clear->batches[i];

		if (batch->list)
			dma_free_coherent(&clear->wq->idxd->pdev->dev,
					  clear->capacity * sizeof(*batch->list),
					  batch->list, batch->dma);
		if (batch->parent)
			idxd_free_desc(clear->wq, batch->parent);
	}
	kfree(clear->batches);
	clear->batches = NULL;
	clear->nr_slots = 0;
}

static int idxd_alloc_page_batch(struct idxd_page_clear *clear)
{
	struct idxd_wq *wq = clear->wq;
	struct idxd_desc *desc;
	unsigned int capacity, slots, i;
	int ret;

	clear->capacity = 1;
	spin_lock_init(&clear->pending_lock);
	spin_lock_init(&clear->dispatch_lock);
	INIT_LIST_HEAD(&clear->pending);
	INIT_LIST_HEAD(&clear->running_direct);
	INIT_LIST_HEAD(&clear->free_batches);
	INIT_LIST_HEAD(&clear->running_batches);
	INIT_WORK(&clear->batch_work, idxd_page_batch_work);
	if (!batch_slots || batch_slots > DSA_CLEAR_MAX_BATCH)
		return -EINVAL;
	if (!test_bit(DSA_OPCODE_BATCH, wq->idxd->opcap_bmap) ||
	    (wq->opcap_bmap && !test_bit(DSA_OPCODE_BATCH, wq->opcap_bmap)) ||
	    wq->num_descs < 3)
		return 0;
	/* Leave at least two child descriptors per reserved parent. */
	slots = min_t(unsigned int, batch_slots, wq->num_descs / 3);
	capacity = min_t(unsigned int, wq->num_descs - slots, DSA_CLEAR_MAX_BATCH);
	capacity = min(capacity, wq->max_batch_size);
	capacity = min(capacity, wq->idxd->max_batch_size);
	if (capacity < 2)
		return 0;

	clear->batches = kcalloc(slots, sizeof(*clear->batches), GFP_KERNEL);
	if (!clear->batches)
		return -ENOMEM;
	clear->capacity = capacity;
	clear->nr_slots = slots;
	/* Reserve parents so exhausted child slots cannot prevent dispatch. */
	for (i = 0; i < slots; i++) {
		struct idxd_page_batch *batch = &clear->batches[i];

		INIT_LIST_HEAD(&batch->requests);
		desc = idxd_alloc_desc(wq, IDXD_OP_NONBLOCK);
		if (IS_ERR(desc)) {
			ret = PTR_ERR(desc);
			goto err_free;
		}
		batch->parent = desc;
		batch->list = dma_alloc_coherent(&wq->idxd->pdev->dev,
						capacity * sizeof(*batch->list),
						&batch->dma, GFP_KERNEL);
		if (!batch->list) {
			ret = -ENOMEM;
			goto err_free;
		}
		list_add_tail(&batch->node, &clear->free_batches);
	}
	return 0;

err_free:
	idxd_free_page_batch(clear);
	return ret;
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
	clear->ops.defer = idxd_defer_pages;
	clear->ops.drain = idxd_drain_pages;
	wq->type = IDXD_WQT_KERNEL;
	ret = idxd_drv_enable_wq(wq);
	if (ret)
		goto out_free;

	clear->requests = kcalloc(wq->num_descs, sizeof(*clear->requests), GFP_KERNEL);
	if (!clear->requests) {
		ret = -ENOMEM;
		goto out_disable;
	}
	ret = idxd_alloc_page_batch(clear);
	if (ret)
		goto out_requests;
	clear->retire_wq = alloc_workqueue("dsa_page_retire",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!clear->retire_wq) {
		ret = -ENOMEM;
		goto out_batch;
	}

	ret = dsa_register_page_clear_v2(&clear->ops);
	if (ret) {
		destroy_workqueue(clear->retire_wq);
		goto out_batch;
	}

	dev_set_drvdata(dev, clear);
	WRITE_ONCE(batch_capacity, clear->capacity);
	WRITE_ONCE(batch_slots_active, clear->nr_slots);
	wq->idxd->cmd_status = 0;
	dev_info(dev, "registered page clearing (sync default, batch capacity %u, slots %u)\n",
		 clear->capacity, clear->nr_slots);
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
	WRITE_ONCE(batch_slots_active, 0);
	dsa_unregister_page_clear_v2(&clear->ops);
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
MODULE_DESCRIPTION("Intel DSA page clearing with synchronous and safe deferred release");
MODULE_IMPORT_NS("IDXD");
