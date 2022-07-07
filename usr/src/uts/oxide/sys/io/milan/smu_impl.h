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

#ifndef _SYS_IO_MILAN_SMU_IMPL_H
#define	_SYS_IO_MILAN_SMU_IMPL_H

/*
 * Definitions for the System Management Unit (SMU), which is probably the same
 * thing as the hidden core called MP1 in some documentation.  Its
 * responsibilities are mainly power and thermal management, but it also manages
 * the DXIO subsystem and PCIe hotplug.
 */

#include <sys/bitext.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SMN addresses to reach the SMU for RPCs.
 */
#define	MILAN_SMU_SMN_RPC_REQ	0x3b10530
#define	MILAN_SMU_SMN_RPC_RESP	0x3b1057c
#define	MILAN_SMU_SMN_RPC_ARG0	0x3b109c4
#define	MILAN_SMU_SMN_RPC_ARG1	0x3b109c8
#define	MILAN_SMU_SMN_RPC_ARG2	0x3b109cc
#define	MILAN_SMU_SMN_RPC_ARG3	0x3b109d0
#define	MILAN_SMU_SMN_RPC_ARG4	0x3b109d4
#define	MILAN_SMU_SMN_RPC_ARG5	0x3b109d8

/*
 * SMU RPC Response codes
 */
#define	MILAN_SMU_RPC_NOTDONE	0x00
#define	MILAN_SMU_RPC_OK	0x01
#define	MILAN_SMU_RPC_EBUSY	0xfc
#define	MILAN_SMU_RPC_EPREREQ	0xfd
#define	MILAN_SMU_RPC_EUNKNOWN	0xfe
#define	MILAN_SMU_RPC_ERROR	0xff

/*
 * SMU RPC Operation Codes. Note, these are tied to firmware and therefore may
 * not be portable between Rome, Milan, or other processors.
 */
#define	MILAN_SMU_OP_TEST		0x01
#define	MILAN_SMU_OP_GET_VERSION	0x02
#define	MILAN_SMU_OP_GET_VERSION_MAJOR(x)	bitx32(x, 23, 16)
#define	MILAN_SMU_OP_GET_VERSION_MINOR(x)	bitx32(x, 15, 8)
#define	MILAN_SMU_OP_GET_VERSION_PATCH(x)	bitx32(x, 7, 0)
#define	MILAN_SMU_OP_ENABLE_FEATURE	0x03
#define	MILAN_SMU_OP_DISABLE_FEATURE	0x04
#define	MILAN_SMU_OP_HAVE_AN_ADDRESS	0x05
#define	MILAN_SMU_OP_TOOLS_ADDRESS	0x06
#define	MILAN_SMU_OP_DEBUG_ADDRESS	0x07
#define	MILAN_SMU_OP_DXIO		0x08
#define	MILAN_SMU_OP_DC_BOOT_CALIB	0x0c
#define	MILAN_SMU_OP_GET_BRAND_STRING	0x0d
#define	MILAN_SMU_OP_TX_PP_TABLE	0x10
#define	MILAN_SMU_OP_TX_PCIE_HP_TABLE	0x12
#define	MILAN_SMU_OP_START_HOTPLUG	0x18
#define	MILAN_SMU_OP_START_HOTPLUG_POLL		0x10
#define	MILAN_SMU_OP_START_HOTPLUG_FWFIRST	0x20
#define	MILAN_SMU_OP_START_HOTPLUG_RESET	0x40
#define	MILAN_SMU_OP_I2C_SWITCH_ADDR	0x1a
#define	MILAN_SMU_OP_SET_HOPTLUG_FLAGS	0x1d
#define	MILAN_SMU_OP_SET_POWER_GATE	0x2a
#define	MILAN_SMU_OP_MAX_ALL_CORES_FREQ	0x2b
#define	MILAN_SMU_OP_SET_NBIO_LCLK	0x34
#define	MILAN_SMU_OP_SET_L3_CREDIT_MODE	0x35
#define	MILAN_SMU_OP_FLL_BOOT_CALIB	0x37
#define	MILAN_SMU_OP_DC_SOC_BOOT_CALIB	0x38
#define	MILAN_SMU_OP_HSMP_PAY_ATTN	0x41
#define	MILAN_SMU_OP_SET_APML_FLOOD	0x42
#define	MILAN_SMU_OP_FDD_BOOT_CALIB	0x43
#define	MILAN_SMU_OP_VDDCR_CPU_LIMIT	0x44
#define	MILAN_SMU_OP_SET_EDC_TRACK	0x45
#define	MILAN_SMU_OP_SET_DF_IRRITATOR	0x46
#define	MILAN_SMU_OP_HAVE_A_HP_ADDRESS	0x47

/*
 * For unknown reasons we have multiple ways to give the SMU an address, and
 * they're apparently operation-specific.  Distinguish them with this.
 */
typedef enum milan_smu_addr_kind {
	MSAK_GENERIC,
	MSAK_HOTPLUG
} milan_smu_addr_kind_t;

/*
 * A structure that can be used to pass around a SMU RPC request.
 */
typedef struct milan_smu_rpc {
	uint32_t	msr_req;
	uint32_t	msr_resp;
	uint32_t	msr_arg0;
	uint32_t	msr_arg1;
	uint32_t	msr_arg2;
	uint32_t	msr_arg3;
	uint32_t	msr_arg4;
	uint32_t	msr_arg5;
} milan_smu_rpc_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_IO_MILAN_SMU_IMPL_H */
