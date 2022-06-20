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

#ifndef _SYS_IO_MILAN_IOHC_H
#define	_SYS_IO_MILAN_IOHC_H

/*
 * Addresses and register definitions for the I/O hub core (IOHC) found in Milan
 * processors and likely future generations as well.  The IOHC is part of the
 * NBIO block, which comes from the legacy "north bridge" designation, and
 * connects the internal HT-based fabric with PCIe, the FCH, and other I/O
 * devices and fabrics.  While there is nominally but one IOHC per I/O die (of
 * which Milan has but one per SOC), in practice there are four instances on
 * that die, each of which is connected to the DF via an I/O master/slave (IOMS)
 * component, has its own independent set of registers, and connects its own
 * collection of downstream resources (root ports, NBIFs, etc.) to the DF.
 * There are several sub-blocks in the IOHC including the IOAGR and SDP mux, and
 * their registers are defined here.  Registers in connected components such as
 * PCIe root ports, NBIFs, IOAPICs, IOMMUs, and the FCH are defined elsewhere.
 */

#include <sys/bitext.h>
#include <sys/io/milan/smn.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t milan_iohc_read32(milan_ioms_t *, uint32_t);
extern void milan_iohc_write32(milan_ioms_t *, uint32_t, uint32_t);

/*
 * IOMS SMN bases and various shifts based on instance ID to indicate the right
 * device. Sometimes we need to select the correct SMN aperture ID and other
 * times we just need to select an offset into that aperture.
 */
#define	MILAN_SMN_IOHC_BASE	0x13b00000
#define	MILAN_SMN_IOHC_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_IOHC_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOHC_BASE_BITS, _r)
#define	MILAN_SMN_IOAGR_BASE	0x15b00000
#define	MILAN_SMN_IOAGR_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_IOAGR_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOAGR_BASE_BITS, _r)
#define	MILAN_SMN_IOMS_SHIFT(x)	((x) << 20)

/*
 * The SDPMUX SMN addresses are a bit weird. There is one per IOMS instance;
 * however, the SMN addresses are very different. While they increment based on
 * the IOMS shift above, they actually add one to the IOMS id, unlike others. In
 * addition, everything beyond the first also adds (1 << 23). It is unclear why
 * exactly this is, but just comes to how the different aperture IDs seem to
 * have shaken out.
 */
#define	MILAN_SMN_SDPMUX_BASE	0x04400000
#define	MILAN_SMN_SDPMUX_BASE_BITS	MILAN_SMN_ADDR_BLOCK_BITS
#define	MILAN_SMN_SDPMUX_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_SDPMUX_BASE_BITS, _r)
#define	MILAN_SMN_SDPMUX_IOMS_SHIFT(x)	((1 << 23) + ((x + 1) << 20))

/*
 * IOHC Registers of Interest. The SMN based addresses are all relative to the
 * IOHC base address.
 */

/*
 * IOHC::NB_TOP_OF_DRAM_SLOT1. This indicates where the top of DRAM below (or
 * at) 4 GiB is. Note, bit 32 for getting to 4 GiB is actually in bit 0.
 * Otherwise it's all bits 31:23.
 */
#define	MILAN_IOHC_R_PCI_NB_TOP_OF_DRAM		0x90
#define	MILAN_IOHC_R_SET_NB_TOP_OF_DRAM(r, v)		bitset32(r, 31, 23, v)
#define	MILAN_IOHC_R_SET_NB_TOP_OF_DRAM_BIT32(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::IOHC_REFCLK_MODE. Seemingly controls the speed of the reference clock
 * that is presumably used by PCIe.
 */
#define	MILAN_IOHC_R_SMN_REFCLK_MODE	0x10020
#define	MILAN_IOHC_R_REFCLK_MODE_SET_MODE_27MHZ(r, v)	bitset32(r, 2, 2, v)
#define	MILAN_IOHC_R_REFCLK_MODE_SET_MODE_25MHZ(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_REFCLK_MODE_SET_MODE_100MHZ(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::IOHC_PCIE_CRS_Count. Controls configuration space retries. The limit
 * indicates the length of time that retries can be issued for. Apparently in
 * 1.6ms units. The delay is the amount of time that is used between retries,
 * which are in units of 1.6us.
 */
#define	MILAN_IOHC_R_SMN_PCIE_CRS_COUNT		0x10028
#define	MILAN_IOHC_R_SET_PCIE_CRS_COUNT_LIMIT(r, v)	bitset32(r, 27, 16, v)
#define	MILAN_IOHC_R_SET_PCIE_CRS_COUNT_DELAY(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::NB_BUS_NUM_CNTL
 */
#define	MILAN_IOHC_R_SMN_BUS_NUM_CNTL		0x10044
#define	MILAN_IOHC_R_SET_BUS_NUM_CNTL_EN(r, v)		bitset32(r, 8, 8, v)
#define	MILAN_IOHC_R_SET_BUS_NUM_CNTL_BUS(r, v)		bitset32(r, 7, 0, v)

