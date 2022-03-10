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
 * Copyright 2022 by Oxide Computer Company
 */

#ifndef	_SYS_SPI_H
#define	_SYS_SPI_H

#ifdef _KERNEL
#include <sys/types32.h>
#else
#include <stdint.h>
#endif

#ifdef	__cplusplus
extern "C" {
#endif

/* spidev ioctl */
#define	SPIDEV_IOC	(('s' << 24) | ('p' << 16) | ('i' << 8))

/* Perform a sequence of SPI transfers */
#define	SPIDEV_TRANSACTION	(SPIDEV_IOC | 0)

typedef struct spidev_transfer {
	/*
	 * Data to be written and read respectively.  Set to NULL if no data is
	 * to be transferred in that direction.  If both are non-NULL, a
	 * bidirectional transfer is performed where, on each clock, one bit is
	 * simultaneously transmitted from tx_buf and received in rx_buf.
	 */
	const uint8_t	*tx_buf;
	uint8_t		*rx_buf;

	/* size of TX and RX buffers (in bytes) */
	uint32_t	len;

	/*
	 * Delay introduced after this transfer but before the next
	 * transfer or completion of transaction.
	 */
	uint16_t	delay_usec;

	/* When non-zero, de-assert chip select at end of this transfer. */
	uint8_t		deassert_cs;
} spidev_transfer_t;

#ifdef _KERNEL
typedef struct spidev_transfer32 {
	caddr32_t	tx_buf;
	caddr32_t	rx_buf;

	uint32_t	len;
	uint16_t	delay_usec;
	uint8_t		deassert_cs;
} spidev_transfer32_t;
#endif

typedef struct spidev_transaction {
	spidev_transfer_t	*spidev_xfers;
	uint8_t			spidev_nxfers;
} spidev_transaction_t;

#ifdef _KERNEL
typedef struct spidev_transaction32 {
	caddr32_t	spidev_xfers;
	uint8_t		spidev_nxfers;
} spidev_transaction32_t;
#endif

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SPI_H */
