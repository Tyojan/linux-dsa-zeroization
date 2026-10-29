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
			{ PRZERO_EXIT_MMAP_END, "exit_mmap_end" },
			{ PRZERO_EXIT_MM_END, "exit_mm_end" },
			{ PRZERO_DO_EXIT_END, "do_exit_end" }),
		  __entry->zeroize)
);

TRACE_EVENT(przero_prctl_walk,
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
