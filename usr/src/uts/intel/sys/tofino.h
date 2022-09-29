/*
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 */

/*
 * Copyright 2022 Oxide Computer Company
 */

#ifndef	_SYS_TOFINO_H
#define	_SYS_TOFINO_H

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Sidecar network header
 *
 * This header is inserted between the ethernet and ip headers by the p4 program
 * running on the Tofino ASIC.
 */
struct schdr {
	uint8_t		sc_code;
	uint16_t	sc_ingress;
	uint16_t	sc_egress;
	uint16_t	sc_ethertype;
	uint8_t		sc_payload[16];
} __packed;

#define	SC_FORWARD_FROM_USERSPACE	0x00
#define	SC_FORWARD_TO_USERSPACE		0x01
#define	SC_ICMP_NEEDED			0x02
#define	SC_ARP_NEEDED			0x03
#define	SC_NEIGHBOR_NEEDED		0x04
#define	SC_INVALID			0xff

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_TOFINO_H */
