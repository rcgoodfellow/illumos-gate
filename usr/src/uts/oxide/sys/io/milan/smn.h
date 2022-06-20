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
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef _SYS_IO_MILAN_SMN_H
#define	_SYS_IO_MILAN_SMN_H

/*
 * Generic definitions for the system management network (SMN) in Milan
 * processors.  Will likely also be applicable to future generations.
 *
 * XXX These could be renamed, moved to common code, and consumed by/unified
 * with e.g. amdzen_umc_smn_addr() which does pretty much the same things.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In general, each functional block attached to the SMN is allotted its own
 * 20-bit aperture, which effectively means the block has a 12-bit identifier
 * or base as well.  Some subsystems have smaller base addresses because they
 * consume some of the register space for things like device and function ids.
 */
#define	MILAN_SMN_ADDR_BLOCK_BITS	12

#define	MILAN_SMN_ADDR_BASE_PART(_addr, _basebits)	\
	bitx32((_addr), 31, 32 - (_basebits))
#define	MILAN_SMN_ADDR_REG_PART(_addr, _basebits)	\
	bitx32((_addr), 31 - (_basebits), 0)

#define	MILAN_SMN_ASSERT_BASE_ADDR(_smnbase, _basebits)	\
	ASSERT0(MILAN_SMN_ADDR_REG_PART(_smnbase, _basebits))
#define	MILAN_SMN_ASSERT_REG_ADDR(_smnreg, _basebits)	\
	ASSERT0(MILAN_SMN_ADDR_BASE_PART(_smnreg, _basebits))

#define	MILAN_SMN_VERIFY_BASE_ADDR(_smnbase, _basebits)	\
	VERIFY0(MILAN_SMN_ADDR_REG_PART(_smnbase, _basebits))
#define	MILAN_SMN_VERIFY_REG_ADDR(_smnreg, _basebits)	\
	VERIFY0(MILAN_SMN_ADDR_BASE_PART(_smnreg, _basebits))

#define	MILAN_SMN_MAKE_ADDR(_smnbase, _basebits, _smnreg)	\
	(					\
	{					\
		uint32_t _b = (_smnbase);	\
		uint32_t _r = (_smnreg);	\
		uint_t _nbits = (_basebits);	\
		MILAN_SMN_ASSERT_BASE_ADDR(_b, (_nbits));	\
		MILAN_SMN_ASSERT_REG_ADDR(_r, (_nbits));	\
		(_b + _r);			\
	})

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_SMN_H */
