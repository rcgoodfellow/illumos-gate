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
 *
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * isa-specific console configuration routines
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/cmn_err.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/debug.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunndi.h>
#include <sys/esunddi.h>
#include <sys/ddi_impldefs.h>
#include <sys/promif.h>
#include <sys/modctl.h>
#include <sys/termios.h>
#include <sys/pci.h>
#include <sys/framebuffer.h>
#include <sys/boot_console.h>
#if defined(__xpv)
#include <sys/hypervisor.h>
#endif

extern int pseudo_isa;

int
plat_use_polled_debug()
{
	return (0);
}

int
plat_support_serial_kbd_and_ms()
{
	return (0);
}

#define	A_CNT(arr)	(sizeof (arr) / sizeof (arr[0]))

#ifndef	CONS_INVALID
#define	CONS_INVALID		-1
#define	CONS_SCREEN_TEXT	0
#define	CONS_TTY		1
#define	CONS_XXX		2	/* Unused */
#define	CONS_USBSER		3
#define	CONS_HYPERVISOR		4
#define	CONS_SCREEN_GRAPHICS	5
#endif	/* CONS_INVALID */

char *plat_fbpath(void);

static int
console_type(int *tnum)
{
	static int tty_num = 0;

	if (tnum != NULL)
		*tnum = tty_num;

	return (CONS_TTY);
}

int
plat_stdin_is_keyboard(void)
{
	return (0);
}

int
plat_stdout_is_framebuffer(void)
{
	return (0);
}

static char *
plat_ttypath(int inum)
{
	static char *defaultpath[] = {
	    "/isa/asy@1,3f8:a",
	    "/isa/asy@1,2f8:b",
	    "/isa/asy@1,3e8:c",
	    "/isa/asy@1,2e8:d"
	};
	static char path[MAXPATHLEN];
	char *bp;
	major_t major;
	dev_info_t *dip;

	if (pseudo_isa)
		return (defaultpath[inum]);

	if ((major = ddi_name_to_major("asy")) == (major_t)-1)
		return (NULL);

	if ((dip = devnamesp[major].dn_head) == NULL)
		return (NULL);

	for (; dip != NULL; dip = ddi_get_next(dip)) {
		if (i_ddi_attach_node_hierarchy(dip) != DDI_SUCCESS)
			return (NULL);

		if (DEVI(dip)->devi_minor->ddm_name[0] == ('a' + (char)inum))
			break;
	}
	if (dip == NULL)
		return (NULL);

	(void) ddi_pathname(dip, path);
	bp = path + strlen(path);
	(void) snprintf(bp, 3, ":%s", DEVI(dip)->devi_minor->ddm_name);

	return (path);
}

/*
 * Another possible enhancement could be to use properties
 * for the port mapping rather than simply hard-code them.
 */
char *
plat_stdinpath(void)
{
	int tty_num = 0;

	ASSERT(console_type(&tty_num) == CONS_TTY);
	return (plat_ttypath(tty_num));
}

char *
plat_stdoutpath(void)
{
	int tty_num = 0;

	ASSERT(console_type(&tty_num) == CONS_TTY);
	return (plat_ttypath(tty_num));
}

char *
plat_diagpath(void)
{
	dev_info_t *root;
	char *diag;
	int tty_num = -1;

	root = ddi_root_node();

	if (ddi_prop_lookup_string(DDI_DEV_T_ANY, root, DDI_PROP_DONTPASS,
	    "diag-device", &diag) == DDI_SUCCESS) {
		if (strlen(diag) == 4 && strncmp(diag, "tty", 3) == 0 &&
		    diag[3] >= 'a' && diag[3] <= 'd') {
			tty_num = diag[3] - 'a';
		}
		ddi_prop_free(diag);
	}

	if (tty_num != -1)
		return (plat_ttypath(tty_num));
	return (NULL);
}

void
plat_tem_get_colors(uint8_t *fg, uint8_t *bg)
{
	*fg = fb_info.fg_color;
	*bg = fb_info.bg_color;
}

void
plat_tem_get_inverses(int *inverse, int *inverse_screen)
{
	*inverse = fb_info.inverse == B_TRUE? 1 : 0;
	*inverse_screen = fb_info.inverse_screen == B_TRUE? 1 : 0;
}

void
plat_tem_get_prom_font_size(int *charheight, int *windowtop)
{
	*charheight = fb_info.font_height;
	*windowtop = fb_info.terminal_origin.y;
}

/*ARGSUSED*/
void
plat_tem_get_prom_size(size_t *height, size_t *width)
{
	*height = fb_info.terminal.y;
	*width = fb_info.terminal.x;
}

/* this gets called once at boot time and only in case of VIS_PIXEL */
void
plat_tem_hide_prom_cursor(void)
{
}

/*ARGSUSED*/
void
plat_tem_get_prom_pos(uint32_t *row, uint32_t *col)
{
	*row = fb_info.cursor.pos.y;
	*col = fb_info.cursor.pos.x;
}
