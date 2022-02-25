/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2022 Oxide Computer Co.
 */

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/mutex.h>
#include <sys/param.h>		/* for NULL */
#include <sys/debug.h>
#include <sys/memlist.h>
#include <sys/memlist_impl.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>

/*
 * For compatibility with existing callers, the "normal" versions of all our
 * routines use a single shared pool that is guaranteed to contain only
 * memlists allocated above KERNELBASE and intended to persist after boot.
 * The extended versions allow for maintenance of other pools separate from
 * that one which may or may not have that property.  Callers of the extended
 * functions are responsible for managing their own pools, but not for
 * locking.
 */
static memlist_pool_t pool;

/*
 * In order to use these routines early in boot (before %gs is set on x86, in
 * particular), we need an escape hatch from locking.  The default pool will
 * be locked, as it has always been, and is not suitable for use in early boot.
 * Callers wishing to create such a pool must set MEMLP_FL_EARLYBOOT, and are
 * responsible for ensuring that the pool's freelist is accessed only when
 * single-threaded or under the protection of some other lock.
 */
#define	MEMLP_LOCK(_p)	\
	do {		\
		if (((_p)->mp_flags & MEMLP_FL_EARLYBOOT) == 0) {	\
			mutex_enter(&(_p)->mp_freelist_mutex);		\
		}							\
	} while (0)

#define	MEMLP_UNLOCK(_p)	\
	do {		\
		if (((_p)->mp_flags & MEMLP_FL_EARLYBOOT) == 0) {	\
			mutex_exit(&(_p)->mp_freelist_mutex);		\
		}							\
	} while (0)

/*
 * Caller must test for NULL return.
 */
struct memlist *
xmemlist_get_one(memlist_pool_t *mpp)
{
	struct memlist *mlp;

	MEMLP_LOCK(mpp);
	mlp = mpp->mp_freelist;
	if (mlp != NULL) {
		mpp->mp_freelist = mlp->ml_next;
		ASSERT(mpp->mp_freelist_count > 0);
		mpp->mp_freelist_count--;
	}
	MEMLP_UNLOCK(mpp);

	return (mlp);
}

struct memlist *
memlist_get_one(void)
{
	return (xmemlist_get_one(&pool));
}

void
xmemlist_free_one(memlist_pool_t *mpp, struct memlist *mlp)
{
	ASSERT(mlp != NULL);

	MEMLP_LOCK(mpp);
	mlp->ml_next = mpp->mp_freelist;
	mpp->mp_freelist = mlp;
	mpp->mp_freelist_count++;
	MEMLP_UNLOCK(mpp);
}

void
memlist_free_one(struct memlist *mlp)
{
	xmemlist_free_one(&pool, mlp);
}

void
xmemlist_free_list(memlist_pool_t *mpp, struct memlist *mlp)
{
	struct memlist *mlendp;
	uint_t count;

	if (mlp == NULL) {
		return;
	}

	count = 1;
	for (mlendp = mlp; mlendp->ml_next != NULL; mlendp = mlendp->ml_next)
		count++;
	MEMLP_LOCK(mpp);
	mlendp->ml_next = mpp->mp_freelist;
	mpp->mp_freelist = mlp;
	mpp->mp_freelist_count += count;
	MEMLP_UNLOCK(mpp);
}

void
memlist_free_list(struct memlist *mlp)
{
	xmemlist_free_list(&pool, mlp);
}

void
xmemlist_free_block(memlist_pool_t *mpp, caddr_t base, size_t bytes)
{
	struct memlist *mlp, *mlendp;
	uint_t count;

	count = bytes / sizeof (struct memlist);
	if (count == 0)
		return;

	mlp = (struct memlist *)base;
	mlendp = &mlp[count - 1];
	for (; mlp != mlendp; mlp++)
		mlp->ml_next = mlp + 1;
	mlendp->ml_next = NULL;
	mlp = (struct memlist *)base;
	MEMLP_LOCK(mpp);
	mlendp->ml_next = mpp->mp_freelist;
	mpp->mp_freelist = mlp;
	mpp->mp_freelist_count += count;
	MEMLP_UNLOCK(mpp);
}

void
memlist_free_block(caddr_t base, size_t bytes)
{
	xmemlist_free_block(&pool, base, bytes);
}

