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
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2022 Oxide Computer Co.
 */

#include <sys/bitext.h>
#include <sys/x86_archext.h>
#include "intr_common.h"

#ifdef _KMDB

#include <kmdb/kmdb_modext.h>

typedef struct ioapic_walk_state {
	uint32_t *iws_addrs[MAX_IO_APIC];
	uint32_t iws_count;
} ioapic_walk_state_t;

/* Macros for reading/writing the IOAPIC RDT entries */
#define	IOAPIC_READ_RDT_LO(ira, ipin) \
	ioapic_read_reg(ira, APIC_RDT_CMD + (2 * (ipin)))

#define	IOAPIC_READ_RDT_HI(ira, ipin) \
	ioapic_read_reg(ira, APIC_RDT_CMD2 + (2 * (ipin)))

static uint32_t
ioapic_read_reg(uintptr_t ira, uint32_t reg)
{
	volatile uint32_t *irp = (volatile uint32_t *)ira;
	uint32_t save_reg, data;

	save_reg = irp[APIC_IO_REG];
	irp[APIC_IO_REG] = reg;
	data = irp[APIC_IO_DATA];
	irp[APIC_IO_REG] = save_reg;

	return (data);
}

static const char *
modetostr(uint32_t reg)
{
	switch (reg & AV_DELIV_MODE) {
	case AV_FIXED:
		return ("Fixed");
	case AV_LOPRI:
		return ("LoPri");
	case AV_SMI:
		return ("SMI");
	case AV_REMOTE:
		return ("Inval");
	case AV_NMI:
		return ("NMI");
	case AV_RESET:
		return ("INIT");
	case AV_STARTUP:
		return ("Inval");
	case AV_EXTINT:
		return ("Ext");
	default:
		ASSERT(0);
		return ("Inval");
	}
}

#define	APIC_ENT_HDR_COMM	"%10s %-8s %11s %5s %-5s %-5s"
#define	APIC_ENT_HDR_ELEM	"REGVAL", "DESTMODE", "DESTINATION", \
				"VECT", "MODE", "FLAGS"

static void
apic_dump_entry_common(uint32_t reg, boolean_t local, uint32_t dst)
{
	char dstbuf[35];

	if (local)
		(void) mdb_snprintf(dstbuf, sizeof (dstbuf), "-");
	else
		(void) mdb_snprintf(dstbuf, sizeof (dstbuf), "%#11r", dst);

	mdb_printf("%#10x %-8s %11s %#5r %-5s %c%c%c%c%c",
	    reg,
	    local ? "Local" : (reg & AV_LDEST) ? "Logical" : "Physical",
	    dstbuf, RDT_VECTOR(reg),
	    modetostr(reg),
	    (reg & AV_PENDING) ? 'P' : '-',
	    (reg & AV_ACTIVE_LOW) ? '-' : '+',
	    (reg & AV_REMOTE_IRR) ? 'I' : '-',
	    (reg & AV_LEVEL) ? 'L' : 'E',
	    (reg & AV_MASK) ? 'M' : '-');
}

static int
ioapic_show_entries(uintptr_t addr, uint_t flags)
{
	uint32_t vers_reg, id_reg, lastpin, id;
	uint32_t pin;

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%<u>%-?s   %5s %5s " APIC_ENT_HDR_COMM "%</u>\n",
		    "REGS", "ID", "PIN", APIC_ENT_HDR_ELEM);
	}

	id_reg = ioapic_read_reg(addr, APIC_ID_CMD);
	id = bitx32(id_reg, 31, 24);

	vers_reg = ioapic_read_reg(addr, APIC_VERS_CMD);
	lastpin = bitx32(vers_reg, 23, 16);

	for (pin = 0; pin <= lastpin; pin++) {
		uint32_t high, low;
		uint32_t dst;

		high = IOAPIC_READ_RDT_HI(addr, pin);
		low = IOAPIC_READ_RDT_LO(addr, pin);

		mdb_printf("%#-?lx %#5r %#5r ", addr, id, pin);
		dst = bitx32(high, 31, 24);
		apic_dump_entry_common(low, B_FALSE, dst);
		mdb_printf("\n");
	}

	return (DCMD_OK);
}

