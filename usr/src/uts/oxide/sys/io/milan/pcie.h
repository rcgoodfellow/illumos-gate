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

#ifndef _SYS_IO_MILAN_PCIE_H
#define	_SYS_IO_MILAN_PCIE_H

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
struct milan_pcie_bridge;
struct milan_pcie_port;

typedef struct milan_pcie_bridge milan_pcie_bridge_t;
typedef struct milan_pcie_port milan_pcie_port_t;

typedef int (*milan_pcie_port_cb_f)(milan_pcie_port_t *, void *);
typedef int (*milan_bridge_cb_f)(milan_pcie_bridge_t *, void *);

/*
 * PCIe related SMN addresses. This is determined based on a combination of
 * which IOMS we're on, which PCIe port we're on on the IOMS, and then finally
 * which PCIe port it is itself. There are two SMN bases. One for internal
 * configuration and one where the common configuration space exists.
 *
 * Registers in this space are sometimes specific to an overall port (e.g. the
 * thing that encompasses a given group of root bridges and an x16 port) or to a
 * bridge within the port.
 *
 * The use of bits [19:18] to represent the sub-block and [15:12] to represent
 * the bridge offset means that the effective base SMN address for per-port core
 * registers occupies 14 bits, and for the per-bridge port and config registers
 * occupies 20 bits.
 */
#define	MILAN_SMN_PCIE_CFG_BASE		0x11100000
#define	MILAN_SMN_PCIE_PORT_BASE	0x11140000
#define	MILAN_SMN_PCIE_CORE_BASE	0x11180000
#define	MILAN_SMN_PCIE_BRIDGE_SHIFT(x)	((x) << 12)
#define	MILAN_SMN_PCIE_PORT_SHIFT(x)	((x) << 22)
#define	MILAN_SMN_PCIE_IOMS_SHIFT(x)	((x) << 20)
#define	MILAN_SMN_PCIE_CORE_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 2)
#define	MILAN_SMN_PCIE_PORT_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 8)
#define	MILAN_SMN_PCIE_CORE_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_PCIE_CORE_BASE_BITS, _r)
#define	MILAN_SMN_PCIE_PORT_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_PCIE_PORT_BASE_BITS, _r)

/*
 * General PCIe port controls. This is a register that exists in 'Port Space'
 * and is specific to a bridge.
 */
#define	MILAN_PCIE_PORT_R_SMN_PORT_CNTL	0x40
#define	MILAN_PCIE_PORT_R_SET_PORT_CNTL_PWRFLT_EN(r, v)		\
    bitset32(r, 4, 4, v)

/*
 * PCIe TX Control. This is a register that exists in 'Port Space' and is
 * specific to a bridge. XXX figure out what other bits we need.
 */
#define	MILAN_PCIE_PORT_R_SMN_TX_CNTL	0x80
#define	MILAN_PCIE_PORT_R_SET_TX_CNTL_TLP_FLUSH_DOWN_DIS(r, v)	\
    bitset32(r, 15, 15, v)

/*
 * Port Link Training Control. This register seems to control some amount of the
 * general aspects of link training.
 */
#define	MILAN_PCIE_PORT_R_SMN_TRAIN_CNTL	0x284
#define	MILAN_PCIE_PORT_R_SET_TRAIN_CNTL_TRAIN_DIS(r, v)	\
    bitset32(r, 13, 13, v)

/*
 * Port Link Control Register 5. There are several others, but this one seems to
 * be require for hotplug.
 */
#define	MILAN_PCIE_PORT_R_SMN_LC_CNTL5	0x2dc
#define	MILAN_PCIE_PORT_R_SET_LC_CNTL5_WAIT_DETECT(r, v)	\
    bitset32(r, 28, 28, v)

/*
 * Port Hotplug Descriptor control. This is a register that exists in 'Port
 * Space' and is specific to a bridge. This seems to relate something in the
 * port to the SMU's hotplug engine.
 */
