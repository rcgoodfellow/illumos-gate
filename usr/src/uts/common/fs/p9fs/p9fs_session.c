

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/stdbool.h>
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
#define	PLAN9_TCLUNK			120
#define	PLAN9_RCLUNK			(PLAN9_TCLUNK + 1)
#define	PLAN9_TSTAT			124
#define	PLAN9_RSTAT			(PLAN9_TSTAT + 1)

#define	TAG_NOTAG			0xFFFF

#define	MODE_OREAD			0
#define	MODE_OWRITE			1
#define	MODE_ORDWR			2
#define	MODE_OEXEC			3
#define	MODE_OTRUNC			0x10
#define	MODE_ORCLOSE			0x40


struct reqbuf {
	unsigned rb_error;
	uint8_t *rb_data;
	size_t rb_capacity;
	size_t rb_pos;
	size_t rb_limit;
};

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

void
reqbuf_free_qid(p9fs_qid_t *qid)
{
	if (qid != NULL) {
		kmem_free(qid, sizeof (*qid));
	}
}

p9fs_qid_t *
reqbuf_get_qid(reqbuf_t *rb)
{
	if (rb->rb_error != 0) {
		return (NULL);
	}

	p9fs_qid_t *qid;
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
 *	size[4] Tclunk tag[2] fid[4]
 */
static void
create_tclunk(reqbuf_t *rb, uint16_t tag, uint32_t fid)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TCLUNK);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
}

/*
 * 9P2000.u:
 * 	size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s])
 */
static void
create_twalk0(reqbuf_t *rb, uint16_t tag, uint32_t fid, uint32_t newfid)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TWALK);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
	reqbuf_append_u32(rb, newfid);
	reqbuf_append_u16(rb, 0);
}

/*
 * 9P2000.u:
 * 	size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s])
 */
static void
create_twalk1(reqbuf_t *rb, uint16_t tag, uint32_t fid, uint32_t newfid,
    const char *name)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TWALK);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
	reqbuf_append_u32(rb, newfid);
	reqbuf_append_u16(rb, 1);
	reqbuf_append_str(rb, name);
}

/*
 * 9P2000.u:
 *	size[4] Tstat tag[2] fid[4]
 */
static void
create_tstat(reqbuf_t *rb, uint16_t tag, uint32_t fid)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TSTAT);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
}

/*
 * 9P2000.u:
 *	size[4] Topen tag[2] fid[4] mode[1]
 */
static void
create_topen(reqbuf_t *rb, uint16_t tag, uint32_t fid, uint8_t omode)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TOPEN);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
	reqbuf_append_u8(rb, omode);
}

/*
 * 9P2000.u:
 *	size[4] Tread tag[2] fid[4] offset[8] count[4]
 */
static void
create_tread(reqbuf_t *rb, uint16_t tag, uint32_t fid, uint64_t offset,
    uint32_t count)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TREAD);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
	reqbuf_append_u64(rb, offset);
	reqbuf_append_u32(rb, count);
}

/*
 * 9P2000.u:
 *	size[4] Tversion tag[2] msize[4] version[s]
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

static void
create_tattach(reqbuf_t *rb, uint16_t tag, uint32_t fid, uint32_t afid,
    const char *uname, const char *aname, uint32_t n_uname)
{
	reqbuf_reset(rb);
	reqbuf_append_u8(rb, PLAN9_TATTACH);
	reqbuf_append_u16(rb, tag);
	reqbuf_append_u32(rb, fid);
	reqbuf_append_u32(rb, afid);
	reqbuf_append_str(rb, uname);
	reqbuf_append_str(rb, aname);
	reqbuf_append_u32(rb, n_uname);
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

read_again:
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
	reqbuf_trim(rrecv, size);

	if (tag != expected_tag) {
		cmn_err(CE_WARN, "p9fs: read tag %x != expected %x, discarding",
		    tag, expected_tag);

		/*
		 * XXX With the current code structure, an interrupted read may
		 * leave a reply to a previous request in the buffer.  Rather
		 * than make this fatal, we discard and try again.
		 *
		 * When this is restructured to correctly track more than one
		 * concurrent request, we'll fix this.
		 */
		goto read_again;
	}

	if (type != expected_type) {
		if (type == PLAN9_RERROR) {
			int decoded = 0;

			/*
			 * XXX Attempt to unpack the error information...
			 */
			char *estr = reqbuf_get_str(rrecv);
			uint32_t eno = reqbuf_get_u32(rrecv);

			if (reqbuf_error(rrecv) == 0) {
				if (strcasecmp("permission denied",
				    estr) == 0) {
					/*
					 * XXX The numeric value 13 that comes
					 * along with this seems like it is
					 * Linux-specific.  That may be fine
					 * for 9P2000.L, but what about .u?
					 */
					decoded = EACCES;
				} else {
					cmn_err(CE_WARN,
					    "p9fs: error \"%s\" num %u",
					    estr != NULL ? estr : "?", eno);
				}
			}

			reqbuf_strfree(estr);

			if (decoded != 0) {
				return (decoded);
			}
		}
		cmn_err(CE_WARN, "p9fs: read type %u != expected %u",
		    type, expected_type);
		return (e);
	}

	return (0);
}

