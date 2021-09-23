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
 * Copyright 2014 Garrett D'Amore <garrett@damore.org>
 * Copyright (c) 2012 Gary Mills
 * Copyright (c) 1992, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2022 Oxide Computer Co.
 */

/*
 *	ISA bus nexus driver, stub version for a hackish serial console only
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/autoconf.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/psm.h>
#include <sys/ddi_impldefs.h>
#include <sys/ddi_subrdefs.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/mach_intr.h>
#include <sys/note.h>
#include <sys/boot_console.h>
#include <sys/apic.h>
#include <sys/smp_impldefs.h>

extern int pseudo_isa;
extern int isa_resource_setup(void);
extern int (*psm_intr_ops)(dev_info_t *, ddi_intr_handle_impl_t *,
    psm_intr_op_t, int *);
static void isa_enumerate(int);
static void isa_create_ranges_prop(dev_info_t *);

#define	USED_RESOURCES	"used-resources"

/*
 * The following typedef is used to represent an entry in the "ranges"
 * property of a pci-isa bridge device node.
 */
typedef struct {
	uint32_t child_high;
	uint32_t child_low;
	uint32_t parent_high;
	uint32_t parent_mid;
	uint32_t parent_low;
	uint32_t size;
} pib_ranges_t;

typedef struct {
	uint32_t base;
	uint32_t len;
} used_ranges_t;

#define	USED_CELL_SIZE	2	/* 1 byte addr, 1 byte size */
#define	ISA_ADDR_IO	1	/* IO address space */
#define	ISA_ADDR_MEM	0	/* memory adress space */

/*
 * #define ISA_DEBUG 1
 */

#define	N_ASY		1
#define	COM_ISR		2	/* 16550 intr status register */
#define	COM_SCR		7	/* 16550 scratch register */

/*
 * This was originally for non-ACPI async ports and parallel ports, but we don't
 * have ACPI and we don't support parallel ports at all, so we need but one.
 */
#define	MAX_EXTRA_RESOURCE	1
static struct regspec isa_extra_resource[MAX_EXTRA_RESOURCE];
static int isa_extra_count = 0;

static struct regspec asy_regs[] = {
	{1, 0x3f8, 0x8},
};

static int asy_intrs[] = {0x3};

static int
isa_bus_map(dev_info_t *dip, dev_info_t *rdip, ddi_map_req_t *mp,
    off_t offset, off_t len, caddr_t *vaddrp);

static int
isa_ctlops(dev_info_t *, dev_info_t *, ddi_ctl_enum_t, void *, void *);

static int
isa_intr_ops(dev_info_t *pdip, dev_info_t *rdip, ddi_intr_op_t intr_op,
    ddi_intr_handle_impl_t *hdlp, void *result);
static int isa_alloc_intr_fixed(dev_info_t *, ddi_intr_handle_impl_t *, void *);
static int isa_free_intr_fixed(dev_info_t *, ddi_intr_handle_impl_t *);

struct bus_ops isa_bus_ops = {
	.busops_rev = BUSO_REV,
	.bus_map = isa_bus_map,
	.bus_map_fault = i_ddi_map_fault,
	.bus_dma_map = ddi_no_dma_map,
	.bus_dma_allochdl = ddi_no_dma_allochdl,
	.bus_dma_freehdl = ddi_no_dma_freehdl,
	.bus_dma_bindhdl = ddi_no_dma_bindhdl,
	.bus_dma_unbindhdl = ddi_no_dma_unbindhdl,
	.bus_dma_flush = ddi_no_dma_flush,
	.bus_dma_win = ddi_no_dma_win,
	.bus_dma_ctl = ddi_no_dma_mctl,
	.bus_ctl = isa_ctlops,
	.bus_prop_op = ddi_bus_prop_op,
	.bus_intr_op = isa_intr_ops
};


static int isa_attach(dev_info_t *devi, ddi_attach_cmd_t cmd);

/*
 * Internal isa ctlops support routines
 */
static int isa_initchild(dev_info_t *child);

struct dev_ops isa_ops = {
	DEVO_REV,		/* devo_rev, */
	0,			/* refcnt  */
	ddi_no_info,		/* info */
	nulldev,		/* identify */
	nulldev,		/* probe */
	isa_attach,		/* attach */
	nulldev,		/* detach */
	nodev,			/* reset */
	(struct cb_ops *)0,	/* driver operations */
	&isa_bus_ops,		/* bus operations */
	NULL,			/* power */
	ddi_quiesce_not_needed,		/* quiesce */
};

