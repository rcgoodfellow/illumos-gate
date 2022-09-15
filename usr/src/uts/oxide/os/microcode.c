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
 * Copyright (c) 2018, Joyent, Inc.
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/asm_linkage.h>
#include <sys/bootconf.h>
#include <sys/cpuvar.h>
#include <sys/cmn_err.h>
#include <sys/controlregs.h>
#include <sys/debug.h>
#include <sys/kobj.h>
#include <sys/kobj_impl.h>
#include <sys/machsystm.h>
#include <sys/ontrap.h>
#include <sys/param.h>
#include <sys/machparam.h>
#include <sys/promif.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/thread.h>
#include <sys/ucode.h>
#include <sys/x86_archext.h>
#include <sys/x_call.h>

/*
 * AMD-specific equivalence table
 */
static ucode_eqtbl_amd_t *ucode_eqtbl_amd;
static uint_t ucode_eqtbl_amd_entries;

/*
 * mcpu_ucode_info for the boot CPU.  Statically allocated.
 */
static struct cpu_ucode_info cpu_ucode_info0;

static ucode_file_t ucodefile;

static void* ucode_zalloc(processorid_t, size_t);
static void ucode_free(processorid_t, void *, size_t);

static int ucode_capable_amd(cpu_t *);
static ucode_errno_t ucode_extract_amd(ucode_update_t *, uint8_t *, int);
static void ucode_file_reset_amd(ucode_file_t *, processorid_t);
static uint32_t ucode_load_amd(ucode_file_t *, cpu_ucode_info_t *, cpu_t *);
static int ucode_equiv_cpu_amd(cpu_t *, uint16_t *);
static ucode_errno_t ucode_locate_amd(cpu_t *, cpu_ucode_info_t *,
    ucode_file_t *);
static ucode_errno_t ucode_match_amd(uint16_t, cpu_ucode_info_t *,
    ucode_file_amd_t *, int);
static void ucode_read_rev_amd(cpu_ucode_info_t *);

static const struct ucode_ops ucode_amd = {
	.write_msr	= MSR_AMD_PATCHLOADER,
	.capable	= ucode_capable_amd,
	.file_reset	= ucode_file_reset_amd,
	.read_rev	= ucode_read_rev_amd,
	.load		= ucode_load_amd,
	.validate	= ucode_validate_amd,
	.extract	= ucode_extract_amd,
	.locate		= ucode_locate_amd
};

const struct ucode_ops *ucode;

static const char ucode_failure_fmt[] =
	"cpu%d: failed to update microcode from version 0x%x to 0x%x";
static const char ucode_success_fmt[] =
	"?cpu%d: microcode has been updated from version 0x%x to 0x%x\n";

/*
 * Force flag.  If set, the first microcode binary that matches
 * signature and platform id will be used for microcode update,
 * regardless of version.  Should only be used for debugging.
 */
int ucode_force_update = 0;

/*
 * Allocate space for mcpu_ucode_info in the machcpu structure
 * for all non-boot CPUs.
 */
void
ucode_alloc_space(cpu_t *cp)
{
	ASSERT(cp->cpu_id != 0);
	ASSERT(cp->cpu_m.mcpu_ucode_info == NULL);
	cp->cpu_m.mcpu_ucode_info =
	    kmem_zalloc(sizeof (*cp->cpu_m.mcpu_ucode_info), KM_SLEEP);
}

void
ucode_free_space(cpu_t *cp)
{
	ASSERT(cp->cpu_m.mcpu_ucode_info != NULL);
	ASSERT(cp->cpu_m.mcpu_ucode_info != &cpu_ucode_info0);
	kmem_free(cp->cpu_m.mcpu_ucode_info,
	    sizeof (*cp->cpu_m.mcpu_ucode_info));
	cp->cpu_m.mcpu_ucode_info = NULL;
}

/*
 * Called when we are done with microcode update on all processors to free up
 * space allocated for the microcode file.
 */
void
ucode_cleanup()
{
	if (ucode == NULL)
		return;

	ucode->file_reset(&ucodefile, -1);
}

/*
 * Allocate/free a buffer used to hold ucode data. Space for the boot CPU is
 * allocated with BOP_ALLOC() and does not require a free.
 */
static void*
ucode_zalloc(processorid_t id, size_t size)
{
	if (id != 0)
		return (kmem_zalloc(size, KM_NOSLEEP));

	/* BOP_ALLOC() failure results in panic */
	return (BOP_ALLOC(bootops, NULL, size, MMU_PAGESIZE));
}

