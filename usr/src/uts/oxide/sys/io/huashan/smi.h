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

#ifndef _SYS_IO_HUASHAN_SMI_H
#define	_SYS_IO_HUASHAN_SMI_H

/*
 * FCH::SMI, all things related to triggering and observing SMIs in the FCH.
 * This does not include ACPI-defined registers in FCH::PM and elsewhere.
 */

#ifndef	_ASM
#include <sys/types.h>
#include <sys/bitext.h>
#include <sys/io/huashan/fchregs.h>
#endif	/* !_ASM */

#ifdef __cplusplus
extern "C" {
#endif

#define	FCH_SMI_PHYS_BASE			0xfed80200
#define	FCH_SMI_SIZE				0x100

#define	FCH_SMI_R_EVENTSTATUS_OFFSET		0x00
#define	FCH_SMI_R_EVENTSTATUS_WIDTH		32

#define	FCH_SMI_R_EVENTEN_OFFSET		0x04
#define	FCH_SMI_R_EVENTEN_WIDTH			32

#define	FCH_SMI_R_CAPT_DATA_OFFSET		0x30
#define	FCH_SMI_R_CAPT_DATA_WIDTH		32

#define	FCH_SMI_R_CAPT_VALID_OFFSET		0x34
#define	FCH_SMI_R_CAPT_VALID_WIDTH		32

#define	FCH_SMI_R_STATUS0_OFFSET		0x80
#define	FCH_SMI_R_STATUS0_WIDTH			32

#define	FCH_SMI_R_STATUS1_OFFSET		0x84
#define	FCH_SMI_R_STATUS1_WIDTH			32

#define	FCH_SMI_R_STATUS2_OFFSET		0x88
#define	FCH_SMI_R_STATUS2_WIDTH			32

#define	FCH_SMI_R_STATUS3_OFFSET		0x8c
#define	FCH_SMI_R_STATUS3_WIDTH			32

#define	FCH_SMI_R_STATUS4_OFFSET		0x90
#define	FCH_SMI_R_STATUS4_WIDTH			32

#define	FCH_SMI_R_SMITRIG0_OFFSET		0x98
#define	FCH_SMI_R_SMITRIG0_WIDTH		32

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_HUASHAN_SMI_H */