/*
 * Module linkage information for the kernel.
 */

static struct modldrv modldrv = {
	&mod_driverops, /* Type of module.  This is ISA bus driver */
	"isa nexus driver for 'ISA'",
	&isa_ops,	/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

int
_init(void)
{
	int	err;

	if ((err = mod_install(&modlinkage)) != 0)
		return (err);

	impl_bus_add_probe(isa_enumerate);
	return (0);
}

int
_fini(void)
{
	int	err;

	impl_bus_delete_probe(isa_enumerate);

	if ((err = mod_remove(&modlinkage)) != 0)
		return (err);

	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}

static int
isa_attach(dev_info_t *devi, ddi_attach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_ATTACH:
		break;
	case DDI_RESUME:
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}

	ddi_report_dev(devi);

	return (DDI_SUCCESS);
}

#define	SET_RNGS(rng_p, used_p, ctyp, ptyp) do {			\
		(rng_p)->child_high = (ctyp);				\
		(rng_p)->child_low = (rng_p)->parent_low = (used_p)->base; \
		(rng_p)->parent_high = (ptyp);				\
		(rng_p)->parent_mid = 0;				\
		(rng_p)->size = (used_p)->len;				\
		_NOTE(CONSTCOND) } while (0)
static uint_t
isa_used_to_ranges(int ctype, int *array, uint_t size, pib_ranges_t *ranges)
{
	used_ranges_t *used_p;
	pib_ranges_t *rng_p = ranges;
	uint_t	i, ptype;

	ptype = (ctype == ISA_ADDR_IO) ? PCI_ADDR_IO : PCI_ADDR_MEM32;
	ptype |= PCI_REG_REL_M;
	size /= USED_CELL_SIZE;
	used_p = (used_ranges_t *)array;
	SET_RNGS(rng_p, used_p, ctype, ptype);
	for (i = 1, used_p++; i < size; i++, used_p++) {
		/* merge ranges record if applicable */
		if (rng_p->child_low + rng_p->size == used_p->base)
			rng_p->size += used_p->len;
		else {
			rng_p++;
			SET_RNGS(rng_p, used_p, ctype, ptype);
		}
	}
	return (rng_p - ranges + 1);
}

static void
isa_create_ranges_prop(dev_info_t *dip)
{
	dev_info_t *used;
	int *ioarray, *memarray, status;
	uint_t nio = 0, nmem = 0, nrng = 0, n;
	pib_ranges_t *ranges;

	used = ddi_find_devinfo(USED_RESOURCES, -1, 0);
	if (used == NULL) {
		cmn_err(CE_WARN, "Failed to find used-resources <%s>\n",
		    ddi_get_name(dip));
		return;
	}
	status = ddi_prop_lookup_int_array(DDI_DEV_T_ANY, used,
	    DDI_PROP_DONTPASS, "io-space", &ioarray, &nio);
	if (status != DDI_PROP_SUCCESS && status != DDI_PROP_NOT_FOUND) {
		cmn_err(CE_WARN, "io-space property failure for %s (%x)\n",
		    ddi_get_name(used), status);
		return;
	}
	status = ddi_prop_lookup_int_array(DDI_DEV_T_ANY, used,
	    DDI_PROP_DONTPASS, "device-memory", &memarray, &nmem);
	if (status != DDI_PROP_SUCCESS && status != DDI_PROP_NOT_FOUND) {
		cmn_err(CE_WARN, "device-memory property failure for %s (%x)\n",
		    ddi_get_name(used), status);
		return;
	}
	n = (nio + nmem) / USED_CELL_SIZE;
	ranges =  (pib_ranges_t *)kmem_zalloc(sizeof (pib_ranges_t) * n,
	    KM_SLEEP);

	if (nio != 0) {
		nrng = isa_used_to_ranges(ISA_ADDR_IO, ioarray, nio, ranges);
		ddi_prop_free(ioarray);
	}
	if (nmem != 0) {
		nrng += isa_used_to_ranges(ISA_ADDR_MEM, memarray, nmem,
		    ranges + nrng);
		ddi_prop_free(memarray);
	}

	if (!pseudo_isa)
		(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, dip, "ranges",
		    (int *)ranges, nrng * sizeof (pib_ranges_t) / sizeof (int));
	kmem_free(ranges, sizeof (pib_ranges_t) * n);
}

