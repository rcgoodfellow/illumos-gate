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
#include <sys/boot_console.h>

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
	/* XXX Fix this so it refers to the correct DW UART driver. */
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
	return (plat_ttypath(0));
}

char *
plat_stdoutpath(void)
{
	return (plat_ttypath(0));
}

char *
plat_diagpath(void)
{
	return (plat_ttypath(0));
}