#define	ENTRY_HELP	\
"%s entries have the following flags:\n"	\
"  P    interrupt is pending on this pin\n"	\
"  +    input pin is active high\n"		\
"  I    level-triggered interrupt has been delivered and not yet serviced\n" \
"  L/E  interrupt is level-triggered/edge-triggered\n"	\
"  M    interrupts from this input pin are masked\n"

static const char *ioapic_help =
"Given an address, print information about the IOAPIC whose registers are\n"
"mapped at that virtual address.  If no address is provided, print that\n"
"information about all IOAPICs known to the kernel.  IOAPICs that are not\n"
"mapped are not included in the output.  A single option is available:\n"
"  -e           dump the contents of all redirection table (RDT) entries\n"
"\n"
"If the -e option is not specified, basic information about the IOAPIC is\n"
"displayed instead.  Output columns may be decoded as follows:\n"
"  REGS         virtual address of the register window\n"
"  ARB          arbitration ID of this IOAPIC\n"
"  PRQ          P = software input pin assertion supported\n"
"  FLAGS        see discussion below of RDT entry flags\n"
"\n"
ENTRY_HELP
"\n"
"This command saves and restores the IOAPIC's index register's contents;\n"
"however, no guarantee can be made that reading the registers is free of\n"
"side effects.  Consult the manual for your IOAPIC implementation.\n";

void
ioapic_dcmd_help(void)
{
	mdb_printf(ioapic_help, "RDT");
}

int
ioapic(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t arg_flags = 0;
	uint32_t id_reg, vers_reg, arb_reg;
	uint32_t id, lastpin, prq, vers, arb_id;

	if (!(flags & DCMD_ADDRSPEC)) {
		if (mdb_pwalk_dcmd("ioapic", "ioapic", argc, argv, 0) != 0) {
			mdb_warn("walking ioapic list failed");
			return (DCMD_ERR);
		}

		return (DCMD_OK);
	}

	if (mdb_getopts(argc, argv,
	    'e', MDB_OPT_SETBITS, IOAPIC_F_SHOW_ENTRIES, &arg_flags,
	    NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((arg_flags & IOAPIC_F_SHOW_ENTRIES) != 0)
		return (ioapic_show_entries(addr, flags));

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%<u>%-?s   %5s %5s %5s %3s %5s%</u>\n",
		    "REGS", "ID", "ARB", "NPIN", "PRQ", "VERS");
	}

	id_reg = ioapic_read_reg(addr, APIC_ID_CMD);
	id = bitx32(id_reg, 31, 24);

	vers_reg = ioapic_read_reg(addr, APIC_VERS_CMD);
	lastpin = bitx32(vers_reg, 23, 16);
	prq = bitx32(vers_reg, 15, 15);
	vers = bitx32(vers_reg, 7, 0);

	arb_reg = ioapic_read_reg(addr, APIC_ARB_CMD);
	arb_id = bitx32(arb_reg, 27, 24);

	mdb_printf("%#-?lx %#5r %#5r %#5r %3s %#5r\n",
	    addr, id, arb_id, lastpin + 1, prq ? "P" : "-", vers);

	return (DCMD_OK);
}

int
ioapic_walk_init(mdb_walk_state_t *wsp)
{
	ioapic_walk_state_t *iwsp;

	if (wsp->walk_addr != 0) {
		mdb_warn("ioapic walker does not support local walks\n");
		return (WALK_ERR);
	}

	iwsp = wsp->walk_data = mdb_zalloc(sizeof (ioapic_walk_state_t),
	    UM_SLEEP | UM_GC);

	if (mdb_readvar(&iwsp->iws_addrs, "apicioadr") == -1) {
		mdb_warn("failed to read apicioadr");
		return (WALK_ERR);
	}

	if (mdb_readvar(&iwsp->iws_count, "apic_io_max") == -1) {
		mdb_warn("failed to read apic_io_max");
		return (WALK_ERR);
	}

	if (iwsp->iws_count == 0)
		return (WALK_DONE);

	wsp->walk_addr = (uintptr_t)0;

	return (WALK_NEXT);
}

