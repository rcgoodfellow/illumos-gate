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

/*
 * Various routines and things to access, initialize, understand, and manage
 * Milan's I/O fabric. This consists of both the data fabric and the
 * northbridges.
 *
 * ---------------------
 * Physical Organization
 * ---------------------
 *
 * In AMD's Zen 2 and 3 designs, the CPU socket is organized as a series of
 * chiplets with a series of compute complexes and then a central I/O die.
 * cpuid.c has an example of what this looks like. Critically, this I/O die is
 * the major device that we are concerned with here as it bridges the cores to
 * basically the outside world through a combination of different devices and
 * I/O paths.
 *
 * XXX More on physical organization, terms, and related. ASCII art.
 */

#include <sys/types.h>
#include <sys/ksynch.h>
#include <sys/pci.h>
#include <sys/pci_cfgspace.h>
#include <sys/pci_cfgspace_impl.h>
#include <sys/pcie.h>
#include <sys/spl.h>
#include <sys/debug.h>
#include <sys/prom_debug.h>
#include <sys/x86_archext.h>
#include <sys/bitext.h>
#include <sys/sysmacros.h>
#include <sys/memlist_impl.h>
#include <sys/machsystm.h>
#include <sys/plat/pci_prd.h>
#include <sys/apic.h>
#include <sys/cpuvar.h>
#include <sys/io/fch.h>
#include <sys/io/fch/gpio.h>
#include <sys/io/fch/i2c.h>
#include <sys/io/fch/iomux.h>
#include <sys/io/fch/misc.h>
#include <sys/io/fch/pmio.h>
#include <sys/io/fch/rmtgpio.h>
#include <sys/io/fch/smi.h>
#include <sys/io/milan/fabric.h>
#include <sys/io/milan/fabric_impl.h>
#include <sys/io/milan/ccx.h>
#include <sys/io/milan/dxio_impl.h>
#include <sys/io/milan/ioapic.h>
#include <sys/io/milan/iohc.h>
#include <sys/io/milan/iommu.h>
#include <sys/io/milan/nbif.h>
#include <sys/io/milan/nbif_impl.h>
#include <sys/io/milan/pcie.h>
#include <sys/io/milan/pcie_impl.h>
#include <sys/io/milan/pcie_rsmu.h>
#include <sys/io/milan/smu_impl.h>

#include <asm/bitmap.h>

#include <sys/amdzen/df.h>

#include <milan/milan_apob.h>
#include <milan/milan_physaddrs.h>

/*
 * XXX This header contains a lot of the definitions that the broader system is
 * currently using for register definitions. For the moment we're trying to keep
 * this consolidated, hence this wacky include path.
 */
#include <io/amdzen/amdzen.h>

/*
 * This is a structure that we can use internally to pass around a DXIO RPC
 * request.
 */
typedef struct milan_dxio_rpc {
	uint32_t	mdr_req;
	uint32_t	mdr_dxio_resp;
	uint32_t	mdr_smu_resp;
	uint32_t	mdr_engine;
	uint32_t	mdr_arg0;
	uint32_t	mdr_arg1;
	uint32_t	mdr_arg2;
	uint32_t	mdr_arg3;
} milan_dxio_rpc_t;

typedef struct milan_bridge_info {
	uint8_t	mpbi_dev;
	uint8_t	mpbi_func;
} milan_bridge_info_t;

/*
 * These three tables encode knowledge about how the SoC assigns devices and
 * functions to root ports.
 */
static const milan_bridge_info_t milan_pcie0[MILAN_IOMS_MAX_PCIE_BRIDGES] = {
	{ 0x1, 0x1 },
	{ 0x1, 0x2 },
	{ 0x1, 0x3 },
	{ 0x1, 0x4 },
	{ 0x1, 0x5 },
	{ 0x1, 0x6 },
	{ 0x1, 0x7 },
	{ 0x2, 0x1 }
};

static const milan_bridge_info_t milan_pcie1[MILAN_IOMS_MAX_PCIE_BRIDGES] = {
	{ 0x3, 0x1 },
	{ 0x3, 0x2 },
	{ 0x3, 0x3 },
	{ 0x3, 0x4 },
	{ 0x3, 0x5 },
	{ 0x3, 0x6 },
	{ 0x3, 0x7 },
	{ 0x4, 0x1 }
};

static const milan_bridge_info_t milan_pcie2[MILAN_IOMS_WAFL_PCIE_NBRIDGES] = {
	{ 0x5, 0x1 },
	{ 0x5, 0x2 }
};

/*
 * These are internal bridges that correspond to NBIFs.
 */
static const milan_bridge_info_t milan_int_bridges[4] = {
	{ 0x7, 0x1 },
	{ 0x8, 0x1 },
	{ 0x8, 0x2 },
	{ 0x8, 0x3 }
};

/*
 * The following table encodes the per-bridge IOAPIC initialization routing. We
 * currently following the recommendation of the PPR.
 */
typedef struct milan_ioapic_info {
	uint8_t mii_group;
	uint8_t mii_swiz;
	uint8_t mii_map;
} milan_ioapic_info_t;

static const milan_ioapic_info_t milan_ioapic_routes[IOAPIC_NROUTES] = {
	{ .mii_group = 0x0, .mii_map = 0x10,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x1, .mii_map = 0x11,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x2, .mii_map = 0x12,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x3, .mii_map = 0x13,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x4, .mii_map = 0x10,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x5, .mii_map = 0x11,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x6, .mii_map = 0x12,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x7, .mii_map = 0x13,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_ABCD },
	{ .mii_group = 0x7, .mii_map = 0x0c,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x6, .mii_map = 0x0d,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x5, .mii_map = 0x0e,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x4, .mii_map = 0x0f,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x3, .mii_map = 0x0c,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x2, .mii_map = 0x0d,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x1, .mii_map = 0x0e,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x0, .mii_map = 0x0f,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_CDAB },
	{ .mii_group = 0x0, .mii_map = 0x08,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC },
	{ .mii_group = 0x1, .mii_map = 0x09,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC },
	{ .mii_group = 0x2, .mii_map = 0x0a,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC },
	{ .mii_group = 0x3, .mii_map = 0x0b,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC },
	{ .mii_group = 0x4, .mii_map = 0x08,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC },
	{ .mii_group = 0x5, .mii_map = 0x09,
	    .mii_swiz = IOAPIC_ROUTE_INTX_SWIZZLE_DABC }
};

/* XXX Track platform default presence */
typedef struct milan_nbif_info {
	milan_nbif_func_type_t	mni_type;
	uint8_t			mni_dev;
	uint8_t			mni_func;
} milan_nbif_info_t;

static const milan_nbif_info_t milan_nbif0[MILAN_NBIF0_NFUNCS] = {
	{ .mni_type = MILAN_NBIF_T_DUMMY, .mni_dev = 0, .mni_func = 0 },
	{ .mni_type = MILAN_NBIF_T_NTB, .mni_dev = 0, .mni_func = 1 },
	{ .mni_type = MILAN_NBIF_T_PTDMA, .mni_dev = 0, .mni_func = 2 }
};

static const milan_nbif_info_t milan_nbif1[MILAN_NBIF1_NFUNCS] = {
	{ .mni_type = MILAN_NBIF_T_DUMMY, .mni_dev = 0, .mni_func = 0 },
	{ .mni_type = MILAN_NBIF_T_PSPCCP, .mni_dev = 0, .mni_func = 1 },
	{ .mni_type = MILAN_NBIF_T_PTDMA, .mni_dev = 0, .mni_func = 2 },
	{ .mni_type = MILAN_NBIF_T_USB, .mni_dev = 0, .mni_func = 3 },
	{ .mni_type = MILAN_NBIF_T_AZ, .mni_dev = 0, .mni_func = 4 },
	{ .mni_type = MILAN_NBIF_T_SATA, .mni_dev = 1, .mni_func = 0 },
	{ .mni_type = MILAN_NBIF_T_SATA, .mni_dev = 2, .mni_func = 0 }
};

static const milan_nbif_info_t milan_nbif2[MILAN_NBIF2_NFUNCS] = {
	{ .mni_type = MILAN_NBIF_T_DUMMY, .mni_dev = 0, .mni_func = 0 },
	{ .mni_type = MILAN_NBIF_T_NTB, .mni_dev = 0, .mni_func = 1 },
	{ .mni_type = MILAN_NBIF_T_NVME, .mni_dev = 0, .mni_func = 2 }
};

/*
 * This structure and the following table encodes the mapping of the set of dxio
 * lanes to a give PCIe port on an IOMS. This is ordered such that all of the
 * normal engines are present; however, the wafl port, being special is not
 * here. The dxio engine uses different lane numbers than the phys. Note, that
 * all lanes here are inclusive. e.g. [start, end].
 */
typedef struct milan_pcie_port_info {
	const char	*mppi_name;
	uint16_t	mppi_dxio_start;
	uint16_t	mppi_dxio_end;
	uint16_t	mppi_phy_start;
	uint16_t	mppi_phy_end;
} milan_pcie_port_info_t;

static const milan_pcie_port_info_t milan_lane_maps[8] = {
	{ "G0", 0x10, 0x1f, 0x10, 0x1f },
	{ "P0", 0x2a, 0x39, 0x00, 0x0f },
	{ "P1", 0x3a, 0x49, 0x20, 0x2f },
	{ "G1", 0x00, 0x0f, 0x30, 0x3f },
	{ "G3", 0x72, 0x81, 0x60, 0x6f },
	{ "P3", 0x5a, 0x69, 0x70, 0x7f },
	{ "P2", 0x4a, 0x59, 0x50, 0x5f },
	{ "G2", 0x82, 0x91, 0x40, 0x4f }
};

static const milan_pcie_port_info_t milan_wafl_map = {
	"WAFL", 0x24, 0x25, 0x80, 0x81
};

/*
 * How many PCIe ports (root complexes) does this NBIO instance have?
 */
uint8_t
milan_nbio_n_pcie_ports(const uint8_t nbno)
{
	if (nbno == MILAN_IOMS_HAS_WAFL)
		return (MILAN_IOMS_MAX_PCIE_PORTS);
	return (MILAN_IOMS_MAX_PCIE_PORTS - 1);
}

/*
 * How many PCIe bridges does this port (root complex) instance have?  Not all
 * bridges are necessarily enabled; this is used to compute the locations of
 * register blocks that pertain to the bridge that may exist.
 */
uint8_t
milan_pcie_port_n_bridges(const uint8_t portno)
{
	if (portno == MILAN_IOMS_WAFL_PCIE_PORT)
		return (MILAN_IOMS_WAFL_PCIE_NBRIDGES);
	return (MILAN_IOMS_MAX_PCIE_BRIDGES);
}

typedef enum milan_iommul1_subunit {
	MIL1SU_NBIF,
	MIL1SU_IOAGR
} milan_iommul1_subunit_t;

/*
 * XXX Belongs in a header.
 */
extern void *contig_alloc(size_t, ddi_dma_attr_t *, uintptr_t, int);
extern void contig_free(void *, size_t);

static boolean_t milan_smu_rpc_read_brand_string(milan_iodie_t *,
    char *, size_t);

/*
 * Our primary global data. This is the reason that we exist.
 */
static milan_fabric_t milan_fabric;
static uint_t nthreads;

/*
 * Variable to let us dump all SMN traffic while still developing.
 */
int milan_smn_log = 0;

static int
milan_fabric_walk_iodie(milan_fabric_t *fabric, milan_iodie_cb_f func,
    void *arg)
{
	for (uint_t socno = 0; socno < fabric->mf_nsocs; socno++) {
		milan_soc_t *soc = &fabric->mf_socs[socno];
		for (uint_t iono = 0; iono < soc->ms_ndies; iono++) {
			int ret;
			milan_iodie_t *iodie = &soc->ms_iodies[iono];

			ret = func(iodie, arg);
			if (ret != 0) {
				return (ret);
			}
		}
	}

	return (0);
}

int
milan_walk_iodie(milan_iodie_cb_f func, void *arg)
{
	return (milan_fabric_walk_iodie(&milan_fabric, func, arg));
}

typedef struct milan_fabric_ioms_cb {
	milan_ioms_cb_f	mfic_func;
	void		*mfic_arg;
} milan_fabric_ioms_cb_t;

static int
milan_fabric_walk_ioms_iodie_cb(milan_iodie_t *iodie, void *arg)
{
	milan_fabric_ioms_cb_t *cb = arg;

	for (uint_t iomsno = 0; iomsno < iodie->mi_nioms; iomsno++) {
		int ret;
		milan_ioms_t *ioms = &iodie->mi_ioms[iomsno];

		ret = cb->mfic_func(ioms, cb->mfic_arg);
		if (ret != 0) {
			return (ret);
		}
	}

	return (0);
}

static int
milan_fabric_walk_ioms(milan_fabric_t *fabric, milan_ioms_cb_f func, void *arg)
{
	milan_fabric_ioms_cb_t cb;

	cb.mfic_func = func;
	cb.mfic_arg = arg;
	return (milan_fabric_walk_iodie(fabric, milan_fabric_walk_ioms_iodie_cb,
	    &cb));
}

int
milan_walk_ioms(milan_ioms_cb_f func, void *arg)
{
	return (milan_fabric_walk_ioms(&milan_fabric, func, arg));
}

typedef struct milan_fabric_nbif_cb {
	milan_nbif_cb_f	mfnc_func;
	void		*mfnc_arg;
} milan_fabric_nbif_cb_t;

static int
milan_fabric_walk_nbif_ioms_cb(milan_ioms_t *ioms, void *arg)
{
	milan_fabric_nbif_cb_t *cb = arg;

	for (uint_t nbifno = 0; nbifno < ioms->mio_nnbifs; nbifno++) {
		int ret;
		milan_nbif_t *nbif = &ioms->mio_nbifs[nbifno];
		ret = cb->mfnc_func(nbif, cb->mfnc_arg);
		if (ret != 0) {
			return (ret);
		}
	}

	return (0);
}

static int
milan_fabric_walk_nbif(milan_fabric_t *fabric, milan_nbif_cb_f func, void *arg)
{
	milan_fabric_nbif_cb_t cb;

	cb.mfnc_func = func;
	cb.mfnc_arg = arg;
	return (milan_fabric_walk_ioms(fabric, milan_fabric_walk_nbif_ioms_cb,
	    &cb));
}

typedef struct milan_fabric_pcie_port_cb {
	milan_pcie_port_cb_f	mfppc_func;
	void			*mfppc_arg;
} milan_fabric_pcie_port_cb_t;

static int
milan_fabric_walk_pcie_port_cb(milan_ioms_t *ioms, void *arg)
{
	milan_fabric_pcie_port_cb_t *cb = arg;

	for (uint_t portno = 0; portno < ioms->mio_npcie_ports; portno++) {
		int ret;
		milan_pcie_port_t *port = &ioms->mio_pcie_ports[portno];

		ret = cb->mfppc_func(port, cb->mfppc_arg);
		if (ret != 0) {
			return (ret);
		}
	}

	return (0);
}

static int
milan_fabric_walk_pcie_port(milan_fabric_t *fabric, milan_pcie_port_cb_f func,
    void *arg)
{
	milan_fabric_pcie_port_cb_t cb;

	cb.mfppc_func = func;
	cb.mfppc_arg = arg;
	return (milan_fabric_walk_ioms(fabric, milan_fabric_walk_pcie_port_cb,
	    &cb));
}

typedef struct milan_fabric_bridge_cb {
	milan_bridge_cb_f	mfbc_func;
	void			*mfbc_arg;
} milan_fabric_bridge_cb_t;

static int
milan_fabric_walk_bridge_cb(milan_pcie_port_t *port, void *arg)
{
	milan_fabric_bridge_cb_t *cb = arg;

	for (uint_t bridgeno = 0; bridgeno < port->mpp_nbridges;
	    bridgeno++) {
		int ret;
		milan_pcie_bridge_t *bridge = &port->mpp_bridges[bridgeno];

		ret = cb->mfbc_func(bridge, cb->mfbc_arg);
		if (ret != 0) {
			return (ret);
		}
	}

	return (0);
}

static int
milan_fabric_walk_bridge(milan_fabric_t *fabric, milan_bridge_cb_f func,
    void *arg)
{
	milan_fabric_bridge_cb_t cb;

	cb.mfbc_func = func;
	cb.mfbc_arg = arg;
	return (milan_fabric_walk_pcie_port(fabric, milan_fabric_walk_bridge_cb,
	    &cb));
}

typedef struct milan_fabric_ccd_cb {
	milan_ccd_cb_f	mfcc_func;
	void		*mfcc_arg;
} milan_fabric_ccd_cb_t;

static int
milan_fabric_walk_ccd_iodie_cb(milan_iodie_t *iodie, void *arg)
{
	milan_fabric_ccd_cb_t *cb = arg;

	for (uint8_t ccdno = 0; ccdno < iodie->mi_nccds; ccdno++) {
		int ret;
		milan_ccd_t *ccd = &iodie->mi_ccds[ccdno];

		if ((ret = cb->mfcc_func(ccd, cb->mfcc_arg)) != 0)
			return (ret);
	}

	return (0);
}

static int
milan_fabric_walk_ccd(milan_fabric_t *fabric, milan_ccd_cb_f func, void *arg)
{
	milan_fabric_ccd_cb_t cb;

	cb.mfcc_func = func;
	cb.mfcc_arg = arg;
	return (milan_fabric_walk_iodie(fabric,
	    milan_fabric_walk_ccd_iodie_cb, &cb));
}

typedef struct milan_fabric_ccx_cb {
	milan_ccx_cb_f	mfcc_func;
	void		*mfcc_arg;
} milan_fabric_ccx_cb_t;

static int
milan_fabric_walk_ccx_ccd_cb(milan_ccd_t *ccd, void *arg)
{
	milan_fabric_ccx_cb_t *cb = arg;

	for (uint8_t ccxno = 0; ccxno < ccd->mcd_nccxs; ccxno++) {
		int ret;
		milan_ccx_t *ccx = &ccd->mcd_ccxs[ccxno];

		if ((ret = cb->mfcc_func(ccx, cb->mfcc_arg)) != 0)
			return (ret);
	}

	return (0);
}

static int
milan_fabric_walk_ccx(milan_fabric_t *fabric, milan_ccx_cb_f func, void *arg)
{
	milan_fabric_ccx_cb_t cb;

	cb.mfcc_func = func;
	cb.mfcc_arg = arg;
	return (milan_fabric_walk_ccd(fabric,
	    milan_fabric_walk_ccx_ccd_cb, &cb));
}

typedef struct milan_fabric_core_cb {
	milan_core_cb_f	mfcc_func;
	void		*mfcc_arg;
} milan_fabric_core_cb_t;

static int
milan_fabric_walk_core_ccx_cb(milan_ccx_t *ccx, void *arg)
{
	milan_fabric_core_cb_t *cb = arg;

	for (uint8_t coreno = 0; coreno < ccx->mcx_ncores; coreno++) {
		int ret;
		milan_core_t *core = &ccx->mcx_cores[coreno];

		if ((ret = cb->mfcc_func(core, cb->mfcc_arg)) != 0)
			return (ret);
	}

	return (0);
}

static int
milan_fabric_walk_core(milan_fabric_t *fabric, milan_core_cb_f func, void *arg)
{
	milan_fabric_core_cb_t cb;

	cb.mfcc_func = func;
	cb.mfcc_arg = arg;
	return (milan_fabric_walk_ccx(fabric,
	    milan_fabric_walk_core_ccx_cb, &cb));
}

typedef struct milan_fabric_thread_cb {
	milan_thread_cb_f	mftc_func;
	void			*mftc_arg;
} milan_fabric_thread_cb_t;

static int
milan_fabric_walk_thread_core_cb(milan_core_t *core, void *arg)
{
	milan_fabric_thread_cb_t *cb = arg;

	for (uint8_t threadno = 0; threadno < core->mc_nthreads; threadno++) {
		int ret;
		milan_thread_t *thread = &core->mc_threads[threadno];

		if ((ret = cb->mftc_func(thread, cb->mftc_arg)) != 0)
			return (ret);
	}

	return (0);
}

static int
milan_fabric_walk_thread(milan_fabric_t *fabric,
    milan_thread_cb_f func, void *arg)
{
	milan_fabric_thread_cb_t cb;

	cb.mftc_func = func;
	cb.mftc_arg = arg;
	return (milan_fabric_walk_core(fabric,
	    milan_fabric_walk_thread_core_cb, &cb));
}

int
milan_walk_thread(milan_thread_cb_f func, void *arg)
{
	return (milan_fabric_walk_thread(&milan_fabric, func, arg));
}

typedef struct {
	uint32_t	mffi_dest;
	milan_ioms_t	*mffi_ioms;
} milan_fabric_find_ioms_t;

static int
milan_fabric_find_ioms_cb(milan_ioms_t *ioms, void *arg)
{
	milan_fabric_find_ioms_t *mffi = arg;

	if (mffi->mffi_dest == ioms->mio_fabric_id) {
		mffi->mffi_ioms = ioms;
	}

	return (0);
}

static int
milan_fabric_find_ioms_by_bus_cb(milan_ioms_t *ioms, void *arg)
{
	milan_fabric_find_ioms_t *mffi = arg;

	if (mffi->mffi_dest == ioms->mio_pci_busno) {
		mffi->mffi_ioms = ioms;
	}

	return (0);
}

static milan_ioms_t *
milan_fabric_find_ioms(milan_fabric_t *fabric, uint32_t destid)
{
	milan_fabric_find_ioms_t mffi;

	mffi.mffi_dest = destid;
	mffi.mffi_ioms = NULL;

	milan_fabric_walk_ioms(fabric, milan_fabric_find_ioms_cb, &mffi);

	return (mffi.mffi_ioms);
}

static milan_ioms_t *
milan_fabric_find_ioms_by_bus(milan_fabric_t *fabric, uint32_t pci_bus)
{
	milan_fabric_find_ioms_t mffi;

	mffi.mffi_dest = pci_bus;
	mffi.mffi_ioms = NULL;

	milan_fabric_walk_ioms(fabric, milan_fabric_find_ioms_by_bus_cb, &mffi);

	return (mffi.mffi_ioms);
}

typedef struct {
	const milan_iodie_t *mffp_iodie;
	uint16_t mffp_start;
	uint16_t mffp_end;
	milan_pcie_port_t *mffp_port;
} milan_fabric_find_port_t;

static int
milan_fabric_find_port_by_lanes_cb(milan_pcie_port_t *port, void *arg)
{
	milan_fabric_find_port_t *mffp = arg;

	if (mffp->mffp_iodie != port->mpp_ioms->mio_iodie) {
		return (0);
	}

	if (mffp->mffp_start >= port->mpp_dxio_lane_start &&
	    mffp->mffp_start <= port->mpp_dxio_lane_end &&
	    mffp->mffp_end >= port->mpp_dxio_lane_start &&
	    mffp->mffp_end <= port->mpp_dxio_lane_end) {
		mffp->mffp_port = port;
		return (1);
	}

	return (0);
}


static milan_pcie_port_t *
milan_fabric_find_port_by_lanes(milan_iodie_t *iodie,
    uint16_t start, uint16_t end)
{
	milan_fabric_find_port_t mffp;

	mffp.mffp_iodie = iodie;
	mffp.mffp_start = start;
	mffp.mffp_end = end;
	mffp.mffp_port = NULL;
	ASSERT3U(start, <=, end);

	(void) milan_fabric_walk_pcie_port(iodie->mi_soc->ms_fabric,
	    milan_fabric_find_port_by_lanes_cb, &mffp);

	return (mffp.mffp_port);
}

typedef struct milan_fabric_find_thread {
	uint32_t	mfft_search;
	uint32_t	mfft_count;
	milan_thread_t	*mfft_found;
} milan_fabric_find_thread_t;

static int
milan_fabric_find_thread_by_cpuid_cb(milan_thread_t *thread, void *arg)
{
	milan_fabric_find_thread_t *mfft = arg;
	if (mfft->mfft_count == mfft->mfft_search) {
		mfft->mfft_found = thread;
		return (1);
	}
	++mfft->mfft_count;
	return (0);
}

milan_thread_t *
milan_fabric_find_thread_by_cpuid(uint32_t cpuid)
{
	milan_fabric_find_thread_t mfft;

	mfft.mfft_search = cpuid;
	mfft.mfft_count = 0;
	mfft.mfft_found = NULL;
	(void) milan_fabric_walk_thread(&milan_fabric,
	    milan_fabric_find_thread_by_cpuid_cb, &mfft);

	return (mfft.mfft_found);
}

