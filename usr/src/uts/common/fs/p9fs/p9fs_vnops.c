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
	v->v_flag = VROOT;

	p9fs_node_t *p9n = kmem_zalloc(sizeof (*p9n), KM_SLEEP);
	p9n->p9n_fs = p9;
	p9n->p9n_fid = fid;
	p9n->p9n_qid = *qid;

	p9n->p9n_vnode = v;
	v->v_data = p9n;
	v->v_vfsp = p9->p9_vfs;

	return (p9n);
}

const fs_operation_def_t p9fs_vnodeops_template[] = {
	{ .name = VOPNAME_GETATTR, .func = { .vop_getattr = p9fs_getattr }},
	{ .name = NULL, .func = NULL },
};
