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

#ifndef _SYS_IO_HUASHAN_FCHREGS_H
#define	_SYS_IO_HUASHAN_FCHREGS_H

#include <sys/types.h>
#include <sys/bitext.h>

/*
 * Macros for FCH registers and register access
 *
 * This is... what it is.  A proc_macro it is not.  To help make sense of this,
 * the macros that constitute the interface are documented.  The others should
 * not be called by consumers (all such begin with __).  What we'd really like
 * to be able to do is say something like:
 *
 * let regs = FCH::PM.map();
 * let dereg = regs.DECODEEN.read();
 * dereg.set(FCH::PM::DECODEEN::IOAPICEN);
 * regs.DECODEEN.write(dereg);
 * regs.unmap();	// Consumes regs
 *
 * Neither C nor the C preprocessor is smart enough (Rust isn't either, not
 * quite, even with proc macros, but we could get very close, compromising
 * perhaps only a bit on syntax).  But we can approximate this, adding at the
 * same time some static type safety.
 *
 * Why is this "nice"?  You don't have to know anything about a register except
 * its name and the bits in it that you want to manipulate.  For now this works
 * for MMIO registers, which all of PMIO are.  This can also be extended to
 * registers that have to be accessed via I/O ports or indirect index/data
 * pairs, but that's not here now.  This is surprisingly helpful, because
 * the PMIO register block contains registers of 8, 16, and 32 bits, and there
 * is generally no rhyme or reason to them.  It also offers a possible aid to
 * transitions among generations, where registers may change size but have the
 * same functionality; code implementing atop the FCH need not change unless
 * the location of a register of the bit offset or size of a field has changed.
 * One may hope this will reduce code replication over time.  It's also nice
 * because you can use the proper names of blocks, registers, and fields in a
 * way that reflects the processor's internal construction.  In principle, one
 * could generate these definitions from SVD or some other automated source
 * if only such were available (and open source).
 *
 * Because we use the typed bitext functions, we can generally catch type
 * errors; we'd expect those to occur only where consumers get cute.  Using
 * the type macros as intended should allow common errors to be caught at
 * compile time.
 *
 * XXX How best to express units?  One option would be to define a set of
 * units, one per type of quantity to measure, and then express all units
 * in terms of that.  For example, we'd choose nanoseconds for time and
 * then do
 *
 * #define	FCH_PM_R_BLAH_U_TIME_NS		USEC2NSEC(16)
 * #define	FCH_PM_R_BLARGH_U_BYTES		32
 * #define	FCH_PM_R_QUUX_U_CURRENT_UA	UA2NA(4)
 *
 * for registers that store time in units of 16 µs, data in units of 32
 * bytes, and current in units of 4 µA.  A consumer could then utter
 * something like
 *
 * FCH_REG_TYPE(PM, BLAH) timeout_reg;
 * hrtime_t timeout;
 * ...
 * timeout_reg = timeout / FCH_REG_UNIT(PM, BLAH, TIME_NS);
 * FCH_MR_WRITE(PM, BLAH, va, timeout_reg);
 *
 * If the kind of thing the register measures is wrong, this won't compile;
 * if it is, it will generally allow code to keep working even if the units
 * change in a future FCH.  Of course, that also means that the change can
 * introduce nasty bugs if for example rounding to zero becomes a problem.
 * For that reason it may be preferable to still have per-FCH code that
 * presents a more stable interface (e.g., accepts the hrtime_t).  That code
 * might still be more pleasant to write and maintain with this kind of
 * facility, though, if one were careful enough to review the changes in
 * the new kind of FCH as one really must anyway.
 *
 * Another option is to say
 *
 * #define	FCH_PM_R_BLAH_U_16US	1
 *
 * or
 *
 * #define	FCH_PM_R_BLAH_U_US	16
 *
 * This seems unfortunate in that the programmer has to know what the units
 * are and can then use the macro symbolically to set or interpret the
 * field.  But it does at least make sure that code won't compile if the
 * macro that's used doesn't match the definition, so it becomes more of an
 * assertion -- but it can get out of sync with the code itself:
 *
 * FCH_REG_TYPE(PM, BLAH) timeout_reg;
 * hrtime_t timeout;
 * ...
 * CTASSERT(FCH_REG_UNIT(PM, BLAH, 16US) == 1);
 * // or CTASSERT(FCH_REG_UNIT(PM, BLAH, US) == 16);
 * timeout_reg = timeout / USEC2NSEC(16);
 * FCH_MR_WRITE(PM, BLAH, va, timeout_reg);
 *
 * So it seems like the first option is preferable, because we can also get
 * the benefit of this kind of assertion by doing:
 *
 * FCH_REG_TYPE(PM, BLAH) timeout_reg;
 * hrtime_t timeout;
 * ...
 * CTASSERT(FCH_REG_UNIT(PM, BLAH, TIME) == USEC2NSEC(16));
 * // or something broader like
 * CTASSERT(FCH_REG_UNIT(PM, BLAH, TIME) > USEC2NSEC(1));
 * timeout_reg = timeout / FCH_REG_UNIT(PM, BLAH, TIME);
 * VERIFY3U(timeout, !=, 0);
 * FCH_MR_WRITE(PM, BLAH, va, timeout_reg);
 *
 * The goal is not to completely automate setting these values, though it
 * would be nice if one could at least, say, wire this sort of thing into
 * mdb so unit display becomes automatic.  The programmer will still need to
 * understand the units.  What we're going for here is (a) make it easy to
 * use the unit macro programmatically and (b) prevent confusing definitions
 * of units that lead to errors and frustrating debugging sessions that end
 * in reading the PPR and finding that the definitions led the programmer
 * astray.  At some point it may just be less work to make Rust work, but
 * in the meantime there is a lot of existing code that will need some of
 * this.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define	__EXPAND_AND_CALL1(_m, _x1)			_m(_x1)
#define	__EXPAND_AND_CALL2(_m, _x1, _x2)		_m(_x1, _x2)
#define	__EXPAND_AND_CALL3(_m, _x1, _x2, _x3)		_m(_x1, _x2, _x3)
#define	__EXPAND_AND_CALL4(_m, _x1, _x2, _x3, _x4)	_m(_x1, _x2, _x3, _x4)

/*CSTYLED*/
#define	__UINTW_T(_w)	uint ## _w ## _t
#define	__BITSETW(_w)	bitset ## _w
#define	__BITXW(_w)	bitx ## _w

