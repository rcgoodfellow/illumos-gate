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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, Intel Corporation.
 * All rights reserved.
 *
 * Copyright (c) 2012 Gary Mills
 * Copyright 2020 Joyent, Inc.
 * Copyright 2021 Oxide Computer Co.
 */

/*
 * This file contains the functionality that mimics the boot operations
 * on SPARC systems or the old boot.bin/multiboot programs on x86 systems.
 * The x86 kernels now do everything on their own.
 */

#include <sys/types.h>
#include <sys/bootconf.h>
#include <sys/bootsvcs.h>
#include <sys/bootinfo.h>
#include <sys/bootvfs.h>
#include <sys/bootprops.h>
#include <sys/varargs.h>
#include <sys/param.h>
#include <sys/machparam.h>
#include <sys/machsystm.h>
#include <sys/archsystm.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/promif.h>
#include <sys/archsystm.h>
#include <sys/x86_archext.h>
#include <sys/kobj.h>
#include <sys/privregs.h>
#include <sys/sysmacros.h>
#include <sys/ctype.h>
#include <vm/kboot_mmu.h>
#include <vm/hat_pte.h>
#include <sys/kobj.h>
#include <sys/kobj_lex.h>
#include <sys/pci_cfgspace_impl.h>
#include <sys/ddipropdefs.h>	/* For DDI prop types */
#include <sys/boot_data.h>
#include <sys/dw_apb_uart.h>
#include <sys/uart.h>
#include <sys/boot_debug.h>

#include <milan/milan_apob.h>

/*
 * Comes from fs/ufsops.c.  For debugging the ramdisk/root fs operations.  Set
 * by the existence of the boot property of the same name.
 */
extern int bootrd_debug;

/*
 * General early boot (pre-kobj, pre-prom_printf) debug flag.  Set by the
 * existence of the boot property of the same name.
 */
boolean_t kbm_debug = 0;

static bootops_t bootop;
static struct bsys_mem bm;
static const bt_prop_t *bt_props;

static struct memlist bsml_usable;
static struct memlist *eb_ram;
static struct memlist bsml_apob;

/*
 * some allocator statistics
 */
static ulong_t total_bop_alloc_scratch = 0;
static ulong_t total_bop_alloc_kernel = 0;

/*
 * Not used here, but required by the openprom driver.
 */
char saved_cmdline[1] = "";

static uintptr_t dw_apb_uart_hdl;

static void
bcons_init(void)
{
	dw_apb_uart_hdl = dw_apb_uart_init(DAP_0, 3000000,
		AD_8BITS, AP_NONE, AS_1BIT);
}

static void
bcons_putchar(int c)
{
	static const uint8_t CR = '\r';
	uint8_t ch = (uint8_t)(c);

	if (ch == '\n')
		dw_apb_uart_tx(dw_apb_uart_hdl, &CR, 1);
	dw_apb_uart_tx(dw_apb_uart_hdl, &ch, 1);
}

static int
bcons_getchar(void)
{
	return (int)(dw_apb_uart_rx_one(dw_apb_uart_hdl));
}

static int
bcons_ischar(void)
{
	return dw_apb_uart_dr(dw_apb_uart_hdl);
}

void
kbm_debug_printf(const char *file, int line, const char *fmt, ...)
{
	/*
	 * This use of a static is safe because we are always single-threaded
	 * when this code is running.
	 */
	static boolean_t continuation = 0;
	size_t fmtlen = strlen(fmt);
	boolean_t is_end = (fmt[fmtlen - 1] == '\n');
	va_list ap;

	if (!kbm_debug)
		return;

	if (!continuation)
		bop_printf(NULL, "%s:%d: ", file, line);

	va_start(ap, fmt);
	vbop_printf(NULL, fmt, ap);
	va_end(ap);

	continuation = !is_end;
}

paddr_t
do_bop_phys_alloc(uint64_t size, uint64_t align)
{
	static paddr_t next_phys = (paddr_t)0x600000; /* from Mapfile.amd64 */
	paddr_t	pa = -(paddr_t)1;
	paddr_t	start;
	paddr_t	end;
	struct memlist *ml = eb_ram;

	size = P2ROUNDUP(size, align);
	for (; ml; ml = ml->ml_next) {
		start = P2ROUNDUP(ml->ml_address, align);
		end = P2ALIGN(ml->ml_address + ml->ml_size, align);
		if (start < next_phys)
			start = P2ROUNDUP(next_phys, align);

		if (end <= start)
			continue;
		if (end - start < size)
			continue;

		if (start < pa)
			pa = start;
	}
	if (pa != -(paddr_t)1) {
		next_phys = pa + size;
		return (pa);
	}
	bop_panic("do_bop_phys_alloc(0x%" PRIx64 ", 0x%" PRIx64
	    ") Out of memory\n", size, align);
	/*NOTREACHED*/
}

/*
 * Allocate and map memory. The size is always rounded up to a multiple
 * of base pagesize.
 */

