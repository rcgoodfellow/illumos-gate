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

#ifndef _MILAN_MILAN_APOB_H
#define	_MILAN_MILAN_APOB_H

#include <sys/memlist.h>

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

#define	MILAN_APOB_FABRIC_PHY_OVERRIDE		21

/*
 * This section constitutes an undocumented AMD interface.  Do not modify
 * these definitions nor remove this packing pragma.
 */
#pragma pack(1)

typedef struct milan_apob_sysmap_ram_hole {
	uint64_t masmrh_base;
	uint64_t masmrh_size;
	uint32_t masmrh_reason;
	uint32_t _pad;
} milan_apob_sysmap_ram_hole_t;

/*
 * What we get back (if anything) from GROUP_FABRIC type 9 instance 0
 */
typedef struct milan_apob_sysmap {
	uint64_t masm_high_phys;
	uint32_t masm_hole_count;
	uint32_t _pad;
	milan_apob_sysmap_ram_hole_t masm_holes[18];
} milan_apob_sysmap_t;

#pragma pack()

extern void milan_apob_init(uint64_t);
extern const void *milan_apob_find(milan_apob_group_t, uint32_t, uint32_t,
    size_t *, int *);

extern void milan_apob_reserve_phys(void);

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_MILAN_APOB_H */