/*
 * IOHC::NB_LOWER_TOP_OF_DRAM2.  Indicates to the NB where DRAM above 4 GiB goes
 * up to. Note, that due to the wholes where there are system reserved ranges of
 * memory near 1 TiB, this may be split into two values.
 */
#define	MILAN_IOHC_R_SMN_DRAM_TOM2_LOW		0x10064
#define	MILAN_IOHC_R_SET_DRAM_TOM2_LOW_EN(r, v)		bitset32(r, 0, 0, v)
#define	MILAN_IOHC_R_SET_DRAM_TOM2_LOW_TOM2(r, v)	bitset32(r, 31, 23, v)

/*
 * IOHC::NB_UPPER_TOP_OF_DRAM2.
 */
#define	MILAN_IOHC_R_SMN_DRAM_TOM2_HI		0x10068
#define	MILAN_IOHC_R_SET_DRAM_TOM2_HI_TOM2(r, v)	bitset32(r, 8, 0, v)

/*
 * IOHC::NB_LOWER_DRAM2_BASE. This indicates the starting address of DRAM at 4
 * GiB. This register resets to all zeros indicating that it starts at 4 GiB,
 * hence why it is not set. This contains the lower 32 bits (of which 31:23) are
 * valid.
 */
#define	MILAN_IOHC_R_SMN_DRAM_BASE2_LOW		0x1006c
#define	MILAN_IOHC_R_SET_DRAM_BASE2_LOW_BASE(x)		bitset32(r, 31, 23, v)

/*
 * IOHC::NB_UPPER_DRAM2_BASE. This indicates the starting address of DRAM at 4
 * GiB. This register resets to 001h indicating that it starts at 4 GiB, hence
 * why it is not set. This contains the upper 8 bits (47:32) of the starting
 * address.
 */
#define	MILAN_IOHC_R_SMN_DRAM_BASE2_HI		0x10070
#define	MILAN_IOHC_R_SET_DRAM_BASE2_HI_BASE(r, v)	bitset32(r, 8, 0, v)

/*
 * IOHC::SB_LOCATION. Indicates where the FCH aka the old south bridge is
 * located.
 */
#define	MILAN_IOHC_R_SMN_SB_LOCATION		0x1007c
#define	MILAN_IOHC_R_SET_SMN_SB_LOCATION_CORE(r, v)	bitset32(r, 31, 16, v)
#define	MILAN_IOHC_R_SET_SMN_SB_LOCATION_PORT(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::IOHC_FEATURE_CNTL. As it says on the tin, controls some various feature
 * bits here.
 */
#define	MILAH_IOHC_R_SMN_FEATURE_CNTL		0x10118
#define	MILAN_IOHC_R_FEATURE_CNTL_GET_DGPU(r)	bitx32(r, 28, 28)
#define	MILAN_IOHC_R_FEATURE_CNTL_SET_ARI(r, v)	bitset32(r, 22, 22, v)
#define	MILAN_IOHC_R_FEATURE_CNTL_GET_ARCH(r)	bitx32(r, 3, 3)
#define	MILAN_IOHC_R_FEATURE_CNTL_SET_P2P(r, v)	bitset32(r, 2, 1, v)
#define	MILAN_IOHC_R_FEATURE_CNTL_P2P_DROP_NMATCH	0
#define	MILAN_IOHC_R_FEATURE_CNTL_P2P_FWD_NMATCH	1
#define	MILAN_IOHC_R_FEATURE_CNTL_P2P_FWD_ALL		2
#define	MILAN_IOHC_R_FEATURE_CNTL_P2P_DISABLE		3
#define	MILAN_IOHC_R_FEATURE_CNTL_GET_HP_DEVID_EN(x)	bitx32(r, 0, 0)

/*
 * IOHC::IOHC_INTERRUPT_EOI.  Used to indicate that an SCI, NMI, or SMI
 * originating from this (or possibly any) IOHC has been serviced.  All fields
 * in this register are write-only and can only meaningfully be set, not
 * cleared.
 */
#define	MILAN_IOHC_R_SMN_INTR_EOI		0x10120
#define	MILAN_IOHC_R_INTR_EOI_SET_NMI(r)	bitset32(r, 2, 2, 1)
#define	MILAN_IOHC_R_INTR_EOI_SET_SCI(r)	bitset32(r, 1, 1, 1)
#define	MILAN_IOHC_R_INTR_EOI_SET_SMI(r)	bitset32(r, 0, 0, 1)

/*
 * IOHC::IOHC_PIN_CNTL.  This register has only a single field, which defines
 * whether external assertion of the NMI_SYNCFLOOD_L pin causes an NMI or a SYNC
 * FLOOD.  This register is defined only for the IOHC which shares an IOMS with
 * the FCH.
 */
