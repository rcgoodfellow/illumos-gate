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

#ifndef _SYS_IO_FCH_RMTGPIO_H
#define	_SYS_IO_FCH_RMTGPIO_H

/*
 * FCH::RMTGPIO provides two functional units, one that looks substantially like
 * FCH::GPIO and one that looks substantially like FCH::IOMUX.  Both apply to a
 * subset of low-speed pads.  Because the remote mux is in the middle with
 * additional GPIO-related registers following, we end up with 3 units here,
 * much as we do with FCH::MISC.
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

#define	FCH_RMTGPIO_OFF		0x1200
#define	FCH_RMTGPIO_SMN_BASE	(FCH_RELOCATABLE_SMN_BASE + FCH_RMTGPIO_OFF)
#define	FCH_RMTGPIO_PHYS_BASE	(FCH_RELOCATABLE_PHYS_BASE + FCH_RMTGPIO_OFF)
#define	FCH_RMTGPIO_SIZE	0xc0

#define	FCH_RMTMUX_OFF		0x12c0
#define	FCH_RMTMUX_SMN_BASE	(FCH_RELOCATABLE_SMN_BASE + FCH_RMTMUX_OFF)
#define	FCH_RMTMUX_PHYS_BASE	(FCH_RELOCATABLE_PHYS_BASE + FCH_RMTMUX_OFF)
#define	FCH_RMTMUX_SIZE		0x10

#define	FCH_RMTGPIO_AGG_OFF	0x12f0
#define	FCH_RMTGPIO_AGG_SMN_BASE	\
	(FCH_RELOCATABLE_SMN_BASE + FCH_RMTGPIO_AGG_OFF)
#define	FCH_RMTGPIO_AGG_PHYS_BASE	\
	(FCH_RELOCATABLE_PHYS_BASE + FCH_RMTGPIO_AGG_OFF)
#define	FCH_RMTGPIO_AGG_SIZE	0x10

#ifndef	_ASM

MAKE_SMN_FCH_REG_FN(RMTGPIO, rmtgpio,
    FCH_RMTGPIO_SMN_BASE, FCH_RMTGPIO_SIZE, 4);
MAKE_MMIO_FCH_RELOC_REG_BLOCK_FNS(RMTGPIO, rmtgpio, FCH_RMTGPIO_OFF,
    FCH_RMTGPIO_SIZE);
MAKE_MMIO_FCH_REG_FN(RMTGPIO, rmtgpio, 4);

MAKE_SMN_FCH_REG_FN(RMTMUX, rmtmux, FCH_RMTMUX_SMN_BASE, FCH_RMTMUX_SIZE, 1);
MAKE_MMIO_FCH_RELOC_REG_BLOCK_FNS(RMTMUX, rmtmux, FCH_RMTMUX_OFF,
    FCH_RMTMUX_SIZE);
MAKE_MMIO_FCH_REG_FN(RMTMUX, rmtmux, 1);

MAKE_SMN_FCH_REG_FN(RMTGPIO_AGG, rmtgpio_agg,
    FCH_RMTGPIO_AGG_SMN_BASE, FCH_RMTGPIO_AGG_SIZE, 4);
MAKE_MMIO_FCH_RELOC_REG_BLOCK_FNS(RMTGPIO_AGG, rmtgpio_agg, FCH_RMTGPIO_AGG_OFF,
    FCH_RMTGPIO_AGG_SIZE);
MAKE_MMIO_FCH_REG_FN(RMTGPIO_AGG, rmtgpio_agg, 4);

/*
 * FCH::RMTGPIO::GPIO_x.  As for FCH::GPIO::GPIO_x.  We reuse the FCH_GPIO_STD
 * register definitions as they are generally the same.
 */
/*CSTYLED*/
#define	D_FCH_RMTGPIO_STD	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_RMTGPIO,	\
	.srd_reg = 0x00,	\
	.srd_nents = 16	\
}
#define	FCH_RMTGPIO_STD(i)	fch_rmtgpio_smn_reg(D_FCH_RMTGPIO_STD, i)
#define	FCH_RMTGPIO_STD_MMIO(b, i)	\
    fch_rmtgpio_mmio_reg(b, D_FCH_RMTGPIO_STD, i)

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_RMTGPIO_H */
