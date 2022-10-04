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

#ifndef _SYS_IO_FCH_UART_H
#define	_SYS_IO_FCH_UART_H

/*
 * FCH::UART contains a collection of DesignWare UART peripherals.  Huashan has
 * 4 of them; Songshan has 3; we model each as a functional sub-unit.  In
 * addition to FCH::UART, each UART is also associated with an AXI DMA
 * controller that does not normally seem to need anything done to/with it for
 * the UARTs to work.  Nevertheless, we include those here as additional
 * functional sub-units.
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

/*
 * SMN access to the UART registers is possible only on Songshan (yes, I tried
 * it on Huashan; no go).  The DMA controllers are never accessible over SMN
 * apparently.
 */

#define	HUASHAN_MAX_UART	4
#define	SONGSHAN_MAX_UART	3

#define	FCH_UART_SMN_BASE	0x2dd9000
#define	FCH_UART_PHYS_BASE	0xfedc9000
#define	FCH_UART_SIZE		0x1000

#define	FCH_DMA_PHYS_BASE	0xfedc7000
#define	FCH_DMA_SIZE		0x1000

#ifndef	_ASM

/*
 * For consumers like fch(4d) that need the address rather than register
 * descriptors.
 */
static inline uint32_t
songshan_uart_smn_aperture(const uint8_t unit)
{
	const uint32_t base = FCH_UART_SMN_BASE;

	ASSERT3U(unit, <, SONGSHAN_MAX_UART);

	const uint32_t unit32 = (uint32_t)unit;

	if (unit == 2)
		return (base + 0x5000U);

	return (base + unit32 * FCH_UART_SIZE);
}

static inline smn_reg_t
songshan_uart_smn_reg(const uint8_t unit, const smn_reg_def_t def)
{
	const uint32_t aperture = songshan_uart_smn_aperture(unit);
	const uint32_t REG_MASK = 0xfffU;
	ASSERT0(aperture & REG_MASK);

	ASSERT0(def.srd_nents);
	ASSERT0(def.srd_stride);
	ASSERT0(def.srd_size);
	ASSERT3S(def.srd_unit, ==, SMN_UNIT_FCH_UART);
	ASSERT0(def.srd_reg & ~REG_MASK);

	return (SMN_MAKE_REG(aperture + def.srd_reg));
}

/*
 * The MMIO physical blocks are always in the same place, provided the
 * peripheral instance exists.  These are not relocatable, so only the primary
 * FCH's peripherals can be accessed this way.
 */
static inline paddr_t
__common_uart_mmio_aperture(const uint8_t unit, const uint8_t count)
{
	const paddr_t base = FCH_UART_PHYS_BASE;
	const paddr_t unit64 = (const paddr_t)unit;

	ASSERT3U(unit, <, count);

	switch (unit) {
	case 0:
	case 1:
		return (base + unit64 * FCH_UART_SIZE);
	case 2:
	case 3:
		return (base + unit64 * FCH_UART_SIZE + 0x3000UL);
	default:
		panic("unreachable code: invalid UART unit %lu", unit64);
	}
}

static inline paddr_t
__common_dma_mmio_aperture(const uint8_t unit, const uint8_t count)
{
	const paddr_t base = FCH_DMA_PHYS_BASE;
	const paddr_t unit64 = (const paddr_t)unit;

	ASSERT3U(unit, <, count);

	switch (unit) {
	case 0:
	case 1:
		return (base + unit64 * FCH_DMA_SIZE);
	case 2:
	case 3:
		return (base + unit64 * FCH_DMA_SIZE + 0x3000UL);
	default:
		panic("unreachable code: invalid DMA unit %lu", unit64);
	}
}

static inline paddr_t
huashan_uart_mmio_aperture(const uint8_t unit)
{
	return (__common_uart_mmio_aperture(unit, HUASHAN_MAX_UART));
}

