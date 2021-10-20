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

#ifndef _SYS_UART_H
#define	_SYS_UART_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum async_databits {
	AD_5BITS = 0x1100001,
	AD_6BITS = 0x1100002,
	AD_7BITS = 0x1100003,
	AD_8BITS = 0x1100004,
	AD_9BITS = 0x1100005
} async_databits_t;

typedef enum async_stopbits {
	AS_1BIT = 0x1200000,
	AS_15BITS = 0x1200001,
	AS_2BITS = 0x1200002
} async_stopbits_t;

typedef enum async_parity {
	AP_NONE = 0x1300000,
	AP_EVEN = 0x1300001,
	AP_ODD = 0x1300002,
	AP_MARK = 0x1300003,
	AP_SPACE = 0x1300004
} async_parity_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UART_H */
