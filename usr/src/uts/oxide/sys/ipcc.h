/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2022 Oxide Computer Company
 */

#ifndef	_SYS_IPCC_H
#define	_SYS_IPCC_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 *	/dev/ipcc
 */
#define	IPCC_DRIVER_NAME	"ipcc"
#define	IPCC_NODE_NAME		"ipcc"
#define	IPCC_MINOR		((minor_t)0x3fffful)
#define	IPCC_DEV		"/dev/ipcc"

/*
 * ioctl numbers
 */
#define	IPCC_IOC		(('i'<<24)|('c'<<16)|('c'<<8))

#define	IPCC_GET_VERSION	(IPCC_IOC|0)
#define	IPCC_REBOOT		(IPCC_IOC|1)
#define	IPCC_POWEROFF		(IPCC_IOC|2)
#define	IPCC_ROT		(IPCC_IOC|3)
#define	IPCC_IDENT		(IPCC_IOC|4)
#define	IPCC_MACS		(IPCC_IOC|5)

#define	IPCC_MIN_MESSAGE_SIZE	19
#define	IPCC_MAX_MESSAGE_SIZE	4123
#define	IPCC_MAX_DATA_SIZE	(IPCC_MAX_MESSAGE_SIZE - IPCC_MIN_MESSAGE_SIZE)

typedef struct ipcc_ident {
	uint8_t		ii_model[11];
	uint8_t		ii_rev;
	uint8_t		ii_serial[11];
} ipcc_ident_t;

typedef struct ipcc_mac {
	uint8_t		im_base[6];
	uint16_t	im_count;
	uint8_t		im_stride;
} ipcc_mac_t;

typedef struct ipcc_rot {
	size_t		ir_len;
	uint8_t		ir_data[IPCC_MAX_DATA_SIZE];
} ipcc_rot_t;

#ifdef __cplusplus
}
#endif

#endif	/* _SYS_IPCC_H */
