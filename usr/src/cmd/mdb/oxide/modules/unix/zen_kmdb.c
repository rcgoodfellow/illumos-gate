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

/*
 * This implements several dcmds for getting at state for use in kmdb. Several
 * of these kind of assume that someone else isn't doing something with them at
 * the same time that we are (mostly because there are only so many slots that
 * can be used for different purposes.
 */

#include <mdb/mdb_modapi.h>
#include <kmdb/kmdb_modext.h>
#include <sys/x86_archext.h>
#include <sys/pci.h>
#include <sys/pcie.h>
#include <sys/sysmacros.h>
#include <milan/milan_physaddrs.h>
#include <io/amdzen/amdzen.h>

static uint64_t pcicfg_physaddr;
static boolean_t pcicfg_valid;

/*
 * These variables, when set, contain a discovered fabric ID.
 */
static boolean_t df_masks_valid;
static uint32_t df_node_shift;
static uint32_t df_node_mask;
static uint32_t df_comp_mask;

typedef struct df_comp {
	uint_t dc_inst;
	const char *dc_name;
	uint_t dc_ndram;
} df_comp_t;

static df_comp_t df_comp_names[0x2b] = {
	{ 0, "UMC0", 2 },
	{ 1, "UMC1", 2 },
	{ 2, "UMC2", 2 },
	{ 3, "UMC3", 2 },
	{ 4, "UMC4", 2 },
	{ 5, "UMC5", 2 },
	{ 6, "UMC6", 2 },
	{ 7, "UMC7", 2 },
	{ 8, "CCIX0" },
	{ 9, "CCIX1" },
	{ 10, "CCIX2" },
	{ 11, "CCIX3" },
	{ 16, "CCM0", 16 },
	{ 17, "CCM1", 16 },
	{ 18, "CCM2", 16 },
	{ 19, "CCM3", 16 },
	{ 20, "CCM4", 16 },
	{ 21, "CCM5", 16 },
	{ 22, "CCM6", 16 },
	{ 23, "CCM7", 16 },
	{ 24, "IOMS0", 16 },
	{ 25, "IOMS1", 16 },
	{ 26, "IOMS2", 16 },
	{ 27, "IOMS3", 16 },
	{ 30, "PIE0", 8 },
	{ 31, "CAKE0" },
	{ 32, "CAKE1" },
	{ 33, "CAKE2" },
	{ 34, "CAKE3" },
	{ 35, "CAKE4" },
	{ 36, "CAKE5" },
	{ 37, "TCDX0" },
	{ 38, "TCDX1" },
	{ 39, "TCDX2" },
	{ 40, "TCDX3" },
	{ 41, "TCDX4" },
	{ 42, "TCDX5" },
	{ 43, "TCDX6" },
	{ 44, "TCDX7" },
	{ 45, "TCDX8" },
	{ 46, "TCDX9" },
	{ 47, "TCDX10" },
	{ 48, "TCDX11" }
};

static const char *df_chan_ileaves[16] = {
	"1", "2", "Reserved", "4",
	"Reserved", "8", "6", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"COD-4 2", "COD-2 4", "COD-1 8", "Reserved"
};

static const char *
df_comp_name(uint32_t compid)
{
	for (uint_t i = 0; i < ARRAY_SIZE(df_comp_names); i++) {
		if (compid == df_comp_names[i].dc_inst) {
			return (df_comp_names[i].dc_name);
		}
	}

	return (NULL);
}

static uint_t
df_comp_ndram(uint32_t compid)
{
	for (uint_t i = 0; i < ARRAY_SIZE(df_comp_names); i++) {
		if (compid == df_comp_names[i].dc_inst) {
			return (df_comp_names[i].dc_ndram);
		}
	}

	return (0);
}

/*
 * Determine if MMIO configuration space is valid at this point. Once it is, we
 * store that fact and don't check again.
 */
