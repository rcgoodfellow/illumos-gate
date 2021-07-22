/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2018 Joyent, Inc.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/archsystm.h>
#include <sys/debug.h>
#include <sys/bootconf.h>
#include <sys/bootsvcs.h>
#include <sys/bootinfo.h>
#include <sys/mman.h>
#include <sys/cmn_err.h>
#include <sys/param.h>
#include <sys/machparam.h>
#include <sys/machsystm.h>
#include <sys/promif.h>
#include <sys/kobj.h>
#ifdef __xpv
#include <sys/hypervisor.h>
#endif
#include <vm/kboot_mmu.h>
#include <vm/hat_pte.h>
#include <vm/hat_i86.h>
#include <vm/seg_kmem.h>

#if 0
/*
 * Joe's debug printing
 */
#define	DBG(x)    \
	bop_printf(NULL, "kboot_mmu.c: %s is %" PRIx64 "\n", #x, (uint64_t)(x));
#else
#define	DBG(x)	/* naught */
#endif

/*
 * Page table and memory stuff.
 */
static caddr_t window;
static caddr_t pte_to_window;

uint_t kbm_nucleus_size = 0;

#define	BOOT_SHIFT(l)	(shift_amt[l])
#define	BOOT_SZ(l)	((size_t)1 << BOOT_SHIFT(l))
#define	BOOT_OFFSET(l)	(BOOT_SZ(l) - 1)
#define	BOOT_MASK(l)	(~BOOT_OFFSET(l))

/*
 * Initialize memory management parameters for boot time page table management
 */
void
kbm_init(struct xboot_info *bi)
{
	/*
	 * Configure mmu information.  XXX Most of this file should move to
	 * intel, and we can share it with i86pc by setting up these parameters.
	 */
	kbm_nucleus_size = (uintptr_t)bi->bi_kseg_size;
	window = bi->bi_pt_window;	/* XXX allocate a 4K page */
	DBG(window);
	pte_to_window = bi->bi_pte_to_pt_window;	/* XXX find_pte() */
	DBG(pte_to_window);

	shift_amt = shift_amt_pae;
	ptes_per_table = 512;
	pte_size = 8;
	lpagesize = TWO_MEG;
	top_level = 3;

	/*
 	 * XXX We can keep using the pagetable the bootloader was using, which
 	 * is probably simplest, or we can go build our own pagetables
 	 * somewhere else, with blackjack, and hookers.  For now we should just
 	 * grab this out of %cr3.
 	 */
	top_page_table = bi->bi_top_page_table;
	DBG(top_page_table);
}

/*
 * Change the addressible page table window to point at a given page
 */
/*ARGSUSED*/
void *
kbm_remap_window(paddr_t physaddr, int writeable)
{
	x86pte_t pt_bits = PT_NOCONSIST | PT_VALID | PT_WRITABLE;

	DBG(physaddr);

	*((x86pte_t *)pte_to_window) = physaddr | pt_bits;
	mmu_invlpg(window);

	DBG(window);
	return (window);
}

/*
 * Add a mapping for the physical page at the given virtual address.
 */
void
kbm_map(uintptr_t va, paddr_t pa, uint_t level, uint_t is_kernel)
{
	x86pte_t *ptep;
	paddr_t pte_physaddr;
	x86pte_t pteval;

	if (khat_running)
		panic("kbm_map() called too late");

	pteval = pa_to_ma(pa) | PT_NOCONSIST | PT_VALID | PT_WRITABLE;
	if (level >= 1)
		pteval |= PT_PAGESIZE;
	if (is_kernel)
		pteval |= PT_GLOBAL;

	/*
	 * Find the pte that will map this address. This creates any
	 * missing intermediate level page tables.
	 */
	ptep = find_pte(va, &pte_physaddr, level, 0);
	if (ptep == NULL)
		bop_panic("kbm_map: find_pte returned NULL");

	*ptep = pteval;
	mmu_invlpg((caddr_t)va);
}

/*
 * Probe the boot time page tables to find the first mapping
 * including va (or higher) and return non-zero if one is found.
 * va is updated to the starting address and len to the pagesize.
 * pp will be set to point to the 1st page_t of the mapped page(s).
 *
 * Note that if va is in the middle of a large page, the returned va
 * will be less than what was asked for.
 */
int
kbm_probe(uintptr_t *va, size_t *len, pfn_t *pfn, uint_t *prot)
{
	uintptr_t	probe_va;
	x86pte_t	*ptep;
	paddr_t		pte_physaddr;
	x86pte_t	pte_val;
	level_t		l;

	if (khat_running)
		panic("kbm_probe() called too late");
	*len = 0;
	*pfn = PFN_INVALID;
	*prot = 0;
	probe_va = *va;
restart_new_va:
	l = top_level;
	for (;;) {
		if (IN_VA_HOLE(probe_va))
			probe_va = mmu.hole_end;

		if (IN_HYPERVISOR_VA(probe_va))
			return (0);

		/*
		 * If we don't have a valid PTP/PTE at this level
		 * then we can bump VA by this level's pagesize and try again.
		 * When the probe_va wraps around, we are done.
		 */
		ptep = find_pte(probe_va, &pte_physaddr, l, 1);
		if (ptep == NULL)
			bop_panic("kbm_probe: find_pte returned NULL");

		pte_val = *ptep;
		if (!PTE_ISVALID(pte_val)) {
			probe_va = (probe_va & BOOT_MASK(l)) + BOOT_SZ(l);
			if (probe_va <= *va)
				return (0);
			goto restart_new_va;
		}

		/*
		 * If this entry is a pointer to a lower level page table
		 * go down to it.
		 */
		if (!PTE_ISPAGE(pte_val, l)) {
			ASSERT(l > 0);
			--l;
			continue;
		}

		/*
		 * We found a boot level page table entry
		 */
		*len = BOOT_SZ(l);
		*va = probe_va & ~(*len - 1);
		*pfn = PTE2PFN(pte_val, l);


		*prot = PROT_READ | PROT_EXEC;
		if (PTE_GET(pte_val, PT_WRITABLE))
			*prot |= PROT_WRITE;

		if (PTE_GET(pte_val, mmu.pt_nx))
			*prot &= ~PROT_EXEC;

		return (1);
	}
}


/*
 * Destroy a boot loader page table 4K mapping.
 */
void
kbm_unmap(uintptr_t va)
{
	if (khat_running)
		panic("kbm_unmap() called too late");
	else {
		x86pte_t *ptep;
		level_t	level = 0;
		uint_t  probe_only = 1;

		ptep = find_pte(va, NULL, level, probe_only);
		if (ptep == NULL)
			return;

		*ptep = 0;
		mmu_invlpg((caddr_t)va);
	}
}


/*
 * Change a boot loader page table 4K mapping.
 * Returns the pfn of the old mapping.
 */
pfn_t
kbm_remap(uintptr_t va, pfn_t pfn)
{
	x86pte_t *ptep;
	level_t	level = 0;
	uint_t  probe_only = 1;
	x86pte_t pte_val = pa_to_ma(pfn_to_pa(pfn)) | PT_WRITABLE |
	    PT_NOCONSIST | PT_VALID;
	x86pte_t old_pte;

	if (khat_running)
		panic("kbm_remap() called too late");
	ptep = find_pte(va, NULL, level, probe_only);
	if (ptep == NULL)
		bop_panic("kbm_remap: find_pte returned NULL");

	old_pte = *ptep;
	*((x86pte_t *)ptep) = pte_val;
	mmu_invlpg((caddr_t)va);

	if (!(old_pte & PT_VALID) || ma_to_pa(old_pte) == -1)
		return (PFN_INVALID);
	return (mmu_btop(ma_to_pa(old_pte)));
}


/*
 * Change a boot loader page table 4K mapping to read only.
 */
void
kbm_read_only(uintptr_t va, paddr_t pa)
{
	x86pte_t pte_val = pa_to_ma(pa) |
	    PT_NOCONSIST | PT_REF | PT_MOD | PT_VALID;

	x86pte_t *ptep;
	level_t	level = 0;

	ptep = find_pte(va, NULL, level, 0);
	if (ptep == NULL)
		bop_panic("kbm_read_only: find_pte returned NULL");

	*ptep = pte_val;
	mmu_invlpg((caddr_t)va);
}

/*
 * interfaces for kernel debugger to access physical memory
 */
static x86pte_t save_pte;

void *
kbm_push(paddr_t pa)
{
	static int first_time = 1;

	if (first_time) {
		first_time = 0;
		return (window);
	}

	save_pte = *((x86pte_t *)pte_to_window);

	return (kbm_remap_window(pa, 0));
}

void
kbm_pop(void)
{
	*((x86pte_t *)pte_to_window) = save_pte;
	mmu_invlpg(window);
}

x86pte_t
get_pteval(paddr_t table, uint_t index)
{
	void *table_ptr = kbm_remap_window(table, 0);

	return (((x86pte_t *)table_ptr)[index]);
}

void
set_pteval(paddr_t table, uint_t index, uint_t level, x86pte_t pteval)
{
	void *table_ptr = kbm_remap_window(table, 0);

	((x86pte_t *)table_ptr)[index] = pteval;
}

paddr_t
make_ptable(x86pte_t *pteval, uint_t level)
{
	paddr_t new_table;
	void *table_ptr;

	new_table = do_bop_phys_alloc(MMU_PAGESIZE, MMU_PAGESIZE);
	table_ptr = kbm_remap_window(new_table, 1);
	bzero(table_ptr, MMU_PAGESIZE);

	*pteval = pa_to_ma(new_table) |
	    PT_VALID | PT_REF | PT_USER | PT_WRITABLE;

	return (new_table);
}

x86pte_t *
map_pte(paddr_t table, uint_t index)
{
	void *table_ptr = kbm_remap_window(table, 0);
	return ((x86pte_t *)((caddr_t)table_ptr + index * pte_size));
}
