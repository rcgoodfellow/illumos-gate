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

#ifndef _SYS_IO_MILAN_NBIF_H
#define	_SYS_IO_MILAN_NBIF_H

/*
 * Milan-specific register and bookkeeping definitions for PCIe root complexes,
 * ports, and bridges.
 */

#include <sys/bitext.h>
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
struct milan_nbif;

typedef struct milan_nbif milan_nbif_t;

/*
 * Function callback signatures for making operating on a given unit simpler.
 */
typedef int (*milan_nbif_cb_f)(milan_nbif_t *, void *);

/*
 * nBIF SMN Addresses. These have multiple different shifts that we need to
 * account for. There are different bases based on which IOMS, which NBIF, and
 * which downstream device and function as well. There is a second SMN aperture
 * ID that seems to be used tha deals with the nBIF's clock gating, DMA
 * enhancements with the syshub, and related.
 */
#define	MILAN_SMN_NBIF_BASE		0x10100000
#define	MILAN_SMN_NBIF_FUNC_OFF		0x34000
#define	MILAN_SMN_NBIF_ALT_BASE		0x01400000
#define	MILAN_SMN_NBIF_FUNC_SHIFT(x)	((x) << 9)
#define	MILAN_SMN_NBIF_DEV_SHIFT(x)	((x) << 12)
#define	MILAN_SMN_NBIF_NBIF_SHIFT(x)	((x) << 22)
#define	MILAN_SMN_NBIF_IOMS_SHIFT(x)	((x) << 20)
#define	MILAN_SMN_NBIF_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_NBIF_ALT_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_NBIF_FUNC_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 11)
#define	MILAN_SMN_NBIF_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_NBIF_BASE_BITS, _r)
#define	MILAN_SMN_NBIF_ALT_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_NBIF_ALT_BASE_BITS, _r)
#define	MILAN_SMN_NBIF_FUNC_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_NBIF_FUNC_BASE_BITS, _r)

/*
 * The NBIF device straps for the port use a different shift style than those
 * above which are for the function space.
 */
#define	MILAN_SMN_NBIF_DEV_PORT_SHIFT(x)	((x) << 9)

/*
 * nBIF related registers.
 */

/*
 * NBIF Function strap 0. This SMN address is relative to the actual function
 * space.
 */
#define	MILAN_NBIF_R_SMN_FUNC_STRAP0	0x00
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_SUP_D2(r, v)	bitset32(r, 31, 31, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_SUP_D1(r, v)	bitset32(r, 30, 30, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_BE_PCIE(r, v)	bitset32(r, 29, 29, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_EXIST(r, v)	bitset32(r, 28, 28, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_GFX_REV(r, v)	bitset32(r, 27, 24, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_MIN_REV(r, v)	bitset32(r, 23, 20, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_MAJ_REV(r, v)	bitset32(r, 19, 16, v)
#define	MILAN_NBIF_R_SET_FUNC_STRAP0_DEV_ID(r, v)	bitset32(r, 0, 15, v)

/*
 * This register is arranged with one byte per device. Each bit corresponds to
 * an endpoint.
 */
#define	MILAN_NBIF_R_SMN_INTR_LINE	0x3a008
#define	MILAN_NBIF_R_INTR_LINE_SET_INTR(reg, dev, func, val)	\
    bitset32(reg, ((dev) * 8) + (func), ((dev) * 8) + (func), val)

/*
 * Straps for the NBIF port. These are relative to the main NBIF base register.
 */
#define	MILAN_NBIF_R_SMN_PORT_STRAP3	0x3100c
#define	MILAN_NBIF_R_SET_PORT_STRAP3_COMP_TO(r, v)	bitset32(r, 7, 7, v)


/*
 * This register seems to control various bits of control for a given NBIF. XXX
 * other bits.
 */
#define	MILAN_NBIF_R_SMN_BIFC_MISC_CTRL0	0x3a010
#define	MILAN_NBIF_R_SET_BIFC_MISC_CTRL0_PME_TURNOFF(r, v)	\
    bitset32(r, 28, 28, v)
#define	MILAN_NBIF_R_BIFC_MISC_CTRL0_PME_TURNOFF_BYPASS		0
#define	MILAN_NBIF_R_BIFC_MISC_CTRL0_PME_TURNOFF_FW		1

/*
 * The following two registers are not found in the PPR. These are used for some
 * amount of arbitration in the same vein as the SION values. The base register
 * seemingly just has a bit which defaults to saying use these values.
 */
#define	MILAN_NBIF_R_SMN_GMI_WRR_WEIGHT2	0x3a124
#define	MILAN_NBIF_R_SMN_GMI_WRR_WEIGHT3	0x3a128
#define	MILAN_NBIF_R_GMI_WRR_WEIGHT_VAL		0x04040404

/*
 * This undocumented register is a weird SYSHUB and NBIF crossover that is in
 * the alternate space.
 */
#define	MILAN_NBIF_R_SMN_SYSHUB_BGEN_BYPASS	0x10008
#define	MILAN_NBIF_R_SET_SYSHUB_BGEN_BYPASS_DMA_SW0(r, v)	\
    bitset32(r, 16, 16, v)
#define	MILAN_NBIF_R_SET_SYSHUB_BGEN_BYPASS_DMA_SW1(r, v)	\
    bitset32(r, 17, 17, v)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_NBIF_H */