static void
ucode_free(processorid_t id, void* buf, size_t size)
{
	if (id != 0)
		kmem_free(buf, size);
}

/*
 * Check whether or not a processor is capable of microcode operations
 * Returns 1 if it is capable, 0 if not.
 *
 * At this point we only support microcode update for:
 * - AMD processors family 0x10 and above.
 */
static int
ucode_capable_amd(cpu_t *cp)
{
	return (cpuid_getfamily(cp) >= 0x10);
}

/*
 * Called when it is no longer necessary to keep the microcode around,
 * or when the cached microcode doesn't match the CPU being processed.
 */
static void
ucode_file_reset_amd(ucode_file_t *ufp, processorid_t id)
{
	ucode_file_amd_t *ucodefp = ufp->amd;

	if (ucodefp == NULL)
		return;

	ucode_free(id, ucodefp, sizeof (ucode_file_amd_t));
	ufp->amd = NULL;
}

/*
 * Find the equivalent CPU id in the equivalence table.
 */
static int
ucode_equiv_cpu_amd(cpu_t *cp, uint16_t *eq_sig)
{
	char name[MAXPATHLEN];
	int cpi_sig = cpuid_getsig(cp);

	(void) snprintf(name, MAXPATHLEN, "/%s/%s/equivalence-table",
	    UCODE_INSTALL_PATH, cpuid_getvendorstr(cp));

	if (cp->cpu_id == 0) {
		/*
		 * No kmem_zalloc() etc. available on boot cpu.
		 */
		ucode_eqtbl_amd_t eqtbl;
		int count, offset = 0;
		intptr_t fd;

		if ((fd = kobj_open(name)) == -1)
			return (EM_OPENFILE);
		do {
			count = kobj_read(fd, (int8_t *)&eqtbl,
			    sizeof (eqtbl), offset);
			if (count != sizeof (eqtbl)) {
				(void) kobj_close(fd);
				return (EM_HIGHERREV);
			}
			offset += count;
		} while (eqtbl.ue_inst_cpu != 0 &&
		    eqtbl.ue_inst_cpu != cpi_sig);
		(void) kobj_close(fd);
		*eq_sig = eqtbl.ue_equiv_cpu;
	} else {
		ucode_eqtbl_amd_t *eqtbl;

		/*
		 * If not already done, load the equivalence table.
		 * Not done on boot CPU.
		 */
		if (ucode_eqtbl_amd == NULL) {
			struct _buf *eq;
			uint64_t size;
			int count;

			if ((eq = kobj_open_file(name)) == (struct _buf *)-1)
				return (EM_OPENFILE);

			if (kobj_get_filesize(eq, &size) < 0) {
				kobj_close_file(eq);
				return (EM_OPENFILE);
			}

			if (size == 0 ||
			    size % sizeof (*ucode_eqtbl_amd) != 0) {
				kobj_close_file(eq);
				return (EM_HIGHERREV);
			}

			ucode_eqtbl_amd = kmem_zalloc(size, KM_NOSLEEP);
			if (ucode_eqtbl_amd == NULL) {
				kobj_close_file(eq);
				return (EM_NOMEM);
			}
			count = kobj_read_file(eq, (char *)ucode_eqtbl_amd,
			    size, 0);
			kobj_close_file(eq);

			if (count != size) {
				ucode_eqtbl_amd_entries = 0;
				return (EM_FILESIZE);
			}

			ucode_eqtbl_amd_entries =
			    size / sizeof (*ucode_eqtbl_amd);
		}

		eqtbl = ucode_eqtbl_amd;
		*eq_sig = 0;
		for (uint_t i = 0; i < ucode_eqtbl_amd_entries; i++, eqtbl++) {
			if (eqtbl->ue_inst_cpu == 0) {
				/* End of table */
				return (EM_HIGHERREV);
			}
			if (eqtbl->ue_inst_cpu == cpi_sig) {
				*eq_sig = eqtbl->ue_equiv_cpu;
				return (EM_OK);
			}
		}
		/*
		 * No equivalent CPU id found, assume outdated microcode file.
		 */
		return (EM_HIGHERREV);
	}

	return (EM_OK);
}

/*
 * Populate the ucode file structure from microcode file corresponding to
 * this CPU, if exists.
 *
 * Return EM_OK on success, corresponding error code on failure.
 */