static void p9fs_session_cleanup(p9fs_session_t *p9s);

int
p9fs_session_init(p9fs_session_t **p9sp, ldi_handle_t lh, uint_t id)
{
	p9fs_session_t *p9s = kmem_zalloc(sizeof (*p9s), KM_SLEEP);
	p9s->p9s_id = id;
	p9s->p9s_ldi = lh;
	p9s->p9s_msize = 4096;
	p9s->p9s_next_tag = 101;
	mutex_init(&p9s->p9s_mutex, NULL, MUTEX_DRIVER, NULL);
	if (reqbuf_alloc(&p9s->p9s_send, p9s->p9s_msize) != 0 ||
	    reqbuf_alloc(&p9s->p9s_recv, p9s->p9s_msize) != 0) {
		goto fail;
	}

	/*
	 * Negotiate the version with the remote peer.  Note that this has the
	 * effect of resetting any previously allocated file handles in a
	 * transport like Virtio where there is no explicit connection per se.
	 */
	create_tversion(p9s->p9s_send, "9P2000.u", p9s->p9s_msize);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, TAG_NOTAG, 
	    PLAN9_RVERSION) != 0) {
		goto fail;
	}

	uint32_t newmsize = reqbuf_get_u32(p9s->p9s_recv);
	char *version = reqbuf_get_str(p9s->p9s_recv);
	if (reqbuf_error(p9s->p9s_recv) != 0) {
		reqbuf_strfree(version);
		cmn_err(CE_WARN, "p9fs: version decode failed");
		goto fail;
	}

	cmn_err(CE_WARN, "p9fs: msize = %u, version = %s", newmsize, version);

	/*
	 * XXX For now, we demand the size and version that we sent.
	 */
	bool version_ok = strcmp(version, "9P2000.u") == 0;
	reqbuf_strfree(version);
	if (newmsize != p9s->p9s_msize || !version_ok) {
		cmn_err(CE_WARN, "p9fs: bogus hypervisor, giving up");
		goto fail;
	}

	char nam[128];
	(void) snprintf(nam, sizeof (nam), "p9fs_session_%u", id);
	if ((p9s->p9s_fid_space = id_space_create(nam, 1, INT_MAX)) == NULL) {
		cmn_err(CE_WARN, "p9fs: idspace failure");
		goto fail;
	}

	/*
	 * Attach as root and look up the root of the file system.
	 */
	p9s->p9s_root_fid = id_alloc(p9s->p9s_fid_space);
	uint16_t t = p9s->p9s_next_tag++;
	create_tattach(p9s->p9s_send, t, p9s->p9s_root_fid, ~0,
	    "root", "", 0);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, 
	    PLAN9_RATTACH) != 0) {
		cmn_err(CE_WARN, "p9fs: could not ATTACH");
		goto fail;
	}

	p9s->p9s_root_qid = reqbuf_get_qid(p9s->p9s_recv);
	if (reqbuf_error(p9s->p9s_recv) != 0) {
		cmn_err(CE_WARN, "p9fs: attach decode failed");
		goto fail;
	}

	*p9sp = p9s;
	return (0);

