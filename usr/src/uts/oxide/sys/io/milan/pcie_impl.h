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

#ifndef _SYS_IO_MILAN_PCIE_IMPL_H
#define	_SYS_IO_MILAN_PCIE_IMPL_H

#include <sys/io/milan/fabric.h>
#include <sys/io/milan/dxio_impl.h>
#include <sys/io/milan/pcie.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	MILAN_IOMS_MAX_PCIE_BRIDGES	8
#define	MILAN_IOMS_WAFL_PCIE_NBRIDGES	2

typedef enum milan_pcie_bridge_flag {
	/*
	 * Indicates that there is a corresponding zen_dxio_engine_t associated
	 * with this bridge.
	 */
	MILAN_PCIE_BRIDGE_F_MAPPED	= 1 << 0,
	/*
	 * Indicates that this bridge has been hidden from visibility. When a
	 * port is not used, the brige is hidden.
	 */
	MILAN_PCIE_BRIDGE_F_HIDDEN	= 1 << 1,
	/*
	 * This bridge is being used for hotplug shenanigans. This means that is
	 * actually meaningful.
	 */
	MILAN_PCIE_BRIDGE_F_HOTPLUG	= 1 << 2
} milan_pcie_bridge_flag_t;

typedef enum milan_pcie_port_flag {
	/*
	 * This is used to indicate that a single engine exists on the port that
	 * is in use.
	 */
	MILAN_PCIE_PORT_F_USED		= 1 << 0,
	/*
	 * This indicates that at least one engine mapped to this port is
	 * considered hotpluggable. This is importnat for making sure that we
	 * deal with the visibility of PCIe devices correctl.
	 */
	MILAN_PCIE_PORT_F_HAS_HOTPLUG	= 1 << 1,
} milan_pcie_port_flag_t;

struct milan_pcie_bridge {
	milan_pcie_bridge_flag_t	mpb_flags;
	uint8_t				mpb_device;
	uint8_t				mpb_func;
	uint32_t			mpb_iohc_smn_base;
	uint32_t			mpb_port_smn_base;
	uint32_t			mpb_cfg_smn_base;
	zen_dxio_engine_t		*mpb_engine;
	smu_hotplug_type_t		mpb_hp_type;
	uint16_t			mpb_hp_slotno;
	uint32_t			mpb_hp_smu_mask;
	milan_pcie_port_t		*mpb_port;
};

struct milan_pcie_port {
	milan_pcie_port_flag_t	mpp_flags;
	uint8_t			mpp_portno;
	uint8_t			mpp_sdp_unit;
	uint8_t			mpp_sdp_port;
	uint8_t			mpp_nbridges;
	uint16_t		mpp_dxio_lane_start;
	uint16_t		mpp_dxio_lane_end;
	uint16_t		mpp_phys_lane_start;
	uint16_t		mpp_phys_lane_end;
	uint32_t		mpp_core_smn_addr;
	uint32_t		mpp_strap_smn_addr;
	milan_pcie_bridge_t	mpp_bridges[MILAN_IOMS_MAX_PCIE_BRIDGES];
	milan_ioms_t		*mpp_ioms;
};

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_PCIE_IMPL_H */