static boolean_t
pcicfg_space_init(void)
{
	uint64_t msr;

	if (pcicfg_valid) {
		return (B_TRUE);
	}

	if (mdb_x86_rdmsr(MSR_AMD_MMIOCFG_BASEADDR, &msr) != DCMD_OK) {
		mdb_warn("failed to read MSR_AMD_MMIOCFG_BASEADDR");
		return (B_FALSE);
	}

	if ((msr & AMD_MMIOCFG_BASEADDR_ENABLE) != 0) {
		pcicfg_physaddr = msr & AMD_MMIOCFG_BASEADDR_MASK;
		pcicfg_valid = B_TRUE;
		return (B_TRUE);
	}

	mdb_warn("PCI config space is not currently enabled in the CPU\n");
	return (B_FALSE);
}

static boolean_t
pcicfg_validate(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg)
{
	if (dev >= PCI_MAX_DEVICES) {
		mdb_warn("invalid pci device: %x\n", dev);
		return (B_FALSE);
	}

	if (func >= PCI_MAX_FUNCTIONS) {
		mdb_warn("invalid pci function: %x\n", func);
	}

	if (reg >= PCIE_CONF_HDR_SIZE) {
		mdb_warn("invalid pci register: %x\n", func);
		return (B_FALSE);
	}

	if ((reg & 0x3) != 0) {
		mdb_warn("register much be 4-byte aligned\n", reg);
		return (B_FALSE);
	}

	if (!pcicfg_space_init()) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

static uint64_t
pcicfg_mkaddr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg)
{
	return (pcicfg_physaddr + (bus << 20) + (dev << 15) + (func << 12) +
	    reg);
}