fail:
	p9fs_session_cleanup(p9s);
	return (EINVAL);
}

void
p9fs_session_lock(p9fs_session_t *p9s)
{
	mutex_enter(&p9s->p9s_mutex);
}

void
p9fs_session_unlock(p9fs_session_t *p9s)
{
	mutex_exit(&p9s->p9s_mutex);
}

void
p9fs_session_fini(p9fs_session_t *p9s)
{
	/*
	 * In case it helps the hypervisor release resources we attempt a reset
	 * by sending a new VERSION message, which has the effect of clunking
	 * all the fids.
	 */
	p9fs_session_lock(p9s);
	create_tversion(p9s->p9s_send, "9P2000.u", p9s->p9s_msize);
	(void) p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, TAG_NOTAG, 
	    PLAN9_RVERSION);
	p9fs_session_unlock(p9s);

	p9fs_session_cleanup(p9s);
}

static void
p9fs_session_cleanup(p9fs_session_t *p9s)
{
	reqbuf_free_qid(p9s->p9s_root_qid);
	reqbuf_free(p9s->p9s_send);
	reqbuf_free(p9s->p9s_recv);
	if (p9s->p9s_fid_space != NULL) {
		id_space_destroy(p9s->p9s_fid_space);
	}
	(void) ldi_close(p9s->p9s_ldi, FREAD | FWRITE | FEXCL, kcred);
	mutex_destroy(&p9s->p9s_mutex);
	kmem_free(p9s, sizeof (*p9s));
}

int
p9fs_session_stat(p9fs_session_t *p9s, uint32_t fid, p9fs_stat_t *p9st)
{
	uint16_t t;

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	t = p9s->p9s_next_tag++;
	create_tstat(p9s->p9s_send, t, fid);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_RSTAT) != 0) {
		cmn_err(CE_WARN, "p9fs: could not STAT %x", fid);
		return (EIO);
	}

	(void) reqbuf_get_u16(p9s->p9s_recv); /* XXX unused length prefix? */

	(void) reqbuf_get_u16(p9s->p9s_recv); /* size */
	(void) reqbuf_get_u16(p9s->p9s_recv); /* type */
	(void) reqbuf_get_u32(p9s->p9s_recv); /* dev */

	p9st->p9st_qid = reqbuf_get_qid(p9s->p9s_recv);

	p9st->p9st_mode = reqbuf_get_u32(p9s->p9s_recv);

	p9st->p9st_atime = reqbuf_get_u32(p9s->p9s_recv);
	p9st->p9st_mtime = reqbuf_get_u32(p9s->p9s_recv);

	p9st->p9st_length = reqbuf_get_u64(p9s->p9s_recv);

	p9st->p9st_name = reqbuf_get_str(p9s->p9s_recv);
	reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* uid */
	reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* gid */
	reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* muid */
	p9st->p9st_extension = reqbuf_get_str(p9s->p9s_recv);

	p9st->p9st_uid = reqbuf_get_u32(p9s->p9s_recv);
	p9st->p9st_gid = reqbuf_get_u32(p9s->p9s_recv);
	p9st->p9st_muid = reqbuf_get_u32(p9s->p9s_recv);

	if (reqbuf_error(p9s->p9s_recv) != 0) {
		p9fs_session_stat_reset(p9st);
		cmn_err(CE_WARN, "p9fs: STAT %u decode failed", fid);
		return (EIO);
	}

	return (0);
}

int
p9fs_session_clunk(p9fs_session_t *p9s, uint32_t fid)
{
	uint16_t t;

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	t = p9s->p9s_next_tag++;
	create_tclunk(p9s->p9s_send, t, fid);

	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_RCLUNK) != 0) {
		cmn_err(CE_WARN, "p9fs: could not CLUNK %x", fid);
		return (EIO);
	}

	id_free(p9s->p9s_fid_space, fid);
	return (0);
}

