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

/*
 * ipcc - interprocessor control channel
 *
 * ...
 */

#include <sys/byteorder.h>
#include <sys/ddi.h>
#include <sys/errno.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/systm.h>
#include <sys/types.h>

#include <sys/ipcc.h>
#include <sys/ipcc_impl.h>

/* Sequence number for requests */
static uint64_t ipcc_seq;

static uint8_t ipcc_msg[IPCC_MAX_MESSAGE_SIZE];
static uint8_t ipcc_pkt[IPCC_MAX_PACKET_SIZE];
static ipcc_panic_data_t ipcc_panic_buf;
static kmutex_t ipcc_mutex;

/*
 * This indicates that we are far enough through boot that it's safe
 * to use mutex_enter/exit and things such as timers.
 */
static bool ipcc_multithreaded;

#define	IPCC_LOCK if (ipcc_multithreaded) mutex_enter(&ipcc_mutex)
#define	IPCC_UNLOCK if (ipcc_multithreaded) mutex_exit(&ipcc_mutex)

void
ipcc_begin_multithreaded(void)
{
	VERIFY(!ipcc_multithreaded);
	mutex_init(&ipcc_mutex, NULL, MUTEX_DEFAULT, NULL);
	ipcc_multithreaded = 1;
}

static uint16_t
ipcc_fletcher16(uint8_t *buf, size_t len)
{
	uint16_t s1 = 0, s2 = 0;

	for (size_t i = 0; i < len; i++) {
		s1 = (s1 + buf[i]) % 0xff;
		s2 = (s2 + s1) % 0xff;
	}

	return ((s2 << 8) | s1);
}

static size_t
ipcc_cobs_encode(const uint8_t *ibuf, size_t bufl, uint8_t *obuf)
{
	size_t in = 0;
	size_t out = 1;
	size_t code_out = 0;
	uint8_t code = 1;

	for (in = 0; in < bufl; in++) {
		if (ibuf[in] == 0) {
			obuf[code_out] = code;
			code = 1;
			code_out = out++;
			continue;
		}

		obuf[out++] = ibuf[in];

		if (++code == 0xff) {
			obuf[code_out] = code;
			code = 1;
			code_out = out++;
		}
	}

	obuf[code_out] = code;

	return (out);
}

static size_t
ipcc_cobs_decode(const uint8_t *ibuf, size_t bufl, uint8_t *obuf)
{
	size_t in = 0;
	size_t out = 0;
	uint8_t code;

	for (in = 0; in < bufl; ) {
		code = ibuf[in];

		if (in + code > bufl && code != 1)
			return (0);

		in++;

		for (uint8_t i = 1; i < code; i++)
			obuf[out++] = ibuf[in++];

		if (code != 0xFF && in != bufl)
			obuf[out++] = '\0';
	}

	return (out);
}

static void
ipcc_encode_bytes(uint8_t *val, uint8_t cnt, uint8_t *buf, size_t *off)
{
#ifdef _LITTLE_ENDIAN
	bcopy(val, &buf[*off], cnt);
#else
#error ipcc driver needs porting for big-endian platforms
#endif
	*off += cnt;
}

static void
ipcc_decode_bytes(uint8_t *val, uint8_t cnt, uint8_t *buf, size_t *off)
{
#ifdef _LITTLE_ENDIAN
	bcopy(&buf[*off], val, cnt);
#else
#error ipcc driver needs porting for big-endian platforms
#endif
	*off += cnt;
}

static const char *
ipcc_failure_str(uint8_t reason)
{
	switch (reason) {
	case IPCC_DECODEFAIL_COBS:
		return "COBS";
	case IPCC_DECODEFAIL_CRC:
		return "CRC";
	case IPCC_DECODEFAIL_DESERIALIZE:
		return "DESERIALIZE";
	case IPCC_DECODEFAIL_MAGIC:
		return "MAGIC";
	case IPCC_DECODEFAIL_VERSION:
		return "VERSION";
	case IPCC_DECODEFAIL_SEQUENCE:
		return "SEQUENCE";
	case IPCC_DECODEFAIL_DATALEN:
		return "DATALEN";
	default:
		return "UNKNOWN";
	}
}

