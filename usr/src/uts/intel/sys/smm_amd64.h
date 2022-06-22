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

#ifndef _SYS_SMM_AMD64_H
#define	_SYS_SMM_AMD64_H

/*
 * Structures, registers, and constants used by system management mode (SMM) on
 * the amd64 architecture.  Most of these definitions are architecturally
 * defined and are not processor- or machine-specific, though any use of them is
 * likely to be.  At present, only the oxide machine architecture implements SMM
 * (and then only to induce an immediate panic should SMM ever be entered).  In
 * preference to using any of this code, the ability to receive SMIs and enter
 * SMM should instead be permanently disabled by hardware fusing on processors
 * supporting that.  In places where AMD64 lacks detail or conflicts with
 * current PPRs, some of the architectural definitions have been supplemented
 * from the Milan rev B1 NDA PPR version 0.63; it may be necessary to check
 * state-save revision and/or processor model before using some fields, and it
 * is possible that other processors misuse the architecturally-defined fields
 * in different ways.  At present, the oxide kernel dumps the state-save area
 * and keeps these types around for later inspection, because any SMI is always
 * 100% fatal, so this state is not interpreted by any code.
 *
 * Note that this has very little to do with the __amd64 preprocessor token that
 * indicates we are compiling 64-bit code.  These structures pertain to the
 * underlying hardware architecture and are independent of the bitness of the
 * kernel, the code that happens to be executing when an SMI occurs, and the
 * bitness of the SMI handler, which is -- wait for it -- 16.  Because of course
 * it is.
 *
 * Finally, most of illumos uses "amd64" in the inclusive sense; ie, it includes
 * not only the amd64 architecture that AMD invented and Intel copied but also
 * Intel's copy which they call, as of this writing, "Intel 64".  While Intel's
 * implementation of amd64 is mostly compatible with the real McCoy, there are
 * exceptions and this is one of them.  Thus, these definitions are not suitable
 * for use on Intel processors.  Because of course they aren't.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef	_ASM

#include <sys/bitext.h>
#include <sys/types.h>

/*
 * AMD64 4.03 vol. 2 sec. 10.2.3 shows attr and limit fields, while more recent
 * PPRs tell us those two fields are reserved.  In addition, the architectural
 * base field is defined by PPRs to contain the in-memory descriptor which would
 * make it a user_desc_t.  This frankly makes a lot more sense than the notion
 * that they've helpfully reassembled all the pieces into a nice base and limit
 * for us.
 *
 * These declarations are dedicated to the loving memory of Bender B. Rodriguez.
 */
#pragma pack(1)
typedef struct amd64_smm_segd {
	uint16_t	ass_selector;
	uint16_t	ass_attr;
	uint32_t	ass_limit;
	union {
		uint64_t	ass_base;
		user_desc_t	ass_desc;
	};
} amd64_smm_segd_t;

/*
 * Likewise, we have inconsistencies here between the architectural definition
 * and the processor-specific definitions.  According to the architecture
 * manual, these are of the same format as the others; according to the PPRs,
 * the attr field is reserved, the limit field contains bits [47:32] of %fsbase
 * or %gsbase, sign-extended out to fill the available 32-bit field.
 */
typedef struct amd64_smm_fgs_segd {
	uint16_t	asfgs_selector;
	uint16_t	asfgs_attr;
	union {
		uint32_t	asfgs_limit;
		uint32_t	asfgs_basehi;
	};
	union {
		uint64_t	asfgs_base;
		user_desc_t	asfgs_desc;
	};
} amd64_smm_fgs_segd_t;