static inline paddr_t
songshan_uart_mmio_aperture(const uint8_t unit)
{
	return (__common_uart_mmio_aperture(unit, SONGSHAN_MAX_UART));
}

static inline paddr_t
huashan_dma_mmio_aperture(const uint8_t unit)
{
	return (__common_dma_mmio_aperture(unit, HUASHAN_MAX_UART));
}

static inline paddr_t
songshan_dma_mmio_aperture(const uint8_t unit)
{
	return (__common_dma_mmio_aperture(unit, SONGSHAN_MAX_UART));
}

static inline mmio_reg_block_t
__common_uart_mmio_block(const uint8_t unit, const uint8_t count)
{
	ASSERT3U(unit, <, count);

	const mmio_reg_block_phys_t phys = {
		.mrbp_base = __common_uart_mmio_aperture(unit, count),
		.mrbp_len = FCH_UART_SIZE
	};

	return (mmio_reg_block_map(SMN_UNIT_FCH_UART, phys));
}

static inline mmio_reg_block_t
__common_dma_mmio_block(const uint8_t unit, const uint8_t count)
{
	ASSERT3U(unit, <, count);

	const mmio_reg_block_phys_t phys = {
		.mrbp_base = __common_dma_mmio_aperture(unit, count),
		.mrbp_len = FCH_DMA_SIZE
	};

	return (mmio_reg_block_map(SMN_UNIT_FCH_DMA, phys));
}

static inline mmio_reg_block_t
huashan_uart_mmio_block(const uint8_t unit)
{
	return (__common_uart_mmio_block(unit, HUASHAN_MAX_UART));
}

static inline mmio_reg_block_t
songhan_uart_mmio_block(const uint8_t unit)
{
	return (__common_dma_mmio_block(unit, SONGSHAN_MAX_UART));
}

static inline mmio_reg_block_t
huashan_dma_mmio_block(const uint8_t unit)
{
	return (__common_dma_mmio_block(unit, HUASHAN_MAX_UART));
}

static inline mmio_reg_block_t
songhan_dma_mmio_block(const uint8_t unit)
{
	return (__common_dma_mmio_block(unit, SONGSHAN_MAX_UART));
}

/*
 * Compile-time constant versions of fch_x_mmio_aperture().  Normal code should
 * not use this, only where required for a const initialiser.  const_fn sure
 * would be nice!
 */
#define	FCH_UART_MMIO_APERTURE(u)	\
	((u < 2) ? FCH_UART_PHYS_BASE + (u * FCH_UART_SIZE) :	\
	FCH_UART_PHYS_BASE + u * FCH_UART_SIZE + 0x3000)

#define	FCH_DMA_MMIO_APERTURE(u)	\
	((u < 2) ? FCH_DMA_PHYS_BASE + (u * FCH_DMA_SIZE) :	\
	FCH_DMA_PHYS_BASE + u * FCH_DMA_SIZE + 0x3000)

MAKE_MMIO_FCH_REG_FN(UART, uart, 4);

#define	FCH_UART_REGOFF_DLL	0x00
#define	FCH_UART_REGOFF_RBR	0x00
#define	FCH_UART_REGOFF_THR	0x00
#define	FCH_UART_REGOFF_DLH	0x04
#define	FCH_UART_REGOFF_IER	0x04
#define	FCH_UART_REGOFF_FCR	0x08
#define	FCH_UART_REGOFF_IIR	0x08
#define	FCH_UART_REGOFF_LCR	0x0C
#define	FCH_UART_REGOFF_MCR	0x10
#define	FCH_UART_REGOFF_LSR	0x14
#define	FCH_UART_REGOFF_MSR	0x18
#define	FCH_UART_REGOFF_SCR	0x1c
#define	FCH_UART_REGOFF_FAR	0x70
#define	FCH_UART_REGOFF_USR	0x7c
#define	FCH_UART_REGOFF_TFL	0x80
#define	FCH_UART_REGOFF_RFL	0x84
#define	FCH_UART_REGOFF_SRR	0x88
#define	FCH_UART_REGOFF_SRTS	0x8C
#define	FCH_UART_REGOFF_SBCR	0x90
#define	FCH_UART_REGOFF_SDMAM	0x94
#define	FCH_UART_REGOFF_SFE	0x98
#define	FCH_UART_REGOFF_SRT	0x9C
#define	FCH_UART_REGOFF_STET	0xA0
#define	FCH_UART_REGOFF_CPR	0xF4
#define	FCH_UART_REGOFF_UCV	0xF8
#define	FCH_UART_REGOFF_CTR	0xFC

