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

#ifndef _AMDZEN_H
#define	_AMDZEN_H

#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/list.h>
#include <sys/pci.h>
#include <sys/taskq.h>
#include <sys/bitmap.h>
#include <sys/bitext.h>

/*
 * This header describes properties of the data fabric and our internal state
 * for the Zen Nexus driver.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The data fabric devices are always defined to be on PCI bus zero starting at
 * device 0x18.
 */
#define	AMDZEN_DF_BUSNO		0x00
#define	AMDZEN_DF_FIRST_DEVICE	0x18

/*
 * The maximum amount of Data Fabric node's we can see. In Zen 1 there were up
 * to four per package.
 */
#define	AMDZEN_MAX_DFS		0x8

/*
 * The maximum number of PCI functions we expect to encounter on the data
 * fabric.
 */
#define	AMDZEN_MAX_DF_FUNCS	0x8


/*
 * Registers in the data fabric space that we care about for the purposes of the
 * nexus driver understanding itself.
 */

/*
 * This set of registers provides us access to the count of instances in the
 * data fabric and then a number of different pieces of information about them
 * like their type. Note, these registers require indirect access because the
 * information cannot be broadcast.
 */
#define	AMDZEN_DF_F0_FBICNT	0x40
#define	AMDZEN_DF_F0_FBICNT_COUNT(x)	BITX(x, 7, 0)
#define	AMDZEN_DF_F0_FBIINFO0	0x44
#define	AMDZEN_DF_F0_FBIINFO0_TYPE(x)		BITX(x, 3, 0)
typedef enum {
	AMDZEN_DF_TYPE_CCM = 0,
	AMDZEN_DF_TYPE_GCM,
	AMDZEN_DF_TYPE_NCM,
	AMDZEN_DF_TYPE_IOMS,
	AMDZEN_DF_TYPE_CS,
	AMDZEN_DF_TYPE_TCDX,
	AMDZEN_DF_TYPE_PIE,
	AMDZEN_DF_TYPE_SPF,
	AMDZEN_DF_TYPE_LLC,
	AMDZEN_DF_TYPE_CAKE
} amdzen_df_type_t;
#define	AMDZEN_DF_F0_FBIINFO0_SDP_WIDTH(x)	BITX(x, 5, 4)
typedef enum {
	AMDZEN_DF_SDP_W_64 = 0,
	AMDZEN_DF_SDP_W_128,
	AMDZEN_DF_SDP_W_256,
	AMDZEN_DF_SDP_W_512
} amdzen_df_sdp_width_t;
#define	AMDZEN_DF_F0_FBIINFO0_ENABLED(x)	BITX(x, 6, 6)
#define	AMDZEN_DF_F0_FBIINFO0_FTI_WIDTH(x)	BITX(x, 9, 8)
typedef enum {
	AMDZEN_DF_FTI_W_64 = 0,
	AMDZEN_DF_FTI_W_128,
	AMDZEN_DF_FTI_W_256,
	AMDZEN_DF_FTI_W_512
} amdzen_df_fti_width_t;
#define	AMDZEN_DF_F0_FBIINFO0_SDP_PCOUNT(x)	BITX(x, 13, 12)
#define	AMDZEN_DF_F0_FBIINFO0_FTI_PCOUNT(x)	BITX(x, 18, 16)
#define	AMDZEN_DF_F0_FBIINFO0_HAS_MCA(x)	BITX(x, 23, 23)
#define	AMDZEN_DF_F0_FBIINFO0_SUBTYPE(x)	BITX(x, 26, 24)
#define	AMDZEN_DF_SUBTYPE_NONE	0
typedef enum {
	AMDZEN_DF_CAKE_SUBTYPE_GMI = 1,
	AMDZEN_DF_CAKE_SUBTYPE_xGMI = 2
} amdzen_df_cake_subtype_t;

typedef enum {
	AMDZEN_DF_IOM_SUBTYPE_IOHUB = 1,
} amdzen_df_iom_subtype_t;

typedef enum {
	AMDZEN_DF_CS_SUBTYPE_UMC = 1,
	AMDZEN_DF_CS_SUBTYPE_CCIX = 2
} amdzen_df_cs_subtype_t;

