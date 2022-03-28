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
 */
/*
 * Copyright (c) 2010, Intel Corporation.
 * All rights reserved.
 */
/*
 * Copyright 2019 Joyent, Inc.
 * Copyright 2022 Oxide Computer Co.
 */

/*
 * Welcome to the world of the "real mode platter", a trip back to the 1970s
 * that AMD refuse to let us escape.  The RMP code lives in ml/mpcore.s.  See
 * mp_startup.c for MP boot theory.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/cpuvar.h>
#include <sys/cpu_module.h>
#include <sys/kmem.h>
#include <sys/archsystm.h>
#include <sys/machsystm.h>
#include <sys/controlregs.h>
#include <sys/x86_archext.h>
#include <sys/smp_impldefs.h>
#include <sys/sysmacros.h>
#include <sys/mach_mmu.h>
#include <sys/promif.h>
#include <sys/cpu.h>
#include <sys/cpu_event.h>
#include <sys/sunndi.h>
#include <sys/fs/dv_node.h>
#include <sys/rm_platter.h>
#include <vm/hat_i86.h>
#include <vm/as.h>

extern void real_mode_start_cpu(void);
extern void real_mode_start_cpu_end(void);

int
mach_cpucontext_init(void)
{
	struct rm_platter *rmpp = (struct rm_platter *)rm_platter_va;
	uint8_t off;

	/*
	 * Create an identity mapping for the RMP so that the bootstrap code
	 * will be able to access itself during the transition to long mode.
	 */
	hat_devload(kas.a_hat,
	    (caddr_t)(uintptr_t)rm_platter_pa, MMU_PAGESIZE,
	    btop(rm_platter_pa), PROT_READ | PROT_WRITE | PROT_EXEC,
	    HAT_LOAD_NOCONSIST);

	/*
	 * Copy in the code from mpcore.s to the beginning of the RMP.
	 */
	VERIFY3U((size_t)real_mode_start_cpu_end -
	    (size_t)real_mode_start_cpu, <=, RM_PLATTER_CODE_SIZE);
	bcopy((caddr_t)real_mode_start_cpu, (caddr_t)rmpp->rm_code,
	    (size_t)real_mode_start_cpu_end -
	    (size_t)real_mode_start_cpu);

	/*
	 * Poke in the jump instruction at the reset vector to get us to the
	 * start of the code.  This is the first instruction the CPU will
	 * execute at startup, so it will be executed in 16-bit real mode.
	 *
	 * The %ip following this 3-byte instruction is %cs:0xfff3 and we wish
	 * to jump to %cs:f000, so the 16-bit displacement is 0xf00d.  Really.
	 * For what this jumps to, see ml/mpcore.s.
	 *
	 * We fill the rest of the RV space with hlt (0xf4) just in case.
	 */
	rmpp->rm_rv_code[0] = 0xe9;
	rmpp->rm_rv_code[1] = 0x0d;
	rmpp->rm_rv_code[2] = 0xf0;
	for (off = 3; off < sizeof (rmpp->rm_rv_code); off++)
		rmpp->rm_rv_code[off] = 0xf4;

	return (0);
}

void
mach_cpucontext_fini(void)
{
	hat_unload(kas.a_hat, (caddr_t)(uintptr_t)rm_platter_pa, MMU_PAGESIZE,
	    HAT_UNLOAD);
}

extern void pe32start(void);
extern void long_mode_64(void);