/*ARGSUSED*/
static int
isa_apply_range(dev_info_t *dip, struct regspec *isa_reg_p,
    pci_regspec_t *pci_reg_p)
{
	pib_ranges_t *ranges, *rng_p;
	int len, i, offset, nrange;

	if (ddi_getlongprop(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "ranges", (caddr_t)&ranges, &len) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "Can't get %s ranges property",
		    ddi_get_name(dip));
		return (DDI_ME_REGSPEC_RANGE);
	}
	nrange = len / sizeof (pib_ranges_t);
	rng_p = ranges;
	for (i = 0; i < nrange; i++, rng_p++) {
		/* Check for correct space */
		if (isa_reg_p->regspec_bustype != rng_p->child_high)
			continue;

		/* Detect whether request entirely fits within a range */
		if (isa_reg_p->regspec_addr < rng_p->child_low)
			continue;
		if ((isa_reg_p->regspec_addr + isa_reg_p->regspec_size - 1) >
		    (rng_p->child_low + rng_p->size - 1))
			continue;

		offset = isa_reg_p->regspec_addr - rng_p->child_low;

		pci_reg_p->pci_phys_hi = rng_p->parent_high;
		pci_reg_p->pci_phys_mid = 0;
		pci_reg_p->pci_phys_low = rng_p->parent_low + offset;
		pci_reg_p->pci_size_hi = 0;
		pci_reg_p->pci_size_low = isa_reg_p->regspec_size;

		break;
	}
	kmem_free(ranges, len);

	if (i < nrange)
		return (DDI_SUCCESS);

	/*
	 * Check extra resource range specially for serial and parallel
	 * devices, which are treated differently from all other ISA
	 * devices. On some machines, serial ports are not enumerated
	 * by ACPI but by BIOS, with io base addresses noted in legacy
	 * BIOS data area. Parallel port on some machines comes with
	 * illegal size.
	 */
	if (isa_reg_p->regspec_bustype != ISA_ADDR_IO) {
		cmn_err(CE_WARN, "Bus type not ISA I/O\n");
		return (DDI_ME_REGSPEC_RANGE);
	}

	for (i = 0; i < isa_extra_count; i++) {
		struct regspec *reg_p = &isa_extra_resource[i];

		if (isa_reg_p->regspec_addr < reg_p->regspec_addr)
			continue;
		if ((isa_reg_p->regspec_addr + isa_reg_p->regspec_size) >
		    (reg_p->regspec_addr + reg_p->regspec_size))
			continue;

		pci_reg_p->pci_phys_hi = PCI_ADDR_IO | PCI_REG_REL_M;
		pci_reg_p->pci_phys_mid = 0;
		pci_reg_p->pci_phys_low = isa_reg_p->regspec_addr;
		pci_reg_p->pci_size_hi = 0;
		pci_reg_p->pci_size_low = isa_reg_p->regspec_size;
		break;
	}
	if (i < isa_extra_count)
		return (DDI_SUCCESS);

	cmn_err(CE_WARN, "isa_apply_range: Out of range base <0x%x>, size <%d>",
	    isa_reg_p->regspec_addr, isa_reg_p->regspec_size);
	return (DDI_ME_REGSPEC_RANGE);
}

static int
isa_bus_map(dev_info_t *dip, dev_info_t *rdip, ddi_map_req_t *mp,
    off_t offset, off_t len, caddr_t *vaddrp)
{
	struct regspec tmp_reg, *rp;
	pci_regspec_t vreg;
	ddi_map_req_t mr = *mp;		/* Get private copy of request */
	int error;

	if (pseudo_isa)
		return (i_ddi_bus_map(dip, rdip, mp, offset, len, vaddrp));

	mp = &mr;

	/*
	 * First, if given an rnumber, convert it to a regspec...
	 */
	if (mp->map_type == DDI_MT_RNUMBER)  {

		int rnumber = mp->map_obj.rnumber;

		rp = i_ddi_rnumber_to_regspec(rdip, rnumber);
		if (rp == (struct regspec *)0)
			return (DDI_ME_RNUMBER_RANGE);

		/*
		 * Convert the given ddi_map_req_t from rnumber to regspec...
		 */
		mp->map_type = DDI_MT_REGSPEC;
		mp->map_obj.rp = rp;
	}

	/*
	 * Adjust offset and length correspnding to called values...
	 * XXX: A non-zero length means override the one in the regspec.
	 * XXX: (Regardless of what's in the parent's range)
	 */

	tmp_reg = *(mp->map_obj.rp);		/* Preserve underlying data */
	rp = mp->map_obj.rp = &tmp_reg;		/* Use tmp_reg in request */

	rp->regspec_addr += (uint_t)offset;
	if (len != 0)
		rp->regspec_size = (uint_t)len;

	if ((error = isa_apply_range(dip, rp, &vreg)) != 0)
		return (error);
	mp->map_obj.rp = (struct regspec *)&vreg;

	/*
	 * Call my parents bus_map function with modified values...
	 */

	return (ddi_map(dip, mp, (off_t)0, (off_t)0, vaddrp));
}