static ucode_errno_t
ucode_locate_amd(cpu_t *cp, cpu_ucode_info_t *uinfop, ucode_file_t *ufp)
{
	char name[MAXPATHLEN];
	intptr_t fd;
	int count, rc;
	ucode_file_amd_t *ucodefp = ufp->amd;

	uint16_t eq_sig = 0;

	/* get equivalent CPU id */
	if ((rc = ucode_equiv_cpu_amd(cp, &eq_sig)) != EM_OK)
		return (rc);

	/*
	 * Allocate a buffer for the microcode patch. If the buffer has been
	 * allocated before, check for a matching microcode to avoid loading
	 * the file again.
	 */

	if (ucodefp == NULL) {
		ucodefp = ucode_zalloc(cp->cpu_id, sizeof (*ucodefp));
	} else if (ucode_match_amd(eq_sig, uinfop, ucodefp, sizeof (*ucodefp))
	    == EM_OK) {
		return (EM_OK);
	}

	if (ucodefp == NULL)
		return (EM_NOMEM);

	ufp->amd = ucodefp;

	/*
	 * Find the patch for this CPU. The patch files are named XXXX-YY, where
	 * XXXX is the equivalent CPU id and YY is the running patch number.
	 * Patches specific to certain chipsets are guaranteed to have lower
	 * numbers than less specific patches, so we can just load the first
	 * patch that matches.
	 */

	for (uint_t i = 0; i < 0xff; i++) {
		(void) snprintf(name, MAXPATHLEN, "/%s/%s/%04X-%02X",
		    UCODE_INSTALL_PATH, cpuid_getvendorstr(cp), eq_sig, i);
		if ((fd = kobj_open(name)) == -1)
			return (EM_NOMATCH);
		count = kobj_read(fd, (char *)ucodefp, sizeof (*ucodefp), 0);
		(void) kobj_close(fd);

		if (ucode_match_amd(eq_sig, uinfop, ucodefp, count) == EM_OK)
			return (EM_OK);
	}

	return (EM_NOMATCH);
}

static ucode_errno_t
ucode_match_amd(uint16_t eq_sig, cpu_ucode_info_t *uinfop,
    ucode_file_amd_t *ucodefp, int size)
{
	ucode_header_amd_t *uh;

	if (ucodefp == NULL || size < sizeof (ucode_header_amd_t))
		return (EM_NOMATCH);

	uh = &ucodefp->uf_header;

	/*
	 * Don't even think about loading patches that would require code
	 * execution. Does not apply to patches for family 0x14 and beyond.
	 */
	if (uh->uh_cpu_rev < 0x5000 &&
	    size > offsetof(ucode_file_amd_t, uf_code_present) &&
	    ucodefp->uf_code_present) {
		return (EM_NOMATCH);
	}

	if (eq_sig != uh->uh_cpu_rev)
		return (EM_NOMATCH);

	if (uh->uh_nb_id) {
		cmn_err(CE_WARN, "ignoring northbridge-specific ucode: "
		    "chipset id %x, revision %x", uh->uh_nb_id, uh->uh_nb_rev);
		return (EM_NOMATCH);
	}

	if (uh->uh_sb_id) {
		cmn_err(CE_WARN, "ignoring southbridge-specific ucode: "
		    "chipset id %x, revision %x", uh->uh_sb_id, uh->uh_sb_rev);
		return (EM_NOMATCH);
	}

	if (uh->uh_patch_id <= uinfop->cui_rev && !ucode_force_update)
		return (EM_HIGHERREV);

	return (EM_OK);
}

static uint32_t
ucode_load_amd(ucode_file_t *ufp, cpu_ucode_info_t *uinfop, cpu_t *cp)
{
	ucode_file_amd_t *ucodefp = ufp->amd;
	on_trap_data_t otd;

	ASSERT(ucode);
	ASSERT(ucodefp);

	kpreempt_disable();
	if (on_trap(&otd, OT_DATA_ACCESS)) {
		no_trap();
		goto out;
	}
	wrmsr(ucode->write_msr, (uintptr_t)ucodefp);
	no_trap();
	ucode->read_rev(uinfop);

out:
	kpreempt_enable();
	return (ucodefp->uf_header.uh_patch_id);
}

static void
ucode_read_rev_amd(cpu_ucode_info_t *uinfop)
{
	uinfop->cui_rev = rdmsr(MSR_AMD_PATCHLEVEL);
}

