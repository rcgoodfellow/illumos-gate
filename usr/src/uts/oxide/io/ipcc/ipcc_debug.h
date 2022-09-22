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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2019 by Delphix. All rights reserved.
 * Copyright 2022 Oxide Computer Company
 */

#ifndef _IPCC_DEBUG_H
#define	_IPCC_DEBUG_H

#include <ipcc_drv.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct ipcc_dbgmsg {
	list_node_t idm_node;
	time_t idm_timestamp;
	char idm_msg[1];
} ipcc_dbgmsg_t;

extern void ipcc_dbgmsg_init(void);
extern void ipcc_dbgmsg_fini(void);
extern void ipcc_dbgmsg(void *, const char *fmt, ...);

#ifdef	__cplusplus
}
#endif

#endif	/* _IPCC_DEBUG_H */