/*
 * FCH::UART::DLL.  Divisor latch low.
 */

/*CSTYLED*/
#define	D_FCH_UART_DLL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_DLL,	\
	.srd_size = 1			\
}
#define	FCH_UART_DLL_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_DLL, 0)

/*
 * FCH::UART::RBR.  Receive buffer register.
 */

/*CSTYLED*/
#define	D_FCH_UART_RBR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_RBR,	\
	.srd_size = 1			\
}
#define	FCH_UART_RBR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_RBR, 0)

/*
 * FCH::UART::THR.  Transmit hold register.
 */

/*CSTYLED*/
#define	D_FCH_UART_THR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_THR,	\
	.srd_size = 1			\
}
#define	FCH_UART_THR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_THR, 0)

/*
 * FCH::UART::DLH.  Divisor latch high.
 */

/*CSTYLED*/
#define	D_FCH_UART_DLH	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_DLH,	\
	.srd_size = 1			\
}
#define	FCH_UART_DLH_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_DLH, 0)

/*
 * FCH::UART::IER.  Interrupt enable register.
 */

/*CSTYLED*/
#define	D_FCH_UART_XXX	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_XXX,	\
	.srd_size = 1			\
}
#define	FCH_UART_XXX_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_XXX, 0)

/*
 * FCH::UART::FCR.  FIFO control register.
 */

/*CSTYLED*/
#define	D_FCH_UART_FCR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_FCR,	\
	.srd_size = 1			\
}
#define	FCH_UART_FCR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_FCR, 0)

/*
 * FCH::UART::IIR.  Interrupt ID register.
 */

/*CSTYLED*/
#define	D_FCH_UART_IIR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_IIR,	\
	.srd_size = 1			\
}
#define	FCH_UART_IIR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_IIR, 0)

/*
 * FCH::UART::LCR.  Line control register.
 */

/*CSTYLED*/
#define	D_FCH_UART_LCR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_LCR,	\
	.srd_size = 1			\
}
#define	FCH_UART_LCR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_LCR, 0)

/*
 * FCH::UART::MCR.  Modem control register.
 */

/*CSTYLED*/
#define	D_FCH_UART_MCR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_MCR,	\
	.srd_size = 1			\
}
#define	FCH_UART_MCR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_MCR, 0)

/*
 * FCH::UART::LSR.  Line status register.
 */

/*CSTYLED*/
#define	D_FCH_UART_LSR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_LSR,	\
	.srd_size = 1			\
}
#define	FCH_UART_LSR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_LSR, 0)

#define	FCH_UART_LSR_GET_DR(r)	bitx8(r, 0, 0)

/*
 * FCH::UART::MSR.  Modem status register.
 */

/*CSTYLED*/
#define	D_FCH_UART_MSR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_MSR,	\
	.srd_size = 1			\
}
#define	FCH_UART_MSR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_MSR, 0)

/*
 * FCH::UART::SCR.  Scratch register.
 */

/*CSTYLED*/
#define	D_FCH_UART_SCR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SCR,	\
	.srd_size = 1			\
}
#define	FCH_UART_SCR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SCR, 0)

/*
 * FCH::UART::FAR.  FIFO access register.
 */

/*CSTYLED*/
#define	D_FCH_UART_FAR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_FAR	\
}
#define	FCH_UART_FAR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_FAR, 0)

