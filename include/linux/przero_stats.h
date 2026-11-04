/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PRZERO_STATS_H
#define _LINUX_PRZERO_STATS_H
#include <linux/types.h>
void przero_stats_mark(unsigned long pages);
void przero_stats_zero(unsigned long pages);
u64 przero_stats_marked(void);
u64 przero_stats_zeroed(void);
#endif
