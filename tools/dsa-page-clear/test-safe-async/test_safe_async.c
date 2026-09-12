// SPDX-License-Identifier: GPL-2.0
/* Fake provider: tests allocator ownership without requiring a DSA device. */
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/dsa.h>

#define PROBES 1024

static struct page *target;
static struct page *held;
static unsigned int held_order;
static unsigned int accepted;
static bool immediate, reject;
static int failures;

static bool test_clear(struct dsa_page_clear_ops *ops, void *addr, unsigned int n)
{
	return false;
}

static bool test_defer(struct dsa_page_clear_ops *ops, struct page *page,
			unsigned int order)
{
	unsigned int i;

	/* Never capture unrelated kernel frees. */
	if (cmpxchg(&target, page, NULL) != page || reject)
		return false;
	for (i = 0; i < (1U << order); i++)
		if (page_count(page + i) || PageCompound(page + i) || PageBuddy(page + i))
			failures++;
	accepted++;
	if (immediate) {
		memset(page_address(page), 0, PAGE_SIZE << order);
		dsa_free_pages_complete(page, order, true);
	} else {
		held_order = order;
		held = page;
	}
	return true;
}

static void test_drain(struct dsa_page_clear_ops *ops)
{
	struct page *page = held;

	held = NULL;
	if (page)
		dsa_free_pages_complete(page, held_order, false);
}

static struct dsa_page_clear_ops test_ops = {
	.clear = test_clear,
	.defer = test_defer,
	.drain = test_drain,
};

enum test_case { DELAY_SUCCESS, DELAY_ERROR, INLINE_SUCCESS, REJECT, DRAIN };

static int run_case(unsigned int order, bool compound, enum test_case which)
{
	struct page **probes, *page;
	unsigned long pfn;
	unsigned int i, count = 0, before = accepted;
	int ret = -EINVAL;
	bool found = false;
	gfp_t flags = GFP_KERNEL | (compound ? __GFP_COMP : 0);

	probes = kcalloc(PROBES, sizeof(*probes), GFP_KERNEL);
	if (!probes)
		return -ENOMEM;
	page = alloc_pages(flags, order);
	if (!page) {
		kfree(probes);
		return -ENOMEM;
	}
	pfn = page_to_pfn(page);
	memset(page_address(page), 0xa5, PAGE_SIZE << order);
	immediate = which == INLINE_SUCCESS;
	reject = which == REJECT;
	WRITE_ONCE(target, page);
	__free_pages(page, order);
	if (READ_ONCE(target) || accepted != before + !reject) {
		pr_err("dsa-safe-test: callback not exercised (init_on_free=1 required)\n");
		WRITE_ONCE(target, NULL);
		goto out;
	}

	if (which == DELAY_SUCCESS || which == DELAY_ERROR || which == DRAIN) {
		if (held != page || memchr_inv(page_address(page), 0xa5, PAGE_SIZE << order))
			goto out;
		/* Allocate pressure while the dirty range is held: it must not return. */
		for (i = 0; i < 128; i++) {
			struct page *probe = alloc_pages(flags | __GFP_NOWARN, order);

			if (!probe)
				break;
			probes[count++] = probe;
			if (page_to_pfn(probe) == pfn)
				goto out;
		}
		if (which == DRAIN) {
			dsa_unregister_page_clear_v2(&test_ops);
			if (held || dsa_register_page_clear_v2(&test_ops))
				goto out;
		} else {
			held = NULL;
			if (which == DELAY_SUCCESS)
				memset(page_address(page), 0, PAGE_SIZE << order);
			else
				/* Simulate an error after a partial DMA write. */
				memset(page_address(page), 0, PAGE_SIZE / 2);
			dsa_free_pages_complete(page, order, which == DELAY_SUCCESS);
		}
	}

	/* Own the original range again before reading it; never read freed pages. */
	while (count < PROBES) {
		struct page *probe = alloc_pages(flags | __GFP_NOWARN, order);

		if (!probe)
			break;
		probes[count++] = probe;
		if (page_to_pfn(probe) == pfn) {
			found = true;
			if (memchr_inv(page_address(probe), 0, PAGE_SIZE << order))
				goto out;
			break;
		}
	}
	if (found && !failures)
		ret = 0;
out:
	WRITE_ONCE(target, NULL);
	test_drain(&test_ops);
	for (i = 0; i < count; i++)
		__free_pages(probes[i], order);
	kfree(probes);
	pr_info("dsa-safe-test: order=%u compound=%u case=%u %s\n",
		order, compound, which, ret ? "FAIL" : "PASS");
	return ret;
}

static int __init test_safe_init(void)
{
	unsigned int order, which;
	int ret = dsa_register_page_clear_v2(&test_ops);

	if (ret)
		return ret;
	for (order = 0; order <= 3; order += 3) {
		for (which = DELAY_SUCCESS; which <= DRAIN; which++) {
			ret = run_case(order, false, which);
			if (ret)
				goto out;
		}
	}
	ret = run_case(3, true, DELAY_ERROR);
out:
	dsa_unregister_page_clear_v2(&test_ops);
	pr_info("dsa-safe-test: %s\n", ret ? "FAILED" : "ALL PASS");
	return ret;
}

static void __exit test_safe_exit(void) {}
module_init(test_safe_init);
module_exit(test_safe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DSA deferred allocator ownership and CPU repair tests (no hardware)");
