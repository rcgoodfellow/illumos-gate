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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2016 Nexenta Systems, Inc.
 * Copyright (c) 2017 by Delphix. All rights reserved.
 * Copyright (c) 2019, Joyent, Inc.
 * Copyright 2020 RackTop Systems, Inc.
 * Copyright 2022 Oxide Computer Company
 */
/*
 * Copyright (c) 2010, Intel Corporation.
 * All rights reserved.
 */

/*
 * PSMI 1.1 extensions are supported only in 2.6 and later versions.
 * PSMI 1.2 extensions are supported only in 2.7 and later versions.
 * PSMI 1.3 and 1.4 extensions are supported in Solaris 10.
 * PSMI 1.5 extensions are supported in Solaris Nevada.
 * PSMI 1.6 extensions are supported in Solaris Nevada.
 * PSMI 1.7 extensions are supported in Solaris Nevada.
 */
#define	PSMI_1_7

#include <sys/processor.h>
#include <sys/time.h>
#include <sys/psm.h>
#include <sys/smp_impldefs.h>
#include <sys/cram.h>
#include <sys/psm_common.h>
#include <sys/apic.h>
#include <sys/apic_timer.h>
#include <sys/pit.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/pci.h>
#include <sys/promif.h>
#include <sys/x86_archext.h>
#include <sys/cpc_impl.h>
#include <sys/uadmin.h>
#include <sys/panic.h>
#include <sys/debug.h>
#include <sys/archsystm.h>
#include <sys/trap.h>
#include <sys/machsystm.h>
#include <sys/cpuvar.h>
#include <sys/rm_platter.h>
#include <sys/privregs.h>
#include <sys/cyclic.h>
#include <sys/note.h>
#include <sys/pci_intr_lib.h>
#include <sys/sunndi.h>
#include <sys/prom_debug.h>
#include <sys/hpet.h>
#include <sys/clock.h>
#include <milan/milan_physaddrs.h>
#include <sys/io/huashan/pmio.h>

/*
 *	Local Function Prototypes
 */
static int apic_find_bus(int busid);
static struct apic_io_intr *apic_find_io_intr(int irqno);
static int apic_find_free_irq(int start, int end);
struct apic_io_intr *apic_find_io_intr_w_busid(int irqno, int busid);

int apic_handle_pci_pci_bridge(dev_info_t *idip, int child_devno,
    int child_ipin, struct apic_io_intr **intrp);
int apic_find_bus_id(int bustype);
int apic_find_intin(uchar_t ioapic, uchar_t intin);
void apic_record_rdt_entry(apic_irq_t *irqptr, int irq);

int apic_debug_mps_id = 0;	/* 1 - print MPS ID strings */

/* ACPI SCI interrupt configuration; -1 if SCI not used */
int apic_sci_vect = -1;
iflag_t apic_sci_flags;

/* ACPI HPET interrupt configuration; -1 if HPET not used */
int apic_hpet_vect = -1;
iflag_t apic_hpet_flags;

/*
 * psm name pointer
 */
char *psm_name;

static int apic_probe_raw(const char *);

static int apic_acpi_irq_configure(acpi_psm_lnk_t *acpipsmlnkp, dev_info_t *dip,
    int *pci_irqp, iflag_t *intr_flagp);

int apic_acpi_translate_pci_irq(dev_info_t *dip, int busid, int devid,
    int ipin, int *pci_irqp, iflag_t *intr_flagp);
uchar_t acpi_find_ioapic(int irq);
static int acpi_intr_compatible(iflag_t iflag1, iflag_t iflag2);

/* Max wait time (in repetitions) for flags to clear in an RDT entry. */
int apic_max_reps_clear_pending = 1000;

int	apic_intr_policy = INTR_ROUND_ROBIN;

int	apic_next_bind_cpu = 1; /* For round robin assignment */
				/* start with cpu 1 */

/*
 * If enabled, the distribution works as follows:
 * On every interrupt entry, the current ipl for the CPU is set in cpu_info
 * and the irq corresponding to the ipl is also set in the aci_current array.
 * interrupt exit and setspl (due to soft interrupts) will cause the current
 * ipl to be be changed. This is cache friendly as these frequently used
 * paths write into a per cpu structure.
 *
 * Sampling is done by checking the structures for all CPUs and incrementing
 * the busy field of the irq (if any) executing on each CPU and the busy field
 * of the corresponding CPU.
 * In periodic mode this is done on every clock interrupt.
 * In one-shot mode, this is done thru a cyclic with an interval of
 * apic_redistribute_sample_interval (default 10 milli sec).
 *
 * Every apic_sample_factor_redistribution times we sample, we do computations
 * to decide which interrupt needs to be migrated (see comments
 * before apic_intr_redistribute().
 */

/*
 * Following 3 variables start as % and can be patched or set using an
 * API to be defined in future. They will be scaled to
 * sample_factor_redistribution which is in turn set to hertz+1 (in periodic
 * mode), or 101 in one-shot mode to stagger it away from one sec processing
 */

int	apic_int_busy_mark = 60;
int	apic_int_free_mark = 20;
int	apic_diff_for_redistribution = 10;

/* sampling interval for interrupt redistribution for dynamic migration */
int	apic_redistribute_sample_interval = NANOSEC / 100; /* 10 millisec */

/*
 * number of times we sample before deciding to redistribute interrupts
 * for dynamic migration
 */
int	apic_sample_factor_redistribution = 101;

int	apic_redist_cpu_skip = 0;
int	apic_num_imbalance = 0;
int	apic_num_rebind = 0;

/*
 * Maximum number of APIC CPUs in the system, -1 indicates that dynamic
 * allocation of CPU ids is disabled.
 */
int	apic_max_nproc = -1;
int	apic_nproc = 0;
size_t	apic_cpus_size = 0;
int	apic_defconf = 0;
int	apic_irq_translate = 0;
int	apic_spec_rev = 0;
int	apic_imcrp = 0;

int	apic_use_acpi_madt_only = 0;	/* 1=ONLY use MADT from ACPI */

/*
 * For interrupt link devices, if apic_unconditional_srs is set, an irq resource
 * will be assigned (via _SRS). If it is not set, use the current
 * irq setting (via _CRS), but only if that irq is in the set of possible
 * irqs (returned by _PRS) for the device.
 */