static boolean_t
pcicfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg,
    uint32_t *val)
{
	ssize_t ret;
	uint64_t addr;

	if (!pcicfg_validate(bus, dev, func, reg)) {
		return (B_FALSE);
	}

	addr = pcicfg_mkaddr(bus, dev, func, reg);
	ret = mdb_pread(val, sizeof (*val), addr);
	if (ret != sizeof (*val)) {
		mdb_warn("failed to read %x/%x/%x reg 0x%x", bus, dev, func,
		    reg);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
pcicfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg,
    uint32_t val)
{
	ssize_t ret;
	uint64_t addr;

	if (!pcicfg_validate(bus, dev, func, reg)) {
		return (B_FALSE);
	}

	addr = pcicfg_mkaddr(bus, dev, func, reg);
	ret = mdb_pwrite(&val, sizeof (val), addr);
	if (ret != sizeof (val)) {
		mdb_warn("failed to write %x/%x/%x reg 0x%x", bus, dev, func,
		    reg);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static const char *dfhelp =
"%s a register %s the data fabric. The register is indicated by the addres\n"
"of the dcmd. This can either be directed at a specific instance or be\n"
"broadcast to all instances. One of -b or -i inst is required. If no socket\n"
"(really the I/O die) is specified, then the first one will be selected. The\n"
"following options are supported:\n"
"\n"
"  -b		broadcast the I/O rather than direct it at a single function\n"
"  -f func	direct the I/O to the specified DF function\n"
"  -i inst	direct the I/O to the specified instance, otherwise use -b\n"
"  -s socket	direct the I/O to the specified I/O die, generally a socket\n";

void
rddf_dcmd_help(void)
{
	mdb_printf(dfhelp, "Read", "from");
}

void
wrdf_dcmd_help(void)
{
	mdb_printf(dfhelp, "Write", "to");
}

static int
df_dcmd_check(uintptr_t addr, uint_t flags, boolean_t inst_set, uintptr_t inst,
    boolean_t func_set, uintptr_t func, boolean_t sock_set, uintptr_t *sock,
    uint_t broadcast)
{
	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	} else if ((addr & ~0x3fc) != 0) {
		mdb_warn("invalid register: 0x%x, must be 4-byte aligned\n",
		    addr);
		return (DCMD_ERR);
	}

	if (sock_set) {
		/*
		 * We don't really know how many I/O dies there are in advance;
		 * however, the theoretical max is 8 (2P naples with 4 dies);
		 * however, on the Oxide architecture there'll only ever be 2.
		 */
		if (*sock > 2) {
			mdb_warn("invalid socket ID: %lu", sock);
			return (DCMD_ERR);
		}
	} else {
		*sock = 0;
	}

	if (!func_set) {
		mdb_warn("-f is required\n");
		return (DCMD_ERR);
	} else if (func >= 8) {
		mdb_warn("only functions 0-7 are required: %lu", func);
		return (DCMD_ERR);
	}


	if ((!inst_set && !broadcast) ||
	    (inst_set && broadcast)) {
		mdb_warn("One of -i or -b must be set\n");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static boolean_t
df_read32_indirect(uint64_t sock, uintptr_t inst, uintptr_t func, uint16_t reg,
    uint32_t *valp)
{
	uint32_t val;

	val = AMDZEN_DF_F4_FICAA_TARG_INST | AMDZEN_DF_F4_FICAA_SET_REG(reg) |
	    AMDZEN_DF_F4_FICAA_SET_FUNC(func) |
	    AMDZEN_DF_F4_FICAA_SET_INST(inst);

	if (!pcicfg_write32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAA, val)) {
		return (B_FALSE);
	}

	if (!pcicfg_read32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAD_LO, &val)) {
		return (B_FALSE);
	}

	*valp = val;
	return (B_TRUE);
}

int
rddf_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t broadcast = FALSE;
	boolean_t inst_set = FALSE, func_set = FALSE, sock_set = FALSE;
	uintptr_t inst, func, sock;
	uint32_t val;
	int ret;

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &broadcast,
	    'f', MDB_OPT_UINTPTR_SET, &func_set, &func,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst,
	    's', MDB_OPT_UINTPTR_SET, &sock_set, &sock,
	    NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((ret = df_dcmd_check(addr, flags, inst_set, inst, func_set, func,
	    sock_set, &sock, broadcast)) != DCMD_OK) {
		return (ret);
	}

	/*
	 * For a broadcast read, read directly. Otherwise we need to use the
	 * FICAA register.
	 */
	if (broadcast) {
		if (!pcicfg_read32(0, 0x18 + sock, func, addr, &val)) {
			return (DCMD_ERR);
		}
	} else {
		if (!df_read32_indirect(sock, inst, func, addr, &val)) {
			return (DCMD_ERR);
		}
	}

	mdb_printf("%x\n", val);
	return (DCMD_OK);
}

int
wrdf_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint_t broadcast = FALSE;
	boolean_t inst_set = FALSE, func_set = FALSE, sock_set = FALSE;
	uintptr_t inst, func, sock;
	u_longlong_t parse_val;
	uint32_t val;
	int ret;

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &broadcast,
	    'f', MDB_OPT_UINTPTR_SET, &func_set, &func,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst,
	    's', MDB_OPT_UINTPTR_SET, &sock_set, &sock,
	    NULL) != argc - 1) {
		mdb_warn("missing required value to write\n");
		return (DCMD_USAGE);
	}

	if (argv[argc - 1].a_type == MDB_TYPE_STRING) {
		parse_val = mdb_strtoull(argv[argc - 1].a_un.a_str);
	} else {
		parse_val = argv[argc - 1].a_un.a_val;
	}
	if (parse_val > UINT32_MAX) {
		mdb_warn("write value must be a 32-bit quantity\n");
		return (DCMD_ERR);
	}
	val = (uint32_t)parse_val;


	if ((ret = df_dcmd_check(addr, flags, inst_set, inst, func_set, func,
	    sock_set, &sock, broadcast)) != DCMD_OK) {
		return (ret);
	}

	if (broadcast) {
		if (!pcicfg_write32(0, 0x18 + sock, func, addr, val)) {
			return (DCMD_ERR);
		}
	} else {
		uint32_t rval = AMDZEN_DF_F4_FICAA_TARG_INST |
		    AMDZEN_DF_F4_FICAA_SET_REG(addr) |
		    AMDZEN_DF_F4_FICAA_SET_FUNC(func) |
		    AMDZEN_DF_F4_FICAA_SET_INST(inst);

		if (!pcicfg_write32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAA,
		    rval)) {
			return (DCMD_ERR);
		}

		if (!pcicfg_write32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAD_LO,
		    val)) {
			return (DCMD_ERR);
		}
	}

	return (DCMD_OK);
}