#define	MILAN_PCIE_PORT_R_SMN_HP_CNTL	0x36c
#define	MILAN_PCIE_PORT_R_SET_HP_CNTL_SLOT(r, v)	bitset32(r, 5, 0, v)
#define	MILAN_PCIE_PORT_R_SET_HP_CNTL_ACTIVE(r, v)	bitset32(r, 31, 31, v)

/*
 * PCIe Port level TX controls. Note, this register is in 'core' space and is
 * specific to the overall milan_pcie_port_t, as opposed to the bridge. XXX Add
 * in other bits.
 */
#define	MILAN_PCIE_CORE_R_SMN_CI_CNTL	0x80
#define	MILAN_PCIE_CORE_R_SET_CI_CNTL_LINK_DOWN_CTO_EN(r, v)		\
    bitset32(r, 29, 29, v)
#define	MILAN_PCIE_CORE_R_SET_CI_CNTL_IGN_LINK_DOWN_CTO_ERR(r, v)	\
    bitset32(r, 31, 31, v)

/*
 * PCIe port SDP Control. This register seems to be used to tell the system how
 * to map a given port to the data fabric and related.
 */
#define	MILAN_PCIE_CORE_R_SMN_SDP_CTRL	0x18c
#define	MILAN_PCIE_CORE_R_SET_SDP_CTRL_PORT_ID(r, v)	bitset32(r, 28, 26, v)
#define	MILAN_PCIE_CORE_R_SET_SDP_CTRL_UNIT_ID(r, v)	bitset32(r, 3, 0, v)

/*
 * PCIe Software Reset Control #6. This is in 'Core Space' and controls whether
 * or not all of a given set of ports are stopped from training.
 */
#define	MILAN_PCIE_CORE_R_SMN_SWRST_CNTL6	0x428
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_A(r, v)	bitset32(r, 0, 0, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_B(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_C(r, v)	bitset32(r, 2, 2, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_D(r, v)	bitset32(r, 3, 3, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_E(r, v)	bitset32(r, 4, 4, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_F(r, v)	bitset32(r, 5, 5, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_G(r, v)	bitset32(r, 6, 6, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_H(r, v)	bitset32(r, 7, 7, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_I(r, v)	bitset32(r, 8, 8, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_J(r, v)	bitset32(r, 9, 9, v)
#define	MILAN_PCIE_CORE_R_SET_SWRST_CNTL6_HOLD_K(r, v)	bitset32(r, 10, 10, v)

/*
 * PCIe Presence Detect Control. This is 'Core Space', so it exists once
 * per port. This is used to determine whether we should consider something
 * present based on the link up OR the side-band signals, or instead require
 * both (e.g. AND).
 */
#define	MILAN_PCIE_CORE_R_SMN_PRES	0x4e0
#define	MILAN_PCIE_CORE_R_SET_PRES_MODE(r, v)	bitset32(r, 24, 24, v)
#define	MILAN_PCIE_CORE_R_PRES_MODE_OR	0
#define	MILAN_PCIE_CORE_R_PRES_MODE_AND	1

/*
 * The following definitions are all in normal PCI configuration space. These
 * represent the fixed offsets into capabilities that normally would be
 * something that one has to walk and find in the device. We opt to use the
 * fixed offsets here because we only care about one specific device, the
 * bridges here. Note, the actual bit definitions are not included here as they
 * are already present in sys/pcie.h.
 */

/*
 * PCIERCCFG::PCIE_CAP. This is the core PCIe capability register offset. This
 * is related to the PCIE_PCIECAP, but already adjusted for the fixed capability
 * offset.
 */
#define	MILAN_BRIDGE_R_PCI_PCIE_CAP	0x5a

/*
 * PCIERCCFG::SLOT_CAP. This is the PCIe capability's slot capability register.
 * This is the illumos PCIE_SLOTCAP, but already adjusted for the capability
 * offset.
 */
#define	MILAN_BRIDGE_R_PCI_SLOT_CAP	0x6c

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_PCIE_H */
