/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DSA_PAGE_CLEAR_H
#define _LINUX_DSA_PAGE_CLEAR_H

#include <linux/types.h>

struct page;

#ifdef CONFIG_X86_DSA_PAGE_CLEAR
bool dsa_defer_clear_pages(struct page *page, unsigned int order);
void dsa_free_pages_complete(struct page *page, unsigned int order, bool success);
#else
static inline bool dsa_defer_clear_pages(struct page *page, unsigned int order)
{
	return false;
}
#endif

#endif
