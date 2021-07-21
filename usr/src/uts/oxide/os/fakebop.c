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
 * Copyright 2020 Joyent, Inc.
 */

/*
 * This file contains the functionality that mimics the boot operations
 * on SPARC systems or the old boot.bin/multiboot programs on x86 systems.
 * The x86 kernel now does everything on its own.
 */

#include <sys/types.h>
#include <sys/bootconf.h>
#include <sys/bootsvcs.h>
#include <sys/bootinfo.h>
#include <sys/multiboot.h>
#include <sys/multiboot2.h>
#include <sys/multiboot2_impl.h>
#include <sys/bootvfs.h>
#include <sys/bootprops.h>
#include <sys/varargs.h>
#include <sys/param.h>
#include <sys/machparam.h>
#include <sys/machsystm.h>
#include <sys/archsystm.h>
#include <sys/boot_console.h>
#include <sys/framebuffer.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/promif.h>
#include <sys/archsystm.h>
#include <sys/x86_archext.h>
#include <sys/kobj.h>
#include <sys/privregs.h>
#include <sys/sysmacros.h>
#include <sys/ctype.h>
#include <sys/fastboot.h>
#include <vm/kboot_mmu.h>
#include <vm/hat_pte.h>
#include <sys/kobj.h>
#include <sys/kobj_lex.h>
#include <sys/pci_cfgspace_impl.h>
#include <sys/fastboot_impl.h>
#include <sys/ddipropdefs.h>	/* For DDI prop types */

#include <sys/dw_apb_uart.h>

static int have_console = 0;	/* set once primitive console is initialized */
static char *boot_args = "";

/*
 * Debugging macros
 */
static uint_t kbm_debug = 0;
#define	DBG_MSG(s)	{ if (kbm_debug) bop_printf(NULL, "%s", s); }
#define	DBG(x)		{ if (kbm_debug)			\
	bop_printf(NULL, "%s is %" PRIx64 "\n", #x, (uint64_t)(x));	\
	}

#define	PUT_STRING(s) {				\
	char *cp;				\
	for (cp = (s); *cp; ++cp)		\
		bcons_putchar(*cp);		\
	}

bootops_t bootop;	/* simple bootops we'll pass on to kernel */
struct bsys_mem bm;

/*
 * Boot info from "glue" code in low memory. xbootp is used by:
 *	do_bop_phys_alloc(), do_bsys_alloc() and read_bootenvrc().
 */
static struct xboot_info *xbootp;
static uintptr_t next_virt;	/* next available virtual address */
static paddr_t next_phys;	/* next available physical address from dboot */
static paddr_t high_phys = -(paddr_t)1;	/* last used physical address */

/*
 * buffer for vsnprintf for console I/O
 */
#define	BUFFERSIZE	512
static char buffer[BUFFERSIZE];

/*
 * stuff to store/report/manipulate boot property settings.
 */
typedef struct bootprop {
	struct bootprop *bp_next;
	char *bp_name;
	int bp_flags;			/* DDI prop type */
	uint_t bp_vlen;			/* 0 for boolean */
	char *bp_value;
} bootprop_t;

static bootprop_t *bprops = NULL;
static char *curr_page = NULL;		/* ptr to avail bprop memory */
static int curr_space = 0;		/* amount of memory at curr_page */

/*
 * some allocator statistics
 */
static ulong_t total_bop_alloc_scratch = 0;
static ulong_t total_bop_alloc_kernel = 0;

static int early_allocation = 1;

int force_fastreboot = 0;
volatile int fastreboot_onpanic = 0;
int post_fastreboot = 0;
volatile int fastreboot_capable = 1;

/*
 * Information saved from current boot for fast reboot.
 * If the information size exceeds what we have allocated, fast reboot
 * will not be supported.
 */
multiboot_info_t saved_mbi;
mb_memory_map_t saved_mmap[FASTBOOT_SAVED_MMAP_COUNT];
uint8_t saved_drives[FASTBOOT_SAVED_DRIVES_SIZE];
char saved_cmdline[FASTBOOT_SAVED_CMDLINE_LEN];
int saved_cmdline_len = 0;
size_t saved_file_size[FASTBOOT_MAX_FILES_MAP];

/*
 * Turn off fastreboot_onpanic to avoid panic loop.
 */
char fastreboot_onpanic_cmdline[FASTBOOT_SAVED_CMDLINE_LEN];
static const char fastreboot_onpanic_args[] = " -B fastreboot_onpanic=0";

/*
 * Allocate aligned physical memory at boot time. This allocator allocates
 * from the highest possible addresses. This avoids exhausting memory that
 * would be useful for DMA buffers.
 */
paddr_t
do_bop_phys_alloc(uint64_t size, uint64_t align)
{
	paddr_t	pa = 0;
	paddr_t	start;
	paddr_t	end;
	struct memlist	*ml = (struct memlist *)xbootp->bi_phys_install;

	/*
	 * Be careful if high memory usage is limited in startup.c
	 * Since there are holes in the low part of the physical address
	 * space we can treat physmem as a pfn (not just a pgcnt) and
	 * get a conservative upper limit.
	 */
	if (physmem != 0 && high_phys > pfn_to_pa(physmem))
		high_phys = pfn_to_pa(physmem);

	/*
	 * find the highest available memory in physinstalled
	 */
	size = P2ROUNDUP(size, align);
	for (; ml; ml = ml->ml_next) {
		start = P2ROUNDUP(ml->ml_address, align);
		end = P2ALIGN(ml->ml_address + ml->ml_size, align);
		if (start < next_phys)
			start = P2ROUNDUP(next_phys, align);
		if (end > high_phys)
			end = P2ALIGN(high_phys, align);

		if (end <= start)
			continue;
		if (end - start < size)
			continue;

		/*
		 * Early allocations need to use low memory, since
		 * physmem might be further limited by bootenv.rc
		 */
		if (early_allocation) {
			if (pa == 0 || start < pa)
				pa = start;
		} else {
			if (end - size > pa)
				pa = end - size;
		}
	}
	if (pa != 0) {
		if (early_allocation)
			next_phys = pa + size;
		else
			high_phys = pa;
		return (pa);
	}
	bop_panic("do_bop_phys_alloc(0x%" PRIx64 ", 0x%" PRIx64
	    ") Out of memory\n", size, align);
	/*NOTREACHED*/
}

