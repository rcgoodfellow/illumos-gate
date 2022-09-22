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

#ifndef _IPCC_DRV_H
#define	_IPCC_DRV_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/stdbool.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/ipcc.h>

/*
 * Device state
 */
typedef struct ipcc_state {
	cred_t		*is_cred;
	ldi_handle_t	is_ldih;
	ldi_ident_t	is_ldiid;
} ipcc_t;

#define	LDI_FLAGS (FEXCL | FREAD | FWRITE)

#ifdef	__cplusplus
}
#endif

#endif	/* _IPCC_DRV_H */
