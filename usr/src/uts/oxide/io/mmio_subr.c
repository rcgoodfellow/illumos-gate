/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2022 Oxide Computer Co.
 */

/*
 * Glue for register-driven MMIO accesses.  See sys/io/mmioreg.h.  These
 * routines are intended for kernel use and will blow assertions if used by DDI
 * consumers.
 *
 * This is not machdep code, though the implementation of device_arena_*() is,
 * and should eventually be moved to uts/intel once we're happy with it.
 */

#include <sys/cmn_err.h>
#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/machsystm.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sunddi.h>
#include <sys/types.h>
#include <sys/io/mmioreg.h>
#include <vm/hat.h>
#include <vm/seg_kmem.h>

mmio_reg_block_t
mmio_reg_block_map(const smn_unit_t unit, const mmio_reg_block_phys_t phys)
{
	ASSERT3S(unit, !=, SMN_UNIT_UNKNOWN);

	const uintptr_t loff = phys.mrbp_base & PAGEOFFSET;
	const uintptr_t moff = phys.mrbp_base & MMU_PAGEOFFSET;

	const uintptr_t nlp = btopr(phys.mrbp_len + loff);
	const uintptr_t nmp = mmu_btopr(phys.mrbp_len + moff);

	const caddr_t va = device_arena_alloc(ptob(nlp), VM_SLEEP);

	hat_devload(kas.a_hat, va, mmu_ptob(nmp), mmu_btop(phys.mrbp_base),
	    PROT_READ | PROT_WRITE | HAT_STRICTORDER, HAT_LOAD_LOCK);

	const mmio_reg_block_t block = {
	    .mrb_unit = unit,
	    .mrb_va = (const caddr_t)((const uintptr_t)va + loff),
	    .mrb_phys = phys
	};

	return (block);
}

void
mmio_reg_block_unmap(mmio_reg_block_t block)
{
	ASSERT0(block.mrb_flags & MRBF_DDI);

	const uintptr_t loff = (const uintptr_t)block.mrb_va & PAGEOFFSET;
	const uintptr_t moff = block.mrb_phys.mrbp_base & MMU_PAGEOFFSET;

	const uintptr_t nlp = btopr(block.mrb_phys.mrbp_len + loff);
	const uintptr_t nmp = mmu_btopr(block.mrb_phys.mrbp_len + moff);

	const uintptr_t vlbase = (const uintptr_t)block.mrb_va & PAGEMASK;
	const uintptr_t vmbase = (const uintptr_t)block.mrb_va & MMU_PAGEMASK;

	hat_unload(kas.a_hat, (const caddr_t)vmbase,
	    (const size_t)mmu_ptob(nmp), HAT_UNLOAD_UNLOCK);
	device_arena_free((const caddr_t)vlbase, (const size_t)ptob(nlp));
}

uint64_t
mmio_reg_read(const mmio_reg_t reg)
{
	ASSERT3P(reg.mr_acc, ==, NULL);

	switch (reg.mr_size) {
	case 1:
		return ((uint64_t)*(volatile uint8_t *)reg.mr_va);
	case 2:
		return ((uint64_t)*(volatile uint16_t *)reg.mr_va);
	case 4:
		return ((uint64_t)*(volatile uint32_t *)reg.mr_va);
	case 8:
		return (*(volatile uint64_t *)reg.mr_va);
	default:
		panic("invalid MMIO register size %u", (uint_t)reg.mr_size);
	}
}

void
mmio_reg_write(const mmio_reg_t reg, const uint64_t val)
{
	ASSERT3P(reg.mr_acc, ==, NULL);

	switch (reg.mr_size) {
	case 1:
		ASSERT0(val & (uint64_t)~UINT8_MAX);
		*(volatile uint8_t *)reg.mr_va = (const uint8_t)val;
		break;
	case 2:
		ASSERT0(val & (uint64_t)~UINT16_MAX);
		*(volatile uint16_t *)reg.mr_va = (const uint16_t)val;
		break;
	case 4:
		ASSERT0(val & (uint64_t)~UINT32_MAX);
		*(volatile uint32_t *)reg.mr_va = (const uint32_t)val;
		break;
	case 8:
		*(volatile uint64_t *)reg.mr_va = val;
		break;
	default:
		panic("invalid MMIO register size %u", (uint_t)reg.mr_size);
	}
}

int
x_ddi_reg_block_setup(dev_info_t *dip, uint_t regnum, ddi_device_acc_attr_t *ap,
    mmio_reg_block_t *rbp)
{
	int res;

	res = ddi_regs_map_setup(dip, regnum, &rbp->mrb_va, 0, 0, ap,
	    &rbp->mrb_acc);
	if (res != DDI_SUCCESS)
		return (res);

	rbp->mrb_flags |= MRBF_DDI;
	rbp->mrb_unit = SMN_UNIT_UNKNOWN;

	return (DDI_SUCCESS);
}

uint64_t
x_ddi_reg_get(const mmio_reg_t reg)
{
	switch (reg.mr_size) {
	case 1:
		return ((uint64_t)ddi_get8(reg.mr_acc, (uint8_t *)reg.mr_va));
	case 2:
		return ((uint64_t)ddi_get16(reg.mr_acc, (uint16_t *)reg.mr_va));
	case 4:
		return ((uint64_t)ddi_get32(reg.mr_acc, (uint32_t *)reg.mr_va));
	case 8:
		return (ddi_get64(reg.mr_acc, (uint64_t *)reg.mr_va));
	default:
		panic("invalid MMIO register size %u", (uint_t)reg.mr_size);
	}
}

void
x_ddi_reg_put(const mmio_reg_t reg, uint64_t val)
{
	switch (reg.mr_size) {
	case 1:
		ASSERT0(val & (uint64_t)~UINT8_MAX);
		ddi_put8(reg.mr_acc, (uint8_t *)reg.mr_va, (const uint8_t)val);
		break;
	case 2:
		ASSERT0(val & (uint64_t)~UINT16_MAX);
		ddi_put16(reg.mr_acc, (uint16_t *)reg.mr_va,
		    (const uint16_t)val);
		break;
	case 4:
		ASSERT0(val & (uint64_t)~UINT32_MAX);
		ddi_put32(reg.mr_acc, (uint32_t *)reg.mr_va,
		    (const uint32_t)val);
		break;
	case 8:
		ddi_put64(reg.mr_acc, (uint64_t *)reg.mr_va, val);
		break;
	default:
		panic("invalid MMIO register size %u", (uint_t)reg.mr_size);
	}
}