/*
 * Check if driver should be treated as an old pre 2.6 driver
 */
static int
old_driver(dev_info_t *dip)
{
	extern int ignore_hardware_nodes;	/* force flag from ddi_impl.c */

	if (ndi_dev_is_persistent_node(dip)) {
		if (ignore_hardware_nodes)
			return (1);
		if (ddi_getprop(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
		    "ignore-hardware-nodes", -1) != -1)
			return (1);
	}
	return (0);
}

typedef struct {
	uint32_t phys_hi;
	uint32_t phys_lo;
	uint32_t size;
} isa_regs_t;

/*
 * Return non-zero if device in tree is a PnP isa device
 */
static int
is_pnpisa(dev_info_t *dip)
{
	isa_regs_t *isa_regs;
	int proplen, pnpisa;

	if (ddi_getlongprop(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS, "reg",
	    (caddr_t)&isa_regs, &proplen) != DDI_PROP_SUCCESS) {
		return (0);
	}
	pnpisa = isa_regs[0].phys_hi & 0x80000000;
	/*
	 * free the memory allocated by ddi_getlongprop().
	 */
	kmem_free(isa_regs, proplen);
	if (pnpisa)
		return (1);
	else
		return (0);
}

/*ARGSUSED*/
static int
isa_ctlops(dev_info_t *dip, dev_info_t *rdip,
    ddi_ctl_enum_t ctlop, void *arg, void *result)
{
	int rn;
	struct ddi_parent_private_data *pdp;

	switch (ctlop) {
	case DDI_CTLOPS_REPORTDEV:
		if (rdip == (dev_info_t *)0)
			return (DDI_FAILURE);
		cmn_err(CE_CONT, "?ISA-device: %s%d\n",
		    ddi_driver_name(rdip), ddi_get_instance(rdip));
		return (DDI_SUCCESS);

	case DDI_CTLOPS_INITCHILD:
		/*
		 * older drivers aren't expecting the "standard" device
		 * node format used by the hardware nodes.  these drivers
		 * only expect their own properties set in their driver.conf
		 * files.  so they tell us not to call them with hardware
		 * nodes by setting the property "ignore-hardware-nodes".
		 */
		if (old_driver((dev_info_t *)arg)) {
			return (DDI_NOT_WELL_FORMED);
		}

		return (isa_initchild((dev_info_t *)arg));

	case DDI_CTLOPS_UNINITCHILD:
		impl_ddi_sunbus_removechild((dev_info_t *)arg);
		return (DDI_SUCCESS);

	case DDI_CTLOPS_SIDDEV:
		if (ndi_dev_is_persistent_node(rdip))
			return (DDI_SUCCESS);
		/*
		 * All ISA devices need to do confirming probes
		 * unless they are PnP ISA.
		 */
		if (is_pnpisa(rdip))
			return (DDI_SUCCESS);
		else
			return (DDI_FAILURE);

	case DDI_CTLOPS_REGSIZE:
	case DDI_CTLOPS_NREGS:
		if (rdip == (dev_info_t *)0)
			return (DDI_FAILURE);

		if ((pdp = ddi_get_parent_data(rdip)) == NULL)
			return (DDI_FAILURE);

		if (ctlop == DDI_CTLOPS_NREGS) {
			*(int *)result = pdp->par_nreg;
		} else {
			rn = *(int *)arg;
			if (rn >= pdp->par_nreg)
				return (DDI_FAILURE);
			*(off_t *)result = (off_t)pdp->par_reg[rn].regspec_size;
		}
		return (DDI_SUCCESS);

	case DDI_CTLOPS_ATTACH:
	case DDI_CTLOPS_DETACH:
	case DDI_CTLOPS_PEEK:
	case DDI_CTLOPS_POKE:
		return (DDI_FAILURE);

	default:
		return (ddi_ctlops(dip, rdip, ctlop, arg, result));
	}
}

