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

#ifndef _SYS_DW_APB_UART_H
#define	_SYS_DW_APB_UART_H

#include <sys/types.h>
#include <sys/uart.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dw_apb_port {
	DAP_0 = 0x1000000,
	DAP_1 = 0x1000001,
	DAP_2 = 0x1000002,
	DAP_3 = 0x1000003
} dw_apb_port_t;

extern void *dw_apb_uart_init(const dw_apb_port_t, const uint32_t,
    const async_databits_t, const async_parity_t, const async_stopbits_t);
extern void dw_apb_uart_flush(void *);
extern size_t dw_apb_uart_rx_nb(void *, uint8_t *, size_t);
extern uint8_t dw_apb_uart_rx_one(void *);
extern size_t dw_apb_uart_tx_nb(void *, const uint8_t *, size_t);
extern void dw_apb_uart_tx(void *, const uint8_t *, size_t);
extern boolean_t dw_apb_uart_dr(void *);
extern boolean_t dw_apb_uart_tfnf(void *);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DW_APB_UART_H */
