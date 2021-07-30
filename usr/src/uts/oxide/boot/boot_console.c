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
 * Copyright 2021 Oxide Computer Co.
 *
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/types.h>
#include <sys/boot_console.h>
#include <sys/dw_apb_uart.h>
#include <sys/uart.h>

static uintptr_t dw_apb_uart_hdl;

void
bcons_init(void)
{
	dw_apb_uart_hdl = dw_apb_uart_init(DAP_0, 3000000,
		AD_8BITS, AP_NONE, AS_1BIT);
}

static void
_doputchar(int c)
{
	dw_apb_uart_tx_nb(dw_apb_uart_hdl, (uint8_t *)(&c), 1);
}

void
bcons_putchar(int c)
{
	if (c == '\n')
		_doputchar('\r');
	_doputchar(c);
}

int
bcons_getchar(void)
{
	return (int)(dw_apb_uart_rx_one(dw_apb_uart_hdl));
}

int
bcons_ischar(void)
{
	return dw_apb_uart_dr(dw_apb_uart_hdl);
}