static struct intrspec *
isa_get_ispec(dev_info_t *rdip, int inum)
{
	struct ddi_parent_private_data *pdp = ddi_get_parent_data(rdip);

	/* Validate the interrupt number */
	if (inum >= pdp->par_nintr)
		return (NULL);

	/* Get the interrupt structure pointer and return that */
	return ((struct intrspec *)&pdp->par_intr[inum]);
}

static int
isa_intr_ops(dev_info_t *pdip, dev_info_t *rdip, ddi_intr_op_t intr_op,
    ddi_intr_handle_impl_t *hdlp, void *result)
{
	struct intrspec *ispec;

	if (pseudo_isa)
		return (i_ddi_intr_ops(pdip, rdip, intr_op, hdlp, result));


	/* Process the interrupt operation */
	switch (intr_op) {
	case DDI_INTROP_GETCAP:
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_GET_CAP, result)) {
			*(int *)result = 0;
			return (DDI_FAILURE);
		}
		break;
	case DDI_INTROP_SETCAP:
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_SET_CAP, result))
			return (DDI_FAILURE);
		break;
	case DDI_INTROP_ALLOC:
		ASSERT(hdlp->ih_type == DDI_INTR_TYPE_FIXED);
		return (isa_alloc_intr_fixed(rdip, hdlp, result));
	case DDI_INTROP_FREE:
		ASSERT(hdlp->ih_type == DDI_INTR_TYPE_FIXED);
		return (isa_free_intr_fixed(rdip, hdlp));
	case DDI_INTROP_GETPRI:
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);
		*(int *)result = ispec->intrspec_pri;
		break;
	case DDI_INTROP_SETPRI:
		/* Validate the interrupt priority passed to us */
		if (*(int *)result > LOCK_LEVEL)
			return (DDI_FAILURE);

		/* Ensure that PSM is all initialized and ispec is ok */
		if ((psm_intr_ops == NULL) ||
		    ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL))
			return (DDI_FAILURE);

		/* update the ispec with the new priority */
		ispec->intrspec_pri =  *(int *)result;
		break;
	case DDI_INTROP_ADDISR:
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);
		ispec->intrspec_func = hdlp->ih_cb_func;
		break;
	case DDI_INTROP_REMISR:
		if (hdlp->ih_type != DDI_INTR_TYPE_FIXED)
			return (DDI_FAILURE);
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);
		ispec->intrspec_func = (uint_t (*)()) 0;
		break;
	case DDI_INTROP_ENABLE:
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);

		/* Call psmi to translate irq with the dip */
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		((ihdl_plat_t *)hdlp->ih_private)->ip_ispecp = ispec;
		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_XLATE_VECTOR,
		    (int *)&hdlp->ih_vector) == PSM_FAILURE)
			return (DDI_FAILURE);

		/* Add the interrupt handler */
		if (!add_avintr((void *)hdlp, ispec->intrspec_pri,
		    hdlp->ih_cb_func, DEVI(rdip)->devi_name, hdlp->ih_vector,
		    hdlp->ih_cb_arg1, hdlp->ih_cb_arg2, NULL, rdip))
			return (DDI_FAILURE);
		break;
	case DDI_INTROP_DISABLE:
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);

		/* Call psm_ops() to translate irq with the dip */
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		((ihdl_plat_t *)hdlp->ih_private)->ip_ispecp = ispec;
		(void) (*psm_intr_ops)(rdip, hdlp,
		    PSM_INTR_OP_XLATE_VECTOR, (int *)&hdlp->ih_vector);

		/* Remove the interrupt handler */
		rem_avintr((void *)hdlp, ispec->intrspec_pri,
		    hdlp->ih_cb_func, hdlp->ih_vector);
		break;
	case DDI_INTROP_SETMASK:
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_SET_MASK, NULL))
			return (DDI_FAILURE);
		break;
	case DDI_INTROP_CLRMASK:
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_CLEAR_MASK, NULL))
			return (DDI_FAILURE);
		break;
	case DDI_INTROP_GETPENDING:
		if (psm_intr_ops == NULL)
			return (DDI_FAILURE);

		if ((*psm_intr_ops)(rdip, hdlp, PSM_INTR_OP_GET_PENDING,
		    result)) {
			*(int *)result = 0;
			return (DDI_FAILURE);
		}
		break;
	case DDI_INTROP_NAVAIL:
	case DDI_INTROP_NINTRS:
		*(int *)result = i_ddi_get_intx_nintrs(rdip);
		if (*(int *)result == 0) {
			return (DDI_FAILURE);
		}
		break;
	case DDI_INTROP_SUPPORTED_TYPES:
		*(int *)result = DDI_INTR_TYPE_FIXED;	/* Always ... */
		break;
	default:
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

