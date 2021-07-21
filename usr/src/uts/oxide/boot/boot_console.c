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
 * Copyright (c) 2012 Gary Mills
 * Copyright 2020 Joyent, Inc.
 *
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Boot console support.  Most of the file is shared between dboot, and the
 * early kernel / fakebop.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/archsystm.h>
#include <sys/framebuffer.h>
#include <sys/boot_console.h>
#include <sys/panic.h>
#include <sys/ctype.h>
#include <sys/ascii.h>
#include <sys/vgareg.h>
#if defined(__xpv)
#include <sys/hypervisor.h>
#endif /* __xpv */

#include "boot_console_impl.h"
#include "boot_serial.h"

#if !defined(_BOOT)
#include <sys/bootconf.h>
#if defined(__xpv)
#include <sys/evtchn_impl.h>
#endif /* __xpv */
static char *defcons_buf;
static char *defcons_cur;
#endif /* _BOOT */

#if defined(__xpv)
extern void bcons_init_xen(char *);
extern void bcons_putchar_xen(int);
extern int bcons_getchar_xen(void);
extern int bcons_ischar_xen(void);
#endif /* __xpv */

#include <sys/dw_apb_uart.h>
#include <sys/uart.h>

fb_info_t fb_info;
static bcons_dev_t bcons_dev;				/* Device callbacks */
static int console = CONS_SCREEN_TEXT;
static int diag = CONS_INVALID;
static int tty_num = 0;
static int tty_addr[] = {0x3f8, 0x2f8, 0x3e8, 0x2e8};
static char *boot_line;
static struct boot_env {
	char	*be_env;	/* ends with double ascii nul */
	size_t	be_size;	/* size of the environment, including nul */
} boot_env;

/*
 * Simple console terminal emulator for early boot.
 * We need this to support kmdb, all other console output is supposed
 * to be simple text output.
 */
typedef enum btem_state_type {
	A_STATE_START,
	A_STATE_ESC,
	A_STATE_CSI,
	A_STATE_CSI_QMARK,
	A_STATE_CSI_EQUAL
} btem_state_type_t;

#define	BTEM_MAXPARAMS	5
typedef struct btem_state {
	btem_state_type_t btem_state;
	boolean_t btem_gotparam;
	int btem_curparam;
	int btem_paramval;
	int btem_params[BTEM_MAXPARAMS];
} btem_state_t;

static btem_state_t boot_tem;

static void defcons_putchar(int);

#if defined(__xpv)
static int console_hypervisor_redirect = B_FALSE;
static int console_hypervisor_device = CONS_INVALID;
static int console_hypervisor_tty_num = 0;

/* Obtain the hypervisor console type */
int
console_hypervisor_dev_type(int *tnum)
{
	if (tnum != NULL)
		*tnum = console_hypervisor_tty_num;
	return (console_hypervisor_device);
}
#endif /* __xpv */

static int port;

static uintptr_t dw_apb_uart_hdl;

/* Advance str pointer past white space */
#define	EAT_WHITE_SPACE(str)	{			\
	while ((*str != '\0') && ISSPACE(*str))		\
		str++;					\
}

/*
 * boot_line is set when we call here.  Search it for the argument name,
 * and if found, return a pointer to it.
 */
