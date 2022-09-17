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

#ifndef _SYS_IO_FCH_IOMUX_H
#define	_SYS_IO_FCH_IOMUX_H

/*
 * FCH::IOMUX provides pinmuxing for low-speed peripherals including GPIO and
 * most of the other FCH peripherals.  In addition to FCH::IOMUX, pinmuxing for
 * the pins associated with FCH::RMTGPIO is provided by a separate unit
 * containing part of that logic's register space; see sys/io/fch/rmtgpio.h.
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

#define	FCH_IOMUX_OFF		0x0d00
#define	FCH_IOMUX_SMN_BASE	(FCH_RELOCATABLE_SMN_BASE + FCH_IOMUX_OFF)
#define	FCH_IOMUX_PHYS_BASE	(FCH_RELOCATABLE_PHYS_BASE + FCH_IOMUX_OFF)
#define	FCH_IOMUX_SIZE		0x100

#ifndef	_ASM

MAKE_SMN_FCH_REG_FN(IOMUX, iomux, FCH_IOMUX_SMN_BASE, FCH_IOMUX_SIZE, 1);
MAKE_MMIO_FCH_RELOC_REG_BLOCK_FNS(IOMUX, iomux, FCH_IOMUX_OFF, FCH_IOMUX_SIZE);
MAKE_MMIO_FCH_REG_FN(IOMUX, iomux, 1);

/*
 * Register definitions go here.  The IOMUX is a bit of an oddball in that all
 * of its registers have exactly the same single field, but the internal
 * functions/signals that correspond to the values are different for each one.
 */

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_IOMUX_H */
