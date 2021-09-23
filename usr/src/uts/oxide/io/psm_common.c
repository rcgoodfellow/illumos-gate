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
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2022 Oxide Computer Co.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cmn_err.h>
#include <sys/promif.h>
#include <sys/sunddi.h>
#include <sys/ddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/pci.h>
#include <sys/debug.h>
#include <sys/psm_common.h>
#include <sys/sunndi.h>
#include <sys/ksynch.h>

/* Global configurables */

char *psm_module_name;	/* used to store name of psm module */

/*
 * acpi_irq_check_elcr: when set elcr will also be consulted for building
 * the reserved irq list.  When 0 (false), the existing state of the ELCR
 * is ignored when selecting a vector during IRQ translation, and the ELCR
 * is programmed to the proper setting for the type of bus (level-triggered
 * for PCI, edge-triggered for non-PCI).  When non-zero (true), vectors
 * set to edge-mode will not be used when in PIC-mode.  The default value
 * is 0 (false).  Note that ACPI's SCI vector is always set to conform to
 * ACPI-specification regardless of this.
 *
 */
int acpi_irq_check_elcr = 0;

int psm_verbose = 0;

#define	PSM_VERBOSE_IRQ(fmt)	\
		if (psm_verbose & PSM_VERBOSE_IRQ_FLAG) \
			cmn_err fmt;

#define	PSM_VERBOSE_POWEROFF(fmt)  \
		if (psm_verbose & PSM_VERBOSE_POWEROFF_FLAG || \
		    psm_verbose & PSM_VERBOSE_POWEROFF_PAUSE_FLAG) \
			prom_printf fmt;

#define	PSM_VERBOSE_POWEROFF_PAUSE(fmt) \
		if (psm_verbose & PSM_VERBOSE_POWEROFF_FLAG || \
		    psm_verbose & PSM_VERBOSE_POWEROFF_PAUSE_FLAG) {\
			prom_printf fmt; \
			if (psm_verbose & PSM_VERBOSE_POWEROFF_PAUSE_FLAG) \
				(void) goany(); \
		}


/* Local storage */
static kmutex_t acpi_irq_cache_mutex;

/*
 * irq_cache_table is a list that serves a two-key cache. It is used
 * as a pci busid/devid/ipin <-> irq cache and also as a acpi
 * interrupt lnk <-> irq cache.
 */
static irq_cache_t *irq_cache_table;

#define	IRQ_CACHE_INITLEN	20
static int irq_cache_len = 0;
static int irq_cache_valid = 0;

extern int goany(void);


#define	NEXT_PRT_ITEM(p)	\
		(void *)(((char *)(p)) + (p)->Length)

int
acpi_psm_init(char *module_name, int verbose_flags)
{
	psm_module_name = module_name;
	psm_verbose = verbose_flags;

	return (ACPI_PSM_FAILURE);
}

/*
 * Return bus/dev/fn for PCI dip (note: not the parent "pci" node).
 */

int
get_bdf(dev_info_t *dip, int *bus, int *device, int *func)
{
	pci_regspec_t *pci_rp;
	int len;

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "reg", (int **)&pci_rp, (uint_t *)&len) != DDI_SUCCESS)
		return (-1);

	if (len < (sizeof (pci_regspec_t) / sizeof (int))) {
		ddi_prop_free(pci_rp);
		return (-1);
	}
	if (bus != NULL)
		*bus = (int)PCI_REG_BUS_G(pci_rp->pci_phys_hi);
	if (device != NULL)
		*device = (int)PCI_REG_DEV_G(pci_rp->pci_phys_hi);
	if (func != NULL)
		*func = (int)PCI_REG_FUNC_G(pci_rp->pci_phys_hi);
	ddi_prop_free(pci_rp);
	return (0);
}


/*
 * Build the reserved ISA irq list, and store it in the table pointed to by
 * reserved_irqs_table. The caller is responsible for allocating this table
 * with a minimum of MAX_ISA_IRQ + 1 entries.
 *
 * The routine looks in the device tree at the subtree rooted at /isa
 * for each of the devices under that node, if an interrupts property
 * is present, its values are used to "reserve" irqs so that later ACPI
 * configuration won't choose those irqs.
 *
 * In addition, if acpi_irq_check_elcr is set, will use ELCR register
 * to identify reserved IRQs.
 */