#define	AMDZEN_DF_F0_FBIINFO1	0x48
#define	AMDZEN_DF_F0_FBIINFO1_FTI0_NINSTID(x)	BITX(x, 7, 0)
#define	AMDZEN_DF_F0_FBIINFO1_FTI1_NINSTID(x)	BITX(x, 15, 8)
#define	AMDZEN_DF_F0_FBIINFO1_FTI2_NINSTID(x)	BITX(x, 23, 16)
#define	AMDZEN_DF_F0_FBIINFO1_FTI3_NINSTID(x)	BITX(x, 31, 24)
#define	AMDZEN_DF_F0_FBIINFO2	0x4c
#define	AMDZEN_DF_F0_FBIINFO2_FTI4_NINSTID(x)	BITX(x, 7, 0)
#define	AMDZEN_DF_F0_FBIINFO2_FTI5_NINSTID(x)	BITX(x, 15, 8)
#define	AMDZEN_DF_F0_FBIINFO3	0x50
#define	AMDZEN_DF_F0_FBIINFO3_INSTID(x)		BITX(x, 7, 0)
#define	AMDZEN_DF_F0_FBIINFO3_FABID(x)		BITX(x, 13, 8)

/*
 * This register contains the information about the configuration of PCIe buses.
 * We care about finding which one has our BUS A, which is required to map it to
 * the northbridge.
 */
#define	AMDZEN_DF_F0_CFG_ADDR_CTL	0x84
#define	AMDZEN_DF_F0_CFG_ADDR_CTL_BUS_NUM(x)	BITX(x, 7, 0)

/*
 * This register describes the capabilities of the data fabric.
 */
#define	AMDZEN_DF_F0_CAPABILITY		0x90
#define	AMDZEN_DF_F0_CAPABILITY_POISON(x)	BITX(x, 0, 0)
#define	AMDZEN_DF_F0_CAPABILITY_SPF(x)		BITX(x, 1, 1)

/*
 * These registers contain the mapping of PCI Buses to IOMS instances of the
 * northbridge. There are 8 of these registers, which generally means one per
 * NBIO instance in a 2P system on EPYC based systems (there are less on other
 * platforms).
 */
#define	AMDZEN_DF_F0_CFGMAP(x)		(0xa0 + ((x) * 4))
#define	AMDZEN_DF_F0_MAX_CFGMAP		8
#define	AMDZEN_DF_F0_GET_CFGMAP_RE(r)		bitx32(r, 0, 0)
#define	AMDZEN_DF_F0_GET_CFGMAP_WE(r)		bitx32(r, 1, 1)
#define	AMDZEN_DF_F0_GET_CFGMAP_DEST_ID(r)	bitx32(r, 13, 4)
#define	AMDZEN_DF_F0_GET_CFGMAP_BUS_BASE(r)	bitx32(r, 23, 16)
#define	AMDZEN_DF_F0_GET_CFGMAP_BUS_LIMIT(r)	bitx32(r, 31, 24)

/*
 * This is one of a pair of registers which are used to control and configure
 * the destination of I/O space.
 */
#define	AMDZEN_DF_F0_IO_BASE(x)		(0xc0 + ((x) * 8))
#define	AMDZEN_DF_F0_MAX_IO_RULES	8
#define	AMDZEN_DF_F0_GET_IO_BASE_RE(r)		bitx32(r, 0, 0)
#define	AMDZEN_DF_F0_GET_IO_BASE_WE(r)		bitx32(r, 1, 1)
#define	AMDZEN_DF_F0_GET_IO_BASE_IE(r)		bitx32(r, 5, 5)
#define	AMDZEN_DF_F0_GET_IO_BASE_BASE(r)	bitx32(r, 24, 12)
#define	AMDZEN_DF_F0_IO_BASE_SHIFT		12

/*
 * This register pairs with the one above and defines the limit of the given I/O
 * space entry.
 */
#define	AMDZEN_DF_F0_IO_LIMIT(x)	(0xc4 + ((x) * 8))
#define	AMDZEN_DF_FO_GET_IO_LIMIT_DEST_ID(r)	bitx32(r, 9, 0)
#define	AMDZEN_DF_F0_GET_IO_LIMIT_LIMIT(r)	bitx32(r, 24, 12)
#define	AMDZEN_DF_F0_IO_LIMIT_SHIFT		12