#define	MILAN_IOHC_R_SMN_PIN_CNTL		0x10128
#define	MILAN_IOHC_R_PIN_CNTL_GET_MODE(r)		bitx32(r, 0, 0)
#define	MILAN_IOHC_R_PIN_CNTL_SET_MODE_SYNCFLOOD(r)	bitset32(r, 0, 0, 0)
#define	MILAN_IOHC_R_PIN_CNTL_SET_MODE_NMI(r)		bitset32(r, 0, 0, 1)

/*
 * IOHC::IOHC_FEATURE_CNTL2.  Status register that indicates whether certain
 * error events have occurred, including NMI drops, CRS retries, SErrs, and NMI
 * generation.  All fields are RW1c except for SErr which is RO.
 */
#define	MILAN_IOHC_R_SMN_FCTL2			0x10130
#define	MILAN_IOHC_R_FCTL2_GET_NP_DMA_DROP(r)	bitx32(r, 18, 18)
#define	MILAN_IOHC_R_FCTL2_SET_NP_DMA_DROP(r)	bitset32(r, 18, 18, 1)
#define	MILAN_IOHC_R_FCTL2_GET_P_DMA_DROP(r)	bitx32(r, 17, 17)
#define	MILAN_IOHC_R_FCTL2_SET_P_DMA_DROP(r)	bitset32(r, 17, 17, 1)
#define	MILAN_IOHC_R_FCTL2_GET_CRS(r)		bitx32(r, 16, 16)
#define	MILAN_IOHC_R_FCTL2_SET_CRS(r)		bitset32(r, 16, 16, 1)
#define	MILAN_IOHC_R_FCTL2_GET_SERR(r)		bitx32(r, 1, 1)
#define	MILAN_IOHC_R_FCTL2_GET_NMI(r)		bitx32(r, 0, 0)
#define	MILAN_IOHC_R_FCTL2_SET_NMI(r)		bitset32(r, 0, 0, 1)

/*
 * IOHC::NB_TOP_OF_DRAM3. This is another use of defining memory. It starts at
 * bit 40 of PA. This register is a bit different from the others in that it is
 * an inclusive register. The register containts bits 51:22, mapped to the
 * register's 29:0.
 */
#define	MILAN_IOHC_R_SMN_DRAM_TOM3		0x10138
#define	MILAN_IOHC_R_SET_DRAM_TOM3_EN(r, v)	bitset32(r, 31, 31, v)
#define	MILAN_IOHC_R_SET_DRAM_TOM3_LIMIT(r, v)	bitset32(r, 29, 0, v)

/*
 * IOHC::PSP_BASE_ADDR_LO. This contains the MMIO address that is used by the
 * PSP.
 */
#define	MILAN_IOHC_R_SMN_PSP_ADDR_LO		0x102e0
#define	MILAN_IOHC_R_SET_PSP_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 20, v)
#define	MILAN_IOHC_R_SET_PSP_ADDR_LO_LOCK(r, v)	bitset32(r, 7, 8, v)
#define	MILAN_IOHC_R_SET_PSP_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::PSP_BASE_ADDR_HI. This contains the upper bits of the PSP base
 * address.
 */
#define	MILAN_IOHC_R_SMN_PSP_ADDR_HI		0x102e4
#define	MILAN_IOHC_R_SET_PSP_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::SMU_BASE_ADDR_LO. This contains the MMIO address that is used by the
 * SMU.
 */
#define	MILAN_IOHC_R_SMN_SMU_ADDR_LO		0x102e8
#define	MILAN_IOHC_R_SET_SMU_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 20, v)
#define	MILAN_IOHC_R_SET_SMU_ADDR_LO_LOCK(r, v)	bitset32(r, 7, 8, v)
#define	MILAN_IOHC_R_SET_SMU_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::SMU_BASE_ADDR_HI. This contains the upper bits of the SMU base
 * address.
 */
#define	MILAN_IOHC_R_SMN_SMU_ADDR_HI		0x102ec
#define	MILAN_IOHC_R_SET_SMU_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::IOAPIC_BASE_ADDR_LO. This contains the MMIO address that is used by the
 * IOAPIC.
 */
#define	MILAN_IOHC_R_SMN_IOAPIC_ADDR_LO		0x102f0
#define	MILAN_IOHC_R_SET_IOAPIC_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 8, v)
#define	MILAN_IOHC_R_SET_IOAPIC_ADDR_LO_LOCK(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_SET_IOAPIC_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::IOAPIC_BASE_ADDR_HI. This contains the upper bits of the IOAPIC base
 * address.
 */
#define	MILAN_IOHC_R_SMN_IOAPIC_ADDR_HI		0x102f4
#define	MILAN_IOHC_R_SET_IOAPIC_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::DBG_BASE_ADDR_LO. This contains the MMIO address that is used by the
 * DBG registers. What this debugs, is unfortunately unclear.
 */
