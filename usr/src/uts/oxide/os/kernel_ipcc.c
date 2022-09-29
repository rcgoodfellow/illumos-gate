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
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/boot_debug.h>
#include <sys/prom_debug.h>
#include <sys/cmn_err.h>
#include <sys/dw_apb_uart.h>
#include <sys/file.h>
#include <sys/ipcc_impl.h>
#include <sys/kernel_ipcc.h>
#include <sys/stdbool.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/systm.h>
#include <sys/uart.h>
#include <vm/kboot_mmu.h>
#include <sys/io/fch/iomux.h>
#include <sys/io/fch/uart.h>

static ipcc_ops_t kernel_ipcc_ops;
static void *kernel_ipcc_arg;
static ipcc_init_t ipcc_init = IPCC_INIT_UNSET;

/*
 * Functions used for IPCC in early boot, using early boot pages before VM
 * is set up. These can only be used until release_bootstrap() is called from
 * main().
 */

static void *eb_ipcc_uart_regs;

static bool
eb_ipcc_pollread(void *regs)
{
	return (dw_apb_uart_dr(regs));
}

static void
eb_ipcc_flush(void *regs)
{
	dw_apb_uart_flush(regs);
}

static off_t
eb_ipcc_read(void *regs, uint8_t *buf, size_t len)
{
	*buf = dw_apb_uart_rx_one(regs);
	return (1);
}

static off_t
eb_ipcc_write(void *regs, uint8_t *buf, size_t len)
{
	dw_apb_uart_tx(regs, buf, len);
	return (len);
}

static void
eb_ipcc_log(void *arg __unused, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	eb_vprintf(fmt, ap);
	eb_printf("\n");
	va_end(ap);
}

static void
eb_ipcc_init(void)
{
	DBG_MSG("kernel_ipcc_init(EARLYBOOT)\n");

	/*
	 * XXX - use more FCH framework? Fake up a mmio_reg_block_t?
	 *
	 * The default mappings for IOMUX pins relating to UART1 are shown
	 * below. Conveniently, setting the value for each pin to 0 results
	 * in the function shown in square brackets, which is what we want.
	 *
	 *  0x8c - GPIO_140	[UART1_CTS_L]
	 *  0x8d - UART1_RXD	[UART1_RXD]
	 *  0x8e - GPIO_142	[UART1_RTS_L]
	 *  0x8f - GPIO_143	[UART1_TXD]
	 */
	const uintptr_t addr = FCH_RELOCATABLE_PHYS_BASE;
	void *regs = (void *)kbm_valloc(MMU_PAGESIZE, MMU_PAGESIZE);
	kbm_map((uintptr_t)regs, addr, 0, PT_WRITABLE | PT_NOCACHE);
	for (uint_t i = 0x8c; i <= 0x8f; i++) {
		uint8_t b, a;

		b = *(volatile const uint8_t * const)(regs + FCH_IOMUX_OFF + i);
		*(volatile uint8_t * const)(regs + FCH_IOMUX_OFF + i) = 0;
		a = *(volatile const uint8_t * const)(regs + FCH_IOMUX_OFF + i);
		DBG_MSG("Pin 0x%x: %x -> %x\n", i, b, a);
	}
	kbm_unmap((uintptr_t)regs);

	eb_ipcc_uart_regs = dw_apb_uart_init(DAP_1, 3000000,
	    AD_8BITS, AP_NONE, AS_1BIT);

	if (eb_ipcc_uart_regs == NULL)
		return;	/* XXX panic? */

	bzero(&kernel_ipcc_ops, sizeof (kernel_ipcc_ops));
	kernel_ipcc_ops.io_pollread = eb_ipcc_pollread;
	//kernel_ipcc_ops.io_pollwrite = eb_ipcc_pollwrite;
	kernel_ipcc_ops.io_flush = eb_ipcc_flush;
	kernel_ipcc_ops.io_read = eb_ipcc_read;
	kernel_ipcc_ops.io_write = eb_ipcc_write;
	kernel_ipcc_ops.io_log = eb_ipcc_log;

	kernel_ipcc_arg = eb_ipcc_uart_regs;
}

/*
 * Functions used for IPCC in mid boot, after KVM has been initialised but
 * before the STREAMS subsystem and UART drivers are loaded.
 */

typedef struct {
	mmio_reg_block_t	imbd_reg_block;
	mmio_reg_t		imbd_reg_thr;
	mmio_reg_t		imbd_reg_rbr;
	mmio_reg_t		imbd_reg_lsr;
	mmio_reg_t		imbd_reg_usr;
	mmio_reg_t		imbd_reg_srr;
} ipcc_mb_data_t;

static ipcc_mb_data_t ipcc_mb_data;