static void
rmp_gdt_init(rm_platter_t *rmpp)
{
	uintptr_t pe32_off, lm64_off;

	/* Use the kas address space for the CPU startup thread. */
	if (mmu_ptob(kas.a_hat->hat_htable->ht_pfn) > 0xffffffffUL) {
		panic("Cannot initialize CPUs; kernel's 64-bit page tables\n"
		    "located above 4G in physical memory (@ 0x%lx)",
		    mmu_ptob(kas.a_hat->hat_htable->ht_pfn));
	}

	/*
	 * Setup pseudo-descriptors for temporary GDT and IDT for use ONLY
	 * by code in real_mode_start_cpu():
	 *
	 * GDT[0]:  NULL selector
	 * GDT[1]:  64-bit CS: Long = 1, Present = 1, bits 12, 11 = 1
	 * GDT[2]:  32-bit CS (big flat)
	 * GDT[3]:  32-bit DS (big flat)
	 *
	 * Clear the IDT as interrupts will be off and a limit of 0 will cause
	 * the CPU to triple fault and reset on an NMI, seemingly as reasonable
	 * a course of action as any other, though it may cause the entire
	 * platform to reset in some cases...
	 */
	rmpp->rm_temp_gdt[0] = 0;
	rmpp->rm_temp_gdt[TEMPGDT_KCODE64] = 0x20980000000000ULL;
	rmpp->rm_temp_gdt[TEMPGDT_KCODE32] = 0xcf9a000000ffffULL;
	rmpp->rm_temp_gdt[TEMPGDT_KDATA32] = 0xcf93000000ffffULL;

	rmpp->rm_temp_gdt_lim = (ushort_t)(sizeof (rmpp->rm_temp_gdt) - 1);
	rmpp->rm_temp_gdt_base = rm_platter_pa +
	    (uint32_t)offsetof(rm_platter_t, rm_temp_gdt);
	rmpp->rm_temp_idt_lim = 0;
	rmpp->rm_temp_idt_base = 0;

	rmpp->rm_basepa = rm_platter_pa;

	/*
	 * Since the CPU needs to jump to protected mode and long mode using an
	 * identity mapped address, we need to calculate it here.
	 */
	pe32_off = (uintptr_t)pe32start - (uintptr_t)real_mode_start_cpu;
	VERIFY3U(pe32_off, <, sizeof (rmpp->rm_code));
	rmpp->rm_pe32_addr = rm_platter_pa + (uint32_t)pe32_off;

	lm64_off = (uintptr_t)long_mode_64 - (uintptr_t)real_mode_start_cpu;
	VERIFY3U(pe32_off, <, sizeof (rmpp->rm_code));
	rmpp->rm_longmode64_addr = rm_platter_pa + (uint32_t)lm64_off;
}

static void *
mach_cpucontext_alloc_tables(struct cpu *cp)
{
	tss_t *ntss;
	struct cpu_tables *ct;
	size_t ctsize;

	/*
	 * Allocate space for stack, tss, gdt and idt. We round the size
	 * allotted for cpu_tables up, so that the TSS is on a unique page.
	 * This is more efficient when running in virtual machines.
	 */
	ctsize = P2ROUNDUP(sizeof (*ct), PAGESIZE);
	ct = kmem_zalloc(ctsize, KM_SLEEP);
	if ((uintptr_t)ct & PAGEOFFSET)
		panic("mach_cpucontext_alloc_tables: cpu%d misaligned tables",
		    cp->cpu_id);

	ntss = cp->cpu_tss = &ct->ct_tss;

	uintptr_t va;
	size_t len;

	/*
	 * #DF (double fault).
	 */
	ntss->tss_ist1 = (uintptr_t)&ct->ct_stack1[sizeof (ct->ct_stack1)];

	/*
	 * #NM (non-maskable interrupt)
	 */
	ntss->tss_ist2 = (uintptr_t)&ct->ct_stack2[sizeof (ct->ct_stack2)];

	/*
	 * #MC (machine check exception / hardware error)
	 */
	ntss->tss_ist3 = (uintptr_t)&ct->ct_stack3[sizeof (ct->ct_stack3)];

	/*
	 * #DB, #BP debug interrupts and KDI/kmdb
	 */
	ntss->tss_ist4 = (uintptr_t)&cp->cpu_m.mcpu_kpti_dbg.kf_tr_rsp;

	if (kpti_enable == 1) {
		/*
		 * #GP, #PF, #SS fault interrupts
		 */
		ntss->tss_ist5 = (uintptr_t)&cp->cpu_m.mcpu_kpti_flt.kf_tr_rsp;

		/*
		 * Used by all other interrupts
		 */
		ntss->tss_ist6 = (uint64_t)&cp->cpu_m.mcpu_kpti.kf_tr_rsp;

		/*
		 * On AMD64 we need to make sure that all of the pages of the
		 * struct cpu_tables are punched through onto the user CPU for
		 * kpti.
		 *
		 * The final page will always be the TSS, so treat that
		 * separately.
		 */
		for (va = (uintptr_t)ct, len = ctsize - MMU_PAGESIZE;
		    len >= MMU_PAGESIZE;
		    len -= MMU_PAGESIZE, va += MMU_PAGESIZE) {
			/* The doublefault stack must be RW */
			hati_cpu_punchin(cp, va, PROT_READ | PROT_WRITE);
		}
		ASSERT3U((uintptr_t)ntss, ==, va);
		hati_cpu_punchin(cp, (uintptr_t)ntss, PROT_READ);
	}

	/*
	 * Set I/O bit map offset equal to size of TSS segment limit
	 * for no I/O permission map. This will cause all user I/O
	 * instructions to generate #gp fault.
	 */
	ntss->tss_bitmapbase = sizeof (*ntss);

	/*
	 * Setup kernel tss.
	 */
	set_syssegd((system_desc_t *)&cp->cpu_gdt[GDT_KTSS], cp->cpu_tss,
	    sizeof (*cp->cpu_tss) - 1, SDT_SYSTSS, SEL_KPL);

	return (ct);
}

