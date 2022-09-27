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
/*
 * The ddm protocol embeds hop-by-hop timestamp information in IPv6 extension
 * headers. The ddm extension header has a fixed 8-byte portion that is always
 * present, followed by a variable sized list of elements. There may be between
 * 0 and 15 elements in a single ddm extension header. DDM over greater than 15
 * hops is not currently supported. If the need arises the 15 element limit per
 * ddm extension header will not change, rather extension headers must be
 * chained. This is to keep in line with the recommendations of RFC 6564 for
 * IPv6 extension headers.
 *
 *           0               0               1               2               3
 *           0               8               6               4               2
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     0x00 |  Next Header  | Header Length |    Version    |A|  Reserved   |
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     0x04 |     0.Id      |           0.Timestamp                         |
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     0x08 |     1.Id      |           1.Timestamp                         |
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *          |     ...       |                ...                            |
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *          |     ...       |                ...                            |
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * (N+1)<<2 |     N.Id      |           N.Timestamp                         :
 *          +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Fixed header fields have the following semantics:
 *
 *   Next Header:   IANA IP protocol number of the next header.
 *
 *   Header Length: Length of the ddm header and all elements in bytes not
 *                  including the leading Next Header byte. Follows convention
 *                  established in RFC 6564.
 *
 *   Version:       Version of the ddm protocol.
 *
 *   A:             Acknowledgement bit. A value of 1 indicates this is an
 *                  acknowledgement, 0 otherwise.
 *
 *   Reserved:      Reserved for future use.
 *
 * Element fields have the following semantics
 *
 *   Id:        Identifier for the node that produced this element.
 *
 *   Timestamp: Time this element was produced. This is an opaque 24-bit value
 *              that is only meaningful to the producer of the timestamp.
 */

#ifndef	_INET_DDM_H
#define	_INET_DDM_H

#include <sys/stream.h>
#include <inet/ip.h>

#ifdef	__cplusplus
extern "C" {
#endif

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
__inline boolean_t
ddm_is_ack(ddm_t *ddh)
{
	return ((ddh->ddm_reserved & 1) != 0);
}

/*
 * set the ddm header acknowledgement bit
 */
__inline void
ddm_set_ack(ddm_t *ddh)
{
	ddh->ddm_reserved |= 1;
}

__inline uint16_t
ddm_total_len(ddm_t *ddh)
{
	/* ddh header length field + 1 for the leading 8 bits (RFC 6564) */
	return (ddh->ddm_length + 1);
}

__inline uint8_t
ddm_elements_len(ddm_t *ddh)
{
	return (ddh->ddm_length - 3);
}

__inline uint8_t
ddm_element_count(ddm_t *ddh)
{
	/*
	 * subtract out the ddh header and divide by 4 (ddm elements are 4 bytes
	 * wide)
	 */
	return (ddm_elements_len(ddh) >> 2);
}

/*
 * First 8 bits are an origin host id, last 24 bits are a timestamp. Timestamp
 * is only meaningful to the host that generated it.
 */
typedef uint32_t ddm_element;

#ifdef _KERNEL

/*
 * process ddm header on an incoming message block
 */
mblk_t *ddm_input(mblk_t *mp, ip6_t *ip6h, ip_recv_attr_t *ira);

/*
 * insert a ddm header into the message block mp containing the ipv6 header
 * ip6h.
 */
mblk_t *ddm_output(mblk_t *mp, ip6_t *ip6h);

/*
 * Update the ddm delay tracking table
 */
void ddm_update(ip6_t *dst, ill_t *ill, uint32_t ifindex, uint32_t timestamp);

#endif /* _KERNEL */

/*
 * Extract node id from an ddm element.
 */
__inline uint8_t
ddm_element_id(ddm_element e)
{
	return ((uint8_t)e);
}

/*
 * Extract 24 bit timestamp from a ddm element.
 */
__inline uint32_t
ddm_element_timestamp(ddm_element e)
{
	return (e >> 8);
}


#ifdef	__cplusplus
}
#endif

#endif /* _INET_DDM_H */