#define	MILAN_IOHC_R_SMN_DBG_ADDR_LO		0x102f8
#define	MILAN_IOHC_R_SET_DBG_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 20, v)
#define	MILAN_IOHC_R_SET_DBG_ADDR_LO_LOCK(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_SET_DBG_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::DBG_BASE_ADDR_HI. This contains the upper bits of the DBG base
 * address.
 */
#define	MILAN_IOHC_R_SMN_DBG_ADDR_HI		0x102fc
#define	MILAN_IOHC_R_SET_DBG_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::FASTREG_BASE_ADDR_LO. This contains the MMIO address that is used by
 * the 'FastRegs' which provides access to an SMN aperture.
 */
#define	MILAN_IOHC_R_SMN_FASTREG_ADDR_LO		0x10300
#define	MILAN_IOHC_R_SET_FASTREG_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 20, v)
#define	MILAN_IOHC_R_SET_FASTREG_ADDR_LO_LOCK(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_SET_FASTREG_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::FASTREG_BASE_ADDR_HI. This contains the upper bits of the FASTREG base
 * address.
 */
#define	MILAN_IOHC_R_SMN_FASTREG_ADDR_HI		0x10304
#define	MILAN_IOHC_R_SET_FASTREG_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::FASTREGCNTL_BASE_ADDR_LO. This contains the MMIO address that is used
 * by the FASTREGCNTL.
 */
#define	MILAN_IOHC_R_SMN_FASTREGCNTL_ADDR_LO		0x10308
#define	MILAN_IOHC_R_SET_FASTREGCNTL_ADDR_LO_ADDR(r, v)	bitset32(r, 31, 12, v)
#define	MILAN_IOHC_R_SET_FASTREGCNTL_ADDR_LO_LOCK(r, v)	bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_SET_FASTREGCNTL_ADDR_LO_EN(r, v)	bitset32(r, 0, 0, v)

/*
 * IOHC::FASTREGCNTL_BASE_ADDR_HI. This contains the upper bits of the
 * FASTREGCNTL base address.
 */
#define	MILAN_IOHC_R_SMN_FASTREGCNTL_ADDR_HI		0x1030c
#define	MILAN_IOHC_R_SET_FASTREGCNTL_ADDR_HI_ADDR(r, v)	bitset32(r, 15, 0, v)

/*
 * IOHC::IOHC_SDP_PORT_CONTROL. This is used to control how the port disconnect
 * behavior operates for the connection to the data fabric.
 */
#define	MILAN_IOHC_R_SMN_SDP_PORT_CONTROL	0x10344
#define	MILAN_IOHC_R_SET_SDP_PORT_CONTROL_SDF_RT_HYSTERESIS(r, v)	\
    bitset32(r, 15, 8, v)
#define	MILAN_IOHC_R_SET_SDP_PORT_CONTROL_PORT_HYSTERESIS(r, v)	\
    bitset32(r, 7, 0, v)

/*
 * IOHC::IOHC_EARLY_WAKE_UP_EN. This is seemingly used to control how the SDP
 * port and DMA work with clock requests.
 */
#define	MILAN_IOHC_R_SMN_SDP_EARLY_WAKE_UP	0x10348
#define	MILAN_IOHC_R_SET_SDP_EARLY_WAKE_UP_HOST_ENABLE(r, v)	\
    bitset32(r, 31, 16, v)
#define	MILAN_IOHC_R_SET_SDP_EARLY_WAKE_UP_DMA_ENABLE(r, v)	\
    bitset32(r, 0, 0, v)

/*
 * IOHC::USB_QoS_CNTL. This controls the USB data fabric priority.
 */
#define	MILAN_IOHC_R_SMN_USB_QOS_CNTL		0x14044
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID1_EN(r, v)	bitset32(r, 28, 28, v)
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID1_PRI(r, v)	bitset32(r, 27, 24, v)
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID1_ID(r, v)	bitset32(r, 22, 16, v)
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID0_EN(r, v)	bitset32(r, 12, 12, v)
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID0_PRI(r, v)	bitset32(r, 11, 8, v)
#define	MILAN_IOHC_R_SET_USB_QOS_CNTL_UNID0_ID(r, v)	bitset32(r, 6, 0, v)

/*
 * IOHC::IOHC_SION_S0_CLIENT_REQ_BURSTTARGET_LOWER and friends. There are a
 * bunch of these and a varying number of them. These registers all seem to
 * adjust arbitration targets, what should be preferred, and related. There are
 * a varying number of instances of this in each IOHC MISC. There are also
 * definitions for values to go in these. Not all of the registers in the PPR
 * are set. Not all instances of these are always set with values. I'm sorry, I
 * can only speculate as to why.
 */
