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

/*
 * Definitions for the DesignWare APB UART found in AMD FCHs.  It is mostly
 * 16550-compatible but is memory-mapped.
 */

#include <sys/bootconf.h>
#include <sys/types.h>
#include <sys/dw_apb_uart.h>
#include <sys/boot_debug.h>
#include <sys/uart.h>
#include <vm/kboot_mmu.h>

static const uintptr_t DW_APB_PORT_ADDRS[] = {
	0xFEDC9000UL,
	0xFEDCA000UL,
	0xFEDCE000UL,
	0xFEDCF000UL
};

/*
 * The high bit in every one of this UART's writable registers is reserved.
 * Therefore we use it to signal an error within this implementation.  This
 * is never exposed to callers in any way.
 */
static const uint32_t DAR_INVALID = 0x80000000;

static const ptrdiff_t DW_APB_REG_DLL = 0x00;
static const ptrdiff_t DW_APB_REG_RBR = 0x00;
static const ptrdiff_t DW_APB_REG_THR = 0x00;
static const ptrdiff_t DW_APB_REG_DLH = 0x04;
static const ptrdiff_t DW_APB_REG_IER = 0x04;
static const ptrdiff_t DW_APB_REG_FCR = 0x08;
static const uint32_t DAR_FCR_FIFOE = 0x01;
static const uint32_t DAR_FCR_RFIFOR = 0x02;
static const uint32_t DAR_FCR_XFIFOR = 0x04;
static const uint32_t DAR_FCR_DMAM = 0x08;
static const uint32_t DAR_FCR_DMAM_SINGLE = 0x00;
static const uint32_t DAR_FCR_DMAM_MULTI = 0x08;
static const uint32_t DAR_FCR_TET = 0x30;
static const uint32_t DAR_FCR_TET_EMPTY = 0x00;
static const uint32_t DAR_FCR_TET_2CHAR = 0x10;
static const uint32_t DAR_FCR_TET_QUARTER = 0x20;
static const uint32_t DAR_FCR_TET_HALF = 0x30;
static const uint32_t DAR_FCR_RT = 0xC0;
static const uint32_t DAR_FCR_RT_1CHAR = 0x00;
static const uint32_t DAR_FCR_RT_QUARTER = 0x40;
static const uint32_t DAR_FCR_RT_HALF = 0x80;
static const uint32_t DAR_FCR_RT_2LEFT = 0xC0;

static const ptrdiff_t DW_APB_REG_IIR = 0x08;
static const ptrdiff_t DW_APB_REG_LCR = 0x0C;
static const uint32_t DAR_LCR_DLS = 0x03;
static const uint32_t DAR_LCR_DLS_5BITS = 0x00;
static const uint32_t DAR_LCR_DLS_6BITS = 0x01;
static const uint32_t DAR_LCR_DLS_7BITS = 0x02;
static const uint32_t DAR_LCR_DLS_8BITS = 0x03;
static const uint32_t DAR_LCR_STOP = 0x04;
static const uint32_t DAR_LCR_STOP_1BIT = 0x00;
static const uint32_t DAR_LCR_STOP_15_2BITS = 0x04;
static const uint32_t DAR_LCR_PEN = 0x08;
static const uint32_t DAR_LCR_EPS = 0x10;
static const uint32_t DAR_LCR_EPS_EVEN_SPACE = 0x10;
static const uint32_t DAR_LCR_EPS_ODD_MARK = 0x00;
static const uint32_t DAR_LCR_SP = 0x20;
static const uint32_t DAR_LCR_BREAK = 0x40;
static const uint32_t DAR_LCR_DLAB = 0x80;

static const ptrdiff_t DW_APB_REG_MCR = 0x10;
static const uint32_t DAR_MCR_DTR = 0x01;
static const uint32_t DAR_MCR_RTS = 0x02;
static const uint32_t DAR_MCR_OUT1 = 0x04;
static const uint32_t DAR_MCR_OUT2 = 0x08;
static const uint32_t DAR_MCR_LOOPBACK = 0x10;
static const uint32_t DAR_MCR_AFCE = 0x20;
static const uint32_t DAR_MCR_SIRE = 0x40;

