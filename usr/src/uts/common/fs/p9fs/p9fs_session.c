

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/fs/p9fs_impl.h>


#define	PLAN9_TVERSION			100
#define	PLAN9_RVERSION			(PLAN9_TVERSION + 1)
#define	PLAN9_TAUTH			102
#define	PLAN9_RAUTH			(PLAN9_TAUTH + 1)
#define	PLAN9_TATTACH			104
#define	PLAN9_RATTACH			(PLAN9_TATTACH + 1)
#define	PLAN9_TERROR			106
#define	PLAN9_RERROR			(PLAN9_TERROR + 1)
#define	PLAN9_TWALK			110
#define	PLAN9_RWALK			(PLAN9_TWALK + 1)
#define	PLAN9_TOPEN			112
#define	PLAN9_ROPEN			(PLAN9_TOPEN + 1)
#define	PLAN9_TREAD			116
#define	PLAN9_RREAD			(PLAN9_TREAD + 1)
#define	PLAN9_TSTAT			124
#define	PLAN9_RSTAT			(PLAN9_TSTAT + 1)

#define	TAG_NOTAG			0xFFFF

#define	MODE_OREAD			0
#define	MODE_OWRITE			1
#define	MODE_ORDWR			2
#define	MODE_OEXEC			3
#define	MODE_OTRUNC			0x10
#define	MODE_ORCLOSE			0x40


typedef struct qid {
	uint8_t qid_type;
	uint32_t qid_version;
	uint64_t qid_path;
} qid_t;

typedef struct reqbuf {
	unsigned rb_error;
	uint8_t *rb_data;
	size_t rb_capacity;
	size_t rb_pos;
	size_t rb_limit;
} reqbuf_t;

/*
 * Reset the buffer to allow the assembly of a new message.
 */
void
reqbuf_reset(reqbuf_t *rb)
{
	rb->rb_error = 0;

	/*
	 * Skip the size[4] field to begin with.  It will get updated when we
	 * flip the buffer later.
	 */
	rb->rb_pos = 4;

	rb->rb_limit = rb->rb_capacity;
	bzero(rb->rb_data, rb->rb_capacity);
}

/*
 * How many bytes are left in the occupied portion of the buffer?
 */
size_t
reqbuf_remainder(reqbuf_t *rb)
{
	if (rb->rb_error != 0) {
		return (0);
	}

	if (rb->rb_pos > rb->rb_limit) {
		return (0);
	}

	return (rb->rb_limit - rb->rb_pos);
}

/*
 * Flip the buffer so that we can send it over the transport.  Update the
 * length prefix based on how much data was assembled in the buffer before the
 * flip.
 */
void
reqbuf_flip(reqbuf_t *rb)
{
	uint32_t size = rb->rb_pos;
	bcopy(&size, rb->rb_data, sizeof (size));

	rb->rb_limit = rb->rb_pos;
	rb->rb_pos = 0;
}

/*
 * Trim the limit to reflect the portion of the buffer we have actually
 * written.
 */
void
reqbuf_trim(reqbuf_t *rb, size_t len)
{
	if (len < rb->rb_limit) {
		rb->rb_limit = len;
	}
}

void
reqbuf_get_bcopy(reqbuf_t *rb, void *target, size_t nbytes)
{
	if (rb->rb_error != 0) {
		return;
	}

	if (nbytes > rb->rb_limit - rb->rb_pos) {
		rb->rb_error = 1;
		return;
	}

	bcopy(&rb->rb_data[rb->rb_pos], target, nbytes);
	rb->rb_pos += nbytes;
}

uint64_t
reqbuf_get_u(reqbuf_t *rb, size_t nbytes)
{
	if (rb->rb_error != 0) {
		return (0);
	}

	if (nbytes > rb->rb_limit - rb->rb_pos) {
		rb->rb_error = 1;
		return (0);
	}

	uint64_t out;
	if (nbytes == 1) {
		uint8_t val;
		bcopy(&rb->rb_data[rb->rb_pos], &val, nbytes);
		out = val;
	} else if (nbytes == 2) {
		uint16_t val;
		bcopy(&rb->rb_data[rb->rb_pos], &val, nbytes);
		out = val;
	} else if (nbytes == 4) {
		uint32_t val;
		bcopy(&rb->rb_data[rb->rb_pos], &val, nbytes);
		out = val;
	} else if (nbytes == 8) {
		uint64_t val;
		bcopy(&rb->rb_data[rb->rb_pos], &val, nbytes);
		out = val;
	} else {
		rb->rb_error = 1;
		return (0);
	}

	rb->rb_pos += nbytes;
	return (out);
}

uint8_t
reqbuf_get_u8(reqbuf_t *rb)
{
	uint8_t val;
	reqbuf_get_bcopy(rb, &val, sizeof (uint8_t));
	return (val);
}

