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
 * Copyright 2021 Oxide Computer Company
 */

/*
 * This file implements a collection of routines that can be used to initialize
 * various aspects of the Milan CPU cores.
 */

#include <milan/milan_physaddrs.h>
#include <milan/milan_ccx.h>
#include <sys/boot_physmem.h>
#include <sys/x86_archext.h>

void
milan_ccx_mmio_init(uint64_t pa)
{
	uint64_t val = AMD_MMIOCFG_BASEADDR_ENABLE;
	val |= AMD_MMIOCFG_BASEADDR_BUSRANGE_256 <<
	    AMD_MMIOCFG_BASEADDR_BUSRANGE_SHIFT;
	val |= (pa & AMD_MMIOCFG_BASEADDR_MASK);
	wrmsr(MSR_AMD_MMIOCFG_BASEADDR, val);

	eb_physmem_reserve_range(pa,
	    (1UL << AMD_MMIOCFG_BASEADDR_BUSRANGE_256) <<
	    AMD_MMIOCFG_BASEADDR_ADDR_SHIFT, EBPR_NOT_RAM);
}

void
milan_ccx_physmem_init(void)
{
	/*
	 * Due to undocumented, unspecified, and unknown bugs in the IOMMU
	 * (supposedly), there is a hole in RAM below 1 TiB.  It may or may not
	 * be usable as MMIO space but regardless we need to not treat it as
	 * RAM.
	 */
	eb_physmem_reserve_range(MILAN_PHYSADDR_MYSTERY_HOLE,
	    MILAN_PHYSADDR_MYSTERY_HOLE_END - MILAN_PHYSADDR_MYSTERY_HOLE,
	    EBPR_NOT_RAM);
}