/*
 * This register is used to determine whether the DRAM hole is valid or not. The
 * DRAM hole is used to determine where MMIO begins below 4 GiB.
 */
#define	AMDZEN_DF_F0_DRAM_HOLE		0x104
#define	AMDZEN_DF_F0_GET_DRAM_HOLE_VALID(r)		bitx32(r, 0, 0)
#define	AMDZEN_DF_F0_GET_DRAM_HOLE_BASE(r)		bitx32(r, 31, 24)

/*
 * DRAM base address register. This determines how DRAM is routed. Note, that
 * like other parts of the data fabric this varies on the generation. Please
 * note that this is sepcific to Zen 2/3. Zen 1 had a different layout. Also,
 * the number of these vary based on the type of instance.
 */
#define	AMDZEN_Z2_3_DF_F0_DRAM_BASE(x)	(0x110 + ((x) * 8))
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_VALID(r)	bitx32(r, 0, 0)
#define	AMDZEN_Z2_3_DF_FO_GET_DRAM_BASE_HOLE_EN(r)	bitx32(r, 1, 1)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_CHAN_ILEAVE(r)	bitx32(r, 5, 2)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_DIE_ILEAVE(r)	bitx32(r, 7, 6)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_SOCK_ILEAVE(r)	bitx32(r, 8, 8)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_ADDR_ILEAVE(r)	bitx32(r, 11, 9)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_BASE_BASE(r)		bitx32(r, 31, 12)
#define	AMDZEN_Z2_3_DF_F0_DRAM_BASE_BASE_SHIFT	28

/*
 * DRAM limit address registers. These pair with the above base register and
 * have the same gotchas.
 */
#define	AMDZEN_Z2_3_DF_F0_DRAM_LIMIT(x)	(0x114 + ((x) * 8))
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_DEST_ID(r)	bitx32(r, 9, 0)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_BUS_BREAK(r)	bitx32(r, 10, 10)
#define	AMDZEN_Z2_3_DF_F0_GET_DRAM_LIMIT_LIMIT(r)	bitx32(r, 31, 12)
#define	AMDZEN_Z2_3_DF_F0_DRAM_LIMIT_LIMIT_SHIFT	28

/*
 * MMIO base and limit registers. These combine with a control register to
 * determine where MMIO ranges go and are used. Unlike with DRAM it does seem
 * that every instance that has something has them for all of them. This
 * register contains address[47:16].
 */
#define	AMDZEN_DF_F0_MAX_MMIO_RULES	16
#define	AMDZEN_DF_F0_MMIO_BASE(x)	(0x200 + ((x) * 0x10))
#define	AMDZEN_DF_F0_MMIO_LIMIT(x)	(0x204 + ((x) * 0x10))
#define	AMDZEN_DF_F0_MMIO_SHIFT		16

#define	AMDZEN_Z2_3_DF_F0_MMIO_CTRL(x)	(0x208 + ((x) * 0x10))
#define	AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_RE(r)		bitx32(r, 0, 0)
#define	AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_WE(r)		bitx32(r, 1, 0)
#define	AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_CPU(r)		bitx32(r, 2, 2)
#define	AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_DEST_ID(r)	bitx32(r, 13, 4)
#define	AMDZEN_Z2_3_DF_F0_GET_MMIO_CTRL_NP(r)		bitx32(r, 16, 16)


/*
 * Registers that describe how the system is actually put together.
 */
#define	AMDZEN_DF_F1_SYSCFG	0x200
#define	AMDZEN_DF_F1_SYSCFG_DIE_PRESENT(X)	BITX(x, 7, 0)
#define	AMDZEN_DF_F1_SYSCFG_DIE_TYPE(x)		BITX(x, 18, 11)
#define	AMDZEN_DF_F1_SYSCFG_MYDIE_TYPE(x)	BITX(x, 24, 23)
typedef enum {
	AMDZEN_DF_DIE_TYPE_CPU	= 0,
	AMDZEN_DF_DIE_TYPE_APU,
	AMDZEN_DF_DIE_TYPE_dGPU
} amdzen_df_die_type_t;
#define	AMDZEN_DF_F1_SYSCFG_OTHERDIE_TYPE(x)	BITX(x, 26, 25)
#define	AMDZEN_DF_F1_SYSCFG_OTHERSOCK(x)	BITX(x, 27, 27)
#define	AMDZEN_DF_F1_SYSCFG_NODEID(x)		BITX(x, 30, 28)

