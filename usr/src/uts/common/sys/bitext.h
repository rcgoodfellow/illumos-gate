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

#ifndef _SYS_BITOPS_H
#define	_SYS_BITOPS_H

#include <sys/debug.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A bunch of routines to make working with bits and registers easier.
 */

/*
 * In case we don't have BITX.
 */
#ifndef	BITX
#define	BITX(u, h, l)	(((u) >> (l)) & ((1LU << ((h) - (l) + 1LU)) - 1LU))
#endif

static inline uint8_t
bitset8(uint8_t reg, uint_t high, uint_t low, uint8_t val)
{
	uint8_t mask;

	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 8);
	ASSERT3U(low, <, 8);

	mask = (1LU << (high - low + 1)) - 1;
	ASSERT0(~mask & val);

	reg &= ~(mask << low);
	reg |= val << low;

	return (reg);
}

static inline uint16_t
bitx16(uint16_t reg, uint_t high, uint_t low)
{
	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 16);
	ASSERT3U(low, <, 16);

	return (BITX(reg, high, low));
}


static inline uint32_t
bitx32(uint32_t reg, uint_t high, uint_t low)
{
	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 32);
	ASSERT3U(low, <, 32);

	return (BITX(reg, high, low));
}

static inline uint64_t
bitx64(uint64_t reg, uint_t high, uint_t low)
{
	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 64);
	ASSERT3U(low, <, 64);

	return (BITX(reg, high, low));
}

static inline uint16_t
bitset16(uint16_t reg, uint_t high, uint_t low, uint16_t val)
{
	uint16_t mask;

	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 16);
	ASSERT3U(low, <, 16);

	mask = (1LU << (high - low + 1)) - 1;
	ASSERT0(~mask & val);

	reg &= ~(mask << low);
	reg |= val << low;

	return (reg);
}

static inline uint32_t
bitset32(uint32_t reg, uint_t high, uint_t low, uint32_t val)
{
	uint32_t mask;

	ASSERT3U(high, >=, low);
	ASSERT3U(high, <, 32);
	ASSERT3U(low, <, 32);

	mask = (1LU << (high - low + 1)) - 1;
	ASSERT0(~mask & val);

	reg &= ~(mask << low);
	reg |= val << low;

	return (reg);
}

#ifdef __cplusplus
}
#endif

#endif /* _SYS_BITOPS_H */