typedef struct amd64_smm_state {
	amd64_smm_segd_t	ass_es;
	amd64_smm_segd_t	ass_cs;
	amd64_smm_segd_t	ass_ss;
	amd64_smm_segd_t	ass_ds;
	amd64_smm_fgs_segd_t	ass_fs;
	amd64_smm_fgs_segd_t	ass_gs;

	/*
	 * These are the plain old GDTR, LDTR, IDTR, and TR registers, but
	 * their contents are not stored here in the same format used by the
	 * instructions that load and store them.  Why?  Why ask why?
	 */
	uint8_t			_reserved_60_63[4];
	uint16_t		ass_gdtr_limit;
	uint8_t			_reserved_66_67[2];
	uint64_t		ass_gdtr_base;

	uint16_t		ass_ldtr_selector;
	uint16_t		ass_ldtr_attr;
	uint32_t		ass_ldtr_limit;
	uint64_t		ass_ldtr_base;

	uint8_t			_reserved_80_83[4];
	uint16_t		ass_idtr_limit;
	uint8_t			_reserved_86_87[2];
	uint64_t		ass_idtr_base;

	uint16_t		ass_tr_selector;
	uint16_t		ass_tr_attr;
	uint32_t		ass_tr_limit;
	uint64_t		ass_tr_base;

	uint64_t		ass_ior_rip;
	uint64_t		ass_ior_rcx;
	uint64_t		ass_ior_rsi;
	uint64_t		ass_ior_rdi;
	union {
		uint32_t	ass_ior_dword;		/* amd64 */
		uint32_t	ass_trap_offset;	/* PPR */
	};
	uint32_t		ass_smi_status;		/* PPR only */
	uint8_t			ass_ior_flag;
	uint8_t			ass_ahr_flag;
	uint8_t			ass_nmi_mask;		/* PPR only */
	uint8_t			ass_cpl;		/* PPR only */
	uint8_t			_reserved_cc_cf[4];

	uint64_t		ass_efer;
	uint64_t		ass_svm_state;
	uint64_t		ass_vmcb_phys;
	uint64_t		ass_avic;
	uint8_t			_reserved_f0_f7[8];
	uint32_t		ass_mstate;		/* PPR only, undoc */

	uint32_t		ass_revid;
	union {
		struct {	/* amd64 */
			uint32_t	ass_smbase_arch;
			uint8_t		_reserved_104_117[20];
			uint64_t	ass_ssp;
		};
		struct {	/* PPR */
			uint64_t	ass_smbase_ppr;
			uint8_t		_reserved_108_11f[24];
		};
	};

	/*
	 * These are expected to be meaningful only when an SMI occurs while
	 * running a guest with SEV-SNP enabled.
	 */
	uint64_t		ass_guest_pat;
	uint64_t		ass_host_efer;
	uint64_t		ass_host_cr4;
	uint64_t		ass_nested_cr3;
	uint64_t		ass_host_cr0;

	uint64_t		ass_cr4;
	uint64_t		ass_cr3;
	uint64_t		ass_cr0;
	uint64_t		ass_dr7;
	uint64_t		ass_dr6;

	/*
	 * Sorta like a normal frame, but in a completely different order.
	 */
	uint64_t		ass_rflags;
	uint64_t		ass_rip;
	uint64_t		ass_r15;
	uint64_t		ass_r14;
	uint64_t		ass_r13;
	uint64_t		ass_r12;
	uint64_t		ass_r11;
	uint64_t		ass_r10;
	uint64_t		ass_r9;
	uint64_t		ass_r8;
	uint64_t		ass_rdi;
	uint64_t		ass_rsi;
	uint64_t		ass_rbp;
	uint64_t		ass_rsp;
	uint64_t		ass_rbx;
	uint64_t		ass_rdx;
	uint64_t		ass_rcx;
	uint64_t		ass_rax;
} amd64_smm_state_t;
#pragma pack()

/* ass_trap_offset fields */
#define	AMD64_SMM_TRAP_OFF_GET_PORT(r)		bitx32(r, 31, 16)
#define	AMD64_SMM_TRAP_OFF_GET_BPR(r)		bitx32(r, 15, 12)
#define	AMD64_SMM_TRAP_OFF_GET_TF(r)		bitx32(r, 11, 11)
#define	AMD64_SMM_TRAP_OFF_GET_SZ32(r)		bitx32(r, 6, 6)
#define	AMD64_SMM_TRAP_OFF_GET_SZ16(r)		bitx32(r, 5, 5)
#define	AMD64_SMM_TRAP_OFF_GET_SZ8(r)		bitx32(r, 4, 4)
#define	AMD64_SMM_TRAP_OFF_GET_REP(r)		bitx32(r, 3, 3)
#define	AMD64_SMM_TRAP_OFF_GET_STR(r)		bitx32(r, 2, 2)
#define	AMD64_SMM_TRAP_OFF_GET_V(r)		bitx32(r, 1, 1)
#define	AMD64_SMM_TRAP_OFF_GET_RW(r)		bitx32(r, 0, 0)
#define	AMD64_SMM_TRAP_OFF_RW_W			0
#define	AMD64_SMM_TRAP_OFF_RW_R			1