#define	__FCH_BLOCK_PHYS_BASE_NAME(_block)			\
	FCH_ ## _block ## _PHYS_BASE

#define	__FCH_BLOCK_SIZE_NAME(_block)				\
	FCH_ ## _block ## _SIZE

#define	__FCH_REG_WIDTH_NAME(_block, _regname)			\
	FCH_ ## _block ## _R_ ## _regname ## _WIDTH

#define	__FCH_REG_OFFSET_NAME(_block, _regname)			\
	FCH_ ## _block ## _R_ ## _regname ## _OFFSET

#define	__FCH_REG_FIELD_NAME(_block, _regname, _fieldname)	\
	FCH_ ## _block ## _R_ ## _regname ## _F_ ## _fieldname

#define	__FCH_REG_UNIT_NAME(_block, _regname, _kind)		\
	FCH_ ## _block ## _R_ ## _regname ## _U_ ## _kind

#define	__FCH_REG_FIELD_UNIT_NAME(_block, _regname, _fieldname, _kind)	\
	FCH_ ## _block ## _R_ ## _regname ## _F_ ## _fieldname ## _U_ ## _kind

/*
 * FCH_REG_TYPE(block, regname)
 *
 * This macro expands to the name of a type suitable for storing the contents
 * of /block/::/regname/.  No variables are declared or defined.  This allows
 * you to use this macro anywhere a type could be used, meaning you can easily
 * declare pointers, statics, externs, consts, and even function parameters
 * with it.
 */
#define	FCH_REG_TYPE(_block, _regname)				\
	__EXPAND_AND_CALL1(__UINTW_T,				\
	    __EXPAND_AND_CALL2(__FCH_REG_WIDTH_NAME, _block, _regname))

/*
 * FCH_REG_UNIT(block, regname, kind)
 *
 * This macro expands to the quantity of /kind/ (a kind of quantity that can
 * be measured) that represents the granularity of the register
 * /block/::/regname/.  All granularities of a given kind are always
 * expressed in terms of only a single unit; the allowable values of /kind/
 * are:
 *
 * TIME_NS	time in nanoseconds	hrtime_t
 * BYTES	number of bytes/octets	size_t
 *
 * If the register does not represent a value of this kind, the use of this
 * macro will cause a compile-time error.  The value returned is guaranteed
 * to be a compile-time constant, so one may also use CTASSERT() with it.
 *
 * Example:
 *
 * hrtime_t timeout = ...;
 * FCH_REG_TYPE(FOO, BAR) timeout_reg;
 * ...
 * timeout_reg = timeout / FCH_REG_UNIT(FOO, BAR, TIME_NS);
 */
#define	FCH_REG_UNIT(_block, _regname, _kind)			\
	__EXPAND_AND_CALL3(__FCH_REG_UNIT_NAME, _block, _regname, _kind)