/*ARGSUSED*/
static caddr_t
do_bsys_alloc(bootops_t *bop, caddr_t virthint, size_t size, int align)
{
	paddr_t a = align;	/* same type as pa for masking */
	uint_t pgsize;
	paddr_t pa;
	uintptr_t va;
	ssize_t s;		/* the aligned size */
	uint_t level;
	x86pte_t pte_flags = PT_WRITABLE;
	boolean_t is_kernel = (virthint != 0);

	if (is_kernel) {
		pte_flags |= PT_GLOBAL;
	}

	if (a < MMU_PAGESIZE)
		a = MMU_PAGESIZE;
	else if (!ISP2(a))
		prom_panic("do_bsys_alloc() incorrect alignment");
	size = P2ROUNDUP(size, MMU_PAGESIZE);

	/*
	 * Use the next aligned virtual address if we weren't given one.
	 */
	if (virthint == NULL) {
		virthint = (caddr_t)kbm_valloc(size, a);
		total_bop_alloc_scratch += size;
	} else {
		total_bop_alloc_kernel += size;
	}

	/*
	 * allocate the physical memory
	 */
	pa = do_bop_phys_alloc(size, a);

	DBG_MSG("bsys_alloc: alloc sz %lx pa %lx for va %p...",
		    size, pa, virthint);

	/*
	 * Add the mappings to the page tables, try large pages first.
	 */
	va = (uintptr_t)virthint;
	s = size;
	level = 1;
	pgsize = TWO_MEG;
	if ((a & (pgsize - 1)) == 0) {
		while (IS_P2ALIGNED(pa, pgsize) && IS_P2ALIGNED(va, pgsize) &&
		    s >= pgsize) {
			kbm_map(va, pa, level, pte_flags);
			va += pgsize;
			pa += pgsize;
			s -= pgsize;
		}
	}

	/*
	 * Map remaining pages use small mappings
	 */
	level = 0;
	pgsize = MMU_PAGESIZE;
	while (s > 0) {
		kbm_map(va, pa, level, pte_flags);
		va += pgsize;
		pa += pgsize;
		s -= pgsize;
	}

	DBG_MSG("done (%lx @ %p)\n", size, virthint);

	return (virthint);
}

/*
 * Free virtual memory - we'll just ignore these.
 */
/*ARGSUSED*/
static void
do_bsys_free(bootops_t *bop, caddr_t virt, size_t size)
{
	bop_printf(NULL, "do_bsys_free(virt=0x%p, size=0x%lx) ignored\n",
	    (void *)virt, size);
}

/*
 * Old interface
 */
/*ARGSUSED*/
static caddr_t
do_bsys_ealloc(bootops_t *bop, caddr_t virthint, size_t size,
    int align, int flags)
{
	prom_panic("unsupported call to BOP_EALLOC()\n");
	return (0);
}

#define	FIND_BT_PROP_F_NO_FALLBACK	0x1U
#define	FIND_BT_PROP_F_ONLY_FALLBACK	0x2U

static const bt_prop_t *
find_bt_prop(const char *name, uint32_t flags)
{
	const bt_prop_t *btpp;

	if ((flags & FIND_BT_PROP_F_ONLY_FALLBACK) != 0 &&
	    (flags & FIND_BT_PROP_F_NO_FALLBACK) != 0) {
		bop_panic("conflicting flags passed to find_bt_prop()");
	}

	if ((flags & FIND_BT_PROP_F_ONLY_FALLBACK) == 0) {
		for (btpp = bt_props; btpp != NULL; btpp = btpp->btp_next) {
			if (strcmp(name, btpp->btp_name) == 0)
				return (btpp);
		}
	}

	if (flags & FIND_BT_PROP_F_NO_FALLBACK)
		return (NULL);

	for (btpp = bt_fallback_props; btpp != NULL; btpp = btpp->btp_next) {
		if (strcmp(name, btpp->btp_name) == 0)
			return (btpp);
	}

	return (NULL);
}

/*
 * to find the type of the value associated with this name
 */
/*ARGSUSED*/
int
do_bsys_getproptype(bootops_t *bop, const char *name)
{
	const bt_prop_t *btpp;

	if ((btpp = find_bt_prop(name, 0)) == NULL)
		return (-1);

	return (btpp->btp_typeflags & DDI_PROP_TYPE_MASK);
}

/*
 * to find the size of the buffer to allocate
 */
/*ARGSUSED*/
int
do_bsys_getproplen(bootops_t *bop, const char *name)
{
	const bt_prop_t *btpp;

	if ((btpp = find_bt_prop(name, 0)) == NULL)
		return (-1);

	/*
	 * The signature of this method should really be changed instead to
	 * return a size_t.  Until we do that work, this ugly thing.
	 */
	if (btpp->btp_vlen > INT_MAX) {
		bop_panic("value for property %s has length %lu, which "
		    "cannot be represented to the legacy bootops interface",
		    name, btpp->btp_vlen);
	}

	return (btpp->btp_vlen);
}