/*
 * buf, len, and return value semantics match those of snprintf(9f).
 */
size_t
milan_fabric_thread_get_brandstr(const milan_thread_t *thread,
    char *buf, size_t len)
{
	milan_soc_t *soc = thread->mt_core->mc_ccx->mcx_ccd->mcd_iodie->mi_soc;
	return (snprintf(buf, len, "%s", soc->ms_brandstr));
}

uint64_t
milan_fabric_ecam_base(void)
{
	uint64_t ecam = milan_fabric.mf_ecam_base;

	ASSERT3U(ecam, !=, 0);

	return (ecam);
}

static uint32_t
milan_df_read32(milan_iodie_t *iodie, uint8_t inst, const df_reg_def_t def)
{
	uint32_t val = 0;
	const df_reg_def_t ficaa = DF_FICAA_V2;
	const df_reg_def_t ficad = DF_FICAD_LO_V2;

	mutex_enter(&iodie->mi_df_ficaa_lock);
	ASSERT3U(def.drd_gens & DF_REV_3, ==, DF_REV_3);
	val = DF_FICAA_V2_SET_TARG_INST(val, 1);
	val = DF_FICAA_V2_SET_FUNC(val, def.drd_func);
	val = DF_FICAA_V2_SET_INST(val, inst);
	val = DF_FICAA_V2_SET_64B(val, 0);
	val = DF_FICAA_V2_SET_REG(val, def.drd_reg >> 2);

	ASSERT0(ficaa.drd_reg & 3);
	pci_putl_func(0, iodie->mi_dfno, ficaa.drd_func, ficaa.drd_reg, val);
	val = pci_getl_func(0, iodie->mi_dfno, ficad.drd_func, ficad.drd_reg);
	mutex_exit(&iodie->mi_df_ficaa_lock);

	return (val);
}

/*
 * A broadcast read is allowed to use PCIe configuration space directly to read
 * the register. Because we are not using the indirect registers, there is no
 * locking being used as the purpose of mi_df_ficaa_lock is just to ensure
 * there's only one use of it at any given time.
 */
static uint32_t
milan_df_bcast_read32(milan_iodie_t *iodie, const df_reg_def_t def)
{
	ASSERT0(def.drd_reg & 3);
	return (pci_getl_func(0, iodie->mi_dfno, def.drd_func, def.drd_reg));
}

static void
milan_df_bcast_write32(milan_iodie_t *iodie, const df_reg_def_t def,
    uint32_t val)
{
	ASSERT0(def.drd_reg & 3);
	pci_putl_func(0, iodie->mi_dfno, def.drd_func, def.drd_reg, val);
}

/*
 * This is used early in boot when we're trying to bootstrap the system so we
 * can construct our fabric data structure. This always reads against the first
 * data fabric instance which is required to be present.
 */
static uint32_t
milan_df_early_read32(const df_reg_def_t def)
{
	ASSERT0(def.drd_reg & 3);
	return (pci_getl_func(AMDZEN_DF_BUSNO, AMDZEN_DF_FIRST_DEVICE,
	    def.drd_func, def.drd_reg));
}

uint32_t
milan_smn_read(milan_iodie_t *iodie, const smn_reg_t reg)
{
	const uint32_t addr = SMN_REG_ADDR(reg);
	const uint32_t base_addr = SMN_REG_ADDR_BASE(reg);
	const uint32_t addr_off = SMN_REG_ADDR_OFF(reg);
	uint32_t val;

	ASSERT(SMN_REG_IS_NATURALLY_ALIGNED(reg));
	ASSERT(SMN_REG_SIZE_IS_VALID(reg));

	mutex_enter(&iodie->mi_smn_lock);
	pci_putl_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_ADDR, base_addr);
	switch (SMN_REG_SIZE(reg)) {
	case 1:
		val = (uint32_t)pci_getb_func(iodie->mi_smn_busno,
		    AMDZEN_NB_SMN_DEVNO, AMDZEN_NB_SMN_FUNCNO,
		    AMDZEN_NB_SMN_DATA + addr_off);
		break;
	case 2:
		val = (uint32_t)pci_getw_func(iodie->mi_smn_busno,
		    AMDZEN_NB_SMN_DEVNO, AMDZEN_NB_SMN_FUNCNO,
		    AMDZEN_NB_SMN_DATA + addr_off);
		break;
	case 4:
		val = pci_getl_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA);
		break;
	default:
		panic("unreachable invalid SMN register size %u",
		    SMN_REG_SIZE(reg));
	}
	if (milan_smn_log != 0) {
		cmn_err(CE_NOTE, "SMN R reg 0x%x: 0x%x", addr, val);
	}
	mutex_exit(&iodie->mi_smn_lock);

	return (val);
}

void
milan_smn_write(milan_iodie_t *iodie, const smn_reg_t reg, const uint32_t val)
{
	const uint32_t addr = SMN_REG_ADDR(reg);
	const uint32_t base_addr = SMN_REG_ADDR_BASE(reg);
	const uint32_t addr_off = SMN_REG_ADDR_OFF(reg);

	ASSERT(SMN_REG_IS_NATURALLY_ALIGNED(reg));
	ASSERT(SMN_REG_SIZE_IS_VALID(reg));
	ASSERT(SMN_REG_VALUE_FITS(reg, val));

	mutex_enter(&iodie->mi_smn_lock);
	if (milan_smn_log != 0) {
		cmn_err(CE_NOTE, "SMN W reg 0x%x: 0x%x", addr, val);
	}
	pci_putl_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
	    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_ADDR, base_addr);
	switch (SMN_REG_SIZE(reg)) {
	case 1:
		pci_putb_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA + addr_off,
		    (uint8_t)val);
		break;
	case 2:
		pci_putw_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA + addr_off,
		    (uint16_t)val);
		break;
	case 4:
		pci_putl_func(iodie->mi_smn_busno, AMDZEN_NB_SMN_DEVNO,
		    AMDZEN_NB_SMN_FUNCNO, AMDZEN_NB_SMN_DATA, val);
		break;
	default:
		panic("unreachable invalid SMN register size %u",
		    SMN_REG_SIZE(reg));
	}

	mutex_exit(&iodie->mi_smn_lock);
}

/*
 * Convenience functions for accessing SMN registers pertaining to a bridge.
 * These are candidates for making public if/when other code needs to manipulate
 * bridges.  There are some tradeoffs here: we don't need any of these
 * functions; callers could instead look up registers themselves, retrieve the
 * iodie by chasing back-pointers, and call milan_smn_{read,write}32()
 * themselves.  Indeed, they still can, and if there are many register accesses
 * to be made in code that materially affects performance, that is likely to be
 * preferable.  However, it has a major drawback: it requires each caller to get
 * the ordered set of instance numbers correct when constructing the register,
 * and there is little or nothing that can be done to help them.  Most of the
 * register accessors will blow up if the instance numbers are obviously out of
 * range, but there is little we can do to prevent them being given out of
 * order, for example.  Constructing incompatible struct types for each instance
 * level seems impractical.  So instead we isolate those calculations here and
 * allow callers to treat each bridge's (or other object's) collections of
 * pertinent registers opaquely.  This is probably closest to what we
 * conceptually want this to look like anyway; callers should be focused on
 * controlling the device, not on the mechanics of how to do so.  Nevertheless,
 * we do not foreclose on arbitrary SMN access if that's useful.
 *
 * We provide similar collections of functions below for other entities we
 * model in the fabric.
 */

static smn_reg_t
milan_pcie_bridge_reg(const milan_pcie_bridge_t *const bridge,
    const smn_reg_def_t def)
{
	milan_pcie_port_t *port = bridge->mpb_port;
	milan_ioms_t *ioms = port->mpp_ioms;
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_IOHCDEV_PCIE:
		reg = milan_iohcdev_pcie_smn_reg(ioms->mio_num, def,
		    port->mpp_portno, bridge->mpb_bridgeno);
		break;
	case SMN_UNIT_PCIE_PORT:
		reg = milan_pcie_port_smn_reg(ioms->mio_num, def,
		    port->mpp_portno, bridge->mpb_bridgeno);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for PCIe "
		    "bridge", def.srd_unit);
	}

	return (reg);
}

static uint32_t
milan_pcie_bridge_read(milan_pcie_bridge_t *bridge, const smn_reg_t reg)
{
	milan_iodie_t *iodie = bridge->mpb_port->mpp_ioms->mio_iodie;

	return (milan_smn_read(iodie, reg));
}

static void
milan_pcie_bridge_write(milan_pcie_bridge_t *bridge, const smn_reg_t reg,
    const uint32_t val)
{
	milan_iodie_t *iodie = bridge->mpb_port->mpp_ioms->mio_iodie;

	milan_smn_write(iodie, reg, val);
}

static smn_reg_t
milan_pcie_port_reg(const milan_pcie_port_t *const port,
    const smn_reg_def_t def)
{
	milan_ioms_t *ioms = port->mpp_ioms;
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_PCIE_CORE:
		reg = milan_pcie_core_smn_reg(ioms->mio_num, def,
		    port->mpp_portno);
		break;
	case SMN_UNIT_PCIE_RSMU:
		reg = milan_pcie_rsmu_smn_reg(ioms->mio_num, def,
		    port->mpp_portno);
		break;
	case SMN_UNIT_IOMMUL1:
		reg = milan_iommul1_pcie_smn_reg(ioms->mio_num, def,
		    port->mpp_portno);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for PCIe port",
		    def.srd_unit);
	}

	return (reg);
}

static uint32_t
milan_pcie_port_read(milan_pcie_port_t *port, const smn_reg_t reg)
{
	milan_iodie_t *iodie = port->mpp_ioms->mio_iodie;

	return (milan_smn_read(iodie, reg));
}

static void
milan_pcie_port_write(milan_pcie_port_t *port, const smn_reg_t reg,
    const uint32_t val)
{
	milan_iodie_t *iodie = port->mpp_ioms->mio_iodie;

	milan_smn_write(iodie, reg, val);
}

/*
 * We consider the IOAGR to be part of the NBIO/IOHC/IOMS, so the IOMMUL1's
 * IOAGR block falls under the IOMS; the IOAPIC, SDPMUX, and IOMMUL2 are similar
 * as they do not (currently) have independent representation in the fabric.
 */

smn_reg_t
milan_ioms_reg(const milan_ioms_t *const ioms, const smn_reg_def_t def,
    const uint16_t reginst)
{
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_IOAPIC:
		reg = milan_ioapic_smn_reg(ioms->mio_num, def, reginst);
		break;
	case SMN_UNIT_IOHC:
		reg = milan_iohc_smn_reg(ioms->mio_num, def, reginst);
		break;
	case SMN_UNIT_IOAGR:
		reg = milan_ioagr_smn_reg(ioms->mio_num, def, reginst);
		break;
	case SMN_UNIT_SDPMUX:
		reg = milan_sdpmux_smn_reg(ioms->mio_num, def, reginst);
		break;
	case SMN_UNIT_IOMMUL1: {
		/*
		 * Confusingly, this pertains to the IOMS, not the NBIF; there
		 * is only one unit per IOMS, not one per NBIF.  Because.  To
		 * accommodate this, we need to treat the reginst as an
		 * enumerated type to distinguish the sub-units.  As gross as
		 * this is, it greatly reduces triplication of register
		 * definitions.  There is no way to win here.
		 */
		const milan_iommul1_subunit_t su =
		    (const milan_iommul1_subunit_t)reginst;
		switch (su) {
		case MIL1SU_NBIF:
			reg = milan_iommul1_nbif_smn_reg(ioms->mio_num, def, 0);
			break;
		case MIL1SU_IOAGR:
			reg = milan_iommul1_ioagr_smn_reg(ioms->mio_num,
			    def, 0);
			break;
		default:
			cmn_err(CE_PANIC, "invalid IOMMUL1 subunit %d", su);
			break;
		}
		break;
	}
	case SMN_UNIT_IOMMUL2:
		reg = milan_iommul2_smn_reg(ioms->mio_num, def, reginst);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for IOMS",
		    def.srd_unit);
	}

	return (reg);
}

uint32_t
milan_ioms_read(milan_ioms_t *ioms, const smn_reg_t reg)
{
	milan_iodie_t *iodie = ioms->mio_iodie;

	return (milan_smn_read(iodie, reg));
}

void
milan_ioms_write(milan_ioms_t *ioms, const smn_reg_t reg, const uint32_t val)
{
	milan_iodie_t *iodie = ioms->mio_iodie;

	milan_smn_write(iodie, reg, val);
}

static smn_reg_t
milan_nbif_reg(const milan_nbif_t *const nbif, const smn_reg_def_t def,
    const uint16_t reginst)
{
	milan_ioms_t *ioms = nbif->mn_ioms;
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_NBIF:
		reg = milan_nbif_smn_reg(ioms->mio_num, def, nbif->mn_nbifno,
		    reginst);
		break;
	case SMN_UNIT_NBIF_ALT:
		reg = milan_nbif_alt_smn_reg(ioms->mio_num, def,
		    nbif->mn_nbifno, reginst);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for NBIF",
		    def.srd_unit);
	}

	return (reg);
}

static uint32_t
milan_nbif_read(milan_nbif_t *nbif, const smn_reg_t reg)
{
	return (milan_smn_read(nbif->mn_ioms->mio_iodie, reg));
}

static void
milan_nbif_write(milan_nbif_t *nbif, const smn_reg_t reg, const uint32_t val)
{
	milan_smn_write(nbif->mn_ioms->mio_iodie, reg, val);
}

static smn_reg_t
milan_nbif_func_reg(const milan_nbif_func_t *const func,
    const smn_reg_def_t def)
{
	milan_nbif_t *nbif = func->mne_nbif;
	milan_ioms_t *ioms = nbif->mn_ioms;
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_NBIF_FUNC:
		reg = milan_nbif_func_smn_reg(ioms->mio_num, def,
		    nbif->mn_nbifno, func->mne_dev, func->mne_func);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for NBIF func",
		    def.srd_unit);
	}

	return (reg);
}

static uint32_t
milan_nbif_func_read(milan_nbif_func_t *func, const smn_reg_t reg)
{
	return (milan_smn_read(func->mne_nbif->mn_ioms->mio_iodie, reg));
}

static void
milan_nbif_func_write(milan_nbif_func_t *func, const smn_reg_t reg,
    const uint32_t val)
{
	milan_smn_write(func->mne_nbif->mn_ioms->mio_iodie, reg, val);
}

smn_reg_t
milan_iodie_reg(const milan_iodie_t *const iodie, const smn_reg_def_t def,
    const uint16_t reginst)
{
	smn_reg_t reg;

	switch (def.srd_unit) {
	case SMN_UNIT_SMU_RPC:
		reg = milan_smu_smn_reg(0, def, reginst);
		break;
	case SMN_UNIT_FCH_SMI:
		reg = fch_smi_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_PMIO:
		reg = fch_pmio_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_MISC_A:
		reg = fch_misc_a_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_I2CPAD:
		reg = fch_i2cpad_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_MISC_B:
		reg = fch_misc_b_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_I2C:
		reg = huashan_i2c_smn_reg(reginst, def);
		break;
	case SMN_UNIT_FCH_IOMUX:
		reg = fch_iomux_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_GPIO:
		reg = fch_gpio_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_RMTGPIO:
		reg = fch_rmtgpio_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_RMTMUX:
		reg = fch_rmtmux_smn_reg(def, reginst);
		break;
	case SMN_UNIT_FCH_RMTGPIO_AGG:
		reg = fch_rmtgpio_agg_smn_reg(def, reginst);
		break;
	default:
		cmn_err(CE_PANIC, "invalid SMN register type %d for IO die",
		    def.srd_unit);
	}

	return (reg);
}

uint32_t
milan_iodie_read(milan_iodie_t *iodie, const smn_reg_t reg)
{
	return (milan_smn_read(iodie, reg));
}

void
milan_iodie_write(milan_iodie_t *iodie, const smn_reg_t reg, const uint32_t val)
{
	milan_smn_write(iodie, reg, val);
}

uint8_t
milan_iodie_node_id(const milan_iodie_t *const iodie)
{
	return (iodie->mi_node_id);
}

milan_iodie_flag_t
milan_iodie_flags(const milan_iodie_t *const iodie)
{
	return (iodie->mi_flags);
}

milan_ioms_flag_t
milan_ioms_flags(const milan_ioms_t *const ioms)
{
	return (ioms->mio_flags);
}

milan_iodie_t *
milan_ioms_iodie(const milan_ioms_t *const ioms)
{
	return (ioms->mio_iodie);
}

typedef enum {
	MBT_GIMLET,
	MBT_ETHANOL
} milan_board_type_t;

/*
 * Here is a temporary rough heuristic for determining what board we're on.
 */
static milan_board_type_t
milan_board_type(const milan_fabric_t *fabric)
{
	if (fabric->mf_nsocs == 2) {
		return (MBT_ETHANOL);
	} else {
		return (MBT_GIMLET);
	}
}

static void
milan_fabric_ioms_pcie_init(milan_ioms_t *ioms)
{
	for (uint_t pcino = 0; pcino < ioms->mio_npcie_ports; pcino++) {
		milan_pcie_port_t *port = &ioms->mio_pcie_ports[pcino];
		const milan_bridge_info_t *binfop = NULL;
		const milan_pcie_port_info_t *info;

		port->mpp_portno = pcino;
		port->mpp_ioms = ioms;
		port->mpp_nbridges = milan_pcie_port_n_bridges(pcino);
		mutex_init(&port->mpp_strap_lock, NULL, MUTEX_SPIN,
		    (ddi_iblock_cookie_t)ipltospl(15));

		VERIFY3U(pcino, <=, MILAN_IOMS_WAFL_PCIE_PORT);
		switch (pcino) {
		case 0:
			/* XXX Macros */
			port->mpp_sdp_unit = 2;
			port->mpp_sdp_port = 0;
			binfop = milan_pcie0;
			break;
		case 1:
			port->mpp_sdp_unit = 3;
			port->mpp_sdp_port = 0;
			binfop = milan_pcie1;
			break;
		case MILAN_IOMS_WAFL_PCIE_PORT:
			port->mpp_sdp_unit = 4;
			port->mpp_sdp_port = 5;
			binfop = milan_pcie2;
			break;
		}

		if (pcino == MILAN_IOMS_WAFL_PCIE_PORT) {
			info = &milan_wafl_map;
		} else {
			info = &milan_lane_maps[ioms->mio_num * 2 + pcino];
		}

		port->mpp_dxio_lane_start = info->mppi_dxio_start;
		port->mpp_dxio_lane_end = info->mppi_dxio_end;
		port->mpp_phys_lane_start = info->mppi_phy_start;
		port->mpp_phys_lane_end = info->mppi_phy_end;

		for (uint_t bridgeno = 0; bridgeno < port->mpp_nbridges;
		    bridgeno++) {
			milan_pcie_bridge_t *bridge =
			    &port->mpp_bridges[bridgeno];

			bridge->mpb_bridgeno = bridgeno;
			bridge->mpb_port = port;
			bridge->mpb_device = binfop[bridgeno].mpbi_dev;
			bridge->mpb_func = binfop[bridgeno].mpbi_func;
			bridge->mpb_hp_type = SMU_HP_INVALID;
		}
	}
}

static void
milan_fabric_ioms_nbif_init(milan_ioms_t *ioms)
{
	for (uint_t nbifno = 0; nbifno < ioms->mio_nnbifs; nbifno++) {
		const milan_nbif_info_t *ninfo = NULL;
		milan_nbif_t *nbif = &ioms->mio_nbifs[nbifno];

		nbif->mn_nbifno = nbifno;
		nbif->mn_ioms = ioms;
		VERIFY3U(nbifno, <, MILAN_IOMS_MAX_NBIF);
		switch (nbifno) {
		case 0:
			nbif->mn_nfuncs = MILAN_NBIF0_NFUNCS;
			ninfo = milan_nbif0;
			break;
		case 1:
			nbif->mn_nfuncs = MILAN_NBIF1_NFUNCS;
			ninfo = milan_nbif1;
			break;
		case 2:
			nbif->mn_nfuncs = MILAN_NBIF2_NFUNCS;
			ninfo = milan_nbif2;
			break;
		}

		for (uint_t funcno = 0; funcno < nbif->mn_nfuncs; funcno++) {
			milan_nbif_func_t *func = &nbif->mn_funcs[funcno];

			func->mne_nbif = nbif;
			func->mne_type = ninfo[funcno].mni_type;
			func->mne_dev = ninfo[funcno].mni_dev;
			func->mne_func = ninfo[funcno].mni_func;

			/*
			 * As there is a dummy device on each of these, this in
			 * theory doesn't need any explicit configuration.
			 */
			if (func->mne_type == MILAN_NBIF_T_DUMMY) {
				func->mne_flags |= MILAN_NBIF_F_NO_CONFIG;
			}
		}
	}
}

static boolean_t
milan_smu_version_at_least(const milan_iodie_t *iodie,
    const uint8_t major, const uint8_t minor, const uint8_t patch)
{
	return (iodie->mi_smu_fw[0] > major ||
	    (iodie->mi_smu_fw[0] == major && iodie->mi_smu_fw[1] > minor) ||
	    (iodie->mi_smu_fw[0] == major && iodie->mi_smu_fw[1] == minor &&
	    iodie->mi_smu_fw[2] >= patch));
}

/*
 * Create DMA attributes that are appropriate for the SMU. In particular, we
 * know experimentally that there is usually a 32-bit length register for DMA
 * and generally a 64-bit address register. There aren't many other bits that we
 * actually know here, as such, we generally end up making some assumptions out
 * of paranoia in an attempt at safety. In particular, we assume and ask for
 * page alignment here.
 *
 * XXX Remove 32-bit addr_hi constraint.
 */
static void
milan_smu_dma_attr(ddi_dma_attr_t *attr)
{
	bzero(attr, sizeof (attr));
	attr->dma_attr_version = DMA_ATTR_V0;
	attr->dma_attr_addr_lo = 0;
	attr->dma_attr_addr_hi = UINT32_MAX;
	attr->dma_attr_count_max = UINT32_MAX;
	attr->dma_attr_align = MMU_PAGESIZE;
	attr->dma_attr_minxfer = 1;
	attr->dma_attr_maxxfer = UINT32_MAX;
	attr->dma_attr_seg = UINT32_MAX;
	attr->dma_attr_sgllen = 1;
	attr->dma_attr_granular = 1;
	attr->dma_attr_flags = 0;
}

static void
milan_smu_rpc(milan_iodie_t *iodie, milan_smu_rpc_t *rpc)
{
	uint32_t resp;

	mutex_enter(&iodie->mi_smu_lock);
	milan_iodie_write(iodie, MILAN_SMU_RPC_RESP(), MILAN_SMU_RPC_NOTDONE);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG0(), rpc->msr_arg0);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG1(), rpc->msr_arg1);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG2(), rpc->msr_arg2);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG3(), rpc->msr_arg3);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG4(), rpc->msr_arg4);
	milan_iodie_write(iodie, MILAN_SMU_RPC_ARG5(), rpc->msr_arg5);
	milan_iodie_write(iodie, MILAN_SMU_RPC_REQ(), rpc->msr_req);

	/*
	 * XXX Infinite spins are bad, but we don't even have drv_usecwait yet.
	 * When we add a timeout this should then return an int.
	 */
	for (;;) {
		resp = milan_iodie_read(iodie, MILAN_SMU_RPC_RESP());
		if (resp != MILAN_SMU_RPC_NOTDONE) {
			break;
		}
	}

	rpc->msr_resp = resp;
	if (rpc->msr_resp == MILAN_SMU_RPC_OK) {
		rpc->msr_arg0 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG0());
		rpc->msr_arg1 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG1());
		rpc->msr_arg2 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG2());
		rpc->msr_arg3 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG3());
		rpc->msr_arg4 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG4());
		rpc->msr_arg5 = milan_iodie_read(iodie, MILAN_SMU_RPC_ARG5());
	}
	mutex_exit(&iodie->mi_smu_lock);
}

static boolean_t
milan_smu_rpc_get_version(milan_iodie_t *iodie, uint8_t *major, uint8_t *minor,
    uint8_t *patch)
{
	milan_smu_rpc_t rpc = { 0 };

	rpc.msr_req = MILAN_SMU_OP_GET_VERSION;
	milan_smu_rpc(iodie, &rpc);
	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		return (B_FALSE);
	}

	*major = MILAN_SMU_OP_GET_VERSION_MAJOR(rpc.msr_arg0);
	*minor = MILAN_SMU_OP_GET_VERSION_MINOR(rpc.msr_arg0);
	*patch = MILAN_SMU_OP_GET_VERSION_PATCH(rpc.msr_arg0);

	return (B_TRUE);
}

