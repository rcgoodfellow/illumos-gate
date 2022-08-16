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
struct milan_pcie_bridge;
struct milan_pcie_port;

typedef struct milan_pcie_bridge milan_pcie_bridge_t;
typedef struct milan_pcie_port milan_pcie_port_t;

typedef int (*milan_pcie_port_cb_f)(milan_pcie_port_t *, void *);
typedef int (*milan_bridge_cb_f)(milan_pcie_bridge_t *, void *);

extern uint8_t milan_nbio_n_pcie_ports(const uint8_t);
extern uint8_t milan_pcie_port_n_bridges(const uint8_t);

/*
 * PCIe related SMN addresses. This is determined based on a combination of
 * which IOMS we're on, which PCIe port we're on on the IOMS, and then finally
 * which PCIe bridge it is itself. We have broken this up into two separate
 * sub-units, one for per-port registers (the "core space") and one for
 * per-bridge registers ("port space").  There is a third sub-unit we don't
 * currently use where the common configuration space exists.
 *
 * The location of registers in each space is somewhat unusual; we've chosen to
 * model this so that in each unit the number of register (and sub-unit)
 * instances is fixed for a given sub-unit (unit). There are two reasons for
 * this: first, the number of register (sub-unit) instances varies depending on
 * the sub-unit (unit) instance number; second, the ioms and port instance
 * numbers are both used to compute the aperture base address.  To simplify our
 * implementation, we consider the bridge instance number to also form part of
 * the aperture base rather than treating the size of each port space as the
 * per-bridge register stride.  The upshot of this is that we ignore srd_nents
 * and srd_stride (more pointedly: they must not be set); similarly, all these
 * registers are 32 bits wide, so srd_size must be 0.
 */

static inline smn_reg_t
milan_pcie_core_smn_reg(const uint8_t iomsno, const smn_reg_def_t def,
    const uint8_t portno)
{
	const uint32_t PCIE_CORE_SMN_REG_MASK = 0x7ffff;
	const uint32_t ioms32 = (const uint32_t)iomsno;
	const uint32_t port32 = (const uint32_t)portno;

	ASSERT0(def.srd_size);
	ASSERT3S(def.srd_unit, ==, SMN_UNIT_PCIE_CORE);
	ASSERT0(def.srd_nents);
	ASSERT0(def.srd_stride);
	ASSERT3U(ioms32, <, 4);
	ASSERT0(def.srd_reg & ~PCIE_CORE_SMN_REG_MASK);

#ifdef	DEBUG
	const uint32_t nents = milan_nbio_n_pcie_ports(iomsno);
	ASSERT3U(nents, >, port32);
#endif	/* DEBUG */

	const uint32_t aperture_base = 0x11180000;

	const uint32_t aperture_off = (ioms32 << 20) + (port32 << 22);
	ASSERT3U(aperture_off, <=, UINT32_MAX - aperture_base);

	const uint32_t aperture = aperture_base + aperture_off;
	ASSERT0(aperture & PCIE_CORE_SMN_REG_MASK);

	return (SMN_MAKE_REG(aperture + def.srd_reg));
}

static inline smn_reg_t
milan_pcie_port_smn_reg(const uint8_t iomsno, const smn_reg_def_t def,
    const uint8_t portno, const uint8_t bridgeno)
{
	const uint32_t PCIE_PORT_SMN_REG_MASK = 0xfff;
	const uint32_t ioms32 = (const uint32_t)iomsno;
	const uint32_t port32 = (const uint32_t)portno;
	const uint32_t bridge32 = (const uint32_t)bridgeno;

	ASSERT0(def.srd_size);
	ASSERT3S(def.srd_unit, ==, SMN_UNIT_PCIE_PORT);
	ASSERT0(def.srd_nents);
	ASSERT0(def.srd_stride);
	ASSERT3U(ioms32, <, 4);
	ASSERT0(def.srd_reg & ~PCIE_PORT_SMN_REG_MASK);

#ifdef	DEBUG
	const uint32_t nports = (const uint32_t)milan_nbio_n_pcie_ports(iomsno);
	ASSERT3U(nports, >, port32);
	const uint32_t nents =
	    (const uint32_t)milan_pcie_port_n_bridges(portno);
	ASSERT3U(nents, >, bridge32);
#endif	/* DEBUG */

	const uint32_t aperture_base = 0x11140000;

	const uint32_t aperture_off = (ioms32 << 20) + (port32 << 22) +
	    (bridge32 << 12);
	ASSERT3U(aperture_off, <=, UINT32_MAX - aperture_base);

	const uint32_t aperture = aperture_base + aperture_off;
	ASSERT0(aperture & PCIE_PORT_SMN_REG_MASK);

	return (SMN_MAKE_REG(aperture + def.srd_reg));
}

