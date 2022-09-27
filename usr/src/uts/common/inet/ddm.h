/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2022 Oxide Computer Company
 */

#ifndef	_INET_DDM_H
#define	_INET_DDM_H

#include <sys/stream.h>
#include <inet/ip.h>

typedef struct ddm_hdr {
	/*
	 * Identifiers the type of header immediately following the ddm
	 * extension header.
	 */
	uint8_t ddm_next_header;
	/*
	 * Length of extension header not including the first 8 octets (RFC
	 * 6564).
	 */
	uint8_t ddm_length;
	/*
	 * DDM protocol version.
	 */
	uint8_t ddm_version;
	/*
	 * Reserved for future use except first bit, which indicates the packet
	 * is a ddm acknowledgement when set to 1.
	 */
	uint8_t ddm_reserved;
} ddm_t;

/*
 * True if the ddm header is an acknowledgement.
 */
boolean_t ddm_is_ack(ddm_t *ddh);

/*
 * First 8 bits are an origin host id, last 24 bits are a timestamp. Timestamp
 * is only meaningful to the host that generated it.
 */
typedef uint32_t ddm_element;

/*
 * process ddm header on an incoming message block
 */
void ddm_input(mblk_t *mp_chain, ip6_t *ip6h, ip_recv_attr_t *ira);

/* TODO */
void ddm_output(mblk_t *mp_chain);

/*
 * Extract node id from an ddm element.
 */
uint8_t ddm_element_id(ddm_element e);

/*
 * Extract 24 bit timestamp from a ddm element.
 */
uint32_t ddm_element_timestamp(ddm_element e);


/* TODO */
void ddm_update(ip6_t *dst, uint32_t timestamp);

#endif /* _INET_DDM_H */
