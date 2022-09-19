/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef	_SYS_PSM_H
#define	_SYS_PSM_H

/*
 * PSMI == Platform Specific Module Interface
 *
 * We have "inherited" the use of this interface from i86pc, and to date have
 * not made any incompatible changes to its definitions.  Therefore we may at
 * some point wish to make files like psm_types.h common code; however, as yet
 * it's not clear that we won't want to change things.
 *
 * This interface has had a number of different purposes, but the main one on
 * i86pc has been to support the evolution from uniprocessor to very old
 * multiprocessor (dual-8259A and then later original LAPIC and Intel MP BIOS
 * tables) to less old (better-defined xAPIC and ACPI tables, later with the
 * addition of the x2APIC access mechanism even in xAPIC mode) to current
 * (x2APIC and ACPI) mechanisms of dealing with interrupt controllers.  The ops
 * vector type is defined in sys/psm_types.h.
 *
 * Historically, the contents of psm_ops changed numerous times, and each time
 * it did so (which included *incompatible* changes like removing members and
 * changing their types), a new PSMI_X_Y macro was introduced.  Code
 * implementing the PSMI (like this file) defines the appropriate version macro
 * before including the headers.  On i86pc, consumers of the PSMI aren't allowed
 * to assume anything (much) about which version if any the PSM that is actually
 * in use supports, as the ops vector itself is not exposed.  Instead, consumers
 * call into the PSM using a set of global function pointers that are required
 * neither to remain unchanged over the life of the kernel nor to be consistent
 * with one another in terms of coming from the same implementation.  Indeed,
 * the PSMI was expressly designed to allow loading multiple PSMs and having
 * some but not all functions from one implementation override a others.
 *
 * The history of the PSMI with respect to legacy Solaris is as follows:
 *
 * PSMI 1.1 extensions are supported only in 2.6 and later versions.
 * PSMI 1.2 extensions are supported only in 2.7 and later versions.
 * PSMI 1.3 and 1.4 extensions are supported in Solaris 10.
 * PSMI 1.5 extensions are supported in Solaris Nevada.
 * PSMI 1.6 extensions are supported in Solaris Nevada.
 * PSMI 1.7 extensions are supported in Solaris Nevada.
 *
 * All illumos versions support and deliver version 1.7 on i86pc.  On the oxide
 * architecture, life begins at PSMI_1_7.  There is only a single PSM -- apix --
 * and it implements this version of the interface.  There are however other ops
 * that are used during the boot process; see mp_machdep.c where the actual
 * function pointers live along with their initial values.  In the extremely
 * unlikely case that out-of-gate PSMs exist for i86pc, they cannot be expected
 * to work on oxide, even if they define the same PSMI version.  As such modules
 * should always be installed in the platform-specific location in the
 * filesystem, the kernel will not try to load them unless an operator has
 * misplaced it deliberately.  We don't attempt to prevent such things as
 * someone with root access can sabotage the machine in arbitrary ways less
 * complicated than this.
 */

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/sunddi.h>
#include <sys/kmem.h>
#include <sys/psm_defs.h>
#include <sys/psm_types.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * PSM External Interfaces
 */
extern int psm_mod_init(void **, struct psm_info *);
extern int psm_mod_fini(void **, struct psm_info *);
extern int psm_mod_info(void **, struct psm_info *, struct modinfo *);

extern int psm_add_intr(int, avfunc, char *, int, caddr_t);
extern int psm_add_nmintr(int, avfunc, char *, caddr_t);
extern processorid_t psm_get_cpu_id(void);

/* map physical address */
extern caddr_t psm_map(paddr_t, size_t, int);

/* unmap the physical address return from psm_map_phys() */
extern void psm_unmap(caddr_t, size_t);

#define	PSM_PROT_READ		0x0000
#define	PSM_PROT_WRITE		0x0001

/* kernel debugger present? */
extern int psm_debugger(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_PSM_H */
