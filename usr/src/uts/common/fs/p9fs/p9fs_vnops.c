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
 * Copyright 2021 Oxide Computer Company
 */

#include <sys/stddef.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <vm/seg.h>
#include <vm/page.h>
#include <vm/page.h>
#include <vm/pvn.h>
#include <vm/seg_map.h>

#include <sys/fs/p9fs_impl.h>

/*
 * XXX p9fs
 */

struct vnodeops *p9fs_vnodeops;

static int
p9fs_getattr(vnode_t *v, struct vattr *va, int flags, struct cred *cr,
    caller_context_t *ct)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	p9fs_stat_t p9st;
	int r;

	bzero(&p9st, sizeof (p9st));

	p9fs_session_lock(p9s);
	r = p9fs_session_stat(p9s, p9n->p9n_fid, &p9st);
	p9fs_session_unlock(p9s);

	if (r != 0) {
		return (r);
	}

	va->va_type = v->v_type;

	/*
	 * Translate permissions:
	 */
	va->va_mode = p9st.p9st_mode & PLAN9_PERM;
	if (p9st.p9st_mode & PLAN9_MODE_SETUID) {
		va->va_mode |= S_ISUID;
	}
	if (p9st.p9st_mode & PLAN9_MODE_SETGID) {
		va->va_mode |= S_ISGID;
	}

	/*
	 * Translate file type:
	 */
	if (p9st.p9st_mode & PLAN9_MODE_DIR) {
		va->va_mode |= S_IFDIR;
	} else if (p9st.p9st_mode & PLAN9_MODE_SYMLINK) {
		va->va_mode |= S_IFLNK;
	} else if (p9st.p9st_mode & PLAN9_MODE_DEVICE) {
		va->va_mode |= S_IFCHR; /* XXX? */
	} else if (p9st.p9st_mode & PLAN9_MODE_NAMED_PIPE) {
		va->va_mode |= S_IFIFO;
	} else if (p9st.p9st_mode & PLAN9_MODE_SOCKET) {
		va->va_mode |= S_IFSOCK;
	} else {
		va->va_mode |= S_IFREG;
	}

	va->va_uid = p9st.p9st_uid;
	va->va_gid = p9st.p9st_gid;

	va->va_fsid = v->v_vfsp->vfs_dev;
	va->va_nodeid = (ino64_t)p9st.p9st_qid->qid_path; /* XXX? */
	va->va_nlink = 1;
	va->va_size = p9st.p9st_length;
	va->va_rdev = 0;
	va->va_nblocks =
	    (fsblkcnt64_t)howmany((offset_t)p9st.p9st_length, DEV_BSIZE);
	va->va_blksize = DEV_BSIZE;

	va->va_mtime.tv_sec = p9st.p9st_mtime;
	va->va_mtime.tv_nsec = 0;

	va->va_atime.tv_sec = p9st.p9st_atime;
	va->va_atime.tv_nsec = 0;

	va->va_ctime = va->va_mtime;

	p9fs_session_stat_reset(&p9st);

	return (0);
}

p9fs_node_t *
p9fs_make_node(p9fs_t *p9, uint32_t fid, p9fs_qid_t *qid, vtype_t type)
{
	vnode_t *v = vn_alloc(KM_SLEEP);
	vn_setops(v, p9fs_vnodeops);

	v->v_type = type;
	if (type == VREG) {
		/*
		 * XXX ?
		 */
		v->v_flag |= VNOSWAP;
	}

	p9fs_node_t *p9n = kmem_zalloc(sizeof (*p9n), KM_SLEEP);
	p9n->p9n_fs = p9;
	p9n->p9n_fid = fid;
	p9n->p9n_qid = *qid;

	p9n->p9n_vnode = v;
	v->v_data = p9n;
	VFS_HOLD(p9->p9_vfs);
	v->v_vfsp = p9->p9_vfs;
	vn_exists(v);
	return (p9n);
}