/*
 * This register describes the total number of components that exist across all
 * sockets.
 */
#define	AMDZEN_DF_F1_SYSCOMP	0x204
#define	AMDZEN_DF_F1_SYSCOMP_PIE(x)		BITX(x, 7, 0)
#define	AMDZEN_DF_F1_SYSCOMP_GCM(X)		BITX(x, 15, 8)
#define	AMDZEN_DF_F1_SYSCOMP_IOMS(x)		BITX(x, 23, 16)

#define	AMDZEN_DF_F1_FIDMASK0	0x208
#define	AMDZEN_DF_F1_FIDMASK0_COMP_MASK(x)	BITX(x, 9, 0)
#define	AMDZEN_DF_F1_FIDMASK0_NODE_MASK(x)	BITX(x, 25, 16)
#define	AMDZEN_DF_F1_FIDMASK1	0x20C
#define	AMDZEN_DF_F1_FIDMASK1_NODE_SHIFT(x)	BITX(x, 3, 0)
#define	AMDZEN_DF_F1_FIDMASK1_SKT_SHIFT(x)	BITX(x, 9, 8)
#define	AMDZEN_DF_F1_FIDMASK1_DIE_MASK(x)	BITX(x, 18, 16)
#define	AMDZEN_DF_F1_FIDMASK1_SKT_MASK(x)	BITX(x, 26, 24)

/*
 * These two registers define information about the PSP and SMU on local and
 * remote dies (from the context of the DF instance). The bits are the same.
 */
#define	AMDZEN_DF_F1_PSPSMU_LOCAL	0x268
#define	AMDZEN_DF_F1_PSPSMU_REMOTE	0x268
#define	AMDZEN_DF_F1_PSPSMU_SMU_VALID(x)	BITX(x, 0, 0)
#define	AMDZEN_DF_F1_PSPSMU_SMU_UNITID(x)	BITX(x, 6, 1)
#define	AMDZEN_DF_F1_PSPSMU_SMU_COMPID(x)	BITX(x, 15, 8)
#define	AMDZEN_DF_F1_PSPSMU_PSP_VALID(x)	BITX(x, 16, 16)
#define	AMDZEN_DF_F1_PSPSMU_PSP_UNITID(x)	BITX(x, 22, 17)
#define	AMDZEN_DF_F1_PSPSMU_PSP_COMPID(x)	BITX(x, 31, 24)

#define	AMDZEN_DF_F1_CAKE_ENCR		0x2cc

/*
 * These registers are used to define Indirect Access, commonly known as FICAA
 * and FICAD for the system. While there are multiple copies of the indirect
 * access registers in device 4, we're only allowed access to one set of those
 * (which are the ones present here). Specifically the OS is given access to set
 * 3.
 */
#define	AMDZEN_DF_F4_FICAA	0x5c
#define	AMDZEN_DF_F4_FICAA_TARG_INST	(1 << 0)
#define	AMDZEN_DF_F4_FICAA_SET_REG(x)	((x) & 0x3fc)
#define	AMDZEN_DF_F4_FICAA_SET_FUNC(x)	(((x) & 0x7) << 11)
#define	AMDZEN_DF_F4_FICAA_SET_64B	(1 << 14)
#define	AMDZEN_DF_F4_FICAA_SET_INST(x)	(((x) & 0xff) << 16)
#define	AMDZEN_DF_F4_FICAD_LO	0x98
#define	AMDZEN_DF_F4_FICAD_HI	0x9c

/*
 * Northbridge registers that are relevant for the nexus, mostly for SMN.
 */
#define	AMDZEN_NB_SMN_DEVNO	0x00
#define	AMDZEN_NB_SMN_FUNCNO	0x00
#define	AMDZEN_NB_SMN_ADDR	0x60
#define	AMDZEN_NB_SMN_DATA	0x64

/*
 * AMD PCI ID for reference
 */
#define	AMDZEN_PCI_VID_AMD	0x1022