/* ass_smi_status fields: core-local SMI source description */
#define	AMD64_SMM_LSS_GET_SRC_MCA(r)		bitx32(r, 18, 18)
#define	AMD64_SMM_LSS_GET_SRC_LVT_EXT(r)	bitx32(r, 17, 17)
#define	AMD64_SMM_LSS_GET_SRC_LVT_LEGACY(r)	bitx32(r, 16, 16)
#define	AMD64_SMM_LSS_GET_WRMSR(r)		bitx32(r, 11, 11)
#define	AMD64_SMM_LSS_GET_MCE_REDIR(r)		bitx32(r, 8, 8)
#define	AMD64_SMM_LSS_GET_IOTRAP(r)		bitx32(r, 3, 0)

/* ass_ior_flag values */
#define	AMD64_SMM_IOR_FLAG_RESTART		0xFF
#define	AMD64_SMM_IOR_FLAG_NORESTART		0

/* ass_ahr_flag fields */
#define	AMD64_SMM_AHR_FLAG_GET(r)		bitx8(r, 0, 0)

/* ass_nmi_mask fields */
#define	AMD64_SMM_NMI_MASK_GET(r)		bitx8(r, 0, 0)
#define	AMD64_SMM_NMI_MASK_SET(r, v)		bitset8(r, 0, 0, v)

/* ass_svm_state fields */
#define	AMD64_SMM_SVM_STATE_GET_HOST_IF(r)	bitx64(r, 3, 3)
#define	AMD64_SMM_SVM_STATE_GET(r)		bitx64(r, 2, 0)
#define	AMD64_SMM_SVM_STATE_NON_GUEST		0
#define	AMD64_SMM_SVM_STATE_GUEST		2
#define	AMD64_SMM_SVM_STATE_GUEST_SNP		6

/* ass_revid fields */
#define	AMD64_SMM_REVID_GET_BRL(r)	bitx32(r, 17, 17)
#define	AMD64_SMM_REVID_GET_IOR(r)	bitx32(r, 16, 16)
#define	AMD64_SMM_REVID_GET_LEVEL(r)	bitx32(r, 15, 0)
#define	AMD64_SMM_REVID_LEVEL_0		0x0064

/* ass_smbase_ppr fields */
#define	AMD64_SMM_SMBASE_PPR_GET(r)	bitx64(r, 31, 0)

#endif	/* !_ASM */

/*
 * Of this group, only SMITRIG is architecturally defined, but all of them
 * generally exist on Zen2/Zen3 processors and almost certainly others.
 */
#define	MSR_AMD_SMI_IO_TRAP_0		0xC0010050
#define	MSR_AMD_SMI_IO_TRAP_1		0xC0010051
#define	MSR_AMD_SMI_IO_TRAP_2		0xC0010052
#define	MSR_AMD_SMI_IO_TRAP_3		0xC0010053
#define	MSR_AMD_SMI_IO_TRAP_CTL		0xC0010054
#define	MSR_AMD_SMITRIG			0xC0010056

/*
 * SMBASE, SMM_ADDR, and SMM_MASK are architecturally defined.
 */
#define	MSR_AMD_SMBASE			0xC0010111

#define	AMD64_SMBASE_HANDLER_OFF	0x8000
#define	AMD64_SMBASE_SS_OFF		0xFE00

#define	MSR_AMD_SMM_ADDR		0xC0010112
#define	MSR_AMD_SMM_MASK		0xC0010113

#define	AMD64_ASEG_BASE			0xA0000
#define	AMD64_ASEG_LEN			0x20000
#define	AMD64_TSEG_ALIGN		0x20000

/*
 * The PFEH registers are non-architectural but exist on all AMD processors that
 * support PFEH (which, again, is at least most if not all current EPYC and
 * Ryzen parts as of the Zen2/Zen3 era).
 */