static void
p9fs_free_node(p9fs_node_t *p9n)
{
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;

	/*
	 * Let go of any 9P session resources:
	 */
	p9fs_session_lock(p9s);
	if (p9n->p9n_readdir != NULL) {
		p9fs_session_readdir_free(p9s, p9n->p9n_readdir);
	}
	if (p9n->p9n_read_fid != 0) {
		(void) p9fs_session_clunk(p9s, p9n->p9n_read_fid);
	}
	(void) p9fs_session_clunk(p9s, p9n->p9n_fid);
	p9fs_session_unlock(p9s);

	/*
	 * Release the hold on the VFS we took in p9fs_make_node() and free.
	 */
	VFS_RELE(p9->p9_vfs);
	vn_free(p9n->p9n_vnode);
	kmem_free(p9n, sizeof (*p9n));
}

static int
p9fs_open(vnode_t **vp, int flag, cred_t *cr, caller_context_t *ct)
{
#if 0
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
#endif

	if (flag & FWRITE) {
		/*
		 * XXX
		 */
		return (EPERM);
	}

	return (0);
}

static int
p9fs_close(vnode_t *v, int flag, int count, offset_t offset, cred_t *cr,
    caller_context_t *ct)
{
	return (0);
}

static int
p9fs_access(vnode_t *v, int mode, int flags, struct cred *cr,
    caller_context_t *ct)
{
	if (mode & VWRITE) {
		return (EPERM);
	}

	/*
	 * XXX check the bits.  secpolicy_vnode_access2() etc?
	 */

	return (0);
}

static int
p9fs_readdir(vnode_t *v, struct uio *uio, struct cred *cr,
    int *eof, caller_context_t *ct, int flags)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	int r;

	/*
	 * XXX This is totally serialised for now.
	 */
	mutex_enter(&p9n->p9n_mutex);

	VERIFY3U(v->v_type, ==, VDIR);

	/*
	 * XXX Each "byte" in our offset will represent a single directory.
	 */
	offset_t offset = uio->uio_loffset;
	offset_t orig_offset = offset;
	if (eof != NULL) {
		*eof = 0;
	}

	p9fs_session_lock(p9s);

	if (p9n->p9n_readdir != NULL) {
		p9fs_readdir_t *p9rd = p9n->p9n_readdir;
		p9fs_readdir_ent_t *p9de = list_head(&p9rd->p9rd_ents);
		bool reset = false;

		if (p9de != NULL) {
			/*
			 * We have a spare directory entry from a previous
			 * readdir that we were not able to pass entirely to
			 * userland.
			 */
			if (offset < p9de->p9de_ord) {
				/*
				 * This walk has reset to an earlier position.
				 * We need to start walking again from the
				 * beginning.
				 */
				reset = true;
			}
		} else if (offset < p9rd->p9rd_next_ord) {
			/*
			 * The next directory entry we were going to emit is
			 * later in the walk than the requested entry.
			 */
			reset = true;
		}

		if (reset) {
			p9fs_session_readdir_free(p9s, p9n->p9n_readdir);
			p9n->p9n_readdir = NULL;
		}
	}

	if (p9n->p9n_readdir == NULL) {
		/*
		 * Open a new readdir cursor for this directory:
		 */
		if ((r = p9fs_session_readdir(p9s, p9n->p9n_fid,
		    &p9n->p9n_readdir) != 0)) {
			goto bail;
		}
	}

	/*
	 * Scroll through the directory entries we have until we find the one
	 * that matches our offset.
	 */
	for (;;) {
		p9fs_readdir_t *p9rd = p9n->p9n_readdir;
		p9fs_readdir_ent_t *p9de = NULL;

		if (offset == 0 || offset == 1) {
			const char *name = offset == 0 ? "." : "..";
			size_t sz = DIRENT64_RECLEN(strlen(name));
			dirent64_t *d = kmem_zalloc(sz, KM_SLEEP);
			d->d_ino = p9n->p9n_qid.qid_path;
			d->d_off = offset;
			d->d_reclen = sz;
			(void) strlcpy(d->d_name, name, DIRENT64_NAMELEN(sz));
			(void) uiomove(d, sz, UIO_READ, uio);
			kmem_free(d, sz);
			offset++;
			continue;
		}

		if ((p9de = list_head(&p9rd->p9rd_ents)) != NULL) {
			if (offset > p9de->p9de_ord) {
				/*
				 * This entry is before our offset, so discard
				 * it.
				 */
				(void) list_remove_head(&p9rd->p9rd_ents);
				p9fs_session_readdir_ent_free(p9de);
				continue;
			}

			/*
			 * XXX Do we have enough space to write this out?
			 */
			size_t sz = DIRENT64_RECLEN(strlen(p9de->p9de_name));
			if (sz > uio->uio_resid) {
				break;
			}

			dirent64_t *d = kmem_zalloc(sz, KM_SLEEP);
			d->d_ino = p9de->p9de_qid.qid_path;
			d->d_off = offset;
			d->d_reclen = sz;
			(void) strlcpy(d->d_name, p9de->p9de_name,
			    DIRENT64_NAMELEN(sz));
			(void) uiomove(d, sz, UIO_READ, uio);
			kmem_free(d, sz);
			offset++;
			continue;
		}

		if (p9rd->p9rd_eof) {
			if (eof != NULL) {
				*eof = 1;
			}
			break;
		}

		/*
		 * Fetch another page of results.
		 */
		if ((r = p9fs_session_readdir_next(p9s, p9rd)) != 0) {
			if (offset != orig_offset) {
				/*
				 * We have written out some entries, so don't
				 * report an I/O failure now.
				 */
				break;
			}
			goto bail;
		}
	}

	uio->uio_loffset = offset;
	r = 0;

