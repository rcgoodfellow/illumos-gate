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
#include <sys/fs/p9fs_impl.h>

/*
 * XXX p9fs
 */

struct vnodeops *p9fs_vnodeops;

static int
p9fs_getattr(struct vnode *v, struct vattr *va, int flags, struct cred *cr,
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
p9fs_make_node(p9fs_t *p9, uint32_t fid, p9fs_qid_t *qid)
{
	struct vnode *v = vn_alloc(KM_SLEEP);
	vn_setops(v, p9fs_vnodeops);
	v->v_type = VDIR;

	p9fs_node_t *p9n = kmem_zalloc(sizeof (*p9n), KM_SLEEP);
	p9n->p9n_fs = p9;
	p9n->p9n_fid = fid;
	p9n->p9n_qid = *qid;

	p9n->p9n_vnode = v;
	v->v_data = p9n;
	v->v_vfsp = p9->p9_vfs;

	return (p9n);
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
p9fs_readdir(struct vnode *v, struct uio *uio, struct cred *cr,
    int *eof, caller_context_t *ct, int flags)
{
	p9fs_node_t *p9n = v->v_data;
	p9fs_t *p9 = p9n->p9n_fs;
	p9fs_session_t *p9s = p9->p9_session;
	int r;

	/*
	 * XXX Deal only with the root directory for now
	 */
	if (!(v->v_flag & VROOT)) {
		mutex_exit(&p9n->p9n_mutex);
		return (EIO);
	}

	/*
	 * XXX Each "byte" in our offset will represent a single directory.
	 */
	offset_t offset = uio->uio_loffset;
	offset_t orig_offset = offset;
	if (eof != NULL) {
		*eof = 0;
	}

	/*
	 * XXX This is totally serialised for now.
	 */
	mutex_enter(&p9n->p9n_mutex);

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

const fs_operation_def_t p9fs_vnodeops_template[] = {
	{ .name = VOPNAME_GETATTR, .func = { .vop_getattr = p9fs_getattr }},
	{ .name = VOPNAME_OPEN, .func = { .vop_open = p9fs_open }},
	{ .name = VOPNAME_CLOSE, .func = { .vop_close = p9fs_close }},
	{ .name = VOPNAME_ACCESS, .func = { .vop_access = p9fs_access }},
	{ .name = VOPNAME_READDIR, .func = { .vop_readdir = p9fs_readdir }},
	{ .name = NULL, .func = NULL },
};