void *
mach_cpucontext_alloc(cpu_t *cp)
{
	struct cpu_tables *ct;
	rm_platter_t *rmpp = (rm_platter_t *)rm_platter_va;

	ct = mach_cpucontext_alloc_tables(cp);
	if (ct == NULL) {
		return (NULL);
	}

	/*
	 * Now copy all that we've set up onto the real mode platter
	 * for the real mode code to digest as part of starting the cpu.
	 */
	rmpp->rm_idt_base = cp->cpu_idt;
	rmpp->rm_idt_lim = sizeof (*cp->cpu_idt) * NIDT - 1;
	rmpp->rm_gdt_base = cp->cpu_gdt;
	rmpp->rm_gdt_lim = sizeof (*cp->cpu_gdt) * NGDT - 1;

	/*
	 * CPU needs to access kernel address space after powering on.
	 */
	rmpp->rm_pdbr = MAKECR3(kas.a_hat->hat_htable->ht_pfn, PCID_NONE);
	rmpp->rm_cpu = cp->cpu_id;

	/*
	 * We need to mask off any bits set on our boot CPU that can't apply
	 * while the subject CPU is initializing.  If appropriate, they are
	 * enabled later on.
	 */
	rmpp->rm_cr4 = getcr4();
	rmpp->rm_cr4 &= ~(CR4_MCE | CR4_PCE | CR4_PCIDE);

	rmp_gdt_init(rmpp);

	return (ct);
}

/*
 * XXX This function, a simplified version of i86pc's, is basically nonsense.
 * The principle goes that we save the context so we can later shut down the
 * CPU we've just started.  But we can't shut down CPUs on this platform, ever.
 * And in fact we can't throw away the context even if we don't shut down,
 * because it's used by various interrupt handlers long after startup.
 *
 * More importantly, the ETIMEDOUT case is ridiculous: there is only a single
 * RMP that is used by every AP to start up, and its contents are unique to
 * that AP.  So if a CPU was poked but didn't actually start, if it were to
 * start later it would almost certainly do so on an incorrect RMP; the result
 * would be at best a triple-fault and shutdown, at worst another CPU's state
 * would be trashed and the box would panic or worse.  I've left this here for
 * now but we should consider changing the ETIMEDOUT path to either panic or
 * somehow gain certainty that the hung CPU has been reset to a permanently
 * quiescent state before proceeding to set up the RMP for the next CPU.  Then
 * this can be reduced to freeing the tables in the error path.
 */
void
mach_cpucontext_free(cpu_t *cp, void *arg, int err)
{
	struct cpu_tables *ct = arg;

	ASSERT(&ct->ct_tss == cp->cpu_tss);
	switch (err) {
	case 0:
		cp->cpu_m.mcpu_mach_ctx_ptr = arg;
		break;
	case ETIMEDOUT:
		/*
		 * The processor was poked, but failed to start before
		 * we gave up waiting for it.  In case it starts later,
		 * don't free anything.
		 */
		cp->cpu_m.mcpu_mach_ctx_ptr = arg;
		break;
	default:
		/*
		 * Some other, passive, error occurred.
		 */
		kmem_free(ct, P2ROUNDUP(sizeof (*ct), PAGESIZE));
		cp->cpu_tss = NULL;
		break;
	}
}
