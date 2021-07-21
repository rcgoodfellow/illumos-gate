#ifndef	_DW_APB_UART_H
#define	_DW_APB_UART_H

#include <sys/types.h>
#include <sys/uart.h>

typedef enum dw_apb_port {
	DAP_0 = 0x1000000,
	DAP_1 = 0x1000001,
	DAP_2 = 0x1000002,
	DAP_3 = 0x1000003
} dw_apb_port_t;

extern uintptr_t dw_apb_uart_init(const dw_apb_port_t, const uint32_t,
    const async_databits_t, const async_parity_t, const async_stopbits_t);
extern size_t dw_apb_uart_rx_nb(uintptr_t, uint8_t *, size_t);
extern uint8_t dw_apb_uart_rx_one(uintptr_t);
extern size_t dw_apb_uart_tx_nb(uintptr_t, const uint8_t *, size_t);
extern void dw_apb_uart_tx(uintptr_t, const uint8_t *, size_t);
extern int dw_apb_uart_dr(uintptr_t);

#endif	/* _DW_APB_UART_H */
