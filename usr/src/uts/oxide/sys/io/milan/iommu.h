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

#ifndef _SYS_IO_MILAN_IOMMU_H
#define	_SYS_IO_MILAN_IOMMU_H

#include <sys/bitext.h>
#include <sys/io/milan/smn.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IOMMU Registers. The IOMMU is broken into an L1 and L2. The L1 exists for
 * multiple different bases, that is for the IOAGR, NBIF0, and the two PCI
 * ports (even on IOMS 0). XXX We only really include the IOAGR variant here for
 * right now. The L2 register set only exists on a per-IOMS basis.
 */
#define	MILAN_SMN_IOMMUL1_BASE	0x14700000
#define	MILAN_SMN_IOMMUL1_DEV_SHIFT(x)	((x) << 22)
#define	MILAN_SMN_IOMMUL1_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_IOMMUL1_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOMMUL1_BASE_BITS, _r)
#define	MILAN_SMN_IOMMUL2_BASE	0x13f00000
#define	MILAN_SMN_IOMMUL2_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_IOMMUL2_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOMMUL2_BASE_BITS, _r)

/*
 * IOMMU types, note that the PCI port ID is designed to correspond to the first
 * two entries.
 */
typedef enum milan_iommul1_type {
	IOMMU_L1_PCIE0,
	IOMMU_L1_PCIE1,
	IOMMU_L1_NBIF,
	IOMMU_L1_IOAGR
} milan_iommul1_type_t;

/*
 * IOMMU1::L1_MISC_CNTRL_1.  This register contains a smorgasbord of settings.
 * Some of which are used in the hotplug path.
 */
#define	MILAN_IOMMUL1_R_SMN_L1_CTL1	0x1c
#define	MILAN_IOMMUL1_R_SET_L1_CTL1_ORDERING(r, v)	bitset32(r, 0, 0, v)

/*
 * IOMMUL1::L1_SB_LOCATION.  Programs where the FCH is into a given L1 IOMMU.
 */
#define	MILAN_IOMMUL1_R_SMN_SB_LOCATION		0x24

/*
 * IOMMUL2::L2_SB_LOCATION. Yet another place we program the FCH information.
 */
#define	MILAN_IOMMUL2_R_SMN_SB_LOCATION		0x112c

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_IOMMU_H */