static char *
find_boot_line_prop(const char *name)
{
	char *ptr;
	char *ret = NULL;
	char end_char;
	size_t len;

	if (boot_line == NULL)
		return (NULL);

	len = strlen(name);

	/*
	 * We have two nested loops here: the outer loop discards all options
	 * except -B, and the inner loop parses the -B options looking for
	 * the one we're interested in.
	 */
	for (ptr = boot_line; *ptr != '\0'; ptr++) {
		EAT_WHITE_SPACE(ptr);

		if (*ptr == '-') {
			ptr++;
			while ((*ptr != '\0') && (*ptr != 'B') &&
			    !ISSPACE(*ptr))
				ptr++;
			if (*ptr == '\0')
				goto out;
			else if (*ptr != 'B')
				continue;
		} else {
			while ((*ptr != '\0') && !ISSPACE(*ptr))
				ptr++;
			if (*ptr == '\0')
				goto out;
			continue;
		}

		do {
			ptr++;
			EAT_WHITE_SPACE(ptr);

			if ((strncmp(ptr, name, len) == 0) &&
			    (ptr[len] == '=')) {
				ptr += len + 1;
				if ((*ptr == '\'') || (*ptr == '"')) {
					ret = ptr + 1;
					end_char = *ptr;
					ptr++;
				} else {
					ret = ptr;
					end_char = ',';
				}
				goto consume_property;
			}

			/*
			 * We have a property, and it's not the one we're
			 * interested in.  Skip the property name.  A name
			 * can end with '=', a comma, or white space.
			 */
			while ((*ptr != '\0') && (*ptr != '=') &&
			    (*ptr != ',') && (!ISSPACE(*ptr)))
				ptr++;

			/*
			 * We only want to go through the rest of the inner
			 * loop if we have a comma.  If we have a property
			 * name without a value, either continue or break.
			 */
			if (*ptr == '\0')
				goto out;
			else if (*ptr == ',')
				continue;
			else if (ISSPACE(*ptr))
				break;
			ptr++;

			/*
			 * Is the property quoted?
			 */
			if ((*ptr == '\'') || (*ptr == '"')) {
				end_char = *ptr;
				ptr++;
			} else {
				/*
				 * Not quoted, so the string ends at a comma
				 * or at white space.  Deal with white space
				 * later.
				 */
				end_char = ',';
			}

			/*
			 * Now, we can ignore any characters until we find
			 * end_char.
			 */
consume_property:
			for (; (*ptr != '\0') && (*ptr != end_char); ptr++) {
				if ((end_char == ',') && ISSPACE(*ptr))
					break;
			}
			if (*ptr && (*ptr != ',') && !ISSPACE(*ptr))
				ptr++;
		} while (*ptr == ',');
	}
out:
	return (ret);
}

/*
 * Find prop from boot env module. The data in module is list of C strings
 * name=value, the list is terminated by double nul.
 */
static const char *
find_boot_env_prop(const char *name)
{
	char *ptr;
	size_t len;
	uintptr_t size;

	if (boot_env.be_env == NULL)
		return (NULL);

	ptr = boot_env.be_env;
	len = strlen(name);

	/*
	 * Make sure we have at least len + 2 bytes in the environment.
	 * We are looking for name=value\0 constructs, and the environment
	 * itself is terminated by '\0'.
	 */
	if (boot_env.be_size < len + 2)
		return (NULL);

	do {
		if ((strncmp(ptr, name, len) == 0) && (ptr[len] == '=')) {
			ptr += len + 1;
			return (ptr);
		}
		/* find the first '\0' */
		while (*ptr != '\0') {
			ptr++;
			size = (uintptr_t)ptr - (uintptr_t)boot_env.be_env;
			if (size > boot_env.be_size)
				return (NULL);
		}
		ptr++;

		/* If the remainder is shorter than name + 2, get out. */
		size = (uintptr_t)ptr - (uintptr_t)boot_env.be_env;
		if (boot_env.be_size - size < len + 2)
			return (NULL);
	} while (*ptr != '\0');
	return (NULL);
}

/*
 * Get prop value from either command line or boot environment.
 * We always check kernel command line first, as this will keep the
 * functionality and will allow user to override the values in environment.
 */
const char *
find_boot_prop(const char *name)
{
	const char *value = find_boot_line_prop(name);

	if (value == NULL)
		value = find_boot_env_prop(name);
	return (value);
}

#define	MATCHES(p, pat)	\
	(strncmp(p, pat, strlen(pat)) == 0 ? (p += strlen(pat), 1) : 0)

#define	SKIP(p, c)				\
	while (*(p) != 0 && *p != (c))		\
		++(p);				\
	if (*(p) == (c))			\
		++(p);

/*
 * find a tty mode property either from cmdline or from boot properties
 */