void
build_reserved_irqlist(uchar_t *reserved_irqs_table)
{
	dev_info_t *isanode = ddi_find_devinfo("isa", -1, 0);
	dev_info_t *isa_child = 0;
	int i;
	uint_t	elcrval;

	/* Initialize the reserved ISA IRQs: */
	for (i = 0; i <= MAX_ISA_IRQ; i++)
		reserved_irqs_table[i] = 0;

	if (acpi_irq_check_elcr) {

		elcrval = (inb(ELCR_PORT2) << 8) | (inb(ELCR_PORT1));
		if (ELCR_EDGE(elcrval, 0) && ELCR_EDGE(elcrval, 1) &&
		    ELCR_EDGE(elcrval, 2) && ELCR_EDGE(elcrval, 8) &&
		    ELCR_EDGE(elcrval, 13)) {
			/* valid ELCR */
			for (i = 0; i <= MAX_ISA_IRQ; i++)
				if (!ELCR_LEVEL(elcrval, i))
					reserved_irqs_table[i] = 1;
		}
	}

	/* always check the isa devinfo nodes */

	if (isanode != 0) { /* Found ISA */
		uint_t intcnt;		/* Interrupt count */
		int *intrs;		/* Interrupt values */

		/* Load first child: */
		isa_child = ddi_get_child(isanode);
		while (isa_child != 0) { /* Iterate over /isa children */
			/* if child has any interrupts, save them */
			if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, isa_child,
			    DDI_PROP_DONTPASS, "interrupts", &intrs, &intcnt)
			    == DDI_PROP_SUCCESS) {
				/*
				 * iterate over child interrupt list, adding
				 * them to the reserved irq list
				 */
				while (intcnt-- > 0) {
					/*
					 * Each value MUST be <= MAX_ISA_IRQ
					 */

					if ((intrs[intcnt] > MAX_ISA_IRQ) ||
					    (intrs[intcnt] < 0))
						continue;

					reserved_irqs_table[intrs[intcnt]] = 1;
				}
				ddi_prop_free(intrs);
			}
			isa_child = ddi_get_next_sibling(isa_child);
		}
		/* The isa node was held by ddi_find_devinfo, so release it */
		ndi_rele_devi(isanode);
	}

	/*
	 * Reserve IRQ14 & IRQ15 for IDE.  It shouldn't be hard-coded
	 * here but there's no other way to find the irqs for
	 * legacy-mode ata (since it's hard-coded in pci-ide also).
	 */
	reserved_irqs_table[14] = 1;
	reserved_irqs_table[15] = 1;
}

/*
 * XXX Placeholders; no ACPI.
 */
int
acpi_translate_pci_irq(dev_info_t *dip, int ipin, int *pci_irqp,
    iflag_t *intr_flagp, acpi_psm_lnk_t *acpipsmlnkp)
{
	return (ACPI_PSM_FAILURE);
}

int
acpi_set_irq_resource(acpi_psm_lnk_t *acpipsmlnkp, int irq)
{
	return (ACPI_PSM_FAILURE);
}

int
acpi_get_current_irq_resource(acpi_psm_lnk_t *acpipsmlnkp, int *pci_irqp,
    iflag_t *intr_flagp)
{
	return (ACPI_PSM_FAILURE);
}

/*
 * Searches for the given IRQ in the irqlist passed in.
 *
 * If multiple matches exist, this returns true on the first match.
 * Returns the interrupt flags, if a match was found, in `intr_flagp' if
 * it's passed in non-NULL
 */
int
acpi_irqlist_find_irq(acpi_irqlist_t *irqlistp, int irq, iflag_t *intr_flagp)
{
	int found = 0;
	int i;

	while (irqlistp != NULL && !found) {
		for (i = 0; i < irqlistp->num_irqs; i++) {
			if (irqlistp->irqs[i] == irq) {
				if (intr_flagp)
					*intr_flagp = irqlistp->intr_flags;
				found = 1;
				break;	/* out of for() */
			}
		}
	}

	return (found ? ACPI_PSM_SUCCESS : ACPI_PSM_FAILURE);
}

/*
 * Frees the irqlist allocated by acpi_get_possible_irq_resource.
 * It takes a count of number of entries in the list.
 */
void
acpi_free_irqlist(acpi_irqlist_t *irqlistp)
{
	acpi_irqlist_t *freednode;

	while (irqlistp != NULL) {
		/* Free the irq list */
		kmem_free(irqlistp->irqs, irqlistp->num_irqs *
		    sizeof (int32_t));

		freednode = irqlistp;
		irqlistp = irqlistp->next;
		kmem_free(freednode, sizeof (acpi_irqlist_t));
	}
}

int
acpi_get_possible_irq_resources(acpi_psm_lnk_t *acpipsmlnkp,
    acpi_irqlist_t **irqlistp)
{
	return (ACPI_PSM_FAILURE);
}

/*
 * Adds a new cache entry to the irq cache which maps an irq and
 * its attributes to PCI bus/dev/ipin and optionally to its associated ACPI
 * interrupt link device object.
 */
