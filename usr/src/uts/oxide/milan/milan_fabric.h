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

#ifndef _MILAN_MILAN_FABRIC_H
#define	_MILAN_MILAN_FABRIC_H

/*
 * Definitions that allow us to access the Milan fabric. This consists of the
 * data fabric, northbridges, SMN, and more.
 */

#include <sys/bitext.h>
#include <sys/memlist.h>
#include <sys/plat/pci_prd.h>
#include <sys/types.h>

#include "milan_ccx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is an entry point for early boot that is used after we have PCIe
 * configuration space set up so we can load up all the information about the
 * actual system itself.
 */
extern void milan_fabric_topo_init(void);

/*
 * This is the primary initialization point for the Milan Data Fabric,
 * Northbridges, PCIe, and related.
 */
extern void milan_fabric_init(void);

extern struct memlist *milan_fabric_pci_subsume(uint32_t, pci_prd_rsrc_t);

typedef int (*milan_thread_cb_f)(milan_thread_t *, void *);
extern int milan_fabric_walk_thread(milan_thread_cb_f, void *);
extern milan_thread_t *milan_fabric_find_thread_by_cpuid(uint32_t);
extern size_t milan_fabric_thread_get_brandstr(const milan_thread_t *,
    char *, size_t);

extern uint32_t milan_smupwr_read32(milan_ccd_t *, uint32_t);
extern void milan_smupwr_write32(milan_ccd_t *, uint32_t, uint32_t);

/*
 * In general, each functional block attached to the SMN is allotted its own
 * 20-bit aperture, which effectively means the block has a 12-bit identifier
 * or base as well.  Some subsystems have smaller base addresses because they
 * consume some of the register space for things like device and function ids.
 */
#define	MILAN_SMN_ADDR_BLOCK_BITS	12

#define	MILAN_SMN_ADDR_BASE_PART(_addr, _basebits)	\
	bitx32((_addr), 31, 32 - (_basebits))
#define	MILAN_SMN_ADDR_REG_PART(_addr, _basebits)	\
	bitx32((_addr), 31 - (_basebits), 0)

#define	MILAN_SMN_ASSERT_BASE_ADDR(_smnbase, _basebits)	\
	ASSERT0(MILAN_SMN_ADDR_REG_PART(_smnbase, _basebits))
#define	MILAN_SMN_ASSERT_REG_ADDR(_smnreg, _basebits)	\
	ASSERT0(MILAN_SMN_ADDR_BASE_PART(_smnreg, _basebits))

#define	MILAN_SMN_VERIFY_BASE_ADDR(_smnbase, _basebits)	\
	VERIFY0(MILAN_SMN_ADDR_REG_PART(_smnbase, _basebits))
#define	MILAN_SMN_VERIFY_REG_ADDR(_smnreg, _basebits)	\
	VERIFY0(MILAN_SMN_ADDR_BASE_PART(_smnreg, _basebits))

#define	MILAN_SMN_MAKE_ADDR(_smnbase, _basebits, _smnreg)	\
	(					\
	{					\
		uint32_t _b = (_smnbase);	\
		uint32_t _r = (_smnreg);	\
		uint_t _nbits = (_basebits);	\
		MILAN_SMN_ASSERT_BASE_ADDR(_b, (_nbits));	\
		MILAN_SMN_ASSERT_REG_ADDR(_r, (_nbits));	\
		(_b + _r);			\
	})

/*
 * SMU::PWR registers, per-CCD.  Note that there seems to be a "true base"
 * at 0x300 (+ CCD_SHIFT) but it's not immediately obvious what it is.
 */
#define	MILAN_SMN_SMUPWR_BASE	0x30081000
#define	MILAN_SMN_SMUPWR_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 8)
#define	MILAN_SMN_SMUPWR_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_SMUPWR_BASE_BITS, _r)
#define	MILAN_SMN_SMUPWR_CCD_SHIFT(x)	((x) << 25)

#define	MILAN_SMUPWR_R_SMN_CCD_DIE_ID		0x00000
#define	MILAN_SMUPWR_R_GET_CCD_DIE_ID_DIE_ID(_r)	bitx32(_r, 2, 0)

#define	MILAN_SMUPWR_R_SMN_THREAD_ENABLE	0x00018
#define	MILAN_SMUPWR_R_GET_THREAD_ENABLE_T(_r, _t)	bitx32(_r, _t, _t)
#define	MILAN_SMUPWR_R_SET_THREAD_ENABLE_T(_r, _t)	bitset32(_r, _t, _t, 1)

#define	MILAN_SMUPWR_R_SMN_THREAD_CONFIGURATION	0x0001c
#define	MILAN_SMUPWR_R_GET_THREAD_CONFIGURATION_SMT_MODE(_r)	\
	bitx32(_r, 8, 8)
#define	MILAN_SMUPWR_R_GET_THREAD_CONFIGURATION_COMPLEX_COUNT(_r)	\
	bitx32(_r, 7, 4)
#define	MILAN_SMUPWR_R_GET_THREAD_CONFIGURATION_CORE_COUNT(_r)	\
	bitx32(_r, 3, 0)

#define	MILAN_SMUPWR_R_SMN_SOFT_DOWNCORE	0x00020
#define	MILAN_SMUPWR_R_GET_SOFT_DOWNCORE_DISCORE(_r)	bitx32(_r, 7, 0)
#define	MILAN_SMUPWR_R_GET_SOFT_DOWNCORE_DISCORE_C(_r, _c)	\
	bitx32(_r, _c, _c)
#define	MILAN_SMUPWR_R_SET_SOFT_DOWNCORE_DISCORE(_r, _v)	\
	bitset32(_r, 7, 0, _v)
#define	MILAN_SMUPWR_R_SET_SOFT_DOWNCORE_DISCORE_C(_r, _c)	\
	bitset32(_r, _c, _c, 1)

#define	MILAN_SMUPWR_R_SMN_CORE_ENABLE		0x00024
#define	MILAN_SMUPWR_R_GET_CORE_ENABLE_COREEN(_r)	bitx32(_r, 7, 0)
#define	MILAN_SMUPWR_R_GET_CORE_ENABLE_COREEN_C(_r, _c)	bitx32(_r, _c, _c)
#define	MILAN_SMUPWR_R_SET_CORE_ENABLE_COREEN(_r, _v)	bitset32(_r, 7, 0, _v)
#define	MILAN_SMUPWR_R_SET_CORE_ENABLE_COREEN_C(_r, _c)	bitset32(_r, _c, _c, 1)

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_MILAN_FABRIC_H */