static void
mb_ipcc_flush(void *arg)
{
	ipcc_mb_data_t *dat = (ipcc_mb_data_t *)arg;
	uint32_t v = 0;

	v = FCH_UART_SRR_SET_XFR(v, 1);
	v = FCH_UART_SRR_SET_RFR(v, 1);
	mmio_reg_write(dat->imbd_reg_srr, v);
}

static bool
mb_ipcc_pollread(void *arg)
{
	ipcc_mb_data_t *dat = (ipcc_mb_data_t *)arg;
	uint32_t lsr;

	lsr = mmio_reg_read(dat->imbd_reg_lsr);
	/* Data Ready */
	return (FCH_UART_LSR_GET_DR(lsr) != 0);
}

static off_t
mb_ipcc_read(void *arg, uint8_t *buf, size_t len)
{
	ipcc_mb_data_t *dat = (ipcc_mb_data_t *)arg;

	/* Wait until there is data available */
	while (!mb_ipcc_pollread(dat))
		;
	*buf = mmio_reg_read(dat->imbd_reg_rbr);

	return (1);
}

static bool
mb_ipcc_pollwrite(void *arg)
{
	ipcc_mb_data_t *dat = (ipcc_mb_data_t *)arg;
	uint32_t usr;

	usr = mmio_reg_read(dat->imbd_reg_usr);
	/* Transmit FIFO Not Full */
	return (FCH_UART_USR_GET_TFNF(usr) != 0);
}

static off_t
mb_ipcc_write(void *arg, uint8_t *buf, size_t len)
{
	ipcc_mb_data_t *dat = (ipcc_mb_data_t *)arg;
	size_t towrite = len;

	while (towrite > 0) {
		size_t l;

		/* Wait until there is room in the FIFO */
		while (!mb_ipcc_pollwrite(dat))
			;

		mmio_reg_write(dat->imbd_reg_thr, *buf);
		buf++;
		towrite--;
	}
	return (len);
}

static void
mb_ipcc_log(void *arg __unused, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vcmn_err(CE_CONT, fmt, ap);
	va_end(ap);
}

static void
mb_ipcc_init(void)
{
	PRM_POINT("kernel_ipcc_init(MIDBOOT)");

	/*
	 * When switching from EB to MB, the UART is already configured
	 * appropriately, we just need to map the registers that we'll
	 * need in this next phase.
	 */

	ipcc_mb_data_t *d = &ipcc_mb_data;
	d->imbd_reg_block = huashan_uart_mmio_block(1);
	d->imbd_reg_thr = FCH_UART_THR_MMIO(d->imbd_reg_block);
	d->imbd_reg_rbr = FCH_UART_RBR_MMIO(d->imbd_reg_block);
	d->imbd_reg_lsr = FCH_UART_LSR_MMIO(d->imbd_reg_block);
	d->imbd_reg_usr = FCH_UART_USR_MMIO(d->imbd_reg_block);
	d->imbd_reg_srr = FCH_UART_SRR_MMIO(d->imbd_reg_block);

	bzero(&kernel_ipcc_ops, sizeof (kernel_ipcc_ops));
	kernel_ipcc_ops.io_pollread = mb_ipcc_pollread;
	kernel_ipcc_ops.io_pollwrite = mb_ipcc_pollwrite;
	kernel_ipcc_ops.io_flush = mb_ipcc_flush;
	kernel_ipcc_ops.io_read = mb_ipcc_read;
	kernel_ipcc_ops.io_write = mb_ipcc_write;
	kernel_ipcc_ops.io_log = mb_ipcc_log;

	kernel_ipcc_arg = &ipcc_mb_data;

	ipcc_begin_multithreaded();
}


/*
 * Functions used for IPCC after STREAMS and the device tree are avaialble.
 */

typedef struct {
	bool		ilbd_init;
	kmutex_t	ilbd_mutex;
	ldi_handle_t	ilbd_ldih;
	ldi_ident_t	ilbd_ldiid;
} ipcc_lb_data_t;

static ipcc_lb_data_t ipcc_lb_data;

static int
lb_ipcc_start(void)
{
	int err = 0;

	mutex_enter(&ipcc_lb_data.ilbd_mutex);

	if (ipcc_lb_data.ilbd_init)
		goto out;

	ipcc_lb_data.ilbd_ldiid = ldi_ident_from_anon();

	err = ldi_open_by_name(IPCC_DEV, FEXCL | FREAD | FWRITE, kcred,
	    &ipcc_lb_data.ilbd_ldih, ipcc_lb_data.ilbd_ldiid);

	if (err != 0) {
		ldi_ident_release(ipcc_lb_data.ilbd_ldiid);

		cmn_err(CE_WARN, "kernel ipcc: Failed to open '%s', error %d",
		    IPCC_DEV, err);
	} else {
		ipcc_lb_data.ilbd_init = true;
	}

out:
	if (err != 0)
		mutex_exit(&ipcc_lb_data.ilbd_mutex);

	return (err);
}