/*
 * Hygon PCI ID for reference
 */
#define	AMDZEN_PCI_VID_HYGON	0x1d94

typedef enum {
	AMDZEN_STUB_TYPE_DF,
	AMDZEN_STUB_TYPE_NB
} amdzen_stub_type_t;

typedef struct {
	list_node_t		azns_link;
	dev_info_t		*azns_dip;
	uint16_t		azns_vid;
	uint16_t		azns_did;
	uint16_t		azns_bus;
	uint16_t		azns_dev;
	uint16_t		azns_func;
	ddi_acc_handle_t	azns_cfgspace;
} amdzen_stub_t;

typedef enum  {
	AMDZEN_DFE_F_MCA	= 1 << 0,
	AMDZEN_DFE_F_ENABLED	= 1 << 1
} amdzen_df_ent_flags_t;

typedef struct {
	uint8_t adfe_drvid;
	amdzen_df_ent_flags_t adfe_flags;
	amdzen_df_type_t adfe_type;
	uint8_t adfe_subtype;
	uint8_t adfe_fabric_id;
	uint8_t adfe_inst_id;
	amdzen_df_sdp_width_t adfe_sdp_width;
	amdzen_df_fti_width_t adfe_fti_width;
	uint8_t adfe_sdp_count;
	uint8_t adfe_fti_count;
	uint32_t adfe_info0;
	uint32_t adfe_info1;
	uint32_t adfe_info2;
	uint32_t adfe_info3;
	uint32_t adfe_syscfg;
	uint32_t adfe_mask0;
	uint32_t adfe_mask1;
} amdzen_df_ent_t;

typedef enum {
	AMDZEN_DF_F_VALID		= 1 << 0,
	AMDZEN_DF_F_FOUND_NB		= 1 << 1
} amdzen_df_flags_t;

typedef struct {
	amdzen_df_flags_t	adf_flags;
	uint_t		adf_nb_busno;
	amdzen_stub_t	*adf_funcs[AMDZEN_MAX_DF_FUNCS];
	amdzen_stub_t	*adf_nb;
	uint_t		adf_nents;
	amdzen_df_ent_t	*adf_ents;
	uint32_t	adf_nodeid;
	uint32_t	adf_syscfg;
	uint32_t	adf_mask0;
	uint32_t	adf_mask1;
} amdzen_df_t;

typedef enum {
	AMDZEN_F_UNSUPPORTED		= 1 << 0,
	AMDZEN_F_DEVICE_ERROR		= 1 << 1,
	AMDZEN_F_MAP_ERROR		= 1 << 2,
	AMDZEN_F_SCAN_DISPATCHED	= 1 << 3,
	AMDZEN_F_SCAN_COMPLETE		= 1 << 4,
	AMDZEN_F_ATTACH_DISPATCHED	= 1 << 5,
	AMDZEN_F_ATTACH_COMPLETE	= 1 << 6
} amdzen_flags_t;

#define	AMDZEN_F_TASKQ_MASK	(AMDZEN_F_SCAN_DISPATCHED | \
    AMDZEN_F_SCAN_COMPLETE | AMDZEN_F_ATTACH_DISPATCHED | \
    AMDZEN_F_ATTACH_COMPLETE)

typedef struct amdzen {
	kmutex_t	azn_mutex;
	kcondvar_t	azn_cv;
	amdzen_flags_t	azn_flags;
	dev_info_t	*azn_dip;
	taskqid_t	azn_taskqid;
	uint_t		azn_nscanned;
	uint_t		azn_npresent;
	list_t		azn_df_stubs;
	list_t		azn_nb_stubs;
	uint_t		azn_ndfs;
	amdzen_df_t	azn_dfs[AMDZEN_MAX_DFS];
} amdzen_t;

typedef enum {
	AMDZEN_C_SMNTEMP = 1,
	AMDZEN_C_USMN,
	AMDZEN_C_ZEN_UDF
} amdzen_child_t;

/*
 * Functions for stubs.
 */
extern int amdzen_attach_stub(dev_info_t *, ddi_attach_cmd_t);
extern int amdzen_detach_stub(dev_info_t *, ddi_detach_cmd_t);

#ifdef __cplusplus
}
#endif

#endif /* _AMDZEN_H */
