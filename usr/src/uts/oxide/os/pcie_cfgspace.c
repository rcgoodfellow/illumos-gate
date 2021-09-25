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
 * This file provides a means of accessing PCIe extended configuration space
 * over memory mapped I/O. Traditionally this was always accessed over the
 * various I/O ports; however, we instead opt to leverage facilities in the CPU
 * to set up memory-mapped I/O. To do this we basically do an initial mapping
 * that we use prior to VM in whatever VA space that we can get. After which,
 * we will unmap that and leverage addresses from the device arena once that has
 * been set up.
 *
 * Configuration space is accessed by constructing and addresss that has the
 * bits arranged in the following pattern to indicate what the bus, device,
 * function, and register is:
 *
 *	bus[7:0]	addr[27:20]
 *	dev[4:0]	addr[19:15]
 *	func[2:0]	addr[14:12]
 *	reg[11:0]	addr[11:0]
 *
 * The CPU does not generally support 64-bit accesses, which means that a 64-bit
 * access requires us to write the lower 32-bits followed by the uppwer 32-bits.
 */

#include <sys/machparam.h>
#include <vm/kboot_mmu.h>
#include <vm/as.h>
#include <vm/hat.h>
#include <sys/mman.h>
#include <sys/bootconf.h>
#include <sys/spl.h>
#include <sys/boot_debug.h>
#include <sys/pci.h>
#include <sys/pcie.h>
#include <sys/pci_cfgacc.h>
#include <sys/machsystm.h>

#include <milan/milan_ccx.h>
#include <milan/milan_physaddrs.h>

/*
 * XXX This section contains variables that the rest of the system expects to
 * have. Their needs should be re-evaluated as we go.
 */
int pci_bios_maxbus = 0xff;

/*
 * These function pointers are entry points that the system has historically
 * assumed to exist. While we only have a single implementation, for now we need
 * to keep the indirect functions.
 *
 * XXX Can we go back and make the arguments for the bdfr unsigned?
 */
uint8_t (*pci_getb_func)(int bus, int dev, int func, int reg);
uint16_t (*pci_getw_func)(int bus, int dev, int func, int reg);
uint32_t (*pci_getl_func)(int bus, int dev, int func, int reg);
void (*pci_putb_func)(int bus, int dev, int func, int reg, uint8_t val);
void (*pci_putw_func)(int bus, int dev, int func, int reg, uint16_t val);
void (*pci_putl_func)(int bus, int dev, int func, int reg, uint32_t val);
extern void (*pci_cfgacc_acc_p)(pci_cfgacc_req_t *req);

/*
 * This is the required size and alignment of PCIe extended configuration space.
 * This needs to be 256 MiB in size. This requires 1 MiB alignment; however,
 * because we use 2 MiB pages, we alway use the larger alignment.
 */
#define	PCIE_CFGSPACE_SIZE	(1024 * 1024 * 256)
#define	PCIE_CFGSPACE_ALIGN	(1024 * 1024 * 2)

/*
 * Bit offsets for extended configuration space and corresponding masks. One
 * notable thing here is that the PCIE_CFGSPACE_FUNC_MASK is set to be 8 bits
 * instead of the normal three. This is to support ARIs.
 */
#define	PCIE_CFGSPACE_BUS_OFFSET	20
#define	PCIE_CFGSPACE_BUS_MASK(x)	((x) & 0xff)
#define	PCIE_CFGSPACE_DEV_OFFSET	15
#define	PCIE_CFGSPACE_DEV_MASK(x)	((x) & 0x1f)
#define	PCIE_CFGSPACE_FUNC_OFFSET	12
#define	PCIE_CFGSPACE_FUNC_MASK(x)	((x) & 0xff)
#define	PCIE_CFGSPACE_REG_MASK(x)	((x) & 0xfff)

/*
 * The pci_cfgacc_req
 */

/*
 * This contains the base virtual address for PCIe configuration space.
 */
static uintptr_t pcie_cfgspace_vaddr;