/*
 * Allocate interrupt vector for FIXED (legacy) type.
 */
static int
isa_alloc_intr_fixed(dev_info_t *rdip, ddi_intr_handle_impl_t *hdlp,
    void *result)
{
	struct intrspec		*ispec;
	ddi_intr_handle_impl_t	info_hdl;
	int			ret;
	int			free_phdl = 0;
	apic_get_type_t		type_info;

	if (psm_intr_ops == NULL)
		return (DDI_FAILURE);

	if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
		return (DDI_FAILURE);

	/*
	 * If the PSM module is "APIX" then pass the request for it
	 * to allocate the vector now.
	 */
	bzero(&info_hdl, sizeof (ddi_intr_handle_impl_t));
	info_hdl.ih_private = &type_info;
	if ((*psm_intr_ops)(NULL, &info_hdl, PSM_INTR_OP_APIC_TYPE, NULL) ==
	    PSM_SUCCESS && strcmp(type_info.avgi_type, APIC_APIX_NAME) == 0) {
		if (hdlp->ih_private == NULL) { /* allocate phdl structure */
			free_phdl = 1;
			i_ddi_alloc_intr_phdl(hdlp);
		}
		((ihdl_plat_t *)hdlp->ih_private)->ip_ispecp = ispec;
		ret = (*psm_intr_ops)(rdip, hdlp,
		    PSM_INTR_OP_ALLOC_VECTORS, result);
		if (free_phdl) { /* free up the phdl structure */
			free_phdl = 0;
			i_ddi_free_intr_phdl(hdlp);
			hdlp->ih_private = NULL;
		}
	} else {
		/*
		 * No APIX module; fall back to the old scheme where the
		 * interrupt vector is allocated during ddi_enable_intr() call.
		 */
		hdlp->ih_pri = ispec->intrspec_pri;
		*(int *)result = hdlp->ih_scratch1;
		ret = DDI_SUCCESS;
	}

	return (ret);
}

/*
 * Free up interrupt vector for FIXED (legacy) type.
 */
static int
isa_free_intr_fixed(dev_info_t *rdip, ddi_intr_handle_impl_t *hdlp)
{
	struct intrspec			*ispec;
	ddi_intr_handle_impl_t		info_hdl;
	int				ret;
	apic_get_type_t			type_info;

	if (psm_intr_ops == NULL)
		return (DDI_FAILURE);

	/*
	 * If the PSM module is "APIX" then pass the request for it
	 * to free up the vector now.
	 */
	bzero(&info_hdl, sizeof (ddi_intr_handle_impl_t));
	info_hdl.ih_private = &type_info;
	if ((*psm_intr_ops)(NULL, &info_hdl, PSM_INTR_OP_APIC_TYPE, NULL) ==
	    PSM_SUCCESS && strcmp(type_info.avgi_type, APIC_APIX_NAME) == 0) {
		if ((ispec = isa_get_ispec(rdip, hdlp->ih_inum)) == NULL)
			return (DDI_FAILURE);
		((ihdl_plat_t *)hdlp->ih_private)->ip_ispecp = ispec;
		ret = (*psm_intr_ops)(rdip, hdlp,
		    PSM_INTR_OP_FREE_VECTORS, NULL);
	} else {
		/*
		 * No APIX module; fall back to the old scheme where
		 * the interrupt vector was already freed during
		 * ddi_disable_intr() call.
		 */
		ret = DDI_SUCCESS;
	}

	return (ret);
}