int	apic_unconditional_srs = 1;

/*
 * For interrupt link devices, if apic_prefer_crs is set when we are
 * assigning an IRQ resource to a device, prefer the current IRQ setting
 * over other possible irq settings under same conditions.
 */

int	apic_prefer_crs = 1;

uchar_t apic_io_id[MAX_IO_APIC];
volatile uint32_t *apicioadr[MAX_IO_APIC];
uchar_t	apic_io_ver[MAX_IO_APIC];
uchar_t	apic_io_vectbase[MAX_IO_APIC];
uchar_t	apic_io_vectend[MAX_IO_APIC];
uchar_t apic_reserved_irqlist[MAX_ISA_IRQ + 1];
uint32_t apic_physaddr[MAX_IO_APIC];

boolean_t ioapic_mask_workaround[MAX_IO_APIC];

/*
 * First available slot to be used as IRQ index into the apic_irq_table
 * for those interrupts (like MSI/X) that don't have a physical IRQ.
 */
int apic_first_avail_irq  = APIC_FIRST_FREE_IRQ;

/*
 * apic_ioapic_lock protects the ioapics (reg select), the status, temp_bound
 * and bound elements of cpus_info and the temp_cpu element of irq_struct
 */
lock_t	apic_ioapic_lock;

int	apic_io_max = 0;	/* no. of i/o apics enabled */

struct apic_io_intr *apic_io_intrp = NULL;

uchar_t	apic_resv_vector[MAXIPL+1];

char	apic_level_intr[APIC_MAX_VECTOR+1];

uint32_t	eisa_level_intr_mask = 0;
	/* At least MSB will be set if EISA bus */

int	apic_pci_bus_total = 0;
uchar_t	apic_single_pci_busid = 0;

/*
 * airq_mutex protects additions to the apic_irq_table - the first
 * pointer and any airq_nexts off of that one. It also protects
 * apic_max_device_irq & apic_min_device_irq. It also guarantees
 * that share_id is unique as new ids are generated only when new
 * irq_t structs are linked in. Once linked in the structs are never
 * deleted. temp_cpu & mps_intr_index field indicate if it is programmed
 * or allocated. Note that there is a slight gap between allocating in
 * apic_introp_xlate and programming in addspl.
 */
kmutex_t	airq_mutex;
apic_irq_t	*apic_irq_table[APIC_MAX_VECTOR+1];
int		apic_max_device_irq = 0;
int		apic_min_device_irq = APIC_MAX_VECTOR;

typedef struct prs_irq_list_ent {
	int			list_prio;
	int32_t			irq;
	iflag_t			intrflags;
	acpi_prs_private_t	prsprv;
	struct prs_irq_list_ent	*next;
} prs_irq_list_t;


/*
 * ACPI variables
 */
/* 1 = acpi is enabled & working, 0 = acpi is not enabled or not there */
int apic_enable_acpi = 0;

/* ACPI Interrupt Source Override Structure ptr */
ACPI_MADT_INTERRUPT_OVERRIDE *acpi_isop = NULL;
int acpi_iso_cnt = 0;

int	apic_poweroff_method = APIC_POWEROFF_NONE;

/*
 * Auto-configuration routines
 */

/*
 * Look at MPSpec 1.4 (Intel Order # 242016-005) for details of what we do here
 * May work with 1.1 - but not guaranteed.
 * According to the MP Spec, the MP floating pointer structure
 * will be searched in the order described below:
 * 1. In the first kilobyte of Extended BIOS Data Area (EBDA)
 * 2. Within the last kilobyte of system base memory
 * 3. In the BIOS ROM address space between 0F0000h and 0FFFFh
 * Once we find the right signature with proper checksum, we call
 * either handle_defconf or parse_mpct to get all info necessary for
 * subsequent operations.
 */
int
apic_probe_common(char *modname)
{
	int	i, retval = PSM_FAILURE;

	PRM_POINT("apic_probe_common()");

	if (apic_forceload < 0)
		return (retval);

	/*
	 * Remember who we are
	 */
	psm_name = modname;

	PRM_POINT("apic_probe_raw()");
	retval = apic_probe_raw(modname);
	PRM_DEBUG(retval);

	if (retval == PSM_SUCCESS) {
		extern int apic_ioapic_method_probe();

		PRM_POINT("apic_ioapic_method_probe()");
		if ((retval = apic_ioapic_method_probe()) == PSM_SUCCESS) {
			PRM_POINT("SUCCESS");
			return (PSM_SUCCESS);
		}
	}

	for (i = 0; i < apic_io_max; i++)
		mapout_ioapic((caddr_t)apicioadr[i], APIC_IO_MEMLEN);
	if (apic_cpus) {
		kmem_free(apic_cpus, apic_cpus_size);
		apic_cpus = NULL;
	}
	if (apicadr) {
		mapout_apic((caddr_t)apicadr, APIC_LOCAL_MEMLEN);
		apicadr = NULL;
	}

	PRM_DEBUG(retval);
	return (retval);
}

