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

#ifndef _MILAN_MILAN_FABRIC_H
#define	_MILAN_MILAN_FABRIC_H

/*
 * Definitions that allow us to access the Milan fabric. This consists of the
 * data fabric, northbridges, SMN, and more.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is an entry point for early boot that is used after we have PCIe
 * configuration space set up so we can load up all the information about the
 * actual system itself.
 */
extern void milan_fabric_topo_init(void);

/*
 * This is the primary initialization point for the Milan Data Fabric,
 * Northbridges, PCIe, and related.
 */
extern void milan_fabric_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_MILAN_FABRIC_H */