/*
 * Not to belabour this, but it's worth thinking about every case to clearly
 * define the semantics of these functions.  The semantics of memlist_del() are
 * obvious: the entry to be removed must be an existing discrete entry on the
 * list.  The semantics of insertion and the two span operations, however,
 * are not.  We define them here by (and to) exhaustion.  This explanation can
 * undoubtedly be simplified.
 *
 * We define operations on a zero-length (N.size == 0) region:
 *
 * insert(N, L): Insert N immediately before the first existing entry A in L
 * whose starting address A.addr is greater than or equal to the new entry's
 * address N.addr.
 *
 * add_span(P, addr, 0, L, RELAXED): nop
 *
 * add_span(P, addr, 0, L, 0):
 *   1. allocate N = (addr, 0) from P
 *   2. insert(N, L)
 *   3. if an existing entry A in L has A.addr == addr or
 *       A.addr + A.size == addr, remove N and free N into P
 *
 * Note that there is considerable opportunity to optimise this case if desired
 * but it's difficult to be certain that no consumers depend on the presence of
 * empty regions.
 *
 * delete_span(P, addr, 0, L, RELAXED): nop
 *
 * delete_span(P, addr, 0, L, 0):
 *   1. if an existing entry A in L has A.addr == addr and A.size == 0,
 *       remove A from L and free it into P
 *   2. otherwise, do nothing
 *
 * Having exhaustively defined the semantics of these operations with respect
 * to empty new entries, there are 41 distinct general cases depending on where
 * the entry to be inserted N fits relative to two existing entries A and B
 * (all cases involving existing lists with only 0 or 1 elements on them are
 * degenerate instances of one of these).  We assume there is no region in the
 * list before A that overlaps or is adjacent to N, and there are no non-empty
 * regions between A and B.  Additional regions, which may be adjacent to or
 * overlap with B and/or N, may lie beyond B; these are subcases where our
 * original B becomes the new A.
 *
 * case 0: before A, before A
 *  |    N    |
 *                |    A    |                        |    B    |
 * case 1: before A, at A
 *      |    N    |
 *                |    A    |                        |    B    |
 * case 2: before A, within A
 *         |    N    |
 *                |    A    |                        |    B    |
 * case 3: before A, end of A
 *         |       N        |
 *                |    A    |                        |    B    |
 * case 4: before A, between A and B
 *         |          N           |
 *                |    A    |                        |    B    |
 * case 5: before A, at B
 *         |                    N                    |
 *                |    A    |                        |    B    |
 * case 6: before A, within B
 *         |                      N                      |
 *                |    A    |                        |    B    |
 * case 7: before A, end of B
 *         |                        N                          |
 *                |    A    |                        |    B    |
 * case 8: before A, beyond B
 *         |                           N                           |
 *                |    A    |                        |    B    |
 * case 9: at A, within A
 *                |  N  |
 *                |    A    |                        |    B    |
 * case 10: at A, end of A
 *                |    N    |
 *                |    A    |                        |    B    |
 * case 11: at A, between A and B
 *                |      N      |
 *                |    A    |                        |    B    |
 * case 12: at A, at B
 *                |                 N                |
 *                |    A    |                        |    B    |
 * case 13: at A, within B
 *                |                   N                  |
 *                |    A    |                        |    B    |
 * case 14: at A, end of B
 *                |                      N                     |
 *                |    A    |                        |    B    |
 * case 15: at A, beyond B
 *                |                        N                     |
 *                |    A    |                        |    B    |
 * case 16: within A, within A
 *                  |  N  |
 *                |    A    |                        |    B    |
 * case 17: within A, end of A
 *                    |  N  |
 *                |    A    |                        |    B    |
 * case 18: within A, between A and B
 *                     |    N    |
 *                |    A    |                        |    B    |
 * case 19: within A, at B
 *                     |              N              |
 *                |    A    |                        |    B    |
 * case 20: within A, within B
 *                       |                N               |
 *                |    A    |                        |    B    |
 * case 21: within A, end of B
 *                       |                  N                  |
 *                |    A    |                        |    B    |
 * case 22: within A, beyond B
 *                       |                    N                    |
 *                |    A    |                        |    B    |
 * case 23: end of A, between A and B
 *                          |    N    |
 *                |    A    |                        |    B    |
 * case 24: end of A, at B
 *                          |            N           |
 *                |    A    |                        |    B    |
 * case 25: end of A, within B
 *                          |             N             |
 *                |    A    |                        |    B    |
 * case 26: end of A, end of B
 *                          |                 N                |
 *                |    A    |                        |    B    |
 * case 27: end of A, beyond B
 *                          |                   N                   |
 *                |    A    |                        |    B    |
 * case 28: between A and B, between A and B
 *                                |    N    |
 *                |    A    |                        |    B    |
 * case 29: between A and B, at B
 *                                         |    N    |
 *                |    A    |                        |    B    |
 * case 30: betweem A and B, within B
 *                                              |    N    |
 *                |    A    |                        |    B    |
 * case 31: between A and B, end of B
 *                                              |      N       |
 *                |    A    |                        |    B    |
 * case 32: between A and B, beyond B
 *                                              |        N        |
 *                |    A    |                        |    B    |
 * case 33: at B, within B
 *                                                   |  N  |
 *                |    A    |                        |    B    |
 * case 34: at B, end of B
 *                                                   |    N    |
 *                |    A    |                        |    B    |
 * case 35: at B, beyond B
 *                                                   |      N      |
 *                |    A    |                        |    B    |
 * case 36: within B, within B
 *                                                     |  N  |
 *                |    A    |                        |    B    |
 * case 37: within B, end of B
 *                                                       |  N  |
 *                |    A    |                        |    B    |
 * case 38: within B, beyond B
 *                                                        |    N    |
 *                |    A    |                        |    B    |
 * case 39: end of B, beyond B
 *                                                             |    N    |
 *                |    A    |                        |    B    |
 * case 40: beyond B, beyond B
 *                                                               |    N    |
 *                |    A    |                        |    B    |
 *
 * Now, insert(N, L) non-coalescing insertion -- is defined as follows:
 *
 * cases 0 and 1: insert N into L immediately prior to the first A such that
 * A.addr >= N.addr + N.size.  All entries at address A.addr will follow N,
 * including entries of zero size.
 *
 * cases 23, 24, 28, and 29: insert N into L immediately prior to the first B
 * such that B.addr >= N.addr + N.size.  Note that there may be additional
 * empty entries between A and N after insertion.
 *
 * cases 39 and 40: insert N into L immediately prior to the first non-empty
 * element beyond B.  If no such element exists, insert it at the end.
 *
 * All other cases are considered programmer error and will result in a panic.
 *
 * Finally, the span operations; let's begin with non-relaxed addition.
 *
 * add_span(P, addr, size, L, 0) where N === (addr, size):
 *
 * cases 0, 28, and 40: equivalent to insert(N, L).
 *
 * cases 1 and 29 are identical unless A and B are adjacent.  Expand A or B,
 * respectively, to start at N.addr, increasing A.size or B.size, respectively,
 * by N.size.  If A and B are adjacent, N.size must be 0 and this is a nop in
 * case 29.
 *
 * cases 23 and 39 are identical unless A and B are adjacent.  Expand A or B,
 * respectively, so that A.size or B.size is increased by N.size.  If A and B
 * are adjacent, N.size must be 0 and this is a nop in case 23.
 *
 * case 24: If A and B are adjacent, N.size must be 0 and this is a nop.  Else,
 * set B.addr == A.addr and B.size == A.size + N.size + B.size.  Free A into P.
 *
 * All other cases return MEML_SPANOP_ESPAN (except on allocation failure, in
 * which case MEML_SPANOP_EALLOC is returned instead).
 *
 * Relaxed span addition allows all 41 cases.
 *
 * add_span(P, addr, size, L, RELAXED) where N === (addr, size):
 *
 * cases 0, 28, and 40: equivalent to insert(N, L).
 *
 * cases 1-4, 9-11, 16-18, 23: Subsume N into A:
 *	let start = MIN(A.addr, N.addr)
 *	let end = MAX(A.addr + A.size, N.addr + N.size)
 *	A.addr = start
 *	A.size = end - start
 *
 * cases 29-31, 33-34, 36-37: Subsume N into B:
 *	let start = MIN(B.addr, N.addr)
 *	let end = MAX(B.addr + B.size, N.addr + N.size)
 *	B.addr = start
 *	B.size = end - start
 *
 * cases 5-7, 12-14, 19-21, 24-26: Subsume A and N into B:
 *	let start = MIN(A.addr, N.addr)
 *	let end = MAX(B.addr + B.size, N.addr + N.size)
 *	B.addr = start
 *	B.size = end - start
 *	free A into P
 *
 * cases 8, 15, 22, 27, 32, 35, 38-39: Subsume A and N into B as for cases 5-7
 * et al.  Then, if a region C exists in L after B such that
 * B.addr + B.size >= C.addr:
 *	let N = B
 *	let A = C
 *	add_span(P, N.addr, N.size, L, RELAXED)
 *
 * Note that our actual implementation is iterative rather than recursive, but
 * is equivalent.
 *
 * Non-relaxed span deletion is much more straightforward.
 *
 * delete_span(P, addr, size, L, 0) where N === (addr, size):
 *
 * Assume that A and B are adjacent.  Note that no coalescing is done after
 * the deletion, even if remaining regions are adjacent.  If A and B are not
 * adjacent, all degenerate cases fail with MEML_SPANOP_ESPAN.
 *
 * cases 0-8, 11-15, 18-22: fail with MEML_SPANOP_ESPAN.
 *
 * case 9: A.addr = N.addr + N.size, A.size is decreased by N.size.
 *
 * case 10: equivalent to delete(A, L).
 *
 * case 16: Split A:
 *	let end = A.addr + A.size
 *	A.size = N.addr - A.size
 *	N.addr = N.addr + N.size
 *	N.size = end - N.addr
 *	insert N into L after A
 *
 * case 17: A.size is decreased by N.size.
 *
 * cases 23-24, 28-29: N.size === 0; this is a nop.
 *
 * cases 25-27 (degenerate) reduce to cases 9-11 because A and B are adjacent.
 *
 * cases 30-32 (degenerate) reduce to cases 2-4 because A and B are adjacent.
 *
 * cases 33-35 (degenerate) reduce to cases 9-11.
 *
 * cases 36-37 (degenerate) reduce to cases 16-17.
 *
 * cases 38-40: fail with MEML_SPANOP_ESPAN.
 *
 * Finally, relaxed span deletion is nearly the same as its non-relaxed cousin,
 * except that it never fails with MEML_SPANOP_ESPAN.  Deletions of nonexistent
 * regions is instead a nop.
 *
 * delete_span(P, addr, size, L, RELAXED):
 *
 * Divide N into 5 exhaustive regions Nk such that Nk and N(k-1) are
 * adjacent and non-overlapping for all k > 0 and
 *
 * N0.addr + N0.size <= A.addr,
 * N1.addr >= A.addr and N1.addr + N1.size <= A.addr + A.size,
 * N2.addr >= A.addr + A.size and N2.addr + N2.size <= B.addr,
 * N3.addr >= B.addr and N3.addr + N3.size <= B.addr + B.size,
 * N4.addr >= B.addr + B.size
 *
 * Then,
 *	delete_span(P, N1.addr, N1.size, L, 0)
 *	delete_span(P, N3.addr, N3.size, L, 0)
 *	delete_span(P, N4.addr, N4.size, L, RELAXED)
 *
 * In plain English, we ignore all parts of N that don't overlap any existing
 * region in the list, and delete spans corresponding to the parts of N that
 * do.  The recursive definition here is once again merely a semantic shorthand
 * addressing the need to consider possible non-empty regions overlapping N
 * beyond B; the implementation is both iterative and much simpler to
 * understand.
 */