static const char *smnhelp =
"%s a register %s the system management network (SMN). The address of the\n"
"dcmd is used to indicate the register to target. If no socket (really the\n"
"I/O die) is specified, then the first one will be selected. The NBIO\n"
"instance to use is determined based on what the DF indicates. The following\n"
"options are supported:\n"
"\n"
"  -s socket	direct the I/O to the specified I/O die, generally a socket\n";

void
rdsmn_dcmd_help(void)
{
	mdb_printf(smnhelp, "Read", "from");
}

void
wrsmn_dcmd_help(void)
{
	mdb_printf(smnhelp, "Write", "to");
}

int
rdsmn_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint64_t sock = 0;
	uint32_t df_busctl, smn_val;
	uint8_t smn_busno;

	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	}

	if (mdb_getopts(argc, argv, 's', MDB_OPT_UINT64, &sock, NULL) !=
	    argc) {
		return (DCMD_USAGE);
	}

	if (sock > 2) {
		mdb_warn("invalid socket ID: %lu", sock);
		return (DCMD_ERR);
	}

	if (!pcicfg_read32(0, 0x18 + sock, 0, AMDZEN_DF_F0_CFG_ADDR_CTL,
	    &df_busctl)) {
		mdb_warn("failed to read DF config address\n");
		return (DCMD_ERR);
	}

	if (df_busctl == PCI_EINVAL32) {
		mdb_warn("got back PCI_EINVAL32 when reading from the df\n");
		return (DCMD_ERR);
	}

	smn_busno = AMDZEN_DF_F0_CFG_ADDR_CTL_BUS_NUM(df_busctl);
	if (!pcicfg_write32(smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_ADDR, addr)) {
		mdb_warn("failed to write to IOHC SMN address register\n");
		return (DCMD_ERR);
	}

	if (!pcicfg_read32(smn_busno, AMDZEN_NB_SMN_DEVNO, AMDZEN_NB_SMN_FUNCNO,
	    AMDZEN_NB_SMN_DATA, &smn_val)) {
		mdb_warn("failed to read from IOHC SMN data register\n");
		return (DCMD_ERR);
	}

	mdb_printf("%lx\n", smn_val);
	return (DCMD_OK);
}