static boolean_t
milan_smu_rpc_i2c_switch(milan_iodie_t *iodie, uint32_t addr)
{
	milan_smu_rpc_t rpc = { 0 };

	rpc.msr_req = MILAN_SMU_OP_I2C_SWITCH_ADDR;
	rpc.msr_arg0 = addr;
	milan_smu_rpc(iodie, &rpc);

	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		cmn_err(CE_WARN, "SMU Set i2c address RPC Failed: addr: 0x%x, "
		    "SMU 0x%x", addr, rpc.msr_resp);
	}

	return (rpc.msr_resp == MILAN_SMU_RPC_OK);
}

static boolean_t
milan_smu_rpc_give_address(milan_iodie_t *iodie, milan_smu_addr_kind_t kind,
    uint64_t addr)
{
	milan_smu_rpc_t rpc = { 0 };

	switch (kind) {
	case MSAK_GENERIC:
		rpc.msr_req = MILAN_SMU_OP_HAVE_AN_ADDRESS;
		break;
	case MSAK_HOTPLUG:
		/*
		 * For a long time, hotplug table addresses were provided to the
		 * SMU in the same manner as any others; however, in recent
		 * versions there is a separate RPC for that.
		 */
		rpc.msr_req = milan_smu_version_at_least(iodie, 45, 90, 0) ?
		    MILAN_SMU_OP_HAVE_A_HP_ADDRESS :
		    MILAN_SMU_OP_HAVE_AN_ADDRESS;
		break;
	default:
		panic("invalid SMU address kind %d", (int)kind);
	}
	rpc.msr_arg0 = bitx64(addr, 31, 0);
	rpc.msr_arg1 = bitx64(addr, 63, 32);
	milan_smu_rpc(iodie, &rpc);

	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		cmn_err(CE_WARN, "SMU Have an Address RPC Failed: addr: 0x%lx, "
		    "SMU req 0x%x resp 0x%x", addr, rpc.msr_req, rpc.msr_resp);
	}

	return (rpc.msr_resp == MILAN_SMU_RPC_OK);

}

static boolean_t
milan_smu_rpc_send_hotplug_table(milan_iodie_t *iodie)
{
	milan_smu_rpc_t rpc = { 0 };

	rpc.msr_req = MILAN_SMU_OP_TX_PCIE_HP_TABLE;
	milan_smu_rpc(iodie, &rpc);

	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		cmn_err(CE_WARN, "SMU TX Hotplug Table Failed: SMU 0x%x",
		    rpc.msr_resp);
	}

	return (rpc.msr_resp == MILAN_SMU_RPC_OK);
}

static boolean_t
milan_smu_rpc_hotplug_flags(milan_iodie_t *iodie, uint32_t flags)
{
	milan_smu_rpc_t rpc = { 0 };

	rpc.msr_req = MILAN_SMU_OP_SET_HOPTLUG_FLAGS;
	rpc.msr_arg0 = flags;
	milan_smu_rpc(iodie, &rpc);

	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		cmn_err(CE_WARN, "SMU Set Hotplug Flags failed: SMU 0x%x",
		    rpc.msr_resp);
	}

	return (rpc.msr_resp == MILAN_SMU_RPC_OK);
}
static boolean_t
milan_smu_rpc_start_hotplug(milan_iodie_t *iodie, boolean_t one_based,
    uint8_t flags)
{
	milan_smu_rpc_t rpc = { 0 };

	rpc.msr_req = MILAN_SMU_OP_START_HOTPLUG;
	if (one_based) {
		rpc.msr_arg0 = 1;
	}
	rpc.msr_arg0 |= flags;
	milan_smu_rpc(iodie, &rpc);

	if (rpc.msr_resp != MILAN_SMU_RPC_OK) {
		cmn_err(CE_WARN, "SMU Start Yer Hotplug Failed: SMU 0x%x",
		    rpc.msr_resp);
	}

	return (rpc.msr_resp == MILAN_SMU_RPC_OK);
}

/*
 * buf and len semantics here match those of snprintf
 */