void
memlist_insert(
	struct memlist *new,
	struct memlist **curmemlistp)
{
	struct memlist *cur, *last;
	uint64_t start, end;

	start = new->ml_address;
	end = start + new->ml_size;
	last = NULL;
	for (cur = *curmemlistp; cur; cur = cur->ml_next) {
		last = cur;
		if (cur->ml_address >= end) {
			new->ml_next = cur;
			new->ml_prev = cur->ml_prev;
			cur->ml_prev = new;
			if (cur == *curmemlistp)
				*curmemlistp = new;
			else
				new->ml_prev->ml_next = new;
			return;
		}
		if (cur->ml_address + cur->ml_size > start)
			panic("munged memory list = 0x%p\n",
			    (void *)curmemlistp);
	}
	new->ml_next = NULL;
	new->ml_prev = last;
	if (last != NULL) {
		last->ml_next = new;
	} else {
		ASSERT3P(*curmemlistp, ==, NULL);
		*curmemlistp = new;
	}
}

void
memlist_del(struct memlist *memlistp,
    struct memlist **curmemlistp)
{
#ifdef DEBUG
	/*
	 * Check that the memlist is on the list.
	 */
	struct memlist *mlp;

	for (mlp = *curmemlistp; mlp != NULL; mlp = mlp->ml_next)
		if (mlp == memlistp)
			break;
	ASSERT(mlp == memlistp);
#endif /* DEBUG */
	if (*curmemlistp == memlistp) {
		ASSERT(memlistp->ml_prev == NULL);
		*curmemlistp = memlistp->ml_next;
	}
	if (memlistp->ml_prev != NULL) {
		ASSERT(memlistp->ml_prev->ml_next == memlistp);
		memlistp->ml_prev->ml_next = memlistp->ml_next;
	}
	if (memlistp->ml_next != NULL) {
		ASSERT(memlistp->ml_next->ml_prev == memlistp);
		memlistp->ml_next->ml_prev = memlistp->ml_prev;
	}
}