static const ptrdiff_t DW_APB_REG_LSR = 0x14;
static const uint32_t DAR_LSR_DR = 0x01;
static const uint32_t DAR_LSR_OE = 0x02;
static const uint32_t DAR_LSR_PE = 0x04;
static const uint32_t DAR_LSR_FE = 0x08;
static const uint32_t DAR_LSR_BI = 0x10;
static const uint32_t DAR_LSR_THRE = 0x20;
static const uint32_t DAR_LSR_TEMT = 0x40;
static const uint32_t DAR_LSR_RFE = 0x80;

static const ptrdiff_t DW_APB_REG_MSR = 0x18;
static const ptrdiff_t DW_APB_REG_SCR = 0x1C;
static const ptrdiff_t DW_APB_REG_FAR = 0x70;
static const ptrdiff_t DW_APB_REG_USR = 0x7C;
static const uint32_t DAR_USR_BUSY = 0x01;
static const uint32_t DAR_USR_TFNF = 0x02;
static const uint32_t DAR_USR_TFE = 0x04;
static const uint32_t DAR_USR_RFNE = 0x08;
static const uint32_t DAR_USR_RFF = 0x10;

static const ptrdiff_t DW_APB_REG_TFL = 0x80;
static const ptrdiff_t DW_APB_REG_RFL = 0x84;
static const ptrdiff_t DW_APB_REG_SRR = 0x88;
static const uint32_t DAR_SRR_UR = 0x1;
static const uint32_t DAR_SRR_RFR = 0x2;
static const uint32_t DAR_SRR_XFR = 0x4;

static const ptrdiff_t DW_APB_REG_SRTS = 0x8C;
static const ptrdiff_t DW_APB_REG_SBCR = 0x90;
static const ptrdiff_t DW_APB_REG_SDMAM = 0x94;
static const ptrdiff_t DW_APB_REG_SFE = 0x98;
static const ptrdiff_t DW_APB_REG_SRT = 0x9C;
static const ptrdiff_t DW_APB_REG_STET = 0xA0;
static const ptrdiff_t DW_APB_REG_CPR = 0xF4;
static const ptrdiff_t DW_APB_REG_UCV = 0xF8;
static const ptrdiff_t DW_APB_REG_CTR = 0xFC;

void
mmwr32(void * const addr, const uint32_t v)
{
	*(volatile uint32_t * const)addr = v;
}

uint32_t
mmrd32(const void * const addr)
{
	return (*(volatile const uint32_t * const)addr);
}