#define	MSR_AMD_PFEH_CFG		0xC0010120
#define	MSR_AMD_PFEH_CLOAK_CFG		0xC0010121
#define	MSR_AMD_PFEH_DEF_INT		0xC0010122

#ifndef	_ASM

#define	AMD64_SMM_MASK_GET_TSEG_MASK(r)		bitx64(r, 47, 17)
#define	AMD64_SMM_MASK_SET_TSEG_MASK(r, v)	bitset64(r, 47, 17, v)
#define	AMD64_SMM_MASK_GET_T_MTYPE_DRAM(r)	bitx64(r, 14, 12)
#define	AMD64_SMM_MASK_SET_T_MTYPE_DRAM(r, v)	bitset64(r, 14, 12, v)
#define	AMD64_SMM_MASK_GET_A_MTYPE_DRAM(r)	bitx64(r, 10, 8)
#define	AMD64_SMM_MASK_SET_A_MTYPE_DRAM(r, v)	bitset64(r, 10, 8, v)
/* Applicable to both T_MTYPE and A_MTYPE for DRAM */
#define	AMD64_SMM_MASK_MTYPE_DRAM_UC	0
#define	AMD64_SMM_MASK_MTYPE_DRAM_WC	1
#define	AMD64_SMM_MASK_MTYPE_DRAM_WT	4
#define	AMD64_SMM_MASK_MTYPE_DRAM_WP	5
#define	AMD64_SMM_MASK_MTYPE_DRAM_WB	6
#define	AMD64_SMM_MASK_GET_T_MTYPE_IO(r)	bitx64(r, 5, 5)
#define	AMD64_SMM_MASK_SET_T_MTYPE_IO(r, v)	bitset64(r, 5, 5, v)
#define	AMD64_SMM_MASK_GET_A_MTYPE_IO(r)	bitx64(r, 4, 4)
#define	AMD64_SMM_MASK_SET_A_MTYPE_IO(r, v)	bitset64(r, 4, 4, v)
/* Applicable to both T_MTYPE and A_MTYPE for IO */
#define	AMD64_SMM_MASK_MTYPE_IO_UC	0
#define	AMD64_SMM_MASK_MTYPE_IO_WC	1
#define	AMD64_SMM_MASK_GET_T_CLOSE(r)		bitx64(r, 3, 3)
#define	AMD64_SMM_MASK_SET_T_CLOSE(r, v)	bitset64(r, 3, 3, v)
#define	AMD64_SMM_MASK_GET_A_CLOSE(r)		bitx64(r, 2, 2)
#define	AMD64_SMM_MASK_SET_A_CLOSE(r, v)	bitset64(r, 2, 2, v)
#define	AMD64_SMM_MASK_GET_T_VALID(r)		bitx64(r, 1, 1)
#define	AMD64_SMM_MASK_SET_T_VALID(r, v)	bitset64(r, 1, 1, v)
#define	AMD64_SMM_MASK_GET_A_VALID(r)		bitx64(r, 0, 0)
#define	AMD64_SMM_MASK_SET_A_VALID(r, v)	bitset64(r, 0, 0, v)

/* HWCR and a few non-SMM fields are defined in controlregs.h */
#define	AMD64_HWCR_GET_SMM_BASE_LOCK(r)		bitx64(r, 31, 31)
#define	AMD64_HWCR_SET_SMM_BASE_LOCK(r, v)	bitset64(r, 31, 31, v)
#define	AMD64_HWCR_GET_RSM_SPCYC_DIS(r)		bitx64(r, 14, 14)
#define	AMD64_HWCR_SET_RSM_SPCYC_DIS(r, v)	bitset64(r, 14, 14, v)
#define	AMD64_HWCR_GET_SMI_SPCYC_DIS(r)		bitx64(r, 13, 13)
#define	AMD64_HWCR_SET_SMI_SPCYC_DIS(r, v)	bitset64(r, 13, 13, v)
#define	AMD64_HWCR_GET_SMM_LOCK(r)		bitx64(r, 0, 0)
#define	AMD64_HWCR_SET_SMM_LOCK(r)		bitset64(r, 0, 0, 1)

#endif	/* !_ASM */

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SMM_AMD64_H */
