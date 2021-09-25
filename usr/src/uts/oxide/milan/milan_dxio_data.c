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
 * Copyright 2021 Oxide Computer Company
 */

/*
 * This file contains platform-specific data blobs that are required for the
 * DXIO engine.
 */

#include <milan/milan_dxio_data.h>
#include <sys/stddef.h>
#include <sys/debug.h>

CTASSERT(sizeof (zen_dxio_link_cap_t) == 0x8);
CTASSERT(sizeof (zen_dxio_config_base_t) == 0x18);
CTASSERT(sizeof (zen_dxio_config_net_t) == 0x18);
CTASSERT(sizeof (zen_dxio_config_pcie_t) == 0x18);
CTASSERT(sizeof (zen_dxio_config_t) == 0x18);
CTASSERT(sizeof (zen_dxio_engine_t) == 0x28);
CTASSERT(offsetof(zen_dxio_engine_t, zde_config) == 0x8);
CTASSERT(sizeof (zen_dxio_platform_t) == 0x10);

/*
 * This structure contains the data table which we use to define the engine data
 * for an AMD Ethanol-X platform. Each socket has its own chunk of data because
 * they are totally different.
 */
const zen_dxio_platform_t ethanolx_engine_s0 = {
    .zdp_type = DXIO_PLATFORM_EPYC, .zdp_nengines = 4, .zdp_engines = {
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x2a, .zde_end_lane = 0x39, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		.zdlc_reverse = 1,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x3a, .zde_end_lane = 0x49, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		.zdlc_reverse = 1,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x4a, .zde_end_lane = 0x59, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* No reversing here */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		.zdlc_hp = DXIO_HOTPLUG_T_EXPRESS_MODULE,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x5a, .zde_end_lane = 0x69, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* No reversing here */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	} }
};

/*
 * XXX There is a bunchof ancillary data for SATA by default. Trying to stay
 * laser focused on the objective and thus skipping it since we don't really
 * care about SATA.
 */
const zen_dxio_platform_t ethanolx_engine_s1 = {
    .zdp_type = DXIO_PLATFORM_EPYC, .zdp_nengines = 5, .zdp_engines = {
	{ .zde_type = DXIO_ENGINE_SATA, .zde_hp = 0,
	    .zde_start_lane = 0x3a, .zde_end_lane = 0x41, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_base = { .zdcb_chan_type = 0, .zdcb_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x2a, .zde_end_lane = 0x2d, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* This socket isn't reversing. Don't ask me why. */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		.zdlc_hp = DXIO_HOTPLUG_T_ENT_SSD,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x2e, .zde_end_lane = 0x31, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* This socket isn't reversing. Don't ask me why. */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		.zdlc_hp = DXIO_HOTPLUG_T_ENT_SSD,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x32, .zde_end_lane = 0x35, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* This socket isn't reversing. Don't ask me why. */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		.zdlc_hp = DXIO_HOTPLUG_T_ENT_SSD,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	},
	{ .zde_type = DXIO_ENGINE_PCIE, .zde_hp = 0,
	    .zde_start_lane = 0x36, .zde_end_lane = 0x39, .zde_gpio_group = 1,
	    .zde_reset_group = 1, .zde_search_depth = 0, .zde_kpnp_reset = 0,
	    .zde_config = { .zdc_pcie = { .zdcp_caps = {
		.zdlc_present = DXIO_PORT_PRESENT,
		.zdlc_early_train = 0,
		.zdlc_comp_mode = 0,
		/* This socket isn't reversing. Don't ask me why. */
		.zdlc_reverse = 0,
		.zdlc_max_speed = DXIO_LINK_SPEED_MAX,
		.zdlc_hp = DXIO_HOTPLUG_T_ENT_SSD,
		/* XXX Next two always seems to be set */
		.zdlc_en_off_config = 1,
		.zdlc_off_unused = 1,
		/* XXX This pair is always overriden */
		.zdlc_eq_override = 1,
		.zdlc_eq_mode = 3,
		/* XXX Trust the gods */
		.zdlc_invert_rx_pol = 0x1,
	    } } }
	} }
};
