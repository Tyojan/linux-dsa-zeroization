// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/hugetlb.h>
#include <linux/pagewalk.h>
#include <linux/sched.h>

static bool przero_vma(struct vm_area_struct *vma)
{
	return vma_is_anonymous(vma) &&
		!(vma->vm_flags & (VM_SHARED | VM_PFNMAP | VM_MIXEDMAP)) &&
		!is_vm_hugetlb_page(vma);
}

/* Called with the page-table lock held, before publishing anon mappings. */
void folio_mark_zeroize_on_free(struct folio *folio,
			       struct vm_area_struct *vma)
{
	if (unlikely(READ_ONCE(vma->vm_mm->zeroize_on_free)) &&
	    przero_vma(vma) && folio_test_anon(folio) &&
	    !folio_is_zone_device(folio))
		SetPageWantInit(&folio->page);
}

static int przero_test_walk(unsigned long addr, unsigned long end,
			    struct mm_walk *walk)
{
	return !przero_vma(walk->vma);
}

static int przero_pte_entry(pte_t *ptep, unsigned long addr,
			    unsigned long end, struct mm_walk *walk)
{
	pte_t pte = ptep_get(ptep);
	struct page *page;

	if (!pte_present(pte))
		return 0;
	page = vm_normal_page(walk->vma, addr, pte);
	if (page)
		folio_mark_zeroize_on_free(page_folio(page), walk->vma);
	return 0;
}

static int przero_pmd_entry(pmd_t *pmdp, unsigned long addr,
			    unsigned long end, struct mm_walk *walk)
{
	spinlock_t *ptl = pmd_trans_huge_lock(pmdp, walk->vma);
	struct page *page;
	pmd_t pmd;

	if (!ptl)
		return 0;
	pmd = *pmdp;
	if (pmd_present(pmd)) {
		page = vm_normal_page_pmd(walk->vma, addr, pmd);
		if (page)
			folio_mark_zeroize_on_free(page_folio(page), walk->vma);
	}
	walk->action = ACTION_CONTINUE;
	spin_unlock(ptl);
	return 0;
}

static const struct mm_walk_ops przero_walk_ops = {
	.test_walk = przero_test_walk,
	.pmd_entry = przero_pmd_entry,
	.pte_entry = przero_pte_entry,
	.walk_lock = PGWALK_WRLOCK,
};

struct przero_check {
	unsigned long mapped;
	unsigned long missing;
	unsigned long first_addr;
	unsigned long first_pfn;
};

static void przero_check_page(struct page *page, unsigned long addr,
			      struct przero_check *check)
{
	struct folio *folio = page_folio(page);

	check->mapped++;
	if (PageWantInit(&folio->page))
		return;
	check->missing++;
	if (check->missing == 1) {
		check->first_addr = addr;
		check->first_pfn = page_to_pfn(page);
	}
}

static int przero_check_pte_entry(pte_t *ptep, unsigned long addr,
				 unsigned long end, struct mm_walk *walk)
{
	struct przero_check *check = walk->private;
	struct page *page;
	pte_t pte = ptep_get(ptep);

	if (!pte_present(pte))
		return 0;
	page = vm_normal_page(walk->vma, addr, pte);
	if (page)
		przero_check_page(page, addr, check);
	return 0;
}

static int przero_check_pmd_entry(pmd_t *pmdp, unsigned long addr,
				 unsigned long end, struct mm_walk *walk)
{
	struct przero_check *check = walk->private;
	spinlock_t *ptl = pmd_trans_huge_lock(pmdp, walk->vma);
	struct page *page;
	pmd_t pmd;

	if (!ptl)
		return 0;
	pmd = *pmdp;
	if (pmd_present(pmd)) {
		page = vm_normal_page_pmd(walk->vma, addr, pmd);
		if (page)
			przero_check_page(page, addr, check);
	}
	walk->action = ACTION_CONTINUE;
	spin_unlock(ptl);
	return 0;
}

static const struct mm_walk_ops przero_check_walk_ops = {
	.test_walk = przero_test_walk,
	.pmd_entry = przero_check_pmd_entry,
	.pte_entry = przero_check_pte_entry,
	.walk_lock = PGWALK_RDLOCK,
};

/*
 * exit_mmap() calls this before unmapping.  It is diagnostic only: a missing
 * marker is reported, but not repaired, so that marker-loss paths remain
 * visible during this PoC.
 */
void mm_check_zeroize_on_free(struct mm_struct *mm)
{
	struct przero_check check = {};
	int ret;

	if (!READ_ONCE(mm->zeroize_on_free))
		return;

	ret = walk_page_range(mm, 0, TASK_SIZE, &przero_check_walk_ops, &check);
	if (ret) {
		pr_warn("przero: pid %d exit mm %px: marker check failed: %d\n",
			current->pid, mm, ret);
		return;
	}
	if (check.missing)
		pr_warn("przero: pid %d exit mm %px: %lu/%lu private anon mappings lack PG_want_init (first va %#lx, pfn %#lx)\n",
			current->pid, mm, check.missing, check.mapped,
			check.first_addr, check.first_pfn);
}

/*
 * Enable policy before scanning resident mappings. VMA write locks also
 * serialize against faults using per-VMA locking. Nonresident swap/migration
 * entries are skipped; this PoC does not erase swap storage.
 * Disabling policy never revokes a physical page's existing wipe request.
 */
int mm_set_zeroize_on_free(struct mm_struct *mm, bool enable)
{
	int ret = 0;

	if (mmap_write_lock_killable(mm))
		return -EINTR;
	WRITE_ONCE(mm->zeroize_on_free, enable);
	if (enable)
		ret = walk_page_range(mm, 0, TASK_SIZE, &przero_walk_ops, NULL);
	mmap_write_unlock(mm);
	return ret;
}
