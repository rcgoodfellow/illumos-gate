/*
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 */

/*
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/policy.h>
#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/dls.h>
#include <sys/mac_ether.h>
#include <sys/mac_provider.h>
#include <sys/vlan.h>
#include <sys/list.h>
#include <sys/mac_impl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sunddi.h>
#include <sys/strsun.h>
#include <sys/tofino.h>
#include "tfpkt.h"
#include "tfpkt_impl.h"

#define	ETHSZ (sizeof (struct ether_header))
#define	SCSZ (sizeof (struct schdr))

static tfpkt_t *tfpkt;
static dev_info_t *tfpkt_dip;

static int tfpkt_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int tfpkt_attach(dev_info_t *, ddi_attach_cmd_t);
static int tfpkt_detach(dev_info_t *, ddi_detach_cmd_t);
static int tfpkt_probe(dev_info_t *);

/* MAC callback function declarations */
static int tfpkt_m_start(void *);
static void tfpkt_m_stop(void *);
static int tfpkt_m_promisc(void *, boolean_t);
static int tfpkt_m_multicst(void *, boolean_t, const uint8_t *);
static int tfpkt_m_unicst(void *, const uint8_t *);
static int tfpkt_m_stat(void *, uint_t, uint64_t *);
static void tfpkt_m_ioctl(void *, queue_t *, mblk_t *);
static mblk_t *tfpkt_m_tx(void *, mblk_t *);

DDI_DEFINE_STREAM_OPS(tfpkt_dev_ops, nulldev, tfpkt_probe, tfpkt_attach,
    tfpkt_detach, nodev, tfpkt_getinfo, D_MP, NULL,
    ddi_quiesce_not_supported);

static mac_callbacks_t tfpkt_m_callbacks = {
	.mc_callbacks =			MC_IOCTL,
	.mc_getstat =			tfpkt_m_stat,
	.mc_start =			tfpkt_m_start,
	.mc_stop =			tfpkt_m_stop,
	.mc_setpromisc =		tfpkt_m_promisc,
	.mc_multicst =			tfpkt_m_multicst,
	.mc_unicst =			tfpkt_m_unicst,
	.mc_tx =			tfpkt_m_tx,
	.mc_ioctl =			tfpkt_m_ioctl,
};

static int
tfpkt_getinfo(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg,
    void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = tfpkt_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = NULL;
		return (DDI_SUCCESS);
	}
	return (DDI_FAILURE);
}

static int
tfpkt_probe(dev_info_t *dip)
{
	return (DDI_PROBE_SUCCESS);
}

static int
tfpkt_init_mac(tfpkt_t *tfp)
{
	mac_register_t *mac;
	uint8_t mac_addr[ETHERADDRL] = {2, 4, 6, 8, 10, 12};
	int rval;

	cmn_err(CE_NOTE, "%s:%d", __func__, __LINE__);
	mac = mac_alloc(MAC_VERSION);
	if (mac == NULL) {
		return (ENOMEM);
	}
	cmn_err(CE_NOTE, "%s:%d", __func__, __LINE__);

	/* Register the new device with the mac(9e) framework */
	mac->m_driver = tfp;
	mac->m_dip = tfp->tfp_dip;
	mac->m_instance = tfp->tfp_instance;
	mac->m_src_addr = mac_addr;
	mac->m_callbacks = &tfpkt_m_callbacks;
	mac->m_min_sdu = 0;
	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_max_sdu = ETHERMTU;
	mac->m_margin = VLAN_TAGSZ;
	rval = mac_register(mac, &tfp->tfp_mh);
	mac_free(mac);

	if (rval == 0) {
		cmn_err(CE_NOTE, "registered with mac");
		mac_link_update(tfp->tfp_mh, LINK_STATE_UP);
		mac_tx_update(tfp->tfp_mh);
	} else {
		dev_err(tfp->tfp_dip, CE_WARN, "failed to register packet "
		    "driver");
	}

	return (rval);
}

static boolean_t
tfpkt_minor_create(dev_info_t *dip, int instance)
{
	minor_t m = (minor_t)instance;

	dev_err(dip, CE_NOTE, "creating tfpkt %d", instance);
	if (ddi_create_minor_node(dip, "tfpkt", S_IFCHR, m, DDI_PSEUDO,
	    0) != DDI_SUCCESS) {
		dev_err(dip, CE_WARN, "failed to create minor node %d",
		    instance);
		return (B_FALSE);
	}

#if 0
	if (ddi_soft_state_zalloc(tofino_state, m) == DDI_FAILURE) {
		ddi_remove_minor_node(tf->tf_dip, NULL);
		return (B_FALSE);
	}
	top = ddi_get_soft_state(tofino_state, m);
	mutex_init(&top->to_mutex, NULL, MUTEX_DRIVER, NULL);
	top->to_device = tf;
	top->to_pages = NULL;
#endif

	return (B_TRUE);
}

