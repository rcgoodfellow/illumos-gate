/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2022 Oxide Computer Company
 */

#ifndef	_IPCC_IMPL_H
#define	_IPCC_IMPL_H

#include <sys/stdbool.h>
#include <sys/ipcc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	IPCC_VERSION		1
#define	IPCC_MAGIC		0x1DE19CC

#define	IPCC_COBS_SIZE(x)	(1 + (x) + (x) / 0xfe)
#define	IPCC_MIN_PACKET_SIZE	IPCC_COBS_SIZE(IPCC_MIN_MESSAGE_SIZE)
#define	IPCC_MAX_PACKET_SIZE	IPCC_COBS_SIZE(IPCC_MAX_MESSAGE_SIZE)
#define	IPCC_MAX_ATTEMPTS	10

#define	IPCC_SEQ_MASK		0x7fffffffffffffffull
#define	IPCC_SEQ_REPLY		0x8000000000000000ull

typedef enum ipcc_hss_cmd {
	IPCC_HSS_REBOOT = 1,
	IPCC_HSS_POWEROFF,
	IPCC_HSS_BSU,
	IPCC_HSS_IDENT,
	IPCC_HSS_MACS,
	IPCC_HSS_BOOTFAIL,
	IPCC_HSS_PANIC,
	IPCC_HSS_STATUS,
	IPCC_HSS_SETSTATUS,
	IPCC_HSS_ALERT,
	IPCC_HSS_ROT,
	IPCC_HSS_ADD_MEASUREMENTS,
	IPCC_HSS_IMAGEBLOCK,
} ipcc_hss_cmd_t;

typedef enum ipcc_sp_cmd {
	IPCC_SP_NONE = 0,
	IPCC_SP_ACK,
	IPCC_SP_DECODEFAIL,
	IPCC_SP_BSU,
	IPCC_SP_IDENT,
	IPCC_SP_MACS,
	IPCC_SP_STATUS,
	IPCC_SP_ALERT,
	IPCC_SP_ROT,
	IPCC_SP_IMAGEBLOCK,
} ipcc_sp_cmd_t;

typedef enum ipcc_sp_decode_failure {
	IPCC_DECODEFAIL_COBS = 1,
	IPCC_DECODEFAIL_CRC,
	IPCC_DECODEFAIL_DESERIALIZE,
	IPCC_DECODEFAIL_MAGIC,
	IPCC_DECODEFAIL_VERSION,
	IPCC_DECODEFAIL_SEQUENCE,
	IPCC_DECODEFAIL_DATALEN,
} ipcc_sp_decode_failure_t;

typedef enum ipcc_sp_status {
	IPCC_STATUS_STARTED	= 1 << 0,
	IPCC_STATUS_ALERT	= 1 << 1,
} ipcc_sp_status_t;

/*
 * A structure to accumulate information relating to a system panic before
 * assembling and sending a panic message to the SP.
 */
#define	IPCC_PANIC_STACKS	0x10
#define	IPCC_PANIC_DATALEN	0x100
#define	IPCC_PANIC_SYMLEN	0x20
#define IPCC_PANIC_MSGLEN	0x80
#define	IPCC_PANIC_EARLYBOOT	0xeb

typedef struct ipcc_panic_stack {
	char			ips_symbol[IPCC_PANIC_SYMLEN];
	uint64_t		ips_offset;
} __packed ipcc_panic_stack_t;

typedef struct ipcc_panic_data {
	uint16_t		ip_cause;
	uint32_t		ip_error;
	uint64_t		ip_instr_ptr;
	uint16_t		ip_code_seg;
	uint64_t		ip_flags_reg;
	uint64_t		ip_stack_ptr;
	uint16_t		ip_stack_seg;
	uint64_t		ip_cr2;
	uint8_t			ip_stackidx;
	uint8_t			ip_message[IPCC_PANIC_MSGLEN];
	ipcc_panic_stack_t	ip_stack[IPCC_PANIC_STACKS];
	uint_t			ip_dataidx;
	uint8_t			ip_data[IPCC_PANIC_DATALEN];
} __packed ipcc_panic_data_t;

typedef enum ipcc_panic_field {
	IPF_CAUSE,
	IPF_ERROR,
	IPF_INSTR_PTR,
	IPF_CODE_SEG,
	IPF_FLAGS_REG,
	IPF_STACK_PTR,
	IPF_STACK_SEG,
	IPF_CR2,
} ipcc_panic_field_t;

#define	IPCC_IDENT_DATALEN	13
#define	IPCC_BSU_DATALEN	1
#define	IPCC_MAC_DATALEN	8
#define	IPCC_STATUS_DATALEN	8

typedef struct ipcc_ops {
	void (*io_flush)(void *);
	off_t (*io_read)(void *, uint8_t *, size_t);
	off_t (*io_write)(void *, uint8_t *, size_t);
	void (*io_log)(void *, const char *, ...);
	bool (*io_pollread)(void *);
	bool (*io_pollwrite)(void *);
	//bool (*io_read_interrupt)(void *);
	//void (*io_set_interrupt)(void *, bool);
} ipcc_ops_t;

extern void ipcc_begin_multithreaded(void);
extern int ipcc_reboot(const ipcc_ops_t *, void *);
extern int ipcc_poweroff(const ipcc_ops_t *, void *);
extern int ipcc_bsu(const ipcc_ops_t *, void *, uint8_t *);
extern int ipcc_ident(const ipcc_ops_t *, void *, ipcc_ident_t *);
extern int ipcc_macs(const ipcc_ops_t *ops, void *arg, ipcc_mac_t *);
extern int ipcc_rot(const ipcc_ops_t *, void *, ipcc_rot_t *);
extern int ipcc_bootfail(const ipcc_ops_t *ops, void *arg, uint8_t);
extern int ipcc_status(const ipcc_ops_t *ops, void *arg, uint64_t *);
extern int ipcc_setstatus(const ipcc_ops_t *ops, void *arg, uint64_t,
    uint64_t *);

extern void ipcc_panic_vmessage(const char *, va_list);
extern void ipcc_panic_field(ipcc_panic_field_t, uint64_t);
extern void ipcc_panic_stack(uint64_t, const char *);
extern void ipcc_panic_vdata(const char *, va_list);
extern void ipcc_panic_data(const char *, ...);
extern int ipcc_panic(const ipcc_ops_t *ops, void *arg);

/*
extern int ipcc_alert(ipcc_ops_t *ops, void *arg,
extern int ipcc_measurements(ipcc_ops_t *ops, void *arg,
extern int ipcc_imageblock(ipcc_ops_t *ops, void *arg,
*/

#ifdef __cplusplus
}
#endif

#endif	/* _IPCC_IMPL_H */
