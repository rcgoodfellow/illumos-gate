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
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef _SYS_PCI_CFGSPACE_IMPL_H
#define	_SYS_PCI_CFGSPACE_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generic PCI constants.  Probably these should be in pci.h.
 */
#define	PCI_MAX_BUSES		256
#define	PCI_MAX_DEVS		32
#define	PCI_MAX_FUNCS		8

/*
 * This is the required size and alignment of PCIe extended configuration space.
 * This needs to be 256 MiB in size. This requires 1 MiB alignment; however,
 * because we use 2 MiB pages, we alway use the larger alignment.
 */
#define	PCIE_CFGSPACE_SIZE	(1024 * 1024 * PCI_MAX_BUSES)
#define	PCIE_CFGSPACE_ALIGN	(1024 * 1024 * 2)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PCI_CFGSPACE_IMPL_H */
