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

#ifndef _PROTOTYPE_H
#define	_PROTOTYPE_H

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
 * |                     |
 * |        XXX          |  There's a whole lot in here we need to document.
 * |     Misc. MMIO      |  The FCH, APICs, other devices, etc.
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

#ifdef __cplusplus
}
#endif

#endif /* _PROTOTYPE_H */