static int
apic_probe_raw(const char *modname)
{
	int i;
	uint32_t irqno;
	caddr_t pmbase;
	const size_t pmsize = FCH_R_BLOCK_GETSIZE(PM);
	FCH_REG_TYPE(PM, DECODEEN) decodeen = 0;

	apic_nproc = 1;
	apic_cpus_size = max(apic_nproc, max_ncpus) * sizeof (*apic_cpus);
	if ((apic_cpus = kmem_zalloc(apic_cpus_size, KM_NOSLEEP)) == NULL) {
		apic_max_nproc = -1;
		apic_nproc = 0;
		return (PSM_FAILURE);
	}

	apic_enable_x2apic();

	apic_cpus[0].aci_local_id = 0;
	apic_cpus[0].aci_local_ver = (uchar_t)
	    (apic_reg_ops->apic_read(APIC_VERS_REG) & 0xff);
	apic_cpus[0].aci_processor_id = 0;
	apic_cpus[0].aci_status = 0;

	/*
	 * What is it?  They have expressly refused to tell us (that is, not
	 * merely begged insufficient resources to document, nor claimed that
	 * no one is around who knows; they have instead told us that the
	 * knowledge itself is forbidden to us).  What do the bits mean?
	 * They have expressly refused to tell us that, too.  All we know is
	 * that the APIC doesn't seem to work without it.  Way to go guys.
	 */
	wrmsr(0xc00110e2, 0x00022afa00080018UL);

	pmbase = psm_map_phys(FCH_MR_BLOCK_GETPA(PM), pmsize,
	    PROT_READ | PROT_WRITE);
	decodeen = FCH_MR_READ(PM, DECODEEN, pmbase);
	decodeen = FCH_R_SET_B(PM, DECODEEN, IOAPICEN, decodeen, 1);
	FCH_MR_WRITE(PM, DECODEEN, pmbase, decodeen);
	psm_unmap_phys(pmbase, pmsize);

	apic_io_id[0] = 0xf0;
	apic_physaddr[0] = MILAN_PHYSADDR_FCH_IOAPIC;
	apicioadr[0] = (void *)mapin_ioapic(apic_physaddr[0], APIC_IO_MEMLEN,
	    PROT_READ | PROT_WRITE);

	apic_io_id[1] = 0xf1;
	apic_physaddr[1] = MILAN_PHYSADDR_IOHC_IOAPIC;
	apicioadr[1] = (void *)mapin_ioapic(apic_physaddr[1], APIC_IO_MEMLEN,
	    PROT_READ | PROT_WRITE);

	apic_io_max = 2;

	for (i = 0, irqno = 0; i < apic_io_max; i++) {
		uint32_t ver = ioapic_read(i, APIC_VERS_CMD);
		uint32_t nent = ((ver >> 16) & 0xff);

		apic_io_ver[i] = (uint8_t)(ver & 0xff);

		ASSERT3U(irqno, <, 256);
		apic_io_vectbase[i] = (uint8_t)irqno;

		ASSERT3U(nent, <=, 256 - irqno);
		irqno += nent;
		apic_io_vectend[i] = apic_io_vectbase[i] + (uint8_t)(nent - 1);

		ioapic_write(i, APIC_ID_CMD, apic_io_id[i] << 24);

		if (apic_first_avail_irq <= apic_io_vectend[i])
			apic_first_avail_irq = apic_io_vectend[i] + 1;
	}

	return (PSM_SUCCESS);
}