uint16_t
reqbuf_get_u16(reqbuf_t *rb)
{
	uint16_t val;
	reqbuf_get_bcopy(rb, &val, sizeof (uint16_t));
	return (val);
}

uint32_t
reqbuf_get_u32(reqbuf_t *rb)
{
	uint32_t val;
	reqbuf_get_bcopy(rb, &val, sizeof (uint32_t));
	return (val);
}

uint64_t
reqbuf_get_u64(reqbuf_t *rb)
{
	uint64_t val;
	reqbuf_get_bcopy(rb, &val, sizeof (uint64_t));
	return (val);
}

qid_t *
reqbuf_get_qid(reqbuf_t *rb)
{
	if (rb->rb_error != 0) {
		return (NULL);
	}

	qid_t *qid;
	if ((qid = kmem_zalloc(sizeof (*qid), KM_SLEEP)) == NULL) {
		rb->rb_error = 1;
		return (NULL);
	}

	qid->qid_type = reqbuf_get_u8(rb);
	qid->qid_version = reqbuf_get_u32(rb);
	qid->qid_path = reqbuf_get_u64(rb);

	if (rb->rb_error != 0) {
		kmem_free(qid, sizeof (*qid));
		return (NULL);
	}

	return (qid);
}

void
reqbuf_strfree(char *s)
{
	if (s != NULL) {
		strfree(s);
	}
}

char *
reqbuf_get_str(reqbuf_t *rb)
{
	uint16_t len = reqbuf_get_u16(rb);
	if (rb->rb_error != 0) {
		return (NULL);
	}

	char *out;
	if ((out = kmem_zalloc(len + 1, KM_SLEEP)) == NULL) {
		rb->rb_error = 1;
		return (NULL);
	}

	reqbuf_get_bcopy(rb, out, len);
	if (rb->rb_error != 0) {
		/*
		 * XXX This will break if there are embedded NULs.
		 */
		strfree(out);
		return (NULL);
	}

	return (out);
}

/*
 * Read an entire message from the virtio transport into this buffer.  The
 * contents of the buffer will be completely destroyed, the position will be
 * reset, and the limit will reflect the quantity of data read.
 */
int
reqbuf_read(reqbuf_t *rb, ldi_handle_t lh)
{
	rb->rb_error = 0;
	rb->rb_pos = 0;

	struct uio uio;
	struct iovec iov;
	int e;
	size_t rem = rb->rb_capacity;

	bzero(&uio, sizeof (uio));
	bzero(&iov, sizeof (iov));
	iov.iov_base = (char *)rb->rb_data;
	iov.iov_len = rem;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = 0;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = rem;

	e = ldi_read(lh, &uio, kcred);
	rb->rb_limit = rem - uio.uio_resid;
	return (e);
}

/*
 * Write an entire message into the transport.  Buffer should be flipped before
 * write.  Data from the position to the limit will be written.  The position
 * will be advanced by the size of the actual write.
 */
int
reqbuf_write(reqbuf_t *rb, ldi_handle_t lh)
{
	if (rb->rb_error != 0) {
		return (EINVAL);
	}

	struct uio uio;
	struct iovec iov;
	size_t rem = rb->rb_limit - rb->rb_pos;
	int e;

	bzero(&uio, sizeof (uio));
	bzero(&iov, sizeof (iov));
	iov.iov_base = (char *)&rb->rb_data[rb->rb_pos];
	iov.iov_len = rem;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = 0;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = rem;

	e = ldi_write(lh, &uio, kcred);
	rb->rb_pos += rem - uio.uio_resid;
	return (e);
}

void
reqbuf_append_bcopy(reqbuf_t *rb, const void *data, size_t len)
{
	if (rb->rb_error != 0) {
		return;
	}

	if (rb->rb_limit - rb->rb_pos < len) {
		rb->rb_error = 1;
		return;
	}

	bcopy(data, &rb->rb_data[rb->rb_pos], len);
	rb->rb_pos += len;
}

void
reqbuf_append_u64(reqbuf_t *rb, uint64_t val)
{
	reqbuf_append_bcopy(rb, &val, sizeof (val));
}

void
reqbuf_append_u32(reqbuf_t *rb, uint32_t val)
{
	reqbuf_append_bcopy(rb, &val, sizeof (val));
}

void
reqbuf_append_u16(reqbuf_t *rb, uint16_t val)
{
	reqbuf_append_bcopy(rb, &val, sizeof (val));
}

void
reqbuf_append_u8(reqbuf_t *rb, uint8_t val)
{
	reqbuf_append_bcopy(rb, &val, sizeof (val));
}

void
reqbuf_append_str(reqbuf_t *rb, const char *s)
{
	uint16_t sz = strlen(s);
	reqbuf_append_bcopy(rb, &sz, sizeof (sz));
	reqbuf_append_bcopy(rb, s, sz);
}

