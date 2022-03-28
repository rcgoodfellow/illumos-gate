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

#ifndef _MILAN_MILAN_CCX_H
#define	_MILAN_MILAN_CCX_H

/*
 * Misc. functions that are required to initialize the Milan core complexes.
 */

#include <sys/types.h>
#include <sys/stdint.h>
#include <sys/apic.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum Zen cores/thread parameters for Milan.  Naples and Rome each have
 * up to 4 cores per CCX and 2 CCXs per CCD; Naples always has 1 CCD per
 * IO die as they were colocated.  Supporting Rome or other old processor
 * packages requires generalising these parameters.  CCX == L3.
 *
 * Namespaces
 *
 * Each CCD, CCX, and core shares two distinct integer namespaces with its
 * siblings: a compact logical one and a possibly sparse physical one.  These
 * names are unique among siblings but not across e.g. cousins.  Both names are
 * provided to us for each object by the DF and APOB, and which name is used
 * to compute a register or bit address varies from one register to the next.
 * Therefore we need, and keep, both of them.  The logical name should always
 * correspond to the index into the parent's array.
 *
 * Threads are different: each core has some number of threads which in current
 * implementations is either 1 or 2.  There is no separate physical thread
 * identifier as there is no way for some discontiguous subset of threads to
 * exist.  Therefore each thread has but a single logical identifier, also its
 * index within its parent core's array of them.  However, the thread also has
 * an APIC ID, which unlike the other identifiers is globally unique across the
 * entire fabric.  The APIC ID namespace is sparse when any of a thread's
 * containing entities is one of a collection of siblings whose number is not
 * a power of 2.
 *
 * One last note on APIC IDs: while we compute the APIC ID that is assigned to
 * each thread by firmware prior to boot, that ID can be changed by writing to
 * the thread's APIC ID MSR (or, in xAPIC mode which we never use, the
 * analogous MMIO register).  The one we compute and store here is the one
 * set by firmware before boot.
 */
#define	MILAN_MAX_CCDS_PER_IODIE	8
#define	MILAN_MAX_CCXS_PER_CCD		1
#define	MILAN_MAX_CORES_PER_CCX		8
#define	MILAN_MAX_THREADS_PER_CORE	2

struct milan_iodie;

typedef struct milan_thread {
	uint8_t			mt_threadno;
	apicid_t		mt_apicid;
	struct milan_core	*mt_core;
} milan_thread_t;

typedef struct milan_core {
	uint8_t			mc_logical_coreno;
	uint8_t			mc_physical_coreno;
	uint8_t			mc_nthreads;
	uint32_t		mc_scfctp_smn_base;
	milan_thread_t		mc_threads[MILAN_MAX_THREADS_PER_CORE];
	struct milan_ccx	*mc_ccx;
} milan_core_t;

typedef struct milan_ccx {
	uint8_t			mcx_logical_cxno;
	uint8_t			mcx_physical_cxno;
	uint8_t			mcx_ncores;
	uint32_t		mcx_scfctp_smn_base;
	milan_core_t		mcx_cores[MILAN_MAX_CORES_PER_CCX];
	struct milan_ccd	*mcx_ccd;
} milan_ccx_t;

typedef struct milan_ccd {
	uint8_t			mcd_logical_dieno;
	uint8_t			mcd_physical_dieno;
	uint8_t			mcd_ccm_fabric_id;
	uint8_t			mcd_ccm_comp_id;
	uint32_t		mcd_smupwr_smn_base;
	uint8_t			mcd_nccxs;
	milan_ccx_t		mcd_ccxs[MILAN_MAX_CCXS_PER_CCD];
	struct milan_iodie	*mcd_iodie;
} milan_ccd_t;

extern void milan_ccx_mmio_init(uint64_t, boolean_t);
extern void milan_ccx_physmem_init(void);
extern boolean_t milan_ccx_start_thread(const milan_thread_t *);
extern milan_thread_t *milan_ccx_thread_self(void);
extern void milan_ccx_set_brandstr(void);

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_MILAN_CCX_H */
