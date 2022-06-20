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

#ifndef _SYS_IO_MILAN_IOAPIC_H
#define	_SYS_IO_MILAN_IOAPIC_H

/*
 * NB IOAPIC register definitions.  While the NBIOAPICs are very similar to the
 * traditional IOAPIC interface, the latter is found in the FCH.  These IOAPICs
 * are not normally programmed beyond initial setup and handle legacy interrupts
 * coming from PCIe and NBIF sources.  Such interrupts, which are not supported
 * on this machine architecture, are then routed to the FCH IOAPIC.
 */

#include <sys/bitext.h>
#include <sys/io/milan/smn.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IOAPIC registers. These exist on a per-IOMS basis. These are not the
 * traditional software IOAPIC registers that exist in the Northbridge.
 */
#define	MILAN_SMN_IOAPIC_BASE	0x14300000
#define	MILAN_SMN_IOAPIC_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_IOAPIC_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOAPIC_BASE_BITS, _r)

/*
 * IOAPIC::FEATURES_ENABLE. This controls various features of the IOAPIC.
 */
#define	MILAN_IOAPIC_R_SMN_FEATURES		0x00
#define	MILAN_IOAPIC_R_SET_FEATURES_LEVEL_ONLY(r, v)	bitset32(r, 9, 9, v)
#define	MILAN_IOAPIC_R_SET_FEATURES_PROC_MODE(r, v)	bitset32(r, 8, 8, v)
#define	MILAN_IOAPIC_R_SET_FEATURES_SECONDARY(r, v)	bitset32(r, 5, 5, v)
#define	MILAN_IOAPIC_R_SET_FEATURES_FCH(r, v)		bitset32(r, 4, 4, v)
#define	MILAN_IOAPIC_R_SET_FEATURES_ID_EXT(r, v)	bitset32(r, 2, 2, v)
#define	MILAN_IOAPIC_R_FEATURES_ID_EXT_4BIT	0
#define	MILAN_IOAPIC_R_FEATURES_ID_EXT_8BIT	1

/*
 * IOAPIC::IOAPIC_BR_INTERRUPT_ROUTING. There are several instances of this
 * register and they determine how a given logical bridge on the IOMS maps to
 * the IOAPIC pins. Hence why there are 22 routes.
 */
#define	MILAN_IOAPIC_R_NROUTES			22
#define	MILAN_IOAPIC_R_SMN_ROUTE		0x40
#define	MILAN_IOAPIC_R_SET_ROUTE_BRIDGE_MAP(r, v)	bitset32(r, 20, 16, v)
#define	MILAN_IOAPIC_R_SET_ROUTE_INTX_SWIZZLE(r, v)	bitset32(r, 5, 4, v)
#define	MILAN_IOAPIC_R_ROUTE_INTX_SWIZZLE_ABCD		0
#define	MILAN_IOAPIC_R_ROUTE_INTX_SWIZZLE_BCDA		1
#define	MILAN_IOAPIC_R_ROUTE_INTX_SWIZZLE_CDAB		2
#define	MILAN_IOAPIC_R_ROUTE_INTX_SWIZZLE_DABC		3
#define	MILAN_IOAPIC_R_SET_ROUTE_INTX_GROUP(r, v)	bitset32(r, 2, 0, v)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_IOAPIC_H */