/*
 * PCIEPORT::PCIEP_PORT_CNTL - General PCIe port controls. This is a register
 * that exists in 'Port Space' and is specific to a bridge.
 */
/*CSTYLED*/
#define	D_PCIE_PORT_PCTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_PORT,	\
	.srd_reg = 0x40	\
}
#define	PCIE_PORT_PCTL(n, p, b)	\
    milan_pcie_port_smn_reg(n, D_PCIE_PORT_PCTL, p, b)
#define	PCIE_PORT_PCTL_SET_PWRFLT_EN(r, v)	bitset32(r, 4, 4, v)

/*
 * PCIEPORT::PCIE_TX_CNTL - PCIe TX Control. This is a register that exists in
 * 'Port Space' and is specific to a bridge. XXX figure out what other bits we
 * need.
 */
/*CSTYLED*/
#define	D_PCIE_PORT_TX_CTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_PORT,	\
	.srd_reg = 0x80	\
}
#define	PCIE_PORT_TX_CTL(n, p, b)	\
    milan_pcie_port_smn_reg((n), D_PCIE_PORT_TX_CTL, (p), (b))
#define	PCIE_PORT_TX_CTL_SET_TLP_FLUSH_DOWN_DIS(r, v)	bitset32(r, 15, 15, v)

/*
 * PCIEPORT::PCIE_LC_TRAINING_CNTL - Port Link Training Control. This register
 * seems to control some amount of the general aspects of link training.
 */
/*CSTYLED*/
#define	D_PCIE_PORT_LC_TRAIN_CTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_PORT,	\
	.srd_reg = 0x284	\
}
#define	PCIE_PORT_LC_TRAIN_CTL(n, p, b)	\
    milan_pcie_port_smn_reg((n), D_PCIE_PORT_LC_TRAIN_CTL, (p), (b))
#define	PCIE_PORT_LC_TRAIN_CTL_SET_TRAIN_DIS(r, v)	bitset32(r, 13, 13, v)

/*
 * PCIEPORT::PCIE_LC_CNTL5 - Port Link Control Register 5. There are several
 * others, but this one seems to be required for hotplug.
 */
/*CSTYLED*/
#define	D_PCIE_PORT_LC_CTL5	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_PORT,	\
	.srd_reg = 0x2dc	\
}
#define	PCIE_PORT_LC_CTL5(n, p, b)	\
    milan_pcie_port_smn_reg((n), D_PCIE_PORT_LC_CTL5, (p), (b))
#define	PCIE_PORT_LC_CTL5_SET_WAIT_DETECT(r, v)	bitset32(r, 28, 28, v)

/*
 * PCIEPORT::PCIEP_HCNT_DESCRIPTOR - Port Hotplug Descriptor control. This is a
 * register that exists in 'Port Space' and is specific to a bridge. This seems
 * to relate something in the port to the SMU's hotplug engine.
 */
/*CSTYLED*/
#define	D_PCIE_PORT_HP_CTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_PORT,	\
	.srd_reg = 0x36c	\
}
#define	PCIE_PORT_HP_CTL(n, p, b)	\
    milan_pcie_port_smn_reg((n), D_PCIE_PORT_HP_CTL, (p), (b))
