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

#include <sys/zone.h>
#include <inet/ddm.h>
#include <inet/ip_ire.h>
#include <netinet/ip6.h>

// maximum timestamp size
#define	MAX_TS (1<<24)

static void
ddm_send_ack(mblk_t *mp, ip6_t *ip6h, ddm_t *ddh, ip_recv_attr_t *ira);

void
ddm_input(mblk_t *mp, ip6_t *ip6h, ip_recv_attr_t *ira)
{
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
	 * (e.g. not directly after the hop-by-hop options)?
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
		DTRACE_PROBE(ddm__input__no__elements);
		return;
	}
	ddm_t *ddh = (ddm_t *)&mp->b_rptr[ira->ira_pktlen];

	/*
	 * if this is not an ack, there is no table update to be made so just
	 * send out an ack and return
	 */
	if (!ddm_is_ack(ddh)) {
		ddm_send_ack(mp, ip6h, ddh, ira);
		return;
	}

	/*
	 * If we're here this is an ack and there should be exactly 1 element on
	 * the stack.
	 *
	 * Stack length less than one indicates there is no ToS element. That
	 * should not happen.
	 *
	 * Stack length greater than one indicates that somehow an ack got back
	 * to us without popping off all path elements on the egress path
	 */
	if (ddh->ddm_length != ((sizeof (ddm_t) - 1) + sizeof (ddm_element))) {
		DTRACE_PROBE1(ddm__input__bad__ack__len,
		    uint8_t, ddh->ddm_length);

		return;
	}

	/*
	 * TODO ensure this ack is for us
	 */

	/*
	 * read ToS element and update ddm table
	 */

	ddm_element dde = *(ddm_element*)&mp->b_rptr[
	    ira->ira_pktlen + sizeof (ddm_t)];

	ddm_update(
	    ip6h,
	    ira->ira_ill,
	    ira->ira_rifindex,
	    ddm_element_timestamp(dde));

	/*
	 * set next header protocol and packet length for ira
	 */

	ira->ira_pktlen += sizeof (ddm_t) + sizeof (ddm_element);
	ira->ira_protocol = ddh->ddm_next_header;
}

static void
ddm_send_ack(mblk_t *mp, ip6_t *ip6h, ddm_t *ddh, ip_recv_attr_t *ira)
{
	/* allocate and link up message blocks */
	mblk_t *ip6_mp = allocb(sizeof (ip6_t), BPRI_HI);
	mblk_t *ddm_mp = allocb(ddm_total_len(ddh), BPRI_HI);
	ip6_mp->b_next = ddm_mp;
	ddm_mp->b_prev = ip6_mp;

	/* create the ipv6 header */
	ip6_t *ack_ip6 = (ip6_t *)ip6_mp->b_wptr;
	ack_ip6->ip6_vcf = ip6h->ip6_vcf;
	ack_ip6->ip6_plen = ddm_total_len(ddh);
	ack_ip6->ip6_nxt = 0xdd;
	ack_ip6->ip6_hlim = 64;
	ip6_mp->b_wptr = (unsigned char *)&ack_ip6[1];

	/* create the ddm extension header */
	ddm_t *ack_ddh = (ddm_t *)ddm_mp->b_wptr;
	*ack_ddh = *ddh;
	/* add elements, an ack includes all the received elements */
	ddm_element *src = (ddm_element*)&ddh[1];
	ddm_element *dst = (ddm_element*)&ack_ddh[1];
	memcpy(dst, src, ddm_elements_len(ddh));
	ddm_mp->b_wptr = (unsigned char *)&dst[ddm_element_count(ddh)];

	/* set up transmit attributes */
	ip_xmit_attr_t ixa;
	bzero(&ixa, sizeof (ixa));
	ixa.ixa_ifindex = ira->ira_rifindex;
	ixa.ixa_ipst = ira->ira_rill->ill_ipst;
	ixa.ixa_flags = IXAF_BASIC_SIMPLE_V6;
	ixa.ixa_flags &= ~IXAF_VERIFY_SOURCE;

	/* send out the ack */
	ip_output_simple_v6(ip6_mp, &ixa);
	ixa_cleanup(&ixa);
}

mblk_t *
ddm_output(mblk_t *mp, ip6_t *ip6h)
{
	ASSERT(mp);
	ASSERT(ip6h);

	mblk_t *mp1 = allocb(
	    sizeof (ip6_t) +
	    sizeof (ddm_t) +
	    sizeof (ddm_element),
	    BPRI_HI);

	if (!mp1) {
		DTRACE_PROBE(ddm__output__allocb__failed);
		return (mp);
	}

	/*
	 * get pointers to header elements in the new message block
	 */

	ip6_t *v6 = (ip6_t *)mp1->b_rptr;
	ddm_t *ddh = (ddm_t *)&v6[1];
	ddm_element *dde = (ddm_element*)&ddh[1];

	/*
	 * fill in the ddm header
	 */
	ddh->ddm_next_header = ip6h->ip6_nxt;
	/* ddh header + 1 element minus leading 8 bits (RFC 6564) */
	ddh->ddm_length = 7;
	ddh->ddm_version = 1;

	/*
	 * fill in the ddm element
	 */

	/* TODO set node id */
	/* Set node timestamp as the high 24 bits. */
	*dde = ((uint32_t)(gethrtime() % MAX_TS) << 8);

	/*
	 * update the ipv6 header and copy into new msg block
	 */
	ip6h->ip6_plen += htons(ntohs(ip6h->ip6_plen) + 8);
	ip6h->ip6_nxt = 0xdd;
	*v6 = *ip6h;

	/*
	 * set write pointer to just after the ddm element, set the original
	 * message block as a continuation of the new one containing the ddm
	 * header and update the read pointer of the original messge block to
	 * move past the ipv6 header that now resides in the new message block
	 */
	mp1->b_wptr = (unsigned char *)&dde[1];
	mp1->b_cont = mp;
	mp->b_rptr += sizeof (ip6_t);

	/* return the new message block to the caller */
	return (mp1);
}

void
ddm_update(
    ip6_t *dst,
    ill_t *ill,
    uint32_t ifindex,
    uint32_t timestamp)
{
	/* look up routing table entry */
	ire_t *ire = ire_ftable_lookup_v6(
	    &dst->ip6_dst,
	    NULL,		/* TODO mask */
	    NULL,		/* TODO gateway */
	    0,			/* TODO type */
	    ill,		/* only consider routes on this ill */
	    ALL_ZONES,		/* TODO zone */
	    NULL,		/* TODO tsl */
	    MATCH_IRE_ILL,
	    0,			/* TODO xmit_hint */
	    ill->ill_ipst,
	    NULL		/* TODO generationop */);

	if (!ire) {
		DTRACE_PROBE1(ddm__update__no__route,
		    in_addr_t *, &dst->ip6_dst);
		return;
	}

	DTRACE_PROBE2(ddm__update_timestamp,
	    in_addr_t *, &dst->ip6_dst,
	    uint32_t, ifindex);

	/* update routing table entry delay measurement */
	uint32_t now = ((uint32_t)(gethrtime() % MAX_TS) << 8);
	ire->ire_delay = now - timestamp;
}