bail:
	p9fs_session_unlock(p9s);
	mutex_exit(&p9n->p9n_mutex);
	return (r);
}

static int
p9fs_lookup(vnode_t *v, char *name, vnode_t **vp, pathname_t *lookpn,
    int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
    int *direntflags, pathname_t *outpn)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	int r;
	uint32_t chfid;
	p9fs_qid_t chqid;

	if (v->v_type != VDIR) {
		return (ENOTDIR);
	}

	if (flags & LOOKUP_XATTR) {
		return (ENOTSUP);
	}

	if (name[0] == '\0') {
		/*
		 * XXX
		 */
		VN_HOLD(v);
		*vp = v;
		return (0);
	}

	p9fs_session_lock(p9s);
	r = p9fs_session_lookup(p9s, p9n->p9n_fid, name, &chfid, &chqid);
	p9fs_session_unlock(p9s);

	if (r != 0) {
		return (r);
	}

	/*
	 * Use the qid type field to determine what vnode type we require:
	 */
	vtype_t vt;
	switch (chqid.qid_type) {
	case PLAN9_QIDTYPE_DIR:
		vt = VDIR;
		break;
	case PLAN9_QIDTYPE_FILE:
		vt = VREG;
		break;
	case PLAN9_QIDTYPE_SYMLINK:
		vt = VLNK;
		break;
	default:
		cmn_err(CE_WARN, "p9fs: lookup \"%s\" had type %x\n",
		    name, chqid.qid_type);
		p9fs_session_lock(p9s);
		(void) p9fs_session_clunk(p9s, chfid);
		p9fs_session_unlock(p9s);
		return (ENOTSUP);
	}

	p9fs_node_t *chnode = p9fs_make_node(p9, chfid, &chqid, vt);
	*vp = chnode->p9n_vnode;
	return (0);
}

