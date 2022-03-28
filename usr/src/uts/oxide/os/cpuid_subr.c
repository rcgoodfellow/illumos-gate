/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2012 Nexenta Systems, Inc. All rights reserved.
 */

/*
 * Portions Copyright 2009 Advanced Micro Devices, Inc.
 */

/*
 * Copyright 2012 Jens Elkner <jel+illumos@cs.uni-magdeburg.de>
 * Copyright 2012 Hans Rosenfeld <rosenfeld@grumpf.hope-2000.org>
 * Copyright 2019 Joyent, Inc.
 * Copyright 2022 Oxide Computer Company
 */

/*
 * Support functions that interpret CPUID and similar information.
 * These should not be used from anywhere other than cpuid.c and
 * cmi_hw.c - as such we will not list them in any header file
 * such as x86_archext.h.
 *
 * In cpuid.c we process CPUID information for each cpu_t instance
 * we're presented with, and stash this raw information and material
 * derived from it in per-cpu_t structures.
 *
 * If we are virtualized then the CPUID information derived from CPUID
 * instructions executed in the guest is based on whatever the hypervisor
 * wanted to make things look like, and the cpu_t are not necessarily in 1:1
 * or fixed correspondence with real processor execution resources.  In cmi_hw.c
 * we are interested in the native properties of a processor - for fault
 * management (and potentially other, such as power management) purposes;
 * it will tunnel through to real hardware information, and use the
 * functionality provided in this file to process it.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/bitmap.h>
#include <sys/x86_archext.h>
#include <sys/pci_cfgspace.h>
#include <sys/sysmacros.h>

/*
 * AMD socket types.
 * First index :
 *		0 for family 0xf, revs B thru E
 *		1 for family 0xf, revs F and G
 *		2 for family 0x10
 *		3 for family 0x11
 *		4 for family 0x12
 *		5 for family 0x14
 *		6 for family 0x15, models 00 - 0f
 *		7 for family 0x15, models 10 - 1f
 *		8 for family 0x15, models 30 - 3f
 *		9 for family 0x15, models 60 - 6f
 *		10 for family 0x15, models 70 - 7f
 *		11 for family 0x16, models 00 - 0f
 *		12 for family 0x16, models 30 - 3f
 *		13 for family 0x17, models 00 - 0f
 *		14 for family 0x17, models 10 - 2f
 *		15 for family 0x17, models 30 - 3f
 *		16 for family 0x17, models 60 - 6f
 *		17 for family 0x17, models 70 - 7f
 *		18 for family 0x18, models 00 - 0f
 *		19 for family 0x19, models 00 - 0f
 *		20 for family 0x19, models 20 - 2f
 * Second index by (model & 0x3) for family 0fh,
 * CPUID pkg bits (Fn8000_0001_EBX[31:28]) for later families.
 */