static boolean_t
pcie_access_check(int bus, int dev, int func, int reg)
{
	if (bus < 0 || bus >= PCI_MAX_BUS_NUM) {
		return (B_FALSE);
	}

	if (dev < 0 || dev >= PCI_MAX_DEVICES) {
		return (B_FALSE);
	}

	/*
	 * Due to the advent of ARIs we want to make sure that we're not overly
	 * stringent here. ARIs retool how the bits are used for the device and
	 * function. This means that if dev == 0, allow func to be up to 0xff.
	 */
	if (func < 0 || (dev != 0 && func >= PCI_MAX_FUNCTIONS) ||
	    (dev == 0 && func >= PCIE_ARI_MAX_FUNCTIONS)) {
		return (B_FALSE);
	}

	/*
	 * Technically the maximum register is determined by the parent. At this
	 * point we have no way of knowing what is PCI or PCIe and will rely on
	 * mmio to solve this for us.
	 */
	if (reg < 0 || reg >= PCIE_CONF_HDR_SIZE) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

static uintptr_t
pcie_bdfr_to_addr(int bus, int dev, int func, int reg)
{
	uintptr_t bdfr;

	bdfr = ((PCIE_CFGSPACE_BUS_MASK(bus) << PCIE_CFGSPACE_BUS_OFFSET) |
	    (PCIE_CFGSPACE_DEV_MASK(dev) << PCIE_CFGSPACE_DEV_OFFSET) |
	    (PCIE_CFGSPACE_FUNC_MASK(func) << PCIE_CFGSPACE_FUNC_OFFSET) |
	    (PCIE_CFGSPACE_REG_MASK(reg)));

	return (bdfr + pcie_cfgspace_vaddr);
}

uint8_t
pcie_cfgspace_read_uint8(int bus, int dev, int func, int reg)
{
	volatile uint8_t *u8p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return (PCI_EINVAL8);
	}

	u8p = (uint8_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	return (*u8p);
}

void
pcie_cfgspace_write_uint8(int bus, int dev, int func, int reg, uint8_t val)
{
	volatile uint8_t *u8p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return;
	}

	u8p = (uint8_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	*u8p = val;
}

uint16_t
pcie_cfgspace_read_uint16(int bus, int dev, int func, int reg)
{
	volatile uint16_t *u16p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return (PCI_EINVAL16);
	}

	u16p = (uint16_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	return (*u16p);
}

void
pcie_cfgspace_write_uint16(int bus, int dev, int func, int reg, uint16_t val)
{
	volatile uint16_t *u16p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return;
	}

	u16p = (uint16_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	*u16p = val;
}

uint32_t
pcie_cfgspace_read_uint32(int bus, int dev, int func, int reg)
{
	volatile uint32_t *u32p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return (PCI_EINVAL32);
	}

	u32p = (uint32_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	return (*u32p);
}

void
pcie_cfgspace_write_uint32(int bus, int dev, int func, int reg, uint32_t val)
{
	volatile uint32_t *u32p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return;
	}

	u32p = (uint32_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	*u32p = val;
}

/*
 * XXX Historically only 32-bit accesses were done to configuration space.
 */
uint64_t
pcie_cfgspace_read_uint64(int bus, int dev, int func, int reg)
{
	volatile uint64_t *u64p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return (PCI_EINVAL64);
	}

	u64p = (uint64_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	return (*u64p);
}

void
pcie_cfgspace_write_uint64(int bus, int dev, int func, int reg, uint64_t val)
{
	volatile uint64_t *u64p;

	if (!pcie_access_check(bus, dev, func, reg)) {
		return;
	}

	u64p = (uint64_t *)pcie_bdfr_to_addr(bus, dev, func, reg);
	*u64p = val;
}

/*
 * The following function is a stub that is expected to exist due to
 * support for older platforms from the old pci_cfgacc_x86.c. Because we don't
 * have the old systems with a broken AMD ECS, we can just make these simple.
 */
void
pci_cfgacc_add_workaround(uint16_t bdf, uchar_t secbus, uchar_t subbus)
{
}

/*
 * This is an entry point that expects accesses in a different pattern from the
 * traditional function pointers used above.
 */
void
pcie_cfgspace_acc(pci_cfgacc_req_t *req)
{
	int bus, dev, func, reg;

	bus = PCI_CFGACC_BUS(req);
	dev = PCI_CFGACC_DEV(req);
	func = PCI_CFGACC_FUNC(req);
	reg = req->offset;

	switch (req->size) {
	case PCI_CFG_SIZE_BYTE:
		if (req->write) {
			pcie_cfgspace_write_uint8(bus, dev, func, reg,
			    VAL8(req));
		} else {
			VAL8(req) = pcie_cfgspace_read_uint8(bus, dev, func,
			    reg);
		}
		break;
	case PCI_CFG_SIZE_WORD:
		if (req->write) {
			pcie_cfgspace_write_uint16(bus, dev, func, reg,
			    VAL16(req));
		} else {
			VAL16(req) = pcie_cfgspace_read_uint16(bus, dev, func,
			    reg);
		}
		break;
	case PCI_CFG_SIZE_DWORD:
		if (req->write) {
			pcie_cfgspace_write_uint32(bus, dev, func, reg,
			    VAL32(req));
		} else {
			VAL32(req) = pcie_cfgspace_read_uint32(bus, dev, func,
			    reg);
		}
		break;
	case PCI_CFG_SIZE_QWORD:
		if (req->write) {
			pcie_cfgspace_write_uint64(bus, dev, func, reg,
			    VAL64(req));
		} else {
			VAL64(req) = pcie_cfgspace_read_uint64(bus, dev, func,
			    reg);
		}
		break;
	default:
		if (!req->write) {
			VAL64(req) = PCI_EINVAL64;
		}
		break;
	}
}

void
pcie_cfgspace_init(void)
{
	/*
	 * This ensures that the boot CPU will be programmed with everything
	 * needed to access PCIe configuration space. XXX Other CPUs.
	 */
	milan_ccx_mmio_init(MILAN_PHYSADDR_PCIECFG);

	/*
	 * This is a temporary VA range that we'll use during bootstrapping.
	 * Once we get vmem set up and the device arena allocated, this will be
	 * remapped to a final address.
	 */
	pcie_cfgspace_vaddr = alloc_vaddr(PCIE_CFGSPACE_SIZE,
	    PCIE_CFGSPACE_ALIGN);
	DBG_MSG("PCIe configuration space mapped at 0x%lx\n",
	    pcie_cfgspace_vaddr);

	for (uintptr_t offset = 0; offset < PCIE_CFGSPACE_SIZE;
	    offset += PCIE_CFGSPACE_ALIGN) {
		kbm_map(pcie_cfgspace_vaddr + offset,
		    MILAN_PHYSADDR_PCIECFG + offset, 1,
		    PT_WRITABLE | PT_NOCACHE);
	}

	pci_getb_func = pcie_cfgspace_read_uint8;
	pci_getw_func = pcie_cfgspace_read_uint16;
	pci_getl_func = pcie_cfgspace_read_uint32;
	pci_putb_func = pcie_cfgspace_write_uint8;
	pci_putw_func = pcie_cfgspace_write_uint16;
	pci_putl_func = pcie_cfgspace_write_uint32;
	pci_cfgacc_acc_p = pcie_cfgspace_acc;

	/*
	 * XXX Now that config space is mapped we need to come back and actually
	 * do things like configure completion timeouts and related.
	 */
}

/*
 * This is called once the device arena has been set up. We don't bother
 * unmapping the original bootstrap address range because it will just be torn
 * down when we tear down that hat.
 */
void
pcie_cfgspace_remap(void)
{
	void *new_va = device_arena_alloc(PCIE_CFGSPACE_SIZE, VM_SLEEP);
	pfn_t pfn = mmu_btop(MILAN_PHYSADDR_PCIECFG);

	hat_devload(kas.a_hat, new_va, PCIE_CFGSPACE_SIZE, pfn,
	    PROT_READ | PROT_WRITE | HAT_STRICTORDER,
	    HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST);
	pcie_cfgspace_vaddr = (uintptr_t)new_va;
}