static int
p9fs_readlink(vnode_t *v, struct uio *uio, cred_t *cr, caller_context_t *ct)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	int r;

	if (v->v_type != VLNK) {
		return (EINVAL);
	}

	p9fs_stat_t p9st;
	bzero(&p9st, sizeof (p9st));

	p9fs_session_lock(p9s);
	r = p9fs_session_stat(p9s, p9n->p9n_fid, &p9st);
	p9fs_session_unlock(p9s);

	if (r == 0 && p9st.p9st_qid->qid_type != PLAN9_QIDTYPE_SYMLINK) {
		/*
		 * We expected a symlink, but we didn't get one in the stat
		 * request.
		 */
		r = EINVAL;
	}

	if (r == 0) {
		/*
		 * The link target is in the extension field for symlink files.
		 */
		size_t sz = strlen(p9st.p9st_extension);
		r = uiomove(p9st.p9st_extension, MIN(sz, uio->uio_resid),
		    UIO_READ, uio);
	}

	p9fs_session_stat_reset(&p9st);

	return (0);
}

static int
p9fs_rw(p9fs_node_t *p9n, struct uio *uio, enum uio_rw rw, int ioflag)
{
	vnode_t *v = p9n->p9n_vnode;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	rlim64_t limit;
	int e = 0;

	if (rw != UIO_READ) {
		/*
		 * XXX
		 */
		return (ENOSYS);
	}

	if (uio->uio_loffset < 0) {
		return (EINVAL);
	}

	if ((limit = uio->uio_llimit) == RLIM64_INFINITY ||
	    limit > MAXOFFSET_T) {
		limit = MAXOFFSET_T;
	}

	/*
	 * if (uio->uio_loffset >= limit && rw == UIO_WRITE) {
	 *	XXX rctl_action?
	 */

	if (uio->uio_resid == 0) {
		return (0);
	}
	ssize_t oresid = uio->uio_resid;

	p9fs_stat_t p9st;
	bzero(&p9st, sizeof (p9st));

	for (;;) {
		/*
		 * Determine how large the file is at present.
		 * XXX cache this?
		 */
		p9fs_session_stat_reset(&p9st);
		p9fs_session_lock(p9s);
		e = p9fs_session_stat(p9s, p9n->p9n_fid, &p9st);
		p9fs_session_unlock(p9s);
		if (e != 0 || uio->uio_loffset >= p9st.p9st_length) {
			break;
		}
		size_t filerem = p9st.p9st_length - uio->uio_loffset;

		/*
		 * Round the target offset down to a MAXBSIZE-aligned chunk of
		 * the page cache, and determine where our target offset begins
		 * within that chunk.
		 */
		u_offset_t mapbase = uio->uio_loffset & MAXBMASK;
		u_offset_t mapoff = uio->uio_loffset & MAXBOFFSET;

		/*
		 * I/O is for whatever remains in this cache chunk:
		 */
		size_t mapsize = MIN(MAXBSIZE - mapoff, uio->uio_resid);
		mapsize = MIN(mapsize, filerem);
		if (mapsize == 0) {
			break;
		}

		uio_prefaultpages(mapsize, uio);

		/*
		 * Locate the start of this chunk in the cache:
		 */
		caddr_t base = segmap_getmap(segkmap, v, mapbase);

		if ((e = uiomove(base + mapoff, mapsize, rw, uio)) != 0) {
			/*
			 * XXX SM_INVAL for UIO_WRITE?
			 */
			(void) segmap_release(segkmap, base, 0);
		} else {
			uint_t flags = 0;

			/*
			 * XXX SM_DONTNEED if we took the whole chunk?
			 * XXX UIO_WRITE
			 */

			e = segmap_release(segkmap, base, flags);
		}

		if (e != 0 || uio->uio_resid <= 0 || mapsize == 0) {
			break;
		}
	}

	p9fs_session_stat_reset(&p9st);

	if (uio->uio_resid != oresid) {
		/*
		 * If we moved any data, discard the error.
		 */
		return (0);
	} else {
		return (e);
	}
}

