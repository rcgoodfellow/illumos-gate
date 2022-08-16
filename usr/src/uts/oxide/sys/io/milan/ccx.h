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
#include <sys/amdzen/smn.h>

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

extern void milan_ccx_mmio_init(uint64_t, boolean_t);
extern void milan_ccx_physmem_init(void);
extern boolean_t milan_ccx_start_thread(const milan_thread_t *);
extern void milan_ccx_init(void);

/* Walker callback function types */
typedef int (*milan_thread_cb_f)(milan_thread_t *, void *);
typedef int (*milan_ccd_cb_f)(milan_ccd_t *, void *);
typedef int (*milan_ccx_cb_f)(milan_ccx_t *, void *);
typedef int (*milan_core_cb_f)(milan_core_t *, void *);

extern int milan_walk_thread(milan_thread_cb_f, void *);

extern milan_thread_t *milan_fabric_find_thread_by_cpuid(uint32_t);
extern apicid_t milan_thread_apicid(const milan_thread_t *);

extern smn_reg_t milan_core_reg(const milan_core_t *const, const smn_reg_def_t);
extern smn_reg_t milan_ccd_reg(const milan_ccd_t *const, const smn_reg_def_t);
extern uint32_t milan_ccd_read(milan_ccd_t *, const smn_reg_t);
extern void milan_ccd_write(milan_ccd_t *, const smn_reg_t, const uint32_t);
extern uint32_t milan_core_read(milan_core_t *, const smn_reg_t);
extern void milan_core_write(milan_core_t *, const smn_reg_t, const uint32_t);

/*
 * SMU::PWR registers, per-CCD.  Note that there is another aperture at
 * 0x4008_1000 that is documented to alias CCD 0.  It's not really clear what if
 * any utility that's supposed to have, except that the name given to these
 * aliases contains "LOCAL" which implies that perhaps rather than aliasing CCD
 * 0 it instead is decoded by the unit on the originating CCD.  We don't use
 * that in any case.
 */
AMDZEN_MAKE_SMN_REG_FN(milan_smupwr_smn_reg, SMUPWR, 0x30081000,
    0xfffff000, 8, 25);

/*
 * SMU::PWR::CCD_DIE_ID - does what it says.
 */
/*CSTYLED*/
#define	D_SMUPWR_CCD_DIE_ID	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SMUPWR,			\
	.srd_reg = 0x00					\
}
#define	SMUPWR_CCD_DIE_ID(c)	\
    milan_smupwr_smn_reg(c, D_SMUPWR_CCD_DIE_ID, 0)
#define	SMUPWR_CCD_DIE_ID_GET(_r)	bitx32(_r, 2, 0)

/*
 * SMU::PWR::THREAD_ENABLE - also does what it says; this is a bitmap of each of
 * the 16 possible threads.  If the bit is set, the thread runs.  Clearing bits
 * is not allowed.
 */
/*CSTYLED*/
#define	D_SMUPWR_THREAD_EN	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SMUPWR,			\
	.srd_reg = 0x18					\
}
#define	SMUPWR_THREAD_EN(c)	\
    milan_smupwr_smn_reg(c, D_SMUPWR_THREAD_EN, 0)
#define	SMUPWR_THREAD_EN_GET_T(_r, _t)	bitx32(_r, _t, _t)
#define	SMUPWR_THREAD_EN_SET_T(_r, _t)	bitset32(_r, _t, _t, 1)

/*
 * SMU::PWR::THREAD_CONFIGURATION - provides core and CCX counts for the die as
 * well as whether SMT is enabled, and a bit to enable or disable SMT *after the
 * next warm reset* (which we don't use).
 */
/*CSTYLED*/
#define	D_SMUPWR_THREAD_CFG	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SMUPWR,			\
	.srd_reg = 0x1c					\
}
#define	SMUPWR_THREAD_CFG(c)	\
    milan_smupwr_smn_reg(c, D_SMUPWR_THREAD_CFG, 0)
#define	SMUPWR_THREAD_CFG_GET_SMT_MODE(_r)	bitx32(_r, 8, 8)
#define	SMUPWR_THREAD_CFG_GET_COMPLEX_COUNT(_r)	bitx32(_r, 7, 4)
#define	SMUPWR_THREAD_CFG_GET_CORE_COUNT(_r)	bitx32(_r, 3, 0)

/*
 * SMU::PWR::SOFT_DOWNCORE - provides a bitmap of cores that may exist; setting
 * each bit disables the corresponding core.  Presumably after a warm reset.
 */
/*CSTYLED*/
#define	D_SMUPWR_SOFT_DOWNCORE	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SMUPWR,			\
	.srd_reg = 0x20					\
}
#define	SMUPWR_SOFT_DOWNCORE(c)	\
    milan_smupwr_smn_reg(c, D_SMUPWR_SOFT_DOWNCORE, 0)