struct memlist *
memlist_find(struct memlist *mlp, uint64_t address)
{
	for (; mlp != NULL; mlp = mlp->ml_next)
		if (address >= mlp->ml_address &&
		    address < (mlp->ml_address + mlp->ml_size))
			break;
	return (mlp);
}

/*
 * Add a span to a memlist.
 * Return:
 * MEML_SPANOP_OK if OK.
 * MEML_SPANOP_ESPAN if part or all of span already exists
 * MEML_SPANOP_EALLOC for allocation failure
 */
int
xmemlist_add_span(
	memlist_pool_t *mpp,
	uint64_t address,
	uint64_t bytes,
	struct memlist **curmemlistp,
	uint64_t flags)
{
	struct memlist *dst;
	struct memlist *prev, *next;

	/*
	 * allocate a new struct memlist
	 */

	dst = xmemlist_get_one(mpp);

	if (dst == NULL) {
		return (MEML_SPANOP_EALLOC);
	}

	dst->ml_address = address;
	dst->ml_size = bytes;

	/*
	 * First insert.
	 */
	if (*curmemlistp == NULL) {
		dst->ml_prev = NULL;
		dst->ml_next = NULL;
		*curmemlistp = dst;
		return (MEML_SPANOP_OK);
	}

	/*
	 * Insert into sorted list.
	 */
	for (prev = NULL, next = *curmemlistp; next != NULL;
	    prev = next, next = next->ml_next) {
		if (address > (next->ml_address + next->ml_size))
			continue;

		/*
		 * Else insert here.
		 */

		if ((flags & MEML_FL_RELAXED) != 0) {
			uint64_t start, end;

			/*
			 * No overlap or adjacency, just insert and we're done.
			 */
			if (address + bytes < next->ml_address) {
				dst->ml_prev = prev;
				dst->ml_next = next;
				next->ml_prev = dst;
				if (prev == NULL) {
					*curmemlistp = dst;
				} else {
					prev->ml_next = dst;
				}

				return (MEML_SPANOP_OK);
			}

			/*
			 * Coalesce all overlapping and adjacent regions into
			 * next, freeing them.
			 */
			start = MIN(address, next->ml_address);
			end = MAX(address + bytes,
			    next->ml_address + next->ml_size);

			next->ml_address = start;
			next->ml_size = end - start;
			xmemlist_free_one(mpp, dst);
			dst = next;

			for (next = dst->ml_next;
			    next != NULL && next->ml_address <= end;
			    next = dst->ml_next) {
				end = MAX(end,
				    next->ml_address + next->ml_size);
				dst->ml_size = end - start;

				dst->ml_next = next->ml_next;
				if (next->ml_next != NULL)
					next->ml_next->ml_prev = dst;
				xmemlist_free_one(mpp, next);
			}

			return (MEML_SPANOP_OK);
		}

		/*
		 * Prepend to next.
		 */
		if ((address + bytes) == next->ml_address) {
			xmemlist_free_one(mpp, dst);

			next->ml_address = address;
			next->ml_size += bytes;

			return (MEML_SPANOP_OK);
		}

		/*
		 * Append to next.
		 */
		if (address == (next->ml_address + next->ml_size)) {
			xmemlist_free_one(mpp, dst);

			if (next->ml_next != NULL) {
				/*
				 * don't overlap with next->ml_next
				 */
				if ((address + bytes) >
				    next->ml_next->ml_address) {
					return (MEML_SPANOP_ESPAN);
				}

				/*
				 * Concatenate next and next->ml_next
				 */
				if ((address + bytes) ==
				    next->ml_next->ml_address) {
					struct memlist *mlp = next->ml_next;

					if (next == *curmemlistp)
						*curmemlistp = next->ml_next;

					mlp->ml_address = next->ml_address;
					mlp->ml_size += next->ml_size;
					mlp->ml_size += bytes;

					if (next->ml_prev)
						next->ml_prev->ml_next = mlp;
					mlp->ml_prev = next->ml_prev;

					xmemlist_free_one(mpp, next);
					return (MEML_SPANOP_OK);
				}
			}

			next->ml_size += bytes;

			return (MEML_SPANOP_OK);
		}

		/* don't overlap with next */
		if ((address + bytes) > next->ml_address) {
			xmemlist_free_one(mpp, dst);
			return (MEML_SPANOP_ESPAN);
		}

		/*
		 * Insert before next.
		 */
		dst->ml_prev = prev;
		dst->ml_next = next;
		next->ml_prev = dst;
		if (prev == NULL) {
			*curmemlistp = dst;
		} else {
			prev->ml_next = dst;
		}
		return (MEML_SPANOP_OK);
	}

	/*
	 * End of list, prev is valid and next is NULL.
	 */
	prev->ml_next = dst;
	dst->ml_prev = prev;
	dst->ml_next = NULL;

	return (MEML_SPANOP_OK);
}

