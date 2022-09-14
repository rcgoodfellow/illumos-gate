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
#include <sys/apix.h>
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
#include <sys/io/huashan/pmio.h>
#include <sys/io/milan/ccx.h>
#include <sys/io/milan/fabric.h>
#include <milan/milan_physaddrs.h>

/*
 *	Local Function Prototypes
 */
static int apic_find_free_irq(int start, int end);

void apic_record_rdt_entry(apic_irq_t *irqptr, int irq);

/* SCI interrupt configuration; -1 if SCI not used */
int apic_sci_vect = -1;
iflag_t apic_sci_flags;

/* HPET interrupt configuration; -1 if HPET not used */
int apic_hpet_vect = -1;
iflag_t apic_hpet_flags;

/*
 * psm name pointer
 */
char *psm_name;

static int apic_probe_raw(const char *);

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
int	apic_irq_translate = 0;
int	apic_spec_rev = 0;

uchar_t apic_io_id[MAX_IO_APIC];
volatile uint32_t *apicioadr[MAX_IO_APIC];
uchar_t	apic_io_ver[MAX_IO_APIC];
uchar_t	apic_io_vectbase[MAX_IO_APIC];
uchar_t	apic_io_vectend[MAX_IO_APIC];
uint32_t apic_physaddr[MAX_IO_APIC];

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

uchar_t	apic_resv_vector[MAXIPL+1];

char	apic_level_intr[APIC_MAX_VECTOR+1];

/*
 * airq_mutex protects additions to the apic_irq_table - the first
 * pointer and any airq_nexts off of that one.  It also guarantees
 * that share_id is unique as new ids are generated only when new
 * irq_t structs are linked in. Once linked in the structs are never
 * deleted.  Note that there is a slight gap between allocating in
 * apic_introp_xlate and programming in addspl.
 */
kmutex_t	airq_mutex;
apic_irq_t	*apic_irq_table[APIC_MAX_VECTOR+1];

