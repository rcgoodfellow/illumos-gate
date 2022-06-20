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

#ifndef _SYS_IO_MILAN_CCX_H
#define	_SYS_IO_MILAN_CCX_H

/*
 * Structure and register definitions for the resources contained on the
 * core-complex dies (CCDs), including the core complexes (CCXs) themselves and
 * the cores and constituent compute threads they contain.
 */

#include <sys/apic.h>
#include <sys/bitext.h>
#include <sys/stdint.h>
#include <sys/types.h>
#include <sys/io/milan/smn.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The implementation of these types is exposed to implementers but not to
 * consumers; therefore we forward-declare them here and provide the actual
 * definitions only in the corresponding *_impl.h.  Consumers are allowed to use
 * pointers to these types only as opaque handles.
 */
struct milan_thread;
struct milan_core;
struct milan_ccx;
struct milan_ccd;

typedef struct milan_thread milan_thread_t;
typedef struct milan_core milan_core_t;
typedef struct milan_ccx milan_ccx_t;
typedef struct milan_ccd milan_ccd_t;

extern uint32_t milan_ccd_smupwr_read32(milan_ccd_t *, uint32_t);
extern void milan_ccd_smupwr_write32(milan_ccd_t *, uint32_t, uint32_t);

extern void milan_ccx_mmio_init(uint64_t, boolean_t);
extern void milan_ccx_physmem_init(void);
extern boolean_t milan_ccx_start_thread(const milan_thread_t *);
extern void milan_ccx_set_brandstr(void);

/* Walker callback function types */
typedef int (*milan_thread_cb_f)(milan_thread_t *, void *);
typedef int (*milan_ccd_cb_f)(milan_ccd_t *, void *);
typedef int (*milan_ccx_cb_f)(milan_ccx_t *, void *);
typedef int (*milan_core_cb_f)(milan_core_t *, void *);

extern int milan_walk_thread(milan_thread_cb_f, void *);

extern milan_thread_t *milan_fabric_find_thread_by_cpuid(uint32_t);
extern apicid_t milan_thread_apicid(const milan_thread_t *);

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

#define	MILAN_SMN_SCFCTP_BASE	0x20000000
#define	MILAN_SMN_SCFCTP_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 3)
#define	MILAN_SMN_SCFCTP_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_SCFCTP_BASE_BITS, _r)
#define	MILAN_SMN_SCFCTP_CCD_SHIFT(_d)	((_d) << 23)
#define	MILAN_SMN_SCFCTP_CORE_SHIFT(_c)	((_c) << 17)

#define	MILAN_SCFCTP_R_SMN_PMREG_INITPKG0	0x2FD0
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_LOGICALDIEID(_r)	\
	bitx32(_r, 22, 19)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_LOGICALCOMPLEXID(_r)	\
	bitx32(_r, 18, 18)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_LOGICALCOREID(_r)	\
	bitx32(_r, 17, 14)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_SOCKETID(_r)	bitx32(_r, 13, 12)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_PHYSICALDIEID(_r)	\
	bitx32(_r, 11, 8)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_PHYSICALCOMPLEXID(_r)	\
	bitx32(_r, 7, 7)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_PHYSICALCOREID(_r)	\
	bitx32(_r, 6, 3)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG0_SMTEN(_r)	bitx32(_r, 2, 0)

#define	MILAN_SCFCTP_R_SMN_PMREG_INITPKG7	0x2FEC
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_NUMOFSOCKETS(_r)	\
	bitx32(_r, 26, 25)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_NUMOFLOGICALDIE(_r)	\
	bitx32(_r, 24, 21)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_NUMOFLOGICALCOMPLEXES(_r)	\
	bitx32(_r, 20, 20)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_NUMOFLOGICALCORES(_r)	\
	bitx32(_r, 19, 16)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_CHIDXHASHEN(_r)	\
	bitx32(_r, 10, 10)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_S3(_r)	bitx32(_r, 9, 9)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_S0I3(_r)	bitx32(_r, 8, 8)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_CORETYPEISARM(_r)	\
	bitx32(_r, 7, 7)
#define	MILAN_SCFCTP_R_GET_PMREG_INITPKG7_SOCID(_r)	bitx32(_r, 6, 3)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_CCX_H */
