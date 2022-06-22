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
 * Copyright 2022 Oxide Computer Co.
 */

#ifndef	_SYS_SMM_H
#define	_SYS_SMM_H

/*
 * The concept here is pretty similar to the gross and loathesome real mode
 * platter used for MP boot (see rm_platter.h, mp_rmp.c, and mpcore.s).  Things
 * are simpler as we are not going to enter 64-bit mode nor run kernel code nor
 * do we need to worry about the reset vector.  It's also trickier because we
 * have very little space to work with: the entire handler and its data must fit
 * into 512 bytes.
 */

#if !defined(_ASM)

#include <sys/types.h>
#include <sys/segments.h>
#include <sys/stddef.h>

#endif

#include <sys/param.h>
#include <sys/smm_amd64.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	SMH_CODE_SIZE		454
#define	SMH_SCRATCH_SIZE	6	/* selector + addr32 */

#define	SMBASE_CPU_STRIDE_LOG2	10	/* 1 KiB per CPU */
#define	SMBASE_CPU_STRIDE	(1 << SMBASE_CPU_STRIDE_LOG2)

#if !defined(_ASM)

typedef struct ksmm {
	amd64_smm_state_t	ksmm_state_save;

	/* MSR data, per-thread from the first thread to take the SMI */
	uint64_t		ksmm_msr_smi_io_trap_0;
	uint64_t		ksmm_msr_smi_io_trap_1;
	uint64_t		ksmm_msr_smi_io_trap_2;
	uint64_t		ksmm_msr_smi_io_trap_3;
	uint64_t		ksmm_msr_smi_io_trap_ctl;
	uint64_t		ksmm_msr_pfeh_cfg;
	uint64_t		ksmm_msr_pfeh_cloak_cfg;
	uint64_t		ksmm_msr_pfeh_def_int;

	/* FCH data, global to the socket */
	uint32_t		ksmm_smi_event_status;
	uint32_t		ksmm_smi_capt_data;
	uint32_t		ksmm_smi_capt_valid;
	uint32_t		ksmm_smi_status_0;
	uint32_t		ksmm_smi_status_1;
	uint32_t		ksmm_smi_status_2;
	uint32_t		ksmm_smi_status_3;
	uint32_t		ksmm_smi_status_4;
	uint32_t		ksmm_smi_trig_0;

	uint32_t		ksmm_valid;
	uint32_t		ksmm_nsmi;
} ksmm_t;

typedef	struct smm_handler {
	uint8_t		smh_code[SMH_CODE_SIZE];
	uint8_t		smh_scratch[SMH_SCRATCH_SIZE];
	uint32_t	smh_ksmmpa;

	uint64_t	smh_gdt[4];
	uint16_t	_gdtdesc_pad;
	uint16_t	smh_gdt_lim;
	uint32_t	smh_gdt_base;
	uint16_t	_idtdesc_pad;
	uint16_t	smh_idt_lim;
	uint32_t	smh_idt_base;
} smm_handler_t;

extern int smm_init(void);
extern void smm_install_handler(void);
extern boolean_t smm_check_nmi(void);

#endif	/* !_ASM */

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_SMM_H */
