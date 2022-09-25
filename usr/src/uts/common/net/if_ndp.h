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

#ifndef	_NET_IF_NDP_H
#define	_NET_IF_NDP_H

#include <net/if.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef enum {
	NDP_TYPE_OTHER = 1,
	NDP_TYPE_DYNAMIC,
	NDP_TYPE_STATIC,
	NDP_TYPE_LOCAL,
} ndp_type;

struct ndpr_entry {
	char			ndpre_ifname[LIFNAMSIZ];
	uint8_t			ndpre_l2_addr[6];
	struct in6_addr		ndpre_l3_addr;
	uint16_t		ndpre_state;
	ndp_type		ndpr_type;
};

struct ndpreq {
	uint32_t		ndpr_count;
	caddr_t			ndpr_buf;
};

#ifdef	__cplusplus
}
#endif

#endif	/* _NET_IF_NDP_H */