unsigned
reqbuf_alloc(reqbuf_t **rb, size_t capacity)
{
	if (capacity < 4) {
		/*
		 * We always need room for the length prefix.
		 */
		return (EINVAL);
	}

	if ((*rb = (kmem_zalloc(sizeof (reqbuf_t), KM_SLEEP))) == NULL) {
		return (ENOMEM);
	}
	if (((*rb)->rb_data = kmem_zalloc(capacity, KM_SLEEP)) == NULL) {
		kmem_free(*rb, sizeof (*rb));
		*rb = NULL;
		return (ENOMEM);
	}
	(*rb)->rb_capacity = capacity;
	(*rb)->rb_limit = capacity;
	(*rb)->rb_pos = 0;
	(*rb)->rb_error = 0;
	return (0);
}

void
reqbuf_free(reqbuf_t *rb)
{
	if (rb == NULL) {
		return;
	}

	kmem_free(rb->rb_data, rb->rb_capacity);
	kmem_free(rb, sizeof (*rb));
}

unsigned
reqbuf_error(reqbuf_t *rb)
{
	return (rb->rb_error);
}

/*
 * 9P2000.u:
 *	 size[4] Tversion tag[2] msize[4] version[s]
 */
static void
create_tversion(reqbuf_t *rb, const char *version, size_t msize)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TVERSION);
	reqbuf_append_u16(rb, TAG_NOTAG);
	reqbuf_append_u32(rb, msize);
	reqbuf_append_str(rb, version);
}

int
p9fs_rpc(p9fs_session_t *p9s, reqbuf_t *rsend, reqbuf_t *rrecv,
    uint16_t expected_tag, uint8_t expected_type)
{
	int e;

	reqbuf_flip(rsend);
	if ((e = reqbuf_write(rsend, p9s->p9s_ldi)) != 0) {
		cmn_err(CE_WARN, "p9fs: write failed: %d", e);
		return (e);
	}
	if (reqbuf_remainder(rsend) != 0) {
		cmn_err(CE_WARN, "p9fs: short write?");
		return (EIO);
	}

	if ((e = reqbuf_read(rrecv, p9s->p9s_ldi)) != 0) {
		cmn_err(CE_WARN, "p9fs: read failed: %d", e);
		return (e);
	}

	/*
	 * Read the standard header fields that should always be present:
	 */
	uint32_t size = reqbuf_get_u32(rrecv);
	uint8_t type = reqbuf_get_u8(rrecv);
	uint16_t tag = reqbuf_get_u16(rrecv);
	if (reqbuf_error(rrecv) != 0) {
		cmn_err(CE_WARN, "p9fs: read early decode failed");
		return (e);
	}

	if (size - 7 > reqbuf_remainder(rrecv)) {
		cmn_err(CE_WARN, "p9fs: read size %u != expected %u",
		    (unsigned)reqbuf_remainder(rrecv), size - 7);
		return (e);
	}

	if (tag != expected_tag) {
		cmn_err(CE_WARN, "p9fs: read tag %x != expected %x",
		    tag, expected_tag);
		return (e);
	}

	if (type != expected_type) {
		cmn_err(CE_WARN, "p9fs: read type %u != expected %u",
		    type, expected_type);
		return (e);
	}

	return (0);
}

int
p9fs_session_init(p9fs_session_t **p9sp, ldi_handle_t lh)
{
	p9fs_session_t *p9s = kmem_zalloc(sizeof (*p9s), KM_SLEEP);
	p9s->p9s_ldi = lh;

	/*
	 * Negotiate the version with the remote peer.  Note that this has the
	 * effect of resetting any previously allocated file handles in a
	 * transport like Virtio where there is no explicit connection per se.
	 */
	uint32_t msize = 4096;
	reqbuf_t *rsend, *rrecv;
	if (reqbuf_alloc(&rsend, msize) != 0 ||
	    reqbuf_alloc(&rrecv, msize) != 0) {
		goto fail;
	}
	create_tversion(rsend, "9P2000.u", msize);

	if (p9fs_rpc(p9s, rsend, rrecv, TAG_NOTAG, PLAN9_RVERSION) != 0) {
		goto fail;
	}
	uint32_t newmsize = reqbuf_get_u32(rrecv);
	char *version = reqbuf_get_str(rrecv);
	if (reqbuf_error(rrecv) != 0) {
		reqbuf_strfree(version);
		cmn_err(CE_WARN, "p9fs: version decode failed");
		goto fail;
	}

	cmn_err(CE_WARN, "p9fs: msize = %u, version = %s", newmsize, version);
	reqbuf_strfree(version);

fail:
	reqbuf_free(rsend);
	reqbuf_free(rrecv);
	return (EINVAL);
}

void
p9fs_session_fini(p9fs_session_t *p9s)
{
}
