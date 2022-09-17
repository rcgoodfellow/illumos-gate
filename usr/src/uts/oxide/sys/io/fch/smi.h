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
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef _SYS_IO_FCH_SMI_H
#define	_SYS_IO_FCH_SMI_H

/*
 * FCH::SMI, all things related to triggering and observing SMIs in the FCH.
 * This does not include ACPI-defined registers in FCH::PM and elsewhere.  Many
 * of the definitions here are tailored for use by assembly, which is why we
 * have the rather annoying _REGOFF macros in addition to the customary macros
 * for constructing C register handles.
 */

#ifndef	_ASM
#include <sys/bitext.h>
#include <sys/types.h>
#include <sys/amdzen/smn.h>
#include <sys/io/mmioreg.h>
#endif	/* !_ASM */

#include <sys/io/fch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	FCH_SMI_OFF		0x0200
#define	FCH_SMI_SMN_BASE	(FCH_RELOCATABLE_SMN_BASE + FCH_SMI_OFF)
#define	FCH_SMI_PHYS_BASE	(FCH_RELOCATABLE_PHYS_BASE + FCH_SMI_OFF)
#define	FCH_SMI_SIZE		0x100

#define	FCH_SMI_REGOFF_EVENTSTATUS	0x00
#define	FCH_SMI_REGOFF_EVENTEN		0x04
#define	FCH_SMI_REGOFF_CAPT_DATA	0x30
#define	FCH_SMI_REGOFF_CAPT_VALID	0x34
#define	FCH_SMI_REGOFF_STATUS0		0x80
#define	FCH_SMI_REGOFF_STATUS1		0x84
#define	FCH_SMI_REGOFF_STATUS2		0x88
#define	FCH_SMI_REGOFF_STATUS3		0x8c
#define	FCH_SMI_REGOFF_STATUS4		0x90
#define	FCH_SMI_REGOFF_SMITRIG0		0x98

#ifndef	_ASM

MAKE_SMN_FCH_REG_FN(SMI, smi, FCH_SMI_SMN_BASE, FCH_SMI_SIZE, 4);
MAKE_MMIO_FCH_RELOC_REG_BLOCK_FNS(SMI, smi, FCH_SMI_OFF, FCH_SMI_SIZE);
MAKE_MMIO_FCH_REG_FN(SMI, smi, 4);

/*
 * There are currently no C consumers of these registers, so there are no
 * register lookup macros.  Of interest, however: some of these registers are
 * saved into a kernel buffer if an SMI ever occurs, and field extractor macros
 * could be useful to interpret the contents of that buffer.  See sys/smm.h.
 */

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_SMI_H */
