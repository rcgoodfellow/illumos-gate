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
 * Copyright 2021 Oxide Computer Co.
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
#include <sys/mach_mmu.h>
#include <sys/boot_debug.h>
#include <vm/kboot_mmu.h>
#include <vm/hat_pte.h>
#include <vm/hat_i86.h>
#include <vm/seg_kmem.h>
#include <sys/sysmacros.h>

static uint_t shift_amt_pae[] = {12, 21, 30, 39};
static uint_t *shift_amt = shift_amt_pae;
uint_t ptes_per_table = 512;		/* consumed by shm */
static uint_t pte_size = 8;
static uint32_t lpagesize = TWO_MEG;
static paddr_t top_page_table = 0;
static uint_t top_level = 3;

/*
 * Page table and memory stuff.
 */
static caddr_t window;
static x86pte_t *pte_to_window;

uint_t kbm_nucleus_size = TWO_MEG;

#define	BOOT_SHIFT(l)	(shift_amt[l])
#define	BOOT_SZ(l)	((size_t)1 << BOOT_SHIFT(l))
#define	BOOT_OFFSET(l)	(BOOT_SZ(l) - 1)
#define	BOOT_MASK(l)	(~BOOT_OFFSET(l))

/*
 * When we get here, %cr3 points to the top-level pagetables established by
 * the bootloader.  Our first goal is to create new pagetables at the top of
 * memory, copying the entries we actually need.  The loader has helpfully
 * marked the entries corresponding to our own segments by setting the
 * architecturally-defined software-available bit 9 in each corresponding PTE.
 * We clear this bit when building the new pagetable.  Additionally, we create
 * PTEs for identity-mapping the UART used by the earlyboot console device.
 * This is a bit hackish and it would be nice to make it cleaner, but we're
 * really just trying to get through this so that segmap etc can be used to
 * do this properly.
 *
 * Once we've built the new pagetable, we switch to it.  This has the effect of
 * unmapping the loader and freeing up all memory other than the kernel itself.
 * Importantly, this also means that all our boot-time properties, as well as
 * things like the ramdisk, modules, etc. that they may point to, will be
 * unmapped.  We'll map them in later as we need them.
 */
void
kbm_init(const struct bsys_mem *memlists)
{
	ulong_t loader_pt_base = getcr3();

	/*
	 * XXX For now we just grab the existing table the loader set up, but
	 * we may want to create our own from scratch and then switch to it.
	 */
	top_page_table = getcr3();
	DBG(top_page_table);
	window = (caddr_t)MMU_PAGESIZE;
	DBG(window);
	pte_to_window = (x86pte_t *)(uintptr_t)0x75ff7008;	/* XXXBOOT */
	DBG(pte_to_window);
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

	*pte_to_window = physaddr | pt_bits;
	mmu_invlpg(window);

	DBG(window);
	return (window);
}

/*
 * Add a mapping for the physical page at the given virtual address.
 */
void
kbm_map(uintptr_t va, paddr_t pa, uint_t level, x86pte_t flags)
{
	x86pte_t *ptep;
	paddr_t pte_physaddr;
	x86pte_t pteval;

	if (khat_running)
		panic("kbm_map() called too late");

	DBG_MSG("kbm_map(%lx, %lx, %x, %lx)\n", va, pa, level, flags);

	pteval = pa | PT_NOCONSIST | PT_VALID | flags;
	if (level >= 1)
		pteval |= PT_PAGESIZE;

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
	x86pte_t pte_val = pfn_to_pa(pfn) | PT_WRITABLE |
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

	if (!(old_pte & PT_VALID) || old_pte == -1)
		return (PFN_INVALID);
	return (mmu_btop(old_pte));
}


/*
 * Change a boot loader page table 4K mapping to read only.
 */
void
kbm_read_only(uintptr_t va, paddr_t pa)
{
	x86pte_t pte_val = pa | PT_NOCONSIST | PT_REF | PT_MOD | PT_VALID;

	x86pte_t *ptep;
	level_t	level = 0;

	ptep = find_pte(va, NULL, level, 0);
	if (ptep == NULL)
		bop_panic("kbm_read_only: find_pte returned NULL");

	*ptep = pte_val;
	mmu_invlpg((caddr_t)va);
}

/*
 * Allocate virtual address space from the imaginary earlyboot arena.  These
 * mappings will be torn down automatically in startup.c when we call
 * clear_boot_mappings().  The address returned is not mapped; the caller is
 * responsible for setting up a mapping via kbm_map() etc.
 */
