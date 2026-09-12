/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_DSA_H
#define _ASM_X86_DSA_H

#include <linux/dsa_page_clear.h>

struct dsa_page_clear_ops {
	/*
	 * Called with preemption disabled, under RCU. Must not sleep.
	 * On return, no DMA may still access the destination or completion
	 * record. Return true only if the entire range has been zeroed.
	 */
	bool (*clear)(struct dsa_page_clear_ops *ops, void *addr,
		      unsigned int npages);
	/*
	 * Same calling context as clear(). Pages have passed free preparation,
	 * have zero references, and are on neither PCP nor buddy lists.
	 * True transfers ownership: complete exactly once, after all DMA access
	 * (including unmapping) ends, via dsa_free_pages_complete(). Completion
	 * may precede this callback's return. False leaves ownership with caller
	 * and guarantees no DMA or completion callback can still access pages.
	 */
	bool (*defer)(struct dsa_page_clear_ops *ops, struct page *page,
		      unsigned int order);
	/* Sleepable; submissions have stopped. Complete every accepted request. */
	void (*drain)(struct dsa_page_clear_ops *ops);
};

/* Versioned symbols prevent loading the former unsafe provider ABI. */
int dsa_register_page_clear_v2(struct dsa_page_clear_ops *ops);
void dsa_unregister_page_clear_v2(struct dsa_page_clear_ops *ops);

#endif /* _ASM_X86_DSA_H */
