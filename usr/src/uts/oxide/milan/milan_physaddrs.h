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

#ifndef _MILAN_MILAN_PHYSADDRS_H
#define	_MILAN_MILAN_PHYSADDRS_H

/*
 * This header contains a bunch of information about physical addresses in the
 * system, what exists, and related.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XXX This memory map is definitely incomplete. Please expand it.
 *
 * The following diagram describes how physical memory is allocated on this
 * system. There are a couple of things to note. First, there are two major
 * reserved areas that exist in the > 4GiB space, each of which is 12 GiB in
 * size. The lower one is problematic in that it shows up right in the middle of
 * the above 4 GiB region of DRAM. As such, we will make sure that we never
 * start MMIO space below this point as we have plenty of space and there's not
 * really much point.
 *
 * +---------------------+ UINT64_MAX
 * |                     |
 * |     End of the      |  All addresses here are aborted by the CPU.
 * |        World        |
 * |                     |
 * +---------------------+ 0xffff_ffff_ffff -- 48 TiB
 * |                     |
 * |       System        |  Reserved by the SoC.
 * |      Reserved       |
 * |                     |
 * +---------------------+ 0xfffd_0000_0000 -- 48 TiB - 12 GiB
 * |                     |
 * |      Primary        |  Primary MMIO Space. Must be assigned to each IOMS
 * |      MMIO to        |  and can then be assigned to each PCIe root complex.
 * |    be assigned      |  Starting address varies based on DRAM population.
 * |                     |
 * +---------------------+ MAX(Core::X86::Msr::TOM2, 0x100_0000_0000 -- 1 TiB)
 *          ~~~~
 * +---------------------+ 0x100_0000_0000 -- 1 TiB
 * |                     |
 * |       System        |  Reserved by the SoC.
 * |      Reserved       |
 * |                     |
 * +---------------------+ 0xfd_0000_0000 -- 1 TiB - 12 GiB
 *          ~~~~
 * +---------------------+ Core::X86::Msr::TOM2
 * |                     |
 * |        DRAM         |  This is the second region of DRAM that continues
 * |       Again!        |  across the lower 4 GiB hole.
 * |                     |
 * +---------------------+ 0x1_0000_0000 -- 4 GiB
 * | boot flash aperture |
 * |     read-only       |
 * +---------------------+ 0xff00_0000
 * |   XXX fill me in!   |
 * +---------------------+ 0xfee0_1000
 * |  legacy LAPIC regs  |
 * |  (movable via BAR)  |
 * +---------------------+ 0xfee0_0000
 * |  XXX more FCH here  |
 * +---------------------+ 0xfedd_0000
 * |        UART3        |
 * +---------------------+ 0xfedc_f000
 * |        UART2        |
 * +---------------------+ 0xfedc_e000
 * |  XXX more FCH here  |
 * +---------------------+ 0xfedc_b000
 * |        UART1        |
 * +---------------------+ 0xfedc_a000
 * |        UART0        |
 * +---------------------+ 0xfedc_9000
 * |  XXX more FCH here  |
 * +---------------------+ 0xfed8_1200
 *          ~~~~			There is much more to fill in here!
 * +---------------------+ 0xfed8_0f00
 * |  FCH miscellaneous  |
 * +- - - - - - - - - - -+ 0xfed8_0e00
 * |        IOMUX        |
 * +- - - - - - - - - - -+ 0xfed8_0d00	Note that all of these devices are
 * |    Watchdog timer   |		part of a single page, so we cannot
 * +- - - - - - - - - - -+ 0xfed8_0b00	protect one driver from another if
 * |   SMBus registers   |		they are separate.
 * +- - - - - - - - - - -+ 0xfed8_0a00
 * |    ASF registers    |
 * +- - - - - - - - - - -+ 0xfed8_0900
 * |    RTC registers    |
 * +- - - - - - - - - - -+ 0xfed8_0700
 * |  ACPI PM2 registers |
 * +- - - - - - - - - - -+ 0xfed8_0400
 * |  ACPI PM registers  |
 * +- - - - - - - - - - -+ 0xfed8_0300
 * |   SMI control regs  |
 * +- - - - - - - - - - -+ 0xfed8_0200
 * |  SMBus controller   |
 * | fake PCI cfg space  |
 * +---------------------+ 0xfed8_0000
 * |        HPET         |
 * +---------------------+ 0xfed0_0000
 * |   eSPI registers    |
 * +---------------------+ 0xfec2_0000
 * |   SPI registers     |
 * +---------------------+ 0xfec1_0000
 * |       IOAPIC        |
 * +---------------------+ 0xfec0_0000
 * |                     |
 * |      Free MMIO      |  This may be available for assigning to 32-bit PCIe
 * |                     |  devices, some may be reserved by the FCH.
 * |                     |
 * +---------------------+ 0xf000_0000 -- 3.75 GiB
 * |                     |
 * |       PCIe          |  Home of our classical memory mapped way of getting
 * |     Extended        |  at PCIe since we no longer need to use I/O ports.
 * | Configuration Space |  There is 1 MiB for each of 256 buses.
 * |                     |
 * +---------------------+ 0xe000_0000 -- 3.5 GiB
 * |                     |
 * |      Available      |  This provides access to 32-bit addresses for PCI
 * |     32-bit MMIO     |  bars and other devices. Still TBD on what we need.
 * |                     |
 * +---------------------+ Core::X86::Msr::TOM
 * |                     |
 * |        DRAM         | In general, this region is the lower part of DRAM.
 * |    from before      | XXX there are probably shenanigans that mean some of
 * |       64-bit        | this is MMIO
 * |                     |
 * +---------------------+ 0x0000_0000 - 0
 */

#define	MILAN_PHYSADDR_PCIECFG	0xe0000000
#define	MILAN_PHYSADDR_MYSTERY_HOLE 0xfd00000000
#define	MILAN_PHYSADDR_MYSTERY_HOLE_END 0x10000000000

#ifdef __cplusplus
}
#endif

#endif /* _MILAN_MILAN_PHYSADDRS_H */