static int
tfpkt_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int instance = ddi_get_instance(dip);

	dev_err(dip, CE_NOTE, "%s", __func__);
	if (cmd != DDI_ATTACH) {
		return (DDI_FAILURE);
	}

	ASSERT(tfpkt != NULL);
	ASSERT3P(tfpkt_dip, ==, NULL);

	if (!tfpkt_minor_create(dip, instance))
		return (DDI_FAILURE);

	tfpkt_dip = dip;
	tfpkt->tfp_dip = dip;
	ddi_set_driver_private(dip, tfpkt);

	if (tf_tbus_init(tfpkt) != 0)
		goto fail0;

	if (tfpkt_init_mac(tfpkt) != 0)
		goto fail1;

	dev_err(dip, CE_NOTE, "%s() - success", __func__);
	return (DDI_SUCCESS);

fail1:
	tf_tbus_fini(tfpkt->tfp_tbus_state);

fail0:
	tfpkt->tfp_dip = NULL;
	ddi_set_driver_private(dip, NULL);
	ddi_remove_minor_node(dip, "tfpkt");
	return (DDI_FAILURE);
}

static int
tfpkt_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	int r;
	tfpkt_t *tfp;

	switch (cmd) {
	case DDI_DETACH:
		tfp = (tfpkt_t *)ddi_get_driver_private(dip);
		ASSERT(tfp == tfpkt);
		dev_err(dip, CE_NOTE, "unregistering from mac");
		if ((r = mac_unregister(tfp->tfp_mh)) != 0) {
			dev_err(dip, CE_NOTE, "mac unregister failed %d", r);
			return (DDI_FAILURE);
		}

		dev_err(dip, CE_NOTE, "unregistering from tofino");
		tf_tbus_fini(tfp->tfp_tbus_state);

		dev_err(dip, CE_NOTE, "removing tfpkt minor");
		ddi_remove_minor_node(dip, "tfpkt");
		tfpkt_dip = NULL;
		return (DDI_SUCCESS);

	case DDI_SUSPEND:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}
}

static int
tfpkt_tx_one(tfpkt_t *tfp, mblk_t *mp_head)
{
	tf_tbus_t *tbp = tfp->tfp_tbus_state;
	size_t full_sz = msgsize(mp_head);
	struct ether_header *eth = NULL;
	caddr_t tx_buf, tx_wp;

	/* Drop packets without a sidecar header */
	eth = (struct ether_header *)mp_head->b_rptr;
	if (ntohs(eth->ether_type) != ETHERTYPE_SIDECAR) {
		cmn_err(CE_WARN, "dropping non-sidecar packet");
		freeb(mp_head);
		return (0);
	}

	if ((tx_buf = tofino_tbus_tx_alloc(tbp, full_sz)) == NULL)
		return (-1);

	tx_wp = tx_buf;

	/* Copy the ethernet header into the transfer buffer */
	bcopy(mp_head->b_rptr, tx_wp, ETHSZ);
	tx_wp += ETHSZ;

	/*
	 * Copy the rest of the packet into the tx buffer, skipping over the
	 * ethernet header we've already copied.
	 */
	size_t skip = ETHSZ;
	for (mblk_t *m = mp_head; m != NULL; m = m->b_cont) {
		size_t sz = MBLKL(m) - skip;

		bcopy(m->b_rptr + skip, tx_wp, sz);
		tx_wp += sz;
		skip = 0;
	}

	if (tofino_tbus_tx(tbp, tx_buf, full_sz) != 0) {
		tofino_tbus_tx_free(tbp, tx_buf);
		return (-1);
	}

	freeb(mp_head);
	return (0);
}

static mblk_t *
tfpkt_m_tx(void *arg, mblk_t *mp_chain)
{
	tfpkt_t *tfp = arg;
	mblk_t *mp, *next;

	for (mp = mp_chain; mp != NULL; mp = next) {
		next = mp->b_next;
		if (tfpkt_tx_one(tfp, mp) != 0) {
			/*
			 * XXX: call mac_tx_update() when more tx_bufs
			 * become available
			 */
			return (mp);
		}
	}

	return (NULL);
}

static void
tfpkt_m_ioctl(void *arg, queue_t *q, mblk_t *mp)
{
	cmn_err(CE_NOTE, "%s", __func__);
	miocnak(q, mp, 0, ENOTSUP);
}