static void
isa_vendor(uint32_t id, char *vendor)
{
	vendor[0] = '@' + ((id >> 26) & 0x1f);
	vendor[1] = '@' + ((id >> 21) & 0x1f);
	vendor[2] = '@' + ((id >> 16) & 0x1f);
	vendor[3] = 0;
}

/*
 * Name a child
 */
static int
isa_name_child(dev_info_t *child, char *name, int namelen)
{
	char vendor[8];
	int device;
	uint32_t serial;
	int func;
	int bustype;
	uint32_t base;
	int proplen;
	int pnpisa = 0;
	isa_regs_t *isa_regs;

	void make_ddi_ppd(dev_info_t *, struct ddi_parent_private_data **);

	/*
	 * older drivers aren't expecting the "standard" device
	 * node format used by the hardware nodes.  these drivers
	 * only expect their own properties set in their driver.conf
	 * files.  so they tell us not to call them with hardware
	 * nodes by setting the property "ignore-hardware-nodes".
	 */
	if (old_driver(child))
		return (DDI_FAILURE);

	/*
	 * Fill in parent-private data
	 */
	if (ddi_get_parent_data(child) == NULL) {
		struct ddi_parent_private_data *pdptr;
		make_ddi_ppd(child, &pdptr);
		ddi_set_parent_data(child, pdptr);
	}

	if (ndi_dev_is_persistent_node(child) == 0) {
		/*
		 * For .conf nodes, generate name from parent private data
		 */
		name[0] = '\0';
		if (sparc_pd_getnreg(child) > 0) {
			(void) snprintf(name, namelen, "%x,%x",
			    (uint_t)sparc_pd_getreg(child, 0)->regspec_bustype,
			    (uint_t)sparc_pd_getreg(child, 0)->regspec_addr);
		}
		return (DDI_SUCCESS);
	}

	/*
	 * For hw nodes, look up "reg" property
	 */
	if (ddi_getlongprop(DDI_DEV_T_ANY, child, DDI_PROP_DONTPASS, "reg",
	    (caddr_t)&isa_regs, &proplen) != DDI_PROP_SUCCESS) {
		return (DDI_FAILURE);
	}

	/*
	 * extract the device identifications
	 */
	pnpisa = isa_regs[0].phys_hi & 0x80000000;
	if (pnpisa) {
		isa_vendor(isa_regs[0].phys_hi, vendor);
		device = isa_regs[0].phys_hi & 0xffff;
		serial = isa_regs[0].phys_lo;
		func = (isa_regs[0].size >> 24) & 0xff;
		if (func != 0)
			(void) snprintf(name, namelen, "pnp%s,%04x,%x,%x",
			    vendor, device, serial, func);
		else
			(void) snprintf(name, namelen, "pnp%s,%04x,%x",
			    vendor, device, serial);
	} else {
		bustype = isa_regs[0].phys_hi;
		base = isa_regs[0].phys_lo;
		(void) sprintf(name, "%x,%x", bustype, base);
	}

	/*
	 * free the memory allocated by ddi_getlongprop().
	 */
	kmem_free(isa_regs, proplen);

	return (DDI_SUCCESS);
}

static int
isa_initchild(dev_info_t *child)
{
	char name[80];

	if (isa_name_child(child, name, 80) != DDI_SUCCESS)
		return (DDI_FAILURE);
	ddi_set_name_addr(child, name);

	if (ndi_dev_is_persistent_node(child) != 0)
		return (DDI_SUCCESS);

	/*
	 * This is a .conf node, try merge properties onto a
	 * hw node with the same name.
	 */
	if (ndi_merge_node(child, isa_name_child) == DDI_SUCCESS) {
		/*
		 * Return failure to remove node
		 */
		impl_ddi_sunbus_removechild(child);
		return (DDI_FAILURE);
	}
	/*
	 * Cannot merge node, permit pseudo children
	 */
	return (DDI_SUCCESS);
}

/*
 * called when ACPI enumeration is not used
 */
static void
add_known_used_resources(void)
{
	/* needs to be in increasing order */
	int intr[] = {0x3};
	int io[] = {0x3f8, 0x8};
	dev_info_t *usedrdip;

	usedrdip = ddi_find_devinfo(USED_RESOURCES, -1, 0);

	if (usedrdip == NULL) {
		(void) ndi_devi_alloc_sleep(ddi_root_node(), USED_RESOURCES,
		    (pnode_t)DEVI_SID_NODEID, &usedrdip);
	}

	(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, usedrdip,
	    "interrupts", (int *)intr, (int)(sizeof (intr) / sizeof (int)));
	(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, usedrdip,
	    "io-space", (int *)io, (int)(sizeof (io) / sizeof (int)));
	(void) ndi_devi_bind_driver(usedrdip, 0);
}

