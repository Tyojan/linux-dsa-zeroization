// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/przero_stats.h>
#include <linux/seq_file.h>

/* System-wide cumulative base-page counts, independent of tracing. */
static atomic64_t przero_marked = ATOMIC64_INIT(0);
static atomic64_t przero_zeroed = ATOMIC64_INIT(0);

void przero_stats_mark(unsigned long pages)
{
	atomic64_add(pages, &przero_marked);
}

void przero_stats_zero(unsigned long pages)
{
	atomic64_add(pages, &przero_zeroed);
}

u64 przero_stats_marked(void)
{
	return atomic64_read(&przero_marked);
}

u64 przero_stats_zeroed(void)
{
	return atomic64_read(&przero_zeroed);
}

#ifdef CONFIG_PROC_FS
static int przero_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "marked_pages %llu\nzeroed_pages %llu\n",
		   (unsigned long long)przero_stats_marked(),
		   (unsigned long long)przero_stats_zeroed());
	return 0;
}

static int __init przero_stats_init(void)
{
	return proc_create_single("przero_stats", 0444, NULL,
				  przero_stats_show) ? 0 : -ENOMEM;
}
fs_initcall(przero_stats_init);
#endif