int
memlist_add_span(uint64_t address, uint64_t bytes, memlist_t **curmemlistp)
{
	return (xmemlist_add_span(&pool, address, bytes, curmemlistp, 0));
}

static int
xmemlist_delete_span_relaxed(memlist_pool_t *mpp, uint64_t address,
    uint64_t bytes, struct memlist **curmemlistp)
{
	struct memlist *next, *del, *second;
	uint64_t end;

	for (next = *curmemlistp; next != NULL; next = next->ml_next) {
		if (next->ml_address + next->ml_size > address)
			break;
	}

	/*
	 * There's nothing to do if either the deleted span begins at or beyond
	 * the end of the last region in the list, or the first region in the
	 * list that extends beyond the start of the deleted span also begins
	 * beyond it.  N1 and N3 are empty and N4 begins beyond the last B.
	 */
	if (next == NULL || next->ml_address > address + bytes)
		return (MEML_SPANOP_OK);

	end = address + bytes;
	while (next != NULL && next->ml_address < end) {
		/*
		 * N contains A.  Delete A from L and proceed.
		 */
		if (next->ml_address >= address &&
		    next->ml_address + next->ml_size <= end) {
			if (next->ml_next != NULL) {
				next->ml_next->ml_prev = next->ml_prev;
			}
			if (next->ml_prev != NULL) {
				next->ml_prev->ml_next = next->ml_next;
			} else {
				*curmemlistp = next->ml_next;
			}
			del = next;
			next = next->ml_next;
			xmemlist_free_one(mpp, del);
			continue;
		}

		/*
		 * N overlaps the first part of A.  Truncate A and return.
		 */
		if (next->ml_address >= address) {
			ASSERT(next->ml_address + next->ml_size > end);
			next->ml_size -= (end - next->ml_address);
			next->ml_address = end;

			return (MEML_SPANOP_OK);
		}

		ASSERT(next->ml_address < address);

		/*
		 * N overlaps the last part of A.  Truncate A and proceed;
		 * there may be something after A that overlaps.
		 */
		if (next->ml_address + next->ml_size <= end) {
			next->ml_size = address - next->ml_address;
			next = next->ml_next;
			continue;
		}

		/*
		 * A contains N.  Split A and return.
		 */
		if ((second = xmemlist_get_one(mpp)) == NULL)
			return (MEML_SPANOP_EALLOC);

		second->ml_address = end;
		second->ml_size = next->ml_address + next->ml_size - end;
		second->ml_next = next->ml_next;
		second->ml_prev = next;

		next->ml_size = address - next->ml_address;
		next->ml_next = second;

		return (MEML_SPANOP_OK);
	}

	/*
	 * We've reached a region A that begins at or beyond the end of N, or
	 * run out of regions entirely.  There's nothing more to do.
	 */
	return (MEML_SPANOP_OK);
}