/*
 * FCH_REG_FIELD_UNIT(block, regname, fieldname, kind)
 *
 * This is analogous to FCH_REG_UNIT but for individual fields within
 * the register.  Its semantics are otherwise identical.
 */
#define	FCH_REG_FIELD_UNIT(_block, _regname, _fieldname, _kind)	\
	__EXPAND_AND_CALL4(__FCH_REG_FIELD_UNIT_NAME, _block,	\
	    _regname, _fieldname, _kind)

#define	__FCH_WRITE_REG_W(_block, _regname, _va, _r)		\
	(*((volatile __EXPAND_AND_CALL1(__UINTW_T,		\
	    __EXPAND_AND_CALL2(__FCH_REG_WIDTH_NAME, _block, _regname)) *) \
	    (_va))) = (_r)

#define	__FCH_READ_REG_W(_block, _regname, _va)			\
	(*((volatile __EXPAND_AND_CALL1(__UINTW_T,		\
	    __EXPAND_AND_CALL2(__FCH_REG_WIDTH_NAME, _block, _regname)) *) \
	    (_va)))

/*
 * FCH_R_SET_B(block, regname, fieldname, varname, value)
 *
 * Set the bits for the field specified by /fieldname/ in /varname/ to /value/.
 * AMD documentation typically expresses /value/ for their fields as if the
 * first bit in the field were bit 0, the same convention used by the bitext.h
 * functions; this macro does the same and uses those functions.  Therefore
 * it is not necessary to mask or shift.  The contents of the variable other
 * than the field /fieldname/ are not modified.  Hardware is not affected.
 *
 * Returns the new value.  This is normally used as a crude implementation of
 * the builder pattern, in which the same variable is used repeatedly to set
 * or clear the fields one wishes to change.  If you want to assign the result
 * to a different variable, you should use either FCH_DECL_REG() above or the
 * GCC __typeof__ extension to declare it.
 */
#define	FCH_R_SET_B(_block, _regname, _fieldname, _r, _v)	\
	__EXPAND_AND_CALL1(__BITSETW,				\
	    __EXPAND_AND_CALL2(__FCH_REG_WIDTH_NAME, _block, _regname))((_r), \
	    __EXPAND_AND_CALL3(__FCH_REG_FIELD_NAME,		\
	    _block, _regname, _fieldname), (_v))

/*
 * FCH_R_GET_B(block, regname, fieldname, varname)
 *
 * Analogous to FCH_R_SET_B(), extracts the bits corresponding to /fieldname/
 * from /varname/, masking and shifting for you so that the return value
 * contains only those bits from /fieldname/, and beginning at bit 0.
 */
#define	FCH_R_GET_B(_block, _regname, _fieldname, _r)		\
	__EXPAND_AND_CALL1(__BITXW,	\
	    __EXPAND_AND_CALL2(__FCH_REG_WIDTH_NAME, _block, _regname))((_r), \
	    __EXPAND_AND_CALL3(__FCH_REG_FIELD_NAME,		\
	    _block, _regname, _fieldname))

/*
 * FCH_MR_BLOCK_GETPA(block)
 *
 * Returns the base physical address of a register block.  This is implemented
 * only for MMIO blocks and will fail to compile if called on anything else.
 */
#define	FCH_MR_BLOCK_GETPA(_block)				\
	(__EXPAND_AND_CALL1(__FCH_BLOCK_PHYS_BASE_NAME, _block))

/*
 * FCH_R_BLOCK_GETSIZE(block)
 *
 * Returns the number of contiguous byte addresses that refer to the register
 * block named by /block/.
 */
#define	FCH_R_BLOCK_GETSIZE(_block)				\
	(__EXPAND_AND_CALL1(__FCH_BLOCK_SIZE_NAME, _block))

/*
 * FCH_MR_BLOCK_GETVA(block, regname, baseva)
 *
 * Returns the virtual address of a register within a register block mapped at
 * /baseva/.  Unfortunately there is no good way to guarantee that the mapping
 * at /baseva/ was obtained from a mapping onto FCH_MR_BLOCK_GETPA() for the
 * same register block.  Be careful.
 *
 * This is implemented only for MMIO blocks and will fail to compile if called
 * on anything else.
 */
#define	FCH_MR_GETVA(_block, _regname, _baseva)			\
	((caddr_t)(((uintptr_t)(caddr_t)(_baseva)) +		\
	    __EXPAND_AND_CALL2(__FCH_REG_OFFSET_NAME, _block, _regname)))

