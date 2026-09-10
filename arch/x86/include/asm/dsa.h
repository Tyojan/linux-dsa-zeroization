/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_DSA_H
#define _ASM_X86_DSA_H

#include <linux/types.h>

struct dsa_page_clear_ops {
	/*
	 * Called with preemption disabled, under RCU. Must not sleep.
	 * On return, no DMA may still access the destination or completion
	 * record. Return true only if the entire range has been zeroed.
	 */
	bool (*clear)(struct dsa_page_clear_ops *ops, void *addr,
		      unsigned int npages);
};

/* Only one provider may be registered. Unregister waits for active calls. */
int dsa_register_page_clear(struct dsa_page_clear_ops *ops);
void dsa_unregister_page_clear(struct dsa_page_clear_ops *ops);

#endif /* _ASM_X86_DSA_H */
