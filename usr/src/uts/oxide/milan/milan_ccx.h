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

#ifndef _MILAN_CCX_H
#define	_MILAN_CCX_H

/*
 * Misc. functions that are required to initialize the Milan core complexes.
 */

#include <sys/stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void milan_ccx_mmio_init(uint64_t);


#ifdef __cplusplus
}
#endif

#endif /* _MILAN_CCX_H */