uintptr_t
alloc_vaddr(size_t size, paddr_t align)
{
	uintptr_t rv;

	next_virt = P2ROUNDUP(next_virt, (uintptr_t)align);
	rv = (uintptr_t)next_virt;
	next_virt += size;
	return (rv);
}

/*
 * Allocate virtual memory. The size is always rounded up to a multiple
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
	uint_t is_kernel = (virthint != 0);

	if (a < MMU_PAGESIZE)
		a = MMU_PAGESIZE;
	else if (!ISP2(a))
		prom_panic("do_bsys_alloc() incorrect alignment");
	size = P2ROUNDUP(size, MMU_PAGESIZE);

	/*
	 * Use the next aligned virtual address if we weren't given one.
	 */
	if (virthint == NULL) {
		virthint = (caddr_t)alloc_vaddr(size, a);
		total_bop_alloc_scratch += size;
	} else {
		total_bop_alloc_kernel += size;
	}

	/*
	 * allocate the physical memory
	 */
	pa = do_bop_phys_alloc(size, a);

	/*
	 * Add the mappings to the page tables, try large pages first.
	 */
	va = (uintptr_t)virthint;
	s = size;
	level = 1;
	pgsize = xbootp->bi_use_pae ? TWO_MEG : FOUR_MEG;
	if (xbootp->bi_use_largepage && a == pgsize) {
		while (IS_P2ALIGNED(pa, pgsize) && IS_P2ALIGNED(va, pgsize) &&
		    s >= pgsize) {
			kbm_map(va, pa, level, is_kernel);
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
		kbm_map(va, pa, level, is_kernel);
		va += pgsize;
		pa += pgsize;
		s -= pgsize;
	}
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


static void
bsetprop(int flags, char *name, int nlen, void *value, int vlen)
{
	uint_t size;
	uint_t need_size;
	bootprop_t *b;

	/*
	 * align the size to 16 byte boundary
	 */
	size = sizeof (bootprop_t) + nlen + 1 + vlen;
	size = (size + 0xf) & ~0xf;
	if (size > curr_space) {
		need_size = (size + (MMU_PAGEOFFSET)) & MMU_PAGEMASK;
		curr_page = do_bsys_alloc(NULL, 0, need_size, MMU_PAGESIZE);
		curr_space = need_size;
	}

	/*
	 * use a bootprop_t at curr_page and link into list
	 */
	b = (bootprop_t *)curr_page;
	curr_page += sizeof (bootprop_t);
	curr_space -=  sizeof (bootprop_t);
	b->bp_next = bprops;
	bprops = b;

	/*
	 * follow by name and ending zero byte
	 */
	b->bp_name = curr_page;
	bcopy(name, curr_page, nlen);
	curr_page += nlen;
	*curr_page++ = 0;
	curr_space -= nlen + 1;

	/*
	 * set the property type
	 */
	b->bp_flags = flags & DDI_PROP_TYPE_MASK;

	/*
	 * copy in value, but no ending zero byte
	 */
	b->bp_value = curr_page;
	b->bp_vlen = vlen;
	if (vlen > 0) {
		bcopy(value, curr_page, vlen);
		curr_page += vlen;
		curr_space -= vlen;
	}

	/*
	 * align new values of curr_page, curr_space
	 */
	while (curr_space & 0xf) {
		++curr_page;
		--curr_space;
	}
}

static void
bsetprops(char *name, char *value)
{
	bsetprop(DDI_PROP_TYPE_STRING, name, strlen(name),
	    value, strlen(value) + 1);
}

static void
bsetprop32(char *name, uint32_t value)
{
	bsetprop(DDI_PROP_TYPE_INT, name, strlen(name),
	    (void *)&value, sizeof (value));
}

static void
bsetprop64(char *name, uint64_t value)
{
	bsetprop(DDI_PROP_TYPE_INT64, name, strlen(name),
	    (void *)&value, sizeof (value));
}

static void
bsetpropsi(char *name, int value)
{
	char prop_val[32];

	(void) snprintf(prop_val, sizeof (prop_val), "%d", value);
	bsetprops(name, prop_val);
}

/*
 * to find the type of the value associated with this name
 */
/*ARGSUSED*/
int
do_bsys_getproptype(bootops_t *bop, const char *name)
{
	bootprop_t *b;

	for (b = bprops; b != NULL; b = b->bp_next) {
		if (strcmp(name, b->bp_name) != 0)
			continue;
		return (b->bp_flags);
	}
	return (-1);
}

/*
 * to find the size of the buffer to allocate
 */
/*ARGSUSED*/
int
do_bsys_getproplen(bootops_t *bop, const char *name)
{
	bootprop_t *b;

	for (b = bprops; b; b = b->bp_next) {
		if (strcmp(name, b->bp_name) != 0)
			continue;
		return (b->bp_vlen);
	}
	return (-1);
}

/*
 * get the value associated with this name
 */
/*ARGSUSED*/
int
do_bsys_getprop(bootops_t *bop, const char *name, void *value)
{
	bootprop_t *b;

	for (b = bprops; b; b = b->bp_next) {
		if (strcmp(name, b->bp_name) != 0)
			continue;
		bcopy(b->bp_value, value, b->bp_vlen);
		return (0);
	}
	return (-1);
}

/*
 * get the name of the next property in succession from the standalone
 */
/*ARGSUSED*/
static char *
do_bsys_nextprop(bootops_t *bop, char *name)
{
	bootprop_t *b;

	/*
	 * A null name is a special signal for the 1st boot property
	 */
	if (name == NULL || strlen(name) == 0) {
		if (bprops == NULL)
			return (NULL);
		return (bprops->bp_name);
	}

	for (b = bprops; b; b = b->bp_next) {
		if (name != b->bp_name)
			continue;
		b = b->bp_next;
		if (b == NULL)
			return (NULL);
		return (b->bp_name);
	}
	return (NULL);
}

/*
 * Parse numeric value from a string. Understands decimal, hex, octal, - and ~
 */
static int
parse_value(char *p, uint64_t *retval)
{
	int adjust = 0;
	uint64_t tmp = 0;
	int digit;
	int radix = 10;

	*retval = 0;
	if (*p == '-' || *p == '~')
		adjust = *p++;

	if (*p == '0') {
		++p;
		if (*p == 0)
			return (0);
		if (*p == 'x' || *p == 'X') {
			radix = 16;
			++p;
		} else {
			radix = 8;
			++p;
		}
	}
	while (*p) {
		if ('0' <= *p && *p <= '9')
			digit = *p - '0';
		else if ('a' <= *p && *p <= 'f')
			digit = 10 + *p - 'a';
		else if ('A' <= *p && *p <= 'F')
			digit = 10 + *p - 'A';
		else
			return (-1);
		if (digit >= radix)
			return (-1);
		tmp = tmp * radix + digit;
		++p;
	}
	if (adjust == '-')
		tmp = -tmp;
	else if (adjust == '~')
		tmp = ~tmp;
	*retval = tmp;
	return (0);
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
 * 2nd part of building the table of boot properties. This includes:
 * - values from /boot/solaris/bootenv.rc (ie. eeprom(1m) values)
 *
 * lines look like one of:
 * ^$
 * ^# comment till end of line
 * setprop name 'value'
 * setprop name value
 * setprop name "value"
 *
 * we do single character I/O since this is really just looking at memory
 */
void
read_bootenvrc(void)
{
	int fd;
	char *line;
	int c;
	int bytes_read;
	char *name;
	int n_len;
	char *value;
	int v_len;
	char *inputdev;	/* these override the command line if serial ports */
	char *outputdev;
	char *consoledev;
	uint64_t lvalue;
	int use_xencons = 0;
	extern int bootrd_debug;

#ifdef __xpv
	if (!DOMAIN_IS_INITDOMAIN(xen_info))
		use_xencons = 1;
#endif /* __xpv */

	DBG_MSG("Opening /boot/solaris/bootenv.rc\n");
	fd = BRD_OPEN(bfs_ops, "/boot/solaris/bootenv.rc", 0);
	DBG(fd);

	line = do_bsys_alloc(NULL, NULL, MMU_PAGESIZE, MMU_PAGESIZE);
	while (fd >= 0) {

		/*
		 * get a line
		 */
		for (c = 0; ; ++c) {
			bytes_read = BRD_READ(bfs_ops, fd, line + c, 1);
			if (bytes_read == 0) {
				if (c == 0)
					goto done;
				break;
			}
			if (line[c] == '\n')
				break;
		}
		line[c] = 0;

		/*
		 * ignore comment lines
		 */
		c = 0;
		while (ISSPACE(line[c]))
			++c;
		if (line[c] == '#' || line[c] == 0)
			continue;

		/*
		 * must have "setprop " or "setprop\t"
		 */
		if (strncmp(line + c, "setprop ", 8) != 0 &&
		    strncmp(line + c, "setprop\t", 8) != 0)
			continue;
		c += 8;
		while (ISSPACE(line[c]))
			++c;
		if (line[c] == 0)
			continue;

		/*
		 * gather up the property name
		 */
		name = line + c;
		n_len = 0;
		while (line[c] && !ISSPACE(line[c]))
			++n_len, ++c;

		/*
		 * gather up the value, if any
		 */
		value = "";
		v_len = 0;
		while (ISSPACE(line[c]))
			++c;
		if (line[c] != 0) {
			value = line + c;
			while (line[c] && !ISSPACE(line[c]))
				++v_len, ++c;
		}

		if (v_len >= 2 && value[0] == value[v_len - 1] &&
		    (value[0] == '\'' || value[0] == '"')) {
			++value;
			v_len -= 2;
		}
		name[n_len] = 0;
		if (v_len > 0)
			value[v_len] = 0;
		else
			continue;

		/*
		 * ignore "boot-file" property, it's now meaningless
		 */
		if (strcmp(name, "boot-file") == 0)
			continue;
		if (strcmp(name, "boot-args") == 0 &&
		    strlen(boot_args) > 0)
			continue;

		/*
		 * If a property was explicitly set on the command line
		 * it will override a setting in bootenv.rc. We make an
		 * exception for a property from the bootloader such as:
		 *
		 * console="text,ttya,ttyb,ttyc,ttyd"
		 *
		 * In such a case, picking the first value here (as
		 * lookup_console_devices() does) is at best a guess; if
		 * bootenv.rc has a value, it's probably better.
		 */
		if (strcmp(name, "console") == 0) {
			char propval[BP_MAX_STRLEN] = "";

			if (do_bsys_getprop(NULL, name, propval) == -1 ||
			    strchr(propval, ',') != NULL)
				bsetprops(name, value);
			continue;
		}

		if (do_bsys_getproplen(NULL, name) == -1)
			bsetprops(name, value);
	}
done:
	if (fd >= 0)
		(void) BRD_CLOSE(bfs_ops, fd);


	/*
	 * Check if we have to limit the boot time allocator
	 */
	if (do_bsys_getproplen(NULL, "physmem") != -1 &&
	    do_bsys_getprop(NULL, "physmem", line) >= 0 &&
	    parse_value(line, &lvalue) != -1) {
		if (0 < lvalue && (lvalue < physmem || physmem == 0)) {
			physmem = (pgcnt_t)lvalue;
			DBG(physmem);
		}
	}
	early_allocation = 0;

	/*
	 * Check for bootrd_debug.
	 */
	if (find_boot_prop("bootrd_debug"))
		bootrd_debug = 1;

	/*
	 * check to see if we have to override the default value of the console
	 */
	if (!use_xencons) {
		inputdev = line;
		v_len = do_bsys_getproplen(NULL, "input-device");
		if (v_len > 0)
			(void) do_bsys_getprop(NULL, "input-device", inputdev);
		else
			v_len = 0;
		inputdev[v_len] = 0;

		outputdev = inputdev + v_len + 1;
		v_len = do_bsys_getproplen(NULL, "output-device");
		if (v_len > 0)
			(void) do_bsys_getprop(NULL, "output-device",
			    outputdev);
		else
			v_len = 0;
		outputdev[v_len] = 0;

		consoledev = outputdev + v_len + 1;
		v_len = do_bsys_getproplen(NULL, "console");
		if (v_len > 0) {
			(void) do_bsys_getprop(NULL, "console", consoledev);
			if (post_fastreboot &&
			    strcmp(consoledev, "graphics") == 0) {
				bsetprops("console", "text");
				v_len = strlen("text");
				bcopy("text", consoledev, v_len);
			}
		} else {
			v_len = 0;
		}
		consoledev[v_len] = 0;
		bcons_post_bootenvrc(inputdev, outputdev, consoledev);
	} else {
		/*
		 * Ensure console property exists
		 * If not create it as "hypervisor"
		 */
		v_len = do_bsys_getproplen(NULL, "console");
		if (v_len < 0)
			bsetprops("console", "hypervisor");
		inputdev = outputdev = consoledev = "hypervisor";
		bcons_post_bootenvrc(inputdev, outputdev, consoledev);
	}

	if (find_boot_prop("prom_debug") || kbm_debug)
		boot_prop_display(line);
}

/*
 * print formatted output
 */
/*ARGSUSED*/
void
vbop_printf(void *ptr, const char *fmt, va_list ap)
{
	if (have_console == 0)
		return;

	(void) vsnprintf(buffer, BUFFERSIZE, fmt, ap);
	PUT_STRING(buffer);
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
	bop_printf(NULL, fmt, ap);
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

static char *whoami;

#define	BUFLEN	64

static void
setup_rarp_props(struct sol_netinfo *sip)
{
	char buf[BUFLEN];	/* to hold ip/mac addrs */
	uint8_t *val;

	val = (uint8_t *)&sip->sn_ciaddr;
	(void) snprintf(buf, BUFLEN, "%d.%d.%d.%d",
	    val[0], val[1], val[2], val[3]);
	bsetprops(BP_HOST_IP, buf);

	val = (uint8_t *)&sip->sn_siaddr;
	(void) snprintf(buf, BUFLEN, "%d.%d.%d.%d",
	    val[0], val[1], val[2], val[3]);
	bsetprops(BP_SERVER_IP, buf);

	if (sip->sn_giaddr != 0) {
		val = (uint8_t *)&sip->sn_giaddr;
		(void) snprintf(buf, BUFLEN, "%d.%d.%d.%d",
		    val[0], val[1], val[2], val[3]);
		bsetprops(BP_ROUTER_IP, buf);
	}

	if (sip->sn_netmask != 0) {
		val = (uint8_t *)&sip->sn_netmask;
		(void) snprintf(buf, BUFLEN, "%d.%d.%d.%d",
		    val[0], val[1], val[2], val[3]);
		bsetprops(BP_SUBNET_MASK, buf);
	}

	if (sip->sn_mactype != 4 || sip->sn_maclen != 6) {
		bop_printf(NULL, "unsupported mac type %d, mac len %d\n",
		    sip->sn_mactype, sip->sn_maclen);
	} else {
		val = sip->sn_macaddr;
		(void) snprintf(buf, BUFLEN, "%x:%x:%x:%x:%x:%x",
		    val[0], val[1], val[2], val[3], val[4], val[5]);
		bsetprops(BP_BOOT_MAC, buf);
	}
}

static void
build_panic_cmdline(const char *cmd, int cmdlen)
{
	int proplen;
	size_t arglen;

	arglen = sizeof (fastreboot_onpanic_args);
	/*
	 * If we allready have fastreboot-onpanic set to zero,
	 * don't add them again.
	 */
	if ((proplen = do_bsys_getproplen(NULL, FASTREBOOT_ONPANIC)) > 0 &&
	    proplen <=  sizeof (fastreboot_onpanic_cmdline)) {
		(void) do_bsys_getprop(NULL, FASTREBOOT_ONPANIC,
		    fastreboot_onpanic_cmdline);
		if (FASTREBOOT_ONPANIC_NOTSET(fastreboot_onpanic_cmdline))
			arglen = 1;
	}

	/*
	 * construct fastreboot_onpanic_cmdline
	 */
	if (cmdlen + arglen > sizeof (fastreboot_onpanic_cmdline)) {
		DBG_MSG("Command line too long: clearing "
		    FASTREBOOT_ONPANIC "\n");
		fastreboot_onpanic = 0;
	} else {
		bcopy(cmd, fastreboot_onpanic_cmdline, cmdlen);
		if (arglen != 1)
			bcopy(fastreboot_onpanic_args,
			    fastreboot_onpanic_cmdline + cmdlen, arglen);
		else
			fastreboot_onpanic_cmdline[cmdlen] = 0;
	}
}


#ifndef	__xpv
/*
 * Construct boot command line for Fast Reboot. The saved_cmdline
 * is also reported by "eeprom bootcmd".
 */
static void
build_fastboot_cmdline(struct xboot_info *xbp)
{
	saved_cmdline_len =  strlen(xbp->bi_cmdline) + 1;
	if (saved_cmdline_len > FASTBOOT_SAVED_CMDLINE_LEN) {
		DBG(saved_cmdline_len);
		DBG_MSG("Command line too long: clearing fastreboot_capable\n");
		fastreboot_capable = 0;
	} else {
		bcopy((void *)(xbp->bi_cmdline), (void *)saved_cmdline,
		    saved_cmdline_len);
		saved_cmdline[saved_cmdline_len - 1] = '\0';
		build_panic_cmdline(saved_cmdline, saved_cmdline_len - 1);
	}
}

/*
 * Save memory layout, disk drive information, unix and boot archive sizes for
 * Fast Reboot.
 */
static void
save_boot_info(struct xboot_info *xbi)
{
	multiboot_info_t *mbi = xbi->bi_mb_info;
	struct boot_modules *modp;
	int i;

	bcopy(mbi, &saved_mbi, sizeof (multiboot_info_t));
	if (mbi->mmap_length > sizeof (saved_mmap)) {
		DBG_MSG("mbi->mmap_length too big: clearing "
		    "fastreboot_capable\n");
		fastreboot_capable = 0;
	} else {
		bcopy((void *)(uintptr_t)mbi->mmap_addr, (void *)saved_mmap,
		    mbi->mmap_length);
	}

	if ((mbi->flags & MB_INFO_DRIVE_INFO) != 0) {
		if (mbi->drives_length > sizeof (saved_drives)) {
			DBG(mbi->drives_length);
			DBG_MSG("mbi->drives_length too big: clearing "
			    "fastreboot_capable\n");
			fastreboot_capable = 0;
		} else {
			bcopy((void *)(uintptr_t)mbi->drives_addr,
			    (void *)saved_drives, mbi->drives_length);
		}
	} else {
		saved_mbi.drives_length = 0;
		saved_mbi.drives_addr = 0;
	}

	/*
	 * Current file sizes.  Used by fastboot.c to figure out how much
	 * memory to reserve for panic reboot.
	 * Use the module list from the dboot-constructed xboot_info
	 * instead of the list referenced by the multiboot structure
	 * because that structure may not be addressable now.
	 */
	saved_file_size[FASTBOOT_NAME_UNIX] = FOUR_MEG - PAGESIZE;
	for (i = 0, modp = (struct boot_modules *)(uintptr_t)xbi->bi_modules;
	    i < xbi->bi_module_cnt; i++, modp++) {
		saved_file_size[FASTBOOT_NAME_BOOTARCHIVE] += modp->bm_size;
	}
}
#endif	/* __xpv */

/*
 * Import boot environment module variables as properties, applying
 * blacklist filter for variables we know we will not use.
 *
 * Since the environment can be relatively large, containing many variables
 * used only for boot loader purposes, we will use a blacklist based filter.
 * To keep the blacklist from growing too large, we use prefix based filtering.
 * This is possible because in many cases, the loader variable names are
 * using a structured layout.
 *
 * We will not overwrite already set properties.
 *
 * Note that the menu items in particular can contain characters not
 * well-handled as bootparams, such as spaces, brackets, and the like, so that's
 * another reason.
 */
static struct bop_blacklist {
	const char *bl_name;
	int bl_name_len;
} bop_prop_blacklist[] = {
	{ "ISADIR", sizeof ("ISADIR") },
	{ "acpi", sizeof ("acpi") },
	{ "autoboot_delay", sizeof ("autoboot_delay") },
	{ "beansi_", sizeof ("beansi_") },
	{ "beastie", sizeof ("beastie") },
	{ "bemenu", sizeof ("bemenu") },
	{ "boot.", sizeof ("boot.") },
	{ "bootenv", sizeof ("bootenv") },
	{ "currdev", sizeof ("currdev") },
	{ "dhcp.", sizeof ("dhcp.") },
	{ "interpret", sizeof ("interpret") },
	{ "kernel", sizeof ("kernel") },
	{ "loaddev", sizeof ("loaddev") },
	{ "loader_", sizeof ("loader_") },
	{ "mainansi_", sizeof ("mainansi_") },
	{ "mainmenu_", sizeof ("mainmenu_") },
	{ "maintoggled_", sizeof ("maintoggled_") },
	{ "menu_timeout_command", sizeof ("menu_timeout_command") },
	{ "menuset_", sizeof ("menuset_") },
	{ "module_path", sizeof ("module_path") },
	{ "nfs.", sizeof ("nfs.") },
	{ "optionsansi_", sizeof ("optionsansi_") },
	{ "optionsmenu_", sizeof ("optionsmenu_") },
	{ "optionstoggled_", sizeof ("optionstoggled_") },
	{ "pcibios", sizeof ("pcibios") },
	{ "prompt", sizeof ("prompt") },
	{ "smbios", sizeof ("smbios") },
	{ "tem", sizeof ("tem") },
	{ "twiddle_divisor", sizeof ("twiddle_divisor") },
	{ "zfs_be", sizeof ("zfs_be") },
};

/*
 * Match the name against prefixes in above blacklist. If the match was
 * found, this name is blacklisted.
 */
static boolean_t
name_is_blacklisted(const char *name)
{
	int i, n;

	n = sizeof (bop_prop_blacklist) / sizeof (bop_prop_blacklist[0]);
	for (i = 0; i < n; i++) {
		if (strncmp(bop_prop_blacklist[i].bl_name, name,
		    bop_prop_blacklist[i].bl_name_len - 1) == 0) {
			return (B_TRUE);
		}
	}
	return (B_FALSE);
}

static void
process_boot_environment(struct boot_modules *benv)
{
	char *env, *ptr, *name, *value;
	uint32_t size, name_len, value_len;

	if (benv == NULL || benv->bm_type != BMT_ENV)
		return;
	ptr = env = benv->bm_addr;
	size = benv->bm_size;
	do {
		name = ptr;
		/* find '=' */
		while (*ptr != '=') {
			ptr++;
			if (ptr > env + size) /* Something is very wrong. */
				return;
		}
		name_len = ptr - name;
		if (sizeof (buffer) <= name_len)
			continue;

		(void) strncpy(buffer, name, sizeof (buffer));
		buffer[name_len] = '\0';
		name = buffer;

		value_len = 0;
		value = ++ptr;
		while ((uintptr_t)ptr - (uintptr_t)env < size) {
			if (*ptr == '\0') {
				ptr++;
				value_len = (uintptr_t)ptr - (uintptr_t)env;
				break;
			}
			ptr++;
		}

		/* Did we reach the end of the module? */
		if (value_len == 0)
			return;

		if (*value == '\0')
			continue;

		/* Is this property already set? */
		if (do_bsys_getproplen(NULL, name) >= 0)
			continue;

		/* Translate netboot variables */
		if (strcmp(name, "boot.netif.gateway") == 0) {
			bsetprops(BP_ROUTER_IP, value);
			continue;
		}
		if (strcmp(name, "boot.netif.hwaddr") == 0) {
			bsetprops(BP_BOOT_MAC, value);
			continue;
		}
		if (strcmp(name, "boot.netif.ip") == 0) {
			bsetprops(BP_HOST_IP, value);
			continue;
		}
		if (strcmp(name, "boot.netif.netmask") == 0) {
			bsetprops(BP_SUBNET_MASK, value);
			continue;
		}
		if (strcmp(name, "boot.netif.server") == 0) {
			bsetprops(BP_SERVER_IP, value);
			continue;
		}
		if (strcmp(name, "boot.netif.server") == 0) {
			if (do_bsys_getproplen(NULL, BP_SERVER_IP) < 0)
				bsetprops(BP_SERVER_IP, value);
			continue;
		}
		if (strcmp(name, "boot.nfsroot.server") == 0) {
			if (do_bsys_getproplen(NULL, BP_SERVER_IP) < 0)
				bsetprops(BP_SERVER_IP, value);
			continue;
		}
		if (strcmp(name, "boot.nfsroot.path") == 0) {
			bsetprops(BP_SERVER_PATH, value);
			continue;
		}

		if (name_is_blacklisted(name) == B_TRUE)
			continue;

		/* Create new property. */
		bsetprops(name, value);

		/* Avoid reading past the module end. */
		if (size <= (uintptr_t)ptr - (uintptr_t)env)
			return;
	} while (*ptr != '\0');
}

/*
 * 1st pass at building the table of boot properties. This includes:
 * - values set on the command line: -B a=x,b=y,c=z ....
 * - known values we just compute (ie. from xbp)
 * - values from /boot/solaris/bootenv.rc (ie. eeprom(1m) values)
 *
 * the grub command line looked like:
 * kernel boot-file [-B prop=value[,prop=value]...] [boot-args]
 *
 * whoami is the same as boot-file
 */
static void
build_boot_properties(struct xboot_info *xbp)
{
	char *name;
	int name_len;
	char *value;
	int value_len;
	struct boot_modules *bm, *rdbm, *benv = NULL;
	char *propbuf;
	int quoted = 0;
	int boot_arg_len;
	uint_t i, midx;
	char modid[32];
#ifndef __xpv
	static int stdout_val = 0;
	uchar_t boot_device;
	char str[3];
#endif

	/*
	 * These have to be done first, so that kobj_mount_root() works
	 */
	DBG_MSG("Building boot properties\n");
	propbuf = do_bsys_alloc(NULL, NULL, MMU_PAGESIZE, 0);
	DBG((uintptr_t)propbuf);
	if (xbp->bi_module_cnt > 0) {
		bm = xbp->bi_modules;
		rdbm = NULL;
		for (midx = i = 0; i < xbp->bi_module_cnt; i++) {
			if (bm[i].bm_type == BMT_ROOTFS) {
				rdbm = &bm[i];
				continue;
			}
			if (bm[i].bm_type == BMT_HASH ||
			    bm[i].bm_type == BMT_FONT ||
			    bm[i].bm_name == NULL)
				continue;

			if (bm[i].bm_type == BMT_ENV) {
				if (benv == NULL)
					benv = &bm[i];
				else
					continue;
			}

			(void) snprintf(modid, sizeof (modid),
			    "module-name-%u", midx);
			bsetprops(modid, (char *)bm[i].bm_name);
			(void) snprintf(modid, sizeof (modid),
			    "module-addr-%u", midx);
			bsetprop64(modid, (uint64_t)(uintptr_t)bm[i].bm_addr);
			(void) snprintf(modid, sizeof (modid),
			    "module-size-%u", midx);
			bsetprop64(modid, (uint64_t)bm[i].bm_size);
			++midx;
		}
		if (rdbm != NULL) {
			bsetprop64("ramdisk_start",
			    (uint64_t)(uintptr_t)rdbm->bm_addr);
			bsetprop64("ramdisk_end",
			    (uint64_t)(uintptr_t)rdbm->bm_addr + rdbm->bm_size);
		}
	}

	/*
	 * If there are any boot time modules or hashes present, then disable
	 * fast reboot.
	 */
	if (xbp->bi_module_cnt > 1) {
		fastreboot_disable(FBNS_BOOTMOD);
	}

#ifndef __xpv
	/*
	 * Disable fast reboot if we're using the Multiboot 2 boot protocol,
	 * since we don't currently support MB2 info and module relocation.
	 * Note that fast reboot will have already been disabled if multiple
	 * modules are present, since the current implementation assumes that
	 * we only have a single module, the boot_archive.
	 */
	if (xbp->bi_mb_version != 1) {
		fastreboot_disable(FBNS_MULTIBOOT2);
	}
#endif

	DBG_MSG("Parsing command line for boot properties\n");
	value = xbp->bi_cmdline;

	/*
	 * allocate memory to collect boot_args into
	 */
	boot_arg_len = strlen(xbp->bi_cmdline) + 1;
	boot_args = do_bsys_alloc(NULL, NULL, boot_arg_len, MMU_PAGESIZE);
	boot_args[0] = 0;
	boot_arg_len = 0;

#ifdef __xpv
	/*
	 * Xen puts a lot of device information in front of the kernel name
	 * let's grab them and make them boot properties.  The first
	 * string w/o an "=" in it will be the boot-file property.
	 */
	(void) strcpy(namebuf, "xpv-");
	for (;;) {
		/*
		 * get to next property
		 */
		while (ISSPACE(*value))
			++value;
		name = value;
		/*
		 * look for an "="
		 */
		while (*value && !ISSPACE(*value) && *value != '=') {
			value++;
		}
		if (*value != '=') { /* no "=" in the property */
			value = name;
			break;
		}
		name_len = value - name;
		value_len = 0;
		/*
		 * skip over the "="
		 */
		value++;
		while (value[value_len] && !ISSPACE(value[value_len])) {
			++value_len;
		}
		/*
		 * build property name with "xpv-" prefix
		 */
		if (name_len + 4 > 32) { /* skip if name too long */
			value += value_len;
			continue;
		}
		bcopy(name, &namebuf[4], name_len);
		name_len += 4;
		namebuf[name_len] = 0;
		bcopy(value, propbuf, value_len);
		propbuf[value_len] = 0;
		bsetprops(namebuf, propbuf);

		/*
		 * xpv-root is set to the logical disk name of the xen
		 * VBD when booting from a disk-based filesystem.
		 */
		if (strcmp(namebuf, "xpv-root") == 0)
			xen_vbdroot_props(propbuf);
		/*
		 * While we're here, if we have a "xpv-nfsroot" property
		 * then we need to set "fstype" to "nfs" so we mount
		 * our root from the nfs server.  Also parse the xpv-nfsroot
		 * property to create the properties that nfs_mountroot will
		 * need to find the root and mount it.
		 */
		if (strcmp(namebuf, "xpv-nfsroot") == 0)
			xen_nfsroot_props(propbuf);

		if (strcmp(namebuf, "xpv-ip") == 0)
			xen_ip_props(propbuf);
		value += value_len;
	}
#endif

	while (ISSPACE(*value))
		++value;
	/*
	 * value now points at the boot-file
	 */
	value_len = 0;
	while (value[value_len] && !ISSPACE(value[value_len]))
		++value_len;
	if (value_len > 0) {
		whoami = propbuf;
		bcopy(value, whoami, value_len);
		whoami[value_len] = 0;
		bsetprops("boot-file", whoami);
		/*
		 * strip leading path stuff from whoami, so running from
		 * PXE/miniroot makes sense.
		 */
		if (strstr(whoami, "/platform/") != NULL)
			whoami = strstr(whoami, "/platform/");
		bsetprops("whoami", whoami);
	}

	/*
	 * Values forcibly set boot properties on the command line via -B.
	 * Allow use of quotes in values. Other stuff goes on kernel
	 * command line.
	 */
	name = value + value_len;
	while (*name != 0) {
		/*
		 * anything not " -B" is copied to the command line
		 */
		if (!ISSPACE(name[0]) || name[1] != '-' || name[2] != 'B') {
			boot_args[boot_arg_len++] = *name;
			boot_args[boot_arg_len] = 0;
			++name;
			continue;
		}

		/*
		 * skip the " -B" and following white space
		 */
		name += 3;
		while (ISSPACE(*name))
			++name;
		while (*name && !ISSPACE(*name)) {
			value = strstr(name, "=");
			if (value == NULL)
				break;
			name_len = value - name;
			++value;
			value_len = 0;
			quoted = 0;
			for (; ; ++value_len) {
				if (!value[value_len])
					break;

				/*
				 * is this value quoted?
				 */
				if (value_len == 0 &&
				    (value[0] == '\'' || value[0] == '"')) {
					quoted = value[0];
					++value_len;
				}

				/*
				 * In the quote accept any character,
				 * but look for ending quote.
				 */
				if (quoted) {
					if (value[value_len] == quoted)
						quoted = 0;
					continue;
				}

				/*
				 * a comma or white space ends the value
				 */
				if (value[value_len] == ',' ||
				    ISSPACE(value[value_len]))
					break;
			}

			if (value_len == 0) {
				bsetprop(DDI_PROP_TYPE_ANY, name, name_len,
				    NULL, 0);
			} else {
				char *v = value;
				int l = value_len;
				if (v[0] == v[l - 1] &&
				    (v[0] == '\'' || v[0] == '"')) {
					++v;
					l -= 2;
				}
				bcopy(v, propbuf, l);
				propbuf[l] = '\0';
				bsetprop(DDI_PROP_TYPE_STRING, name, name_len,
				    propbuf, l + 1);
			}
			name = value + value_len;
			while (*name == ',')
				++name;
		}
	}

	/*
	 * set boot-args property
	 * 1275 name is bootargs, so set
	 * that too
	 */
	bsetprops("boot-args", boot_args);
	bsetprops("bootargs", boot_args);

	process_boot_environment(benv);

#ifndef __xpv
	/*
	 * Build boot command line for Fast Reboot
	 */
	build_fastboot_cmdline(xbp);

	if (xbp->bi_mb_version == 1) {
		multiboot_info_t *mbi = xbp->bi_mb_info;
		int netboot;
		struct sol_netinfo *sip;

		/*
		 * set the BIOS boot device from GRUB
		 */
		netboot = 0;

		/*
		 * Save various boot information for Fast Reboot
		 */
		save_boot_info(xbp);

		if (mbi != NULL && mbi->flags & MB_INFO_BOOTDEV) {
			boot_device = mbi->boot_device >> 24;
			if (boot_device == 0x20)
				netboot++;
			str[0] = (boot_device >> 4) + '0';
			str[1] = (boot_device & 0xf) + '0';
			str[2] = 0;
			bsetprops("bios-boot-device", str);
		} else {
			netboot = 1;
		}

		/*
		 * In the netboot case, drives_info is overloaded with the
		 * dhcp ack. This is not multiboot compliant and requires
		 * special pxegrub!
		 */
		if (netboot && mbi->drives_length != 0) {
			sip = (struct sol_netinfo *)(uintptr_t)mbi->drives_addr;
			if (sip->sn_infotype == SN_TYPE_BOOTP)
				bsetprop(DDI_PROP_TYPE_BYTE,
				    "bootp-response",
				    sizeof ("bootp-response"),
				    (void *)(uintptr_t)mbi->drives_addr,
				    mbi->drives_length);
			else if (sip->sn_infotype == SN_TYPE_RARP)
				setup_rarp_props(sip);
		}
	}

	bsetprop32("stdout", stdout_val);
#endif /* __xpv */

	/*
	 * more conjured up values for made up things....
	 */
	bsetprops("mfg-name", "Oxide Computer Company");
	bsetprops("impl-arch-name", "oxide");
}

#if !defined(__xpv)
/*
 * simple description of a stack frame (args are 32 bit only currently)
 */
typedef struct bop_frame {
	struct bop_frame *old_frame;
	pc_t retaddr;
	long arg[1];
} bop_frame_t;

void
bop_traceback(bop_frame_t *frame)
{
	pc_t pc;
	int cnt;
	char *ksym;
	ulong_t off;

	bop_printf(NULL, "Stack traceback:\n");
	for (cnt = 0; cnt < 30; ++cnt) {	/* up to 30 frames */
		pc = frame->retaddr;
		if (pc == 0)
			break;
		ksym = kobj_getsymname(pc, &off);
		if (ksym)
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
#endif	/* !defined(__xpv) */

char * empty_cmdline = "";
/*
 * This is where we enter the kernel. It dummies up the boot_ops and
 * boot_syscalls vectors and jumps off to _kobj_boot()
 */
void
_start(struct xboot_info *xbp)
{
	bootops_t *bops = &bootop;
	extern void _kobj_boot();

	__asm__ __volatile__(
		"outw	%0, $0x80\n\t"
	: : "a" ((unsigned short)(0x1de)) : );

	/*
	 * 1st off - initialize the console for any error messages
	 */
	xbootp = xbp;

	if (*((uint32_t *)(FASTBOOT_SWTCH_PA + FASTBOOT_STACK_OFFSET)) ==
	    FASTBOOT_MAGIC) {
		post_fastreboot = 1;
		*((uint32_t *)(FASTBOOT_SWTCH_PA + FASTBOOT_STACK_OFFSET)) = 0;
	}

	bcons_init(xbp);
	have_console = 1;

	/*
	 * enable debugging XXXBOOT
	 */
	if (1 || find_boot_prop("kbm_debug") != NULL)
		kbm_debug = 1;

	// TODO(ganshun): remove when we have proper cmdline handling.
	xbp->bi_cmdline = empty_cmdline;

	DBG_MSG("\n\n*** Entered illumos in _start() cmdline is: ");
	DBG_MSG((char *)xbp->bi_cmdline);
	DBG_MSG("\n\n\n");

	/*
	 * physavail is no longer used by startup
	 */
	bm.physinstalled = xbp->bi_phys_install;
	bm.pcimem = xbp->bi_pcimem;
	bm.rsvdmem = xbp->bi_rsvdmem;
	bm.physavail = NULL;

	/*
	 * initialize the boot time allocator
	 */
	next_phys = xbp->bi_next_paddr;
	DBG(next_phys);
	next_virt = (uintptr_t)xbp->bi_next_vaddr;
	DBG(next_virt);
	DBG_MSG("Initializing boot time memory management...");
	kbm_init(xbp);
	DBG_MSG("done\n");

	/*
	 * Fill in the bootops vector
	 */
	bops->bsys_version = BO_VERSION;
	bops->boot_mem = &bm;
	bops->bsys_alloc = do_bsys_alloc;
	bops->bsys_free = do_bsys_free;
	bops->bsys_getproplen = do_bsys_getproplen;
	bops->bsys_getprop = do_bsys_getprop;
	bops->bsys_nextprop = do_bsys_nextprop;
	bops->bsys_printf = bop_printf;

	/*
	 * BOP_EALLOC() is no longer needed
	 */
	bops->bsys_ealloc = do_bsys_ealloc;

#ifdef __xpv
	/*
	 * On domain 0 we need to free up some physical memory that is
	 * usable for DMA. Since GRUB loaded the boot_archive, it is
	 * sitting in low MFN memory. We'll relocated the boot archive
	 * pages to high PFN memory.
	 */
	if (DOMAIN_IS_INITDOMAIN(xen_info))
		relocate_boot_archive(xbp);
#endif

#ifndef __xpv
	/*
	 * Install an IDT to catch early pagefaults (shouldn't have any).
	 * Also needed for kmdb.
	 */
	bop_idt_init();
#endif
	/*
	 * Start building the boot properties from the command line
	 */
	DBG_MSG("Initializing boot properties:\n");
	build_boot_properties(xbp);

	if (find_boot_prop("prom_debug") || kbm_debug) {
		char *value;

		value = do_bsys_alloc(NULL, NULL, MMU_PAGESIZE, MMU_PAGESIZE);
		boot_prop_display(value);
	}

	/*
	 * jump into krtld...
	 */
	_kobj_boot(&bop_sysp, NULL, bops, NULL);
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

/*ARGSUSED*/
int
boot_compinfo(int fd, struct compinfo *cbp)
{
	cbp->iscmp = 0;
	cbp->blksize = MAXBSIZE;
	return (0);
}

/*
 * Get an integer value for given boot property
 */
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

int
bootprop_getstr(const char *prop_name, char *buf, size_t buflen)
{
	int boot_prop_len = BOP_GETPROPLEN(bootops, prop_name);

	if (boot_prop_len < 0 || boot_prop_len >= buflen ||
	    BOP_GETPROP(bootops, prop_name, buf) < 0)
		return (-1);

	return (0);
}