static ucode_errno_t
ucode_extract_amd(ucode_update_t *uusp, uint8_t *ucodep, int size)
{
	uint32_t *ptr = (uint32_t *)ucodep;
	ucode_eqtbl_amd_t *eqtbl;
	ucode_file_amd_t *ufp;
	int count;
	int higher = 0;
	ucode_errno_t rc = EM_NOMATCH;
	uint16_t eq_sig;

	/* skip over magic number & equivalence table header */
	ptr += 2; size -= 8;

	count = *ptr++; size -= 4;
	for (eqtbl = (ucode_eqtbl_amd_t *)ptr;
	    eqtbl->ue_inst_cpu && eqtbl->ue_inst_cpu != uusp->sig;
	    eqtbl++)
		;

	eq_sig = eqtbl->ue_equiv_cpu;

	/* No equivalent CPU id found, assume outdated microcode file. */
	if (eq_sig == 0)
		return (EM_HIGHERREV);

	/* Use the first microcode patch that matches. */
	do {
		ptr += count >> 2; size -= count;

		if (!size)
			return (higher ? EM_HIGHERREV : EM_NOMATCH);

		ptr++; size -= 4;
		count = *ptr++; size -= 4;
		ufp = (ucode_file_amd_t *)ptr;

		rc = ucode_match_amd(eq_sig, &uusp->info, ufp, count);
		if (rc == EM_HIGHERREV)
			higher = 1;
	} while (rc != EM_OK);

	uusp->ucodep = (uint8_t *)ufp;
	uusp->usize = count;
	uusp->expected_rev = ufp->uf_header.uh_patch_id;

	return (EM_OK);
}

static int
ucode_write(xc_arg_t arg1, xc_arg_t unused2, xc_arg_t unused3)
{
	ucode_update_t *uusp = (ucode_update_t *)arg1;
	cpu_ucode_info_t *uinfop = CPU->cpu_m.mcpu_ucode_info;
	on_trap_data_t otd;

	ASSERT(ucode);
	ASSERT(uusp->ucodep);

	/*
	 * Check one more time to see if it is really necessary to update
	 * microcode just in case this is a hyperthreaded processor where
	 * the threads share the same microcode.
	 */
	if (!ucode_force_update) {
		ucode->read_rev(uinfop);
		uusp->new_rev = uinfop->cui_rev;
		if (uinfop->cui_rev >= uusp->expected_rev)
			return (0);
	}

	if (!on_trap(&otd, OT_DATA_ACCESS))
		wrmsr(ucode->write_msr, (uintptr_t)uusp->ucodep);

	no_trap();
	ucode->read_rev(uinfop);
	uusp->new_rev = uinfop->cui_rev;

	return (0);
}

/*
 * Entry point to microcode update from the ucode_drv driver.
 *
 * Returns EM_OK on success, corresponding error code on failure.
 */
ucode_errno_t
ucode_update(uint8_t *ucodep, int size)
{
	int		found = 0;
	processorid_t	id;
	ucode_update_t	cached = { 0 };
	ucode_update_t	*cachedp = NULL;
	ucode_errno_t	rc = EM_OK;
	ucode_errno_t	search_rc = EM_NOMATCH; /* search result */
	cpuset_t cpuset;

	ASSERT(ucode);
	ASSERT(ucodep);
	CPUSET_ZERO(cpuset);

	if (!ucode->capable(CPU))
		return (EM_NOTSUP);

	mutex_enter(&cpu_lock);

	for (id = 0; id < max_ncpus; id++) {
		cpu_t *cpu;
		ucode_update_t uus = { 0 };
		ucode_update_t *uusp = &uus;

		/*
		 * If there is no such CPU or it is not xcall ready, skip it.
		 */
		if ((cpu = cpu_get(id)) == NULL ||
		    !(cpu->cpu_flags & CPU_READY))
			continue;

		uusp->sig = cpuid_getsig(cpu);
		bcopy(cpu->cpu_m.mcpu_ucode_info, &uusp->info,
		    sizeof (uusp->info));

		/*
		 * If the current CPU has the same signature and platform
		 * id as the previous one we processed, reuse the information.
		 */
		if (cachedp && cachedp->sig == cpuid_getsig(cpu) &&
		    cachedp->info.cui_platid == uusp->info.cui_platid) {
			uusp->ucodep = cachedp->ucodep;
			uusp->expected_rev = cachedp->expected_rev;
			/*
			 * Intuitively we should check here to see whether the
			 * running microcode rev is >= the expected rev, and
			 * quit if it is.  But we choose to proceed with the
			 * xcall regardless of the running version so that
			 * the other threads in an HT processor can update
			 * the cpu_ucode_info structure in machcpu.
			 */
		} else if ((search_rc = ucode->extract(uusp, ucodep, size))
		    == EM_OK) {
			bcopy(uusp, &cached, sizeof (cached));
			cachedp = &cached;
			found = 1;
		}

		/* Nothing to do */
		if (uusp->ucodep == NULL)
			continue;

		CPUSET_ADD(cpuset, id);
		kpreempt_disable();
		xc_sync((xc_arg_t)uusp, 0, 0, CPUSET2BV(cpuset), ucode_write);
		kpreempt_enable();
		CPUSET_DEL(cpuset, id);

		if (uusp->new_rev != 0 && uusp->info.cui_rev == uusp->new_rev &&
		    !ucode_force_update) {
			rc = EM_HIGHERREV;
		} else if ((uusp->new_rev == 0) || (uusp->expected_rev != 0 &&
		    uusp->expected_rev != uusp->new_rev)) {
			cmn_err(CE_WARN, ucode_failure_fmt,
			    id, uusp->info.cui_rev, uusp->expected_rev);
			rc = EM_UPDATE;
		} else {
			cmn_err(CE_CONT, ucode_success_fmt,
			    id, uusp->info.cui_rev, uusp->new_rev);
		}
	}

	mutex_exit(&cpu_lock);

	if (!found) {
		rc = search_rc;
	} else if (rc == EM_OK) {
		cpuid_post_ucodeadm();
	}

	return (rc);
}