static int
ipcc_msg_init(uint8_t *buf, size_t len, size_t *off, ipcc_hss_cmd_t cmd)
{
	uint32_t ver = IPCC_VERSION;
	uint32_t magic = IPCC_MAGIC;

	if (len - *off < IPCC_MIN_PACKET_SIZE)
		return (ENOBUFS);

	/* Wrap if we have got to the reply namespace (top bit set) */
	if (++ipcc_seq & IPCC_SEQ_REPLY)
		ipcc_seq = 1;

	ipcc_encode_bytes((uint8_t *)&magic, sizeof (magic), buf, off);
	ipcc_encode_bytes((uint8_t *)&ver, sizeof (ver), buf, off);
	ipcc_encode_bytes((uint8_t *)&ipcc_seq, sizeof (ipcc_seq),
	    buf, off);
	ipcc_encode_bytes((uint8_t *)&cmd, sizeof (uint8_t), buf, off);

	return (0);
}

static int
ipcc_msg_fini(uint8_t *buf, size_t len, size_t *off)
{
	uint16_t crc;

	if (len - *off < sizeof (uint16_t))
		return (ENOBUFS);

	crc = ipcc_fletcher16(buf, *off);
	ipcc_encode_bytes((uint8_t *)&crc, sizeof (uint16_t), buf, off);

	return (0);
}

static ssize_t
ipcc_pkt_send(uint8_t *pkt, size_t len, const ipcc_ops_t *ops, void *arg)
{
	if (ops->io_flush != NULL)
		ops->io_flush(arg);

	while (len > 0) {
		ssize_t n;

		/* XXX - implement some kind of timeout? */
		if (ops->io_pollwrite != NULL) {
			while (!ops->io_pollwrite(arg))
				;
		}

		if ((n = ops->io_write(arg, pkt, len)) < 0)
			return (n);

		VERIFY3U(n, <=, len);

		pkt += n;
		len -= n;
	}

	return (0);
}

static ssize_t
ipcc_pkt_recv(uint8_t *pkt, size_t len, uint8_t **endp,
    const ipcc_ops_t *ops, void *arg)
{
	*endp = NULL;

	do {
		ssize_t n;

		/* XXX - implement some kind of timeout? */
		if (ops->io_pollread != NULL) {
			while (!ops->io_pollread(arg))
				;
		}

		if ((n = ops->io_read(arg, pkt, 1)) < 0)
			return (n);

		VERIFY3U(n, ==, 1);

		if (*pkt == 0) {
			*endp = pkt;
			return (0);
		}

		pkt += n;
		len -= n;
	} while (len > 0);

	return (ENOBUFS);
}

#define	IPCC_HEXCH(x) ((x) < 0xa ? (x) + '0' : (x) - 0xa + 'a')

static void
ipcc_loghex(const char *tag, const uint8_t *buf, size_t bufl,
    const ipcc_ops_t *ops, void *arg)
{
	bufl = MIN(bufl, 64);
	char obuf[bufl * 3 + 1];
	uint_t oi = 0;

	/* In early boot we do not have the likes of snprintf() */
	for (uint_t i = 0; i < bufl; i++) {
		obuf[oi++] = IPCC_HEXCH(buf[i] >> 4);
		obuf[oi++] = IPCC_HEXCH(buf[i] & 0xf);
		obuf[oi++] = ' ';
	}
	obuf[oi] = '\0';

	ops->io_log(arg, "%s: %s", tag, obuf);
}

#define	LOG(...) if (ops->io_log != NULL) ops->io_log(arg, __VA_ARGS__)
#define	LOGHEX(tag, buf, len) \
	if (ops->io_log != NULL) ipcc_loghex((tag), (buf), (len), ops, arg)