static boolean_t
milan_smu_rpc_read_brand_string(milan_iodie_t *iodie, char *buf, size_t len)
{
	milan_smu_rpc_t rpc = { 0 };
	uint_t off;

	len = MIN(len, CPUID_BRANDSTR_STRLEN + 1);
	buf[len - 1] = '\0';
	rpc.msr_req = MILAN_SMU_OP_GET_BRAND_STRING;

	for (off = 0; off * 4 < len - 1; off++) {
		rpc.msr_arg0 = off;
		milan_smu_rpc(iodie, &rpc);

		if (rpc.msr_resp != MILAN_SMU_RPC_OK)
			return (B_FALSE);

		bcopy(&rpc.msr_arg0, buf + off * 4, len - off * 4);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_version_at_least(const milan_iodie_t *iodie,
    const uint32_t major, const uint32_t minor)
{
	return (iodie->mi_dxio_fw[0] > major ||
	    (iodie->mi_dxio_fw[0] == major && iodie->mi_dxio_fw[1] >= minor));
}

static void
milan_dxio_rpc(milan_iodie_t *iodie, milan_dxio_rpc_t *dxio_rpc)
{
	milan_smu_rpc_t smu_rpc = { 0 };

	smu_rpc.msr_req = MILAN_SMU_OP_DXIO;
	smu_rpc.msr_arg0 = dxio_rpc->mdr_req;
	smu_rpc.msr_arg1 = dxio_rpc->mdr_engine;
	smu_rpc.msr_arg2 = dxio_rpc->mdr_arg0;
	smu_rpc.msr_arg3 = dxio_rpc->mdr_arg1;
	smu_rpc.msr_arg4 = dxio_rpc->mdr_arg2;
	smu_rpc.msr_arg5 = dxio_rpc->mdr_arg3;

	milan_smu_rpc(iodie, &smu_rpc);

	dxio_rpc->mdr_smu_resp = smu_rpc.msr_resp;
	if (smu_rpc.msr_resp == MILAN_SMU_RPC_OK) {
		dxio_rpc->mdr_dxio_resp = smu_rpc.msr_arg0;
		dxio_rpc->mdr_engine = smu_rpc.msr_arg1;
		dxio_rpc->mdr_arg0 = smu_rpc.msr_arg2;
		dxio_rpc->mdr_arg1 = smu_rpc.msr_arg3;
		dxio_rpc->mdr_arg2 = smu_rpc.msr_arg4;
		dxio_rpc->mdr_arg3 = smu_rpc.msr_arg5;
	}
}

static boolean_t
milan_dxio_rpc_get_version(milan_iodie_t *iodie, uint32_t *major,
    uint32_t *minor)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_GET_VERSION;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Get Version RPC Failed: SMU 0x%x, "
		    "DXIO: 0x%x", rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	*major = rpc.mdr_arg0;
	*minor = rpc.mdr_arg1;

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_init(milan_iodie_t *iodie)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_INIT;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Init RPC Failed: SMU 0x%x, DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_set_var(milan_iodie_t *iodie, uint32_t var, uint32_t val)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_SET_VARIABLE;
	rpc.mdr_engine = var;
	rpc.mdr_arg0 = val;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    !(rpc.mdr_dxio_resp == MILAN_DXIO_RPC_OK ||
	    rpc.mdr_dxio_resp == MILAN_DXIO_RPC_MBOX_IDLE)) {
		cmn_err(CE_WARN, "DXIO Set Variable Failed: Var: 0x%x, "
		    "Val: 0x%x, SMU 0x%x, DXIO: 0x%x", var, val,
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_pcie_poweroff_config(milan_iodie_t *iodie, uint8_t delay,
    boolean_t disable_prep)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_SET_VARIABLE;
	rpc.mdr_engine = MILAN_DXIO_VAR_PCIE_POWER_OFF_DELAY;
	rpc.mdr_arg0 = delay;
	rpc.mdr_arg1 = disable_prep ? 1 : 0;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    !(rpc.mdr_dxio_resp == MILAN_DXIO_RPC_OK ||
	    rpc.mdr_dxio_resp == MILAN_DXIO_RPC_MBOX_IDLE)) {
		cmn_err(CE_WARN, "DXIO Set PCIe Power Off Config Failed: "
		    "Delay: 0x%x, Disable Prep: 0x%x, SMU 0x%x, DXIO: 0x%x",
		    delay, disable_prep, rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_clock_gating(milan_iodie_t *iodie, uint8_t mask, uint8_t val)
{
	milan_dxio_rpc_t rpc = { 0 };

	/*
	 * The mask and val are only allowed to be 7-bit values.
	 */
	VERIFY0(mask & 0x80);
	VERIFY0(val & 0x80);
	rpc.mdr_req = MILAN_DXIO_OP_SET_RUNTIME_PROP;
	rpc.mdr_engine = MILAN_DXIO_ENGINE_PCIE;
	rpc.mdr_arg0 = MILAN_DXIO_RT_CONF_CLOCK_GATE;
	rpc.mdr_arg1 = mask;
	rpc.mdr_arg2 = val;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Clock Gating Failed: SMU 0x%x, "
		    "DXIO: 0x%x", rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Currently there are no capabilities defined, which makes it hard for us to
 * know the exact command layout here. The only thing we know is safe is that
 * it's all zeros, though it probably otherwise will look like
 * MILAN_DXIO_OP_LOAD_DATA.
 */
static boolean_t
milan_dxio_rpc_load_caps(milan_iodie_t *iodie)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_LOAD_CAPS;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Load Caps Failed: SMU 0x%x, DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_load_data(milan_iodie_t *iodie, uint32_t type,
    uint64_t phys_addr, uint32_t len, uint32_t mystery)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_LOAD_DATA;
	rpc.mdr_engine = (uint32_t)(phys_addr >> 32);
	rpc.mdr_arg0 = phys_addr & 0xffffffff;
	rpc.mdr_arg1 = len / 4;
	rpc.mdr_arg2 = mystery;
	rpc.mdr_arg3 = type;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Load Data Failed: Heap: 0x%x, PA: "
		    "0x%lx, Len: 0x%x, SMU 0x%x, DXIO: 0x%x", type, phys_addr,
		    len, rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_conf_training(milan_iodie_t *iodie, uint32_t reset_time,
    uint32_t rx_poll, uint32_t l0_poll)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_SET_RUNTIME_PROP;
	rpc.mdr_engine = MILAN_DXIO_ENGINE_PCIE;
	rpc.mdr_arg0 = MILAN_DXIO_RT_CONF_PCIE_TRAIN;
	rpc.mdr_arg1 = reset_time;
	rpc.mdr_arg2 = rx_poll;
	rpc.mdr_arg3 = l0_poll;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    !(rpc.mdr_dxio_resp == MILAN_DXIO_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK)) {
		cmn_err(CE_WARN, "DXIO Conf. PCIe Training RPC Failed: "
		    "SMU 0x%x, DXIO: 0x%x", rpc.mdr_smu_resp,
		    rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * This is a hodgepodge RPC that is used to set various rt configuration
 * properties.
 */
static boolean_t
milan_dxio_rpc_misc_rt_conf(milan_iodie_t *iodie, uint32_t code,
    boolean_t state)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_SET_RUNTIME_PROP;
	rpc.mdr_engine = MILAN_DXIO_ENGINE_NONE;
	rpc.mdr_arg0 = MILAN_DXIO_RT_SET_CONF;
	rpc.mdr_arg1 = code;
	rpc.mdr_arg2 = state ? 1 : 0;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    !(rpc.mdr_dxio_resp == MILAN_DXIO_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK)) {
		cmn_err(CE_WARN, "DXIO Set Misc. rt conf failed: Code: 0x%x, "
		    "Val: 0x%x, SMU 0x%x, DXIO: 0x%x", code, state,
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_sm_start(milan_iodie_t *iodie)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_START_SM;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO SM Start RPC Failed: SMU 0x%x, "
		    "DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_sm_resume(milan_iodie_t *iodie)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_RESUME_SM;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO SM Start RPC Failed: SMU 0x%x, "
		    "DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
milan_dxio_rpc_sm_reload(milan_iodie_t *iodie)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_RELOAD_SM;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO SM Reload RPC Failed: SMU 0x%x, "
		    "DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}


static boolean_t
milan_dxio_rpc_sm_getstate(milan_iodie_t *iodie, milan_dxio_reply_t *smp)
{
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_GET_SM_STATE;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO SM Start RPC Failed: SMU 0x%x, "
		    "DXIO: 0x%x",
		    rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	smp->mds_type = bitx64(rpc.mdr_engine, 7, 0);
	smp->mds_nargs = bitx64(rpc.mdr_engine, 16, 8);
	smp->mds_arg0 = rpc.mdr_arg0;
	smp->mds_arg1 = rpc.mdr_arg1;
	smp->mds_arg2 = rpc.mdr_arg2;
	smp->mds_arg3 = rpc.mdr_arg3;

	return (B_TRUE);
}

/*
 * Retrieve the current engine data from DXIO.
 */
static boolean_t
milan_dxio_rpc_retrieve_engine(milan_iodie_t *iodie)
{
	milan_dxio_config_t *conf = &iodie->mi_dxio_conf;
	milan_dxio_rpc_t rpc = { 0 };

	rpc.mdr_req = MILAN_DXIO_OP_GET_ENGINE_CFG;
	rpc.mdr_engine = (uint32_t)(conf->mdc_pa >> 32);
	rpc.mdr_arg0 = conf->mdc_pa & 0xffffffff;
	rpc.mdr_arg1 = conf->mdc_alloc_len / 4;

	milan_dxio_rpc(iodie, &rpc);
	if (rpc.mdr_smu_resp != MILAN_SMU_RPC_OK ||
	    rpc.mdr_dxio_resp != MILAN_DXIO_RPC_OK) {
		cmn_err(CE_WARN, "DXIO Retrieve Engine Failed: SMU 0x%x, "
		    "DXIO: 0x%x", rpc.mdr_smu_resp, rpc.mdr_dxio_resp);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static int
milan_dump_versions(milan_iodie_t *iodie, void *arg)
{
	uint8_t maj, min, patch;
	uint32_t dxmaj, dxmin;
	milan_soc_t *soc = iodie->mi_soc;

	if (milan_smu_rpc_get_version(iodie, &maj, &min, &patch)) {
		cmn_err(CE_NOTE, "Socket %u SMU Version: %u.%u.%u",
		    soc->ms_socno, maj, min, patch);
		iodie->mi_smu_fw[0] = maj;
		iodie->mi_smu_fw[1] = min;
		iodie->mi_smu_fw[2] = patch;
	} else {
		cmn_err(CE_NOTE, "Socket %u: failed to read SMU version",
		    soc->ms_socno);
	}

	if (milan_dxio_rpc_get_version(iodie, &dxmaj, &dxmin)) {
		cmn_err(CE_NOTE, "Socket %u DXIO Version: %u.%u",
		    soc->ms_socno, dxmaj, dxmin);
		iodie->mi_dxio_fw[0] = dxmaj;
		iodie->mi_dxio_fw[1] = dxmin;
	} else {
		cmn_err(CE_NOTE, "Socket %u: failed to read DXIO version",
		    soc->ms_socno);
	}

	return (0);
}

static void
milan_ccx_init_core(milan_ccx_t *ccx, uint8_t lidx, uint8_t pidx)
{
	smn_reg_t reg;
	uint32_t val;
	milan_core_t *core = &ccx->mcx_cores[lidx];
	milan_ccd_t *ccd = ccx->mcx_ccd;
	milan_iodie_t *iodie = ccd->mcd_iodie;

	core->mc_ccx = ccx;
	core->mc_physical_coreno = pidx;

	reg = milan_core_reg(core, D_SCFCTP_PMREG_INITPKG0);
	val = milan_core_read(core, reg);
	VERIFY3U(val, !=, 0xffffffffU);

	core->mc_logical_coreno = SCFCTP_PMREG_INITPKG0_GET_LOG_CORE(val);

	VERIFY3U(SCFCTP_PMREG_INITPKG0_GET_PHYS_CORE(val), ==, pidx);
	VERIFY3U(SCFCTP_PMREG_INITPKG0_GET_PHYS_CCX(val), ==,
	    ccx->mcx_physical_cxno);
	VERIFY3U(SCFCTP_PMREG_INITPKG0_GET_PHYS_DIE(val), ==,
	    ccx->mcx_ccd->mcd_physical_dieno);

	core->mc_nthreads = SCFCTP_PMREG_INITPKG0_GET_SMTEN(val) + 1;
	VERIFY3U(core->mc_nthreads, <=, MILAN_MAX_THREADS_PER_CORE);

	for (uint8_t thr = 0; thr < core->mc_nthreads; thr++) {
		uint32_t apicid = 0;
		milan_thread_t *thread = &core->mc_threads[thr];

		thread->mt_threadno = thr;
		thread->mt_core = core;
		nthreads++;

		/*
		 * You may be wondering why we don't use the contents of
		 * DF::CcdUnitIdMask here to determine the number of bits at
		 * each level.  There are two reasons, one simple and one not:
		 *
		 * - First, it's not correct.  The UnitId masks describe (*)
		 *   the physical ID spaces, which are distinct from how APIC
		 *   IDs are computed.  APIC IDs depend on the number of each
		 *   component that are *actually present*, rounded up to the
		 *   next power of 2 at each component.  For example, if there
		 *   are 4 CCDs, there will be 2 bits in the APIC ID for the
		 *   logical CCD number, even though representing the UnitId
		 *   on Milan requires 3 bits for the CCD.  No, we don't know
		 *   why this is so; it would certainly have been simpler to
		 *   always use the physical ID to compute the initial APIC ID.
		 * - Second, not only are APIC IDs not UnitIds, there is nothing
		 *   documented that does consume UnitIds.  We are given a nice
		 *   discussion of what they are and this lovingly detailed way
		 *   to discover how to compute them, but so far as I have been
		 *   able to tell, neither UnitIds nor the closely related
		 *   CpuIds are ever used.  If we later find that we do need
		 *   these identifiers, additional code to construct them based
		 *   on this discovery mechanism should be added.
		 */
		apicid = iodie->mi_soc->ms_socno;
		apicid <<= highbit(iodie->mi_soc->ms_ndies - 1);
		apicid |= 0;	/* XXX multi-die SOCs not supported here */
		apicid <<= highbit(iodie->mi_nccds - 1);
		apicid |= ccd->mcd_logical_dieno;
		apicid <<= highbit(ccd->mcd_nccxs - 1);
		apicid |= ccx->mcx_logical_cxno;
		apicid <<= highbit(ccx->mcx_ncores - 1);
		apicid |= core->mc_logical_coreno;
		apicid <<= highbit(core->mc_nthreads - 1);
		apicid |= thr;

		thread->mt_apicid = (apicid_t)apicid;
	}
}

static void
milan_ccx_init_soc(milan_soc_t *soc)
{
	const milan_fabric_t *fabric = soc->ms_fabric;
	milan_iodie_t *iodie = &soc->ms_iodies[0];

	/*
	 * We iterate over the physical CCD space; population of that
	 * space may be sparse.  Keep track of the logical CCD index in
	 * lccd; ccdpno is the physical CCD index we're considering.
	 */
	for (uint8_t ccdpno = 0, lccd = 0;
	    ccdpno < MILAN_MAX_CCDS_PER_IODIE; ccdpno++) {
		uint8_t core_shift, pcore, lcore;
		smn_reg_t reg;
		uint32_t val;
		uint32_t cores_enabled;
		milan_ccd_t *ccd = &iodie->mi_ccds[lccd];
		milan_ccx_t *ccx = &ccd->mcd_ccxs[0];

		/*
		 * The CCM is part of the IO die, not the CCD itself.
		 * If it is disabled, we skip this CCD index as even if
		 * it exists nothing can reach it.
		 */
		val = milan_df_read32(iodie, MILAN_DF_FIRST_CCM_ID + ccdpno,
		    DF_FBIINFO0);

		VERIFY3U(DF_FBIINFO0_GET_TYPE(val), ==, DF_TYPE_CCM);
		if (DF_FBIINFO0_V3_GET_ENABLED(val) == 0)
			continue;

		/*
		 * At leaast some of the time, a CCM will be enabled
		 * even if there is no corresponding CCD.  To avoid
		 * a possibly invalid read (see milan_fabric_topo_init()
		 * comments), we also check whether any core is enabled
		 * on this CCD.
		 *
		 * XXX reduce magic
		 */
		val = milan_df_bcast_read32(iodie, (ccdpno < 4) ?
		    DF_PHYS_CORE_EN0_V3 : DF_PHYS_CORE_EN1_V3);
		core_shift = (ccdpno & 3) * MILAN_MAX_CORES_PER_CCX *
		    MILAN_MAX_CCXS_PER_CCD;
		cores_enabled = bitx32(val, core_shift + 7, core_shift);

		if (cores_enabled == 0)
			continue;

		VERIFY3U(lccd, <, MILAN_MAX_CCDS_PER_IODIE);
		ccd->mcd_iodie = iodie;
		ccd->mcd_logical_dieno = lccd++;
		ccd->mcd_physical_dieno = ccdpno;
		ccd->mcd_ccm_comp_id = MILAN_DF_FIRST_CCM_ID + ccdpno;
		/*
		 * XXX Non-Milan may require nonzero component ID shift.
		 */
		ccd->mcd_ccm_fabric_id = ccd->mcd_ccm_comp_id |
		    (iodie->mi_node_id << fabric->mf_node_shift);

		/* XXX avoid panicking on bad data from firmware */
		reg = milan_ccd_reg(ccd, D_SMUPWR_CCD_DIE_ID);
		val = milan_ccd_read(ccd, reg);
		VERIFY3U(val, ==, ccdpno);

		reg = milan_ccd_reg(ccd, D_SMUPWR_THREAD_CFG);
		val = milan_ccd_read(ccd, reg);
		ccd->mcd_nccxs = SMUPWR_THREAD_CFG_GET_COMPLEX_COUNT(val) + 1;
		VERIFY3U(ccd->mcd_nccxs, <=, MILAN_MAX_CCXS_PER_CCD);

		if (ccd->mcd_nccxs == 0) {
			cmn_err(CE_NOTE, "CCD 0x%x: no CCXs reported",
			    ccd->mcd_physical_dieno);
			continue;
		}

		/*
		 * Make sure that the CCD's local understanding of
		 * enabled cores matches what we found earlier through
		 * the DF.  A mismatch here is a firmware bug; XXX and
		 * if that happens?
		 */
		reg = milan_ccd_reg(ccd, D_SMUPWR_CORE_EN);
		val = milan_ccd_read(ccd, reg);
		VERIFY3U(SMUPWR_CORE_EN_GET(val), ==, cores_enabled);

		/*
		 * XXX While we know there is only ever 1 CCX per Milan CCD,
		 * DF::CCXEnable allows for 2 because the DFv3 implementation
		 * is shared with Rome, which has up to 2 CCXs per CCD.
		 * Although we know we only ever have 1 CCX, we don't,
		 * strictly, know that the CCX is always physical index 0.
		 * Here we assume it, but we probably want to change the
		 * MILAN_MAX_xxx_PER_yyy so that they reflect the size of the
		 * physical ID spaces rather than the maximum logical entity
		 * counts.  Doing so would accommodate a part that has a single
		 * CCX per CCD, but at index 1.
		 */
		ccx->mcx_ccd = ccd;
		ccx->mcx_logical_cxno = 0;
		ccx->mcx_physical_cxno = 0;

		/*
		 * All the cores on the CCD will (should) return the
		 * same values in PMREG_INITPKG0 and PMREG_INITPKG7.
		 * The catch is that we have to read them from a core
		 * that exists or we get all-1s.  Use the mask of
		 * cores enabled on this die that we already computed
		 * to find one to read from, then bootstrap into the
		 * core enumeration.  XXX At some point we probably
		 * should do away with all this cross-checking and
		 * choose something to trust.
		 */
		for (pcore = 0;
		    (cores_enabled & (1 << pcore)) == 0 &&
		    pcore < MILAN_MAX_CORES_PER_CCX; pcore++)
			;
		VERIFY3U(pcore, <, MILAN_MAX_CORES_PER_CCX);

		reg = SCFCTP_PMREG_INITPKG7(ccdpno, pcore);
		val = milan_smn_read(iodie, reg);
		VERIFY3U(val, !=, 0xffffffffU);

		ccx->mcx_ncores = SCFCTP_PMREG_INITPKG7_GET_N_CORES(val) + 1;
		iodie->mi_nccds = SCFCTP_PMREG_INITPKG7_GET_N_DIES(val) + 1;

		for (pcore = 0, lcore = 0;
		    pcore < MILAN_MAX_CORES_PER_CCX; pcore++) {
			if ((cores_enabled & (1 << pcore)) == 0)
				continue;
			milan_ccx_init_core(ccx, lcore, pcore);
			++lcore;
		}

		VERIFY3U(lcore, ==, ccx->mcx_ncores);
	}
}

/*
 * Right now we're running on the boot CPU. We know that a single socket has to
 * be populated. Our job is to go through and determine what the rest of the
 * topology of this system looks like in terms of the data fabric, north
 * bridges, and related. We can rely on the DF instance 0/18/0 to exist;
 * however, that's it.
 *
 * An important rule of discovery here is that we should not rely on invalid PCI
 * reads. We should be able to bootstrap from known good data and what the
 * actual SoC has discovered here rather than trying to fill that in ourselves.
 */
void
milan_fabric_topo_init(void)
{
	uint8_t nsocs;
	uint32_t syscfg, syscomp, fidmask;
	milan_fabric_t *fabric = &milan_fabric;

	PRM_POINT("milan_fabric_topo_init() starting...");

	/*
	 * Before we can do anything else, we must set up PCIe ECAM.  We locate
	 * this region beyond either the end of DRAM or the IOMMU hole,
	 * whichever is higher.  The remainder of the 64-bit MMIO space is
	 * available for allocation to IOMSs (for e.g. PCIe devices).
	 */
	fabric->mf_tom = MSR_AMD_TOM_MASK(rdmsr(MSR_AMD_TOM));
	fabric->mf_tom2 = MSR_AMD_TOM2_MASK(rdmsr(MSR_AMD_TOM2));

	fabric->mf_ecam_base = P2ROUNDUP(MAX(fabric->mf_tom2,
	    MILAN_PHYSADDR_IOMMU_HOLE_END), PCIE_CFGSPACE_ALIGN);
	fabric->mf_mmio64_base = fabric->mf_ecam_base + PCIE_CFGSPACE_SIZE;

	pcie_cfgspace_init();

	syscfg = milan_df_early_read32(DF_SYSCFG_V3);
	syscomp = milan_df_early_read32(DF_COMPCNT_V2);
	nsocs = DF_SYSCFG_V3_GET_OTHER_SOCK(syscfg) + 1;

	/*
	 * These are used to ensure that we're on a platform that matches our
	 * expectations. These are generally constraints of Rome and Milan.
	 */
	VERIFY3U(nsocs, ==, DF_COMPCNT_V2_GET_PIE(syscomp));
	VERIFY3U(nsocs * MILAN_IOMS_PER_IODIE, ==,
	    DF_COMPCNT_V2_GET_IOMS(syscomp));

	/*
	 * Gather the register masks for decoding global fabric IDs into local
	 * instance IDs.
	 */
	fidmask = milan_df_early_read32(DF_FIDMASK0_V3);
	fabric->mf_node_mask = DF_FIDMASK0_V3_GET_NODE_MASK(fidmask);
	fabric->mf_comp_mask = DF_FIDMASK0_V3_GET_COMP_MASK(fidmask);

	fidmask = milan_df_early_read32(DF_FIDMASK1_V3);
	fabric->mf_node_shift = DF_FIDMASK1_V3_GET_NODE_SHIFT(fidmask);

	fabric->mf_nsocs = nsocs;
	for (uint8_t socno = 0; socno < nsocs; socno++) {
		uint32_t busno, nodeid;
		const df_reg_def_t rd = DF_SYSCFG_V3;
		milan_soc_t *soc = &fabric->mf_socs[socno];
		milan_iodie_t *iodie = &soc->ms_iodies[0];

		soc->ms_socno = socno;
		soc->ms_ndies = MILAN_FABRIC_MAX_DIES_PER_SOC;
		soc->ms_fabric = fabric;
		iodie->mi_dfno = AMDZEN_DF_FIRST_DEVICE + socno;

		nodeid = pci_getl_func(AMDZEN_DF_BUSNO, iodie->mi_dfno,
		    rd.drd_func, rd.drd_reg);
		iodie->mi_node_id = DF_SYSCFG_V3_GET_NODE_ID(nodeid);
		iodie->mi_soc = soc;

		if (iodie->mi_node_id == 0) {
			iodie->mi_flags |= MILAN_IODIE_F_PRIMARY;
		}

		/*
		 * XXX Because we do not know the circumstances all these locks
		 * will be used during early initialization, set these to be
		 * spin locks for the moment.
		 */
		mutex_init(&iodie->mi_df_ficaa_lock, NULL, MUTEX_SPIN,
		    (ddi_iblock_cookie_t)ipltospl(15));
		mutex_init(&iodie->mi_smn_lock, NULL, MUTEX_SPIN,
		    (ddi_iblock_cookie_t)ipltospl(15));
		mutex_init(&iodie->mi_smu_lock, NULL, MUTEX_SPIN,
		    (ddi_iblock_cookie_t)ipltospl(15));

		busno = milan_df_bcast_read32(iodie, DF_CFG_ADDR_CTL_V2);
		iodie->mi_smn_busno = DF_CFG_ADDR_CTL_GET_BUS_NUM(busno);

		iodie->mi_nioms = MILAN_IOMS_PER_IODIE;
		fabric->mf_total_ioms += iodie->mi_nioms;
		for (uint8_t iomsno = 0; iomsno < iodie->mi_nioms; iomsno++) {
			uint32_t val;

			milan_ioms_t *ioms = &iodie->mi_ioms[iomsno];

			ioms->mio_num = iomsno;
			ioms->mio_iodie = iodie;
			ioms->mio_comp_id = MILAN_DF_FIRST_IOMS_ID + iomsno;
			ioms->mio_fabric_id = ioms->mio_comp_id |
			    (iodie->mi_node_id << fabric->mf_node_shift);

			val = milan_df_read32(iodie, ioms->mio_comp_id,
			    DF_CFG_ADDR_CTL_V2);
			ioms->mio_pci_busno = DF_CFG_ADDR_CTL_GET_BUS_NUM(val);

			/*
			 * Only IOMS 0 has a WAFL port.
			 */
			ioms->mio_npcie_ports = milan_nbio_n_pcie_ports(iomsno);
			if (iomsno == MILAN_IOMS_HAS_WAFL) {
				ioms->mio_flags |= MILAN_IOMS_F_HAS_WAFL;
			}
			ioms->mio_nnbifs = MILAN_IOMS_MAX_NBIF;

			if (iomsno == MILAN_IOMS_HAS_FCH) {
				ioms->mio_flags |= MILAN_IOMS_F_HAS_FCH;
			}

			milan_fabric_ioms_pcie_init(ioms);
			milan_fabric_ioms_nbif_init(ioms);
		}

		/*
		 * In order to guarantee that we can safely perform SMU and DXIO
		 * functions once we have returned (and when we go to read the
		 * brand string for the CCXs even before then), we go through
		 * now and capture firmware versions.
		 */
		VERIFY0(milan_dump_versions(iodie, NULL));

		milan_ccx_init_soc(soc);
		if (!milan_smu_rpc_read_brand_string(iodie, soc->ms_brandstr,
		    sizeof (soc->ms_brandstr))) {
			soc->ms_brandstr[0] = '\0';
		}
	}

	if (nthreads > NCPU) {
		cmn_err(CE_WARN, "%d CPUs found but only %d supported",
		    nthreads, NCPU);
		nthreads = NCPU;
	}
	boot_max_ncpus = max_ncpus = boot_ncpus = nthreads;
}

/*
 * The IOHC needs our help to know where the top of memory is. This is
 * complicated for a few reasons. Right now we're relying on where TOM and TOM2
 * have been programmed by the PSP to determine that. The biggest gotcha here is
 * the secondary MMIO hole that leads to us needing to actually have a 3rd
 * register in the IOHC for indicating DRAM/MMIO splits.
 */
static int
milan_fabric_init_tom(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;
	uint64_t tom2, tom3;
	milan_fabric_t *fabric = ioms->mio_iodie->mi_soc->ms_fabric;

	/*
	 * This register is a little funky. Bit 32 of the address has to be
	 * specified in bit 0. Otherwise, bits 31:23 are the limit.
	 */
	val = pci_getl_func(ioms->mio_pci_busno, 0, 0, IOHC_TOM);
	if (bitx64(fabric->mf_tom, 32, 32) != 0) {
		val = IOHC_TOM_SET_BIT32(val, 1);
	}

	val = IOHC_TOM_SET_TOM(val, bitx64(fabric->mf_tom, 31, 23));
	pci_putl_func(ioms->mio_pci_busno, 0, 0, IOHC_TOM, val);

	if (fabric->mf_tom2 == 0) {
		return (0);
	}

	if (fabric->mf_tom2 > MILAN_PHYSADDR_IOMMU_HOLE_END) {
		tom2 = MILAN_PHYSADDR_IOMMU_HOLE;
		tom3 = fabric->mf_tom2 - 1;
	} else {
		tom2 = fabric->mf_tom2;
		tom3 = 0;
	}

	/*
	 * Write the upper register before the lower so we don't accidentally
	 * enable it in an incomplete fashion.
	 */
	reg = milan_ioms_reg(ioms, D_IOHC_DRAM_TOM2_HI, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_DRAM_TOM2_HI_SET_TOM2(val, bitx64(tom2, 40, 32));
	milan_ioms_write(ioms, reg, val);

	reg = milan_ioms_reg(ioms, D_IOHC_DRAM_TOM2_LOW, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_DRAM_TOM2_LOW_SET_EN(val, 1);
	val = IOHC_DRAM_TOM2_LOW_SET_TOM2(val, bitx64(tom2, 31, 23));
	milan_ioms_write(ioms, reg, val);

	if (tom3 == 0) {
		return (0);
	}

	reg = milan_ioms_reg(ioms, D_IOHC_DRAM_TOM3, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_DRAM_TOM3_SET_EN(val, 1);
	val = IOHC_DRAM_TOM3_SET_LIMIT(val, bitx64(tom3, 51, 22));
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * We want to disable VGA and send all downstream accesses to its address range
 * to DRAM just as we do from the cores.  This requires clearing
 * IOHC::NB_PCI_ARB[VGA_HOLE]; for reasons unknown, the default here is
 * different from the other settings that typically default to VGA-off.  The
 * rest of this register has nothing to do with decoding and we leave its
 * contents alone.
 */
static int
milan_fabric_disable_iohc_vga(milan_ioms_t *ioms, void *arg)
{
	uint32_t val;

	val = pci_getl_func(ioms->mio_pci_busno, 0, 0, IOHC_NB_PCI_ARB);
	val = IOHC_NB_PCI_ARB_SET_VGA_HOLE(val, IOHC_NB_PCI_ARB_VGA_HOLE_RAM);
	pci_putl_func(ioms->mio_pci_busno, 0, 0, IOHC_NB_PCI_ARB, val);

	return (0);
}

/*
 * Different parts of the IOMS need to be programmed such that they can figure
 * out if they have a corresponding FCH present on them. The FCH is only present
 * on IOMS 3. Therefore if we're on IOMS 3 we need to update various other bis
 * of the IOAGR and related; however, if we're not on IOMS 3 then we just need
 * to zero out some of this.
 */
static int
milan_fabric_init_iohc_fch_link(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;

	reg = milan_ioms_reg(ioms, D_IOHC_SB_LOCATION, 0);
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0) {
		smn_reg_t iommureg;
		uint32_t val;

		val = milan_ioms_read(ioms, reg);
		iommureg = milan_ioms_reg(ioms, D_IOMMUL1_SB_LOCATION,
		    MIL1SU_IOAGR);
		milan_ioms_write(ioms, iommureg, val);
		iommureg = milan_ioms_reg(ioms, D_IOMMUL2_SB_LOCATION, 0);
		milan_ioms_write(ioms, iommureg, val);
	} else {
		milan_ioms_write(ioms, reg, 0);
	}

	return (0);
}

/*
 * For some reason the PCIe reference clock does not default to 100 MHz. We need
 * to do this ourselves. If we don't do this, PCIe will not be very happy.
 */
static int
milan_fabric_init_pcie_refclk(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_ioms_reg(ioms, D_IOHC_REFCLK_MODE, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_REFCLK_MODE_SET_27MHZ(val, 0);
	val = IOHC_REFCLK_MODE_SET_25MHZ(val, 0);
	val = IOHC_REFCLK_MODE_SET_100MHZ(val, 1);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * While the value for the delay comes from the PPR, the value for the limit
 * comes from other AMD sources.
 */
static int
milan_fabric_init_pci_to(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_ioms_reg(ioms, D_IOHC_PCIE_CRS_COUNT, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_PCIE_CRS_COUNT_SET_LIMIT(val, 0x262);
	val = IOHC_PCIE_CRS_COUNT_SET_DELAY(val, 0x6);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * Here we initialize several of the IOHC features and related vendor-specific
 * messages are all set up correctly. XXX We're using lazy defaults of what the
 * system default has historically been here for some of these. We should test
 * and forcibly disable in hardware. Probably want to manipulate
 * IOHC::PCIE_VDM_CNTL2 at some point to better figure out the VDM story. XXX
 * Also, ARI entablement is being done earlier than otherwise because we want to
 * only touch this reg in one place if we can.
 */
static int
milan_fabric_init_iohc_features(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_ioms_reg(ioms, D_IOHC_FCTL, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_FCTL_SET_ARI(val, 1);
	/* XXX Wants to be IOHC_FCTL_P2P_DISABLE? */
	val = IOHC_FCTL_SET_P2P(val, IOHC_FCTL_P2P_DROP_NMATCH);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

static int
milan_fabric_init_arbitration_ioms(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	/*
	 * Start with IOHC burst related entries. These are always the same
	 * across every entity. The value used for the actual time entries just
	 * varies.
	 */
	for (uint_t i = 0; i < IOHC_SION_MAX_ENTS; i++) {
		uint32_t tsval;

		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOHC_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOHC_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S1_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOHC_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S1_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOHC_SION_CLIREQ_BURST_VAL);

		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOHC_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOHC_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S1_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOHC_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S1_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOHC_SION_RDRSP_BURST_VAL);

		switch (i) {
		case 0:
		case 1:
		case 2:
			tsval = IOHC_SION_CLIREQ_TIME_0_2_VAL;
			break;
		case 3:
		case 4:
			tsval = IOHC_SION_CLIREQ_TIME_3_4_VAL;
			break;
		case 5:
			tsval = IOHC_SION_CLIREQ_TIME_5_VAL;
			break;
		default:
			continue;
		}

		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_CLIREQ_TIME_LOW, i);
		milan_ioms_write(ioms, reg, tsval);
		reg = milan_ioms_reg(ioms, D_IOHC_SION_S0_CLIREQ_TIME_HI, i);
		milan_ioms_write(ioms, reg, tsval);
	}

	/*
	 * Yes, we only set [4:0] here. I know it's odd. We're actually setting
	 * S1's only instance (0) and the first 4 of the 6 instances of S0.
	 * Apparently it's not necessary to set instances 5 and 6.
	 */
	for (uint_t i = 0; i < 4; i++) {
		reg = milan_ioms_reg(ioms, D_IOHC_SION_Sn_CLI_NP_DEFICIT, i);

		val = milan_ioms_read(ioms, reg);
		val = IOHC_SION_CLI_NP_DEFICIT_SET(val,
		    IOHC_SION_CLI_NP_DEFICIT_VAL);
		milan_ioms_write(ioms, reg, val);
	}

	/*
	 * Go back and finally set the live lock watchdog to finish off the
	 * IOHC.
	 */
	reg = milan_ioms_reg(ioms, D_IOHC_SION_LLWD_THRESH, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_SION_LLWD_THRESH_SET(val, IOHC_SION_LLWD_THRESH_VAL);
	milan_ioms_write(ioms, reg, val);

	/*
	 * Next on our list is the IOAGR. While there are 5 entries, only 4 are
	 * ever set it seems.
	 */
	for (uint_t i = 0; i < 4; i++) {
		uint32_t tsval;

		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S1_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S1_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_CLIREQ_BURST_VAL);

		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S1_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S1_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, IOAGR_SION_RDRSP_BURST_VAL);

		switch (i) {
		case 0:
		case 1:
		case 2:
			tsval = IOAGR_SION_CLIREQ_TIME_0_2_VAL;
			break;
		case 3:
			tsval = IOAGR_SION_CLIREQ_TIME_3_VAL;
			break;
		default:
			continue;
		}

		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_CLIREQ_TIME_LOW, i);
		milan_ioms_write(ioms, reg, tsval);
		reg = milan_ioms_reg(ioms, D_IOAGR_SION_S0_CLIREQ_TIME_HI, i);
		milan_ioms_write(ioms, reg, tsval);
	}

	/*
	 * The IOAGR only has the watchdog.
	 */

	reg = milan_ioms_reg(ioms, D_IOAGR_SION_LLWD_THRESH, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOAGR_SION_LLWD_THRESH_SET(val, IOAGR_SION_LLWD_THRESH_VAL);
	milan_ioms_write(ioms, reg, val);

	/*
	 * Finally, the SDPMUX variant, which is surprisingly consistent
	 * compared to everything else to date.
	 */
	for (uint_t i = 0; i < SDPMUX_SION_MAX_ENTS; i++) {
		reg = milan_ioms_reg(ioms,
		    D_SDPMUX_SION_S0_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S0_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms,
		    D_SDPMUX_SION_S1_CLIREQ_BURST_LOW, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S1_CLIREQ_BURST_HI, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_BURST_VAL);

		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S0_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S0_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S1_RDRSP_BURST_LOW, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_RDRSP_BURST_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S1_RDRSP_BURST_HI, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_RDRSP_BURST_VAL);

		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S0_CLIREQ_TIME_LOW, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_TIME_VAL);
		reg = milan_ioms_reg(ioms, D_SDPMUX_SION_S0_CLIREQ_TIME_HI, i);
		milan_ioms_write(ioms, reg, SDPMUX_SION_CLIREQ_TIME_VAL);
	}

	reg = milan_ioms_reg(ioms, D_SDPMUX_SION_LLWD_THRESH, 0);
	val = milan_ioms_read(ioms, reg);
	val = SDPMUX_SION_LLWD_THRESH_SET(val, SDPMUX_SION_LLWD_THRESH_VAL);
	milan_ioms_write(ioms, reg, val);

	/*
	 * XXX We probably don't need this since we don't have USB. But until we
	 * have things working and can experiment, hard to say. If someone were
	 * to use the bus, probably something we need to consider.
	 */
	reg = milan_ioms_reg(ioms, D_IOHC_USB_QOS_CTL, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_USB_QOS_CTL_SET_UNID1_EN(val, 0x1);
	val = IOHC_USB_QOS_CTL_SET_UNID1_PRI(val, 0x0);
	val = IOHC_USB_QOS_CTL_SET_UNID1_ID(val, 0x30);
	val = IOHC_USB_QOS_CTL_SET_UNID0_EN(val, 0x1);
	val = IOHC_USB_QOS_CTL_SET_UNID0_PRI(val, 0x0);
	val = IOHC_USB_QOS_CTL_SET_UNID0_ID(val, 0x2f);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

static int
milan_fabric_init_arbitration_nbif(milan_nbif_t *nbif, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_nbif_reg(nbif, D_NBIF_GMI_WRR_WEIGHT2, 0);
	milan_nbif_write(nbif, reg, NBIF_GMI_WRR_WEIGHTn_VAL);
	reg = milan_nbif_reg(nbif, D_NBIF_GMI_WRR_WEIGHT3, 0);
	milan_nbif_write(nbif, reg, NBIF_GMI_WRR_WEIGHTn_VAL);

	reg = milan_nbif_reg(nbif, D_NBIF_BIFC_MISC_CTL0, 0);
	val = milan_nbif_read(nbif, reg);
	val = NBIF_BIFC_MISC_CTL0_SET_PME_TURNOFF(val,
	    NBIF_BIFC_MISC_CTL0_PME_TURNOFF_FW);
	milan_nbif_write(nbif, reg, val);

	return (0);
}

/*
 * This sets up a bunch of hysteresis and port controls around the SDP, DMA
 * actions, and ClkReq. In general, these values are what we're told to set them
 * to in the PPR. Note, there is no need to change
 * IOAGR::IOAGR_SDP_PORT_CONTROL, which is why it is missing. The SDPMUX does
 * not have an early wake up register.
 */
static int
milan_fabric_init_sdp_control(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_ioms_reg(ioms, D_IOHC_SDP_PORT_CTL, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_SDP_PORT_CTL_SET_PORT_HYSTERESIS(val, 0xff);
	milan_ioms_write(ioms, reg, val);

	reg = milan_ioms_reg(ioms, D_IOHC_SDP_EARLY_WAKE_UP, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_SDP_EARLY_WAKE_UP_SET_HOST_ENABLE(val, 0xffff);
	val = IOHC_SDP_EARLY_WAKE_UP_SET_DMA_ENABLE(val, 0x1);
	milan_ioms_write(ioms, reg, val);

	reg = milan_ioms_reg(ioms, D_IOAGR_EARLY_WAKE_UP, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOAGR_EARLY_WAKE_UP_SET_DMA_ENABLE(val, 0x1);
	milan_ioms_write(ioms, reg, val);

	reg = milan_ioms_reg(ioms, D_SDPMUX_SDP_PORT_CTL, 0);
	val = milan_ioms_read(ioms, reg);
	val = SDPMUX_SDP_PORT_CTL_SET_HOST_ENABLE(val, 0xffff);
	val = SDPMUX_SDP_PORT_CTL_SET_DMA_ENABLE(val, 0x1);
	val = SDPMUX_SDP_PORT_CTL_SET_PORT_HYSTERESIS(val, 0xff);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * XXX This bit of initialization is both strange and not very well documented.
 * This is a bit weird where by we always set this on nbif0 across all IOMS
 * instances; however, we only do it on NBIF1 for IOMS 0/1. Not clear why that
 * is. There are a bunch of things that don't quite make sense about being
 * specific to the syshub when generally we expect the one we care about to
 * actually be on IOMS 3.
 */
static int
milan_fabric_init_nbif_syshub_dma(milan_nbif_t *nbif, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	/*
	 * This register, like all SYSHUBMM registers, has no instance on NBIF2.
	 */
	if (nbif->mn_nbifno > 1 ||
	    (nbif->mn_nbifno > 0 && nbif->mn_ioms->mio_num > 1)) {
		return (0);
	}
	reg = milan_nbif_reg(nbif, D_NBIF_ALT_BGEN_BYP_SOC, 0);
	val = milan_nbif_read(nbif, reg);
	val = NBIF_ALT_BGEN_BYP_SOC_SET_DMA_SW0(val, 1);
	milan_nbif_write(nbif, reg, val);
	return (0);
}

/*
 * We need to initialize each IOAPIC as there is one per IOMS. First we
 * initialize the interrupt routing table. This is used to mux the various
 * legacy INTx interrupts and the bridge's interrupt to a given location. This
 * follow from the PPR.
 *
 * After that we need to go through and program the feature register for the
 * IOAPIC and its address. Because there is one IOAPIC per IOMS, one has to be
 * elected the primary and the rest, secondary. This is done based on which IOMS
 * has the FCH.
 */
static int
milan_fabric_init_ioapic(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	ASSERT3U(ARRAY_SIZE(milan_ioapic_routes), ==, IOAPIC_NROUTES);

	for (uint_t i = 0; i < ARRAY_SIZE(milan_ioapic_routes); i++) {
		smn_reg_t reg = milan_ioms_reg(ioms, D_IOAPIC_ROUTE, i);
		uint32_t route = milan_ioms_read(ioms, reg);

		route = IOAPIC_ROUTE_SET_BRIDGE_MAP(route,
		    milan_ioapic_routes[i].mii_map);
		route = IOAPIC_ROUTE_SET_INTX_SWIZZLE(route,
		    milan_ioapic_routes[i].mii_swiz);
		route = IOAPIC_ROUTE_SET_INTX_GROUP(route,
		    milan_ioapic_routes[i].mii_group);

		milan_ioms_write(ioms, reg, route);
	}

	/*
	 * The address registers are in the IOHC while the feature registers are
	 * in the IOAPIC SMN space. To ensure that the other IOAPICs can't be
	 * enabled with reset addresses, we instead lock them. XXX Should we
	 * lock primary?
	 */
	reg = milan_ioms_reg(ioms, D_IOHC_IOAPIC_ADDR_HI, 0);
	val = milan_ioms_read(ioms, reg);
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0) {
		val = IOHC_IOAPIC_ADDR_HI_SET_ADDR(val,
		    bitx64(MILAN_PHYSADDR_IOHC_IOAPIC, 47, 32));
	} else {
		val = IOHC_IOAPIC_ADDR_HI_SET_ADDR(val, 0);
	}
	milan_ioms_write(ioms, reg, val);

	reg = milan_ioms_reg(ioms, D_IOHC_IOAPIC_ADDR_LO, 0);
	val = milan_ioms_read(ioms, reg);
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0) {
		val = IOHC_IOAPIC_ADDR_LO_SET_ADDR(val,
		    bitx64(MILAN_PHYSADDR_IOHC_IOAPIC, 31, 8));
		val = IOHC_IOAPIC_ADDR_LO_SET_LOCK(val, 0);
		val = IOHC_IOAPIC_ADDR_LO_SET_EN(val, 1);
	} else {
		val = IOHC_IOAPIC_ADDR_LO_SET_ADDR(val, 0);
		val = IOHC_IOAPIC_ADDR_LO_SET_LOCK(val, 1);
		val = IOHC_IOAPIC_ADDR_LO_SET_EN(val, 0);
	}
	milan_ioms_write(ioms, reg, val);

	/*
	 * Every IOAPIC requires that we enable 8-bit addressing and that it be
	 * able to generate interrupts to the FCH. The most important bit here
	 * is the secondary bit which determines whether or not this IOAPIC is
	 * subordinate to another.
	 */
	reg = milan_ioms_reg(ioms, D_IOAPIC_FEATURES, 0);
	val = milan_ioms_read(ioms, reg);
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0) {
		val = IOAPIC_FEATURES_SET_SECONDARY(val, 0);
	} else {
		val = IOAPIC_FEATURES_SET_SECONDARY(val, 1);
	}
	val = IOAPIC_FEATURES_SET_FCH(val, 1);
	val = IOAPIC_FEATURES_SET_ID_EXT(val, 1);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * Each IOHC has registers that can further constraion what type of PCI bus
 * numbers the IOHC itself is expecting to reply to. As such, we program each
 * IOHC with its primary bus number and enable this.
 */
static int
milan_fabric_init_bus_num(milan_ioms_t *ioms, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_ioms_reg(ioms, D_IOHC_BUS_NUM_CTL, 0);
	val = milan_ioms_read(ioms, reg);
	val = IOHC_BUS_NUM_CTL_SET_EN(val, 1);
	val = IOHC_BUS_NUM_CTL_SET_BUS(val, ioms->mio_pci_busno);
	milan_ioms_write(ioms, reg, val);

	return (0);
}

/*
 * Go through and configure and set up devices and functions. In particular we
 * need to go through and set up the following:
 *
 *  o Strap bits that determine whether or not the function is enabled
 *  o Enabling the interrupts of corresponding functions
 *  o Setting up specific PCI device straps around multi-function, FLR, poison
 *    control, TPH settings, etc.
 *
 * XXX For getting to PCIe faster and since we're not going to use these, and
 * they're all disabled, for the moment we just ignore the straps that aren't
 * related to interrupts, enables, and cfg comps.
 */
static int
milan_fabric_init_nbif_dev_straps(milan_nbif_t *nbif, void *arg)
{
	smn_reg_t reg;
	uint32_t intr;

	reg = milan_nbif_reg(nbif, D_NBIF_INTR_LINE_EN, 0);
	intr = milan_nbif_read(nbif, reg);
	for (uint8_t funcno = 0; funcno < nbif->mn_nfuncs; funcno++) {
		smn_reg_t strapreg;
		uint32_t strap;
		milan_nbif_func_t *func = &nbif->mn_funcs[funcno];

		/*
		 * This indicates that we have a dummy function or similar. In
		 * which case there's not much to do here, the system defaults
		 * are generally what we want. XXX Kind of sort of. Not true
		 * over time.
		 */
		if ((func->mne_flags & MILAN_NBIF_F_NO_CONFIG) != 0) {
			continue;
		}

		strapreg = milan_nbif_func_reg(func, D_NBIF_FUNC_STRAP0);
		strap = milan_nbif_func_read(func, strapreg);

		if ((func->mne_flags & MILAN_NBIF_F_ENABLED) != 0) {
			strap = NBIF_FUNC_STRAP0_SET_EXIST(strap, 1);
			intr = NBIF_INTR_LINE_EN_SET_I(intr,
			    func->mne_dev, func->mne_func, 1);

			/*
			 * Strap enabled SATA devices to what AMD asks for.
			 */
			if (func->mne_type == MILAN_NBIF_T_SATA) {
				strap = NBIF_FUNC_STRAP0_SET_MAJ_REV(strap, 7);
				strap = NBIF_FUNC_STRAP0_SET_MIN_REV(strap, 1);
			}
		} else {
			strap = NBIF_FUNC_STRAP0_SET_EXIST(strap, 0);
			intr = NBIF_INTR_LINE_EN_SET_I(intr,
			    func->mne_dev, func->mne_func, 0);
		}

		milan_nbif_func_write(func, strapreg, strap);
	}

	milan_nbif_write(nbif, reg, intr);

	/*
	 * Each nBIF has up to three devices on them, though not all of them
	 * seem to be used. However, it's suggested that we enable completion
	 * timeouts on all three device straps.
	 */
	for (uint8_t devno = 0; devno < MILAN_NBIF_MAX_DEVS; devno++) {
		smn_reg_t reg;
		uint32_t val;

		reg = milan_nbif_reg(nbif, D_NBIF_PORT_STRAP3, devno);
		val = milan_nbif_read(nbif, reg);
		val = NBIF_PORT_STRAP3_SET_COMP_TO(val, 1);
		milan_nbif_write(nbif, reg, val);
	}

	return (0);
}

/*
 * There are five bridges that are associated with the NBIFs. One on NBIF0,
 * three on NBIF1, and the last on the SB. There is nothing on NBIF 2 which is
 * why we don't use the nbif iterator, though this is somewhat uglier. The
 * default expectation of the system is that the CRS bit is set. XXX these have
 * all been left enabled for now.
 */
static int
milan_fabric_init_nbif_bridge(milan_ioms_t *ioms, void *arg)
{
	uint32_t val;
	const smn_reg_t smn_regs[5] = {
		IOHCDEV_NBIF_BRIDGE_CTL(ioms->mio_num, 0, 0),
		IOHCDEV_NBIF_BRIDGE_CTL(ioms->mio_num, 1, 0),
		IOHCDEV_NBIF_BRIDGE_CTL(ioms->mio_num, 1, 1),
		IOHCDEV_NBIF_BRIDGE_CTL(ioms->mio_num, 1, 2),
		IOHCDEV_SB_BRIDGE_CTL(ioms->mio_num)
	};

	for (uint_t i = 0; i < ARRAY_SIZE(smn_regs); i++) {
		val = milan_ioms_read(ioms, smn_regs[i]);
		val = IOHCDEV_BRIDGE_CTL_SET_CRS_ENABLE(val, 1);
		milan_ioms_write(ioms, smn_regs[i], val);
	}
	return (0);
}

static int
milan_dxio_init(milan_iodie_t *iodie, void *arg)
{
	milan_soc_t *soc = iodie->mi_soc;

	/*
	 * XXX Ethanol-X has a BMC hanging off socket 0, so on that platform we
	 * need to reload the state machine because it's already been used to do
	 * what the ABL calls early link training.  Not doing this results in
	 * this failure when we run dxio_load: DXIO Load Data Failed: Heap: 0x6,
	 * PA: 0x7ff98000, Len: 0x13e, SMU 0x1, DXIO: 0x2
	 *
	 * There's a catch: the dependency here is specifically that this is
	 * required on any socket where early link training has been done, which
	 * is controlled by an APCB token -- it's not board-dependent, although
	 * in practice the correct value for the token is permanently fixed for
	 * each board.  If the SM reload is run on a socket other than the one
	 * that has been marked for this use in the APCB, it will fail and at
	 * present that will result in not doing the rest of DXIO setup and then
	 * panicking in PCIe setup.
	 *
	 * Historically Gimlet's APCB was basically the same as Ethanol-X's,
	 * which included doing (or trying, since there's nothing connected)
	 * early link training.  That necessitated always running SM RELOAD on
	 * socket 0.  That option is set incorrectly for Gimlet, though, which
	 * means this should really depend on milan_board_type(); when it does,
	 * there will be an APCB-unix flag day.  We probably want to see if we
	 * can do better by figuring out whether this is needed on socket 0, 1,
	 * or neither.
	 */
	if (soc->ms_socno == 0 && !milan_dxio_rpc_sm_reload(iodie)) {
		return (1);
	}


	if (!milan_dxio_rpc_init(iodie)) {
		return (1);
	}

	/*
	 * XXX These 0x4f values were kind of given to us. Do better than a
	 * magic constant, rm.
	 */
	if (!milan_dxio_rpc_clock_gating(iodie, 0x4f, 0x4f)) {
		return (1);
	}

	/*
	 * Set up a few different variables in firmware. Best guesses is that we
	 * need MILAN_DXIO_VAR_PCIE_COMPL so we can get PCIe completions to
	 * actually happen, MILAN_DXIO_VAR_SLIP_INTERVAL is disabled, but I
	 * can't say why. XXX We should probably disable NTB hotplug because we
	 * don't have them just in case something changes here.
	 */
	if (!milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_PCIE_COMPL, 1) ||
	    !milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_SLIP_INTERVAL, 0)) {
		return (1);
	}

	/*
	 * This seems to configure behavior when the link is going down and
	 * power off. We explicitly ask for no delay. The latter argument is
	 * about disabling another command (which we don't use), but to keep
	 * firmware in its expected path we don't set that.  Older DXIO firmware
	 * doesn't support this so we skip it there.
	 */
	if (milan_dxio_version_at_least(iodie, 45, 682) &&
	    !milan_dxio_rpc_pcie_poweroff_config(iodie, 0, B_FALSE)) {
		return (1);
	}

	/*
	 * Next we set a couple of variables that are required for us to
	 * cause the state machine to pause after a couple of different stages
	 * and then also to indicate that we want to use the v1 ancillary data
	 * format.
	 */
	if (!milan_dxio_rpc_set_var(iodie, MLIAN_DXIO_VAR_RET_AFTER_MAP, 1) ||
	    !milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_RET_AFTER_CONF, 1) ||
	    !milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_ANCILLARY_V1, 1)) {
		return (1);
	}

	/*
	 * Here, it's worth calling out what we're not setting. One of which is
	 * MILAN_DXIO_VAR_MAP_EXACT_MATCH which ends up being used to cause
	 * the mapping phase to only work if there are exact matches. I believe
	 * this means that if a device has more lanes then the configured port,
	 * it wouldn't link up, which generally speaking isn't something we want
	 * to do. Similarly, since there is no S3 support here, no need to
	 * change the save and restore mode with MILAN_DXIO_VAR_S3_MODE.
	 *
	 * From here, we do want to set MILAN_DXIO_VAR_SKIP_PSP, because the PSP
	 * really doesn't need to do anything with us. We do want to enable
	 * MILAN_DXIO_VAR_PHY_PROG so the dxio engine can properly configure
	 * things.
	 *
	 * XXX Should we gamble and set things that aren't unconditionally set
	 * so we don't rely on hw defaults?
	 */
	if (!milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_PHY_PROG, 1) ||
	    !milan_dxio_rpc_set_var(iodie, MILAN_DXIO_VAR_SKIP_PSP, 1)) {
		return (0);
	}

	return (0);
}

/*
 * Here we need to assemble data for the system we're actually on. XXX Right now
 * we're just assuming we're Ethanol-X and only leveraging ancillary data from
 * the PSP.
 */
static int
milan_dxio_plat_data(milan_iodie_t *iodie, void *arg)
{
	ddi_dma_attr_t attr;
	size_t engn_size;
	pfn_t pfn;
	milan_dxio_config_t *conf = &iodie->mi_dxio_conf;
	milan_soc_t *soc = iodie->mi_soc;
	const zen_dxio_platform_t *source_data;
	zen_dxio_anc_data_t *anc;
	const void *phy_override;
	size_t phy_len;
	int err;

	/*
	 * XXX Figure out how to best not hardcode Ethanol. Realistically
	 * probably an SP boot property.
	 */
	if (milan_board_type(soc->ms_fabric) == MBT_ETHANOL) {
		if (soc->ms_socno == 0) {
			source_data = &ethanolx_engine_s0;
		} else {
			source_data = &ethanolx_engine_s1;
		}
	} else {
		VERIFY3U(soc->ms_socno, ==, 0);
		source_data = &gimlet_engine;
	}

	engn_size = sizeof (zen_dxio_platform_t) +
	    source_data->zdp_nengines * sizeof (zen_dxio_engine_t);
	VERIFY3U(engn_size, <=, MMU_PAGESIZE);
	conf->mdc_conf_len = engn_size;

	milan_smu_dma_attr(&attr);
	conf->mdc_alloc_len = MMU_PAGESIZE;
	conf->mdc_conf = contig_alloc(MMU_PAGESIZE, &attr, MMU_PAGESIZE, 1);
	bzero(conf->mdc_conf, MMU_PAGESIZE);

	pfn = hat_getpfnum(kas.a_hat, (caddr_t)conf->mdc_conf);
	conf->mdc_pa = mmu_ptob((uint64_t)pfn);

	bcopy(source_data, conf->mdc_conf, engn_size);

	/*
	 * We need to account for an extra 8 bytes, surprisingly. It's a good
	 * thing we have a page. Note, dxio wants this in uint32_t units. We do
	 * that when we make the RPC call. Finally, we want to make sure that if
	 * we're in an incomplete word, that we account for that in the length.
	 */
	conf->mdc_conf_len += 8;
	conf->mdc_conf_len = P2ROUNDUP(conf->mdc_conf_len, 4);

	phy_override = milan_apob_find(MILAN_APOB_GROUP_FABRIC,
	    MILAN_APOB_FABRIC_PHY_OVERRIDE, 0, &phy_len, &err);
	if (phy_override == NULL) {
		if (err == ENOENT) {
			return (0);
		}

		cmn_err(CE_WARN, "failed to find phy override table in APOB: "
		    "0x%x", err);
		return (1);
	}

	conf->mdc_anc = contig_alloc(MMU_PAGESIZE, &attr, MMU_PAGESIZE, 1);
	bzero(conf->mdc_anc, MMU_PAGESIZE);

	pfn = hat_getpfnum(kas.a_hat, (caddr_t)conf->mdc_anc);
	conf->mdc_anc_pa = mmu_ptob((uint64_t)pfn);

	/*
	 * First we need to program the initial descriptor. Its type is one of
	 * the Heap types. Yes, this is different from the sub data payloads
	 * that we use. Yes, this is different from the way that the engine
	 * config data is laid out. Each entry has the amount of space they take
	 * up. Confusingly, it seems that the top entry does not include the
	 * space its header takes up. However, the subsequent payloads do.
	 */
	anc = conf->mdc_anc;
	anc->zdad_type = MILAN_DXIO_HEAP_ANCILLARY;
	anc->zdad_vers = DXIO_ANCILLARY_VERSION;
	anc->zdad_nu32s = (sizeof (zen_dxio_anc_data_t) + phy_len) >> 2;
	anc++;
	anc->zdad_type = ZEN_DXIO_ANCILLARY_T_PHY;
	anc->zdad_vers = DXIO_ANCILLARY_PAYLOAD_VERSION;
	anc->zdad_nu32s = (sizeof (zen_dxio_anc_data_t) + phy_len) >> 2;
	anc++;
	bcopy(phy_override, anc, phy_len);
	conf->mdc_anc_len = phy_len + 2 * sizeof (zen_dxio_anc_data_t);

	return (0);
}

static int
milan_dxio_load_data(milan_iodie_t *iodie, void *arg)
{
	milan_dxio_config_t *conf = &iodie->mi_dxio_conf;

	/*
	 * Begin by loading the NULL capabilities before we load any data heaps.
	 */
	if (!milan_dxio_rpc_load_caps(iodie)) {
		return (1);
	}

	if (conf->mdc_anc != NULL && !milan_dxio_rpc_load_data(iodie,
	    MILAN_DXIO_HEAP_ANCILLARY, conf->mdc_anc_pa, conf->mdc_anc_len,
	    0)) {
		return (1);
	}

	/*
	 * It seems that we're required to load both of these heaps with the
	 * mystery bit set to one. It's called that because we don't know what
	 * it does; however, these heaps are always loaded with no data, even
	 * though ancillary is skipped if there is none.
	 */
	if (!milan_dxio_rpc_load_data(iodie, MILAN_DXIO_HEAP_MACPCS,
	    0, 0, 1) ||
	    !milan_dxio_rpc_load_data(iodie, MILAN_DXIO_HEAP_GPIO, 0, 0, 1)) {
		return (1);
	}

	/*
	 * Load our real data!
	 */
	if (!milan_dxio_rpc_load_data(iodie, MILAN_DXIO_HEAP_ENGINE_CONFIG,
	    conf->mdc_pa, conf->mdc_conf_len, 0)) {
		return (1);
	}

	return (0);
}

static int
milan_dxio_more_conf(milan_iodie_t *iodie, void *arg)
{
	/*
	 * Note, here we might use milan_dxio_rpc_conf_training() if we want to
	 * override any of the properties there. But the defaults in DXIO
	 * firmware seem to be used by default. We also might apply various
	 * workarounds that we don't seem to need to
	 * (MILAN_DXIO_RT_SET_CONF_DXIO_WA, MILAN_DXIO_RT_SET_CONF_SPC_WA,
	 * MILAN_DXIO_RT_SET_CONF_FC_CRED_WA_DIS).
	 */

	/*
	 * XXX Do we care about any of the following:
	 *    o MILAN_DXIO_RT_SET_CONF_TX_CLOCK
	 *    o MILAN_DXIO_RT_SET_CONF_SRNS
	 *    o MILAN_DXIO_RT_SET_CONF_DLF_WA_DIS
	 *
	 * I wonder why we don't enable MILAN_DXIO_RT_SET_CONF_CE_SRAM_ECC in
	 * the old world.
	 */

	/*
	 * This is set to 1 by default because we want 'latency behaviour' not
	 * 'improved latency'.
	 */
	if (!milan_dxio_rpc_misc_rt_conf(iodie,
	    MILAN_DXIO_RT_SET_CONF_TX_FIFO_MODE, 1)) {
		return (1);
	}

	return (0);
}


/*
 * Given all of the engines on an I/O die, try and map each one to a
 * corresponding IOMS and bridge. We only care about an engine if it is a PCIe
 * engine. Note, because each I/O die is processed independently, this only
 * operates on a single I/O die.
 */
static boolean_t
milan_dxio_map_engines(milan_fabric_t *fabric, milan_iodie_t *iodie)
{
	boolean_t ret = B_TRUE;
	zen_dxio_platform_t *plat = iodie->mi_dxio_conf.mdc_conf;

	for (uint_t i = 0; i < plat->zdp_nengines; i++) {
		zen_dxio_engine_t *en = &plat->zdp_engines[i];
		milan_pcie_port_t *port;
		milan_pcie_bridge_t *bridge;
		uint8_t bridgeno;

		if (en->zde_type != DXIO_ENGINE_PCIE)
			continue;


		port = milan_fabric_find_port_by_lanes(iodie,
		    en->zde_start_lane, en->zde_end_lane);
		if (port == NULL) {
			cmn_err(CE_WARN, "failed to map engine %u [%u, %u] to "
			    "a PCIe port", i, en->zde_start_lane,
			    en->zde_end_lane);
			ret = B_FALSE;
			continue;
		}

		bridgeno = en->zde_config.zdc_pcie.zdcp_mac_port_id;
		if (bridgeno >= port->mpp_nbridges) {
			cmn_err(CE_WARN, "failed to map engine %u [%u, %u] to "
			    "a PCIe bridge: found nbridges %u, but mapped to "
			    "bridge %u",  i, en->zde_start_lane,
			    en->zde_end_lane, port->mpp_nbridges, bridgeno);
			ret = B_FALSE;
			continue;
		}

		bridge = &port->mpp_bridges[bridgeno];
		if (bridge->mpb_engine != NULL) {
			cmn_err(CE_WARN, "engine %u [%u, %u] mapped to "
			    "bridge %u, which already has an engine [%u, %u]",
			    i, en->zde_start_lane, en->zde_end_lane,
			    port->mpp_nbridges,
			    bridge->mpb_engine->zde_start_lane,
			    bridge->mpb_engine->zde_end_lane);
			ret = B_FALSE;
			continue;
		}

		bridge->mpb_flags |= MILAN_PCIE_BRIDGE_F_MAPPED;
		bridge->mpb_engine = en;
		port->mpp_flags |= MILAN_PCIE_PORT_F_USED;
		if (en->zde_config.zdc_pcie.zdcp_caps.zdlc_hp !=
		    DXIO_HOTPLUG_T_DISABLED) {
			port->mpp_flags |= MILAN_PCIE_PORT_F_HAS_HOTPLUG;
		}
	}

	return (ret);
}

/*
 * These PCIe straps need to be set after mapping is done, but before link
 * training has started. While we do not understand in detail what all of these
 * registers do, we've split this broadly into 2 categories:
 * 1) Straps where:
 *     a) the defaults in hardware seem to be reasonable given our (sometimes
 *     limited) understanding of their function
 *     b) are not features/parameters that we currently care specifically about
 *     one way or the other
 *     c) and we are currently ok with the defaults changing out from underneath
 *     us on different hardware revisions unless proven otherwise.
 * or 2) where:
 *     a) We care specifically about a feature enough to ensure that it is set
 *     (e.g. AERs) or purposefully disabled (e.g. I2C_DBG_EN)
 *     b) We are not ok with these changing based on potentially different
 *     defaults set in different hardware revisions
 * For 1), we've chosen to leave them based on whatever the hardware has chosen
 * as the default, while all the straps detailed underneath fall into category
 * 2. Note that this list is by no means definitive, and will almost certainly
 * change as our understanding of what we require from the hardware evolves.
 */
typedef struct milan_pcie_strap_setting {
	uint32_t		strap_reg;
	uint32_t		strap_data;
} milan_pcie_strap_setting_t;

/*
 * PCIe Straps that we unconditionally set to 1
 */
static const uint32_t milan_pcie_strap_enable[] = {
	MILAN_STRAP_PCIE_MSI_EN,
	MILAN_STRAP_PCIE_AER_EN,
	MILAN_STRAP_PCIE_GEN2_COMP,
	/* We want completion timeouts */
	MILAN_STRAP_PCIE_CPL_TO_EN,
	MILAN_STRAP_PCIE_TPH_EN,
	MILAN_STRAP_PCIE_MULTI_FUNC_EN,
	MILAN_STRAP_PCIE_DPC_EN,
	MILAN_STRAP_PCIE_ARI_EN,
	MILAN_STRAP_PCIE_PL_16G_EN,
	MILAN_STRAP_PCIE_LANE_MARGIN_EN,
	MILAN_STRAP_PCIE_LTR_SUP,
	MILAN_STRAP_PCIE_LINK_BW_NOTIF_SUP,
	MILAN_STRAP_PCIE_GEN3_1_FEAT_EN,
	MILAN_STRAP_PCIE_GEN4_FEAT_EN,
	MILAN_STRAP_PCIE_ECRC_GEN_EN,
	MILAN_STRAP_PCIE_ECRC_CHECK_EN,
	MILAN_STRAP_PCIE_CPL_ABORT_ERR_EN,
	MILAN_STRAP_PCIE_INT_ERR_EN,
	MILAN_STRAP_PCIE_RXP_ACC_FULL_DIS,

	/* ACS straps */
	MILAN_STRAP_PCIE_ACS_EN,
	MILAN_STRAP_PCIE_ACS_SRC_VALID,
	MILAN_STRAP_PCIE_ACS_TRANS_BLOCK,
	MILAN_STRAP_PCIE_ACS_DIRECT_TRANS_P2P,
	MILAN_STRAP_PCIE_ACS_P2P_CPL_REDIR,
	MILAN_STRAP_PCIE_ACS_P2P_REQ_RDIR,
	MILAN_STRAP_PCIE_ACS_UPSTREAM_FWD,
};

/*
 * PCIe Straps that we unconditionally set to 0
 * These are generally debug and test settings that are usually not a good idea
 * in my experience to allow accidental enablement.
 */
static const uint32_t milan_pcie_strap_disable[] = {
	MILAN_STRAP_PCIE_I2C_DBG_EN,
	MILAN_STRAP_PCIE_DEBUG_RXP,
	MILAN_STRAP_PCIE_NO_DEASSERT_RX_EN_TEST,
	MILAN_STRAP_PCIE_ERR_REPORT_DIS,
	MILAN_STRAP_PCIE_TX_TEST_ALL,
	MILAN_STRAP_PCIE_MCAST_EN,
};

/*
 * PCIe Straps that have other values.
 */
static const milan_pcie_strap_setting_t milan_pcie_strap_settings[] = {
	{
	    .strap_reg = MILAN_STRAP_PCIE_EQ_DS_RX_PRESET_HINT,
	    .strap_data = MILAN_STRAP_PCIE_RX_PRESET_9DB,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_EQ_US_RX_PRESET_HINT,
	    .strap_data = MILAN_STRAP_PCIE_RX_PRESET_9DB,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_EQ_DS_TX_PRESET,
	    .strap_data = MILAN_STRAP_PCIE_TX_PRESET_7,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_EQ_US_TX_PRESET,
	    .strap_data = MILAN_STRAP_PCIE_TX_PRESET_7,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_16GT_EQ_DS_TX_PRESET,
	    .strap_data = MILAN_STRAP_PCIE_TX_PRESET_7,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_16GT_EQ_US_TX_PRESET,
	    .strap_data = MILAN_STRAP_PCIE_TX_PRESET_5,
	},
};

/*
 * Strap settings that only apply to Ethanol
 */
static const milan_pcie_strap_setting_t milan_pcie_strap_ethanol_settings[] = {
};

/*
 * Strap settings that only apply to Gimlet
 */
static const milan_pcie_strap_setting_t milan_pcie_strap_gimlet_settings[] = {
	{
	    .strap_reg = MILAN_STRAP_PCIE_SUBVID,
	    .strap_data = PCI_VENDOR_ID_OXIDE,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_SUBDID,
	    .strap_data = MILAN_STRAP_PCIE_SUBDID_BRIDGE,
	},
};

/*
 * PCIe Straps that exist on a per-bridge level.
 */
static const milan_pcie_strap_setting_t milan_pcie_bridge_settings[] = {
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_EXT_TAG_SUP,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_E2E_TLP_PREFIX_EN,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_10B_TAG_CMPL_SUP,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_10B_TAG_REQ_SUP,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_TCOMMONMODE_TIME,
	    .strap_data = 0xa,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_TPON_SCALE,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_TPON_VALUE,
	    .strap_data = 0xf,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_DLF_SUP,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_DLF_EXCHANGE_EN,
	    .strap_data = 0x1,
	},
	{
	    .strap_reg = MILAN_STRAP_PCIE_P_FOM_TIME,
	    .strap_data = MILAN_STRAP_PCIE_P_FOM_300US,
	},
};

static void
milan_fabric_write_pcie_strap(milan_pcie_port_t *port,
    const uint32_t reg, const uint32_t data)
{
	smn_reg_t a_reg, d_reg;

	a_reg = milan_pcie_port_reg(port, D_PCIE_RSMU_STRAP_ADDR);
	d_reg = milan_pcie_port_reg(port, D_PCIE_RSMU_STRAP_DATA);

	mutex_enter(&port->mpp_strap_lock);
	milan_pcie_port_write(port, a_reg, MILAN_STRAP_PCIE_ADDR_UPPER + reg);
	milan_pcie_port_write(port, d_reg, data);
	mutex_exit(&port->mpp_strap_lock);
}

/*
 * Here we set up all the straps for PCIe features that we care about and want
 * advertised as capabilities. Note that we do not enforce any order between the
 * straps. It is our understanding that the straps themselves do not kick off
 * any change, but instead another stage (presumably before link training)
 * initializes the read of all these straps in one go.
 * Currently, we set these straps on all ports and all bridges regardless of
 * whether they are used, though this may be changed if it proves problematic.
 */
static int
milan_fabric_init_pcie_straps(milan_pcie_port_t *port, void *arg)
{
	const milan_fabric_t *fabric =
	    port->mpp_ioms->mio_iodie->mi_soc->ms_fabric;
	for (uint_t i = 0; i < ARRAY_SIZE(milan_pcie_strap_enable); i++) {
		milan_fabric_write_pcie_strap(port,
		    milan_pcie_strap_enable[i], 0x1);
	}
	for (uint_t i = 0; i < ARRAY_SIZE(milan_pcie_strap_disable); i++) {
		milan_fabric_write_pcie_strap(port,
		    milan_pcie_strap_disable[i], 0x0);
	}
	for (uint_t i = 0; i < ARRAY_SIZE(milan_pcie_strap_settings); i++) {
		const milan_pcie_strap_setting_t *strap =
		    &milan_pcie_strap_settings[i];

		milan_fabric_write_pcie_strap(port,
		    strap->strap_reg, strap->strap_data);
	}

	/* Handle Special case for DLF which needs to be set on non WAFL */
	if (port->mpp_portno != MILAN_IOMS_WAFL_PCIE_PORT) {
		milan_fabric_write_pcie_strap(port,
		    MILAN_STRAP_PCIE_DLF_EN, 1);
	}

	/* Handle board specific straps */
	const milan_pcie_strap_setting_t *board_list;
	int array_size;
	if (milan_board_type(fabric) == MBT_ETHANOL) {
		board_list = milan_pcie_strap_ethanol_settings;
		array_size = ARRAY_SIZE(milan_pcie_strap_ethanol_settings);
	} else {
		board_list = milan_pcie_strap_gimlet_settings;
		array_size = ARRAY_SIZE(milan_pcie_strap_gimlet_settings);
	}
	for (uint_t i = 0; i < array_size; i++) {
		const milan_pcie_strap_setting_t *strap =
		    &board_list[i];

		milan_fabric_write_pcie_strap(port,
		    strap->strap_reg, strap->strap_data);
	}

	/* Handle per bridge initialization */
	for (uint_t i = 0; i < ARRAY_SIZE(milan_pcie_bridge_settings); i++) {
		const milan_pcie_strap_setting_t *strap =
		    &milan_pcie_bridge_settings[i];
		for (uint_t j = 0; j < port->mpp_nbridges; j++) {
			milan_fabric_write_pcie_strap(port,
			    strap->strap_reg +
			    (j * MILAN_STRAP_PCIE_NUM_PER_BRIDGE),
			    strap->strap_data);
		}
	}

	return (0);
}

/*
 * Helper function for PCIe reset (PERST_L) deassertion.  Used only on Ethanol-X
 * during the DXIO state machine execution.  GPIOs are in two primary banks;
 * those above 255 are part of a different register block.
 */
static void
milan_perst_deassert(milan_iodie_t *iodie, uint16_t gpio)
{
	smn_reg_t reg;
	uint32_t val;

	if (gpio < 256) {
		reg = milan_iodie_reg(iodie, D_FCH_GPIO_STD, gpio);
	} else {
		reg = milan_iodie_reg(iodie, D_FCH_RMTGPIO_STD, gpio - 256);
	}

	val = FCH_GPIO_STD_SET_OUTPUT_EN(0, 1);
	val = FCH_GPIO_STD_SET_OUTPUT_VAL(val,
	    FCH_GPIO_STD_OUTPUT_VAL_ASSERT);
	val = FCH_GPIO_STD_SET_STRENGTH(val,
	    FCH_GPIO_STD_STRENGTH_40OHM);
	milan_iodie_write(iodie, reg, val);
}

/*
 * Here we are, it's time to actually kick off the state machine that we've
 * wanted to do.
 */
static int
milan_dxio_state_machine(milan_iodie_t *iodie, void *arg)
{
	milan_soc_t *soc = iodie->mi_soc;
	milan_fabric_t *fabric = soc->ms_fabric;

	if (!milan_dxio_rpc_sm_start(iodie)) {
		return (1);
	}

	for (;;) {
		milan_dxio_reply_t reply = { 0 };

		if (!milan_dxio_rpc_sm_getstate(iodie, &reply)) {
			return (1);
		}

		switch (reply.mds_type) {
		case MILAN_DXIO_DATA_TYPE_SM:
			cmn_err(CE_WARN, "Socket %u SM 0x%x->0x%x",
			    soc->ms_socno, iodie->mi_state, reply.mds_arg0);
			iodie->mi_state = reply.mds_arg0;
			switch (iodie->mi_state) {
			/*
			 * The mapped state indicates that the engines and lanes
			 * that we have provided in our DXIO configuration have
			 * been mapped back to the actual set of PCIe ports on
			 * the IOMS (e.g. G0, P0) and specific bridge indexes
			 * within that port group. The very first thing we need
			 * to do here is to figure out what actually has been
			 * mapped to what and update what ports are actually
			 * being used by devices or not.
			 */
			case MILAN_DXIO_SM_MAPPED:
				if (!milan_dxio_rpc_retrieve_engine(iodie)) {
					return (1);
				}

				if (!milan_dxio_map_engines(fabric, iodie)) {
					cmn_err(CE_WARN, "failed to map all "
					    "DXIO engines to devices in the "
					    "milan_fabric_t");
					return (1);
				}
				cmn_err(CE_WARN, "XXX skipping a ton of mapped "
				    "stuff");
				/*
				 * Now that we have the mapping done, we set up
				 * the straps for PCIe.
				 */
				(void) milan_fabric_walk_pcie_port(fabric,
				    milan_fabric_init_pcie_straps, NULL);

				cmn_err(CE_NOTE, "Finished writing PCIe "
				    "straps.");
				break;
			case MILAN_DXIO_SM_CONFIGURED:
				cmn_err(CE_WARN, "XXX skipping a ton of "
				    "configured stuff");
				break;
			case MILAN_DXIO_SM_DONE:
				/*
				 * We made it. Somehow we're done!
				 */
				cmn_err(CE_WARN, "we're out of here");
				goto done;
			default:
				/*
				 * For most states there doesn't seem to be much
				 * to do. So for now we just leave the default
				 * case to continue and proceed to the next
				 * state machine state.
				 */
				break;
			}
			break;
		case MILAN_DXIO_DATA_TYPE_RESET:
			cmn_err(CE_WARN, "let's go deasserting: %x, %x",
			    reply.mds_arg0, reply.mds_arg1);
			if (reply.mds_arg0 == 0) {
				cmn_err(CE_WARN, "Asked to set GPIO to zero, "
				    "which  would PERST. Nope. Continuing?");
				break;
			}

			if (milan_board_type(fabric) != MBT_ETHANOL)
				break;

			/*
			 * Release PERST manually on Ethanol-X which requires
			 * it.  PCIE_RSTn_L shares pins with the following
			 * GPIOs:
			 *
			 * FCH::GPIO::GPIO_26
			 * FCH::GPIO::GPIO_27
			 * FCH::RMTGPIO::GPIO_266
			 * FCH::RMTGPIO::GPIO_267
			 *
			 * If we were going to support this generically, these
			 * should probably be part of the board definition.
			 * They should also be DPIOs, but we probably can't use
			 * the DPIO subsystem itself yet.
			 *
			 * XXX The only other function on these pins is the PCIe
			 * reset itself.  Can we assume the mux is passing the
			 * GPIO function at this point?  If it's not, this will
			 * do nothing.
			 */
			milan_perst_deassert(iodie, 26);
			milan_perst_deassert(iodie, 27);
			milan_perst_deassert(iodie, 266);
			milan_perst_deassert(iodie, 267);
			break;
		case MILAN_DXIO_DATA_TYPE_NONE:
			cmn_err(CE_WARN, "Got the none data type... are we "
			    "actually done?");
			goto done;
		default:
			cmn_err(CE_WARN, "Got unexpected DXIO return type: "
			    "0x%x. Sorry, no PCIe for us on socket %u.",
			    reply.mds_type, soc->ms_socno);
			return (1);
		}

		if (!milan_dxio_rpc_sm_resume(iodie)) {
			return (1);
		}
	}

done:
	if (!milan_dxio_rpc_retrieve_engine(iodie)) {
		return (1);
	}

	return (0);
}

/*
 * Our purpose here is to set up memlist structures for use in tracking. Right
 * now we use the xmemlist feature, though having something that is backed by
 * kmem would make life easier; however, that will wait for the great memlist
 * merge that is likely not to happen anytime soon.
 */
static int
milan_fabric_init_memlists(milan_ioms_t *ioms, void *arg)
{
	ioms_memlists_t *imp = &ioms->mio_memlists;
	void *page = kmem_zalloc(MMU_PAGESIZE, KM_SLEEP);

	mutex_init(&imp->im_lock, NULL, MUTEX_DRIVER, NULL);
	xmemlist_free_block(&imp->im_pool, page, MMU_PAGESIZE);
	return (0);
}

/*
 * We want to walk the DF and record information about how PCI buses are routed.
 * We make an assumption here, which is that each DF instance has been
 * programmed the same way by the PSP/SMU (which if was not done would lead to
 * some chaos). As such, we end up using the first socket's df and its first
 * IOMS to figure this out.
 */
static void
milan_route_pci_bus(milan_fabric_t *fabric)
{
	milan_iodie_t *iodie = &fabric->mf_socs[0].ms_iodies[0];
	uint_t inst = iodie->mi_ioms[0].mio_comp_id;

	for (uint_t i = 0; i < DF_MAX_CFGMAP; i++) {
		int ret;
		milan_ioms_t *ioms;
		ioms_memlists_t *imp;
		uint32_t base, limit, dest;
		uint32_t val = milan_df_read32(iodie, inst, DF_CFGMAP_V2(i));

		/*
		 * If a configuration map entry doesn't have both read and write
		 * enabled, then we treat that as something that we should skip.
		 * There is no validity bit here, so this is the closest that we
		 * can come to.
		 */
		if (DF_CFGMAP_V2_GET_RE(val) == 0 ||
		    DF_CFGMAP_V2_GET_WE(val) == 0) {
			continue;
		}

		base = DF_CFGMAP_V2_GET_BUS_BASE(val);
		limit = DF_CFGMAP_V2_GET_BUS_LIMIT(val);
		dest = DF_CFGMAP_V3_GET_DEST_ID(val);

		ioms = milan_fabric_find_ioms(fabric, dest);
		if (ioms == NULL) {
			cmn_err(CE_WARN, "PCI Bus fabric rule %u [0x%x, 0x%x] "
			    "maps to unknown fabric id: 0x%x", i, base, limit,
			    dest);
			continue;
		}
		imp = &ioms->mio_memlists;

		if (base != ioms->mio_pci_busno) {
			cmn_err(CE_PANIC, "unexpected bus routing rule, rule "
			    "base 0x%x does not match destination base: 0x%x",
			    base, ioms->mio_pci_busno);
		}

		/*
		 * We assign the IOMS's PCI bus as used and all the remainin as
		 * available.
		 */
		ret = xmemlist_add_span(&imp->im_pool, base, 1,
		    &imp->im_bus_used, 0);
		VERIFY3S(ret, ==, MEML_SPANOP_OK);

		if (base == limit)
			continue;
		ret = xmemlist_add_span(&imp->im_pool, base + 1, limit - base,
		    &imp->im_bus_avail, 0);
		VERIFY3S(ret, ==, MEML_SPANOP_OK);
	}
}

typedef struct milan_route_io {
	uint32_t	mri_per_ioms;
	uint32_t	mri_next_base;
	uint32_t	mri_cur;
	uint32_t	mri_last_ioms;
	uint32_t	mri_bases[DF_MAX_IO_RULES];
	uint32_t	mri_limits[DF_MAX_IO_RULES];
	uint32_t	mri_dests[DF_MAX_IO_RULES];
} milan_route_io_t;

static int
milan_io_ports_allocate(milan_ioms_t *ioms, void *arg)
{
	int ret;
	milan_route_io_t *mri = arg;
	ioms_memlists_t *imp = &ioms->mio_memlists;
	uint32_t pci_base;

	/*
	 * The primary FCH (e.g. the IOMS that has the FCH on iodie 0) always
	 * has a base of zero so we can cover the legacy I/O ports.  That range
	 * is not available for PCI allocation, however.
	 */
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0 &&
	    (ioms->mio_iodie->mi_flags & MILAN_IODIE_F_PRIMARY) != 0) {
		mri->mri_bases[mri->mri_cur] = 0;
		pci_base = MILAN_IOPORT_COMPAT_SIZE;
	} else {
		pci_base = mri->mri_bases[mri->mri_cur] = mri->mri_next_base;
		mri->mri_next_base += mri->mri_per_ioms;

		mri->mri_last_ioms = mri->mri_cur;
	}

	mri->mri_limits[mri->mri_cur] = mri->mri_bases[mri->mri_cur] +
	    mri->mri_per_ioms - 1;
	mri->mri_dests[mri->mri_cur] = ioms->mio_fabric_id;

	/*
	 * We must always have some I/O port space available for PCI.  The PCI
	 * space must always be higher than any space reserved for generic/FCH
	 * use.  While this is ultimately due to the way the hardware works, the
	 * more important reason is that our memlist code below relies on it.
	 */
	ASSERT3U(mri->mri_limits[mri->mri_cur], >, pci_base);
	ASSERT3U(mri->mri_bases[mri->mri_cur], <=, pci_base);

	/*
	 * We purposefully assign all of the I/O ports here and not later on as
	 * we want to make sure that we don't end up recording the fact that
	 * someone has the rest of the ports that aren't available on x86.
	 * While there is some logic in pci_boot.c that attempts to avoid
	 * allocating the legacy/compatibility space port range to PCI
	 * endpoints, it's better to tell that code exactly what's really
	 * available and what isn't.  We also need to reserve the compatibility
	 * space for later allocation to FCH devices if the FCH driver or one of
	 * its children requests it.
	 */
	if (pci_base != mri->mri_bases[mri->mri_cur]) {
		ret = xmemlist_add_span(&imp->im_pool,
		    mri->mri_bases[mri->mri_cur], pci_base,
		    &imp->im_io_avail_gen, 0);
		VERIFY3S(ret, ==, MEML_SPANOP_OK);
	}
	ret = xmemlist_add_span(&imp->im_pool, pci_base,
	    mri->mri_limits[mri->mri_cur] - mri->mri_bases[mri->mri_cur] + 1,
	    &imp->im_io_avail_pci, 0);
	VERIFY3S(ret, ==, MEML_SPANOP_OK);

	mri->mri_cur++;
	return (0);
}

/*
 * The I/O ports effectively use the RE and WE bits as enable bits. Therefore we
 * need to make sure to set the limit register before setting the base register
 * for a given entry.
 */
static int
milan_io_ports_assign(milan_iodie_t *iodie, void *arg)
{
	milan_route_io_t *mri = arg;

	for (uint32_t i = 0; i < mri->mri_cur; i++) {
		uint32_t base = 0, limit = 0;

		base = DF_IO_BASE_V2_SET_RE(base, 1);
		base = DF_IO_BASE_V2_SET_WE(base, 1);
		base = DF_IO_BASE_V2_SET_BASE(base,
		    mri->mri_bases[i] >> DF_IO_BASE_SHIFT);

		limit = DF_IO_LIMIT_V3_SET_DEST_ID(limit, mri->mri_dests[i]);
		limit = DF_IO_LIMIT_V2_SET_LIMIT(limit,
		    mri->mri_limits[i] >> DF_IO_LIMIT_SHIFT);

		milan_df_bcast_write32(iodie, DF_IO_LIMIT_V2(i), limit);
		milan_df_bcast_write32(iodie, DF_IO_BASE_V2(i), base);
	}

	return (0);
}

/*
 * We need to set up the I/O port mappings to all IOMS instances. Like with
 * other things, for the moment we do the simple thing and make them shared
 * equally across all units. However, there are a few gotchas:
 *
 *  o The first 4 KiB of I/O ports are considered 'legacy'/'compatibility' I/O.
 *    This means that they need to go to the IOMS with the FCH.
 *  o The I/O space base and limit registers all have a 12-bit granularity.
 *  o The DF actually supports 24-bits of I/O space
 *  o x86 cores only support 16-bits of I/O space
 *  o There are only 8 routing rules here, so 1/IOMS in a 2P system
 *
 * So with all this in mind, we're going to do the following:
 *
 *  o Each IOMS will be assigned a single route (whether there are 4 or 8)
 *  o We're basically going to assign the 16-bits of ports evenly between all
 *    found IOMS instances.
 *  o Yes, this means the FCH is going to lose some I/O ports relative to
 *    everything else, but that's fine. If we're constrained on I/O ports, we're
 *    in trouble.
 *  o Because we have a limited number of entries, the FCH on node 0 (e.g. the
 *    primary one) has the region starting at 0.
 *  o Whoever is last gets all the extra I/O ports filling up the 1 MiB.
 */
static void
milan_route_io_ports(milan_fabric_t *fabric)
{
	milan_route_io_t mri;
	uint32_t total_size = UINT16_MAX + 1;

	bzero(&mri, sizeof (mri));
	mri.mri_per_ioms = total_size / fabric->mf_total_ioms;
	VERIFY3U(mri.mri_per_ioms, >=, 1 << DF_IO_BASE_SHIFT);
	mri.mri_next_base = mri.mri_per_ioms;

	/*
	 * First walk each IOMS to assign things evenly. We'll come back and
	 * then find the last non-primary one and that'll be the one that gets a
	 * larger limit.
	 */
	(void) milan_fabric_walk_ioms(fabric, milan_io_ports_allocate, &mri);
	mri.mri_limits[mri.mri_last_ioms] = DF_MAX_IO_LIMIT;
	(void) milan_fabric_walk_iodie(fabric, milan_io_ports_assign, &mri);
}

typedef struct milan_route_mmio {
	uint32_t	mrm_cur;
	uint32_t	mrm_mmio32_base;
	uint32_t	mrm_mmio32_chunks;
	uint32_t	mrm_fch_base;
	uint32_t	mrm_fch_chunks;
	uint64_t	mrm_mmio64_base;
	uint64_t	mrm_mmio64_chunks;
	uint64_t	mrm_bases[DF_MAX_MMIO_RULES];
	uint64_t	mrm_limits[DF_MAX_MMIO_RULES];
	uint32_t	mrm_dests[DF_MAX_MMIO_RULES];
} milan_route_mmio_t;

/*
 * We allocate two rules per device. The first is a 32-bit rule. The second is
 * then its corresponding 64-bit.  32-bit memory is always treated as
 * non-prefetchable due to the dearth of it.  64-bit memory is only treated as
 * prefetchable because we can't practically do anything else with it due to
 * the limitations of PCI-PCI bridges (64-bit memory has to be prefetch).
 */
static int
milan_mmio_allocate(milan_ioms_t *ioms, void *arg)
{
	int ret;
	milan_route_mmio_t *mrm = arg;
	const uint32_t mmio_gran = 1 << DF_MMIO_SHIFT;
	ioms_memlists_t *imp = &ioms->mio_memlists;

	/*
	 * The primary FCH is treated as a special case so that its 32-bit MMIO
	 * region is as close to the subtractive compat region as possible.
	 * That region must not be made available for PCI allocation, but we do
	 * need to keep track of where it is so the FCH driver or its children
	 * can allocate from it.
	 */
	if ((ioms->mio_flags & MILAN_IOMS_F_HAS_FCH) != 0 &&
	    (ioms->mio_iodie->mi_flags & MILAN_IODIE_F_PRIMARY) != 0) {
		mrm->mrm_bases[mrm->mrm_cur] = mrm->mrm_fch_base;
		mrm->mrm_limits[mrm->mrm_cur] = mrm->mrm_fch_base;
		mrm->mrm_limits[mrm->mrm_cur] += mrm->mrm_fch_chunks *
		    mmio_gran - 1;
		ret = xmemlist_add_span(&imp->im_pool,
		    mrm->mrm_limits[mrm->mrm_cur] + 1, MILAN_COMPAT_MMIO_SIZE,
		    &imp->im_mmio_avail_gen, 0);
		VERIFY3S(ret, ==, MEML_SPANOP_OK);
	} else {
		mrm->mrm_bases[mrm->mrm_cur] = mrm->mrm_mmio32_base;
		mrm->mrm_limits[mrm->mrm_cur] = mrm->mrm_mmio32_base;
		mrm->mrm_limits[mrm->mrm_cur] += mrm->mrm_mmio32_chunks *
		    mmio_gran - 1;
		mrm->mrm_mmio32_base += mrm->mrm_mmio32_chunks *
		    mmio_gran;
	}

	mrm->mrm_dests[mrm->mrm_cur] = ioms->mio_fabric_id;
	ret = xmemlist_add_span(&imp->im_pool, mrm->mrm_bases[mrm->mrm_cur],
	    mrm->mrm_limits[mrm->mrm_cur] - mrm->mrm_bases[mrm->mrm_cur] + 1,
	    &imp->im_mmio_avail_pci, 0);
	VERIFY3S(ret, ==, MEML_SPANOP_OK);

	mrm->mrm_cur++;

	/*
	 * Now onto the 64-bit register, which is thankfully uniform for all
	 * IOMS entries.
	 */
	mrm->mrm_bases[mrm->mrm_cur] = mrm->mrm_mmio64_base;
	mrm->mrm_limits[mrm->mrm_cur] = mrm->mrm_mmio64_base +
	    mrm->mrm_mmio64_chunks * mmio_gran - 1;
	mrm->mrm_mmio64_base += mrm->mrm_mmio64_chunks * mmio_gran;
	mrm->mrm_dests[mrm->mrm_cur] = ioms->mio_fabric_id;

	ret = xmemlist_add_span(&imp->im_pool, mrm->mrm_bases[mrm->mrm_cur],
	    mrm->mrm_limits[mrm->mrm_cur] - mrm->mrm_bases[mrm->mrm_cur] + 1,
	    &imp->im_pmem_avail, 0);
	VERIFY3S(ret, ==, MEML_SPANOP_OK);

	mrm->mrm_cur++;

	return (0);
}

/*
 * We need to set the three registers that make up an MMIO rule. Importantly we
 * set the control register last as that's what contains the effective enable
 * bits.
 */
static int
milan_mmio_assign(milan_iodie_t *iodie, void *arg)
{
	milan_route_mmio_t *mrm = arg;

	for (uint32_t i = 0; i < mrm->mrm_cur; i++) {
		uint32_t base, limit;
		uint32_t ctrl = 0;

		base = mrm->mrm_bases[i] >> DF_MMIO_SHIFT;
		limit = mrm->mrm_limits[i] >> DF_MMIO_SHIFT;
		ctrl = DF_MMIO_CTL_SET_RE(ctrl, 1);
		ctrl = DF_MMIO_CTL_SET_WE(ctrl, 1);
		ctrl = DF_MMIO_CTL_V3_SET_DEST_ID(ctrl, mrm->mrm_dests[i]);

		milan_df_bcast_write32(iodie, DF_MMIO_BASE_V2(i), base);
		milan_df_bcast_write32(iodie, DF_MMIO_LIMIT_V2(i), limit);
		milan_df_bcast_write32(iodie, DF_MMIO_CTL_V2(i), ctrl);
	}

	return (0);
}

/*
 * Routing MMIO is both important and a little complicated mostly due to the how
 * x86 actually has historically split MMIO between the below 4 GiB region and
 * the above 4 GiB region. In addition, there are only 16 routing rules that we
 * can write, which means we get a maximum of 2 routing rules per IOMS (mostly
 * because we're being lazy).
 *
 * The below 4 GiB space is split due to the compat region
 * (MILAN_PHYSADDR_COMPAT_MMIO).  The way we divide up the lower region is
 * simple:
 *
 *   o The region between TOM and 4 GiB is split evenly among all IOMSs.
 *     In a 1P system with the MMIO base set at 0x8000_0000 (as it always is in
 *     the oxide architecture) this results in 512 MiB per IOMS; with 2P it's
 *     simply half that.
 *
 *   o The part of this region at the top is assigned to the IOMS with the FCH
 *     A small part of this is removed from this routed region to account for
 *     the adjacent FCH compatibility space immediately below 4 GiB; the
 *     remainder is routed to the primary root bridge.
 *
 * 64-bit space is also simple. We find which is higher: TOM2 or the top of the
 * second hole (MILAN_PHYSADDR_IOMMU_HOLE_END).  The 256 MiB ECAM region lives
 * there; above it, we just divide all the remaining space between that and
 * MILAN_PHYSADDR_MMIO_END. This is the milan_fabric_t's mf_mmio64_base member.
 *
 * Our general assumption with this strategy is that 64-bit MMIO is plentiful
 * and that's what we'd rather assign and use.  This ties into the last bit
 * which is important: the hardware requires us to allocate in 16-bit chunks. So
 * we actually really treat all of our allocations as units of 64 KiB.
 */
static void
milan_route_mmio(milan_fabric_t *fabric)
{
	uint32_t mmio32_size;
	uint64_t mmio64_size;
	uint_t nioms32;
	milan_route_mmio_t mrm;
	const uint32_t mmio_gran = DF_MMIO_LIMIT_EXCL;

	VERIFY(IS_P2ALIGNED(fabric->mf_tom, mmio_gran));
	VERIFY3U(MILAN_PHYSADDR_COMPAT_MMIO, >, fabric->mf_tom);
	mmio32_size = MILAN_PHYSADDR_MMIO32_END - fabric->mf_tom;
	nioms32 = fabric->mf_total_ioms;
	VERIFY3U(mmio32_size, >,
	    nioms32 * mmio_gran + MILAN_COMPAT_MMIO_SIZE);

	VERIFY(IS_P2ALIGNED(fabric->mf_mmio64_base, mmio_gran));
	VERIFY3U(MILAN_PHYSADDR_MMIO_END, >, fabric->mf_mmio64_base);
	mmio64_size = MILAN_PHYSADDR_MMIO_END - fabric->mf_mmio64_base;
	VERIFY3U(mmio64_size, >,  fabric->mf_total_ioms * mmio_gran);

	CTASSERT(IS_P2ALIGNED(MILAN_PHYSADDR_COMPAT_MMIO, DF_MMIO_LIMIT_EXCL));

	bzero(&mrm, sizeof (mrm));
	mrm.mrm_mmio32_base = fabric->mf_tom;
	mrm.mrm_mmio32_chunks = mmio32_size / mmio_gran / nioms32;
	mrm.mrm_fch_base = MILAN_PHYSADDR_MMIO32_END - mmio32_size / nioms32;
	mrm.mrm_fch_chunks = mrm.mrm_mmio32_chunks -
	    MILAN_COMPAT_MMIO_SIZE / mmio_gran;
	mrm.mrm_mmio64_base = fabric->mf_mmio64_base;
	mrm.mrm_mmio64_chunks = mmio64_size / mmio_gran / fabric->mf_total_ioms;

	(void) milan_fabric_walk_ioms(fabric, milan_mmio_allocate, &mrm);
	(void) milan_fabric_walk_iodie(fabric, milan_mmio_assign, &mrm);
}

static ioms_rsrc_t
milan_ioms_prd_to_rsrc(pci_prd_rsrc_t rsrc)
{
	switch (rsrc) {
	case PCI_PRD_R_IO:
		return (IR_PCI_LEGACY);
	case PCI_PRD_R_MMIO:
		return (IR_PCI_MMIO);
	case PCI_PRD_R_PREFETCH:
		return (IR_PCI_PREFETCH);
	case PCI_PRD_R_BUS:
		return (IR_PCI_BUS);
	default:
		return (IR_NONE);
	}
}

static struct memlist *
milan_fabric_rsrc_subsume(milan_ioms_t *ioms, ioms_rsrc_t rsrc)
{
	ioms_memlists_t *imp;
	struct memlist **avail, **used, *ret;

	imp = &ioms->mio_memlists;
	mutex_enter(&imp->im_lock);
	switch (rsrc) {
	case IR_PCI_LEGACY:
		avail = &imp->im_io_avail_pci;
		used = &imp->im_io_used;
		break;
	case IR_PCI_MMIO:
		avail = &imp->im_mmio_avail_pci;
		used = &imp->im_mmio_used;
		break;
	case IR_PCI_PREFETCH:
		avail = &imp->im_pmem_avail;
		used = &imp->im_pmem_used;
		break;
	case IR_PCI_BUS:
		avail = &imp->im_bus_avail;
		used = &imp->im_bus_used;
		break;
	case IR_GEN_LEGACY:
		avail = &imp->im_io_avail_gen;
		used = &imp->im_io_used;
		break;
	case IR_GEN_MMIO:
		avail = &imp->im_mmio_avail_gen;
		used = &imp->im_mmio_used;
		break;
	default:
		mutex_exit(&imp->im_lock);
		return (NULL);
	}

	/*
	 * If there are no resources, that may be because there never were any
	 * or they had already been handed out.
	 */
	if (*avail == NULL) {
		mutex_exit(&imp->im_lock);
		return (NULL);
	}

	/*
	 * We have some resources available for this NB instance. In this
	 * particular case, we need to first duplicate these using kmem and then
	 * we can go ahead and move all of these to the used list.  This is done
	 * for the benefit of PCI code which expects it, but we do it
	 * universally for consistency.
	 */
	ret = memlist_kmem_dup(*avail, KM_SLEEP);

	/*
	 * XXX This ends up not really coalescing ranges, but maybe that's fine.
	 */
	while (*avail != NULL) {
		struct memlist *to_move = *avail;
		memlist_del(to_move, avail);
		memlist_insert(to_move, used);
	}

	mutex_exit(&imp->im_lock);
	return (ret);
}

/*
 * This is a request that we take resources from a given IOMS root port and
 * basically give what remains and hasn't been allocated to PCI. This is a bit
 * of a tricky process as we want to both:
 *
 *  1. Give everything that's currently available to PCI; however, it needs
 *     memlists that are allocated with kmem due to how PCI memlists work.
 *  2. We need to move everything that we're giving to PCI into our used list
 *     just for our own tracking purposes.
 */
struct memlist *
milan_fabric_pci_subsume(uint32_t bus, pci_prd_rsrc_t rsrc)
{
	milan_ioms_t *ioms;
	milan_fabric_t *fabric = &milan_fabric;
	ioms_rsrc_t ir;

	ioms = milan_fabric_find_ioms_by_bus(fabric, bus);
	if (ioms == NULL) {
		return (NULL);
	}

	ir = milan_ioms_prd_to_rsrc(rsrc);

	return (milan_fabric_rsrc_subsume(ioms, ir));
}

/*
 * This is for the rest of the available legacy IO and MMIO space that we've set
 * aside for things that are not PCI.  The intent is that the caller will feed
 * the space to busra or the moral equivalent.  While this is presently used
 * only by the FCH and is set up only for the IOMSs that have an FCH attached,
 * in principle this could be applied to other users as well, including IOAPICs
 * and IOMMUs that are present in all NB instances.  For now this is really
 * about getting all this out of earlyboot context where we don't have modules
 * like rootnex and busra and into places where it's better managed; in this it
 * has the same purpose as its PCI counterpart above.  The memlists we supply
 * don't have to be allocated by kmem, but we do it anyway for consistency and
 * ease of use for callers.
 *
 * Curiously, AMD's documentation indicates that each of the PCI and non-PCI
 * regions associated with each NB instance must be contiguous, but there's no
 * hardware reason for that beyond the mechanics of assigning resources to PCIe
 * root ports.  So if we were to improve busra to manage these resources
 * globally instead of making PCI its own separate pool, we wouldn't need this
 * clumsy non-PCI reservation and could instead assign resources globally with
 * respect to each NB instance regardless of the requesting device type.  The
 * future's so bright, we gotta wear shades.
 */
struct memlist *
milan_fabric_gen_subsume(milan_ioms_t *ioms, ioms_rsrc_t ir)
{
	return (milan_fabric_rsrc_subsume(ioms, ir));
}

/*
 * Here we are going through bridges and need to start setting them up with the
 * various features that we care about. Most of these are an attempt to have
 * things set up so PCIe enumeration can meaningfully actually use these. The
 * exact set of things required is ill-defined. Right now this includes:
 *
 *   o Enabling the bridges such that they can actually allow software to use
 *     them. XXX Though really we should disable DMA until such a time as we're
 *     OK with that.
 *
 *   o Changing settings that will allow the links to actually flush TLPs when
 *     the link goes down.
 */
static int
milan_fabric_init_bridges(milan_pcie_bridge_t *bridge, void *arg)
{
	smn_reg_t reg;
	uint32_t val;
	boolean_t hide;
	milan_pcie_port_t *port = bridge->mpb_port;
	milan_ioms_t *ioms = port->mpp_ioms;

	/*
	 * We need to determine whether or not this bridge should be considered
	 * visible. This is messy. Ideally, we'd just have every bridge be
	 * visible; however, life isn't that simple because convincing the PCIe
	 * engine that it should actually allow for completion timeouts to
	 * function as expected. In addition, having bridges that have no
	 * devices present and never can due to the platform definition can end
	 * up being rather wasteful of precious 32-bit non-prefetchable memory.
	 * The current masking rules are based on what we have learned from
	 * trial and error works.
	 *
	 * Strictly speaking, a bridge will work from a completion timeout
	 * perspective if the SMU thinks it belongs to a PCIe port that has any
	 * hotpluggable elements or otherwise has a device present.
	 * Unfortunately the case you really want to work, a non-hotpluggable,
	 * but defined device that does not have a device present should be
	 * visible does not work.
	 *
	 * Ultimately, what we have implemented here is to basically say if a
	 * bridge is not mapped to an endpoint, then it is not shown. If it is,
	 * and it belongs to a hot-pluggable port then we always show it.
	 * Otherwise we only show it if there's a device present.
	 */
	if ((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_MAPPED) != 0) {
		boolean_t hotplug, trained;
		uint8_t lt;

		hotplug = (port->mpp_flags & MILAN_PCIE_PORT_F_HAS_HOTPLUG) !=
		    0;
		lt = bridge->mpb_engine->zde_config.zdc_pcie.zdcp_link_train;
		trained = lt == MILAN_DXIO_PCIE_SUCCESS;
		hide = !hotplug && !trained;
	} else {
		hide = B_TRUE;
	}

	if (hide) {
		bridge->mpb_flags |= MILAN_PCIE_BRIDGE_F_HIDDEN;
	}

	reg = milan_pcie_bridge_reg(bridge, D_IOHCDEV_PCIE_BRIDGE_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = IOHCDEV_BRIDGE_CTL_SET_CRS_ENABLE(val, 1);
	if (hide) {
		val = IOHCDEV_BRIDGE_CTL_SET_BRIDGE_DISABLE(val, 1);
		val = IOHCDEV_BRIDGE_CTL_SET_DISABLE_BUS_MASTER(val, 1);
		val = IOHCDEV_BRIDGE_CTL_SET_DISABLE_CFG(val, 1);
	} else {
		val = IOHCDEV_BRIDGE_CTL_SET_BRIDGE_DISABLE(val, 0);
		val = IOHCDEV_BRIDGE_CTL_SET_DISABLE_BUS_MASTER(val, 0);
		val = IOHCDEV_BRIDGE_CTL_SET_DISABLE_CFG(val, 0);
	}
	milan_pcie_bridge_write(bridge, reg, val);

	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_TX_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_TX_CTL_SET_TLP_FLUSH_DOWN_DIS(val, 0);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Make sure the hardware knows the corresponding b/d/f for this bridge.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_TX_ID);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_TX_ID_SET_BUS(val, ioms->mio_pci_busno);
	val = PCIE_PORT_TX_ID_SET_DEV(val, bridge->mpb_device);
	val = PCIE_PORT_TX_ID_SET_FUNC(val, bridge->mpb_func);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Next, we have to go through and set up a bunch of the lane controller
	 * configuration controls for the individual bridge. These include
	 * various settings around how idle transitions occur, how it replies to
	 * certain messages, and related.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_CTL_SET_L1_IMM_ACK(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_TRAIN_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_TRAIN_CTL_SET_L0S_L1_TRAIN(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_WIDTH_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_WIDTH_CTL_SET_DUAL_RECONFIG(val, 1);
	val = PCIE_PORT_LC_WIDTH_CTL_SET_RENEG_EN(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_CTL2);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_CTL2_SET_ELEC_IDLE(val,
	    PCIE_PORT_LC_CTL2_ELEC_IDLE_M1);
	/*
	 * This is supposed to be set as part of some workaround for ports that
	 * support at least PCIe Gen 3.0 speeds. As all supported platforms
	 * (gimlet, Ethanol-X, etc.) always support that on the port unless this
	 * is one of the WAFL related lanes, we always set this.
	 */
	if (port->mpp_portno != MILAN_IOMS_WAFL_PCIE_PORT) {
		val = PCIE_PORT_LC_CTL2_SET_TS2_CHANGE_REQ(val,
		    PCIE_PORT_LC_CTL2_TS2_CHANGE_128);
	}
	milan_pcie_bridge_write(bridge, reg, val);

	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_CTL3);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_CTL3_SET_DOWN_SPEED_CHANGE(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Lucky Hardware Debug 15. Why is it lucky? Because all we know is
	 * we've been told to set it.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_HW_DBG);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_HW_DBG_SET_DBG15(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Software expects to see the PCIe slot implemented bit when a slot
	 * actually exists. For us, this is basically anything that actually is
	 * considered MAPPED. Set that now on the bridge.
	 */
	if ((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_MAPPED) != 0) {
		uint16_t reg;

		reg = pci_getw_func(ioms->mio_pci_busno, bridge->mpb_device,
		    bridge->mpb_func, MILAN_BRIDGE_R_PCI_PCIE_CAP);
		reg |= PCIE_PCIECAP_SLOT_IMPL;
		pci_putw_func(ioms->mio_pci_busno, bridge->mpb_device,
		    bridge->mpb_func, MILAN_BRIDGE_R_PCI_PCIE_CAP, reg);
	}

	return (0);
}

/*
 * This is a companion to milan_fabric_init_bidges, that operates on the PCIe
 * port level before we get to the individual bridge. This initialization
 * generally is required to ensure that each port (regardless of whether it's
 * hidden or not) is able to properly generate an all 1s response. In addition
 * we have to take care of things like atomics, idling defaults, certain
 * receiver completion buffer checks, etc.
 */
static int
milan_fabric_init_pcie_ports(milan_pcie_port_t *port, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	reg = milan_pcie_port_reg(port, D_PCIE_CORE_CI_CTL);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_CI_CTL_SET_LINK_DOWN_CTO_EN(val, 1);
	val = PCIE_CORE_CI_CTL_SET_IGN_LINK_DOWN_CTO_ERR(val, 1);
	milan_pcie_port_write(port, reg, val);

	/*
	 * Program the unit ID for this device's SDP port.
	 */
	reg = milan_pcie_port_reg(port, D_PCIE_CORE_SDP_CTL);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_SDP_CTL_SET_PORT_ID(val, port->mpp_sdp_port);
	val = PCIE_CORE_SDP_CTL_SET_UNIT_ID(val, port->mpp_sdp_unit);
	milan_pcie_port_write(port, reg, val);

	/*
	 * Ensure that RCB checking is what's seemingly expected.
	 */
	reg = milan_pcie_port_reg(port, D_PCIE_CORE_PCIE_CTL);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_PCIE_CTL_SET_RCB_BAD_ATTR_DIS(val, 1);
	val = PCIE_CORE_PCIE_CTL_SET_RCB_BAD_SIZE_DIS(val, 0);
	milan_pcie_port_write(port, reg, val);

	/*
	 * Enabling atomics in the core requires a few different registers. Both
	 * a strap has to be overridden and then corresponding control bits.
	 */
	reg = milan_pcie_port_reg(port, D_PCIE_CORE_STRAP_F0);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_STRAP_F0_SET_ATOMIC_ROUTE(val, 1);
	val = PCIE_CORE_STRAP_F0_SET_ATOMIC_EN(val, 1);
	milan_pcie_port_write(port, reg, val);

	reg = milan_pcie_port_reg(port, D_PCIE_CORE_PCIE_CTL2);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_PCIE_CTL2_TX_ATOMIC_ORD_DIS(val, 1);
	val = PCIE_CORE_PCIE_CTL2_TX_ATOMIC_OPS_DIS(val, 0);
	milan_pcie_port_write(port, reg, val);

	/*
	 * Ensure the correct electrical idle mode detection is set. In
	 * addition, it's been recommended we ignore the K30.7 EDB (EnD Bad)
	 * special symbol errors.
	 */
	reg = milan_pcie_port_reg(port, D_PCIE_CORE_PCIE_P_CTL);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_PCIE_P_CTL_SET_ELEC_IDLE(val,
	    PCIE_CORE_PCIE_P_CTL_ELEC_IDLE_M1);
	val = PCIE_CORE_PCIE_P_CTL_SET_IGN_EDB_ERR(val, 1);
	milan_pcie_port_write(port, reg, val);

	/*
	 * The IOMMUL1 does not have an instance for the on-the side WAFL lanes.
	 * Skip the WAFL port if we're that.
	 */
	if (port->mpp_portno >= IOMMUL1_N_PCIE_PORTS)
		return (0);

	reg = milan_pcie_port_reg(port, D_IOMMUL1_CTL1);
	val = milan_pcie_port_read(port, reg);
	val = IOMMUL1_CTL1_SET_ORDERING(val, 1);
	milan_pcie_port_write(port, reg, val);

	return (0);
}

typedef struct {
	milan_ioms_t *pbc_ioms;
	uint8_t pbc_busoff;
} pci_bus_counter_t;

static int
milan_fabric_hack_bridges_cb(milan_pcie_bridge_t *bridge, void *arg)
{
	uint8_t bus, secbus;
	pci_bus_counter_t *pbc = arg;
	milan_ioms_t *ioms = bridge->mpb_port->mpp_ioms;

	bus = ioms->mio_pci_busno;
	if (pbc->pbc_ioms != ioms) {
		pbc->pbc_ioms = ioms;
		pbc->pbc_busoff = 1 + ARRAY_SIZE(milan_int_bridges);
		for (uint_t i = 0; i < ARRAY_SIZE(milan_int_bridges); i++) {
			const milan_bridge_info_t *info = &milan_int_bridges[i];
			pci_putb_func(bus, info->mpbi_dev, info->mpbi_func,
			    PCI_BCNF_PRIBUS, bus);
			pci_putb_func(bus, info->mpbi_dev, info->mpbi_func,
			    PCI_BCNF_SECBUS, bus + 1 + i);
			pci_putb_func(bus, info->mpbi_dev, info->mpbi_func,
			    PCI_BCNF_SUBBUS, bus + 1 + i);

		}
	}

	if ((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_HIDDEN) != 0) {
		return (0);
	}

	secbus = bus + pbc->pbc_busoff;

	pci_putb_func(bus, bridge->mpb_device, bridge->mpb_func,
	    PCI_BCNF_PRIBUS, bus);
	pci_putb_func(bus, bridge->mpb_device, bridge->mpb_func,
	    PCI_BCNF_SECBUS, secbus);
	pci_putb_func(bus, bridge->mpb_device, bridge->mpb_func,
	    PCI_BCNF_SUBBUS, secbus);

	pbc->pbc_busoff++;
	return (0);
}

/*
 * XXX This whole function exists to workaround deficiencies in software and
 * basically try to ape parts of the PCI firmware spec. The OS should natively
 * handle this. In particular, we currently do the following:
 *
 *   o Program a single downstream bus onto each root port. We can only get away
 *     with this because we know there are no other bridges right now. This
 *     cannot be a long term solution, though I know we will be temped to make
 *     it one. I'm sorry future us.
 */
static void
milan_fabric_hack_bridges(milan_fabric_t *fabric)
{
	pci_bus_counter_t c;
	bzero(&c, sizeof (c));

	milan_fabric_walk_bridge(fabric, milan_fabric_hack_bridges_cb, &c);
}

/*
 * If this assertion fails, fix the definition in dxio_impl.h or increase the
 * size of the contiguous mapping below.
 */
CTASSERT(sizeof (smu_hotplug_table_t) <= MMU_PAGESIZE);

/*
 * Allocate and initialize the hotplug table. The return value here is used to
 * indicate whether or not the platform has hotplug and thus should continue or
 * not with actual set up.
 */
static boolean_t
milan_smu_hotplug_data_init(milan_fabric_t *fabric)
{
	ddi_dma_attr_t attr;
	milan_hotplug_t *hp = &fabric->mf_hotplug;
	const smu_hotplug_entry_t *entry;
	pfn_t pfn;
	boolean_t cont;

	milan_smu_dma_attr(&attr);
	hp->mh_alloc_len = MMU_PAGESIZE;
	hp->mh_table = contig_alloc(MMU_PAGESIZE, &attr, MMU_PAGESIZE, 1);
	bzero(hp->mh_table, MMU_PAGESIZE);
	pfn = hat_getpfnum(kas.a_hat, (caddr_t)hp->mh_table);
	hp->mh_pa = mmu_ptob((uint64_t)pfn);

	if (milan_board_type(fabric) == MBT_ETHANOL) {
		entry = ethanolx_hotplug_ents;
	} else {
		entry = gimlet_hotplug_ents;
	}

	cont = entry[0].se_slotno != SMU_HOTPLUG_ENT_LAST;

	/*
	 * The way the SMU takes this data table is that entries are indexed by
	 * physical slot number. We basically use an interim structure that's
	 * different so we can have a sparse table. In addition, if we find a
	 * device, update that info on its bridge.
	 */
	for (uint_t i = 0; entry[i].se_slotno != SMU_HOTPLUG_ENT_LAST; i++) {
		uint_t slot = entry[i].se_slotno;
		const smu_hotplug_map_t *map;
		milan_iodie_t *iodie;
		milan_ioms_t *ioms;
		milan_pcie_port_t *port;
		milan_pcie_bridge_t *bridge;

		hp->mh_table->smt_map[slot] = entry[i].se_map;
		hp->mh_table->smt_func[slot] = entry[i].se_func;
		hp->mh_table->smt_reset[slot] = entry[i].se_reset;

		/*
		 * Attempt to find the bridge this corresponds to. It should
		 * already have been mapped.
		 */
		map = &entry[i].se_map;
		iodie = &fabric->mf_socs[map->shm_die_id].ms_iodies[0];
		ioms = &iodie->mi_ioms[map->shm_tile_id % 4];
		port = &ioms->mio_pcie_ports[map->shm_tile_id / 4];
		bridge = &port->mpp_bridges[map->shm_port_id];

		cmn_err(CE_NOTE, "mapped entry %u to bridge %p", i, bridge);
		VERIFY((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_MAPPED) != 0);
		VERIFY((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_HIDDEN) == 0);
		bridge->mpb_flags |= MILAN_PCIE_BRIDGE_F_HOTPLUG;
		bridge->mpb_hp_type = map->shm_format;
		bridge->mpb_hp_slotno = slot;
		bridge->mpb_hp_smu_mask = entry[i].se_func.shf_mask;
	}

	return (cont);
}

/*
 * Determine the set of feature bits that should be enabled. If this is Ethanol,
 * use our hacky static versions for a moment.
 */
static uint32_t
milan_hotplug_bridge_features(milan_pcie_bridge_t *bridge)
{
	uint32_t feats;
	milan_fabric_t *fabric =
	    bridge->mpb_port->mpp_ioms->mio_iodie->mi_soc->ms_fabric;

	if (milan_board_type(fabric) == MBT_ETHANOL) {
		if (bridge->mpb_hp_type == SMU_HP_ENTERPRISE_SSD) {
			return (ethanolx_pcie_slot_cap_entssd);
		} else {
			return (ethanolx_pcie_slot_cap_express);
		}
	}

	feats = PCIE_SLOTCAP_HP_SURPRISE | PCIE_SLOTCAP_HP_CAPABLE;

	/*
	 * The set of features we enable changes based on the type of hotplug
	 * mode. While Enterprise SSD uses a static set of features, the various
	 * ExpressModule modes have a mask register that is used to tell the SMU
	 * that it doesn't support a given feature. As such, we check for these
	 * masks to determine what to enable. Because these bits are used to
	 * turn off features in the SMU, we check for the absence of it (e.g. ==
	 * 0) to indicate that we should enable the feature.
	 */
	switch (bridge->mpb_hp_type) {
	case SMU_HP_ENTERPRISE_SSD:
		/*
		 * For Enterprise SSD the set of features that are supported are
		 * considered a constant and this doesn't really vary based on
		 * the board. There is no power control, just surprise hotplug
		 * capabilities. Apparently in this mode there is no SMU command
		 * completion.
		 */
		return (feats | PCIE_SLOTCAP_NO_CMD_COMP_SUPP);
	case SMU_HP_EXPRESS_MODULE_A:
		if ((bridge->mpb_hp_smu_mask & SMU_ENTA_ATTNSW) == 0) {
			feats |= PCIE_SLOTCAP_ATTN_BUTTON;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTA_EMILS) == 0 ||
		    (bridge->mpb_hp_smu_mask & SMU_ENTA_EMIL) == 0) {
			feats |= PCIE_SLOTCAP_EMI_LOCK_PRESENT;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTA_PWREN) == 0) {
			feats |= PCIE_SLOTCAP_POWER_CONTROLLER;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTA_ATTNLED) == 0) {
			feats |= PCIE_SLOTCAP_ATTN_INDICATOR;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTA_PWRLED) == 0) {
			feats |= PCIE_SLOTCAP_PWR_INDICATOR;
		}
		break;
	case SMU_HP_EXPRESS_MODULE_B:
		if ((bridge->mpb_hp_smu_mask & SMU_ENTB_ATTNSW) == 0) {
			feats |= PCIE_SLOTCAP_ATTN_BUTTON;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTB_EMILS) == 0 ||
		    (bridge->mpb_hp_smu_mask & SMU_ENTB_EMIL) == 0) {
			feats |= PCIE_SLOTCAP_EMI_LOCK_PRESENT;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTB_PWREN) == 0) {
			feats |= PCIE_SLOTCAP_POWER_CONTROLLER;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTB_ATTNLED) == 0) {
			feats |= PCIE_SLOTCAP_ATTN_INDICATOR;
		}

		if ((bridge->mpb_hp_smu_mask & SMU_ENTB_PWRLED) == 0) {
			feats |= PCIE_SLOTCAP_PWR_INDICATOR;
		}
		break;
	default:
		return (0);
	}

	return (feats);
}

/*
 * At this point we have finished telling the SMU and its hotplug system to get
 * started. In particular, there are a few things that we do to try and
 * synchronize the PCIe slot and the SMU state, because they are not the same.
 * In particular, we have reason to believe that without a write to the slot
 * control register, the SMU will not write to the GPIO expander and therefore
 * all the outputs will remain at their hardware device's default. The most
 * important part of this is to ensure that we put the slot's power into a
 * defined state.
 */
static int
milan_hotplug_bridge_post_start(milan_pcie_bridge_t *bridge, void *arg)
{
	uint16_t ctl, sts;
	milan_ioms_t *ioms = bridge->mpb_port->mpp_ioms;

	sts = pci_getw_func(ioms->mio_pci_busno, bridge->mpb_device,
	    bridge->mpb_func, MILAN_BRIDGE_R_PCI_SLOT_STS);

	/*
	 * At this point, surprisingly enough, it is expected that all the
	 * notification and fault detection bits be turned on at the SMU as part
	 * of turning on and off the slot. This is a little surprising. Power
	 * was one thing, but at this point it expects to have hotplug
	 * interrupts enabled and all the rest of the features that the hardware
	 * supports (e.g. no MRL sensor changed). Note, we have explicitly left
	 * out setting the following that it does:
	 *
	 *   o The power indicator to on when a device is present
	 */
	ctl = pci_getw_func(ioms->mio_pci_busno, bridge->mpb_device,
	    bridge->mpb_func, MILAN_BRIDGE_R_PCI_SLOT_CTL);
	ctl |= PCIE_SLOTCTL_ATTN_BTN_EN;
	ctl |= PCIE_SLOTCTL_PWR_FAULT_EN;
	ctl |= PCIE_SLOTCTL_PRESENCE_CHANGE_EN;
	ctl |= PCIE_SLOTCTL_HP_INTR_EN;

	/*
	 * Finally we need to initialize the power state based on slot presence
	 * at this time. Reminder: slot power is enabled when the bit is zero.
	 * It is possible that this may still be creating a race downstream of
	 * this, but in that case, that'll be on the pcieb hotplug logic rather
	 * than us to set up that world here.
	 */
	if ((sts & PCIE_SLOTSTS_PRESENCE_DETECTED) != 0) {
		ctl &= ~PCIE_SLOTCTL_PWR_CONTROL;
	} else {
		ctl |= PCIE_SLOTCTL_PWR_CONTROL;
	}
	pci_putw_func(ioms->mio_pci_busno, bridge->mpb_device,
	    bridge->mpb_func, MILAN_BRIDGE_R_PCI_SLOT_CTL, ctl);

	return (0);
}

/*
 * At this point we need to go through and prep all hotplug-capable bridges.
 * This means setting up the following:
 *
 *   o Setting the appropriate slot capabilities.
 *   o Setting the slot's actual number in PCIe and in a secondary SMN location.
 *   o Setting control bits in the PCIe IP to ensure we don't enter loopback
 *     mode and some amount of other state machine control.
 *   o Making sure that power faults work.
 */
static int
milan_hotplug_bridge_init(milan_pcie_bridge_t *bridge, void *arg)
{
	smn_reg_t reg;
	uint32_t val;
	uint32_t slot_mask;
	milan_pcie_port_t *port = bridge->mpb_port;
	milan_ioms_t *ioms = port->mpp_ioms;

	/*
	 * Skip over all non-hotplug slots and the simple presence mode. Though
	 * one has to ask oneself, why have hotplug if you're going to use the
	 * simple presence mode.
	 */
	if ((bridge->mpb_flags & MILAN_PCIE_BRIDGE_F_HOTPLUG) == 0 ||
	    bridge->mpb_hp_type == SMU_HP_PRESENCE_DETECT) {
		return (0);
	}

	/*
	 * Set the hotplug slot information in the PCIe IP, presumably so that
	 * it'll do something useful for the SMU.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_HP_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_HP_CTL_SET_SLOT(val, bridge->mpb_hp_slotno);
	val = PCIE_PORT_HP_CTL_SET_ACTIVE(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * This register is apparently set to ensure that we don't remain in the
	 * detect state machine state.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_CTL5);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_CTL5_SET_WAIT_DETECT(val, 0);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * This ensures the port can't enter loopback mode.
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_LC_TRAIN_CTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_LC_TRAIN_CTL_SET_TRAIN_DIS(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Make sure that power faults can actually work (in theory).
	 */
	reg = milan_pcie_bridge_reg(bridge, D_PCIE_PORT_PCTL);
	val = milan_pcie_bridge_read(bridge, reg);
	val = PCIE_PORT_PCTL_SET_PWRFLT_EN(val, 1);
	milan_pcie_bridge_write(bridge, reg, val);

	/*
	 * Go through and set up the slot capabilities register. In our case
	 * we've already filtered out the non-hotplug capable bridges. To
	 * determine the set of hotplug features that should be set here we
	 * derive that from the actual hoptlug entities. Because one is required
	 * to give the SMU a list of functions to mask, the unmasked bits tells
	 * us what to enable as features here.
	 */
	slot_mask = PCIE_SLOTCAP_ATTN_BUTTON | PCIE_SLOTCAP_POWER_CONTROLLER |
	    PCIE_SLOTCAP_MRL_SENSOR | PCIE_SLOTCAP_ATTN_INDICATOR |
	    PCIE_SLOTCAP_PWR_INDICATOR | PCIE_SLOTCAP_HP_SURPRISE |
	    PCIE_SLOTCAP_HP_CAPABLE | PCIE_SLOTCAP_EMI_LOCK_PRESENT |
	    PCIE_SLOTCAP_NO_CMD_COMP_SUPP;

	val = pci_getl_func(ioms->mio_pci_busno, bridge->mpb_device,
	    bridge->mpb_func, MILAN_BRIDGE_R_PCI_SLOT_CAP);
	val &= ~(PCIE_SLOTCAP_PHY_SLOT_NUM_MASK <<
	    PCIE_SLOTCAP_PHY_SLOT_NUM_SHIFT);
	val |= bridge->mpb_hp_slotno << PCIE_SLOTCAP_PHY_SLOT_NUM_SHIFT;
	val &= ~slot_mask;
	val |= milan_hotplug_bridge_features(bridge);
	pci_putl_func(ioms->mio_pci_busno, bridge->mpb_device,
	    bridge->mpb_func, MILAN_BRIDGE_R_PCI_SLOT_CAP, val);

	/*
	 * Finally we need to go through and unblock training now that we've set
	 * everything else on the slot. Note, this is done before we tell the
	 * SMU about hotplug configuration, so strictly speaking devices will
	 * unlikely start suddenly training.
	 */
	reg = milan_pcie_port_reg(port, D_PCIE_CORE_SWRST_CTL6);
	val = milan_pcie_port_read(port, reg);
	val = bitset32(val, bridge->mpb_bridgeno, bridge->mpb_bridgeno, 0);
	milan_pcie_port_write(port, reg, val);

	return (0);
}

/*
 * This is an analogue to the above functions; however, it operates on the PCIe
 * port basis rather than the individual bridge. This mostly includes:
 *   o Making sure that there are no holds on link training on any port.
 *   o Ensuring that presence detection is based on an 'OR'
 *
 * XXX SMN_NBIO0PCIE0_SWRST_CONTROL_6_A
 */
static int
milan_hotplug_port_init(milan_pcie_port_t *port, void *arg)
{
	smn_reg_t reg;
	uint32_t val;

	/*
	 * Nothing to do if there's no hotplug.
	 */
	if ((port->mpp_flags & MILAN_PCIE_PORT_F_HAS_HOTPLUG) == 0) {
		return (0);
	}

	reg = milan_pcie_port_reg(port, D_PCIE_CORE_PRES);
	val = milan_pcie_port_read(port, reg);
	val = PCIE_CORE_PRES_SET_MODE(val, PCIE_CORE_PRES_MODE_OR);
	milan_pcie_port_write(port, reg, val);

	return (0);
}

/*
 * XXX This is a total hack. Unfortunately the SMU relies on x86 software to
 * actually set the i2c clock up to something expected for it. Temporarily do
 * this the max power way.
 */
static boolean_t
xxx_fixup_i2c_clock(void)
{
	void *va = device_arena_alloc(MMU_PAGESIZE, VM_SLEEP);
	pfn_t pfn = mmu_btop(0xfedc2000);
	hat_devload(kas.a_hat, va, MMU_PAGESIZE, pfn,
	    PROT_READ | PROT_WRITE | HAT_STRICTORDER,
	    HAT_LOAD_LOCK | HAT_LOAD_NOCONSIST);
	*(uint32_t *)va = 0x63;
	hat_unload(kas.a_hat, va, MMU_PAGESIZE, HAT_UNLOAD_UNLOCK);
	device_arena_free(va, MMU_PAGESIZE);

	return (B_TRUE);
}

/*
 * Begin the process of initializing the hotplug subsystem with the SMU. In
 * particular we need to do the following steps:
 *
 *  o Send a series of commands to set up the i2c switches in general. These
 *    correspond to the various bit patterns that we program in the function
 *    payload.
 *
 *  o Set up and send across our hotplug table.
 *
 *  o Finish setting up the bridges to be ready for hotplug.
 *
 *  o Actually tell it to start.
 *
 * Unlike with DXIO initialization, it appears that hotplug initialization only
 * takes place on the primary SMU. In some ways, this makes some sense because
 * the hotplug table has information about which dies and sockets are used for
 * what and further, only the first socket ever is connected to the hotplug i2c
 * bus; however, it is still also a bit mysterious.
 */
static boolean_t
milan_hotplug_init(milan_fabric_t *fabric)
{
	milan_hotplug_t *hp = &fabric->mf_hotplug;
	milan_iodie_t *iodie = &fabric->mf_socs[0].ms_iodies[0];

	/*
	 * These represent the addresses that we need to program in the SMU.
	 * Strictly speaking, the lower 8-bits represents the addresses that the
	 * SMU seems to expect. The upper byte is a bit more of a mystery;
	 * however, it does correspond to the expected values that AMD roughly
	 * documents for 5-bit bus segment value which is the shf_i2c_bus member
	 * of the smu_hotplug_function_t.
	 */
	const uint32_t i2c_addrs[4] = { 0x70, 0x171, 0x272, 0x373 };

	if (!milan_smu_hotplug_data_init(fabric)) {
		/*
		 * This case is used to indicate that there was nothing in
		 * particular that needed hotplug. Therefore, we don't bother
		 * trying to tell the SMU about it.
		 */
		return (B_TRUE);
	}

	for (uint_t i = 0; i < ARRAY_SIZE(i2c_addrs); i++) {
		if (!milan_smu_rpc_i2c_switch(iodie, i2c_addrs[i])) {
			return (B_FALSE);
		}
	}

	if (!milan_smu_rpc_give_address(iodie, MSAK_HOTPLUG, hp->mh_pa)) {
		return (B_FALSE);
	}

	if (!milan_smu_rpc_send_hotplug_table(iodie)) {
		return (B_FALSE);
	}

	/*
	 * Go through now and set up bridges for hotplug data. Honor the spirit
	 * of the old world by doing this after we send the hotplug table, but
	 * before we enable things. It's unclear if the order is load bearing or
	 * not.
	 */
	(void) milan_fabric_walk_pcie_port(fabric, milan_hotplug_port_init,
	    NULL);
	(void) milan_fabric_walk_bridge(fabric, milan_hotplug_bridge_init,
	    NULL);

	if (!milan_smu_rpc_hotplug_flags(iodie, 0)) {
		return (B_FALSE);
	}

	/*
	 * XXX This is an unfortunate bit. The SMU relies on someone else to
	 * have set the actual state of the i2c clock.
	 */
	if (!xxx_fixup_i2c_clock()) {
		return (B_FALSE);
	}

	if (!milan_smu_rpc_start_hotplug(iodie, B_FALSE, 0)) {
		return (B_FALSE);
	}

	/*
	 * Now that this is done, we need to go back through and do some final
	 * pieces of slot initialization which are probably necessary to get the
	 * SMU into the same place as we are with everything else.
	 */
	(void) milan_fabric_walk_bridge(fabric, milan_hotplug_bridge_post_start,
	    NULL);

	return (B_TRUE);
}

/*
 * This is the main place where we basically do everything that we need to do to
 * get the PCIe engine up and running.
 */
void
milan_fabric_init(void)
{
	milan_fabric_t *fabric = &milan_fabric;

	/*
	 * XXX We're missing initialization of some different pieces of the data
	 * fabric here. While some of it like scrubbing should be done as part
	 * of the memory controller driver and broader policy rather than all
	 * here right now.
	 */

	/*
	 * When we come out of reset, the PSP and/or SMU have set up our DRAM
	 * routing rules and the PCI bus routing rules. We need to go through
	 * and save this information as well as set up I/O ports and MMIO. This
	 * process will also save our own allocations of these resources,
	 * allowing us to use them for our own purposes or for PCI.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_memlists, NULL);
	milan_route_pci_bus(fabric);
	milan_route_io_ports(fabric);
	milan_route_mmio(fabric);

	/*
	 * While DRAM training seems to have programmed the initial memory
	 * settings our boot CPU and the DF, it is not done on the various IOMS
	 * instances. It is up to us to program that across them all.  With MMIO
	 * routed and the IOHC's understanding of TOM set up, we also want to
	 * disable the VGA MMIO hole so that the entire low memory region goes
	 * to DRAM for downstream requests just as it does from the cores.  We
	 * don't use VGA and we don't use ASeg, so there's no reason to hide
	 * this RAM from anyone.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_tom, NULL);
	milan_fabric_walk_ioms(fabric, milan_fabric_disable_iohc_vga, NULL);

	/*
	 * Let's set up PCIe. To lead off, let's make sure the system uses the
	 * right clock and let's start the process of dealing with the how
	 * configuration space retries should work, though this isn't sufficient
	 * for them to work.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_pcie_refclk, NULL);
	milan_fabric_walk_ioms(fabric, milan_fabric_init_pci_to, NULL);
	milan_fabric_walk_ioms(fabric, milan_fabric_init_iohc_features, NULL);

	/*
	 * There is a lot of different things that we have to do here. But first
	 * let me apologize in advance. The what here is weird and the why is
	 * non-existent. Effectively this is being done because either we were
	 * explicitly told to in the PPR or through other means. This is going
	 * to be weird and you have every right to complain.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_iohc_fch_link, NULL);
	milan_fabric_walk_ioms(fabric, milan_fabric_init_arbitration_ioms,
	    NULL);
	milan_fabric_walk_nbif(fabric, milan_fabric_init_arbitration_nbif,
	    NULL);
	milan_fabric_walk_ioms(fabric, milan_fabric_init_sdp_control, NULL);
	milan_fabric_walk_nbif(fabric, milan_fabric_init_nbif_syshub_dma,
	    NULL);

	/*
	 * XXX IOHC and friends clock gating.
	 */

	/*
	 * With that done, proceed to initialize the IOAPIC in each IOMS. While
	 * the FCH contains what the OS generally thinks of as the IOAPIC, we
	 * need to go through and deal with interrupt routing and how that
	 * interface with each of the northbridges here.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_ioapic, NULL);

	/*
	 * XXX For some reason programming IOHC::NB_BUS_NUM_CNTL is lopped in
	 * with the IOAPIC initialization. We may want to do this, but it can at
	 * least be its own function.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_bus_num, NULL);

	/*
	 * Go through and configure all of the straps for NBIF devices before
	 * they end up starting up.
	 *
	 * XXX There's a bunch we're punting on here and we'll want to make sure
	 * that we actually have the platform's config for this. But this
	 * includes doing things like:
	 *
	 *  o Enabling and Disabling devices visibility through straps and their
	 *    interrupt lines.
	 *  o Device multi-function enable, related PCI config space straps.
	 *  o Lots of clock gating
	 *  o Subsystem IDs
	 *  o GMI round robin
	 *  o BIFC stuff
	 */

	/* XXX Need a way to know which devs to enable on the board */
	milan_fabric_walk_nbif(fabric, milan_fabric_init_nbif_dev_straps, NULL);

	/*
	 * To wrap up the nBIF devices, go through and update the bridges here.
	 * We do two passes, one to get the NBIF instances and another to deal
	 * with the special instance that we believe is for the southbridge.
	 */
	milan_fabric_walk_ioms(fabric, milan_fabric_init_nbif_bridge, NULL);

	/*
	 * Currently we do all of our initial DXIO training for PCIe before we
	 * enable features that have to do with the SMU. XXX Cargo Culting.
	 */

	/*
	 * It's time to begin the dxio initialization process. We do this in a
	 * few different steps:
	 *
	 *   1. Program all of the misc. settings and variables that it wants
	 *	before we begin to load data anywhere.
	 *   2. Construct the per-die payloads that we require and assemble
	 *	them.
	 *   3. Actually program all of the different payloads we need.
	 *   4. Go back and set a bunch more things that probably can all be
	 *	done in (1) when we're done aping.
	 *   5. Make the appropriate sacrifice to the link training gods.
	 *   6. Kick off and process the state machines, one I/O die at a time.
	 *
	 * XXX htf do we want to handle errors
	 */
	if (milan_fabric_walk_iodie(fabric, milan_dxio_init, NULL) != 0) {
		cmn_err(CE_WARN, "DXIO Initialization failed: lasciate ogni "
		    "speranza voi che pcie");
		return;
	}

	if (milan_fabric_walk_iodie(fabric, milan_dxio_plat_data, NULL) != 0) {
		cmn_err(CE_WARN, "DXIO Initialization failed: no platform "
		    "data");
		return;
	}

	if (milan_fabric_walk_iodie(fabric, milan_dxio_load_data, NULL) != 0) {
		cmn_err(CE_WARN, "DXIO Initialization failed: failed to load "
		    "data into dxio");
		return;
	}

	if (milan_fabric_walk_iodie(fabric, milan_dxio_more_conf, NULL) != 0) {
		cmn_err(CE_WARN, "DXIO Initialization failed: failed to do yet "
		    "more configuration");
		return;
	}

	if (milan_fabric_walk_iodie(fabric, milan_dxio_state_machine, NULL) !=
	    0) {
		cmn_err(CE_WARN, "DXIO Initialization failed: failed to walk "
		    "through the state machine");
		return;
	}

	cmn_err(CE_NOTE, "DXIO devices successfully trained?");

	/*
	 * Now that we have successfully trained devices, it's time to go
	 * through and set up the bridges so that way we can actual handle them
	 * aborting transactions and related.
	 */
	milan_fabric_walk_pcie_port(fabric, milan_fabric_init_pcie_ports, NULL);
	milan_fabric_walk_bridge(fabric, milan_fabric_init_bridges, NULL);

	/*
	 * XXX This is a terrible hack. We should really fix pci_boot.c and we
	 * better before we go to market.
	 */
	milan_fabric_hack_bridges(fabric);

	/*
	 * At this point, go talk to the SMU to actually initialize our hotplug
	 * support.
	 */
	if (!milan_hotplug_init(fabric)) {
		cmn_err(CE_WARN, "Eh, just don't unplug anything. I'm sure it "
		    "will be fine. Not like someone's going to come and steal "
		    "your silmarils");
	}

	/*
	 * XXX At some point, maybe not here, but before we really go too much
	 * futher we should lock all the various MMIO assignment registers,
	 * especially ones we don't intend to use.
	 */
}