uintptr_t
kbm_valloc(size_t size, paddr_t align)
{
	/*
	 * Next available virtual address to allocate.  Do not allocate page 0.
	 */
	static uintptr_t next_virt = (uintptr_t)(MMU_PAGESIZE * 2);
	uintptr_t rv;

	DBG_MSG("kbm_valloc: sz %lx align %lx", size, align);

	next_virt = P2ROUNDUP(next_virt, (uintptr_t)align);
	rv = next_virt;
	next_virt += size;

	DBG_MSG(" = %lx\n", rv);

	return (rv);
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

	save_pte = *pte_to_window;

	return (kbm_remap_window(pa, 0));
}

void
kbm_pop(void)
{
	*pte_to_window = save_pte;
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

	*pteval = new_table | PT_VALID | PT_REF | PT_USER | PT_WRITABLE;

	return (new_table);
}

x86pte_t *
map_pte(paddr_t table, uint_t index)
{
	void *table_ptr = kbm_remap_window(table, 0);
	return ((x86pte_t *)((caddr_t)table_ptr + index * pte_size));
}

/*
 * Return the index corresponding to a virt address at a given page table level.
 */
static uint_t
vatoindex(uint64_t va, uint_t level)
{
	return ((va >> shift_amt[level]) & (ptes_per_table - 1));
}

/*
 * Return a pointer to the page table entry that maps a virtual address.
 * If there is no page table and probe_only is not set, one is created.
 */
x86pte_t *
find_pte(uint64_t va, paddr_t *pa, uint_t level, uint_t probe_only)
{
	uint_t l;
	uint_t index;
	paddr_t table;
	x86pte_t *pp;

	if (pa)
		*pa = 0;

	/*
	 * Walk down the page tables creating any needed intermediate tables.
	 */
	table = top_page_table;
	for (l = top_level; l != level; --l) {
		uint64_t pteval;
		paddr_t new_table;

		index = vatoindex(va, l);
		pteval = get_pteval(table, index);

		/*
		 * Life is easy if we find the pagetable.  We just use it.
		 */
		if (pteval & PT_VALID) {
			table = pteval & MMU_PAGEMASK;
			continue;
		}

		if (probe_only)
			return (NULL);

		new_table = make_ptable(&pteval, l);
		set_pteval(table, index, l, pteval);

		table = new_table;
	}

	/*
	 * Return a pointer into the current pagetable.
	 */
	index = vatoindex(va, l);
	if (pa)
		*pa = table + index * pte_size;
	pp = map_pte(table, index);

	return (pp);
}

#ifdef DEBUG
/*
 * Dump out the contents of page tables, assuming that they are all identity
 * mapped; this will panic otherwise so use with extreme caution.
 */
void
dump_tables(void)
{
	uint_t save_index[4];	/* for recursion */
	char *save_table[4];	/* for recursion */
	uint_t	l;
	uint64_t va;
	uint64_t pgsize;
	int index;
	int i;
	x86pte_t pteval;
	char *table;
	static char *tablist = "\t\t\t";
	char *tabs = tablist + 3 - top_level;
	uint64_t pa, pa1;

	bop_printf(NULL, "Pagetables:\n");
	table = (char *)(uintptr_t)top_page_table;
	l = top_level;
	va = 0;
	for (index = 0; index < ptes_per_table; ++index) {
		pgsize = 1UL << shift_amt[l];
		pteval = ((x86pte_t *)table)[index];
		if (pteval == 0)
			goto next_entry;

		bop_printf(NULL, "%s %p[0x%x] = %" PRIx64 ", va=%" PRIx64,
		    tabs + l, (void *)table, index, (uint64_t)pteval, va);
		pa = pteval & MMU_PAGEMASK;
		bop_printf(NULL, " physaddr=%lx\n", pa);

		/*
		 * Don't try to walk hypervisor private pagetables
		 */
		if ((l > 2 || (l > 0 && (pteval & PT_PAGESIZE) == 0))) {
			save_table[l] = table;
			save_index[l] = index;
			--l;
			index = -1;
			table = (char *)(uintptr_t)(pteval & MMU_PAGEMASK);
			goto recursion;
		}

		/*
		 * shorten dump for consecutive mappings
		 */
		for (i = 1; index + i < ptes_per_table; ++i) {
			pteval = ((x86pte_t *)table)[index + i];
			if (pteval == 0)
				break;
			pa1 = pteval & MMU_PAGEMASK;
			if (pa1 != pa + i * pgsize)
				break;
		}
		if (i > 2) {
			bop_printf(NULL, "%s...\n", tabs + l);
			va += pgsize * (i - 2);
			index += i - 2;
		}
next_entry:
		va += pgsize;
		if (l == 3 && index == 255)	/* VA hole */
			va = 0xffff800000000000UL;
recursion:
		;
	}
	if (l < top_level) {
		++l;
		index = save_index[l];
		table = save_table[l];
		goto recursion;
	}
}
#endif