#define	MILAN_IOHC_R_SMN_SION_S0_CLIREQ_BURST_LOW	0x14400
#define	MILAN_IOHC_R_SMN_SION_S0_CLIREQ_BURST_HI	0x14404
#define	MILAN_IOHC_R_SMN_SION_S0_CLIREQ_TIME_LOW	0x14408
#define	MILAN_IOHC_R_SMN_SION_S0_CLIREQ_TIME_HI		0x1440c

#define	MILAN_IOHC_R_SMN_SION_S0_RDRSP_BURST_LOW	0x14410
#define	MILAN_IOHC_R_SMN_SION_S0_RDRSP_BURST_HI		0x14414
#define	MILAN_IOHC_R_SMN_SION_S0_RDRSP_TIME_LOW		0x14418
#define	MILAN_IOHC_R_SMN_SION_S0_RDRSP_TIME_HI		0x1441c

#define	MILAN_IOHC_R_SMN_SION_S0_WRRSP_BURST_LOW	0x14420
#define	MILAN_IOHC_R_SMN_SION_S0_WRRSP_BURST_HI		0x14424
#define	MILAN_IOHC_R_SMN_SION_S0_WRRSP_TIME_LOW		0x14428
#define	MILAN_IOHC_R_SMN_SION_S0_WRRSP_TIME_HI		0x1442c

#define	MILAN_IOHC_R_SMN_SION_S1_CLIREQ_BURST_LOW	0x14430
#define	MILAN_IOHC_R_SMN_SION_S1_CLIREQ_BURST_HI	0x14434
#define	MILAN_IOHC_R_SMN_SION_S1_CLIREQ_TIME_LOW	0x14438
#define	MILAN_IOHC_R_SMN_SION_S1_CLIREQ_TIME_HI		0x1443c

#define	MILAN_IOHC_R_SMN_SION_S1_RDRSP_BURST_LOW	0x14440
#define	MILAN_IOHC_R_SMN_SION_S1_RDRSP_BURST_HI		0x14444
#define	MILAN_IOHC_R_SMN_SION_S1_RDRSP_TIME_LOW		0x14448
#define	MILAN_IOHC_R_SMN_SION_S1_RDRSP_TIME_HI		0x1444c

#define	MILAN_IOHC_R_SMN_SION_S1_WRRSP_BURST_LOW	0x14450
#define	MILAN_IOHC_R_SMN_SION_S1_WRRSP_BURST_HI		0x14454
#define	MILAN_IOHC_R_SMN_SION_S1_WRRSP_TIME_LOW		0x14458
#define	MILAN_IOHC_R_SMN_SION_S1_WRRSP_TIME_HI		0x1445c

#define	MILAN_IOHC_R_SION_MAX_ENTS	7
#define	MILAN_IOHC_R_SION_SHIFT(x)	((x) * 404)

#define	MILAN_IOHC_R_SION_CLIREQ_BURST_VAL	0x08080808
#define	MILAN_IOHC_R_SION_CLIREQ_TIME_0_2_VAL	0x21212121
#define	MILAN_IOHC_R_SION_CLIREQ_TIME_3_4_VAL	0x84218421
#define	MILAN_IOHC_R_SION_CLIREQ_TIME_5_VAL	0x85218521
#define	MILAN_IOHC_R_SION_RDRSP_BURST_VAL	0x02020202

/*
 * IOHC::IOHC_SION_S1_CLIENT_NP_ReqDeficitThreshold only has a single instance
 * and IOHC::IOHC_SION_S0_CLIENT_NP_ReqDeficitThreshold actually starts at
 * instance 1, there is no instance 0.
 */
#define	MILAN_IOHC_R_SMN_SION_S1_CLI_NP_DEFICIT	0x14480
#define	MILAN_IOHC_R_SMN_SION_S0_CLI_NP_DEFICIT	0x14484
#define	MILAN_IOHC_R_SET_SION_CLI_NP_DEFICIT(r, v)	bitset32(r, 7, 0, v)
#define	MILAN_IOHC_R_SION_CLI_NP_DEFICIT_VAL	0x40
#define	MILAN_IOHC_R_SION_NP_DEFICIT_SHIFT(x)	((x - 1) * 404)

/*
 * IOHC::IOHC_SION_LiveLock_WatchDog_Threshold. This is used to set an
 * arbitration threshold for the overall bus.
 */
#define	MILAN_IOHC_R_SMN_SION_LLWD_THRESH	0x15c9c
#define	MILAN_IOHC_R_SET_SION_LLWD_THRESH_THRESH(r, v)	bitset32(r, 7, 0, v)
#define	MILAN_IOHC_R_SION_LLWD_THRESH_VAL	0x11

/*
 * IOHC::MISC_RAS_CONTROL.  Controls the effects of RAS events, including
 * interrupt generation and PCIe link disable.  Also controls whether the
 * NMI_SYNCFLOOD_L pin is enabled at all.
 */