int
wrsmn_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uintptr_t sock = 0;
	uint32_t df_busctl, smn_val;
	uint8_t smn_busno;
	u_longlong_t parse_val;

	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	}

	if (mdb_getopts(argc, argv, 's', MDB_OPT_UINTPTR, &sock, NULL) !=
	    argc - 1) {
		return (DCMD_USAGE);
	}

	if (argv[argc - 1].a_type == MDB_TYPE_STRING) {
		parse_val = mdb_strtoull(argv[argc - 1].a_un.a_str);
	} else {
		parse_val = argv[argc - 1].a_un.a_val;
	}
	if (parse_val > UINT32_MAX) {
		mdb_warn("write value must be a 32-bit quantity\n");
		return (DCMD_ERR);
	}
	smn_val = (uint32_t)parse_val;


	if (sock > 2) {
		mdb_warn("invalid socket ID: %lu", sock);
		return (DCMD_ERR);
	}

	if (!pcicfg_read32(0, 0x18 + sock, 0, AMDZEN_DF_F0_CFG_ADDR_CTL,
	    &df_busctl)) {
		mdb_warn("failed to read DF config address\n");
		return (DCMD_ERR);
	}

	if (df_busctl == PCI_EINVAL32) {
		mdb_warn("got back PCI_EINVAL32 when reading from the df\n");
		return (DCMD_ERR);
	}

	smn_busno = AMDZEN_DF_F0_CFG_ADDR_CTL_BUS_NUM(df_busctl);
	if (!pcicfg_write32(smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_ADDR, addr)) {
		mdb_warn("failed to write to IOHC SMN address register\n");
		return (DCMD_ERR);
	}

	if (!pcicfg_write32(smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA, smn_val)) {
		mdb_warn("failed to write to IOHC SMN data register\n");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static boolean_t
df_fetch_masks(void)
{
	uint32_t fid0, fid1;

	if (!pcicfg_read32(0, 0x18, 1, AMDZEN_DF_F1_FIDMASK0, &fid0) ||
	    !pcicfg_read32(0, 0x18, 1, AMDZEN_DF_F1_FIDMASK1, &fid1)) {
		mdb_warn("failed to read masks register\n");
		return (B_FALSE);
	}


	df_node_mask = AMDZEN_DF_F1_FIDMASK0_NODE_MASK(fid0);
	df_comp_mask = AMDZEN_DF_F1_FIDMASK0_COMP_MASK(fid0);
	df_node_shift = AMDZEN_DF_F1_FIDMASK1_NODE_SHIFT(fid1);

	df_masks_valid = B_TRUE;
	return (B_TRUE);
}

/*
 * Given a data fabric fabric ID (critically not an instance ID), print
 * information about that.
 */
static void
df_print_dest(uint32_t dest)
{
	uint32_t node, comp;
	const char *name;

	if (!df_masks_valid) {
		if (!df_fetch_masks()) {
			mdb_printf("%x", dest);
			return;
		}
	}

	node = (dest & df_node_mask) >> df_node_shift;
	comp = dest & df_comp_mask;
	name = df_comp_name(comp);

	mdb_printf("%#x (%#x/%#x)", dest, node, comp);
	if (name != NULL) {
		mdb_printf(" -- %s", name);
	}
}

static const char *df_route_help =
"Print out routing rules in the data fabric. This currently supports reading\n"
"the PCI bus, I/O port, MMIO, and DRAM routing rules. These values can vary,\n"
"especially with DRAM, from instance to instance. All route entries of a\n"
"given type are printed. Where possible, we will select a default instance to\n"
"use for this. The following options are used to specify the type of routing\n"
"entries to print:\n"
"  -b           print PCI bus routing entries\n"
"  -d           print DRAM routing entries\n"
"  -I           print I/O port entries\n"
"  -m           print MMIO routing entries\n"
"\n"
"The following options are used to control which instance to print from\n"
"  -i inst	print entries from the specified instance\n"
"  -s socket	print entries from the specified I/O die, generally a socket\n"
"\n"
"The following letters are used in the rather terse FLAGS output:\n"
"\n"
"    R		Read Enabled (PCI Bus, I/O Ports, MMIO)\n"
"    W		Write Enabled (PCI Bus, I/O Ports, MMIO)\n"
"    I		ISA Shenanigans (I/O ports)\n"
"    N		Non-posted mode (MMIO)\n"
"    C		CPU redirected to compat addresses (MMIO)\n"
"    B		Break Bus lock (DRAM)\n"
"    H		MMIO Hole Enabled (DRAM)\n"
"    V		Rule Valid (DRAM)\n";

void
df_route_dcmd_help(void)
{
	mdb_printf(df_route_help);
}

static int
df_route_buses(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-7s %-7s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINATION");
	}

	for (uint_t i = 0; i < AMDZEN_DF_F0_MAX_CFGMAP; i++) {
		uint32_t val;

		if (!df_read32_indirect(sock, inst, 0, AMDZEN_DF_F0_CFGMAP(i),
		    &val)) {
			mdb_warn("failed to read cfgmap %u\n", i);
			continue;
		}

		if (val == PCI_EINVAL32) {
			mdb_warn("got back invalid read for cfgmap %u\n", i);
			continue;
		}

		mdb_printf("%-7#x %-7#x %c%c       ",
		    AMDZEN_DF_F0_GET_CFGMAP_BUS_BASE(val),
		    AMDZEN_DF_F0_GET_CFGMAP_BUS_LIMIT(val),
		    AMDZEN_DF_F0_GET_CFGMAP_RE(val) ? 'R' : '-',
		    AMDZEN_DF_F0_GET_CFGMAP_WE(val) ? 'W' : '-');
		df_print_dest(AMDZEN_DF_F0_GET_CFGMAP_DEST_ID(val));
		mdb_printf("\n");
	}

	return (DCMD_OK);
}