static const char *
get_mode_value(char *name)
{
	/*
	 * when specified on boot line it looks like "name" "="....
	 */
	if (boot_line != NULL) {
		return (find_boot_prop(name));
	}

#if defined(_BOOT)
	return (NULL);
#else
	/*
	 * if we're running in the full kernel we check the bootenv.rc settings
	 */
	{
		static char propval[20];

		propval[0] = 0;
		if (do_bsys_getproplen(NULL, name) <= 0)
			return (NULL);
		(void) do_bsys_getprop(NULL, name, propval);
		return (propval);
	}
#endif
}

/* Obtain the console type */
int
boot_console_type(int *tnum)
{
	if (tnum != NULL)
		*tnum = tty_num;
	return (console);
}

/*
 * A structure to map console names to values.
 */
typedef struct {
	char *name;
	int value;
} console_value_t;

console_value_t console_devices[] = {
	{ "ttya", CONS_TTY },	/* 0 */
	{ "ttyb", CONS_TTY },	/* 1 */
	{ "ttyc", CONS_TTY },	/* 2 */
	{ "ttyd", CONS_TTY },	/* 3 */
	{ "text", CONS_SCREEN_TEXT },
	{ "graphics", CONS_SCREEN_GRAPHICS },
#if defined(__xpv)
	{ "hypervisor", CONS_HYPERVISOR },
#endif
#if !defined(_BOOT)
	{ "usb-serial", CONS_USBSER },
#endif
	{ NULL, CONS_INVALID }
};

static void
bcons_init_env(struct xboot_info *xbi)
{
	uint32_t i;
	struct boot_modules *modules;

	modules = (struct boot_modules *)(uintptr_t)xbi->bi_modules;
	for (i = 0; i < xbi->bi_module_cnt; i++) {
		if (modules[i].bm_type == BMT_ENV)
			break;
	}
	if (i == xbi->bi_module_cnt)
		return;

	boot_env.be_env = (char *)(uintptr_t)modules[i].bm_addr;
	boot_env.be_size = modules[i].bm_size;
}

/*
 * TODO.
 * quick and dirty local atoi. Perhaps should build with strtol, but
 * dboot & early boot mix does overcomplicate things much.
 * Stolen from libc anyhow.
 */
static int
atoi(const char *p)
{
	int n, c, neg = 0;
	unsigned char *up = (unsigned char *)p;

	if (!isdigit(c = *up)) {
		while (isspace(c))
			c = *++up;
		switch (c) {
		case '-':
			neg++;
			/* FALLTHROUGH */
		case '+':
			c = *++up;
		}
		if (!isdigit(c))
			return (0);
	}
	for (n = '0' - c; isdigit(c = *++up); ) {
		n *= 10; /* two steps to avoid unnecessary overflow */
		n += '0' - c; /* accum neg to avoid surprises at MAX */
	}
	return (neg ? n : -n);
}

/*
 * Go through the known console device names trying to match the string we were
 * given.  The string on the command line must end with a comma or white space.
 *
 * For convenience, we provide the caller with an integer index for the CONS_TTY
 * case.
 */
static int
lookup_console_device(const char *cons_str, int *indexp)
{
	int n, cons;
	size_t len, cons_len;
	console_value_t *consolep;

	cons = CONS_INVALID;
	if (cons_str != NULL) {

		cons_len = strlen(cons_str);
		for (n = 0; console_devices[n].name != NULL; n++) {
			consolep = &console_devices[n];
			len = strlen(consolep->name);
			if ((len <= cons_len) && ((cons_str[len] == '\0') ||
			    (cons_str[len] == ',') || (cons_str[len] == '\'') ||
			    (cons_str[len] == '"') || ISSPACE(cons_str[len])) &&
			    (strncmp(cons_str, consolep->name, len) == 0)) {
				cons = consolep->value;
				if (cons == CONS_TTY)
					*indexp = n;
				break;
			}
		}
	}
	return (cons);
}