int
p9fs_session_lookup(p9fs_session_t *p9s, uint32_t fid, const char *name,
    uint32_t *newfid, p9fs_qid_t *newqid)
{
	int r;
	uint16_t t;
	id_t id;

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	if ((id = id_alloc_nosleep(p9s->p9s_fid_space)) == -1) {
		return (ENOMEM);
	}

	t = p9s->p9s_next_tag++;
	create_twalk1(p9s->p9s_send, t, fid, id, name);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_RWALK) != 0) {
		cmn_err(CE_WARN, "p9fs: could not WALK %x", fid);
		id_free(p9s->p9s_fid_space, id);
		return (EIO);
	}

	/*
	 * 9P2000.u:
	 *	size[4] Rwalk tag[2] nwqid[2] nwqid*(qid[13])
	 */
	uint16_t nqids = reqbuf_get_u16(p9s->p9s_recv);
	p9fs_qid_t *qid = reqbuf_get_qid(p9s->p9s_recv);

	if (reqbuf_error(p9s->p9s_recv) == 0 && nqids == 1) {
		*newfid = id;
		*newqid = *qid;
		r = 0;
	} else {
		cmn_err(CE_WARN, "p9fs: lookup %u decode failed", fid);
		r = EIO;
	}

	reqbuf_free_qid(qid);
	return (r);
}

int
p9fs_session_dupfid(p9fs_session_t *p9s, uint32_t fid, uint32_t *newfid)
{
	uint16_t t;
	id_t id;

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	if ((id = id_alloc_nosleep(p9s->p9s_fid_space)) == -1) {
		return (ENOMEM);
	}

	t = p9s->p9s_next_tag++;
	create_twalk0(p9s->p9s_send, t, fid, id);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_RWALK) != 0) {
		cmn_err(CE_WARN, "p9fs: could not WALK %x", fid);
		id_free(p9s->p9s_fid_space, id);
		return (EIO);
	}

	/*
	 * 9P2000.u:
	 *	size[4] Rwalk tag[2] nwqid[2] nwqid*(qid[13])
	 *
	 * XXX If this fid duplication was a success, discard the qid for now?
	 */
	*newfid = id;
	return (0);
}

void
p9fs_session_stat_reset(p9fs_stat_t *p9st)
{
	reqbuf_strfree(p9st->p9st_name);
	reqbuf_strfree(p9st->p9st_extension);
	reqbuf_free_qid(p9st->p9st_qid);
	bzero(p9st, sizeof (*p9st));
}

int
p9fs_session_readdir(p9fs_session_t *p9s, uint32_t fid, p9fs_readdir_t **p9rdp)
{
	int r;
	uint16_t t;
	p9fs_readdir_t *p9rd = kmem_zalloc(sizeof (*p9rd), KM_SLEEP);

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	if ((r = p9fs_session_dupfid(p9s, fid, &p9rd->p9rd_fid)) != 0) {
		kmem_free(p9rd, sizeof (*p9rd));
		return (r);
	}

	/*
	 * After duplicating the fid, we must open it for read.
	 */
	t = p9s->p9s_next_tag++;
	create_topen(p9s->p9s_send, t, p9rd->p9rd_fid, MODE_OREAD);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_ROPEN) != 0) {
		cmn_err(CE_WARN, "p9fs: could not OPEN %x", fid);
		(void) p9fs_session_clunk(p9s, p9rd->p9rd_fid);
		kmem_free(p9rd, sizeof (*p9rd));
		return (EIO);
	}

	list_create(&p9rd->p9rd_ents, sizeof (p9fs_readdir_ent_t),
	    offsetof(p9fs_readdir_ent_t, p9de_link));
	p9rd->p9rd_next_ord = 2; /* XXX skip ".", 0, and "..", 1. */
	p9rd->p9rd_next_offset = 0;
	p9rd->p9rd_eof = false;

	*p9rdp = p9rd;
	return (0);
}

void
p9fs_session_readdir_ent_free(p9fs_readdir_ent_t *p9de)
{
	reqbuf_strfree(p9de->p9de_name);
	kmem_free(p9de, sizeof (*p9de));
}

void
p9fs_session_readdir_free(p9fs_session_t *p9s, p9fs_readdir_t *p9rd)
{
	p9fs_readdir_ent_t *t;

	(void) p9fs_session_clunk(p9s, p9rd->p9rd_fid);

	while ((t = list_remove_head(&p9rd->p9rd_ents)) != NULL) {
		p9fs_session_readdir_ent_free(t);
	}
	list_destroy(&p9rd->p9rd_ents);

	kmem_free(p9rd, sizeof (*p9rd));
}