static int
ipcc_command_locked(const ipcc_ops_t *ops, void *arg,
    ipcc_hss_cmd_t cmd, ipcc_sp_cmd_t expected_rcmd,
    uint8_t *dataout, size_t dataoutl,
    uint8_t **datain, size_t *datainl)
{
	size_t off, pktl, rcvd_datal;
	uint64_t rcvd_seq;
	uint32_t rcvd_magic, rcvd_version;
	uint16_t rcvd_crc, crc;
	uint8_t rcvd_cmd, *end;
	uint8_t attempt = 0;
	int err = 0;

	if (ipcc_multithreaded) {
		ASSERT(MUTEX_HELD(&ipcc_mutex));
	}

resend:

	if (++attempt > IPCC_MAX_ATTEMPTS) {
		LOG("Maximum attempts exceeded");
		return (ETIMEDOUT);
	}

	LOG("\n-----------> Sending command 0x%x, attempt %u/%u", cmd, attempt,
	    IPCC_MAX_ATTEMPTS);

	off = 0;
	err = ipcc_msg_init(ipcc_msg, sizeof (ipcc_msg), &off, cmd);
	if (err != 0)
		return (err);

	if (dataout != NULL && dataoutl > 0) {
		if (sizeof (ipcc_msg) - off < dataoutl)
			return (ENOBUFS);
		ipcc_encode_bytes(dataout, dataoutl, ipcc_msg, &off);
		LOG("Additional data length: 0x%lx", dataoutl);
		LOGHEX("DATA OUT", dataout, dataoutl);
	}

	if ((err = ipcc_msg_fini(ipcc_msg, sizeof (ipcc_msg), &off)) != 0)
		return (err);

	if (IPCC_COBS_SIZE(off) > sizeof (ipcc_pkt) - 1)
		return (ENOBUFS);

	LOGHEX("     OUT", ipcc_msg, off);
	pktl = ipcc_cobs_encode(ipcc_msg, off, ipcc_pkt);
	LOGHEX("COBS OUT", ipcc_pkt, pktl);
	ipcc_pkt[pktl++] = 0;

	err = ipcc_pkt_send(ipcc_pkt, pktl, ops, arg);
	if (err != 0)
		return (err);

	if (expected_rcmd == IPCC_SP_NONE) {
		/* No response expected. */
		return (0);
	}

reread:

	err = ipcc_pkt_recv(ipcc_pkt, sizeof (ipcc_pkt), &end, ops, arg);
	if (err != 0)
		return (err);

	if (end == NULL) {
		LOG("Could not find frame terminator");
		goto resend;
	}

	if (end == ipcc_pkt) {
		LOG("Received frame terminator with no data");
		goto resend;
	}

	/* Decode the frame */
	LOGHEX(" COBS IN", ipcc_pkt, end - ipcc_pkt);
	pktl = ipcc_cobs_decode(ipcc_pkt, end - ipcc_pkt, ipcc_msg);
	if (pktl == 0) {
		LOG("Error decoding COBS frame");
		goto resend;
	}
	LOGHEX("      IN", ipcc_msg, pktl);
	if (pktl < IPCC_MIN_MESSAGE_SIZE) {
		LOG("Short message received - 0x%lx byte(s)", pktl);
		goto resend;
	}

	rcvd_datal = pktl - IPCC_MIN_MESSAGE_SIZE;
	LOG("Additional data length: 0x%lx", rcvd_datal);

	/* Validate checksum */
	off = pktl - 2;
	crc = ipcc_fletcher16(ipcc_msg, off);
	ipcc_decode_bytes((uint8_t *)&rcvd_crc, sizeof (rcvd_crc),
	    ipcc_msg, &off);

	if (crc != rcvd_crc) {
		LOG("Checksum mismatch got 0x%x calculated 0x%x",
		    rcvd_crc, crc);
		goto resend;
	}

	off = 0;
	ipcc_decode_bytes((uint8_t *)&rcvd_magic, sizeof (rcvd_magic),
	    ipcc_msg, &off);
	ipcc_decode_bytes((uint8_t *)&rcvd_version, sizeof (rcvd_version),
	    ipcc_msg, &off);
	ipcc_decode_bytes((uint8_t *)&rcvd_seq, sizeof (rcvd_seq),
	    ipcc_msg, &off);
	ipcc_decode_bytes((uint8_t *)&rcvd_cmd, sizeof (rcvd_cmd),
	    ipcc_msg, &off);

	if (rcvd_magic != IPCC_MAGIC) {
		LOG("Invalid magic number in response, 0x%x", rcvd_magic);
		goto resend;
	}
	if (rcvd_version != IPCC_VERSION) {
		LOG("Invalid version field in response, 0x%x", rcvd_version);
		goto resend;
	}
	if (!(rcvd_seq & IPCC_SEQ_REPLY)) {
		LOG("Response not a reply (sequence 0x%016lx)", rcvd_seq);
		goto resend;
	}
	if (rcvd_cmd == IPCC_SP_DECODEFAIL && rcvd_seq == 0xffffffffffffffff) {
		LOG("Decode failed, sequence ignored.");
	} else {
		rcvd_seq &= IPCC_SEQ_MASK;
		if (rcvd_seq != ipcc_seq) {
			LOG("Incorrect sequence in response "
			    "(0x%lx) vs expected (0x%lx)", rcvd_seq, ipcc_seq);
			/*
			 * If we've received an old sequence number from the SP
			 * in an otherwise valid packet, then we may be out of
			 * sync. Read again - XXX ponder solutions.
			 */
			if (rcvd_seq < ipcc_seq)
				goto reread;
			goto resend;
		}
	}
	if (rcvd_cmd == IPCC_SP_DECODEFAIL) {
		if (rcvd_datal != 1) {
			LOG("SP failed to decode packet (no reason sent)");
		} else {
			uint8_t dfreason;

			ipcc_decode_bytes(&dfreason, sizeof (dfreason),
			    ipcc_msg, &off);

			LOG("SP failed to decode packet (reason 0x%x - %s)",
			    dfreason, ipcc_failure_str(dfreason));
		}
		goto resend;
	}
	if (rcvd_cmd != expected_rcmd) {
		LOG("Incorrect reply cmd: got 0x%x, expected 0x%x",
		    rcvd_cmd, expected_rcmd);
		goto resend;
	}

	if (datainl != NULL && *datainl > 0 && *datainl != rcvd_datal) {
		LOG("Incorrect data length in reply - got 0x%lx expected 0x%lx",
		    rcvd_datal, *datainl);
		/*
		 * Given all of the other checks have passed, and this looks
		 * like a valid message, there is probably no benefit it
		 * re-attempting the request...
		 */
		return (ENOMEM);
	}

	if (rcvd_datal > 0) {
		LOGHEX(" DATA IN", ipcc_msg + off, rcvd_datal);

		if (datain == NULL || datainl == NULL) {
			LOG("No storage provided for incoming data - "
			    "received 0x%lx byte(s)", rcvd_datal);
			return (ENOMEM);
		}

		*datain = ipcc_msg + off;
		*datainl = rcvd_datal;
	} else {
		if (datain != NULL)
			*datain = NULL;
		if (datainl != NULL)
			*datainl = 0;
	}

	return (err);
}