void
bcons_init(struct xboot_info *xbi)
{
	console = CONS_TTY;
	dw_apb_uart_hdl = dw_apb_uart_init(DAP_0, 3000000,
		AD_8BITS, AP_NONE, AS_1BIT);
}

static int
serial_ischar(void)
{
	return (inb(port + LSR) & RCA);
}

static void
btem_control(btem_state_t *btem, int c)
{
	int y, rows, cols;

	rows = fb_info.cursor.pos.y;
	cols = fb_info.cursor.pos.x;

	btem->btem_state = A_STATE_START;
	switch (c) {
	case A_BS:
		bcons_dev.bd_setpos(rows, cols - 1);
		break;

	case A_HT:
		cols += 8 - (cols % 8);
		if (cols >= fb_info.terminal.x)
			cols = fb_info.terminal.x - 1;
		bcons_dev.bd_setpos(rows, cols);
		break;

	case A_CR:
		bcons_dev.bd_setpos(rows, 0);
		break;

	case A_FF:
		for (y = 0; y < fb_info.terminal.y; y++) {
			bcons_dev.bd_setpos(y, 0);
			bcons_dev.bd_eraseline();
		}
		bcons_dev.bd_setpos(0, 0);
		break;

	case A_ESC:
		btem->btem_state = A_STATE_ESC;
		break;

	default:
		bcons_dev.bd_putchar(c);
		break;
	}
}

/*
 * if parameters [0..count - 1] are not set, set them to the value
 * of newparam.
 */
static void
btem_setparam(btem_state_t *btem, int count, int newparam)
{
	int i;

	for (i = 0; i < count; i++) {
		if (btem->btem_params[i] == -1)
			btem->btem_params[i] = newparam;
	}
}

static void
btem_chkparam(btem_state_t *btem, int c)
{
	int rows, cols;

	rows = fb_info.cursor.pos.y;
	cols = fb_info.cursor.pos.x;
	switch (c) {
	case '@':			/* insert char */
		btem_setparam(btem, 1, 1);
		bcons_dev.bd_shift(btem->btem_params[0]);
		break;

	case 'A':			/* cursor up */
		btem_setparam(btem, 1, 1);
		bcons_dev.bd_setpos(rows - btem->btem_params[0], cols);
		break;

	case 'B':			/* cursor down */
		btem_setparam(btem, 1, 1);
		bcons_dev.bd_setpos(rows + btem->btem_params[0], cols);
		break;

	case 'C':			/* cursor right */
		btem_setparam(btem, 1, 1);
		bcons_dev.bd_setpos(rows, cols + btem->btem_params[0]);
		break;

	case 'D':			/* cursor left */
		btem_setparam(btem, 1, 1);
		bcons_dev.bd_setpos(rows, cols - btem->btem_params[0]);
		break;

	case 'K':
		bcons_dev.bd_eraseline();
		break;
	default:
		/* bcons_dev.bd_putchar(c); */
		break;
	}
	btem->btem_state = A_STATE_START;
}

static void
btem_getparams(btem_state_t *btem, int c)
{
	if (isdigit(c)) {
		btem->btem_paramval = btem->btem_paramval * 10 + c - '0';
		btem->btem_gotparam = B_TRUE;
		return;
	}

	if (btem->btem_curparam < BTEM_MAXPARAMS) {
		if (btem->btem_gotparam == B_TRUE) {
			btem->btem_params[btem->btem_curparam] =
			    btem->btem_paramval;
		}
		btem->btem_curparam++;
	}

	if (c == ';') {
		/* Restart parameter search */
		btem->btem_gotparam = B_FALSE;
		btem->btem_paramval = 0;
	} else {
		btem_chkparam(btem, c);
	}
}