static void
lb_ipcc_fini(void)
{
	prom_printf("[%s]\n", __func__);
	mutex_exit(&ipcc_lb_data.ilbd_mutex);
}

static int
lb_ipcc_call(int cmd, intptr_t arg)
{
	int rv;

	ASSERT(MUTEX_HELD(&ipcc_lb_data.ilbd_mutex));

	prom_printf("[%s/%x]\n", __func__, cmd);

	return (ldi_ioctl(ipcc_lb_data.ilbd_ldih, cmd, arg,
	    FKIOCTL, kcred, &rv));
}

static void
lb_ipcc_init(void)
{
	int err;

	PRM_POINT("kernel_ipcc_init(LATEBOOT)");

	mutex_init(&ipcc_lb_data.ilbd_mutex, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * Leave the existing ops vector intact for now.. XXX
	 */
	/*
	mmio_reg_block_unmap(ipcc_reg_block);
	bzero(&kernel_ipcc_ops, sizeof (kernel_ipcc_ops));
	*/
}

/*
 * Entry points
 */

void
kernel_ipcc_init(ipcc_init_t stage)
{
	switch (stage) {
	case IPCC_INIT_EARLYBOOT:
		VERIFY3U(ipcc_init, ==, IPCC_INIT_UNSET);
		eb_ipcc_init();
		break;
	case IPCC_INIT_KVMAVAIL:
		VERIFY3U(ipcc_init, ==, IPCC_INIT_EARLYBOOT);
		mb_ipcc_init();
		break;
	case IPCC_INIT_DEVTREE:
		VERIFY3U(ipcc_init, ==, IPCC_INIT_KVMAVAIL);
		lb_ipcc_init();
		break;
	default:
		break;
	}

	ipcc_init = stage;
}

void
kernel_ipcc_reboot(void)
{
	if (ipcc_init == IPCC_INIT_DEVTREE) {
		if (lb_ipcc_start() == 0) {
			(void) lb_ipcc_call(IPCC_REBOOT, (intptr_t)NULL);
			lb_ipcc_fini();
			return;
		}
		/*
		 * If start fails then fall back to driving the UART directly
		 * to get the message across.
		 */
	}
	(void) ipcc_reboot(&kernel_ipcc_ops, kernel_ipcc_arg);
}

void
kernel_ipcc_poweroff(void)
{
	if (ipcc_init == IPCC_INIT_DEVTREE) {
		if (lb_ipcc_start() == 0) {
			(void) lb_ipcc_call(IPCC_POWEROFF, (intptr_t)NULL);
			lb_ipcc_fini();
			return;
		}
		/*
		 * If start fails then fall back to driving the UART directly
		 * to get the message across.
		 */
	}
	(void) ipcc_poweroff(&kernel_ipcc_ops, kernel_ipcc_arg);
}

int
kernel_ipcc_ident(ipcc_ident_t *ident)
{
	if (ipcc_init == IPCC_INIT_DEVTREE) {
		int ret;

		ret = lb_ipcc_start();
		if (ret == 0) {
			ret = lb_ipcc_call(IPCC_IDENT, (intptr_t)ident);
			lb_ipcc_fini();
		}
		return (ret);
	} else {
		return (ipcc_ident(&kernel_ipcc_ops, kernel_ipcc_arg, ident));
	}
}

void
kernel_ipcc_panic(void)
{
	(void) ipcc_panic(&kernel_ipcc_ops, kernel_ipcc_arg);
}

int
kernel_ipcc_bsu(uint8_t *bsu)
{
	VERIFY3U(ipcc_init, <, IPCC_INIT_DEVTREE);
	return (ipcc_bsu(&kernel_ipcc_ops, kernel_ipcc_arg, bsu));
}

int
kernel_ipcc_macs(ipcc_mac_t *mac)
{
	VERIFY3U(ipcc_init, <, IPCC_INIT_DEVTREE);
	return (ipcc_macs(&kernel_ipcc_ops, kernel_ipcc_arg, mac));
}

int
kernel_ipcc_status(uint64_t *status)
{
	VERIFY3U(ipcc_init, <, IPCC_INIT_DEVTREE);
	return (ipcc_status(&kernel_ipcc_ops, kernel_ipcc_arg, status));
}

int
kernel_ipcc_ackstart(void)
{
	VERIFY3U(ipcc_init, <, IPCC_INIT_DEVTREE);
	return (ipcc_ackstart(&kernel_ipcc_ops, kernel_ipcc_arg));
}