/*
 * Return non-zero if UART device exists.
 */
static int
uart_exists(ushort_t port)
{
	outb(port + COM_SCR, (char)0x5a);
	outb(port + COM_ISR, (char)0x00);
	return (inb(port + COM_SCR) == (char)0x5a);
}

static void
isa_enumerate(int reprogram)
{
	int circ, i;
	dev_info_t *xdip;
	dev_info_t *isa_dip = ddi_find_devinfo("isa", -1, 0);

	if (reprogram || !isa_dip)
		return;

	bzero(isa_extra_resource, MAX_EXTRA_RESOURCE * sizeof (struct regspec));

	ndi_devi_enter(isa_dip, &circ);

	/*
	 * XXX This belongs somewhere else (apix?) but then this whole
	 * thing is a hack so whatever.  Disable all legacy IDE/SATA
	 * interrupts and instead allow use of the numbers they use on
	 * PCs for other things.
	 */
	outb(0xc00, 0x08);
	outb(0xc01, 0xff);

	/*
	 * Enable interrupts.
	 */
	outb(0xc00, 0x09);
	outb(0xc01, 0xf7);

	/* serial ports - we have only the one, which is for the console */
	for (i = 0; i < N_ASY; i++) {
		ushort_t addr = asy_regs[i].regspec_addr;
		caddr_t ahb_reg_page = psm_map_phys_new(0xfedc0000,
		    MMU_PAGESIZE, PROT_READ | PROT_WRITE);

		ASSERT3U(addr, ==, 0x3f8);
		/*
		 * XXX Undocumented register believed to look like this:
		 *
		 *  15 14 13 12 11 10 9   8 7  4 3   0
		 * | P3A | P2A | P1A | P0A | -- | DEC |
		 *
		 * where PxA = port x address selector:
		 *	00 => 0x2e8
		 *	01 => 0x2f8
		 *	10 => 0x3e8
		 *	11 => 0x3f8
		 *
		 * 'x' is the physical port number, so this controls the routing
		 * of IO space to each physical port.
		 *
		 * DEC = decode bits, each can be set individually:
		 *	0 => decode 0x2e8/3
		 *	1 => decode 0x2f8/3
		 *	2 => decode 0x3e8/3
		 *	3 => decode 0x3f8/3
		 *
		 * Thus, note that this allows us to do things that make no
		 * sense, like route an address to multiple ports or select an
		 * address for a port for which we haven't enabled decoding.
		 * Presumably such things work poorly or not at all.  We care
		 * only about port 0, which we want to have address 0x3f8, so
		 * we need to set [9:8] to 0b11 and set bit 3.  Probably.  It
		 * is unknown whether [7:4] do anything.
		 */
		*(uint16_t *)(ahb_reg_page + 0x20) = 0x0308;
		psm_unmap_phys(ahb_reg_page, MMU_PAGESIZE);

		if (!uart_exists(addr))
			continue;

		ndi_devi_alloc_sleep(isa_dip, "asy",
		    (pnode_t)DEVI_SID_NODEID, &xdip);
		(void) ndi_prop_update_string(DDI_DEV_T_NONE, xdip,
		    "model", "AMD legacy UART hack");
		(void) ndi_prop_update_int_array(DDI_DEV_T_NONE, xdip,
		    "reg", (int *)&asy_regs[i], 3);
		(void) ndi_prop_update_int(DDI_DEV_T_NONE, xdip,
		    "interrupts", asy_intrs[i]);
		(void) ndi_devi_bind_driver(xdip, 0);
		/* Adjusting isa_extra here causes a kernel dump later. */

		/*
		 * XXX Move to huashan intr_ops path.
		 * Set up the IOAPIC pin for this UART; no PIC support ever!
		 */
		outb(0xc00, 0xf4);
		outb(0xc01, asy_intrs[i]);
	}

	add_known_used_resources();

	ndi_devi_exit(isa_dip, circ);

	isa_create_ranges_prop(isa_dip);
}
