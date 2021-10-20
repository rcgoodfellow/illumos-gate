/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2021 Oxide Computer Co.
 */

#ifndef _SYS_BOOT_DEBUG_H
#define	_SYS_BOOT_DEBUG_H

/*
 * Common macros for printf debugging during early boot phases.  These macros
 * are available prior to prom_printf() and should be used only by machine-
 * specific code.
 */

#include <sys/types.h>
#include <sys/bootconf.h>

#ifdef __cplusplus
extern "C" {
#endif

extern boolean_t kbm_debug;
extern void kbm_debug_printf(const char *, int, const char *, ...)
    __KPRINTFLIKE(3);

#define	DBG_MSG(_fmt, ...)		\
	kbm_debug_printf(__FILE__, __LINE__, _fmt, ##__VA_ARGS__)

#define	DBG(_var)	\
	kbm_debug_printf(__FILE__, __LINE__, "%s is %" PRIx64 "\n", #_var, \
	    ((uint64_t)(_var)))

#define	eb_printf(_fmt, ...)		\
	bop_printf(NULL, _fmt, ##__VA_ARGS__)

#define	eb_vprintf(_fmt, _ap)		\
	vbop_printf(NULL, _fmt, _ap)

extern void eb_halt(void) __NORETURN;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_BOOT_DEBUG_H */