static int
df_route_dram(uint_t flags, uint64_t sock, uintptr_t inst)
{
	uint_t ndram = df_comp_ndram(inst);

	if (ndram == 0) {
		mdb_warn("component 0x%x has no DRAM rules\n", inst);
		return (DCMD_ERR);
	}

	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-?s %-?s %-7s %-15s %s\n", "BASE", "LIMIT",
		    "FLAGS", "INTERLEAVE", "DESTINATION");
	}

	for (uint_t i = 0; i < ndram; i++) {
		uint32_t breg, lreg;
		uint64_t base, limit;
		const char *chan;
		char ileave[16];

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_Z2_3_DF_F0_DRAM_BASE(i), &breg)) {
			mdb_warn("failed to read DRAM port base %u\n", i);
			continue;
		}

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_Z2_3_DF_F0_DRAM_LIMIT(i), &lreg)) {
			mdb_warn("failed to read DRAM port limit %u\n", i);
			continue;
		}

		base = AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_BASE(breg);
		base <<= AMDZEN_Z2_3_DF_F0_DRAM_BASE_BASE_SHIFT;
		limit = AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_LIMIT(lreg);
		limit <<= AMDZEN_Z2_3_DF_F0_DRAM_LIMIT_LIMIT_SHIFT;
		limit += (1 << AMDZEN_Z2_3_DF_F0_DRAM_LIMIT_LIMIT_SHIFT) - 1;

		chan = df_chan_ileaves[
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_CHAN_ILEAVE(breg)];
		(void) mdb_snprintf(ileave, sizeof (ileave), "%u/%s/%u/%u",
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_ADDR_ILEAVE(breg) + 8, chan,
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_DIE_ILEAVE(breg) + 1,
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_SOCK_ILEAVE(breg) + 1);

		mdb_printf("%-?#lx %-?#lx %c%c%c     %-15s ", base, limit,
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_VALID(breg) ? 'V' : '-',
		    AMDZEN_Z2_3_DF_FO_GET_DRAM_BASE_HOLE_EN(breg) ? 'H' : '-',
		    AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_BUS_BREAK(lreg) ?
		    'B' : '-', ileave);
		df_print_dest(AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_DEST_ID(lreg));
		mdb_printf("\n");
	}

	return (DCMD_OK);
}

static int
df_route_ioports(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-8s %-8s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINAION");
	}

	for (uint_t i = 0; i < AMDZEN_DF_F0_MAX_IO_RULES; i++) {
		uint32_t breg, lreg, base, limit;

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_DF_F0_IO_BASE(i), &breg)) {
			mdb_warn("failed to read I/O port base %u\n", i);
			continue;
		}

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_DF_F0_IO_LIMIT(i), &lreg)) {
			mdb_warn("failed to read I/O port limit %u\n", i);
			continue;
		}

		base = AMDZEN_DF_F0_GET_IO_BASE_BASE(breg);
		base <<= AMDZEN_DF_F0_IO_BASE_SHIFT;
		limit = AMDZEN_DF_F0_GET_IO_LIMIT_LIMIT(lreg);
		limit <<= AMDZEN_DF_F0_IO_LIMIT_SHIFT;
		limit += (1 << AMDZEN_DF_F0_IO_LIMIT_SHIFT) - 1;

		mdb_printf("%-8#x %-8#x %c%c%c      ", base, limit,
		    AMDZEN_DF_F0_GET_IO_BASE_RE(breg) ? 'R' : '-',
		    AMDZEN_DF_F0_GET_IO_BASE_WE(breg) ? 'W' : '-',
		    AMDZEN_DF_F0_GET_IO_BASE_IE(breg) ? 'I' : '-');
		df_print_dest(AMDZEN_DF_FO_GET_IO_LIMIT_DEST_ID(lreg));
		mdb_printf("\n");
	}

	return (DCMD_OK);
}