/*
 * FCH::UART::USR.  UART status register.
 */

/*CSTYLED*/
#define	D_FCH_UART_USR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_USR	\
	}
#define	FCH_UART_USR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_USR, 0)

#define	FCH_UART_USR_GET_RFF(r)		bitx32(r, 4, 4)
#define	FCH_UART_USR_GET_RFNE(r)	bitx32(r, 3, 3)
#define	FCH_UART_USR_GET_TFE(r)		bitx32(r, 2, 2)
#define	FCH_UART_USR_GET_TFNF(r)	bitx32(r, 1, 1)

/*
 * FCH::UART::TFL.  Transmit FIFO level.
 */

/*CSTYLED*/
#define	D_FCH_UART_TFL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_TFL	\
}
#define	FCH_UART_TFL_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_TFL, 0)

/*
 * FCH::UART::RFL.  Receive FIFO level.
 */

/*CSTYLED*/
#define	D_FCH_UART_RFL	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_RFL	\
}
#define	FCH_UART_RFL_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_RFL, 0)

/*
 * FCH::UART::SRR.  Shadow reset register.
 */

/*CSTYLED*/
#define	D_FCH_UART_SRR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SRR	\
}
#define	FCH_UART_SRR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SRR, 0)

#define	FCH_UART_SRR_SET_XFR(r, v)	bitset32(r, 2, 2, v)
#define	FCH_UART_SRR_SET_RFR(r, v)	bitset32(r, 1, 1, v)
#define	FCH_UART_SRR_SET_UR(r, v)	bitset32(r, 0, 0, v)

/*
 * FCH::UART::SRTS.  Shadow request to send.
 */

/*CSTYLED*/
#define	D_FCH_UART_SRTS	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SRTS	\
}
#define	FCH_UART_SRTS_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SRTS, 0)

/*
 * FCH::UART::SBCR.  Shadow break control bit.
 */

/*CSTYLED*/
#define	D_FCH_UART_SBCR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SBCR	\
}
#define	FCH_UART_SBCR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SBCR, 0)

/*
 * FCH::UART::SDMAM.  Shadow DMA mode.
 */

/*CSTYLED*/
#define	D_FCH_UART_SDMAM	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SDMAM,\
}
#define	FCH_UART_SDMAM_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SDMAM, 0)

/*
 * FCH::UART::SFE.  Shadow FIFO enable.
 */

/*CSTYLED*/
#define	D_FCH_UART_SFE	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SFE	\
}
#define	FCH_UART_SFE_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SFE, 0)

/*
 * FCH::UART::SRT.  Shadow RCVR trigger.
 */

/*CSTYLED*/
#define	D_FCH_UART_SRT	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_SRT	\
}
#define	FCH_UART_SRT_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_SRT, 0)

/*
 * FCH::UART::STET.  Shadow TX empty trigger.
 */

/*CSTYLED*/
#define	D_FCH_UART_STET	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_STET	\
}
#define	FCH_UART_STET_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_STET, 0)

/*
 * FCH::UART::CPR
 */

/*CSTYLED*/
#define	D_FCH_UART_CPR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_CPR	\
}
#define	FCH_UART_CPR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_CPR, 0)

/*
 * FCH::UART::UCV.  UART component version.
 */

/*CSTYLED*/
#define	D_FCH_UART_UCV	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_UCV	\
}
#define	FCH_UART_UCV_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_UCV, 0)

/*
 * FCH::UART::CTR.  Peripheral's identification code.
 */

/*CSTYLED*/
#define	D_FCH_UART_CTR	(const smn_reg_def_t){	\
	.srd_unit = SMN_UNIT_FCH_UART,	\
	.srd_reg = FCH_UART_REGOFF_CTR	\
}
#define	FCH_UART_CTR_MMIO(b)	\
	fch_uart_mmio_reg((b), D_FCH_UART_CTR, 0)

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_UART_H */
