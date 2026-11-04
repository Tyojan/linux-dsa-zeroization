/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM przero

#if !defined(_TRACE_PRZERO_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PRZERO_H

#include <linux/tracepoint.h>

/* define_trace.h includes this header again while generating trace code. */
#ifndef _PRZERO_STAGE_ENUM
#define _PRZERO_STAGE_ENUM
/* Timestamped milestones; pair begin/end using the task and mm pointers. */
enum przero_stage {
	PRZERO_DO_EXIT_BEGIN,
	PRZERO_EXIT_MM_BEGIN,
	PRZERO_EXIT_MMAP_BEGIN,
	PRZERO_LRU_DRAIN_BEGIN,
	PRZERO_LRU_DRAIN_END,
	PRZERO_LRU_DRAIN_SKIPPED,
	PRZERO_EXIT_MMAP_END,
	PRZERO_EXIT_MM_END,
	PRZERO_DO_EXIT_END,
};
#endif

TRACE_EVENT(przero_lifecycle,
	TP_PROTO(pid_t pid, const void *task, const void *mm, int stage,
		 bool zeroize),
	TP_ARGS(pid, task, mm, stage, zeroize),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(const void *, task)
		__field(const void *, mm)
		__field(int, stage)
		__field(bool, zeroize)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->task = task;
		__entry->mm = mm;
		__entry->stage = stage;
		__entry->zeroize = zeroize;
	),
	TP_printk("pid=%d task=%p mm=%p stage=%s zeroize=%d", __entry->pid,
		  __entry->task, __entry->mm,
		  __print_symbolic(__entry->stage,
			{ PRZERO_DO_EXIT_BEGIN, "do_exit_begin" },
			{ PRZERO_EXIT_MM_BEGIN, "exit_mm_begin" },
			{ PRZERO_EXIT_MMAP_BEGIN, "exit_mmap_begin" },
			{ PRZERO_LRU_DRAIN_BEGIN, "lru_drain_begin" },
			{ PRZERO_LRU_DRAIN_END, "lru_drain_end" },
			{ PRZERO_LRU_DRAIN_SKIPPED, "lru_drain_skipped" },
			{ PRZERO_EXIT_MMAP_END, "exit_mmap_end" },
			{ PRZERO_EXIT_MM_END, "exit_mm_end" },
			{ PRZERO_DO_EXIT_END, "do_exit_end" }),
		  __entry->zeroize)
);

/* Global totals at final mm teardown; counters are never reset by tracing. */
TRACE_EVENT(przero_exit_stats,
	TP_PROTO(pid_t pid, u64 marked, u64 zeroed),
	TP_ARGS(pid, marked, zeroed),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(u64, marked)
		__field(u64, zeroed)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->marked = marked;
		__entry->zeroed = zeroed;
	),
	TP_printk("pid=%d marked_pages=%llu zeroed_pages=%llu", __entry->pid,
		  (unsigned long long)__entry->marked,
		  (unsigned long long)__entry->zeroed)
);

/* Includes tasklist lock acquisition and per-task lock contention. */
TRACE_EVENT(przero_task_scan,
	TP_PROTO(pid_t pid, pid_t target_pid, const void *mm, bool enable,
		 u64 duration_ns, unsigned long scanned, unsigned long matched),
	TP_ARGS(pid, target_pid, mm, enable, duration_ns, scanned, matched),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(pid_t, target_pid)
		__field(const void *, mm)
		__field(bool, enable)
		__field(u64, duration_ns)
		__field(unsigned long, scanned)
		__field(unsigned long, matched)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->target_pid = target_pid;
		__entry->mm = mm;
		__entry->enable = enable;
		__entry->duration_ns = duration_ns;
		__entry->scanned = scanned;
		__entry->matched = matched;
	),
	TP_printk("pid=%d target_pid=%d mm=%p enable=%d duration_ns=%llu scanned=%lu matched=%lu",
		  __entry->pid, __entry->target_pid, __entry->mm,
		  __entry->enable, (unsigned long long)__entry->duration_ns,
		  __entry->scanned, __entry->matched)
);

TRACE_EVENT(przero_policy_walk,
	TP_PROTO(pid_t pid, const void *mm, u64 duration_ns,
		 unsigned long vmas, unsigned long ptes,
		 unsigned long marked_pages, int ret),
	TP_ARGS(pid, mm, duration_ns, vmas, ptes, marked_pages, ret),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(const void *, mm)
		__field(u64, duration_ns)
		__field(unsigned long, vmas)
		__field(unsigned long, ptes)
		__field(unsigned long, marked_pages)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->mm = mm;
		__entry->duration_ns = duration_ns;
		__entry->vmas = vmas;
		__entry->ptes = ptes;
		__entry->marked_pages = marked_pages;
		__entry->ret = ret;
	),
	TP_printk("pid=%d mm=%p duration_ns=%llu vmas=%lu ptes=%lu marked_pages=%lu ret=%d",
		  __entry->pid, __entry->mm,
		  (unsigned long long)__entry->duration_ns, __entry->vmas,
		  __entry->ptes, __entry->marked_pages, __entry->ret)
);

/* A PFN can be reused, so pair events in chronological order. */
DECLARE_EVENT_CLASS(przero_page_stage,
	TP_PROTO(unsigned long pfn, unsigned long pages),
	TP_ARGS(pfn, pages),
	TP_STRUCT__entry(
		__field(unsigned long, pfn)
		__field(unsigned long, pages)
	),
	TP_fast_assign(
		__entry->pfn = pfn;
		__entry->pages = pages;
	),
	TP_printk("pfn=%#lx pages=%lu", __entry->pfn, __entry->pages)
);

DEFINE_EVENT(przero_page_stage, przero_page_last_ref,
	TP_PROTO(unsigned long pfn, unsigned long pages),
	TP_ARGS(pfn, pages)
);

DEFINE_EVENT(przero_page_stage, przero_page_zero_begin,
	TP_PROTO(unsigned long pfn, unsigned long pages),
	TP_ARGS(pfn, pages)
);

/* The page can be released on another CPU after the owner has exited. */
TRACE_EVENT(przero_page_zero,
	TP_PROTO(unsigned long pfn, unsigned long pages, u64 duration_ns),
	TP_ARGS(pfn, pages, duration_ns),
	TP_STRUCT__entry(
		__field(unsigned long, pfn)
		__field(unsigned long, pages)
		__field(u64, duration_ns)
	),
	TP_fast_assign(
		__entry->pfn = pfn;
		__entry->pages = pages;
		__entry->duration_ns = duration_ns;
	),
	TP_printk("pfn=%#lx pages=%lu duration_ns=%llu", __entry->pfn,
		  __entry->pages, (unsigned long long)__entry->duration_ns)
);

TRACE_EVENT(przero_task_zero,
	TP_PROTO(pid_t pid, const void *task, unsigned long bytes, u64 duration_ns),
	TP_ARGS(pid, task, bytes, duration_ns),
	TP_STRUCT__entry(
		__field(pid_t, pid)
		__field(const void *, task)
		__field(unsigned long, bytes)
		__field(u64, duration_ns)
	),
	TP_fast_assign(
		__entry->pid = pid;
		__entry->task = task;
		__entry->bytes = bytes;
		__entry->duration_ns = duration_ns;
	),
	TP_printk("pid=%d task=%p bytes=%lu duration_ns=%llu", __entry->pid,
		  __entry->task, __entry->bytes,
		  (unsigned long long)__entry->duration_ns)
);

#endif /* _TRACE_PRZERO_H */

#include <trace/define_trace.h>