int
ioapic_walk_step(mdb_walk_state_t *wsp)
{
	ioapic_walk_state_t *iwsp = wsp->walk_data;

	if (wsp->walk_addr >= iwsp->iws_count)
		return (WALK_DONE);

	return (wsp->walk_callback((uintptr_t)iwsp->iws_addrs[wsp->walk_addr++],
	    NULL, wsp->walk_cbdata));
}

static uint32_t
apic_timer_divide_map(uint32_t reg)
{
	switch (reg) {
	case 0:
	case 1:
	case 2:
	case 3:
		return (1U << (reg + 1));
	case 8:
	case 9:
	case 0xa:
		return (1U << (reg - 3));
	case 0xb:
		return (1);
	default:
		return (0);
	}
}

static int
apic_read(apic_mode_t mode, volatile uint32_t *ap, uint32_t reg, uint64_t *rvp)
{
	switch (mode) {
	case LOCAL_X2APIC:
		return (mdb_x86_rdmsr(REG_X2APIC_BASE_MSR +
		    ((reg >> 2) & 0xffffffff), rvp));
	case LOCAL_APIC:
		*rvp = (uint64_t)(ap[reg]);
		return (DCMD_OK);
	default:
		ASSERT(0);
		return (DCMD_ERR);
	}
}

