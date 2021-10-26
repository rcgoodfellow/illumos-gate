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
#include <milan/milan_physaddrs.h>
#include <io/amdzen/amdzen.h>

static uint64_t pcicfg_physaddr;
static boolean_t pcicfg_valid;

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
		 * however, the max is 8 (4 I/O dies in Naples per socket).
		 */
		if (*sock > 8) {
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
		val = AMDZEN_DF_F4_FICAA_TARG_INST |
		    AMDZEN_DF_F4_FICAA_SET_REG(addr) |
		    AMDZEN_DF_F4_FICAA_SET_FUNC(func) |
		    AMDZEN_DF_F4_FICAA_SET_INST(inst);

		if (!pcicfg_write32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAA,
		    val)) {
			return (DCMD_ERR);
		}

		if (!pcicfg_read32(0, 0x18 + sock, 4, AMDZEN_DF_F4_FICAD_LO,
		    &val)) {
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
	uintptr_t sock = 0;
	uint32_t df_busctl, smn_val;
	uint8_t smn_busno;

	if (!(flags & DCMD_ADDRSPEC)) {
		mdb_warn("a register must be specified via an address\n");
		return (DCMD_USAGE);
	}

	if (mdb_getopts(argc, argv, 's', MDB_OPT_UINTPTR, &sock, NULL) !=
	    argc) {
		return (DCMD_USAGE);
	}

	if (sock > 8) {
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


	if (sock > 8) {
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