static int
df_route_mmio(uint_t flags, uint64_t sock, uintptr_t inst)
{
	if (DCMD_HDRSPEC(flags)) {
		mdb_printf("%-?s %-?s %-8s %s\n", "BASE", "LIMIT", "FLAGS",
		    "DESTINAION");
	}

	for (uint_t i = 0; i < AMDZEN_DF_F0_MAX_MMIO_RULES; i++) {
		uint32_t breg, lreg, control;
		uint64_t base, limit;

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_DF_F0_MMIO_BASE(i), &breg)) {
			mdb_warn("failed to read MMIO base %u\n", i);
			continue;
		}

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_DF_F0_MMIO_LIMIT(i), &lreg)) {
			mdb_warn("failed to read MMIO limit %u\n", i);
			continue;
		}

		if (!df_read32_indirect(sock, inst, 0,
		    AMDZEN_Z2_3_DF_F0_MMIO_CTRL(i), &control)) {
			mdb_warn("failed to read MMIO control %u\n", i);
			continue;
		}

		base = (uint64_t)breg << AMDZEN_DF_F0_MMIO_SHIFT;
		limit = (uint64_t)lreg << AMDZEN_DF_F0_MMIO_SHIFT;
		limit += (1 << AMDZEN_DF_F0_MMIO_SHIFT) - 1;

		mdb_printf("%-?#lx %-?#lx %c%c%c%c     ", base, limit,
		    AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_RE(control) ? 'R' : '-',
		    AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_WE(control) ? 'W' : '-',
		    AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_NP(control) ? 'N' : '-',
		    AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_CPU(control) ? 'C' : '-');
		df_print_dest(AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_DEST_ID(control));
		mdb_printf("\n");
	}
	return (DCMD_OK);
}

int
df_route_dcmd(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	uint64_t sock = 0;
	uintptr_t inst;
	boolean_t inst_set = B_FALSE;
	uint_t opt_b = FALSE, opt_d = FALSE, opt_I = FALSE, opt_m = FALSE;
	uint_t count = 0;

	if (mdb_getopts(argc, argv,
	    'b', MDB_OPT_SETBITS, TRUE, &opt_b,
	    'd', MDB_OPT_SETBITS, TRUE, &opt_d,
	    'I', MDB_OPT_SETBITS, TRUE, &opt_I,
	    'm', MDB_OPT_SETBITS, TRUE, &opt_m,
	    's', MDB_OPT_UINT64, &sock,
	    'i', MDB_OPT_UINTPTR_SET, &inst_set, &inst, NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((flags & DCMD_ADDRSPEC) != 0) {
		mdb_warn("df_route does not support addresses\n");
		return (DCMD_USAGE);
	}

	if (opt_b) {
		count++;
	}
	if (opt_d)
		count++;
	if (opt_I)
		count++;
	if (opt_m)
		count++;

	if (count == 0) {
		mdb_warn("one of -b, -d, -I, and -m must be specified\n");
		return (DCMD_ERR);
	} else if (count > 1) {
		mdb_warn("only one of -b -d, -I, and -m may be specified");
		return (DCMD_ERR);
	}

	if (sock > 2) {
		mdb_warn("invalid socket ID: %lu", sock);
		return (DCMD_ERR);
	}

	/*
	 * For DRAM, default to CCM0 (we don't use a UMC because it has very few
	 * rules). For I/O ports, use CCM0 as well as the IOMS entries don't
	 * really have rules here. For MMIO and PCI buses, use IOMS0.
	 */
	if (!inst_set) {
		if (opt_d || opt_I) {
			inst = 0x10;
		} else {
			inst = 0x18;
		}
	}

	if (opt_d) {
		return (df_route_dram(flags, sock, inst));
	} else if (opt_b) {
		return (df_route_buses(flags, sock, inst));
	} else if (opt_I) {
		return (df_route_ioports(flags, sock, inst));
	} else {
		return (df_route_mmio(flags, sock, inst));
	}

	return (DCMD_OK);
}