/*
 * Delete a span from a memlist.
 * Return:
 * MEML_SPANOP_OK if OK.
 * MEML_SPANOP_ESPAN if part or all of span does not exist and not relaxed
 * MEML_SPANOP_EALLOC for allocation failure
 */
int
xmemlist_delete_span(
	memlist_pool_t *mpp,
	uint64_t address,
	uint64_t bytes,
	struct memlist **curmemlistp,
	uint64_t flags)
{
	struct memlist *dst, *next;

	/*
	 * It's not totally inconceivable to refactor this, but these two
	 * implementations really don't have much in common.
	 */
	if ((flags & MEML_FL_RELAXED)) {
		return (xmemlist_delete_span_relaxed(mpp, address, bytes,
		    curmemlistp));
	}

	/*
	 * Find element containing address.
	 */
	for (next = *curmemlistp; next != NULL; next = next->ml_next) {
		if ((address >= next->ml_address) &&
		    (address < next->ml_address + next->ml_size))
			break;
	}

	/*
	 * If start address not in list.
	 */
	if (next == NULL) {
		return (MEML_SPANOP_ESPAN);
	}

	/*
	 * Error if size goes off end of this struct memlist.
	 */
	if (address + bytes > next->ml_address + next->ml_size) {
		return (MEML_SPANOP_ESPAN);
	}

	/*
	 * Span at beginning of struct memlist.
	 */
	if (address == next->ml_address) {
		/*
		 * If start & size match, delete from list.
		 */
		if (bytes == next->ml_size) {
			if (next == *curmemlistp)
				*curmemlistp = next->ml_next;
			if (next->ml_prev != NULL)
				next->ml_prev->ml_next = next->ml_next;
			if (next->ml_next != NULL)
				next->ml_next->ml_prev = next->ml_prev;

			xmemlist_free_one(mpp, next);
		} else {
			/*
			 * Increment start address by bytes.
			 */
			next->ml_address += bytes;
			next->ml_size -= bytes;
		}
		return (MEML_SPANOP_OK);
	}

	/*
	 * Span at end of struct memlist.
	 */
	if (address + bytes == next->ml_address + next->ml_size) {
		/*
		 * decrement size by bytes
		 */
		next->ml_size -= bytes;
		return (MEML_SPANOP_OK);
	}

	/*
	 * Delete a span in the middle of the struct memlist.
	 */
	{
		/*
		 * create a new struct memlist
		 */
		dst = xmemlist_get_one(mpp);

		if (dst == NULL) {
			return (MEML_SPANOP_EALLOC);
		}

		/*
		 * Existing struct memlist gets address
		 * and size up to start of span.
		 */
		dst->ml_address = address + bytes;
		dst->ml_size =
		    (next->ml_address + next->ml_size) - dst->ml_address;
		next->ml_size = address - next->ml_address;

		/*
		 * New struct memlist gets address starting
		 * after span, until end.
		 */

		/*
		 * link in new memlist after old
		 */
		dst->ml_next = next->ml_next;
		dst->ml_prev = next;

		if (next->ml_next != NULL)
			next->ml_next->ml_prev = dst;
		next->ml_next = dst;
	}
	return (MEML_SPANOP_OK);
}

int
memlist_delete_span(uint64_t address, uint64_t bytes, memlist_t **curmemlistp)
{
	return (xmemlist_delete_span(&pool, address, bytes, curmemlistp, 0));
}

struct memlist *
memlist_kmem_dup(const struct memlist *src, int kmflags)
{
	struct memlist *dest = NULL, *last = NULL;

	while (src != NULL) {
		struct memlist *new;

		new = kmem_zalloc(sizeof (struct memlist), kmflags);
		if (new == NULL) {
			while (dest != NULL) {
				struct memlist *to_free;

				to_free = dest;
				dest = dest->ml_next;
				kmem_free(to_free, sizeof (struct memlist));
			}
			return (NULL);
		}

		new->ml_address = src->ml_address;
		new->ml_size = src->ml_size;
		new->ml_next = NULL;
		new->ml_prev = last;
		if (last != NULL) {
			last->ml_next = new;
		} else {
			dest = new;
		}

		last = new;
		src = src->ml_next;
	}

	return (dest);
}
