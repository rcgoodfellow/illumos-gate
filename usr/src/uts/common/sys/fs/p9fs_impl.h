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

#ifndef _SYS_FS_P9FS_IMPL_H
#define	_SYS_FS_P9FS_IMPL_H

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>

/*
 * XXX p9fs
 */

#ifdef __cplusplus
extern "C" {
#endif

extern const struct fs_operation_def p9fs_vnodeops_template[];
extern struct vnodeops *p9fs_vnodeops;

typedef struct p9fs {
	vnode_t *p9_root;
} p9fs_t;

typedef struct p9fs_session {
	ldi_handle_t p9s_ldi;
} p9fs_session_t;

typedef struct p9fs_req {
} p9fs_req_t;


extern int p9fs_session_init(p9fs_session_t **p9s, ldi_handle_t lh);
extern void p9fs_session_fini(p9fs_session_t *p9s);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FS_P9FS_IMPL_H */
