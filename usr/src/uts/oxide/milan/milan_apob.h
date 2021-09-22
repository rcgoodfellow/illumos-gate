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

#ifndef _MILAN_APOB_H
#define	_MILAN_APOB_H

/*
 * Definitions that relate to parsing and understanding the Milan APOB
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum milan_apob_group {
	MILAN_APOB_GROUP_MEMORY	= 1,
	MILAN_APOB_GROUP_DF,
	MILAN_APOB_GROUP_CCX,
	MILAN_APOB_GROUP_NBIO,
	MILAN_APOB_GROUP_FCH,
	MILAN_APOB_GROUP_PSP,
	MILAN_APOB_GROUP_GENERAL,
	MILAN_APOB_GROUP_SMBIOS,
	MILAN_APOB_GROUP_FABRIC
} milan_apob_group_t;

extern void milan_apob_init(uint64_t);
extern const void *milan_apob_find(milan_apob_group_t, uint32_t, uint32_t,
    size_t *, int *);

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_APOB_H */