#define	APIC_READ_REG(_m, _p, _r, _vp)	\
	do {								\
		if (apic_read((_m), (_p), (_r), (_vp)) != DCMD_OK) {	\
			mdb_warn("failed to read " #_r);		\
			return (DCMD_ERR);				\
		}							\
	} while (0)

static const char *apic_help =
"Print information about the current CPU's local APIC, if it is enabled.\n"
"Three options are available to select the information to be displayed;\n"
"any combination may be supplied:\n"
"  -b           show basic information from per-APIC registers\n"
"  -e           dump the contents of all local vector table (LVT) entries\n"
"  -f           show per-vector flag bits (copious)\n"
"\n"
"If no options are supplied, the default output selection is -b.\n"
ENTRY_HELP
"  1    for the timer LVT only, one-shot mode; otherwise, periodic\n"
"\n"
"When displaying flag bits (-f) for each interrupt, a table of the 240\n"
"non-reserved interrupts is displayed in a grid format similar to that output\n"
"by ::dump.  Each interrupt has 3 or 4 associated bits, depending upon\n"
"whether extended APIC functionality is present and includes IER.  These\n"
"flags are as follows:\n"
"  S    interrupt is being serviced by this core\n"
"  L/E  last interrupt accepted was level/edge-triggered\n"
"  R    request has been accepted by this APIC\n"
"  *    interrupt is enabled (IER only)\n";

void
apic_dcmd_help(void)
{
	mdb_printf(apic_help, "LVT");
}

static int
apic_print_flags(apic_mode_t am, volatile uint32_t *papic, boolean_t have_ier)
{
	uint32_t intr;
	uint64_t sr, tr, rr, er;

	/*
	 * We have either 3 or (if IER is supported) 4 bits to show for each
	 * of 240 interrupts.  Therefore if we have IER, we can show only 8
	 * interrupts per line; otherwise, 16.
	 */
	mdb_printf("%<u>VECT");
	if (have_ier) {
		for (intr = 0; intr < 8; intr++)
			mdb_printf(" %4x", intr);
	} else {
		for (intr = 0; intr < 16; intr++)
			mdb_printf(" %3x", intr);
	}
	mdb_printf("%</u>");

	APIC_READ_REG(am, papic, APIC_IN_SVC_BASE_REG, &sr);
	APIC_READ_REG(am, papic, APIC_TM_BASE_REG, &tr);
	APIC_READ_REG(am, papic, APIC_REQUEST_BASE_REG, &rr);
	if (have_ier)
		APIC_READ_REG(am, papic, APIC_EXTD_IER_BASE_REG, &er);

	for (intr = 16; intr < 256; intr++) {
		if ((intr & 31) == 0) {
			APIC_READ_REG(am, papic,
			    APIC_IN_SVC_BASE_REG + (intr >> 3), &sr);
			APIC_READ_REG(am, papic,
			    APIC_TM_BASE_REG + (intr >> 3), &tr);
			APIC_READ_REG(am, papic,
			    APIC_REQUEST_BASE_REG + (intr >> 3), &rr);
			if (have_ier) {
				APIC_READ_REG(am, papic,
				    APIC_EXTD_IER_BASE_REG + (intr >> 3), &er);
			}
		}
		if ((have_ier && ((intr & 7) == 0)) || ((intr & 15) == 0))
			mdb_printf("\n%-#4x", intr);

		mdb_printf(" %c%c%c",
		    (sr & (1UL << (intr & 31))) ? 'S' : '-',
		    (tr & (1UL << (intr & 31))) ? 'L' : 'E',
		    (rr & (1UL << (intr & 31))) ? 'R' : '-');
		if (have_ier) {
			mdb_printf("%c",
			    (er & (1UL << (intr & 31))) ? '*' : '-');
		}
	}

	mdb_printf("\n\n");

	return (DCMD_OK);
}

int
apic(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t arg_flags = 0;
	apic_mode_t am;
	uint64_t apic_bar;
	volatile uint32_t *papic = NULL;
	uint64_t r, rh;
	boolean_t have_extd = B_FALSE;
	boolean_t have_ier = B_FALSE;
	uint32_t nlvt_extd = 0, lvt, dsh;
	uint8_t vers;
	GElf_Sym sym;

	static const mdb_bitmask_t vers_flag_bits[] = {
		{ "D-EOI",	APIC_DIRECTED_EOI_BIT,	APIC_DIRECTED_EOI_BIT },
		{ "EXTD",	APIC_EXTENDED_BIT,	APIC_EXTENDED_BIT },
		{ NULL,		0,			0 }
	};

	static const mdb_bitmask_t spur_flag_bits[] = {
		{ "SWEN",	AV_UNIT_ENABLE,		AV_UNIT_ENABLE },
		{ "FD",		AV_FOCUS_DISABLE,	AV_FOCUS_DISABLE },
		{ NULL,		0,			0 }
	};

	static const mdb_bitmask_t irrs_flag_bits[] = {
		{ "READ-INVALID",	(AV_READ_PENDING | AV_REMOTE_STATUS),
					0 },
		{ "PENDING",		(AV_READ_PENDING | AV_REMOTE_STATUS),
					AV_READ_PENDING },
		{ "COMPLETE",		(AV_READ_PENDING | AV_REMOTE_STATUS),
					AV_REMOTE_STATUS },
		{ "Invalid",		(AV_READ_PENDING | AV_REMOTE_STATUS),
					(AV_READ_PENDING | AV_REMOTE_STATUS) },
		{ NULL,			0,	0 }
	};

	static const mdb_bitmask_t dsh_flag_bits[] = {
		{ "NSH",	AV_SH_ALL_EXCSELF,	0 },
		{ "SELF",	AV_SH_ALL_EXCSELF,	AV_SH_SELF },
		{ "ALL",	AV_SH_ALL_EXCSELF,	AV_SH_ALL_INCSELF },
		{ "ALL-EXC-SELF",
				AV_SH_ALL_EXCSELF,	AV_SH_ALL_EXCSELF },
		{ NULL,		0,			0 }
	};

	static const mdb_bitmask_t extf_flag_bits[] = {
		{ "IER",	APIC_EXTF_IER,		APIC_EXTF_IER },
		{ "SEOI",	APIC_EXTF_SEOI,		APIC_EXTF_SEOI },
		{ "8BIT-ID",	APIC_EXTF_8BIT_ID,	APIC_EXTF_8BIT_ID },
		{ NULL,		0,			0 }
	};

	if ((flags & DCMD_ADDRSPEC) != 0)
		return (DCMD_USAGE);

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, APIC_F_SHOW_BASIC, &arg_flags,
	    'e', MDB_OPT_SETBITS, APIC_F_SHOW_ENTRIES, &arg_flags,
	    'f', MDB_OPT_SETBITS, APIC_F_SHOW_FLAGS, &arg_flags,
	    NULL) != argc)
		return (DCMD_USAGE);

	if (arg_flags == 0)
		arg_flags = APIC_F_SHOW_BASIC;

	if (mdb_lookup_by_obj("apix", "apic_mode", &sym) != 0) {
		mdb_warn("failed to look up apix`apic_mode");
		return (DCMD_ERR);
	}

	ASSERT3U(sym.st_size, ==, sizeof (apic_mode));
	if (mdb_vread(&am, sym.st_size, (uintptr_t)sym.st_value) !=
	    sym.st_size) {
		mdb_warn("failed to read apix`apic_mode");
		return (DCMD_ERR);
	}

	if (mdb_x86_rdmsr(REG_APIC_BASE_MSR, &apic_bar) != DCMD_OK) {
		mdb_warn("failed to read REG_APIC_BASE_MSR");
		return (DCMD_ERR);
	}

	switch (apic_bar & LAPIC_MODE_MASK) {
	case LAPIC_ENABLE_MASK:
		if (am != LOCAL_APIC) {
			mdb_warn("apic_mode does not match APIC_BAR MSR; "
			    "using HW mode LOCAL_APIC\n");
			am = LOCAL_APIC;
		}
		if (mdb_lookup_by_obj("apix", "apicadr", &sym) != 0) {
			mdb_warn("failed to look up apix`apicadr");
			return (DCMD_ERR);
		}
		ASSERT3U(sym.st_size, ==, sizeof (papic));
		if (mdb_vread(&papic, sym.st_size, (uintptr_t)sym.st_value) !=
		    sym.st_size) {
			mdb_warn("failed to read apicadr");
			return (DCMD_ERR);
		}
		break;
	case (LAPIC_ENABLE_MASK | X2APIC_ENABLE_MASK):
		if (am != LOCAL_X2APIC) {
			mdb_warn("apic_mode does not match APIC_BAR MSR; "
			    "using HW mode LOCAL_X2APIC\n");
			am = LOCAL_X2APIC;
		}
		break;
	default:
		if (am == LOCAL_APIC || am == LOCAL_X2APIC) {
			mdb_warn("local APIC is enabled by apic_mode but "
			    "not on this CPU\n");
		} else {
			mdb_warn("local APIC is not enabled\n");
		}
		return (DCMD_ERR);
	}

	if (arg_flags & APIC_F_SHOW_BASIC) {
		mdb_printf("APIC mode: %s",
		    (am == LOCAL_APIC) ? "xAPIC/MMIO" : "x2APIC/MSR");
		if (am == LOCAL_APIC)
			mdb_printf(" @ %lx", papic);
		mdb_printf("\n");

		APIC_READ_REG(am, papic, APIC_LID_REG, &r);
		mdb_printf("ApicId = %#lr", r);
		if (am == LOCAL_APIC)
			mdb_printf(" <id:%#lr>", r >> APIC_ID_BIT_OFFSET);
		mdb_printf("\n");
	}

	APIC_READ_REG(am, papic, APIC_VERS_REG, &r);
	if (r & APIC_EXTENDED_BIT)
		have_extd = B_TRUE;

	if (arg_flags & APIC_F_SHOW_BASIC) {
		vers = bitx64(r, 7, 0);
		mdb_printf("ApicVersion = %#lr <version:%#lr nlvt:%#lr "
		    "flags:%#lb>\n",
		    r, vers,
		    vers >= APIC_INTEGRATED_VERS ? bitx64(r, 23, 16) + 1 : 3,
		    r & ~(0xff00ffUL), vers_flag_bits);

		APIC_READ_REG(am, papic, APIC_TASK_REG, &r);
		mdb_printf("TaskPriority = %#lr\n", r);

		APIC_READ_REG(am, papic, APIC_ARB_PRI_REG, &r);
		mdb_printf("ArbitrationPriority = %#lr\n", r);

		APIC_READ_REG(am, papic, APIC_PROC_PRI_REG, &r);
		mdb_printf("ProcessorPriority = %#lr\n", r);

		APIC_READ_REG(am, papic, APIC_DEST_REG, &r);
		mdb_printf("LogicalDestination = %#lr\n", r);

		if (am == LOCAL_APIC) {
			APIC_READ_REG(am, papic, APIC_FORMAT_REG, &r);
			mdb_printf("DestinationFormat = %#lr <format:%s>\n",
			    r, ((r & 0xf0000000) == 0xf0000000) ? "FLAT" :
			    ((r & 0xf0000000) == 0) ? "CLUSTER" : "Invalid");
		}

		APIC_READ_REG(am, papic, APIC_SPUR_INT_REG, &r);
		mdb_printf("SpuriousInterruptVector = %#lr "
		    "<vect:%#lr flags:%#lb>\n",
		    r, r & 0xffUL, r & ~0xffUL, spur_flag_bits);

		APIC_READ_REG(am, papic, APIC_INT_CMD1, &r);
		if (am == LOCAL_APIC) {
			APIC_READ_REG(am, papic, APIC_INT_CMD2, &rh);
			r |= ((rh & 0xffffffffUL) << 32);
		}
		dsh = r & AV_SH_ALL_EXCSELF;
		mdb_printf("InterruptCommand = %#lr <vect:%#lr msgtype:%s",
		    r, r & 0xff, modetostr((uint32_t)r));
		if (dsh == 0 || dsh == AV_SH_SELF) {
			mdb_printf(" dm:%c",
			    ((r & AV_LDEST) == AV_LDEST) ? 'L' : 'P');
		}
		if (am == LOCAL_APIC)
			mdb_printf(" ds:%c", (r & AV_PENDING) ? 'P' : 'I');
		mdb_printf(" asserted:%c tm:%c\n",
		    (r & AV_REMOTE_IRR) ? 'Y' : 'N',
		    (r & AV_LEVEL) ? 'L' : 'E');
		mdb_printf("    irrs:%#lb dsh:%#lb",
		    (r & (AV_READ_PENDING | AV_REMOTE_STATUS)), irrs_flag_bits,
		    dsh, dsh_flag_bits);
		if (dsh == 0) {
			mdb_printf(" dst:%#lr", bitx64(r, 63, 32));
		}
		mdb_printf(">\n");

		APIC_READ_REG(am, papic, APIC_INIT_COUNT, &r);
		mdb_printf("TimerInitialCount = %#lr\n", r);

		APIC_READ_REG(am, papic, APIC_CURR_COUNT, &r);
		mdb_printf("TimerCurrentCount = %#lr\n", r);

		APIC_READ_REG(am, papic, APIC_DIVIDE_REG, &r);
		mdb_printf("TimerDivideConfiguration = %#lr <div:%#lr>\n",
		    r, apic_timer_divide_map((uint32_t)(r & 0xf)));
	}

	if (have_extd) {
		boolean_t have_seoi = B_FALSE;

		APIC_READ_REG(am, papic, APIC_EXTD_FEATURE_REG, &r);
		nlvt_extd = (uint32_t)bitx64(r, 23, 16);
		if (r & APIC_EXTF_IER)
			have_ier = B_TRUE;
		if (r & APIC_EXTF_SEOI)
			have_seoi = B_TRUE;

		if ((arg_flags & APIC_F_SHOW_BASIC) != 0) {
			mdb_printf("ExtendedApicFeature = %#lr <nlvt:%#lr "
			    "flags:%#lb>\n",
			    r, nlvt_extd, r & ~(0xff0000UL), extf_flag_bits);

			APIC_READ_REG(am, papic, APIC_EXTD_CTRL_REG, &r);
			mdb_printf("ExtendedApicControl = %#r <flags:%#lb>\n",
			    r, r, extf_flag_bits);

			if (have_seoi) {
				APIC_READ_REG(am, papic,
				    APIC_EXTD_SEOI_REG, &r);
				mdb_printf("SpecificEndOfInterrupt = %#r\n", r);
			}
		}
	}

	if ((arg_flags & APIC_F_SHOW_BASIC) != 0)
		mdb_printf("\n");

	if ((arg_flags & APIC_F_SHOW_ENTRIES) != 0) {
		mdb_printf("%<u>%-5s " APIC_ENT_HDR_COMM "%</u>\n",
		    "LVT", APIC_ENT_HDR_ELEM);

		/*
		 * XXX This code used to read and show the CMCI LVT also.
		 * However, that LVT is highly nonstandard and Intel-only.
		 * The generic_cpu module offers a means to discover whether
		 * it's supported on this cpu, but we can assume neither that
		 * this cpu uses generic_cpu nor that it has been initialised
		 * if so.  Absent that, discovering support for this feature
		 * requires duplicating a substantial part of the
		 * gcpu_mca_init() logic.  While the rest of the code in this
		 * module is intended to be shareable with i86pc, we decline to
		 * implement dumping of APIC_CMCI_VECT on the grounds that it
		 * would be expensive to do for something that is *never*
		 * supported on the oxide architecture.  The AMD processors we
		 * support offer identical functionality (with greater
		 * flexibility) via the extended LVT spaces and MCA threshold
		 * and deferred error reporting configuration; we do
		 * automatically discover and report those LVTs if they exist.
		 */

		APIC_READ_REG(am, papic, APIC_LOCAL_TIMER, &r);
		mdb_printf("%-5s ", "TIMER");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("%c\n", (r & AV_PERIODIC) ? '-' : '1');

		APIC_READ_REG(am, papic, APIC_THERM_VECT, &r);
		mdb_printf("%-5s ", "THERM");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("\n");

		APIC_READ_REG(am, papic, APIC_PCINT_VECT, &r);
		mdb_printf("%-5s ", "PERF");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("\n");

		APIC_READ_REG(am, papic, APIC_INT_VECT0, &r);
		mdb_printf("%-5s ", "LINT0");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("\n");

		APIC_READ_REG(am, papic, APIC_INT_VECT1, &r);
		mdb_printf("%-5s ", "LINT1");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("\n");

		APIC_READ_REG(am, papic, APIC_ERR_VECT, &r);
		mdb_printf("%-5s ", "ERROR");
		apic_dump_entry_common(r, B_TRUE, 0);
		mdb_printf("\n");

		if (have_extd) {
			for (lvt = 0; lvt < nlvt_extd; lvt++) {
				APIC_READ_REG(am, papic,
				    APIC_EXTD_LVT_BASE_REG + (lvt << 2), &r);
				mdb_printf("%3s%02x ", "EXT", lvt);
				apic_dump_entry_common(r, B_TRUE, 0);
				mdb_printf("\n");
			}
		}

		mdb_printf("\n");
	}

	if ((arg_flags & APIC_F_SHOW_FLAGS) != 0)
		return (apic_print_flags(am, papic, have_ier));

	return (DCMD_OK);
}

#endif /* _KMDB */