static int
tfpkt_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	tfpkt_t *tfp = arg;
	int rval = 0;

	ASSERT(tfp->tfp_mh != NULL);

	switch (stat) {
	case MAC_STAT_IFSPEED:
		*val = 100 * 1000000ull; /* 100 Mbps */
		break;
	case MAC_STAT_LINK_STATE:
		*val = LINK_DUPLEX_FULL;
		break;
	case MAC_STAT_LINK_UP:
		if (tfp->tfp_runstate == TFPKT_RUNSTATE_RUNNING)
			*val = LINK_STATE_UP;
		else
			*val = LINK_STATE_DOWN;
		break;
	case MAC_STAT_PROMISC:
	case MAC_STAT_MULTIRCV:
	case MAC_STAT_MULTIXMT:
	case MAC_STAT_BRDCSTRCV:
	case MAC_STAT_BRDCSTXMT:
		rval = ENOTSUP;
		break;
	case MAC_STAT_OPACKETS:
		*val = tfp->tfp_stats.tfs_xmit_count;
		break;
	case MAC_STAT_OBYTES:
		*val = tfp->tfp_stats.tfs_obytes;
		break;
	case MAC_STAT_IERRORS:
		*val = tfp->tfp_stats.tfs_recv_errors;
		break;
	case MAC_STAT_OERRORS:
		*val = tfp->tfp_stats.tfs_xmit_errors;
		break;
	case MAC_STAT_RBYTES:
		*val = tfp->tfp_stats.tfs_rbytes;
		break;
	case MAC_STAT_IPACKETS:
		*val = tfp->tfp_stats.tfs_recv_count;
		break;
	default:
		rval = ENOTSUP;
		break;
	}

	return (rval);
}

static int
tfpkt_m_start(void *arg)
{
	tfpkt_t *tfp = arg;

	tfp->tfp_runstate = TFPKT_RUNSTATE_RUNNING;
	return (0);
}

static void
tfpkt_m_stop(void *arg)
{
	tfpkt_t *tfp = arg;

	tfp->tfp_runstate = TFPKT_RUNSTATE_STOPPED;
}

static int
tfpkt_m_promisc(void *arg, boolean_t on)
{
	tfpkt_t *tfp = arg;

	tfp->tfp_promisc = on;
	return (0);
}

static int
tfpkt_m_multicst(void *arg, boolean_t add, const uint8_t *addrp)
{
	return (0);
}

static int
tfpkt_m_unicst(void *arg, const uint8_t *macaddr)
{
	return (0);
}

void
tfpkt_rx(tfpkt_t *tfp, void *vaddr, size_t mblk_sz)
{
	caddr_t addr = (caddr_t)vaddr;
	mblk_t *mp;

	if (mblk_sz < ETHSZ) {
		goto done;
	}

	if ((mp = allocb(mblk_sz, 0)) == NULL) {
		dev_err(tfp->tfp_dip, CE_NOTE, "%s - allocb failed", __func__);
		goto done;
	}

	bcopy(addr, mp->b_rptr, mblk_sz);
	mp->b_wptr = mp->b_rptr + mblk_sz;
	mac_rx(tfp->tfp_mh, NULL, mp);

done:
	tofino_tbus_rx_done(tfp->tfp_tbus_state, addr, mblk_sz);
}

static struct modldrv tfpkt_modldrv = {
	.drv_modops =			&mod_driverops,
	.drv_linkinfo =			"Tofino Switch Packet Driver",
	.drv_dev_ops =			&tfpkt_dev_ops,
};

static struct modlinkage modlinkage = {
	.ml_rev =			MODREV_1,
	.ml_linkage =			{ &tfpkt_modldrv, NULL },
};

static tfpkt_t *
tfpkt_dev_alloc()
{
	tfpkt_t *tfp;

	tfp = kmem_zalloc(sizeof (*tfp), KM_NOSLEEP);
	if (tfp != NULL)
		mutex_init(&tfp->tfp_mutex, NULL, MUTEX_DRIVER, NULL);
	return (tfp);
}

static void
tfpkt_dev_free(tfpkt_t *tfp)
{
	mutex_destroy(&tfp->tfp_mutex);
	kmem_free(tfp, sizeof (*tfp));
}

int
_init(void)
{
	tfpkt_t *tfp;
	int status;

	if ((tfp = tfpkt_dev_alloc()) == NULL) {
		cmn_err(CE_NOTE, "failed to alloc tfpkt struct");
		return (ENOMEM);
	}

	mac_init_ops(&tfpkt_dev_ops, "tfpkt");
	if ((status = mod_install(&modlinkage)) == 0) {
		tfpkt = tfp;
	} else {
		cmn_err(CE_NOTE, "failed to install tfpkt");
		mac_fini_ops(&tfpkt_dev_ops);
		tfpkt_dev_free(tfp);
	}

	return (status);
}

int
_fini(void)
{
	int status;

	if ((status = mod_remove(&modlinkage)) == 0) {
		cmn_err(CE_NOTE, "unloaded tfpkt:%s()", __func__);
		mac_fini_ops(&tfpkt_dev_ops);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