/* Simple boot terminal parser. */
static void
btem_parse(btem_state_t *btem, int c)
{
	int i;

	/* Normal state? */
	if (btem->btem_state == A_STATE_START) {
		if (c == A_CSI || c < ' ')
			btem_control(btem, c);
		else
			bcons_dev.bd_putchar(c);
		return;
	}

	/* In <ESC> sequence */
	if (btem->btem_state != A_STATE_ESC) {
		btem_getparams(btem, c);
		return;
	}

	/* Previous char was <ESC> */
	switch (c) {
	case '[':
		btem->btem_curparam = 0;
		btem->btem_paramval = 0;
		btem->btem_gotparam = B_FALSE;
		/* clear the parameters */
		for (i = 0; i < BTEM_MAXPARAMS; i++)
			btem->btem_params[i] = -1;
		btem->btem_state = A_STATE_CSI;
		return;

	case 'Q':	/* <ESC>Q */
	case 'C':	/* <ESC>C */
		btem->btem_state = A_STATE_START;
		return;

	default:
		btem->btem_state = A_STATE_START;
		break;
	}

	if (c < ' ')
		btem_control(btem, c);
	else
		bcons_dev.bd_putchar(c);
}

static void
_doputchar(int device, int c)
{
	switch (device) {
	case CONS_TTY:
		dw_apb_uart_tx_nb(dw_apb_uart_hdl, (uint8_t*)(&c), 1);
		return;
	case CONS_SCREEN_TEXT:
	case CONS_FRAMEBUFFER:
		bcons_dev.bd_cursor(B_FALSE);
		btem_parse(&boot_tem, c);
		bcons_dev.bd_cursor(B_TRUE);
		return;
	case CONS_SCREEN_GRAPHICS:
#if !defined(_BOOT)
	case CONS_USBSER:
		defcons_putchar(c);
#endif /* _BOOT */
	default:
		return;
	}
}

void
bcons_putchar(int c)
{
#if defined(__xpv)
	if (!DOMAIN_IS_INITDOMAIN(xen_info) ||
	    console == CONS_HYPERVISOR) {
		bcons_putchar_xen(c);
		return;
	}
#endif /* __xpv */

	if (c == '\n') {
		_doputchar(console, '\r');
		if (diag != console)
			_doputchar(diag, '\r');
	}
	_doputchar(console, c);
	if (diag != console)
		_doputchar(diag, c);
}

/*
 * kernel character input functions
 */
int
bcons_getchar(void)
{
	for (;;) {
		return (int)(dw_apb_uart_rx_one(dw_apb_uart_hdl));
		//if (serial_ischar())
			//return (serial_getchar());
	}
}

/*
 * Nothing below is used by dboot.
 */
#if !defined(_BOOT)

int
bcons_ischar(void)
{
	return dw_apb_uart_dr(dw_apb_uart_hdl);
}

/*
 * 2nd part of console initialization: we've now processed bootenv.rc; update
 * console settings as appropriate. This only really processes serial console
 * modifications.
 */
void
bcons_post_bootenvrc(char *inputdev, char *outputdev, char *consoledev)
{
	int cons = CONS_INVALID;
	int ttyn;
	char *devnames[] = { consoledev, outputdev, inputdev, NULL };
	console_value_t *consolep;
	int i;
	extern int post_fastreboot;

	ttyn = 0;
	if (post_fastreboot && console == CONS_SCREEN_GRAPHICS)
		console = CONS_SCREEN_TEXT;

	for (i = 0; devnames[i] != NULL; i++) {
		cons = lookup_console_device(devnames[i], &ttyn);
		if (cons != CONS_INVALID)
			break;
	}

	if (cons == CONS_INVALID) {
		/*
		 * No console change, but let's see if bootenv.rc had a mode
		 * setting we should apply.
		 * Gan: I don't think we need to vary the parameters for our
		 * system post boot. Going to ignore this.
		 */
		//if (console == CONS_TTY && !bootprop_set_tty_mode)
		//	serial_init();
		return;
	}

	console = cons;

	if (console == CONS_TTY) {
		tty_num = ttyn;
		//serial_init();
	}
}

static void
defcons_putchar(int c)
{
	if (defcons_buf != NULL &&
	    defcons_cur + 1 - defcons_buf < MMU_PAGESIZE) {
		*defcons_cur++ = c;
		*defcons_cur = 0;
	}
}

#endif /* _BOOT */