/*
 * Auto-configuration routines
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
apic_count_thread(milan_thread_t *mtp, void *arg)
{
	int *nthreadp = arg;

	++*nthreadp;

	return (0);
}

static int
apic_enumerate_one(milan_thread_t *mtp, void *arg)
{
	uint32_t *idxp = arg;
	apic_cpus_info_t *acip = &apic_cpus[*idxp];

	acip->aci_local_id = milan_thread_apicid(mtp);
	acip->aci_processor_id = acip->aci_local_id;
	acip->aci_local_ver = 0;
	acip->aci_status = 0;
	CPUSET_ADD(apic_cpumask, *idxp);
	acip->aci_local_ver =
	    (uchar_t)(apic_reg_ops->apic_read(APIC_VERS_REG) & 0xff);

	VERIFY3S(*idxp, <, apic_nproc);
	++*idxp;

	return (0);
}

static int
apic_probe_raw(const char *modname)
{
	int i;
	uint32_t irqno;
	caddr_t pmbase;
	const size_t pmsize = FCH_R_BLOCK_GETSIZE(PM);
	uint32_t apic_index = 0;
	FCH_REG_TYPE(PM, DECODEEN) decodeen = 0;

	(void) milan_walk_thread(apic_count_thread, &apic_nproc);
	apic_cpus_size = max(apic_nproc, max_ncpus) * sizeof (*apic_cpus);
	if ((apic_cpus = kmem_zalloc(apic_cpus_size, KM_NOSLEEP)) == NULL) {
		apic_max_nproc = -1;
		apic_nproc = 0;
		return (PSM_FAILURE);
	}

	apic_enable_x2apic();

	CPUSET_ZERO(apic_cpumask);
	(void) milan_walk_thread(apic_enumerate_one, &apic_index);

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
apic_get_apic_version(void)
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
				if (IS_IRQ_FREE(apic_irq_table[i])) {
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
		apic_irq_table[freeirq]->airq_kind = AIRQK_FREE;
	}
	return (freeirq);
}

static int
apic_find_free_irq(int start, int end)
{
	int	i;

	for (i = start; i <= end; i++) {
		/* Then see if it is free */
		if (IS_IRQ_FREE(apic_irq_table[i]))
			return (i);
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
	uint_t	level, po;

	DDI_INTR_IMPLDBG((CE_CONT, "apic_record_rdt_entry: kind = %d "
	    "irq = 0x%x dip = 0x%p vector = 0x%x\n", irqptr->airq_kind, irq,
	    (void *)irqptr->airq_dip, irqptr->airq_vector));

	vector = irqptr->airq_vector;
	ioapicindex = irqptr->airq_ioapicindex;
	/* Assume edge triggered by default */
	level = 0;
	/* Assume active high by default */
	po = 0;

	switch (irqptr->airq_kind) {
	case AIRQK_RESERVED:
	case AIRQK_FREE:
		/* XXX should we assert !FREE? */
		apic_error |= APIC_ERR_INVALID_INDEX;
		/*FALLTHROUGH*/
	case AIRQK_MSI:
	case AIRQK_MSIX:
		return;
	case AIRQK_FIXED:
		/*
		 * XXX This code is wrong and needs to be removed.  To
		 * understand why, a history lesson is required.
		 *
		 * In the early days, before MSIs and before SoCs and processor
		 * families with but a single supported PCH or FCH, every
		 * board might have had many different fixed interrupt sources
		 * and each would have had its own unique routing of those
		 * sources as physical wires into an IOAPIC (or even before
		 * that, a PIC).  To understand these sources and their routings
		 * each OS would have needed some kind of lookup table.  That
		 * might have been fine, except that the only people who knew
		 * what those tables should have contained were the board
		 * manufacturers; they could have added to such tables in open
		 * source OSs, but support of Microsoft Windows and other
		 * proprietary OSs necessitated putting this somewhere else,
		 * somewhere that could be controlled by the board vendor's
		 * code.  Out of this pair of needs eventually arose the MPS
		 * tables and later ACPI.
		 *
		 * Part of the contents of those tables has (almost) always been
		 * the polarity of each fixed interrupt and whether assertion of
		 * it is level- or edge-triggered.  There was, realistically, no
		 * reliable way to know this other than having designed the
		 * board and read the datasheets of the components on it.  So
		 * this information, too, was encoded in the vendor-supplied
		 * tables.
		 *
		 * Today, there is basically no reason for any PCI/-X/e device
		 * to need or use fixed interrupts; MSI has been mandatory
		 * since PCI 2.2.  So the only fixed sources we have are those
		 * from devices inside the SoC itself, which means that their
		 * attributes are no longer board-specific but rather generic
		 * across every board (regardless of machine architecture!) with
		 * the same SoC on it.  These sources are mostly from FCH
		 * peripherals, though some can originate from parts of the NBIO
		 * logic.  The one exception is INTx-emulation, which NBIO
		 * translates into virtual wire interrupts to the FCH IOAPIC
		 * as specified by the mapping table accessed via legacy I/O
		 * ports 0xC00 and 0xC01.  The oxide architecture does not
		 * support INTx emulation and all such sources are mapped to
		 * the IOAPIC's catch-all (spurious) virtual input pin.
		 *
		 * As such, the polarity and trigger type are known and fixed
		 * for each interrupt source; in the fullness of time, when we
		 * support multiple SoCs (and/or if we ever choose/need to
		 * support an external FCH), we may need a lookup table here
		 * for each processor family or external FCH.  Critically,
		 * there are only a few ways to get here (all via
		 * apix_intx_set_vector()):
		 *
		 * - ioapix_init_intr() via apix_alloc_intx(), only for SCI and
		 *   HPET interrupts which we currently do not set up.
		 * - the apix_rebind() path, which deals with interrupts that
		 *   have already been set up and must already have a known
		 *   polarity and trigger mode.
		 * - the other apix_alloc_intx() path, which is the interesting
		 *   one because it's how drivers request interrupts; this
		 *   path always starts with apix_intx_xlate_irq(), which
		 *   enforces the constraints described above and always sets
		 *   the polarity and trigger mode to fixed values before we
		 *   get here.
		 *
		 * We'd like to detect incorrect polarity and trigger mode, but
		 * this is not the place to do it because there's no way to
		 * know what's correct; only calling code can do that.  That is,
		 * the SoC-specific lookup table, if one is needed, must be used
		 * before we get here.  All we can do here, and what we should
		 * do here, is ensure that these attributes have been
		 * initialised... which is impossible given the possible range
		 * of values we've temporarily inherited from i86pc (and
		 * ultimately MPS): there is no sentinel value.
		 *
		 * It should now be clear that we should never be setting the
		 * level or trigger mode here, and that we should adopt a
		 * simpler way for callers to specify them here, one that does
		 * not require any interpretation other than guaranteeing that
		 * they have been initialised.  That is the code that belongs
		 * here in place of this.  Fix this when apix_intx_xlate_irq()
		 * is fixed, with the introduction of the huashan nexus driver
		 * for FCH legacy peripherals.  That driver is where this
		 * knowledge ought best to live, at least for now.
		 */
		bus_type = irqptr->airq_iflag.bustype;
		if (irqptr->airq_iflag.intr_el == INTR_EL_CONFORM) {
			if (bus_type == BUS_PCI)
				level = AV_LEVEL;
		} else
			level = (irqptr->airq_iflag.intr_el == INTR_EL_LEVEL) ?
			    AV_LEVEL : 0;
		if (level != 0 &&
		    ((irqptr->airq_iflag.intr_po == INTR_PO_ACTIVE_LOW) ||
		    (irqptr->airq_iflag.intr_po == INTR_PO_CONFORM &&
		    bus_type == BUS_PCI)))
			po = AV_ACTIVE_LOW;
		break;
	default:
		cmn_err(CE_PANIC, "invalid airq_kind 0x%x", irqptr->airq_kind);
	}
	if (level)
		apic_level_intr[irq] = 1;

	/* Never on this architecture. */
	VERIFY(apic_io_ver[ioapicindex] != IOAPIC_VER_82489DX);

	if (apic_verbose & APIC_VERBOSE_IOAPIC_FLAG)
		prom_printf("setio: ioapic=0x%x intin=0x%x level=0x%x po=0x%x "
		    "vector=0x%x cpu=0x%x\n\n", ioapicindex,
		    irqptr->airq_intin_no, level, po, vector, irqptr->airq_cpu);

	irqptr->airq_rdt_entry = level | po | vector;
}

void
ioapic_disable_redirection(void)
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