/*
 * FCH_MR_WRITE(block, regname, baseva, regval)
 *
 * Set the contents of the hardware register /block/::/regname/ to /regval/
 * using the mapping of the register block based at /baseva/.  The same
 * caveat described for FCH_MR_BLOCK_GETVA() applies.  There is no return value.
 * /regval/ may be a literal, in which case you must manually ensure that it
 * fits into the register, or a variable declared with FCH_DECL_REG() or using
 * GCC's __typeof__ extension to replicate such a variable.
 *
 * This is implemented only for MMIO blocks and will fail to compile if called
 * on anything else.
 */
#define	FCH_MR_WRITE(_block, _regname, _baseva, _r)		\
	__FCH_WRITE_REG_W(_block, _regname,			\
	    FCH_MR_GETVA(_block, _regname, _baseva), (_r))

/*
 * FCH_MR_READ(block, regname, baseva)
 *
 * Read and return the contents of the hardware register /block/::/regname/
 * using the mapping of the register block based at /baseva/.  The same caveat
 * described for FCH_MR_BLOCK_GETBA() applies.  You want to assign the return
 * value to a variable declared via FCH_DECL_REG() or created from such a
 * variable using GCC's __typeof__ extension.
 *
 * This example pulls in most of the useful functionality; it sets the SMBus
 * controller's BAR to what happens to be the default value without changing
 * anything else in hardware:
 *
 * size_t pmsize = FCH_R_BLOCK_GETSIZE(PM);
 * caddr_t pmbase = psm_map_phys(FCH_MR_BLOCK_GETPA(PM), pmsize,
 *     PROT_READ | PROT_WRITE);
 * FCH_REG_TYPE(PM, DECODEEN) dereg;
 *
 * dereg = FCH_MR_READ(PM, DECODEEN, pmbase);
 * dereg = FCH_R_SET_B(PM, DECODEEN, SMBUSASFIOBASE, dereg, 0xb);
 * FCH_MR_WRITE(PM, DECODEEN, pmbase, dereg);
 * psm_unmap_phys(pmbase, pmsize);
 */
#define	FCH_MR_READ(_block, _regname, _baseva)			\
	__FCH_READ_REG_W(_block, _regname,			\
	    FCH_MR_GETVA(_block, _regname, _baseva))

/*
 * Implementing a register (block)
 *
 * MMIO blocks must have a macro named FCH_<blockname>_PHYS_BASE with a value
 * equal to the physical address of the register block.  It need not be
 * page-aligned; routines such as psm_map_phys() will correctly map such an
 * address and return a properly-offset VA for later use with the above macros.
 * Every block must have a macro named FCH_<blockname>_SIZE with a value equal
 * to the size in bytes of the register block (the number of contiguous
 * addresses that refer to the block).
 *
 * Each register must have two macros:
 *
 * FCH_<blockname>_R_<regname>_OFFSET
 *
 * This is the offset in bytes into the register block at which this register
 * is located.
 *
 * FCH_<blockname>_R_<regname>_WIDTH
 *
 * This is the size in bits of the register.  Only 8, 16, 32, and 64 are
 * allowed; other values will fail to compile.  That should be ok; I am not
 * aware of any registers in the FCH (or anywhere else in the AMD processor)
 * that require oddball accesses.
 *
 * Optionally, each register may have any number (limited only by the number
 * of bits it contains) of fields.  Each field is specified by a pair of bit
 * ranges, the interpretation of which is identical to that used by the
 * functions in bitext.h: the higher bit must be first, and the range is
 * inclusive at both ends.  Thus, the highest bit index allowed in a 32-bit
 * register is 31, and a single-bit field is specified by repeating the index
 * of that bit.  These pairs must be separated by a comma; each index may be
 * a parenthesised constant expression, but the pair must not be parenthesised
 * or otherwise enclosed.  As such, these macros should not be used directly
 * by consumers; the above macros construct them from block, register, and
 * field names only in contexts in which they may safely be used.  The name of
 * each field macro must be of the form
 *
 * FCH_<blockname>_R_<regname>_F_<fieldname>
 *
 * Where useful, symbolic constants should also be defined for values of
 * individual fields.  Remember that each field's value starts from bit 0 when
 * using these access macros.  Values for a register or field should be
 * given by macros of the form
 *
 * FCH_<blockname>_R_<regname>_V_<valuename>
 * FCH_<blockname>_R_<regname>_F_<fieldname>_V_<valuename>
 *
 * Likewise, units may be defined for a register or field by macros of the
 * form
 *
 * FCH_<blockname>_R_<regname>_U_<unit>
 * FCH_<blockname>_R_<regname>_F_<fieldname>_U_<unit>
 *
 * See the discussion of units above.
 *
 * See pmio.h for an example of a basic MMIO register block.
 */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_HUASHAN_FCHREGS_H */