/*
 * get the value associated with this name
 */
/*ARGSUSED*/
int
do_bsys_getprop(bootops_t *bop, const char *name, void *value)
{
	const bt_prop_t *btpp;

	if ((btpp = find_bt_prop(name, 0)) == NULL)
		return (-1);

	bcopy(btpp->btp_value, value, btpp->btp_vlen);
	return (0);
}

/*
 * get the name of the next property in succession from the standalone
 */
/*ARGSUSED*/
static char *
do_bsys_nextprop(bootops_t *bop, char *name)
{
	const bt_prop_t *btpp;

	/*
	 * We want to return all the normal properties (from the SP) in order;
	 * if we're given NULL we're being asked for the named of the first
	 * one.  However, once those are exhausted, we want to return the
	 * fallback properties iff they're not shadowed by a real property.
	 *
	 * In principle this should all be a merged map, which would be much
	 * faster, but this whole path is run through only once and this is
	 * still fairly simple: once we're given the name of a property that
	 * exists only as a fallback, we return only fallbacks.
	 */
	if (name == NULL || strlen(name) == 0) {
		if (bt_props != NULL) {
			return ((char *)bt_props->btp_name);
		}
		return ((char *)bt_fallback_props->btp_name);
	}

	btpp = find_bt_prop(name, FIND_BT_PROP_F_NO_FALLBACK);
	if (btpp != NULL) {
		if (btpp->btp_next != NULL)
			return ((char *)btpp->btp_next->btp_name);
		btpp = bt_fallback_props;
	} else {
		btpp = find_bt_prop(name, FIND_BT_PROP_F_ONLY_FALLBACK);
		if (btpp == NULL) {
			bop_panic("unknown boot-time property name '%s' "
			    "passed as previous property name", name);
		}
		btpp = btpp->btp_next;
	}

	while (btpp != NULL &&
	    find_bt_prop(btpp->btp_name, FIND_BT_PROP_F_NO_FALLBACK) != NULL)
		btpp = btpp->btp_next;

	/* XXX constify this interface properly; it has few consumers */
	return (btpp == NULL ? NULL : (char *)btpp->btp_name);
}

