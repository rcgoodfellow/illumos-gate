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
 * Copyright (c) 1992, 2010, Oracle and/or its affiliates. All rights reserved.
 */
/*
 * Copyright (c) 2010, Intel Corporation.
 * All rights reserved.
 */
/*
 * Copyright 2018 Joyent, Inc.
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef	_SYS_RM_PLATTER_H
#define	_SYS_RM_PLATTER_H

#if !defined(_ASM)

#include <sys/types.h>
#include <sys/tss.h>
#include <sys/segments.h>
#include <sys/stddef.h>

#endif

#include <sys/param.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	RM_PLATTER_CODE_SIZE		0x400
#define	RESET_VECTOR_SIZE		0x10
#define	RESET_VECTOR_PAGE_OFF		(MMU_PAGESIZE - RESET_VECTOR_SIZE)

#if !defined(_ASM)

typedef	struct rm_platter {
	/* 0 */
	uint8_t		rm_code[RM_PLATTER_CODE_SIZE];

	/* 0x400 */

	uint32_t	rm_basepa;	/* absolute physaddr of the RMP */

	/*
	 * The compiler will want to 64-bit align the 64-bit rm_gdt_base
	 * pointer, so we need to add an extra six bytes of padding here to
	 * make sure rm_gdt_lim and rm_gdt_base will align to create a proper
	 * ten byte GDT pseudo-descriptor.
	 */
	uint8_t		rm_gdt_pad[2];
	uint16_t	rm_gdt_lim;	/* stuff for lgdt */
	user_desc_t	*rm_gdt_base;

	/* 0x410 */

	uint32_t	rm_cpu;		/* easy way to know which CPU we are */

	/*
	 * The compiler will want to 64-bit align the 64-bit rm_idt_base
	 * pointer, so we need to add an extra six bytes of padding here to
	 * make sure rm_idt_lim and rm_idt_base will align to create a proper
	 * ten byte IDT pseudo-descriptor.
	 */
	uint8_t		rm_idt_pad[2];
	uint16_t	rm_idt_lim;	/* stuff for lidt */
	gate_desc_t	*rm_idt_base;

	/* 0x420 */

	/*
	 * The code executing in the rm_platter needs the absolute addresses at
	 * which the 32-bit and 64-bit code starts, so have mp_startup
	 * calculate it and store it here.
	 */
	uint32_t	rm_pe32_addr;
	uint32_t	rm_longmode64_addr;
	uint32_t	rm_pdbr;	/* cr3 value */
	uint32_t	rm_cr4;		/* cr4 value on cpu0 */

	/* 0x430 */

	/*
	 * Temporary GDT for the brief transition from real mode to protected
	 * mode before a CPU continues on into long mode.
	 *
	 * Putting it here assures it will be located in identity mapped memory
	 * (va == pa, 1:1).
	 *
	 * rm_temp_gdt is sized to hold only three descriptors plus the required
	 * null descriptor; these are what we need to get to 64-bit mode.
	 *
	 * rm_temp_[gi]dt_lim and rm_temp_[gi]dt_base are the pseudo-descriptors
	 * for the temporary GDT and IDT, respectively.
	 */
	uint64_t	rm_temp_gdt[4];
	uint16_t	rm_temp_gdtdesc_pad;	/* 0x440, align GDT desc */
	uint16_t	rm_temp_gdt_lim;
	uint32_t	rm_temp_gdt_base;
	uint16_t	rm_temp_idtdesc_pad;	/* 0x448, align IDT desc */
	uint16_t	rm_temp_idt_lim;
	uint32_t	rm_temp_idt_base;

	/* 0x460 */

	/*
	 * This space will be used as the initial real mode stack, mainly for
	 * debugging if a fault occurs but possibly also for push-push-ret
	 * transfers because people don't seem to like ljmpl.  See mpcore.s.
	 */
	uint8_t		rm_rv_pad[MMU_PAGESIZE - 0x460 - RESET_VECTOR_SIZE];

	/* 0xFF0 */

	/*
	 * The offset of the reset vector is architecturally defined to be the
	 * end of the segment less 16 bytes.  On this machine type, we always
	 * point the RMP at the last page of the segment from which the BSP
	 * itself booted, which in turn guarantees that this holds.
	 */
	uint8_t		rm_rv_code[RESET_VECTOR_SIZE];
} rm_platter_t;

/*
 * Try to protect against incorrect internal alignments.
 */
#define	CTASSERT_COMPACT(_ma, _mb)	\
	CTASSERT(offsetof(rm_platter_t, _mb) == offsetof(rm_platter_t, _ma) + \
	    sizeof (((rm_platter_t *)(NULL))->_ma))
CTASSERT(sizeof (rm_platter_t) == MMU_PAGESIZE);
CTASSERT(offsetof(rm_platter_t, rm_rv_code) == RESET_VECTOR_PAGE_OFF);
CTASSERT_COMPACT(rm_idt_lim, rm_idt_base);
CTASSERT_COMPACT(rm_gdt_lim, rm_gdt_base);
CTASSERT_COMPACT(rm_temp_gdt_lim, rm_temp_gdt_base);
CTASSERT_COMPACT(rm_temp_idt_lim, rm_temp_idt_base);
#undef	CTASSERT_COMPACT

/*
 * cpu tables put within a single structure two of the tables which need to be
 * allocated when a CPU starts up.
 *
 * Note: the tss should be 16 byte aligned for best performance on amd64
 * Since DEFAULTSTKSIZE is a multiple of PAGESIZE tss will be aligned.
 */
struct cpu_tables {
	/* IST stacks */
	char		ct_stack1[DEFAULTSTKSZ];	/* dblfault */
	char		ct_stack2[DEFAULTSTKSZ];	/* nmi */
	char		ct_stack3[DEFAULTSTKSZ];	/* mce */
	tss_t		ct_tss;
};

/*
 * gdt entries are 8 bytes long, ensure that we have an even no. of them.
 */
#if ((NGDT / 2) * 2 != NGDT)
#error "rm_platter.h: tss not properly aligned"
#endif

#endif	/* !_ASM */

/*
 * Offset of the RMP's base within the AP's boot segment.  While we could if
 * we wished use multiple pages (as many as 16) for the RMP, we need only one,
 * and the one we MUST provide is the last because that's where the reset
 * vector is architecturally defined to reside.
 */
#define	RMP_BASE_SEGOFF	(0x10000 - MMU_PAGESIZE)

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_RM_PLATTER_H */
