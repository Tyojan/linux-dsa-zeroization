// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <asm/dsa.h>
#include <asm/page.h>

static DEFINE_MUTEX(dsa_page_clear_lock);
static struct dsa_page_clear_ops __rcu *dsa_page_clear_provider;

int dsa_register_page_clear(struct dsa_page_clear_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->clear)
		return -EINVAL;

	mutex_lock(&dsa_page_clear_lock);
	if (rcu_access_pointer(dsa_page_clear_provider))
		ret = -EBUSY;
	else
		rcu_assign_pointer(dsa_page_clear_provider, ops);
	mutex_unlock(&dsa_page_clear_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(dsa_register_page_clear);

void dsa_unregister_page_clear(struct dsa_page_clear_ops *ops)
{
	mutex_lock(&dsa_page_clear_lock);
	if (rcu_access_pointer(dsa_page_clear_provider) == ops) {
		RCU_INIT_POINTER(dsa_page_clear_provider, NULL);
		synchronize_rcu();
	}
	mutex_unlock(&dsa_page_clear_lock);
}
EXPORT_SYMBOL_GPL(dsa_unregister_page_clear);

bool dsa_clear_pages(void *addr, unsigned int npages)
{
	struct dsa_page_clear_ops *ops;
	bool cleared = false;

	/*
	 * DMA mapping may take locks also held by callers freeing pages.
	 * Leave atomic/IRQ contexts to the CPU. Disabling preemption below
	 * also makes any recursive free from DMA mapping take that fallback.
	 */
	if (!rcu_access_pointer(dsa_page_clear_provider) ||
	    !in_task() || !preemptible())
		return false;

	preempt_disable();
	rcu_read_lock();
	ops = rcu_dereference(dsa_page_clear_provider);
	if (ops)
		cleared = ops->clear(ops, addr, npages);
	rcu_read_unlock();
	preempt_enable();
	return cleared;
}
EXPORT_SYMBOL_GPL(dsa_clear_pages);