static int
p9fs_read(vnode_t *v, struct uio *uio, int ioflag, cred_t *cr,
    caller_context_t *ct)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;

	if (v->v_type != VREG) {
		return (EINVAL);
	}

	return (p9fs_rw(p9n, uio, UIO_READ, ioflag));
}

static int
p9fs_seek(vnode_t *v, offset_t oldoff, offset_t *newoff, caller_context_t *ct)
{
	if (v->v_type == VDIR) {
		return (0);
	}

	return ((*newoff < 0) ? EINVAL : 0);
}

static int
p9fs_bio(struct buf *b, cred_t *cr, bool *past_end)
{
	p9fs_node_t *p9n = b->b_vp->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;

	DTRACE_IO1(start, struct buf *, b);

	caddr_t bufpos = b->b_un.b_addr;
	uint64_t offset = dbtob(b->b_blkno);
	uint32_t count = b->b_bcount;

	if (!(b->b_flags & B_READ)) {
		/*
		 * XXX don't support writes yet
		 */
		b->b_error = ENOSYS;
		b->b_flags |= B_ERROR;
		DTRACE_IO1(done, struct buf *, b);
		return (ENOSYS);
	}

	/*
	 * Make 9P read requests to fill out the page.  Note that there does
	 * not seem to be a requirement that a read be full length, even if
	 * those bytes are available, so we must loop carefully here.
	 */
	p9fs_session_lock(p9s);
	while (count > 0) {
		int r;
		uint32_t rcount;

		if ((r = p9fs_session_read(p9s, p9n->p9n_read_fid, offset,
		    bufpos, count, &rcount)) != 0) {
			b->b_error = r;
			b->b_flags |= B_ERROR;
			break;
		}

		if (rcount == 0) {
			/*
			 * We reached the end of the file.
			 */
			break;
		}

		VERIFY3U(rcount, <=, count);
		count -= rcount;
		offset += rcount;
		bufpos += rcount;
	}
	p9fs_session_unlock(p9s);

	if (past_end != NULL && b->b_error == 0 &&
	    b->b_bcount > 0 && count == 0) {
		/*
		 * Signal that this offset is past the end of the file.
		 */
		*past_end = true;
	}

	b->b_resid = count;

	/*
	 * Zero the remainder of the buffer.
	 */
	bzero(b->b_un.b_addr + b->b_bcount - b->b_resid, b->b_resid);

	DTRACE_IO1(done, struct buf *, b);

	return (b->b_error);
}

static int
p9fs_getapage(vnode_t *v, u_offset_t off, size_t len, uint_t *prot,
    page_t *pl[], size_t plsz, struct seg *seg, caddr_t addr,
    enum seg_rw rw, struct cred *cr)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	int e;

	if (pl == NULL) {
		/*
		 * XXX no "readahead"?
		 */
		return (0);
	}
	pl[0] = NULL;

	page_t *pp;
again:
	if (page_exists(v, off) != NULL) {
		/*
		 * Try to obtain a shared lock on the page.  If we cannot get a
		 * lock, we'll need to read it from disk.
		 */
		if ((pp = page_lookup(v, off, SE_SHARED)) != NULL) {
			pl[0] = pp;
			pl[1] = NULL;
			return (0);
		}
	}

	u_offset_t io_off;
	size_t io_len;
	if ((pp = pvn_read_kluster(v, off, seg, addr, &io_off, &io_len,
	    off, PAGESIZE, 0)) == NULL) {
		/*
		 * Another thread may have created the page?  Try again.
		 */
		goto again;
	}

	/*
	 * XXX Round the request size up to a page boundary?  Something about
	 * zeroing unread regions at EOF.
	 */
	io_len = ptob(btopr(io_len));

	bool past_end = false;
	struct buf *b = pageio_setup(pp, io_len, v, B_READ);
	VERIFY(b != NULL);
	VERIFY0(b->b_un.b_addr);

	b->b_edev = 0;
	b->b_dev = 0;
	b->b_lblkno = lbtodb(io_off);
	b->b_file = v;
	b->b_offset = off;
	bp_mapin(b);

	e = p9fs_bio(b, cr, &past_end);

	bp_mapout(b);
	pageio_done(b);

	if (e == 0 && past_end && seg != segkmap) {
		/*
		 * XXX A write system call may first read past the end of the
		 * file while appending, according to comments in NFS.  In that
		 * case, return our buffer of all zero.  Otherwise, report an
		 * error.
		 */
		e = EFAULT;
	}

	if (e != 0) {
		pvn_read_done(pp, B_ERROR);
		return (e);
	}

	pvn_plist_init(pp, pl, plsz, off, io_len, rw);
	return (e);
}

