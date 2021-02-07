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

#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/fs/p9fs_impl.h>

static int
p9fs_mount(struct vfs *vfs, struct vnode *vn, struct mounta *uap,
    struct cred *cr)
{
	int e;

	if ((e = secpolicy_fs_mount(cr, vn, vfs)) != 0) {
		return (EPERM);
	}

	return (EINVAL);
}

static int
p9fs_unmount(struct vfs *vfs, struct vnode *vn, struct mounta *uap,
    struct cred *cr)
{
	int e;

	if ((e = secpolicy_fs_unmount(cr, vn, vfs)) != 0) {
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

static const fs_operation_def_t p9fs_vfsops_template[] = {
	{ .name = VFSNAME_MOUNT, .func = { .vfs_mount = p9fs_mount }},
	{ .name = VFSNAME_UNMOUNT, .func = { .vfs_unmount = p9fs_unmount }},
	{ .name = NULL, .func = NULL },
};

static int p9fs_fstyp;
vfsopts_t *p9fs_vfsops;

static int
p9fs_init(int fstyp, char *name)
{
	int e;

	if ((e = vfs_setfsops(fstyp, p9fs_vfsops_template,
	    &p9fs_vfsops)) != 0) {
		cmn_err(CE_WARN, "p9fs: bad vfs ops template");
		return (error);
	}

	if ((e = vn_make_ops(name, p9fs_vnops_template,
	    &p9fs_vnodeops)) != 0) {
		(void) vfs_freevfsops_by_type(fstyp);
		cmn_err(CE_WARN, "p9fs: bad vnode ops template");
		return (error);
	}

	p9fs_fstyp = fstyp;

	return (0);
}

static mntopts_t p9fs_mntopts_list[] = {
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
}

int
_init(void)
{
	int r;

	if ((r = mod_install(&p9fs_modlinkage)) != 0) {
		/*
		 * XXX fail
		 */
	}

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
		/*
		 * XXX cleanup
		 */
	}

	return (r);
}