/*
 * Entry point to microcode update from mlsetup() and mp_startup()
 * Initialize mcpu_ucode_info, and perform microcode update if necessary.
 * cpuid_info must be initialized before ucode_check can be called.
 */
void
ucode_check(cpu_t *cp)
{
	cpu_ucode_info_t *uinfop;
	ucode_errno_t rc = EM_OK;

	ASSERT(cp);
	/*
	 * Space statically allocated for BSP, ensure pointer is set
	 */
	if (cp->cpu_id == 0 && cp->cpu_m.mcpu_ucode_info == NULL)
		cp->cpu_m.mcpu_ucode_info = &cpu_ucode_info0;

	uinfop = cp->cpu_m.mcpu_ucode_info;
	ASSERT(uinfop);

	/* set up function pointers if not already done */
	if (!ucode)
		switch (cpuid_getvendor(cp)) {
		case X86_VENDOR_AMD:
			ucode = &ucode_amd;
			break;
		default:
			ucode = NULL;
			return;
		}

	if (!ucode->capable(cp))
		return;

	ucode->read_rev(uinfop);

	/*
	 * Check to see if we need ucode update
	 */
	if ((rc = ucode->locate(cp, uinfop, &ucodefile)) == EM_OK) {
		uint32_t old_rev, new_rev;

		old_rev = uinfop->cui_rev;
		new_rev = ucode->load(&ucodefile, uinfop, cp);

		if (uinfop->cui_rev != new_rev) {
			cmn_err(CE_WARN, ucode_failure_fmt, cp->cpu_id,
			    old_rev, new_rev);
		} else {
			cmn_err(CE_CONT, ucode_success_fmt, cp->cpu_id,
			    old_rev, new_rev);
		}
	}

	/*
	 * If we fail to find a match for any reason, free the file structure
	 * just in case we have read in a partial file.
	 *
	 * Since the scratch memory for holding the microcode for the boot CPU
	 * came from BOP_ALLOC, we will reset the data structure as if we
	 * never did the allocation so we don't have to keep track of this
	 * special chunk of memory.  We free the memory used for the rest
	 * of the CPUs in start_other_cpus().
	 */
	if (rc != EM_OK || cp->cpu_id == 0)
		ucode->file_reset(&ucodefile, cp->cpu_id);
}

/*
 * Returns microcode revision from the machcpu structure.
 */
ucode_errno_t
ucode_get_rev(uint32_t *revp)
{
	int i;

	ASSERT(ucode);
	ASSERT(revp);

	if (!ucode->capable(CPU))
		return (EM_NOTSUP);

	mutex_enter(&cpu_lock);
	for (i = 0; i < max_ncpus; i++) {
		cpu_t *cpu;

		if ((cpu = cpu_get(i)) == NULL)
			continue;

		revp[i] = cpu->cpu_m.mcpu_ucode_info->cui_rev;
	}
	mutex_exit(&cpu_lock);

	return (EM_OK);
}
