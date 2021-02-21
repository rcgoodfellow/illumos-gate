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

/*
 * XXX p9fs
 */

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/fs/p9fs_impl.h>


static ldi_ident_t p9fs_li;
static int p9fs_fstyp;
vfsops_t *p9fs_vfsops;
uint_t p9fs_next_ses_id;

static int
p9fs_mount(struct vfs *vfs, struct vnode *mv, struct mounta *uap,
    struct cred *cr)
{
	int e;
	int fromspace = (uap->flags & MS_SYSSPACE) ?
	    UIO_SYSSPACE : UIO_USERSPACE;
	struct pathname dir, spec;
	ldi_handle_t lh = NULL;
	dev_t dev;
	p9fs_t *p9 = NULL;

	if ((e = secpolicy_fs_mount(cr, mv, vfs)) != 0) {
		return (EPERM);
	}

	if (mv->v_type != VDIR) {
		return (ENOTDIR);
	}

	if (uap->flags & MS_REMOUNT) {
		return (ENOTSUP);
	}

	mutex_enter(&mv->v_lock);
	if (!(uap->flags & MS_OVERLAY) &&
	    (mv->v_count != 1 || (mv->v_flag & VROOT))) {
		mutex_exit(&mv->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mv->v_lock);

	if ((uap->flags & MS_DATA) || uap->datalen > 0) {
		/*
		 * Consumers must use MS_OPTIONSTR.
		 */
		return (EINVAL);
	}

	if ((e = pn_get(uap->dir, fromspace, &dir)) != 0) {
		return (e);
	}
	if ((e = pn_get(uap->spec, fromspace, &spec)) != 0) {
		pn_free(&dir);
		return (e);
	}
	cmn_err(CE_WARN, "p9fs: spec = %s", spec.pn_path);

	if (ldi_open_by_name(spec.pn_path, FREAD | FWRITE | FEXCL, cr, &lh,
	    p9fs_li) != 0 ||
	    ldi_get_dev(lh, &dev) != 0) {
		cmn_err(CE_WARN, "ldi open of %s failed", spec.pn_path);
		goto bail;
	}
	cmn_err(CE_WARN, "ldi open of %s ok!", spec.pn_path);

	p9 = kmem_zalloc(sizeof (*p9), KM_SLEEP);
	p9->p9_vfs = vfs;

	if (p9fs_session_init(&p9->p9_session, lh, p9fs_next_ses_id++) != 0) {
		cmn_err(CE_WARN, "p9fs session failure!");
		goto bail;
	}

	if (p9->p9_session->p9s_root_qid->qid_type != PLAN9_QIDTYPE_DIR) {
		cmn_err(CE_WARN, "p9fs: / is not a directory?");
		goto bail;
	}

	p9->p9_root = p9fs_make_node(p9, p9->p9_session->p9s_root_fid,
	    p9->p9_session->p9s_root_qid);
	p9->p9_root->p9n_vnode->v_flag |= VROOT;

	/*
	 * The LDI handle now belongs to the session.
	 */
	lh = NULL;

	vfs->vfs_data = p9;
	vfs->vfs_dev = dev;
	vfs->vfs_fstype = p9fs_fstyp;
	vfs_make_fsid(&vfs->vfs_fsid, dev, p9fs_fstyp);

	/*
	 * Create the root vnode.
	 */

	return (0);

bail:
	if (p9->p9_session != NULL) {
		p9fs_session_fini(p9->p9_session);
	}
	if (lh != NULL) {
		(void) ldi_close(lh, FREAD | FWRITE | FEXCL, cr);
	}
	pn_free(&spec);
	pn_free(&dir);
	return (EINVAL);
}

static int
p9fs_unmount(struct vfs *vfs, int flag, struct cred *cr)
{
	int e;

	if ((e = secpolicy_fs_unmount(cr, vfs)) != 0) {
		return (EPERM);
	}

	if (flag & MS_FORCE) {
		return (ENOTSUP);
	}

	/*
	 * XXX No unmount for now...
	 */
	return (EBUSY);
}

static int
p9fs_root(struct vfs *vfs, struct vnode **vnp)
{
	p9fs_t *p9 = vfs->vfs_data;
	vnode_t *vn = p9->p9_root->p9n_vnode;

	VN_HOLD(vn);
	*vnp = vn;
	return (0);
}

static int
p9fs_statvfs(struct vfs *vfs, struct statvfs64 *st)
{
	/*
	 * XXX
	 */
	return (EIO);
}

static int
p9fs_sync(struct vfs *vfs, short flag, struct cred *cr)
{
	/*
	 * XXX
	 */
	return (0);
}

static int
p9fs_vget(struct vfs *vfs, struct vnode **vnp, struct fid *fid)
{
	/*
	 * XXX
	 */
	return (EIO);
}

static const fs_operation_def_t p9fs_vfsops_template[] = {
	{ .name = VFSNAME_MOUNT, .func = { .vfs_mount = p9fs_mount }},
	{ .name = VFSNAME_UNMOUNT, .func = { .vfs_unmount = p9fs_unmount }},
	{ .name = VFSNAME_ROOT, .func = { .vfs_root = p9fs_root }},
	{ .name = VFSNAME_STATVFS, .func = { .vfs_statvfs = p9fs_statvfs }},
	{ .name = VFSNAME_SYNC, .func = { .vfs_sync = p9fs_sync }},
	{ .name = VFSNAME_VGET, .func = { .vfs_vget = p9fs_vget }},
	{ .name = NULL, .func = NULL },
};

static int
p9fs_init(int fstyp, char *name)
{
	int e;

	if ((e = vfs_setfsops(fstyp, p9fs_vfsops_template,
	    &p9fs_vfsops)) != 0) {
		cmn_err(CE_WARN, "p9fs: bad vfs ops template");
		return (e);
	}

	if ((e = vn_make_ops(name, p9fs_vnodeops_template,
	    &p9fs_vnodeops)) != 0) {
		(void) vfs_freevfsops_by_type(fstyp);
		cmn_err(CE_WARN, "p9fs: bad vnode ops template");
		return (e);
	}

	p9fs_fstyp = fstyp;

	return (0);
}

static mntopt_t p9fs_mntopts_list[] = {
};

static mntopts_t p9fs_mntopts = {
	.mo_list =		p9fs_mntopts_list,
	.mo_count =		ARRAY_SIZE(p9fs_mntopts_list),
};

static vfsdef_t p9fs_vfsdev = {
	.def_version =		VFSDEF_VERSION,
	.name =			"p9fs",
	.init =			p9fs_init,
	.flags =		VSW_HASPROTO,
	.optproto =		&p9fs_mntopts,
};

static struct modlfs p9fs_modlfs = {
	.fs_modops =		&mod_fsops,
	.fs_linkinfo =		"plan 9 file system (9P2000.u)",
	.fs_vfsdef =		&p9fs_vfsdev,
};

static struct modlinkage p9fs_modlinkage = {
	.ml_rev =		MODREV_1,
	.ml_linkage =		{ &p9fs_modlfs, NULL },
};

int
_init(void)
{
	int r;

	if ((r = mod_install(&p9fs_modlinkage)) != 0) {
		return (r);
	}

	VERIFY0(ldi_ident_from_mod(&p9fs_modlinkage, &p9fs_li));

	return (r);
}

int
_info(struct modinfo *mip)
{
	return (mod_info(&p9fs_modlinkage, mip));
}

int
_fini(void)
{
	int r;

	if ((r = mod_remove(&p9fs_modlinkage)) == 0) {
		ldi_ident_release(p9fs_li);
		p9fs_li = NULL;
	}

	return (r);
}
