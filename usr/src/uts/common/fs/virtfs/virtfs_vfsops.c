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
 * XXX virtfs
 */

#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/sysmacros.h>
#include <sys/fs/virtfs_impl.h>

static int
virtfs_mount(struct vfs *vfs, struct vnode *vn, struct mounta *uap,
    struct cred *cr)
{
	int e;

	if ((e = secpolicy_fs_mount(cr, vn, vfs)) != 0) {
		return (EPERM);
	}

	return (EINVAL);
}

static int
virtfs_unmount(struct vfs *vfs, struct vnode *vn, struct mounta *uap,
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

static const fs_operation_def_t virtfs_vfsops_template[] = {
	{ .name = VFSNAME_MOUNT, .func = { .vfs_mount = virtfs_mount }},
	{ .name = VFSNAME_UNMOUNT, .func = { .vfs_unmount = virtfs_unmount }},
	{ .name = NULL, .func = NULL },
};

static int virtfs_fstyp;
vfsopts_t *virtfs_vfsops;

static int
virtfs_init(int fstyp, char *name)
{
	int e;

	if ((e = vfs_setfsops(fstyp, virtfs_vfsops_template,
	    &virtfs_vfsops)) != 0) {
		cmn_err(CE_WARN, "virtfs: bad vfs ops template");
		return (error);
	}

	if ((e = vn_make_ops(name, virtfs_vnops_template,
	    &virtfs_vnodeops)) != 0) {
		(void) vfs_freevfsops_by_type(fstyp);
		cmn_err(CE_WARN, "virtfs: bad vnode ops template");
		return (error);
	}

	virtfs_fstyp = fstyp;

	return (0);
}

static mntopts_t virtfs_mntopts_list[] = {
};

static mntopts_t virtfs_mntopts = {
	.mo_list =		virtfs_mntopts_list,
	.mo_count =		ARRAY_SIZE(virtfs_mntopts_list),
};

static vfsdef_t virtfs_vfsdev = {
	.def_version =		VFSDEF_VERSION,
	.name =			"virtfs",
	.init =			virtfs_init,
	.flags =		VSW_HASPROTO,
	.optproto =		&virtfs_mntopts,
};

static struct modlfs virtfs_modlfs = {
	.fs_modops =		&mod_fsops,
	.fs_linkinfo =		"virtio file system",
	.fs_vfsdef =		&virtfs_vfsdev,
}

int
_init(void)
{
	int r;

	if ((r = mod_install(&virtfs_modlinkage)) != 0) {
		/*
		 * XXX fail
		 */
	}

	return (r);
}

int
_info(struct modinfo *mip)
{
	return (mod_info(&virtfs_modlinkage, mip));
}

int
_fini(void)
{
	int r;

	if ((r = mod_remove(&virtfs_modlinkage)) == 0) {
		/*
		 * XXX cleanup
		 */
	}

	return (r);
}
