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

#include <inet/ddm.h>
#include <netinet/ip6.h>

void ddm_input(mblk_t *mp, ip6_t *ip6h, ip_recv_attr_t *ira) {

	/*
	 * At this point the ipv6 header has been read and any hop-by-hop
	 * extension headers have been read and we've detected that next header
	 * is a ddm header.
	 *
	 * Now we do the following.
	 * 1. Read and sanity check the ddm static header.
	 * 2. Ensure the top-of-stack ToS ddm element is for us (bail if not).
	 * 3. Read the ToS element and update the kernel ddm table.
	 * 4. Set the ira protocol to the next header value in the ddm packet.
	 * 5. Move the ira packet length past the ddm extension header.
	 *
	 * TODO:
	 * - What about ddm headers that come after other extension headers
	 * (e.g. not directly after the hop-by-hop options).
	 */

	/*
	 * sanity check static header
	 */

	/*
	 * there must be at least one ddm element for us to do something
	 * useful.
	 */
	char *data = ip_pullup(
		mp,
		ira->ira_pktlen + sizeof (ddm_t) + sizeof (ddm_element),
		ira);

	if (!data) {
		/* TODO dtrace */
		return;
	}
	ddm_t *ddh = (ddm_t *)&mp->b_rptr[ira->ira_pktlen];

	/* if this is not an ack, there is nothing to do */
	if (!ddm_is_ack(ddh)) {
		/*
		 * TODO dtrace: it's not normal for a ddm packet that's not an
		 * acknowledgement to land at a server router (currently the ddm
		 * illumos module does not support acting as a transit router).
		 */
		return;
	}

	if (ddh->ddm_length != (sizeof (ddm_t) - 8 + sizeof (ddm_element))) {
		/*
		 * TODO dtrace:
		 *  - shorter indicates there is no ToS element
		 *  - longer indicates that somehow an ack got back to us
		 *    without popping off all path elements on the egress path
		 */
		return;
	}

	/*
	 * TODO ensure this is an ack for us
	 */

	/*
	 * read ToS element and update ddm table
	 */

	ddm_element dde = *(ddm_element*)&mp->b_rptr[
		ira->ira_pktlen + sizeof (ddm_t) + sizeof (ddm_element)];

	ddm_update(ip6h, ddm_element_timestamp(dde));

	/*
	 * set next header protocol and packet length for ira
	 */

	ira->ira_pktlen += sizeof (ddm_t) + sizeof (ddm_element);
	ira->ira_protocol = ddh->ddm_next_header;

}

/* TODO */
void ddm_output(mblk_t *mp) {
}

uint8_t ddm_element_id(ddm_element e) {
	return ((uint8_t)e);
}

uint32_t ddm_element_timestamp(ddm_element e) {
	return (e >> 8);
}

/* TODO */
void ddm_update(ip6_t *dst, uint32_t timestamp) {
}

boolean_t ddm_is_ack(ddm_t *ddh) {
	return ((ddh->ddm_reserved & 1) != 0);
}