#define	PCIE_PORT_HP_CTL_SET_ACTIVE(r, v)	bitset32(r, 31, 31, v)
#define	PCIE_PORT_HP_CTL_SET_SLOT(r, v)		bitset32(r, 5, 0, v)

/*
 * PCIECORE::PCIE_CI_CNTL - PCIe Port level TX controls. Note, this register is
 * in 'core' space and is specific to the overall milan_pcie_port_t, as opposed
 * to the bridge. XXX Add in other bits.
 */
/*CSTYLED*/
#define	D_PCIE_CORE_CI_CTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_CORE,	\
	.srd_reg = 0x80	\
}
#define	PCIE_CORE_CI_CTL(n, p)	\
    milan_pcie_core_smn_reg((n), D_PCIE_CORE_CI_CTL, (p))
#define	PCIE_CORE_CI_CTL_SET_IGN_LINK_DOWN_CTO_ERR(r, v)	\
    bitset32(r, 31, 31, v)
#define	PCIE_CORE_CI_CTL_SET_LINK_DOWN_CTO_EN(r, v)	bitset32(r, 29, 29, v)

/*
 * PCIECORE::PCIE_SDP_CTRL - PCIe port SDP Control. This register seems to be
 * used to tell the system how to map a given port to the data fabric and
 * related.
 */
/*CSTYLED*/
#define	D_PCIE_CORE_SDP_CTL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_CORE,	\
	.srd_reg = 0x18c	\
}
#define	PCIE_CORE_SDP_CTL(n, p)	\
    milan_pcie_core_smn_reg((n), D_PCIE_CORE_SDP_CTL, (p))
#define	PCIE_CORE_SDP_CTL_SET_PORT_ID(r, v)	bitset32(r, 28, 26, v)
#define	PCIE_CORE_SDP_CTL_SET_UNIT_ID(r, v)	bitset32(r, 3, 0, v)

/*
 * PCIECORE::SWRST_CONTROL_6 - PCIe Software Reset Control #6. This is in 'Core
 * Space' and controls whether or not all of a given set of ports are stopped
 * from training.
 */
/*CSTYLED*/
#define	D_PCIE_CORE_SWRST_CTL6	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_CORE,	\
	.srd_reg = 0x428	\
}
#define	PCIE_CORE_SWRST_CTL6(n, p)	\
    milan_pcie_core_smn_reg((n), D_PCIE_CORE_SWRST_CTL6, (p))
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_K(r, v)	bitset32(r, 10, 10, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_J(r, v)	bitset32(r, 9, 9, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_I(r, v)	bitset32(r, 8, 8, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_H(r, v)	bitset32(r, 7, 7, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_G(r, v)	bitset32(r, 6, 6, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_F(r, v)	bitset32(r, 5, 5, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_E(r, v)	bitset32(r, 4, 4, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_D(r, v)	bitset32(r, 3, 3, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_C(r, v)	bitset32(r, 2, 2, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_B(r, v)	bitset32(r, 1, 1, v)
#define	PCIE_CORE_SWRST_CTL6_SET_HOLD_A(r, v)	bitset32(r, 0, 0, v)

/*
 * PCIECORE::PCIE_PRESENCE_DETECT_SELECT - PCIe Presence Detect Control. This is
 * 'Core Space', so it exists once per port. This is used to determine whether
 * we should consider something present based on the link up OR the side-band
 * signals, or instead require both (e.g. AND).
 */
/*CSTYLED*/
#define	D_PCIE_CORE_PRES	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_PCIE_CORE,	\
	.srd_reg = 0x4e0	\
}
#define	PCIE_CORE_PRES(n, p)	\
    milan_pcie_core_smn_reg((n), D_PCIE_CORE_PRES, (p))
#define	PCIE_CORE_PRES_SET_MODE(r, v)	bitset32(r, 24, 24, v)
#define	PCIE_CORE_PRES_MODE_OR	0
#define	PCIE_CORE_PRES_MODE_AND	1

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
