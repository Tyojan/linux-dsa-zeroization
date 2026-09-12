// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/suspend.h>
#include <asm/dsa.h>
#include <asm/page.h>

static DEFINE_MUTEX(dsa_page_clear_lock);
static struct dsa_page_clear_ops __rcu *dsa_page_clear_provider;
static unsigned int dsa_page_clear_paused;

#define DSA_PAUSE_MEMORY BIT(0)
#define DSA_PAUSE_PM BIT(1)

int dsa_register_page_clear_v2(struct dsa_page_clear_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->clear || (ops->defer && !ops->drain))
		return -EINVAL;

	mutex_lock(&dsa_page_clear_lock);
	if (rcu_access_pointer(dsa_page_clear_provider))
		ret = -EBUSY;
	else
		rcu_assign_pointer(dsa_page_clear_provider, ops);
	mutex_unlock(&dsa_page_clear_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(dsa_register_page_clear_v2);

void dsa_unregister_page_clear_v2(struct dsa_page_clear_ops *ops)
{
	mutex_lock(&dsa_page_clear_lock);
	if (rcu_access_pointer(dsa_page_clear_provider) == ops) {
		RCU_INIT_POINTER(dsa_page_clear_provider, NULL);
		synchronize_rcu();
		if (ops->drain)
			ops->drain(ops);
	}
	mutex_unlock(&dsa_page_clear_lock);
}
EXPORT_SYMBOL_GPL(dsa_unregister_page_clear_v2);

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
	if (ops && !READ_ONCE(dsa_page_clear_paused))
		cleared = ops->clear(ops, addr, npages);
	rcu_read_unlock();
	preempt_enable();
	return cleared;
}
EXPORT_SYMBOL_GPL(dsa_clear_pages);

bool dsa_defer_clear_pages(struct page *page, unsigned int order)
{
	struct dsa_page_clear_ops *ops;
	bool accepted = false;

	if (!rcu_access_pointer(dsa_page_clear_provider) ||
	    !in_task() || !preemptible())
		return false;
	preempt_disable();
	rcu_read_lock();
	ops = rcu_dereference(dsa_page_clear_provider);
	if (ops && ops->defer && !READ_ONCE(dsa_page_clear_paused))
		accepted = ops->defer(ops, page, order);
	rcu_read_unlock();
	preempt_enable();
	return accepted;
}

/* Drain before memory can be offlined or the DMA device suspended. */
static void dsa_page_clear_pause(unsigned int reason, bool pause)
{
	struct dsa_page_clear_ops *ops;

	mutex_lock(&dsa_page_clear_lock);
	if (pause) {
		WRITE_ONCE(dsa_page_clear_paused, dsa_page_clear_paused | reason);
		synchronize_rcu();
		ops = rcu_dereference_protected(dsa_page_clear_provider,
					lockdep_is_held(&dsa_page_clear_lock));
		if (ops && ops->drain)
			ops->drain(ops);
	} else {
		WRITE_ONCE(dsa_page_clear_paused, dsa_page_clear_paused & ~reason);
	}
	mutex_unlock(&dsa_page_clear_lock);
}

static int dsa_page_clear_memory(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	switch (action) {
	case MEM_GOING_OFFLINE:
		dsa_page_clear_pause(DSA_PAUSE_MEMORY, true);
		break;
	case MEM_OFFLINE:
	case MEM_CANCEL_OFFLINE:
		dsa_page_clear_pause(DSA_PAUSE_MEMORY, false);
		break;
	}
	return NOTIFY_OK;
}

static int dsa_page_clear_pm(struct notifier_block *nb,
			     unsigned long action, void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		dsa_page_clear_pause(DSA_PAUSE_PM, true);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		dsa_page_clear_pause(DSA_PAUSE_PM, false);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block dsa_memory_nb = { .notifier_call = dsa_page_clear_memory };
static struct notifier_block dsa_pm_nb = { .notifier_call = dsa_page_clear_pm };

static int __init dsa_page_clear_init(void)
{
	int ret = register_memory_notifier(&dsa_memory_nb);

	if (ret)
		return ret;
	ret = register_pm_notifier(&dsa_pm_nb);
	if (ret)
		unregister_memory_notifier(&dsa_memory_nb);
	return ret;
}
subsys_initcall(dsa_page_clear_init);