#define	SMUPWR_SOFT_DOWNCORE_GET_DISCORE(_r)		bitx32(_r, 7, 0)
#define	SMUPWR_SOFT_DOWNCORE_GET_DISCORE_C(_r, _c)	bitx32(_r, _c, _c)
#define	SMUPWR_SOFT_DOWNCORE_SET_DISCORE(_r, _v)	bitset32(_r, 7, 0, _v)
#define	SMUPWR_SOFT_DOWNCORE_SET_DISCORE_C(_r, _c)	bitset32(_r, _c, _c, 1)

/*
 * SMU::PWR::CORE_ENABLE - nominally writable, this register contains a bitmap
 * of cores; a bit that is set means the core whose physical ID is that bit
 * position is enabled.  The effect of modifying this register, if any, is
 * undocumented and unknown.
 */
/*CSTYLED*/
#define	D_SMUPWR_CORE_EN	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SMUPWR,			\
	.srd_reg = 0x24					\
}
#define	SMUPWR_CORE_EN(c)	\
    milan_smupwr_smn_reg(c, D_SMUPWR_CORE_EN, 0)
#define	SMUPWR_CORE_EN_GET(_r)		bitx32(_r, 7, 0)
#define	SMUPWR_CORE_EN_GET_C(_r, _c)	bitx32(_r, _c, _c)
#define	SMUPWR_CORE_EN_SET(_r, _v)	bitset32(_r, 7, 0, _v)
#define	SMUPWR_CORE_EN_SET_C(_r, _c)	bitset32(_r, _c, _c, 1)

/*
 * SCFCTP has one functional unit per CCD.  It appears that all registers have
 * an instance per supported core, with the size of each core's block 0x2_0000.
 */
AMDZEN_MAKE_SMN_REG_FN(milan_scfctp_smn_reg, SCFCTP, 0x20000000,
    SMN_APERTURE_MASK, 8, 23);
#define	SCFCTP_CORE_STRIDE	0x20000

/*
 * L3::SCFCTP::PMREG_INITPKG0 - Nominally writable, this register contains
 * information allowing us to discover where this core fits into the logical and
 * physical topology of the processor.
 */
/*CSTYLED*/
#define	D_SCFCTP_PMREG_INITPKG0	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SCFCTP,			\
	.srd_reg = 0x2fd0,				\
	.srd_nents = 8,					\
	.srd_stride = SCFCTP_CORE_STRIDE		\
}
#define	SCFCTP_PMREG_INITPKG0(d, c)	\
    milan_scfctp_smn_reg(d, D_SCFCTP_PMREG_INITPKG0, c)
#define	SCFCTP_PMREG_INITPKG0_GET_LOG_DIE(_r)	bitx32(_r, 22, 19)
#define	SCFCTP_PMREG_INITPKG0_GET_LOG_CCX(_r)	bitx32(_r, 18, 18)
#define	SCFCTP_PMREG_INITPKG0_GET_LOG_CORE(_r)	bitx32(_r, 17, 14)
#define	SCFCTP_PMREG_INITPKG0_GET_SOCKET(_r)	bitx32(_r, 13, 12)
#define	SCFCTP_PMREG_INITPKG0_GET_PHYS_DIE(_r)	bitx32(_r, 11, 8)
#define	SCFCTP_PMREG_INITPKG0_GET_PHYS_CCX(_r)	bitx32(_r, 7, 7)
#define	SCFCTP_PMREG_INITPKG0_GET_PHYS_CORE(_r)	bitx32(_r, 6, 3)
#define	SCFCTP_PMREG_INITPKG0_GET_SMTEN(_r)	bitx32(_r, 2, 0)

/*
 * L3::SCFCTP::PMREG_INITPKG7 - Similarly, this register describes this
 * processor's overall internal core topology.
 */
/*CSTYLED*/
#define	D_SCFCTP_PMREG_INITPKG7	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_SCFCTP,			\
	.srd_reg = 0x2fec,				\
	.srd_nents = 8,					\
	.srd_stride = SCFCTP_CORE_STRIDE		\
}
#define	SCFCTP_PMREG_INITPKG7(d, c)	\
    milan_scfctp_smn_reg(d, D_SCFCTP_PMREG_INITPKG7, c)
#define	SCFCTP_PMREG_INITPKG7_GET_N_SOCKETS(_r)		bitx32(_r, 26, 25)
#define	SCFCTP_PMREG_INITPKG7_GET_N_DIES(_r)		bitx32(_r, 24, 21)
#define	SCFCTP_PMREG_INITPKG7_GET_N_CCXS(_r)		bitx32(_r, 20, 20)
#define	SCFCTP_PMREG_INITPKG7_GET_N_CORES(_r)		bitx32(_r, 19, 16)
#define	SCFCTP_PMREG_INITPKG7_GET_CHIDXHASHEN(_r)	bitx32(_r, 10, 10)
#define	SCFCTP_PMREG_INITPKG7_GET_S3(_r)		bitx32(_r, 9, 9)
#define	SCFCTP_PMREG_INITPKG7_GET_S0I3(_r)		bitx32(_r, 8, 8)
#define	SCFCTP_PMREG_INITPKG7_GET_CORETYPEISARM(_r)	bitx32(_r, 7, 7)
#define	SCFCTP_PMREG_INITPKG7_GET_SOCID(_r)		bitx32(_r, 6, 3)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_CCX_H */
