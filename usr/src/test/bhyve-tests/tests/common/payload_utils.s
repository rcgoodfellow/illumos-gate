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
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/asm_linkage.h>

/* void outb(uint16_t port, uint8_t val) */
ENTRY(outb)
	movw    %di, %dx
	movb    %sil, %al
	outb    (%dx)
	ret
SET_SIZE(outb)

/* void outw(uint16_t port, uint16_t val) */
ENTRY(outw)
	movw    %di, %dx
	movw    %si, %ax
	outw    (%dx)
	ret
SET_SIZE(outb)

/* void outl(uint16_t port, uint32_t val) */
ENTRY(outl)
	movw    %di, %dx
	movl    %esi, %eax
	outl    (%dx)
	ret
SET_SIZE(outl)

/* uint8_t inb(uint16_t port) */
ENTRY(inb)
	movw    %di, %dx
	inb    (%dx)
	ret
SET_SIZE(inb)

/* uint16_t inw(uint16_t port) */
ENTRY(inw)
	movw    %di, %dx
	inw    (%dx)
	ret
SET_SIZE(inw)

/* uint32_t inl(uint16_t port) */
ENTRY(inl)
	movw    %di, %dx
	inl    (%dx)
	ret
SET_SIZE(inl)

/* uint64_t wrmsr(uint32_t msr) */
ENTRY(rdmsr)
	movl    %edi, %ecx
	rdmsr
	shlq    $32, %rdx
	orq     %rdx, %rax
	ret
SET_SIZE(rdmsr)

/* void wrmsr(uint32_t msr, uint64_t val) */
ENTRY(wrmsr)
	movq    %rsi, %rdx
	shrq    $32, %rdx
	movl    %esi, %eax
	movl    %edi, %ecx
	wrmsr
	ret
SET_SIZE(wrmsr)

/* void cpuid(uint32_t in_eax, uint32_t in_ecx, uint32_t *out_regs) */
ENTRY(cpuid)
	pushq   %rbx
	movl    %edi, %eax
	movl    %esi, %ecx
	movq    %rdx, %r8
	cpuid
	movl    %eax, (%r8)
	movl    %ebx, 4(%r8)
	movl    %ecx, 8(%r8)
	movl    %edx, 12(%r8)
	popq    %rbx
	ret
SET_SIZE(cpuid)