boolean_t
apic_cpu_in_range(int cpu)
{
	cpu &= ~IRQ_USER_BOUND;
	/* Check whether cpu id is in valid range. */
	if (cpu < 0 || cpu >= apic_nproc) {
		return (B_FALSE);
	} else if (apic_max_nproc != -1 && cpu >= apic_max_nproc) {
		/*
		 * Check whether cpuid is in valid range if CPU DR is enabled.
		 */
		return (B_FALSE);
	} else if (!CPU_IN_SET(apic_cpumask, cpu)) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

processorid_t
apic_get_next_bind_cpu(void)
{
	int i, count;
	processorid_t cpuid = 0;

	for (count = 0; count < apic_nproc; count++) {
		if (apic_next_bind_cpu >= apic_nproc) {
			apic_next_bind_cpu = 0;
		}
		i = apic_next_bind_cpu++;
		if (apic_cpu_in_range(i)) {
			cpuid = i;
			break;
		}
	}

	return (cpuid);
}

uint16_t
apic_get_apic_version()
{
	int i;
	uchar_t min_io_apic_ver = 0;
	static uint16_t version;		/* Cache as value is constant */
	static boolean_t found = B_FALSE;	/* Accomodate zero version */

	if (found == B_FALSE) {
		found = B_TRUE;

		/*
		 * Don't assume all IO APICs in the system are the same.
		 *
		 * Set to the minimum version.
		 */
		for (i = 0; i < apic_io_max; i++) {
			if ((apic_io_ver[i] != 0) &&
			    ((min_io_apic_ver == 0) ||
			    (min_io_apic_ver >= apic_io_ver[i])))
				min_io_apic_ver = apic_io_ver[i];
		}

		/* Assume all local APICs are of the same version. */
		version = (min_io_apic_ver << 8) | apic_cpus[0].aci_local_ver;
	}
	return (version);
}

/*
 * On machines with PCI-PCI bridges, a device behind a PCI-PCI bridge
 * needs special handling.  We may need to chase up the device tree,
 * using the PCI-PCI Bridge specification's "rotating IPIN assumptions",
 * to find the IPIN at the root bus that relates to the IPIN on the
 * subsidiary bus (for ACPI or MP).  We may, however, have an entry
 * in the MP table or the ACPI namespace for this device itself.
 * We handle both cases in the search below.
 */
/* this is the non-acpi version */
int
apic_handle_pci_pci_bridge(dev_info_t *idip, int child_devno, int child_ipin,
    struct apic_io_intr **intrp)
{
	return (-1);
}

uchar_t
irq_to_ioapic_index(int irq)
{
	int i;

	for (i = 0; i < apic_io_max; i++) {
		if (irq >= apic_io_vectbase[i] && irq <= apic_io_vectend[i])
			return ((uchar_t)i);
	}
	return (0xFF);	/* shouldn't happen */
}

/*
 * See if two irqs are compatible for sharing a vector.
 * Currently we only support sharing of PCI devices.
 */
static int
acpi_intr_compatible(iflag_t iflag1, iflag_t iflag2)
{
	uint_t	level1, po1;
	uint_t	level2, po2;

	/* Assume active high by default */
	po1 = 0;
	po2 = 0;

	if (iflag1.bustype != iflag2.bustype || iflag1.bustype != BUS_PCI)
		return (0);

	if (iflag1.intr_el == INTR_EL_CONFORM)
		level1 = AV_LEVEL;
	else
		level1 = (iflag1.intr_el == INTR_EL_LEVEL) ? AV_LEVEL : 0;

	if (level1 && ((iflag1.intr_po == INTR_PO_ACTIVE_LOW) ||
	    (iflag1.intr_po == INTR_PO_CONFORM)))
		po1 = AV_ACTIVE_LOW;

	if (iflag2.intr_el == INTR_EL_CONFORM)
		level2 = AV_LEVEL;
	else
		level2 = (iflag2.intr_el == INTR_EL_LEVEL) ? AV_LEVEL : 0;

	if (level2 && ((iflag2.intr_po == INTR_PO_ACTIVE_LOW) ||
	    (iflag2.intr_po == INTR_PO_CONFORM)))
		po2 = AV_ACTIVE_LOW;

	if ((level1 == level2) && (po1 == po2))
		return (1);

	return (0);
}

struct apic_io_intr *
apic_find_io_intr_w_busid(int irqno, int busid)
{
	struct	apic_io_intr	*intrp;

	/*
	 * It can have more than 1 entry with same source bus IRQ,
	 * but unique with the source bus id
	 */
	intrp = apic_io_intrp;
	if (intrp != NULL) {
		while (intrp->intr_entry == APIC_IO_INTR_ENTRY) {
			if (intrp->intr_irq == irqno &&
			    intrp->intr_busid == busid &&
			    intrp->intr_type == IO_INTR_INT)
				return (intrp);
			intrp++;
		}
	}
	APIC_VERBOSE_IOAPIC((CE_NOTE, "Did not find io intr for irqno:"
	    "busid %x:%x\n", irqno, busid));
	return ((struct apic_io_intr *)NULL);
}

static int
apic_find_bus(int busid)
{
	APIC_VERBOSE_IOAPIC((CE_WARN, "Did not find bus for bus id %x", busid));
	return (0);
}

int
apic_find_bus_id(int bustype)
{
	APIC_VERBOSE_IOAPIC((CE_WARN, "Did not find bus id for bustype %x",
	    bustype));
	return (-1);
}

/*
 * Check if a particular irq need to be reserved for any io_intr
 */
static struct apic_io_intr *
apic_find_io_intr(int irqno)
{
	struct	apic_io_intr	*intrp;

	intrp = apic_io_intrp;
	if (intrp != NULL) {
		while (intrp->intr_entry == APIC_IO_INTR_ENTRY) {
			if (intrp->intr_irq == irqno &&
			    intrp->intr_type == IO_INTR_INT)
				return (intrp);
			intrp++;
		}
	}
	return ((struct apic_io_intr *)NULL);
}

/*
 * Check if the given ioapicindex intin combination has already been assigned
 * an irq. If so return irqno. Else -1
 */
int
apic_find_intin(uchar_t ioapic, uchar_t intin)
{
	apic_irq_t *irqptr;
	int	i;

	/* find ioapic and intin in the apic_irq_table[] and return the index */
	for (i = apic_min_device_irq; i <= apic_max_device_irq; i++) {
		irqptr = apic_irq_table[i];
		while (irqptr) {
			if ((irqptr->airq_mps_intr_index >= 0) &&
			    (irqptr->airq_intin_no == intin) &&
			    (irqptr->airq_ioapicindex == ioapic)) {
				APIC_VERBOSE_IOAPIC((CE_NOTE, "!Found irq "
				    "entry for ioapic:intin %x:%x "
				    "shared interrupts ?", ioapic, intin));
				return (i);
			}
			irqptr = irqptr->airq_next;
		}
	}
	return (-1);
}

int
apic_allocate_irq(int irq)
{
	int	freeirq, i;

	if ((freeirq = apic_find_free_irq(irq, (APIC_RESV_IRQ - 1))) == -1) {
		if ((freeirq = apic_find_free_irq(APIC_FIRST_FREE_IRQ,
		    (irq - 1))) == -1) {
			/*
			 * if BIOS really defines every single irq in the mps
			 * table, then don't worry about conflicting with
			 * them, just use any free slot in apic_irq_table
			 */
			for (i = APIC_FIRST_FREE_IRQ; i < APIC_RESV_IRQ; i++) {
				if ((apic_irq_table[i] == NULL) ||
				    apic_irq_table[i]->airq_mps_intr_index ==
				    FREE_INDEX) {
					freeirq = i;
					break;
				}
			}

			if (freeirq == -1) {
				/* This shouldn't happen, but just in case */
				cmn_err(CE_WARN, "%s: NO available IRQ",
				    psm_name);
				return (-1);
			}
		}
	}

	if (apic_irq_table[freeirq] == NULL) {
		apic_irq_table[freeirq] =
		    kmem_zalloc(sizeof (apic_irq_t), KM_NOSLEEP);
		if (apic_irq_table[freeirq] == NULL) {
			cmn_err(CE_WARN, "%s: NO memory to allocate IRQ",
			    psm_name);
			return (-1);
		}
		apic_irq_table[freeirq]->airq_temp_cpu = IRQ_UNINIT;
		apic_irq_table[freeirq]->airq_mps_intr_index = FREE_INDEX;
	}
	return (freeirq);
}

static int
apic_find_free_irq(int start, int end)
{
	int	i;

	for (i = start; i <= end; i++)
		/* Check if any I/O entry needs this IRQ */
		if (apic_find_io_intr(i) == NULL) {
			/* Then see if it is free */
			if ((apic_irq_table[i] == NULL) ||
			    (apic_irq_table[i]->airq_mps_intr_index ==
			    FREE_INDEX)) {
				return (i);
			}
		}
	return (-1);
}

/*
 * compute the polarity, trigger mode and vector for programming into
 * the I/O apic and record in airq_rdt_entry.
 */
void
apic_record_rdt_entry(apic_irq_t *irqptr, int irq)
{
	int	ioapicindex, bus_type, vector;
	short	intr_index;
	uint_t	level, po, io_po;
	struct apic_io_intr *iointrp;

	intr_index = irqptr->airq_mps_intr_index;
	DDI_INTR_IMPLDBG((CE_CONT, "apic_record_rdt_entry: intr_index=%d "
	    "irq = 0x%x dip = 0x%p vector = 0x%x\n", intr_index, irq,
	    (void *)irqptr->airq_dip, irqptr->airq_vector));

	if (intr_index == RESERVE_INDEX) {
		apic_error |= APIC_ERR_INVALID_INDEX;
		return;
	} else if (APIC_IS_MSI_OR_MSIX_INDEX(intr_index)) {
		return;
	}

	vector = irqptr->airq_vector;
	ioapicindex = irqptr->airq_ioapicindex;
	/* Assume edge triggered by default */
	level = 0;
	/* Assume active high by default */
	po = 0;

	if (intr_index == DEFAULT_INDEX || intr_index == FREE_INDEX) {
		ASSERT(irq < 16);
		if (eisa_level_intr_mask & (1 << irq))
			level = AV_LEVEL;
		if (intr_index == FREE_INDEX && apic_defconf == 0)
			apic_error |= APIC_ERR_INVALID_INDEX;
	} else if (intr_index == ACPI_INDEX) {
		bus_type = irqptr->airq_iflag.bustype;
		if (irqptr->airq_iflag.intr_el == INTR_EL_CONFORM) {
			if (bus_type == BUS_PCI)
				level = AV_LEVEL;
		} else
			level = (irqptr->airq_iflag.intr_el == INTR_EL_LEVEL) ?
			    AV_LEVEL : 0;
		if (level &&
		    ((irqptr->airq_iflag.intr_po == INTR_PO_ACTIVE_LOW) ||
		    (irqptr->airq_iflag.intr_po == INTR_PO_CONFORM &&
		    bus_type == BUS_PCI)))
			po = AV_ACTIVE_LOW;
	} else {
		iointrp = apic_io_intrp + intr_index;
		bus_type = apic_find_bus(iointrp->intr_busid);
		if (iointrp->intr_el == INTR_EL_CONFORM) {
			if ((irq < 16) && (eisa_level_intr_mask & (1 << irq)))
				level = AV_LEVEL;
			else if (bus_type == BUS_PCI)
				level = AV_LEVEL;
		} else
			level = (iointrp->intr_el == INTR_EL_LEVEL) ?
			    AV_LEVEL : 0;
		if (level && ((iointrp->intr_po == INTR_PO_ACTIVE_LOW) ||
		    (iointrp->intr_po == INTR_PO_CONFORM &&
		    bus_type == BUS_PCI)))
			po = AV_ACTIVE_LOW;
	}
	if (level)
		apic_level_intr[irq] = 1;
	/*
	 * The 82489DX External APIC cannot do active low polarity interrupts.
	 */
	if (po && (apic_io_ver[ioapicindex] != IOAPIC_VER_82489DX))
		io_po = po;
	else
		io_po = 0;

	if (apic_verbose & APIC_VERBOSE_IOAPIC_FLAG)
		prom_printf("setio: ioapic=0x%x intin=0x%x level=0x%x po=0x%x "
		    "vector=0x%x cpu=0x%x\n\n", ioapicindex,
		    irqptr->airq_intin_no, level, io_po, vector,
		    irqptr->airq_cpu);

	irqptr->airq_rdt_entry = level|io_po|vector;
}

int
apic_acpi_translate_pci_irq(dev_info_t *dip, int busid, int devid,
    int ipin, int *pci_irqp, iflag_t *intr_flagp)
{

	int status;
	acpi_psm_lnk_t acpipsmlnk;

	if ((status = acpi_get_irq_cache_ent(busid, devid, ipin, pci_irqp,
	    intr_flagp)) == ACPI_PSM_SUCCESS) {
		APIC_VERBOSE_IRQ((CE_CONT, "!%s: Found irqno %d "
		    "from cache for device %s, instance #%d\n", psm_name,
		    *pci_irqp, ddi_get_name(dip), ddi_get_instance(dip)));
		return (status);
	}

	bzero(&acpipsmlnk, sizeof (acpi_psm_lnk_t));

	if ((status = acpi_translate_pci_irq(dip, ipin, pci_irqp, intr_flagp,
	    &acpipsmlnk)) == ACPI_PSM_FAILURE) {
		APIC_VERBOSE_IRQ((CE_WARN, "%s: "
		    " acpi_translate_pci_irq failed for device %s, instance"
		    " #%d", psm_name, ddi_get_name(dip),
		    ddi_get_instance(dip)));
		return (status);
	}

	if (status == ACPI_PSM_PARTIAL && acpipsmlnk.lnkobj != NULL) {
		status = apic_acpi_irq_configure(&acpipsmlnk, dip, pci_irqp,
		    intr_flagp);
		if (status != ACPI_PSM_SUCCESS) {
			status = acpi_get_current_irq_resource(&acpipsmlnk,
			    pci_irqp, intr_flagp);
		}
	}

	if (status == ACPI_PSM_SUCCESS) {
		acpi_new_irq_cache_ent(busid, devid, ipin, *pci_irqp,
		    intr_flagp, &acpipsmlnk);

		APIC_VERBOSE_IRQ((CE_CONT, "%s: [ACPI] "
		    "new irq %d for device %s, instance #%d\n", psm_name,
		    *pci_irqp, ddi_get_name(dip), ddi_get_instance(dip)));
	}

	return (status);
}

/*
 * Adds an entry to the irq list passed in, and returns the new list.
 * Entries are added in priority order (lower numerical priorities are
 * placed closer to the head of the list)
 */
static prs_irq_list_t *
acpi_insert_prs_irq_ent(prs_irq_list_t *listp, int priority, int irq,
    iflag_t *iflagp, acpi_prs_private_t *prsprvp)
{
	struct prs_irq_list_ent *newent, *prevp = NULL, *origlistp;

	newent = kmem_zalloc(sizeof (struct prs_irq_list_ent), KM_SLEEP);

	newent->list_prio = priority;
	newent->irq = irq;
	newent->intrflags = *iflagp;
	newent->prsprv = *prsprvp;
	/* ->next is NULL from kmem_zalloc */

	/*
	 * New list -- return the new entry as the list.
	 */
	if (listp == NULL)
		return (newent);

	/*
	 * Save original list pointer for return (since we're not modifying
	 * the head)
	 */
	origlistp = listp;

	/*
	 * Insertion sort, with entries with identical keys stored AFTER
	 * existing entries (the less-than-or-equal test of priority does
	 * this for us).
	 */
	while (listp != NULL && listp->list_prio <= priority) {
		prevp = listp;
		listp = listp->next;
	}

	newent->next = listp;

	if (prevp == NULL) { /* Add at head of list (newent is the new head) */
		return (newent);
	} else {
		prevp->next = newent;
		return (origlistp);
	}
}

/*
 * Frees the list passed in, deallocating all memory and leaving *listpp
 * set to NULL.
 */
static void
acpi_destroy_prs_irq_list(prs_irq_list_t **listpp)
{
	struct prs_irq_list_ent *nextp;

	ASSERT(listpp != NULL);

	while (*listpp != NULL) {
		nextp = (*listpp)->next;
		kmem_free(*listpp, sizeof (struct prs_irq_list_ent));
		*listpp = nextp;
	}
}

/*
 * apic_choose_irqs_from_prs returns a list of irqs selected from the list of
 * irqs returned by the link device's _PRS method.  The irqs are chosen
 * to minimize contention in situations where the interrupt link device
 * can be programmed to steer interrupts to different interrupt controller
 * inputs (some of which may already be in use).  The list is sorted in order
 * of irqs to use, with the highest priority given to interrupt controller
 * inputs that are not shared.   When an interrupt controller input
 * must be shared, apic_choose_irqs_from_prs adds the possible irqs to the
 * returned list in the order that minimizes sharing (thereby ensuring lowest
 * possible latency from interrupt trigger time to ISR execution time).
 */
static prs_irq_list_t *
apic_choose_irqs_from_prs(acpi_irqlist_t *irqlistent, dev_info_t *dip,
    int crs_irq)
{
	int32_t irq;
	int i;
	prs_irq_list_t *prsirqlistp = NULL;
	iflag_t iflags;

	while (irqlistent != NULL) {
		irqlistent->intr_flags.bustype = BUS_PCI;

		for (i = 0; i < irqlistent->num_irqs; i++) {

			irq = irqlistent->irqs[i];

			if (irq <= 0) {
				/* invalid irq number */
				continue;
			}

			if ((irq < 16) && (apic_reserved_irqlist[irq]))
				continue;

			if ((apic_irq_table[irq] == NULL) ||
			    (apic_irq_table[irq]->airq_dip == dip)) {

				prsirqlistp = acpi_insert_prs_irq_ent(
				    prsirqlistp, 0 /* Highest priority */, irq,
				    &irqlistent->intr_flags,
				    &irqlistent->acpi_prs_prv);

				/*
				 * If we do not prefer the current irq from _CRS
				 * or if we do and this irq is the same as the
				 * current irq from _CRS, this is the one
				 * to pick.
				 */
				if (!(apic_prefer_crs) || (irq == crs_irq)) {
					return (prsirqlistp);
				}
				continue;
			}

			/*
			 * Edge-triggered interrupts cannot be shared
			 */
			if (irqlistent->intr_flags.intr_el == INTR_EL_EDGE)
				continue;

			/*
			 * To work around BIOSes that contain incorrect
			 * interrupt polarity information in interrupt
			 * descriptors returned by _PRS, we assume that
			 * the polarity of the other device sharing this
			 * interrupt controller input is compatible.
			 * If it's not, the caller will catch it when
			 * the caller invokes the link device's _CRS method
			 * (after invoking its _SRS method).
			 */
			iflags = irqlistent->intr_flags;
			iflags.intr_po =
			    apic_irq_table[irq]->airq_iflag.intr_po;

			if (!acpi_intr_compatible(iflags,
			    apic_irq_table[irq]->airq_iflag)) {
				APIC_VERBOSE_IRQ((CE_CONT, "!%s: irq %d "
				    "not compatible [%x:%x:%x !~ %x:%x:%x]",
				    psm_name, irq,
				    iflags.intr_po,
				    iflags.intr_el,
				    iflags.bustype,
				    apic_irq_table[irq]->airq_iflag.intr_po,
				    apic_irq_table[irq]->airq_iflag.intr_el,
				    apic_irq_table[irq]->airq_iflag.bustype));
				continue;
			}

			/*
			 * If we prefer the irq from _CRS, no need
			 * to search any further (and make sure
			 * to add this irq with the highest priority
			 * so it's tried first).
			 */
			if (crs_irq == irq && apic_prefer_crs) {

				return (acpi_insert_prs_irq_ent(
				    prsirqlistp,
				    0 /* Highest priority */,
				    irq, &iflags,
				    &irqlistent->acpi_prs_prv));
			}

			/*
			 * Priority is equal to the share count (lower
			 * share count is higher priority). Note that
			 * the intr flags passed in here are the ones we
			 * changed above -- if incorrect, it will be
			 * caught by the caller's _CRS flags comparison.
			 */
			prsirqlistp = acpi_insert_prs_irq_ent(
			    prsirqlistp,
			    apic_irq_table[irq]->airq_share, irq,
			    &iflags, &irqlistent->acpi_prs_prv);
		}

		/* Go to the next irqlist entry */
		irqlistent = irqlistent->next;
	}

	return (prsirqlistp);
}

/*
 * Configures the irq for the interrupt link device identified by
 * acpipsmlnkp.
 *
 * Gets the current and the list of possible irq settings for the
 * device. If apic_unconditional_srs is not set, and the current
 * resource setting is in the list of possible irq settings,
 * current irq resource setting is passed to the caller.
 *
 * Otherwise, picks an irq number from the list of possible irq
 * settings, and sets the irq of the device to this value.
 * If prefer_crs is set, among a set of irq numbers in the list that have
 * the least number of devices sharing the interrupt, we pick current irq
 * resource setting if it is a member of this set.
 *
 * Passes the irq number in the value pointed to by pci_irqp, and
 * polarity and sensitivity in the structure pointed to by dipintrflagp
 * to the caller.
 *
 * Note that if setting the irq resource failed, but successfuly obtained
 * the current irq resource settings, passes the current irq resources
 * and considers it a success.
 *
 * Returns:
 * ACPI_PSM_SUCCESS on success.
 *
 * ACPI_PSM_FAILURE if an error occured during the configuration or
 * if a suitable irq was not found for this device, or if setting the
 * irq resource and obtaining the current resource fails.
 *
 */
static int
apic_acpi_irq_configure(acpi_psm_lnk_t *acpipsmlnkp, dev_info_t *dip,
    int *pci_irqp, iflag_t *dipintr_flagp)
{
	int32_t irq;
	int cur_irq = -1;
	acpi_irqlist_t *irqlistp;
	prs_irq_list_t *prs_irq_listp, *prs_irq_entp;
	boolean_t found_irq = B_FALSE;

	dipintr_flagp->bustype = BUS_PCI;

	if ((acpi_get_possible_irq_resources(acpipsmlnkp, &irqlistp))
	    == ACPI_PSM_FAILURE) {
		APIC_VERBOSE_IRQ((CE_WARN, "!%s: Unable to determine "
		    "or assign IRQ for device %s, instance #%d: The system was "
		    "unable to get the list of potential IRQs from ACPI.",
		    psm_name, ddi_get_name(dip), ddi_get_instance(dip)));

		return (ACPI_PSM_FAILURE);
	}

	if ((acpi_get_current_irq_resource(acpipsmlnkp, &cur_irq,
	    dipintr_flagp) == ACPI_PSM_SUCCESS) && (!apic_unconditional_srs) &&
	    (cur_irq > 0)) {
		/*
		 * If an IRQ is set in CRS and that IRQ exists in the set
		 * returned from _PRS, return that IRQ, otherwise print
		 * a warning
		 */

		if (acpi_irqlist_find_irq(irqlistp, cur_irq, NULL)
		    == ACPI_PSM_SUCCESS) {

			ASSERT(pci_irqp != NULL);
			*pci_irqp = cur_irq;
			acpi_free_irqlist(irqlistp);
			return (ACPI_PSM_SUCCESS);
		}

		APIC_VERBOSE_IRQ((CE_WARN, "!%s: Could not find the "
		    "current irq %d for device %s, instance #%d in ACPI's "
		    "list of possible irqs for this device. Picking one from "
		    " the latter list.", psm_name, cur_irq, ddi_get_name(dip),
		    ddi_get_instance(dip)));
	}

	if ((prs_irq_listp = apic_choose_irqs_from_prs(irqlistp, dip,
	    cur_irq)) == NULL) {

		APIC_VERBOSE_IRQ((CE_WARN, "!%s: Could not find a "
		    "suitable irq from the list of possible irqs for device "
		    "%s, instance #%d in ACPI's list of possible irqs",
		    psm_name, ddi_get_name(dip), ddi_get_instance(dip)));

		acpi_free_irqlist(irqlistp);
		return (ACPI_PSM_FAILURE);
	}

	acpi_free_irqlist(irqlistp);

	for (prs_irq_entp = prs_irq_listp;
	    prs_irq_entp != NULL && found_irq == B_FALSE;
	    prs_irq_entp = prs_irq_entp->next) {

		acpipsmlnkp->acpi_prs_prv = prs_irq_entp->prsprv;
		irq = prs_irq_entp->irq;

		APIC_VERBOSE_IRQ((CE_CONT, "!%s: Setting irq %d for "
		    "device %s instance #%d\n", psm_name, irq,
		    ddi_get_name(dip), ddi_get_instance(dip)));

		if ((acpi_set_irq_resource(acpipsmlnkp, irq))
		    == ACPI_PSM_SUCCESS) {
			/*
			 * setting irq was successful, check to make sure CRS
			 * reflects that. If CRS does not agree with what we
			 * set, return the irq that was set.
			 */

			if (acpi_get_current_irq_resource(acpipsmlnkp, &cur_irq,
			    dipintr_flagp) == ACPI_PSM_SUCCESS) {

				if (cur_irq != irq)
					APIC_VERBOSE_IRQ((CE_WARN,
					    "!%s: IRQ resource set "
					    "(irqno %d) for device %s "
					    "instance #%d, differs from "
					    "current setting irqno %d",
					    psm_name, irq, ddi_get_name(dip),
					    ddi_get_instance(dip), cur_irq));
			} else {
				/*
				 * On at least one system, there was a bug in
				 * a DSDT method called by _STA, causing _STA to
				 * indicate that the link device was disabled
				 * (when, in fact, it was enabled).  Since _SRS
				 * succeeded, assume that _CRS is lying and use
				 * the iflags from this _PRS interrupt choice.
				 * If we're wrong about the flags, the polarity
				 * will be incorrect and we may get an interrupt
				 * storm, but there's not much else we can do
				 * at this point.
				 */
				*dipintr_flagp = prs_irq_entp->intrflags;
			}

			/*
			 * Return the irq that was set, and not what _CRS
			 * reports, since _CRS has been seen to return
			 * different IRQs than what was passed to _SRS on some
			 * systems (and just not return successfully on others).
			 */
			cur_irq = irq;
			found_irq = B_TRUE;
		} else {
			APIC_VERBOSE_IRQ((CE_WARN, "!%s: set resource "
			    "irq %d failed for device %s instance #%d",
			    psm_name, irq, ddi_get_name(dip),
			    ddi_get_instance(dip)));

			if (cur_irq == -1) {
				acpi_destroy_prs_irq_list(&prs_irq_listp);
				return (ACPI_PSM_FAILURE);
			}
		}
	}

	acpi_destroy_prs_irq_list(&prs_irq_listp);

	if (!found_irq)
		return (ACPI_PSM_FAILURE);

	ASSERT(pci_irqp != NULL);
	*pci_irqp = cur_irq;
	return (ACPI_PSM_SUCCESS);
}

void
ioapic_disable_redirection()
{
	int ioapic_ix;
	int intin_max;
	int intin_ix;

	/* Disable the I/O APIC redirection entries */
	for (ioapic_ix = 0; ioapic_ix < apic_io_max; ioapic_ix++) {

		/* Bits 23-16 define the maximum redirection entries */
		intin_max = (ioapic_read(ioapic_ix, APIC_VERS_CMD) >> 16)
		    & 0xff;

		for (intin_ix = 0; intin_ix <= intin_max; intin_ix++) {
			/*
			 * The assumption here is that this is safe, even for
			 * systems with IOAPICs that suffer from the hardware
			 * erratum because all devices have been quiesced before
			 * this function is called from apic_shutdown()
			 * (or equivalent). If that assumption turns out to be
			 * false, this mask operation can induce the same
			 * erratum result we're trying to avoid.
			 */
			ioapic_write(ioapic_ix, APIC_RDT_CMD + 2 * intin_ix,
			    AV_MASK);
		}
	}
}

struct apic_state {
	int32_t as_task_reg;
	int32_t as_dest_reg;
	int32_t as_format_reg;
	int32_t as_local_timer;
	int32_t as_pcint_vect;
	int32_t as_int_vect0;
	int32_t as_int_vect1;
	int32_t as_err_vect;
	int32_t as_init_count;
	int32_t as_divide_reg;
	int32_t as_spur_int_reg;
	uint32_t as_ioapic_ids[MAX_IO_APIC];
};

static void
apic_save_state(struct apic_state *sp)
{
	int	i, cpuid;
	ulong_t	iflag;

	PMD(PMD_SX, ("apic_save_state %p\n", (void *)sp))
	/*
	 * First the local APIC.
	 */
	sp->as_task_reg = apic_reg_ops->apic_get_pri();
	sp->as_dest_reg =  apic_reg_ops->apic_read(APIC_DEST_REG);
	if (apic_mode == LOCAL_APIC)
		sp->as_format_reg = apic_reg_ops->apic_read(APIC_FORMAT_REG);
	sp->as_local_timer = apic_reg_ops->apic_read(APIC_LOCAL_TIMER);
	sp->as_pcint_vect = apic_reg_ops->apic_read(APIC_PCINT_VECT);
	sp->as_int_vect0 = apic_reg_ops->apic_read(APIC_INT_VECT0);
	sp->as_int_vect1 = apic_reg_ops->apic_read(APIC_INT_VECT1);
	sp->as_err_vect = apic_reg_ops->apic_read(APIC_ERR_VECT);
	sp->as_init_count = apic_reg_ops->apic_read(APIC_INIT_COUNT);
	sp->as_divide_reg = apic_reg_ops->apic_read(APIC_DIVIDE_REG);
	sp->as_spur_int_reg = apic_reg_ops->apic_read(APIC_SPUR_INT_REG);

	/*
	 * If on the boot processor then save the IOAPICs' IDs
	 */
	if ((cpuid = psm_get_cpu_id()) == 0) {

		iflag = intr_clear();
		lock_set(&apic_ioapic_lock);

		for (i = 0; i < apic_io_max; i++)
			sp->as_ioapic_ids[i] = ioapic_read(i, APIC_ID_CMD);

		lock_clear(&apic_ioapic_lock);
		intr_restore(iflag);
	}

	/* apic_state() is currently invoked only in Suspend/Resume */
	apic_cpus[cpuid].aci_status |= APIC_CPU_SUSPEND;
}

static void
apic_restore_state(struct apic_state *sp)
{
	int	i;
	ulong_t	iflag;

	/*
	 * First the local APIC.
	 */
	apic_reg_ops->apic_write_task_reg(sp->as_task_reg);
	if (apic_mode == LOCAL_APIC) {
		apic_reg_ops->apic_write(APIC_DEST_REG, sp->as_dest_reg);
		apic_reg_ops->apic_write(APIC_FORMAT_REG, sp->as_format_reg);
	}
	apic_reg_ops->apic_write(APIC_LOCAL_TIMER, sp->as_local_timer);
	apic_reg_ops->apic_write(APIC_PCINT_VECT, sp->as_pcint_vect);
	apic_reg_ops->apic_write(APIC_INT_VECT0, sp->as_int_vect0);
	apic_reg_ops->apic_write(APIC_INT_VECT1, sp->as_int_vect1);
	apic_reg_ops->apic_write(APIC_ERR_VECT, sp->as_err_vect);
	apic_reg_ops->apic_write(APIC_INIT_COUNT, sp->as_init_count);
	apic_reg_ops->apic_write(APIC_DIVIDE_REG, sp->as_divide_reg);
	apic_reg_ops->apic_write(APIC_SPUR_INT_REG, sp->as_spur_int_reg);

	/*
	 * the following only needs to be done once, so we do it on the
	 * boot processor, since we know that we only have one of those
	 */
	if (psm_get_cpu_id() == 0) {

		iflag = intr_clear();
		lock_set(&apic_ioapic_lock);

		/* Restore IOAPICs' APIC IDs */
		for (i = 0; i < apic_io_max; i++) {
			ioapic_write(i, APIC_ID_CMD, sp->as_ioapic_ids[i]);
		}

		lock_clear(&apic_ioapic_lock);
		intr_restore(iflag);

		/*
		 * restore acpi link device mappings
		 */
		acpi_restore_link_devices();
	}
}

/*
 * Returns 0 on success
 */
int
apic_state(psm_state_request_t *rp)
{
	PMD(PMD_SX, ("apic_state "))
	switch (rp->psr_cmd) {
	case PSM_STATE_ALLOC:
		rp->req.psm_state_req.psr_state =
		    kmem_zalloc(sizeof (struct apic_state), KM_NOSLEEP);
		if (rp->req.psm_state_req.psr_state == NULL)
			return (ENOMEM);
		rp->req.psm_state_req.psr_state_size =
		    sizeof (struct apic_state);
		PMD(PMD_SX, (":STATE_ALLOC: state %p, size %lx\n",
		    rp->req.psm_state_req.psr_state,
		    rp->req.psm_state_req.psr_state_size))
		return (0);

	case PSM_STATE_FREE:
		kmem_free(rp->req.psm_state_req.psr_state,
		    rp->req.psm_state_req.psr_state_size);
		PMD(PMD_SX, (" STATE_FREE: state %p, size %lx\n",
		    rp->req.psm_state_req.psr_state,
		    rp->req.psm_state_req.psr_state_size))
		return (0);

	case PSM_STATE_SAVE:
		PMD(PMD_SX, (" STATE_SAVE: state %p, size %lx\n",
		    rp->req.psm_state_req.psr_state,
		    rp->req.psm_state_req.psr_state_size))
		apic_save_state(rp->req.psm_state_req.psr_state);
		return (0);

	case PSM_STATE_RESTORE:
		apic_restore_state(rp->req.psm_state_req.psr_state);
		PMD(PMD_SX, (" STATE_RESTORE: state %p, size %lx\n",
		    rp->req.psm_state_req.psr_state,
		    rp->req.psm_state_req.psr_state_size))
		return (0);

	default:
		return (EINVAL);
	}
}