static boolean_t
unprintable(char *value, int size)
{
	int i;

	if (size <= 0 || value[0] == '\0')
		return (B_TRUE);

	for (i = 0; i < size; i++) {
		if (value[i] == '\0')
			return (i != (size - 1));

		if (!isprint(value[i]))
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Print out information about all boot properties.
 * buffer is pointer to pre-allocated space to be used as temporary
 * space for property values.
 */
static void
boot_prop_display(char *buffer)
{
	char *name = "";
	int i, len, flags, *buf32;
	int64_t *buf64;

	bop_printf(NULL, "\nBoot properties:\n");

	while ((name = do_bsys_nextprop(NULL, name)) != NULL) {
		bop_printf(NULL, "\t0x%p %s = ", (void *)name, name);
		(void) do_bsys_getprop(NULL, name, buffer);
		len = do_bsys_getproplen(NULL, name);
		flags = do_bsys_getproptype(NULL, name);
		bop_printf(NULL, "len=%d ", len);

		switch (flags) {
		case DDI_PROP_TYPE_INT:
			len = len / sizeof (int);
			buf32 = (int *)buffer;
			for (i = 0; i < len; i++) {
				bop_printf(NULL, "%08x", buf32[i]);
				if (i < len - 1)
					bop_printf(NULL, ".");
			}
			break;
		case DDI_PROP_TYPE_STRING:
			bop_printf(NULL, "%s", buffer);
			break;
		case DDI_PROP_TYPE_INT64:
			len = len / sizeof (int64_t);
			buf64 = (int64_t *)buffer;
			for (i = 0; i < len; i++) {
				bop_printf(NULL, "%016" PRIx64, buf64[i]);
				if (i < len - 1)
					bop_printf(NULL, ".");
			}
			break;
		default:
			if (!unprintable(buffer, len)) {
				buffer[len] = 0;
				bop_printf(NULL, "%s", buffer);
				break;
			}
			for (i = 0; i < len; i++) {
				bop_printf(NULL, "%02x", buffer[i] & 0xff);
				if (i < len - 1)
					bop_printf(NULL, ".");
			}
			break;
		}
		bop_printf(NULL, "\n");
	}
}

/*
 * bootenv.rc is not supported on this architecture.  We've already done the
 * equivalent long before anyone could ask us to do so.
 */
void
read_bootenvrc(void)
{
}

/*
 * print formatted output
 */
/*ARGSUSED*/
void
vbop_printf(void *ptr, const char *fmt, va_list ap)
{
	const char *cp;
	static char buffer[512];

	(void) vsnprintf(buffer, sizeof(buffer), fmt, ap);
	for (cp = buffer; *cp != '\0'; ++cp)
		bcons_putchar(*cp);
}

/*PRINTFLIKE2*/
void
bop_printf(void *bop, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vbop_printf(bop, fmt, ap);
	va_end(ap);
}

/*
 * Another panic() variant; this one can be used even earlier during boot than
 * prom_panic().
 */
/*PRINTFLIKE1*/
void
bop_panic(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vbop_printf(NULL, fmt, ap);
	va_end(ap);

	bop_printf(NULL, "\nPress any key to reboot.\n");
	(void) bcons_getchar();
	bop_printf(NULL, "Resetting...\n");
	pc_reset();
}

static struct boot_syscalls bop_sysp = {
	bcons_getchar,
	bcons_putchar,
	bcons_ischar,
};

/*
 * simple description of a stack frame (args are 64 bit only currently)
 */
typedef struct bop_frame {
	struct bop_frame *old_frame;
	pc_t retaddr;
	long arg[1];
} bop_frame_t;

static boolean_t
weakish_is_null(void *p)
{
	return (p == NULL);
}

/* XXX shareable */
void
bop_traceback(bop_frame_t *frame)
{
	pc_t pc;
	int cnt;
	char *ksym;
	ulong_t off;
	extern kmutex_t mod_lock;

	bop_printf(NULL, "Stack traceback:\n");
	for (cnt = 0; cnt < 30; ++cnt) {	/* up to 30 frames */
		pc = frame->retaddr;
		if (pc == 0)
			break;

		if (weakish_is_null(&mod_lock))
			ksym = NULL;
		else
			ksym = kobj_getsymname(pc, &off);

		if (ksym != NULL)
			bop_printf(NULL, "  %s+%lx", ksym, off);
		else
			bop_printf(NULL, "  0x%lx", pc);

		frame = frame->old_frame;
		if (frame == 0) {
			bop_printf(NULL, "\n");
			break;
		}
		bop_printf(NULL, "\n");
	}
}

struct trapframe {
	ulong_t error_code;	/* optional */
	ulong_t inst_ptr;
	ulong_t code_seg;
	ulong_t flags_reg;
	ulong_t stk_ptr;
	ulong_t stk_seg;
};

/* XXX shareable */
void
bop_trap(ulong_t *tfp)
{
	struct trapframe *tf = (struct trapframe *)tfp;
	bop_frame_t fakeframe;
	static int depth = 0;

	/*
	 * Check for an infinite loop of traps.
	 */
	if (++depth > 2)
		bop_panic("Nested trap");

	bop_printf(NULL, "Unexpected trap\n");

	/*
	 * adjust the tf for optional error_code by detecting the code selector
	 */
	if (tf->code_seg != B64CODE_SEL)
		tf = (struct trapframe *)(tfp - 1);
	else
		bop_printf(NULL, "error code           0x%lx\n",
		    tf->error_code & 0xffffffff);

	bop_printf(NULL, "instruction pointer  0x%lx\n", tf->inst_ptr);
	bop_printf(NULL, "code segment         0x%lx\n", tf->code_seg & 0xffff);
	bop_printf(NULL, "flags register       0x%lx\n", tf->flags_reg);
	bop_printf(NULL, "return %%rsp          0x%lx\n", tf->stk_ptr);
	bop_printf(NULL, "return %%ss           0x%lx\n", tf->stk_seg & 0xffff);
	bop_printf(NULL, "%%cr2			0x%lx\n", getcr2());

	/* grab %[er]bp pushed by our code from the stack */
	fakeframe.old_frame = (bop_frame_t *)*(tfp - 3);
	fakeframe.retaddr = (pc_t)tf->inst_ptr;
	bop_printf(NULL, "Attempting stack backtrace:\n");
	bop_traceback(&fakeframe);
	bop_panic("unexpected trap in early boot");
}

extern void bop_trap_handler(void);
static gate_desc_t *bop_idt;
static desctbr_t bop_idt_info;

/* XXX shareable? */
/*
 * Install a temporary IDT that lets us catch errors in the boot time code.
 * We shouldn't get any faults at all while this is installed, so we'll
 * just generate a traceback and exit.
 */
static void
bop_idt_init(void)
{
	int t;

	bop_idt = (gate_desc_t *)
	    do_bsys_alloc(NULL, NULL, MMU_PAGESIZE, MMU_PAGESIZE);
	bzero(bop_idt, MMU_PAGESIZE);
	for (t = 0; t < NIDT; ++t) {
		/*
		 * Note that since boot runs without a TSS, the
		 * double fault handler cannot use an alternate stack (64-bit).
		 */
		set_gatesegd(&bop_idt[t], &bop_trap_handler, B64CODE_SEL,
		    SDT_SYSIGT, TRP_KPL, 0);
	}
	bop_idt_info.dtr_limit = (NIDT * sizeof (gate_desc_t)) - 1;
	bop_idt_info.dtr_base = (uintptr_t)bop_idt;
	wr_idtr(&bop_idt_info);
}

static boolean_t
memlists_overlap(const memlist_t *ap, const memlist_t *bp)
{
	if (ap->ml_address >= bp->ml_address &&
	    ap->ml_address <= bp->ml_address + bp->ml_size)
		return (B_TRUE);

	if (bp->ml_address >= ap->ml_address &&
	    bp->ml_address <= ap->ml_address + ap->ml_size)
		return (B_TRUE);

	return (B_FALSE);
}

static boolean_t
memlist_coalesce_pair(memlist_t *ap, const memlist_t *bp)
{
	uint64_t end;

	if (!memlists_overlap(ap, bp))
		return (B_FALSE);

	end = MAX(ap->ml_address + ap->ml_size, bp->ml_address + bp->ml_size);
	ap->ml_address = MIN(ap->ml_address, bp->ml_address);
	ap->ml_size = end - ap->ml_address;

	return (B_TRUE);
}

static void
apob_build_memlists(const memlist_t **extra_holes, uint_t extra_hole_count,
    memlist_t **rsvd_out, memlist_t **ram_out)
{
	size_t sysmap_len = 0;
	int err = 0;
	int i, j;
	uint32_t holes_assigned = 0;
	const milan_apob_sysmap_t *smp = 
	    milan_apob_find(MILAN_APOB_GROUP_FABRIC, 9, 0, &sysmap_len, &err);
	const uint_t MAX_APOB_HOLES = ARRAY_SIZE(smp->masm_holes);
	memlist_t *ram_chunks;
	memlist_t *rsvd_holes;
	uint32_t apob_hole_count;
	uint64_t max_paddr, last_phys, end;
	uint64_t usable_pages;
	/*
	 * If the APOB is toast, assume 2 GiB of DRAM with only known holes.
	 */
	apob_hole_count = 0;
	max_paddr = 0x80000000UL;

	if (err != 0) {
		bop_printf(NULL, "couldn't find APOB system memory map "
		    "(errno = %d); using bootstrap RAM only\n", err);
	} else if (sysmap_len < sizeof (*smp)) {
		bop_printf(NULL, "APOB system memory map too small "
		    "(0x%lx < 0x%lx bytes); using bootstrap RAM only\n",
		    sysmap_len, sizeof (*smp));
	} else if (smp->masm_hole_count > MAX_APOB_HOLES) {
		bop_printf(NULL, "APOB system memory map has too many holes "
		    "(0x%x > 0x%x allowed); using bootstrap RAM only\n",
		    smp->masm_hole_count, MAX_APOB_HOLES);
	} else {
		apob_hole_count = smp->masm_hole_count;
		max_paddr = P2ALIGN(smp->masm_high_phys, MMU_PAGESIZE);
	}

	DBG(apob_hole_count);
	DBG(max_paddr);

	rsvd_holes = (memlist_t *)do_bsys_alloc(NULL, NULL,
	    (apob_hole_count + extra_hole_count) * sizeof (memlist_t), 0);

	for (i = 0; i < apob_hole_count; i++) {
		DBG_MSG("APOB: RAM hole @ %lx size %lx\n",
		    smp->masm_holes[i].masmrh_base,
		    smp->masm_holes[i].masmrh_size);
		end = P2ROUNDUP(smp->masm_holes[i].masmrh_base +
		    smp->masm_holes[i].masmrh_size, MMU_PAGESIZE);
		rsvd_holes[i].ml_address =
		    P2ALIGN(smp->masm_holes[i].masmrh_base, MMU_PAGESIZE);
		rsvd_holes[i].ml_size = end - rsvd_holes[i].ml_address;
		rsvd_holes[i].ml_next = rsvd_holes[i].ml_prev = NULL;
	}

	for (i = 0; i < extra_hole_count; i++) {
		DBG_MSG("BOOT: RAM hole @ %lx size %lx\n",
		    extra_holes[i]->ml_address, extra_holes[i]->ml_size);
		rsvd_holes[apob_hole_count + i] = *extra_holes[i];
		rsvd_holes[apob_hole_count + i].ml_next =
		    rsvd_holes[apob_hole_count + i].ml_prev = NULL;
	}

	/*
	 * No qsort() yet; it's in genunix.
	 */
	for (i = 1; i < apob_hole_count + extra_hole_count; i++) {
		for (j = i; j > 0; j--) {
			if (rsvd_holes[j - 1].ml_address >
			    rsvd_holes[j].ml_address) {
				const memlist_t t = rsvd_holes[j];
				rsvd_holes[j] = rsvd_holes[j - 1];
				rsvd_holes[j - 1] = t;
			} else {
				break;
			}
		}
	}

	holes_assigned = apob_hole_count + extra_hole_count;

	/*
	 * The holes are now sorted by starting address.  Coalesce overlapping
	 * or adjacent pairs of holes, limiting any that extend beyond
	 * max_paddr.  This gives us our final list of holes.
	 */
	for (i = 0; i < holes_assigned - 1;) {
		if (memlist_coalesce_pair(&rsvd_holes[i], &rsvd_holes[i + 1])) {
			for (j = i + 1; j < holes_assigned; j++)
				rsvd_holes[j] = rsvd_holes[j + 1];
			--holes_assigned;
		} else {
			++i;
		}
	}

	while (holes_assigned > 0 &&
	    rsvd_holes[holes_assigned - 1].ml_address >= max_paddr)
		--holes_assigned;

	if (holes_assigned > 0 &&
	    rsvd_holes[holes_assigned - 1].ml_address +
	    rsvd_holes[i].ml_size >= max_paddr) {
		max_paddr = rsvd_holes[holes_assigned - 1].ml_address;
		--holes_assigned;
	}

	/*
	 * Now it's all over but the shouting; we know the maximum address and
	 * all the holes, so fill in the RAM we can use.
	 */
	ram_chunks = (memlist_t *)do_bsys_alloc(NULL, NULL,
	    (holes_assigned + 1) * sizeof (memlist_t), 0);

	last_phys = usable_pages = 0;
	for (i = 0; i < holes_assigned; i++) {
		ram_chunks[i].ml_address = last_phys;
		ram_chunks[i].ml_size = rsvd_holes[i].ml_address - last_phys;
		last_phys = rsvd_holes[i].ml_address + rsvd_holes[i].ml_size;
		usable_pages += (ram_chunks[i].ml_size >> MMU_PAGESHIFT);
	}
	ram_chunks[i].ml_address = last_phys;
	ram_chunks[i].ml_size = max_paddr - last_phys;
	usable_pages += (ram_chunks[i].ml_size >> MMU_PAGESHIFT);

	if (kbm_debug) {
		bop_printf(NULL, "Coalesced RAM holes/reservations:\n");
		for (i = 0; i < holes_assigned; i++) {
			bop_printf(NULL, "\t[%lx, %lx] (%lx bytes)\n",
			    rsvd_holes[i].ml_address,
			    rsvd_holes[i].ml_address +
			    rsvd_holes[i].ml_size - 1,
			    rsvd_holes[i].ml_size);
		}
		bop_printf(NULL, "Usable RAM regions:\n");
		for (i = 0; i < holes_assigned + 1; i++) {
			bop_printf(NULL, "\t[%lx, %lx] (%lx bytes)\n",
			    ram_chunks[i].ml_address,
			    ram_chunks[i].ml_address +
			    ram_chunks[i].ml_size - 1,
			    ram_chunks[i].ml_size);
		}
	}

	DBG(usable_pages);

	/*
	 * Finally, assign pointers to link these into lists, for historical
	 * reasons.
	 */
	if (holes_assigned > 0) {
		rsvd_holes[0].ml_prev = NULL;
		rsvd_holes[0].ml_next = NULL;
		rsvd_holes[holes_assigned - 1].ml_next = NULL;
	}
	for (i = 0; i < holes_assigned - 1; i++) {
		rsvd_holes[i].ml_next = &rsvd_holes[i + 1];
		rsvd_holes[i + 1].ml_prev = &rsvd_holes[i];
	}

	ram_chunks[0].ml_prev = NULL;
	ram_chunks[0].ml_next = NULL;
	ram_chunks[holes_assigned].ml_next = NULL;
	for (i = 0; i < holes_assigned; i++) {
		ram_chunks[i].ml_next = &ram_chunks[i + 1];
		ram_chunks[i + 1].ml_prev = &ram_chunks[i];
	}

	if (rsvd_out != NULL)
		*rsvd_out = rsvd_holes;
	if (ram_out != NULL)
		*ram_out = ram_chunks;
}

static void
get_ramdisk_hole(memlist_t *mlp)
{
	uint64_t start, end;

	if (do_bsys_getproplen(NULL, "ramdisk_start") != sizeof (uint64_t) ||
	    do_bsys_getprop(NULL, "ramdisk_start", &start) != 0 ||
	    do_bsys_getproplen(NULL, "ramdisk_end") != sizeof (uint64_t) ||
	    do_bsys_getprop(NULL, "ramdisk_end", &end) != 0) {
		mlp->ml_address = 0;
		mlp->ml_size = 0;

		return;
	}

	end = P2ROUNDUP(end, MMU_PAGESIZE);
	mlp->ml_address = P2ALIGN(start, MMU_PAGESIZE);
	mlp->ml_size = end - mlp->ml_address;
}

static void
build_memlists(void)
{
	/*
	 * Extra hole memlists for use when building our own more conservative
	 * lists for use by the earlyboot allocator.  These regions aren't
	 * needed once we're done with boot (or are protected by other logic),
	 * but we must tell our own allocator -- which doesn't otherwise know
	 * any better -- not to hand them out.
	 */
	const memlist_t stack_hole = {
		.ml_address = bsml_usable.ml_address + bsml_usable.ml_size,
		.ml_size = 0x8000000UL -
		    (bsml_usable.ml_address + bsml_usable.ml_size),
		.ml_next = NULL,
		.ml_prev = NULL
	};

	memlist_t ramdisk_hole;
	get_ramdisk_hole(&ramdisk_hole);

	/*
	 * This is always forbidden to touch.
	 */
	const memlist_t mystery_hole = {
		.ml_address = 0xfd00000000UL,
		.ml_size = 0x300000000UL,
		.ml_next = NULL,
		.ml_prev = NULL
	};

	const memlist_t *eballoc_extra_holes[] = {
		&bsml_apob,
		&stack_hole,
		&ramdisk_hole,
		&mystery_hole
	};

	const memlist_t *kernel_extra_holes[] = {
		&mystery_hole
	};

	apob_build_memlists(eballoc_extra_holes,
	    ARRAY_SIZE(eballoc_extra_holes), NULL, &eb_ram);
	apob_build_memlists(kernel_extra_holes, ARRAY_SIZE(kernel_extra_holes),
	    &bm.rsvdmem, &bm.physinstalled);
	bm.pcimem = NULL;	/* Used only if bus 0 resources not set */
}

static void
apob_init(void)
{
	const bt_prop_t *apob_prop = find_bt_prop(BTPROP_NAME_APOB_ADDRESS, 0);

	if (apob_prop == NULL) {
		bop_panic("APOB address property %s is missing; don't "
		    "know how to probe memory ourselves",
		    BTPROP_NAME_APOB_ADDRESS);
	}

	if ((apob_prop->btp_typeflags & DDI_PROP_TYPE_MASK) !=
	    DDI_PROP_TYPE_INT64) {
		bop_panic("Boot-time property %s has incorrect type; can't "
		    "find the APOB without it", BTPROP_NAME_APOB_ADDRESS);
	}

	milan_apob_init((paddr_t)(*(uint64_t *)apob_prop->btp_value),
	    &bsml_apob);

	bsml_apob.ml_next = bsml_apob.ml_prev = NULL;
}

static void
bootmem_init(void)
{
	/*
	 * We assume in the absence of evidence to the contrary that the entire
	 * portion of the physical address space up to 0x8000_0000 contains
	 * RAM.  The bootloader, including the pagetables we're currently
	 * using, is located within that range so that it can be built as a
	 * small code model static executable.  To leave ourselves as much
	 * space as possible, it locates itself at the top of that range; below
	 * it is the stack on which we are currently executing.  Therefore we
	 * are free to use all the physical address space below our stack for
	 * bootstrap memory.  The bootstrap allocator will allocate these pages
	 * from the bottom while our stack grows downward.  We need to exclude
	 * three regions so the allocator doesn't use them, because at this
	 * moment physical memory looks like this:
	 *
	 * +------------------------+ 0x7fff_ffff
	 * | loader pages including |
	 * | our active pagetables  |
	 * +------------------------+ %cr3
	 * | loader pages and stack |
	 * +------------------------+ %rsp
	 * | more stack space we're |
	 * |  sure to need shortly  |
	 * +------------------------+ %rsp - 8 pages, rounded down
	 * |   and more free pages  |
	 * +------------------------+ somewhere + something
	 * |          APOB          |
	 * +------------------------+ somewhere
	 * |  delicious free pages  |
	 * +------------------------+ 0x0600_0000
	 * |   kernel nucleus data  |
	 * +------------------------+ 0x0400_0000
	 * |   kernel nucleus text  |
	 * +------------------------+ 0x0200_0000
	 * |  free pages we want to |
	 * |  save for special uses |
	 * +------------------------+ 0
	 *
	 * We'll bootstrap the allocator by setting up a single region from
	 * the top of the kernel nucleus up to the bottom of our current stack
	 * less a healthy margin for further growth.
	 *
	 * The last bit to consider is the APOB.  For now we will ignore it;
	 * we're about to go process its contents, at which point we will also
	 * reserve its own memory for the remainder of the boot process.  If
	 * its contents seem bogus, we'll ignore them but keep them around
	 * for inspection; see build_memlists().
	 *
	 * Note that this assumes the stack we are on is identity-mapped, which
	 * the loader currently guarantees.  Should we choose to give ourselves
	 * some other stack, this needs revisiting.
	 *
	 * Until we've found how how much RAM we really have and where it is,
	 * we'll tell anyone who asks that this bootstrap region is all we
	 * have; right now that's no one.
	 */

	uint64_t rsp;

	bsml_usable.ml_address = 0x600000UL;	/* sync with Mapfile.amd64 */
	__asm__ __volatile__("movq %%rsp, %0" : "=r" (rsp) : :);
	rsp = P2ALIGN(rsp - 8 * MMU_PAGESIZE, MMU_PAGESIZE);
	bsml_usable.ml_size = rsp - bsml_usable.ml_address;
	bsml_usable.ml_next = NULL;
	bsml_usable.ml_prev = NULL;

	eb_ram = &bsml_usable;
	bm.physinstalled = &bsml_usable;
}

/*
 * This is where we enter the kernel. It dummies up the boot_ops and
 * boot_syscalls vectors and jumps off to _kobj_boot().  How does the loader
 * find this entry point?  By the miracle of looking at the ELF e_entry field.
 */
void
_start(const bt_discovery_t *btdp)
{
	extern void _kobj_boot();

	/*
	 * XXX This works only on *non* Oxide hardware and should be deleted.
	 */
	outw(0x80, 0x1DE);

	if (btdp == NULL) {
#ifdef	USE_DISCOVERY_STUB
		btdp = &bt_discovery_stub;
#else
		outw(0x80, 0xD15C);
		return;
#endif	/* USE_DISCOVERY_STUB */
	}

	bt_props = btdp->btd_prop_list;
	bcons_init();

	kbm_debug = (find_bt_prop("kbm_debug", 0) != NULL);

	if (find_bt_prop("bootrd_debug", 0) != NULL)
		bootrd_debug = 1;

	DBG_MSG("\n\n*** Entered illumos in _start()\n");
	DBG(btdp);
	DBG(btdp->btd_prop_list);

	DBG_MSG("Initializing boot time memory management...");
	kbm_init(&bm);
	DBG_MSG("done\n");

	/*
	 * After we return here, it is possible to use the boot-time memory
	 * allocation routines (in particular, do_bop_phys_alloc()) which makes
	 * setting up the real memory lists much more pleasant.
	 */
	bootmem_init();

	/*
	 * Fill in the bootops vector; all of this can now work.
	 */
	bootop.bsys_version = BO_VERSION;
	bootop.boot_mem = &bm;
	bootop.bsys_alloc = do_bsys_alloc;
	bootop.bsys_free = do_bsys_free;
	bootop.bsys_getproplen = do_bsys_getproplen;
	bootop.bsys_getprop = do_bsys_getprop;
	bootop.bsys_nextprop = do_bsys_nextprop;
	bootop.bsys_printf = bop_printf;

	/*
	 * BOP_EALLOC() is no longer needed
	 */
	bootop.bsys_ealloc = do_bsys_ealloc;

	/*
	 * Install an IDT to catch early pagefaults (shouldn't have any).
	 * Also needed for kmdb.
	 */
	DBG_MSG("Initializing temporary IDT: ");
	bop_idt_init();
	DBG_MSG("done\n");

	if (find_bt_prop("prom_debug", 0) != NULL || kbm_debug) {
		char *bufpage;

		bufpage = do_bsys_alloc(NULL, NULL, MMU_PAGESIZE, MMU_PAGESIZE);
		boot_prop_display(bufpage);
	}

	/*
	 * Initialize the APOB boot operations. This will be required for us to
	 * successfully use it as a boot operation vector.
	 */
	apob_init();

	/*
	 * Find usable memory regions, trusting the contents of the APOB if
	 * available.  If successful, this will let us use the entirety of
	 * DRAM during the rest of boot; we should still (barely) be able to
	 * boot without it.
	 */
	build_memlists();

	/*
	 * _kobj_boot() vectors us to mlsetup and thence to main(), so there
	 * is no return from this point.
	 */
	_kobj_boot(&bop_sysp, NULL, &bootop, NULL);
}

/*ARGSUSED*/
static caddr_t
no_more_alloc(bootops_t *bop, caddr_t virthint, size_t size, int align)
{
	panic("Attempt to bsys_alloc() too late\n");
	return (NULL);
}

/*ARGSUSED*/
static void
no_more_free(bootops_t *bop, caddr_t virt, size_t size)
{
	panic("Attempt to bsys_free() too late\n");
}

void
bop_no_more_mem(void)
{
	DBG(total_bop_alloc_scratch);
	DBG(total_bop_alloc_kernel);
	bootops->bsys_alloc = no_more_alloc;
	bootops->bsys_free = no_more_free;
}

/* XXX shareable */
/*ARGSUSED*/
int
boot_compinfo(int fd, struct compinfo *cbp)
{
	cbp->iscmp = 0;
	cbp->blksize = MAXBSIZE;
	return (0);
}

/* XXX shareable */
int
bootprop_getval(const char *prop_name, u_longlong_t *prop_value)
{
	int		boot_prop_len;
	char		str[BP_MAX_STRLEN];
	u_longlong_t	value;

	boot_prop_len = BOP_GETPROPLEN(bootops, prop_name);
	if (boot_prop_len < 0 || boot_prop_len >= sizeof (str) ||
	    BOP_GETPROP(bootops, prop_name, str) < 0 ||
	    kobj_getvalue(str, &value) == -1)
		return (-1);

	if (prop_value)
		*prop_value = value;

	return (0);
}

/* XXX shareable */
int
bootprop_getstr(const char *prop_name, char *buf, size_t buflen)
{
	int boot_prop_len = BOP_GETPROPLEN(bootops, prop_name);

	if (boot_prop_len < 0 || boot_prop_len >= buflen ||
	    BOP_GETPROP(bootops, prop_name, buf) < 0)
		return (-1);

	return (0);
}