static uint32_t amd_skts[21][8] = {
	/*
	 * Family 0xf revisions B through E
	 */
#define	A_SKTS_0			0
	{
		X86_SOCKET_754,		/* 0b000 */
		X86_SOCKET_940,		/* 0b001 */
		X86_SOCKET_754,		/* 0b010 */
		X86_SOCKET_939,		/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},
	/*
	 * Family 0xf revisions F and G
	 */
#define	A_SKTS_1			1
	{
		X86_SOCKET_S1g1,	/* 0b000 */
		X86_SOCKET_F1207,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_AM2,		/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},
	/*
	 * Family 0x10
	 */
#define	A_SKTS_2			2
	{
		X86_SOCKET_F1207,	/* 0b000 */
		X86_SOCKET_AM2R2,	/* 0b001 */
		X86_SOCKET_S1g3,	/* 0b010 */
		X86_SOCKET_G34,		/* 0b011 */
		X86_SOCKET_ASB2,	/* 0b100 */
		X86_SOCKET_C32,		/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x11
	 */
#define	A_SKTS_3			3
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_S1g2,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x12
	 */
#define	A_SKTS_4			4
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_FS1,		/* 0b001 */
		X86_SOCKET_FM1,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x14
	 */
#define	A_SKTS_5			5
	{
		X86_SOCKET_FT1,		/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x15 models 00 - 0f
	 */
#define	A_SKTS_6			6
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_AM3R2,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_G34,		/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_C32,		/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x15 models 10 - 1f
	 */
#define	A_SKTS_7			7
	{
		X86_SOCKET_FP2,		/* 0b000 */
		X86_SOCKET_FS1R2,	/* 0b001 */
		X86_SOCKET_FM2,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x15 models 30-3f
	 */
#define	A_SKTS_8			8
	{
		X86_SOCKET_FP3,		/* 0b000 */
		X86_SOCKET_FM2R2,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x15 models 60-6f
	 */
#define	A_SKTS_9			9
	{
		X86_SOCKET_FP4,		/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_FM2R2,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x15 models 70-7f
	 */
#define	A_SKTS_10			10
	{
		X86_SOCKET_FP4,		/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_FT4,		/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x16 models 00-0f
	 */
#define	A_SKTS_11			11
	{
		X86_SOCKET_FT3,		/* 0b000 */
		X86_SOCKET_FS1B,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x16 models 30-3f
	 */
#define	A_SKTS_12			12
	{
		X86_SOCKET_FT3B,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_FP4,		/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x17 models 00-0f	(Zen 1 - Naples, Ryzen)
	 */
#define	A_SKTS_13			13
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_SP3,		/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_SP3R2	/* 0b111 */
	},

	/*
	 * Family 0x17 models 10-2f	(Zen 1 - APU: Raven Ridge)
	 *				(Zen 1 - APU: Banded Kestrel)
	 *				(Zen 1 - APU: Dali)
	 */
#define	A_SKTS_14			14
	{
		X86_SOCKET_FP5,		/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x17 models 30-3f	(Zen 2 - Rome)
	 */
#define	A_SKTS_15			15
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_SP3,		/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_SP3R2	/* 0b111 */
	},

	/*
	 * Family 0x17 models 60-6f	(Zen 2 - Renoir)
	 */
#define	A_SKTS_16			16
	{
		X86_SOCKET_FP6,		/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x17 models 70-7f	(Zen 2 - Matisse)
	 */
#define	A_SKTS_17			17
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	},

	/*
	 * Family 0x18 models 00-0f	(Dhyana)
	 */
#define	A_SKTS_18			18
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_SL1,		/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_DM1,		/* 0b110 */
		X86_SOCKET_SL1R2	/* 0b111 */
	},

	/*
	 * Family 0x19 models 00-0f	(Zen 3 - Milan)
	 */
#define	A_SKTS_19			19
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_UNKNOWN,	/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_SP3,		/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_STRX4	/* 0b111 */
	},

	/*
	 * Family 0x19 models 20-2f	(Zen 3 - Vermeer)
	 */
#define	A_SKTS_20			20
	{
		X86_SOCKET_UNKNOWN,	/* 0b000 */
		X86_SOCKET_UNKNOWN,	/* 0b001 */
		X86_SOCKET_AM4,		/* 0b010 */
		X86_SOCKET_UNKNOWN,	/* 0b011 */
		X86_SOCKET_UNKNOWN,	/* 0b100 */
		X86_SOCKET_UNKNOWN,	/* 0b101 */
		X86_SOCKET_UNKNOWN,	/* 0b110 */
		X86_SOCKET_UNKNOWN	/* 0b111 */
	}
};

struct amd_sktmap_s {
	uint32_t	skt_code;
	char		sktstr[16];
};
static struct amd_sktmap_s amd_sktmap_strs[X86_NUM_SOCKETS + 1] = {
	{ X86_SOCKET_754,	"754" },
	{ X86_SOCKET_939,	"939" },
	{ X86_SOCKET_940,	"940" },
	{ X86_SOCKET_S1g1,	"S1g1" },
	{ X86_SOCKET_AM2,	"AM2" },
	{ X86_SOCKET_F1207,	"F(1207)" },
	{ X86_SOCKET_S1g2,	"S1g2" },
	{ X86_SOCKET_S1g3,	"S1g3" },
	{ X86_SOCKET_AM,	"AM" },
	{ X86_SOCKET_AM2R2,	"AM2r2" },
	{ X86_SOCKET_AM3,	"AM3" },
	{ X86_SOCKET_G34,	"G34" },
	{ X86_SOCKET_ASB2,	"ASB2" },
	{ X86_SOCKET_C32,	"C32" },
	{ X86_SOCKET_FT1,	"FT1" },
	{ X86_SOCKET_FM1,	"FM1" },
	{ X86_SOCKET_FS1,	"FS1" },
	{ X86_SOCKET_AM3R2,	"AM3r2" },
	{ X86_SOCKET_FP2,	"FP2" },
	{ X86_SOCKET_FS1R2,	"FS1r2" },
	{ X86_SOCKET_FM2,	"FM2" },
	{ X86_SOCKET_FP3,	"FP3" },
	{ X86_SOCKET_FM2R2,	"FM2r2" },
	{ X86_SOCKET_FP4,	"FP4" },
	{ X86_SOCKET_AM4,	"AM4" },
	{ X86_SOCKET_FT3,	"FT3" },
	{ X86_SOCKET_FT4,	"FT4" },
	{ X86_SOCKET_FS1B,	"FS1b" },
	{ X86_SOCKET_FT3B,	"FT3b" },
	{ X86_SOCKET_SP3,	"SP3" },
	{ X86_SOCKET_SP3R2,	"SP3r2" },
	{ X86_SOCKET_FP5,	"FP5" },
	{ X86_SOCKET_FP6,	"FP6" },
	{ X86_SOCKET_STRX4,	"sTRX4" },
	{ X86_SOCKET_SL1,	"SL1" },
	{ X86_SOCKET_SL1R2,	"SL1R2" },
	{ X86_SOCKET_DM1,	"DM1" },
	{ X86_SOCKET_UNKNOWN,	"Unknown" }
};

static const struct amd_skt_mapent {
	uint_t sm_family;
	uint_t sm_modello;
	uint_t sm_modelhi;
	uint_t sm_sktidx;
} amd_sktmap[] = {
	{ 0x10, 0x00, 0xff, A_SKTS_2 },
	{ 0x11, 0x00, 0xff, A_SKTS_3 },
	{ 0x12, 0x00, 0xff, A_SKTS_4 },
	{ 0x14, 0x00, 0x0f, A_SKTS_5 },
	{ 0x15, 0x00, 0x0f, A_SKTS_6 },
	{ 0x15, 0x10, 0x1f, A_SKTS_7 },
	{ 0x15, 0x30, 0x3f, A_SKTS_8 },
	{ 0x15, 0x60, 0x6f, A_SKTS_9 },
	{ 0x15, 0x70, 0x7f, A_SKTS_10 },
	{ 0x16, 0x00, 0x0f, A_SKTS_11 },
	{ 0x16, 0x30, 0x3f, A_SKTS_12 },
	{ 0x17, 0x00, 0x0f, A_SKTS_13 },
	{ 0x17, 0x10, 0x2f, A_SKTS_14 },
	{ 0x17, 0x30, 0x3f, A_SKTS_15 },
	{ 0x17, 0x60, 0x6f, A_SKTS_16 },
	{ 0x17, 0x70, 0x7f, A_SKTS_17 },
	{ 0x18, 0x00, 0x0f, A_SKTS_18 },
	{ 0x19, 0x00, 0x0f, A_SKTS_19 },
	{ 0x19, 0x20, 0x2f, A_SKTS_20 }
};

/*
 * Table for mapping AMD Family 0xf and AMD Family 0x10 model/stepping
 * combination to chip "revision" and socket type.
 *
 * The first member of this array that matches a given family, extended model
 * plus model range, and stepping range will be considered a match.
 */
static const struct amd_rev_mapent {
	uint_t rm_family;
	uint_t rm_modello;
	uint_t rm_modelhi;
	uint_t rm_steplo;
	uint_t rm_stephi;
	uint32_t rm_chiprev;
	const char *rm_chiprevstr;
	uint_t rm_sktidx;
} amd_revmap[] = {
	/*
	 * =============== AuthenticAMD Family 0x17 ===============
	 */
	{ 0x17, 0x01, 0x01, 0x1, 0x1, X86_CHIPREV_AMD_17_ZP_B1, "ZP-B1",
	    A_SKTS_13 },
	{ 0x17, 0x01, 0x01, 0x2, 0x2, X86_CHIPREV_AMD_17_ZP_B2, "ZP-B2",
	    A_SKTS_13 },
	{ 0x17, 0x01, 0x01, 0x1, 0x1, X86_CHIPREV_AMD_17_PiR_B2, "PiR-B2",
	    A_SKTS_13 },

	{ 0x17, 0x11, 0x11, 0x0, 0x0, X86_CHIPREV_AMD_17_RV_B0, "RV-B0",
	    A_SKTS_14 },
	{ 0x17, 0x11, 0x11, 0x1, 0x1, X86_CHIPREV_AMD_17_RV_B1, "RV-B1",
	    A_SKTS_14 },
	{ 0x17, 0x18, 0x18, 0x1, 0x1, X86_CHIPREV_AMD_17_PCO_B1, "PCO-B1",
	    A_SKTS_14 },

	{ 0x17, 0x30, 0x30, 0x0, 0x0, X86_CHIPREV_AMD_17_SSP_A0, "SSP-A0",
	    A_SKTS_15 },
	{ 0x17, 0x31, 0x31, 0x0, 0x0, X86_CHIPREV_AMD_17_SSP_B0, "SSP-B0",
	    A_SKTS_15 },

	{ 0x17, 0x71, 0x71, 0x0, 0x0, X86_CHIPREV_AMD_17_MTS_B0, "MTS-B0",
	    A_SKTS_17 },

	/*
	 * =============== AuthenticAMD Family 0x19 ===============
	 */
	{ 0x19, 0x00, 0x00, 0x0, 0x0, X86_CHIPREV_AMD_19_GN_A0, "GN-A0",
	    A_SKTS_19 },
	{ 0x19, 0x01, 0x01, 0x0, 0x0, X86_CHIPREV_AMD_19_GN_B0, "GN-B0",
	    A_SKTS_19 },
	{ 0x19, 0x01, 0x01, 0x1, 0x1, X86_CHIPREV_AMD_19_GN_B1, "GN-B1",
	    A_SKTS_19 },
	{ 0x19, 0x01, 0x01, 0x2, 0x2, X86_CHIPREV_AMD_19_GN_B2, "GN-B2",
	    A_SKTS_19 },
};

/*
 * AMD keeps the socket type in CPUID Fn8000_0001_EBX, bits 31:28.
 */
static uint32_t
synth_amd_skt_cpuid(uint_t family, uint_t sktid)
{
	struct cpuid_regs cp;
	uint_t idx;

	cp.cp_eax = 0x80000001;
	(void) __cpuid_insn(&cp);

	/* PkgType bits */
	idx = BITX(cp.cp_ebx, 31, 28);

	if (idx > 7) {
		return (X86_SOCKET_UNKNOWN);
	}

	return (amd_skts[sktid][idx]);
}

static void
synth_amd_skt(uint_t family, uint_t model, uint32_t *skt_p)
{
	const struct amd_skt_mapent *skt;
	uint_t i;

	if (skt_p == NULL || family < 0xf)
		return;

	for (i = 0, skt = amd_sktmap; i < ARRAY_SIZE(amd_sktmap);
	    i++, skt++) {
		if (family == skt->sm_family &&
		    model >= skt->sm_modello && model <= skt->sm_modelhi) {
			*skt_p = synth_amd_skt_cpuid(family, skt->sm_sktidx);
		}
	}
}

static void
synth_amd_info(uint_t family, uint_t model, uint_t step,
    uint32_t *skt_p, uint32_t *chiprev_p, const char **chiprevstr_p)
{
	const struct amd_rev_mapent *rmp;
	int found = 0;
	int i;

	if (family < 0xf)
		return;

	for (i = 0, rmp = amd_revmap; i < ARRAY_SIZE(amd_revmap); i++, rmp++) {
		if (family == rmp->rm_family &&
		    model >= rmp->rm_modello && model <= rmp->rm_modelhi &&
		    step >= rmp->rm_steplo && step <= rmp->rm_stephi) {
			found = 1;
			break;
		}
	}

	if (!found) {
		synth_amd_skt(family, model, skt_p);
		return;
	}

	if (chiprev_p != NULL)
		*chiprev_p = rmp->rm_chiprev;
	if (chiprevstr_p != NULL)
		*chiprevstr_p = rmp->rm_chiprevstr;

	if (skt_p != NULL) {
		*skt_p = synth_amd_skt_cpuid(family, rmp->rm_sktidx);
	}
}

uint32_t
_cpuid_skt(uint_t vendor, uint_t family, uint_t model, uint_t step)
{
	uint32_t skt = X86_SOCKET_UNKNOWN;

	synth_amd_info(family, model, step, &skt, NULL, NULL);
	return (skt);
}

const char *
_cpuid_sktstr(uint_t vendor, uint_t family, uint_t model, uint_t step)
{
	const char *sktstr = "Unknown";
	struct amd_sktmap_s *sktmapp;
	uint32_t skt = X86_SOCKET_UNKNOWN;

	synth_amd_info(family, model, step, &skt, NULL, NULL);

	sktmapp = amd_sktmap_strs;
	while (sktmapp->skt_code != X86_SOCKET_UNKNOWN) {
		if (sktmapp->skt_code == skt)
			break;
		sktmapp++;
	}
	sktstr = sktmapp->sktstr;

	return (sktstr);
}

uint32_t
_cpuid_chiprev(uint_t vendor, uint_t family, uint_t model, uint_t step)
{
	uint32_t chiprev = X86_CHIPREV_UNKNOWN;

	synth_amd_info(family, model, step, NULL, &chiprev, NULL);
	return (chiprev);
}

const char *
_cpuid_chiprevstr(uint_t vendor, uint_t family, uint_t model, uint_t step)
{
	const char *revstr = "Unknown";

	synth_amd_info(family, model, step, NULL, NULL, &revstr);
	return (revstr);
}

/*
 * CyrixInstead is a variable used by the Cyrix detection code
 * in locore.
 */
const char CyrixInstead[] = X86_VENDORSTR_CYRIX;

/*
 * Map the vendor string to a type code
 */
uint_t
_cpuid_vendorstr_to_vendorcode(char *vendorstr)
{
#if 0
	if (strcmp(vendorstr, X86_VENDORSTR_Intel) == 0)
		return (X86_VENDOR_Intel);
	else if (strcmp(vendorstr, X86_VENDORSTR_AMD) == 0)
		return (X86_VENDOR_AMD);
	else if (strcmp(vendorstr, X86_VENDORSTR_HYGON) == 0)
		return (X86_VENDOR_HYGON);
	else if (strcmp(vendorstr, X86_VENDORSTR_TM) == 0)
		return (X86_VENDOR_TM);
	else if (strcmp(vendorstr, CyrixInstead) == 0)
		return (X86_VENDOR_Cyrix);
	else if (strcmp(vendorstr, X86_VENDORSTR_UMC) == 0)
		return (X86_VENDOR_UMC);
	else if (strcmp(vendorstr, X86_VENDORSTR_NexGen) == 0)
		return (X86_VENDOR_NexGen);
	else if (strcmp(vendorstr, X86_VENDORSTR_Centaur) == 0)
		return (X86_VENDOR_Centaur);
	else if (strcmp(vendorstr, X86_VENDORSTR_Rise) == 0)
		return (X86_VENDOR_Rise);
	else if (strcmp(vendorstr, X86_VENDORSTR_SiS) == 0)
		return (X86_VENDOR_SiS);
	else if (strcmp(vendorstr, X86_VENDORSTR_NSC) == 0)
		return (X86_VENDOR_NSC);
	else
		return (X86_VENDOR_IntelClone);
#endif
	return (X86_VENDOR_AMD);
}