#define	WR_REG(_b, _r, _v) \
	mmwr32((void *)((_b) + DW_APB_REG_##_r), (uint8_t)(_v))

#define	RD_REG(_b, _r) \
	mmrd32((void * const)((_b) + DW_APB_REG_##_r))

static uintptr_t
dw_apb_port_addr(const dw_apb_port_t port)
{
	switch (port) {
	case DAP_0:
		return (DW_APB_PORT_ADDRS[0]);
	case DAP_1:
		return (DW_APB_PORT_ADDRS[1]);
	case DAP_2:
		return (DW_APB_PORT_ADDRS[2]);
	case DAP_3:
		return (DW_APB_PORT_ADDRS[3]);
	default:
		return (0);
	}
}

static uint32_t
dw_apb_lcr(const async_databits_t db, const async_parity_t par,
    const async_stopbits_t sb)
{
	uint32_t lcr = 0;

	switch (sb) {
	case AS_1BIT:
		break;
	case AS_15BITS:
		if (db != AD_5BITS)
			return (DAR_INVALID);
		lcr |= DAR_LCR_STOP;
		break;
	case AS_2BITS:
		if (db == AD_5BITS)
			return (DAR_INVALID);
		lcr |= DAR_LCR_STOP;
		break;
	default:
		return (DAR_INVALID);
	}

	switch (db) {
	case AD_5BITS:
		lcr |= DAR_LCR_DLS_5BITS;
		break;
	case AD_6BITS:
		lcr |= DAR_LCR_DLS_6BITS;
		break;
	case AD_7BITS:
		lcr |= DAR_LCR_DLS_7BITS;
		break;
	case AD_8BITS:
		lcr |= DAR_LCR_DLS_8BITS;
		break;
	default:
		return (DAR_INVALID);
	}

	switch (par) {
	case AP_NONE:
		break;
	case AP_SPACE:
		lcr |= DAR_LCR_SP;
		/* FALLTHROUGH */
	case AP_EVEN:
		lcr |= DAR_LCR_EPS_EVEN_SPACE;
		lcr |= DAR_LCR_PEN;
		break;
	case AP_MARK:
		lcr |= DAR_LCR_SP;
		/* FALLTHROUGH */
	case AP_ODD:
		lcr |= DAR_LCR_PEN;
		break;
	default:
		return (DAR_INVALID);
	}

	return (lcr);
}

void *
dw_apb_uart_init(const dw_apb_port_t port, const uint32_t baud,
    const async_databits_t db, const async_parity_t par,
    const async_stopbits_t sb)
{
	/*
	 * XXX We should really get our clock from whatever controls it.  We
	 * may also want to do something sensible if the baud rate is inexact
	 * or unsatisfiable.
	 */
	const uint32_t divisor = 3000000 / baud;
	const uint32_t dlh = (divisor & 0xff00) >> 8;
	const uint32_t dll = (divisor & 0x00ff);
	const uintptr_t addr = dw_apb_port_addr(port);
	const uint32_t lcr = dw_apb_lcr(db, par, sb);
	void *regs;

	if (addr == 0)
		bop_panic("UART port %x invalid", (uint_t)port);

	if (lcr == DAR_INVALID)
		bop_panic("UART port configuration invalid");

	regs = (void *)kbm_valloc(MMU_PAGESIZE, MMU_PAGESIZE);
	kbm_map((uintptr_t)regs, addr, 0, PT_WRITABLE | PT_NOCACHE);

	WR_REG(regs, SRR, DAR_SRR_UR | DAR_SRR_RFR | DAR_SRR_XFR);
	WR_REG(regs, LCR, DAR_LCR_DLAB);
	WR_REG(regs, DLH, dlh);
	WR_REG(regs, DLL, dll);
	WR_REG(regs, LCR, lcr);

	WR_REG(regs, FCR, DAR_FCR_FIFOE | DAR_FCR_XFIFOR | DAR_FCR_RFIFOR |
	    DAR_FCR_DMAM | DAR_FCR_TET_QUARTER | DAR_FCR_RT_QUARTER);

	/*
	 * XXX We always enable automatic flow control, but we should really
	 * check with the IOMUX to determine whether this port supports it.
	 */
	WR_REG(regs, MCR, DAR_MCR_AFCE | DAR_MCR_OUT2 | DAR_MCR_RTS |
	    DAR_MCR_DTR);

	return (regs);
}

void
dw_apb_uart_flush(void *regs)
{
	WR_REG(regs, SRR, DAR_SRR_RFR | DAR_SRR_XFR);
}

size_t
dw_apb_uart_rx_nb(void *regs, uint8_t *dbuf, size_t len)
{
	size_t i;
	uint32_t lsr;
	uint32_t rbr;

	if (dbuf == NULL)
		return (0);

	for (lsr = RD_REG(regs, LSR), i = 0;
	    (lsr & DAR_LSR_DR) != 0 && i < len;
	    lsr = RD_REG(regs, LSR), ++i) {
		rbr = RD_REG(regs, RBR);
		dbuf[i] = rbr & 0xff;
	}

	return (i);
}

uint8_t
dw_apb_uart_rx_one(void *regs)
{
	uint8_t ch;

	while (dw_apb_uart_rx_nb(regs, &ch, 1) < 1)
		;

	return (ch);
}

size_t
dw_apb_uart_tx_nb(void *regs, const uint8_t *dbuf, size_t len)
{
	size_t i;
	uint32_t usr;

	for (usr = RD_REG(regs, USR), i = 0;
	    (usr & DAR_USR_TFNF) && i < len;
	    usr = RD_REG(regs, USR), ++i) {
		WR_REG(regs, THR, (uint32_t)dbuf[i]);
	}

	return (i);
}

void
dw_apb_uart_tx(void *regs, const uint8_t *dbuf, size_t len)
{
	while (len > 0) {
		size_t sent = dw_apb_uart_tx_nb(regs, dbuf, len);
		dbuf += sent;
		len -= sent;
	}
}

boolean_t
dw_apb_uart_dr(void *regs)
{
	return ((RD_REG(regs, LSR) & DAR_LSR_DR) != 0);
}