int
p9fs_session_readdir_next(p9fs_session_t *p9s, p9fs_readdir_t *p9rd)
{
	uint16_t t;
	id_t id;

	VERIFY(MUTEX_HELD(&p9s->p9s_mutex));

	t = p9s->p9s_next_tag++;
	create_tread(p9s->p9s_send, t, p9rd->p9rd_fid, p9rd->p9rd_next_offset, 256);
	if (p9fs_rpc(p9s, p9s->p9s_send, p9s->p9s_recv, t, PLAN9_RREAD) != 0) {
		cmn_err(CE_WARN, "p9fs: could not READ %x", p9rd->p9rd_fid);
		return (EIO);
	}

	/*
	 * The read response for a directory is specially formatted.  For
	 * 9P2000.u, the body of the read contains a whole number of variable
	 * length RSTAT-style responses.
	 *
	 * First, determine the number of bytes that were read:
	 */
	uint32_t rcount = reqbuf_get_u32(p9s->p9s_recv);

	if (rcount == 0) {
		p9rd->p9rd_eof = true;
		return (0);
	}

	if (rcount != reqbuf_remainder(p9s->p9s_recv)) {
		/*
		 * XXX
		 */
		cmn_err(CE_WARN, "p9fs: rcount %u != remainder %lu",
		    rcount, reqbuf_remainder(p9s->p9s_recv));
		return (EIO);
	}

	while (reqbuf_remainder(p9s->p9s_recv) != 0) {
		/*
		 * Read the next stat entry, keeping only the relevant fields.
		 */
		p9fs_qid_t *qid;
		char *name;

		(void) reqbuf_get_u16(p9s->p9s_recv); /* size */
		(void) reqbuf_get_u16(p9s->p9s_recv); /* type */
		(void) reqbuf_get_u32(p9s->p9s_recv); /* dev */

		qid = reqbuf_get_qid(p9s->p9s_recv);

		(void) reqbuf_get_u32(p9s->p9s_recv); /* mode */

		(void) reqbuf_get_u32(p9s->p9s_recv); /* atime */
		(void) reqbuf_get_u32(p9s->p9s_recv); /* mtime */

		(void) reqbuf_get_u64(p9s->p9s_recv); /* length */

		name = reqbuf_get_str(p9s->p9s_recv);
		reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* uid */
		reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* gid */
		reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* muid */
		reqbuf_strfree(reqbuf_get_str(p9s->p9s_recv)); /* ext. */

		(void) reqbuf_get_u32(p9s->p9s_recv); /* numeric uid */
		(void) reqbuf_get_u32(p9s->p9s_recv); /* numeric gid */
		(void) reqbuf_get_u32(p9s->p9s_recv); /* numeric muid */

		if (reqbuf_error(p9s->p9s_recv) != 0) {
			reqbuf_free_qid(qid);
			reqbuf_strfree(name);
			cmn_err(CE_WARN, "p9fs: readdir %u decode failed",
			    p9rd->p9rd_fid);
			return (EIO);
		}

		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			/*
			 * There is a brief note in the Plan 9 intro(5)
			 * page:
			 *
			 *	All directories must support walks to the
			 *	directory '..' (dot-dot) meaning parent
			 *	directory, although by convention directories
			 *	contain no explicit entry for '..' or '.'
			 *	(dot).
			 *
			 * Although QEMU appears to have missed this memo, and
			 * includes both special entries at least some of the
			 * time in a read of a directory under 9P2000.u, we
			 * will omit whatever it told us here and insert our
			 * own entries.
			 */
			reqbuf_free_qid(qid);
			reqbuf_strfree(name);
			continue;
		}

		p9fs_readdir_ent_t *p9de = kmem_zalloc(sizeof (*p9de),
		    KM_SLEEP);
		p9de->p9de_qid = *qid;
		reqbuf_free_qid(qid);
		p9de->p9de_name = name;
		p9de->p9de_ord = p9rd->p9rd_next_ord++;
		list_insert_tail(&p9rd->p9rd_ents, p9de);
	}

	p9rd->p9rd_next_offset += rcount;

	return (0);
}