#define	MILAN_IOHC_R_SMN_MISC_RAS_CTL		0x201d0
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_SW_NMI_EN(r)	bitx32(r, 17, 17)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_SW_NMI_EN(r, v)	bitset32(r, 17, 17, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_SW_SMI_EN(r)	bitx32(r, 16, 16)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_SW_SMI_EN(r, v)	bitset32(r, 16, 16, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_SW_SCI_EN(r)	bitx32(r, 15, 15)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_SW_SCI_EN(r, v)	bitset32(r, 15, 15, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_PCIE_SMI_EN(r)	bitx32(r, 14, 14)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_PCIE_SMI_EN(r, v)	bitset32(r, 14, 14, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_PCIE_SCI_EN(r)	bitx32(r, 13, 13)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_PCIE_SCI_EN(r, v)	bitset32(r, 13, 13, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_PCIE_NMI_EN(r)	bitx32(r, 12, 12)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_PCIE_NMI_EN(r, v)	bitset32(r, 12, 12, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_SYNCFLOOD_DIS(r)	bitx32(r, 11, 11)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_SYNCFLOOD_DIS(r, v)	\
    bitset32(r, 11, 11, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_LINKDIS_DIS(r)	bitx32(r, 10, 10)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_LINKDIS_DIS(r, v)	bitset32(r, 10, 10, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_INTR_DIS(r)	bitx32(r, 9, 9)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_INTR_DIS(r, v)	bitset32(r, 9, 9, v)
#define	MILAN_IOHC_R_MISC_RAS_CTL_GET_NMI_SYNCFLOOD_EN(r)	\
    bitx32(r, 2, 2)
#define	MILAN_IOHC_R_MISC_RAS_CTL_SET_NMI_SYNCFLOOD_EN(r, v)	\
    bitset32(r, 2, 2, v)

/*
 * IOHC Device specific addresses. There are a region of IOHC addresses that are
 * devoted to each PCIe bridge, NBIF, and the southbridge.
 */
#define	MILAN_IOHC_R_SMN_PCIE_BASE	0x31000
#define	MILAN_SMN_IOHC_PCIE_BASE_BITS	(MILAN_SMN_ADDR_BLOCK_BITS + 10)
#define	MILAN_SMN_IOHC_PCIE_MAKE_ADDR(_b, _r)	\
	MILAN_SMN_MAKE_ADDR(_b, MILAN_SMN_IOHC_PCIE_BASE_BITS, _r)

/*
 * IOHC::IOHC_Bridge_CNTL. This register controls several internal properties of
 * the various bridges.  The address of this register is confusing because it
 * shows up in different locations with a large number of instances at different
 * bases. There is an instance for each PCIe root port in the system and then
 * one for each NBIF integrated root complex (note NBIF2 does not have one of
 * these). There is also one for the southbridge/fch.
 */
#define	MILAN_IOHC_R_SMN_BRIDGE_CNTL_PCIE	0x4
#define	MILAN_IOHC_R_SMN_BRIDGE_CNTL_BRIDGE_SHIFT(x)	((x) << 10)
#define	MILAN_IOHC_R_SMN_BRIDGE_CNTL_NBIF	0x38004
#define	MILAN_IOHC_R_SMN_BRIDGE_CNTL_NBIF_SHIFT(x)	((x) << 12)
#define	MILAN_IOHC_R_SMN_BRIDGE_CNTL_SB		0x3c004
#define	MILAN_IOHC_R_BRIDGE_CNTL_GET_APIC_RANGE(r)	bitx32(r, 31, 24)
#define	MILAN_IOHC_R_BRIDGE_CNTL_GET_APIC_ENABLE(r)	bitx32(r, 23, 23)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_CRS_ENABLE(r, v)	bitset32(r, 18, 18, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_IDO_MODE(r, v)	bitset32(r, 11, 10, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_IDO_MODE_NO_MOD		0
#define	MILAN_IOHC_R_BRIDGE_CNTL_IDO_MODE_DIS			1
#define	MILAN_IOHC_R_BRIDGE_CNTL_IDO_MODE_FORCE_ON		2
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_FORCE_RSP_PASS(r, v)	\
    bitset32(r, 9, 9, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_DISABLE_NO_SNOOP(r, v)	\
    bitset32(r, 8, 8, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_DISABLE_RELAX_POW(r, v)	\
    bitset32(r, 7, 7, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_MASK_UR(r, v)		\
    bitset32(r, 6, 6, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_DISABLE_CFG(r, v)		\
    bitset32(r, 2, 2, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_DISABLE_BUS_MASTER(r, v)	\
    bitset32(r, 1, 1, v)
#define	MILAN_IOHC_R_BRIDGE_CNTL_SET_BRIDGE_DISABLE(r, v)	\
    bitset32(r, 0, 0, v)

/*
 * IOAGR Registers. The SMN based addresses are all relative to the IOAGR base
 * address.
 */

/*
 * IOAGR::IOAGR_EARLY_WAKE_UP_EN. This register controls the ability to interact
 * with the clocks and DMA. Specifics unclear. Companion to the IOHC variant.
 */
#define	MILAN_IOAGR_R_SMN_EARLY_WAKE_UP		0x00090
#define	MILAN_IOAGR_R_SET_EARLY_WAKE_UP_HOST_ENABLE(r, v)	\
    bitset32(r, 31, 16, v)
#define	MILAN_IOAGR_R_SET_EARLY_WAKE_UP_DMA_ENABLE(r, v)	\
    bitset32(r, 0, 0, v)

/*
 * IOAGR::IOAGR_SION_S0_Client_Req_BurstTarget_Lower. While the case has
 * changed and the number of entries from our friends in the IOHC, everything
 * said above is still true.
 */
#define	MILAN_IOAGR_R_SMN_SION_S0_CLIREQ_BURST_LOW	0x00400
#define	MILAN_IOAGR_R_SMN_SION_S0_CLIREQ_BURST_HI	0x00404
#define	MILAN_IOAGR_R_SMN_SION_S0_CLIREQ_TIME_LOW	0x00408
#define	MILAN_IOAGR_R_SMN_SION_S0_CLIREQ_TIME_HI	0x0040c

#define	MILAN_IOAGR_R_SMN_SION_S0_RDRSP_BURST_LOW	0x00410
#define	MILAN_IOAGR_R_SMN_SION_S0_RDRSP_BURST_HI	0x00414
#define	MILAN_IOAGR_R_SMN_SION_S0_RDRSP_TIME_LOW	0x00418
#define	MILAN_IOAGR_R_SMN_SION_S0_RDRSP_TIME_HI		0x0041c

#define	MILAN_IOAGR_R_SMN_SION_S0_WRRSP_BURST_LOW	0x00420
#define	MILAN_IOAGR_R_SMN_SION_S0_WRRSP_BURST_HI	0x00424
#define	MILAN_IOAGR_R_SMN_SION_S0_WRRSP_TIME_LOW	0x00428
#define	MILAN_IOAGR_R_SMN_SION_S0_WRRSP_TIME_HI		0x0042c

#define	MILAN_IOAGR_R_SMN_SION_S1_CLIREQ_BURST_LOW	0x00430
#define	MILAN_IOAGR_R_SMN_SION_S1_CLIREQ_BURST_HI	0x00434
#define	MILAN_IOAGR_R_SMN_SION_S1_CLIREQ_TIME_LOW	0x00438
#define	MILAN_IOAGR_R_SMN_SION_S1_CLIREQ_TIME_HI	0x0043c

#define	MILAN_IOAGR_R_SMN_SION_S1_RDRSP_BURST_LOW	0x00440
#define	MILAN_IOAGR_R_SMN_SION_S1_RDRSP_BURST_HI	0x00444
#define	MILAN_IOAGR_R_SMN_SION_S1_RDRSP_TIME_LOW	0x00448
#define	MILAN_IOAGR_R_SMN_SION_S1_RDRSP_TIME_HI		0x0044c

#define	MILAN_IOAGR_R_SMN_SION_S1_WRRSP_BURST_LOW	0x00450
#define	MILAN_IOAGR_R_SMN_SION_S1_WRRSP_BURST_HI	0x00454
#define	MILAN_IOAGR_R_SMN_SION_S1_WRRSP_TIME_LOW	0x00458
#define	MILAN_IOAGR_R_SMN_SION_S1_WRRSP_TIME_HI		0x0045c

#define	MILAN_IOAGR_R_SION_MAX_ENTS	5
#define	MILAN_IOAGR_R_SION_SHIFT(x)	((x) * 400)

#define	MILAN_IOAGR_R_SION_CLIREQ_BURST_VAL	0x08080808
#define	MILAN_IOAGR_R_SION_CLIREQ_TIME_0_2_VAL	0x21212121
#define	MILAN_IOAGR_R_SION_CLIREQ_TIME_3_VAL	0x84218421
#define	MILAN_IOAGR_R_SION_RDRSP_BURST_VAL	0x02020202

/*
 * IOAGR::IOAGR_SION_LiveLock_WatchDog_Threshold. This is used to set an
 * arbitration threshold for the IOAGR. Companion to the IOHC variant.
 */
#define	MILAN_IOAGR_R_SMN_SION_LLWD_THRESH		0x01498
#define	MILAN_IOAGR_R_SET_SION_LLWD_THRESH_THRESH(r, v)	bitset32(r, 7, 0, v)
#define	MILAN_IOAGR_R_SION_LLWD_THRESH_VAL	0x11

/*
 * SDPMUX registers of interest.
 */

/*
 * SDPMUX::SDPMUX_SDP_PORT_CONTROL. More Clock request bits in the spirit of
 * other blocks.
 */
#define	MILAN_SDPMUX_R_SMN_SDP_PORT_CONTROL		0x00008
#define	MILAN_SDPMUX_R_SET_SDP_PORT_CONTROL_HOST_ENABLE(r, v)		\
    bitset32(r, 31, 16, v)
#define	MILAN_SDPMUX_R_SET_SDP_PORT_CONTROL_DMA_ENABLE(r, v)		\
    bitset32(r, 15, 15, v)
#define	MILAN_SDPMUX_R_SET_SDP_PORT_CONTROL_PORT_HYSTERESIS(r, v)	\
    bitset32(r, 7, 0, v)

/*
 * SDPMUX::SDPMUX_SION_LiveLock_WatchDog_Threshold. This is used to set an
 * arbitration threshold for the SDPMUX. Companion to the IOHC variant.
 */
#define	MILAN_SDPMUX_R_SMN_SION_LLWD_THRESH		0x01498
#define	MILAN_SDPMUX_R_SET_SION_LLWD_THRESH_THRESH(r, v)	\
    bitset32(r, 7, 0, v)
#define	MILAN_SDPMUX_R_SION_LLWD_THRESH_VAL	0x11

/*
 * SDPMUX::SDPMUX_SION_S0_Client_Req_BurstTarget_Lower. While the case has
 * changed and the number of entries from our friends in the IOHC, everything
 * said above is still true.
 */
#define	MILAN_SDPMUX_R_SMN_SION_S0_CLIREQ_BURST_LOW	0x00400
#define	MILAN_SDPMUX_R_SMN_SION_S0_CLIREQ_BURST_HI	0x00404
#define	MILAN_SDPMUX_R_SMN_SION_S0_CLIREQ_TIME_LOW	0x00408
#define	MILAN_SDPMUX_R_SMN_SION_S0_CLIREQ_TIME_HI	0x0040c

#define	MILAN_SDPMUX_R_SMN_SION_S0_RDRSP_BURST_LOW	0x00410
#define	MILAN_SDPMUX_R_SMN_SION_S0_RDRSP_BURST_HI	0x00414
#define	MILAN_SDPMUX_R_SMN_SION_S0_RDRSP_TIME_LOW	0x00418
#define	MILAN_SDPMUX_R_SMN_SION_S0_RDRSP_TIME_HI	0x0041c

#define	MILAN_SDPMUX_R_SMN_SION_S0_WRRSP_BURST_LOW	0x00420
#define	MILAN_SDPMUX_R_SMN_SION_S0_WRRSP_BURST_HI	0x00424
#define	MILAN_SDPMUX_R_SMN_SION_S0_WRRSP_TIME_LOW	0x00428
#define	MILAN_SDPMUX_R_SMN_SION_S0_WRRSP_TIME_HI	0x0042c

#define	MILAN_SDPMUX_R_SMN_SION_S1_CLIREQ_BURST_LOW	0x00430
#define	MILAN_SDPMUX_R_SMN_SION_S1_CLIREQ_BURST_HI	0x00434
#define	MILAN_SDPMUX_R_SMN_SION_S1_CLIREQ_TIME_LOW	0x00438
#define	MILAN_SDPMUX_R_SMN_SION_S1_CLIREQ_TIME_HI	0x0043c

#define	MILAN_SDPMUX_R_SMN_SION_S1_RDRSP_BURST_LOW	0x00440
#define	MILAN_SDPMUX_R_SMN_SION_S1_RDRSP_BURST_HI	0x00444
#define	MILAN_SDPMUX_R_SMN_SION_S1_RDRSP_TIME_LOW	0x00448
#define	MILAN_SDPMUX_R_SMN_SION_S1_RDRSP_TIME_HI	0x0044c

#define	MILAN_SDPMUX_R_SMN_SION_S1_WRRSP_BURST_LOW	0x00450
#define	MILAN_SDPMUX_R_SMN_SION_S1_WRRSP_BURST_HI	0x00454
#define	MILAN_SDPMUX_R_SMN_SION_S1_WRRSP_TIME_LOW	0x00458
#define	MILAN_SDPMUX_R_SMN_SION_S1_WRRSP_TIME_HI	0x0045c

#define	MILAN_SDPMUX_R_SION_MAX_ENTS	5
#define	MILAN_SDPMUX_R_SION_SHIFT(x)	((x) * 400)

#define	MILAN_SDPMUX_R_SION_CLIREQ_BURST_VAL	0x08080808
#define	MILAN_SDPMUX_R_SION_CLIREQ_TIME_VAL	0x21212121
#define	MILAN_SDPMUX_R_SION_RDRSP_BURST_VAL	0x02020202

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_IOHC_H */