static int
p9fs_getpage(vnode_t *v, offset_t off, size_t len, uint_t *prot, page_t *pl[],
    size_t plsz, struct seg *seg, caddr_t addr, enum seg_rw rw,
    cred_t *cr, caller_context_t *ct)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;

	if (v->v_flag & VNOMAP) {
		/*
		 * XXX ?
		 */
		return (ENOSYS);
	}

	if (prot != NULL) {
		/*
		 * XXX ?
		 */
		*prot = PROT_ALL;
	}

	/*
	 * If we have not yet opened a read fid for this vnode, do so first.
	 */
	int r = 0;
	mutex_enter(&p9n->p9n_mutex);
	if (p9n->p9n_read_fid == 0) {
		p9fs_session_lock(p9s);
		r = p9fs_session_open(p9s, p9n->p9n_fid, &p9n->p9n_read_fid);
		p9fs_session_unlock(p9s);
	}
	mutex_exit(&p9n->p9n_mutex);
	if (r != 0) {
		return (r);
	}

	return (pvn_getpages(p9fs_getapage, v, off, len, prot, pl, plsz,
	    seg, addr, rw, cr));
}

static void
p9fs_inactive(vnode_t *v, cred_t *cr, caller_context_t *ct)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;

	/*
	 * An asynchronous hold may appear between vn_rele() and when we take
	 * the lock.  Don't destroy anything unless we really are the last
	 * reference.
	 */
	mutex_enter(&v->v_lock);
	VERIFY(v->v_count >= 1);
	if (v->v_count > 1) {
		VN_RELE_LOCKED(v);
		mutex_exit(&v->v_lock);
		return;
	}
	mutex_exit(&v->v_lock);

	/*
	 * The vnode is ours to destroy.
	 */
	p9fs_free_node(p9n);
}

const fs_operation_def_t p9fs_vnodeops_template[] = {
	{ .name = VOPNAME_GETATTR, .func = { .vop_getattr = p9fs_getattr }},
	{ .name = VOPNAME_OPEN, .func = { .vop_open = p9fs_open }},
	{ .name = VOPNAME_CLOSE, .func = { .vop_close = p9fs_close }},
	{ .name = VOPNAME_ACCESS, .func = { .vop_access = p9fs_access }},
	{ .name = VOPNAME_READDIR, .func = { .vop_readdir = p9fs_readdir }},
	{ .name = VOPNAME_LOOKUP, .func = { .vop_lookup = p9fs_lookup }},
	{ .name = VOPNAME_READLINK, .func = { .vop_readlink = p9fs_readlink }},
	{ .name = VOPNAME_READ, .func = { .vop_read = p9fs_read }},
	{ .name = VOPNAME_SEEK, .func = { .vop_seek = p9fs_seek }},
	{ .name = VOPNAME_GETPAGE, .func = { .vop_getpage = p9fs_getpage }},
	{ .name = VOPNAME_INACTIVE, .func = { .vop_inactive = p9fs_inactive }},
	{ .name = NULL, .func = NULL },
};
