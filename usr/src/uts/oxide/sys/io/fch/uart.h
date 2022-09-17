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

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_FCH_UART_H */