static int
ipcc_command(const ipcc_ops_t *ops, void *arg,
    ipcc_hss_cmd_t cmd, ipcc_sp_cmd_t expected_rcmd,
    uint8_t *dataout, size_t dataoutl,
    uint8_t **datain, size_t *datainl)
{
	int err;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, cmd, expected_rcmd,
	    dataout, dataoutl, datain, datainl);
	IPCC_UNLOCK;

	return (err);
}

int
ipcc_reboot(const ipcc_ops_t *ops, void *arg)
{
	return (ipcc_command(ops, arg, IPCC_HSS_REBOOT, IPCC_SP_NONE,
	    NULL, 0, NULL, NULL));
}

int
ipcc_poweroff(const ipcc_ops_t *ops, void *arg)
{
	return (ipcc_command(ops, arg, IPCC_HSS_POWEROFF, IPCC_SP_NONE,
	    NULL, 0, NULL, NULL));
}

int
ipcc_bsu(const ipcc_ops_t *ops, void *arg, uint8_t *bsu)
{
	uint8_t *data;
	size_t datal = IPCC_BSU_DATALEN;
	int err = 0;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, IPCC_HSS_BSU, IPCC_SP_BSU,
	    NULL, 0, &data, &datal);

	if (err != 0)
		goto out;

	*bsu = *data;

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_ident(const ipcc_ops_t *ops, void *arg, ipcc_ident_t *ident)
{
	uint8_t *data;
	size_t datal = IPCC_IDENT_DATALEN;
	size_t off;
	int err = 0;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, IPCC_HSS_IDENT, IPCC_SP_IDENT,
	    NULL, 0, &data, &datal);

	if (err != 0)
		goto out;

	bzero(ident, sizeof (*ident));
	off = 0;
	ipcc_decode_bytes((uint8_t *)&ident->ii_model, sizeof (ident->ii_model),
	    data, &off);
	ipcc_decode_bytes((uint8_t *)&ident->ii_rev, sizeof (ident->ii_rev),
	    data, &off);
	ipcc_decode_bytes((uint8_t *)&ident->ii_serial,
	    sizeof (ident->ii_serial), data, &off);

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_macs(const ipcc_ops_t *ops, void *arg, ipcc_mac_t *mac)
{
	uint8_t *data;
	size_t datal = IPCC_MAC_DATALEN;
	size_t off;
	int err = 0;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, IPCC_HSS_MACS, IPCC_SP_MACS,
	    NULL, 0, &data, &datal);

	if (err != 0)
		goto out;

	bzero(mac, sizeof (*mac));
	off = 0;
	ipcc_decode_bytes((uint8_t *)&mac->im_base, sizeof (mac->im_base),
	    data, &off);
	ipcc_decode_bytes((uint8_t *)&mac->im_count, sizeof (mac->im_count),
	    data, &off);
	ipcc_decode_bytes((uint8_t *)&mac->im_stride, sizeof (mac->im_stride),
	    data, &off);

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_rot(const ipcc_ops_t *ops, void *arg, ipcc_rot_t *rot)
{
	int err = 0;
	uint8_t *data;
	size_t datal = 0;

	IPCC_LOCK;

	err = ipcc_command_locked(ops, arg, IPCC_HSS_ROT, IPCC_SP_ROT,
	    rot->ir_data, rot->ir_len, &data, &datal);

	if (err != 0)
		goto out;

	if (datal > sizeof (rot->ir_data)) {
		LOG("Too much data in RoT response - got 0x%lx bytes", datal);
		err = ENOMEM;
		goto out;
	}

	rot->ir_len = datal;
	bcopy(data, rot->ir_data, datal);

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_bootfail(const ipcc_ops_t *ops, void *arg, uint8_t reason)
{
	return (ipcc_command(ops, arg, IPCC_HSS_BOOTFAIL, IPCC_SP_ACK,
	    &reason, sizeof (reason), NULL, NULL));
}

int
ipcc_status(const ipcc_ops_t *ops, void *arg, uint64_t *status)
{
	uint8_t *data;
	size_t datal = IPCC_STATUS_DATALEN;
	size_t off;
	int err = 0;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, IPCC_HSS_STATUS, IPCC_SP_STATUS,
	    NULL, 0, &data, &datal);

	if (err != 0)
		goto out;

	off = 0;
	ipcc_decode_bytes((uint8_t *)status, sizeof (*status), data, &off);

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_setstatus(const ipcc_ops_t *ops, void *arg, uint64_t mask,
    uint64_t *status)
{
	uint8_t *data;
	size_t datal = IPCC_STATUS_DATALEN;
	size_t off;
	int err = 0;

	IPCC_LOCK;
	err = ipcc_command_locked(ops, arg, IPCC_HSS_SETSTATUS, IPCC_SP_STATUS,
	    (uint8_t *)&mask, sizeof (mask), &data, &datal);

	if (err != 0)
		goto out;

	off = 0;
	ipcc_decode_bytes((uint8_t *)status, sizeof (*status), data, &off);

out:
	IPCC_UNLOCK;
	return (err);
}

int
ipcc_panic(const ipcc_ops_t *ops, void *arg)
{
	return (ipcc_command(ops, arg, IPCC_HSS_PANIC, IPCC_SP_ACK,
	    (uint8_t *)&ipcc_panic_buf,
	    MIN(sizeof (ipcc_panic_buf), IPCC_MAX_DATA_SIZE), NULL, NULL));
}

void
ipcc_panic_field(ipcc_panic_field_t type, uint64_t val)
{
	switch (type) {
	case IPF_CAUSE:
		ipcc_panic_buf.ip_cause = val & 0xffff;
		break;
	case IPF_ERROR:
		ipcc_panic_buf.ip_error = val & 0xffff;
		break;
	case IPF_INSTR_PTR:
		ipcc_panic_buf.ip_instr_ptr = val;
		break;
	case IPF_CODE_SEG:
		ipcc_panic_buf.ip_code_seg = val & 0xffff;
		break;
	case IPF_FLAGS_REG:
		ipcc_panic_buf.ip_flags_reg = val;
		break;
	case IPF_STACK_PTR:
		ipcc_panic_buf.ip_stack_ptr = val;
		break;
	case IPF_STACK_SEG:
		ipcc_panic_buf.ip_stack_seg = val & 0xffff;
		break;
	case IPF_CR2:
		ipcc_panic_buf.ip_cr2 = val;
		break;
	}
}

void
ipcc_panic_vmessage(const char *fmt, va_list ap)
{
	(void) vsnprintf((char *)ipcc_panic_buf.ip_message,
	    sizeof (ipcc_panic_buf.ip_message), fmt, ap);
}

void
ipcc_panic_stack(uint64_t addr, const char *sym)
{

	if (ipcc_panic_buf.ip_stackidx >= IPCC_PANIC_STACKS)
		return;
	ipcc_panic_buf.ip_stack[ipcc_panic_buf.ip_stackidx].ips_offset = addr;
	if (sym != NULL) {
		bcopy((char *)sym,
		    ipcc_panic_buf.ip_stack[ipcc_panic_buf.ip_stackidx]
		    .ips_symbol, MIN(IPCC_PANIC_SYMLEN, strlen(sym)));
	}
	ipcc_panic_buf.ip_stackidx++;
}

void
ipcc_panic_vdata(const char *fmt, va_list ap)
{
	size_t datalen, space;

	space = sizeof (ipcc_panic_buf.ip_data) - ipcc_panic_buf.ip_dataidx;
	if (space == 0)
		return;

	datalen = vsnprintf((char *)ipcc_panic_buf.ip_data +
	    ipcc_panic_buf.ip_dataidx, space, fmt, ap);

	if (datalen <= space)
		ipcc_panic_buf.ip_dataidx += datalen;
	else
		ipcc_panic_buf.ip_dataidx = sizeof (ipcc_panic_buf.ip_data);
}

void
ipcc_panic_data(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ipcc_panic_vdata(fmt, ap);
	va_end(ap);
}

/*
ipcc_alert(const ipcc_ops_t *ops, void *arg,
ipcc_measurements(const ipcc_ops_t *ops, void *arg,
ipcc_imageblock(const ipcc_ops_t *ops, void *arg,
*/

