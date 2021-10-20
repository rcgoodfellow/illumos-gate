/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 1997-1998 by Sun Microsystems, Inc.
 * All rights reserved.
 * Copyright 2021 Oxide Computer Co.
 */

#ifndef	_SYS_MEMLIST_IMPL_H
#define	_SYS_MEMLIST_IMPL_H

/*
 * Common memlist routines.
 */

#include <sys/memlist.h>
#include <sys/mutex.h>

#ifdef __cplusplus
extern "C" {
#endif

struct memlist_pool {
	memlist_t *mp_freelist;
	uint_t mp_freelist_count;
	kmutex_t mp_freelist_mutex;
	uint_t mp_flags;
};

#define	MEMLP_FL_EARLYBOOT	1

extern struct memlist *memlist_get_one(void);
extern void memlist_free_one(struct memlist *);
extern void memlist_free_list(struct memlist *);
extern void memlist_free_block(caddr_t, size_t);
extern void memlist_insert(struct memlist *, struct memlist **);
extern void memlist_del(struct memlist *, struct memlist **);
extern struct memlist *memlist_find(struct memlist *, uint64_t);

extern struct memlist *xmemlist_get_one(struct memlist_pool *);
extern void xmemlist_free_one(struct memlist_pool *, struct memlist *);
extern void xmemlist_free_list(struct memlist_pool *, struct memlist *);
extern void xmemlist_free_block(struct memlist_pool *, caddr_t, size_t);

#define	MEML_SPANOP_OK		0
#define	MEML_SPANOP_ESPAN	1
#define	MEML_SPANOP_EALLOC	2

/*
 * Optional for span operations: allow munging (relaxed coalescing).  When set,
 * the span to be added or deleted from the list may overlap multiple existing
 * entries and/or addresses not contained within the list.  See notes in
 * memlist_new.c.
 */
#define	MEML_FL_RELAXED	1

extern int memlist_add_span(uint64_t, uint64_t, struct memlist **);
extern int memlist_delete_span(uint64_t, uint64_t, struct memlist **);
extern int xmemlist_add_span(struct memlist_pool *, uint64_t, uint64_t,
    struct memlist **, uint64_t);
extern int xmemlist_delete_span(struct memlist_pool *, uint64_t, uint64_t,
    struct memlist **, uint64_t);

#ifdef __cplusplus
}
#endif

#endif	/* _SYS_MEMLIST_IMPL_H */
