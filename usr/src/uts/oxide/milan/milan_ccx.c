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
 * Copyright 2022 Oxide Computer Company
 */

/*
 * This file implements a collection of routines that can be used to initialize
 * various aspects of the Milan CPU cores.
 */

#include <milan/milan_physaddrs.h>
#include <sys/io/milan/fabric.h>
#include <sys/io/milan/ccx.h>
#include <sys/io/milan/ccx_impl.h>
#include <sys/boot_physmem.h>
#include <sys/x86_archext.h>

void
milan_ccx_mmio_init(uint64_t pa, boolean_t reserve)
{
	uint64_t val = AMD_MMIOCFG_BASEADDR_ENABLE;
	val |= AMD_MMIOCFG_BASEADDR_BUSRANGE_256 <<
	    AMD_MMIOCFG_BASEADDR_BUSRANGE_SHIFT;
	val |= (pa & AMD_MMIOCFG_BASEADDR_MASK);
	wrmsr(MSR_AMD_MMIOCFG_BASEADDR, val);

	if (reserve) {
		eb_physmem_reserve_range(pa,
		    (1UL << AMD_MMIOCFG_BASEADDR_BUSRANGE_256) <<
		    AMD_MMIOCFG_BASEADDR_ADDR_SHIFT, EBPR_NOT_RAM);
	}
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

/*
 * In this context, "thread" == AP.  SMT may or may not be enabled (by HW, FW,
 * or our own controls).  That may affect the number of threads per core, but
 * doesn't otherwise change anything here.
 *
 * This function is one-way; once a thread has been enabled, we are told that
 * we must never clear this bit.  What happens if we do, I do not know.  If the
 * thread was already booted, this function does nothing and returns B_FALSE;
 * otherwise it returns B_TRUE and the AP will be started.  There is no way to
 * fail; we don't construct a milan_thread_t for hardware that doesn't exist, so
 * it's always possible to perform this operation if what we are handed points
 * to genuine data.
 *
 * See MP boot theory in os/mp_startup.c
 */
boolean_t
milan_ccx_start_thread(const milan_thread_t *thread)
{
	milan_core_t *core = thread->mt_core;
	milan_ccx_t *ccx = core->mc_ccx;
	milan_ccd_t *ccd = ccx->mcx_ccd;
	uint8_t thr_ccd_idx;
	uint32_t en;

	VERIFY3U(CPU->cpu_id, ==, 0);

	thr_ccd_idx = ccx->mcx_logical_cxno;
	thr_ccd_idx *= ccx->mcx_ncores;
	thr_ccd_idx += core->mc_logical_coreno;
	thr_ccd_idx *= core->mc_nthreads;
	thr_ccd_idx += thread->mt_threadno;

	VERIFY3U(thr_ccd_idx, <, MILAN_MAX_CCXS_PER_CCD *
	    MILAN_MAX_CORES_PER_CCX * MILAN_MAX_THREADS_PER_CORE);

	en = milan_ccd_smupwr_read32(ccd, MILAN_SMUPWR_R_SMN_THREAD_ENABLE);
	if (MILAN_SMUPWR_R_GET_THREAD_ENABLE_T(en, thr_ccd_idx) != 0)
		return (B_FALSE);

	en = MILAN_SMUPWR_R_SET_THREAD_ENABLE_T(en, thr_ccd_idx);
	milan_ccd_smupwr_write32(ccd, MILAN_SMUPWR_R_SMN_THREAD_ENABLE, en);
	return (B_TRUE);
}

void
milan_ccx_set_brandstr(void)
{
	const milan_thread_t *thread = CPU->cpu_m.mcpu_hwthread;
	char str[CPUID_BRANDSTR_STRLEN + 1];
	uint_t n;

	if (milan_fabric_thread_get_brandstr(thread, str, sizeof (str)) >
	    CPUID_BRANDSTR_STRLEN || str[0] == '\0') {
		return;
	}

	for (n = 0; n < sizeof (str) / sizeof (uint64_t); n++) {
		uint64_t sv = *(uint64_t *)&str[n * sizeof (uint64_t)];

		wrmsr(MSR_AMD_PROC_NAME_STRING0 + n, sv);
	}
}

apicid_t
milan_thread_apicid(const milan_thread_t *thread)
{
	return (thread->mt_apicid);
}

void
milan_ccx_init(void)
{
}
