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
#include <sys/stdbool.h>

/*
 * XXX p9fs
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XXX 9P constants
 */
typedef enum plan9_qidtype {
	PLAN9_QIDTYPE_DIR =	0x80,
	PLAN9_QIDTYPE_APPEND =	0x40,
	PLAN9_QIDTYPE_EXCL =	0x20,
	PLAN9_QIDTYPE_MOUNT =	0x10,
	PLAN9_QIDTYPE_AUTH =	0x08,
	PLAN9_QIDTYPE_TMP =	0x04,
	PLAN9_QIDTYPE_SYMLINK =	0x02,
	PLAN9_QIDTYPE_LINK =	0x01,
	PLAN9_QIDTYPE_FILE =	0x00,
} plan9_qidtype_t;

typedef enum plan9_mode {
	PLAN9_MODE_DIR =	0x80000000,
	PLAN9_MODE_APPEND =	0x40000000,
	PLAN9_MODE_EXCL =	0x20000000,
	PLAN9_MODE_MOUNT =	0x10000000,
	PLAN9_MODE_AUTH =	0x08000000,
	PLAN9_MODE_TMP =	0x04000000,

	PLAN9_MODE_U_R =	0400,
	PLAN9_MODE_U_W =	0200,
	PLAN9_MODE_U_X =	0100,

	PLAN9_MODE_G_R =	0040,
	PLAN9_MODE_G_W =	0020,
	PLAN9_MODE_G_X =	0010,

	PLAN9_MODE_O_R =	0004,
	PLAN9_MODE_O_W =	0002,
	PLAN9_MODE_O_X =	0001,

	/*
	 * 9P2000.u:
	 */
	PLAN9_MODE_SYMLINK =	0x02000000,
	PLAN9_MODE_DEVICE =	0x00800000,
	PLAN9_MODE_NAMED_PIPE =	0x00200000,
	PLAN9_MODE_SOCKET =	0x00100000,
	PLAN9_MODE_SETUID =	0x00080000,
	PLAN9_MODE_SETGID =	0x00040000,
} plan9_mode_t;

#define	PLAN9_MODE_U	(PLAN9_MODE_U_R | PLAN9_MODE_U_W | PLAN9_MODE_U_X)
#define	PLAN9_MODE_G	(PLAN9_MODE_G_R | PLAN9_MODE_G_W | PLAN9_MODE_G_X)
#define	PLAN9_MODE_O	(PLAN9_MODE_O_R | PLAN9_MODE_O_W | PLAN9_MODE_O_X)
#define	PLAN9_PERM	(PLAN9_MODE_U | PLAN9_MODE_G | PLAN9_MODE_O)

extern const struct fs_operation_def p9fs_vnodeops_template[];
extern struct vnodeops *p9fs_vnodeops;

typedef struct p9fs_qid {
	plan9_qidtype_t qid_type;
	uint32_t qid_version;
	uint64_t qid_path;
} p9fs_qid_t;

typedef struct reqbuf reqbuf_t;

typedef struct p9fs_session {
	uint_t p9s_id;
	ldi_handle_t p9s_ldi;
	kmutex_t p9s_mutex;
	size_t p9s_msize;
	uint16_t p9s_next_tag;

	id_space_t *p9s_fid_space;

	/*
	 * XXX
	 */
	reqbuf_t *p9s_send;
	reqbuf_t *p9s_recv;

	p9fs_qid_t *p9s_root_qid;
	uint32_t p9s_root_fid;
} p9fs_session_t;

typedef struct p9fs {
	vfs_t *p9_vfs;
	struct p9fs_node *p9_root;
	p9fs_session_t *p9_session;
} p9fs_t;

typedef struct p9fs_node {
	p9fs_t *p9n_fs;
	vnode_t *p9n_vnode;
	uint32_t p9n_fid;
	p9fs_qid_t p9n_qid;

	kmutex_t p9n_mutex;
	struct p9fs_readdir *p9n_readdir;
} p9fs_node_t;

typedef struct p9fs_readdir_ent {
	p9fs_qid_t p9de_qid;
	offset_t p9de_ord;
	char *p9de_name;
	list_node_t p9de_link;
} p9fs_readdir_ent_t;

typedef struct p9fs_readdir {
	uint32_t p9rd_fid;
	bool p9rd_eof;
	list_t p9rd_ents;
	uint64_t p9rd_next_offset;
	offset_t p9rd_next_ord;
} p9fs_readdir_t;

typedef struct p9fs_req {
} p9fs_req_t;

/*
 * Unpacked RSTAT response, with some skipped fields:
 */
typedef struct p9fs_stat {
	p9fs_qid_t *p9st_qid;
	plan9_mode_t p9st_mode;
	uint32_t p9st_atime;
	uint32_t p9st_mtime;
	uint64_t p9st_length;
	char *p9st_name;
	char *p9st_extension;
	uint32_t p9st_uid;
	uint32_t p9st_gid;
	uint32_t p9st_muid;
} p9fs_stat_t;

extern int p9fs_session_init(p9fs_session_t **p9s, ldi_handle_t lh, uint_t);
extern void p9fs_session_fini(p9fs_session_t *p9s);
extern void p9fs_session_lock(p9fs_session_t *p9s);
extern void p9fs_session_unlock(p9fs_session_t *p9s);
extern int p9fs_session_stat(p9fs_session_t *, uint32_t, p9fs_stat_t *);
extern void p9fs_session_stat_reset(p9fs_stat_t *);
extern int p9fs_session_readdir(p9fs_session_t *, uint32_t, p9fs_readdir_t **);
extern void p9fs_session_readdir_ent_free(p9fs_readdir_ent_t *);
extern void p9fs_session_readdir_free(p9fs_session_t *, p9fs_readdir_t *);
extern int p9fs_session_readdir_next(p9fs_session_t *, p9fs_readdir_t *);


extern p9fs_node_t *p9fs_make_node(p9fs_t *p9, uint32_t fid, p9fs_qid_t *qid);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FS_P9FS_IMPL_H */