void
acpi_new_irq_cache_ent(int bus, int dev, int ipin, int pci_irq,
    iflag_t *intr_flagp, acpi_psm_lnk_t *acpipsmlnkp)
{
	int newsize;
	irq_cache_t *new_arr, *ep;

	mutex_enter(&acpi_irq_cache_mutex);
	if (irq_cache_valid >= irq_cache_len) {
		/* initially, or re-, allocate array */

		newsize = (irq_cache_len ?
		    irq_cache_len * 2 : IRQ_CACHE_INITLEN);
		new_arr = kmem_zalloc(newsize * sizeof (irq_cache_t), KM_SLEEP);
		if (irq_cache_len != 0) {
			/* realloc: copy data, free old */
			bcopy(irq_cache_table, new_arr,
			    irq_cache_len * sizeof (irq_cache_t));
			kmem_free(irq_cache_table,
			    irq_cache_len * sizeof (irq_cache_t));
		}
		irq_cache_len = newsize;
		irq_cache_table = new_arr;
	}
	ep = &irq_cache_table[irq_cache_valid++];
	ep->bus = (uchar_t)bus;
	ep->dev = (uchar_t)dev;
	ep->ipin = (uchar_t)ipin;
	ep->flags = *intr_flagp;
	ep->irq = (uchar_t)pci_irq;
	ASSERT(acpipsmlnkp != NULL);
	ep->lnkobj = acpipsmlnkp->lnkobj;
	mutex_exit(&acpi_irq_cache_mutex);
}


/*
 * Searches the irq caches for the given bus/dev/ipin.
 *
 * If info is found, stores polarity and sensitivity in the structure
 * pointed to by intr_flagp, and irqno in the value pointed to by pci_irqp,
 * and returns ACPI_PSM_SUCCESS.
 * Otherwise, ACPI_PSM_FAILURE is returned.
 */
int
acpi_get_irq_cache_ent(uchar_t bus, uchar_t dev, int ipin,
    int *pci_irqp, iflag_t *intr_flagp)
{

	irq_cache_t *irqcachep;
	int i;
	int ret = ACPI_PSM_FAILURE;

	mutex_enter(&acpi_irq_cache_mutex);
	for (irqcachep = irq_cache_table, i = 0; i < irq_cache_valid;
	    irqcachep++, i++)
		if ((irqcachep->bus == bus) &&
		    (irqcachep->dev == dev) &&
		    (irqcachep->ipin == ipin)) {
			ASSERT(pci_irqp != NULL && intr_flagp != NULL);
			*pci_irqp = irqcachep->irq;
			*intr_flagp = irqcachep->flags;
			ret = ACPI_PSM_SUCCESS;
			break;
		}

	mutex_exit(&acpi_irq_cache_mutex);
	return (ret);
}

/*
 * Walk the irq_cache_table and re-configure the link device to
 * the saved state.
 */
void
acpi_restore_link_devices(void)
{
	irq_cache_t *irqcachep;
	acpi_psm_lnk_t psmlnk;
	int i, status;

	/* XXX: may not need to hold this mutex */
	mutex_enter(&acpi_irq_cache_mutex);
	for (irqcachep = irq_cache_table, i = 0; i < irq_cache_valid;
	    irqcachep++, i++) {
		if (irqcachep->lnkobj != NULL) {
			/* only field used from psmlnk in set_irq is lnkobj */
			psmlnk.lnkobj = irqcachep->lnkobj;
			status = acpi_set_irq_resource(&psmlnk, irqcachep->irq);
			/* warn if set_irq failed; soldier on */
			if (status != ACPI_PSM_SUCCESS)
				cmn_err(CE_WARN, "Could not restore interrupt "
				    "link device for IRQ 0x%x: Devices using "
				    "this IRQ may no longer function properly."
				    "\n", irqcachep->irq);
		}
	}
	mutex_exit(&acpi_irq_cache_mutex);
}

int
acpi_poweroff(void)
{
	return (1);
}


/*
 * psm_set_elcr() sets ELCR bit for specified vector
 */
void
psm_set_elcr(int vecno, int val)
{
	int elcr_port = ELCR_PORT1 + (vecno >> 3);
	int elcr_bit = 1 << (vecno & 0x07);

	ASSERT((vecno >= 0) && (vecno < 16));

	if (val) {
		/* set bit to force level-triggered mode */
		outb(elcr_port, inb(elcr_port) | elcr_bit);
	} else {
		/* clear bit to force edge-triggered mode */
		outb(elcr_port, inb(elcr_port) & ~elcr_bit);
	}
}

/*
 * psm_get_elcr() returns status of ELCR bit for specific vector
 */
int
psm_get_elcr(int vecno)
{
	int elcr_port = ELCR_PORT1 + (vecno >> 3);
	int elcr_bit = 1 << (vecno & 0x07);

	ASSERT((vecno >= 0) && (vecno < 16));

	return ((inb(elcr_port) & elcr_bit) ? 1 : 0);
}
