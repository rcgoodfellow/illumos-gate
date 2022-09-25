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

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/poll.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/tofino.h>
#include <sys/tofino_tbus.h>
#include "tofino_impl.h"

static dev_info_t *tofino_dip;
static void *tofino_statep;

/*
 * We only support a single tofino device for now
 */
#define	TOFINO_MINOR_NO	0

typedef struct tofino_devstate {
	kmutex_t		ts_mutex;
	uint32_t		ts_open;
} tofino_devstate_t;


void *
tofino_tbus_tx_alloc(tofino_tbus_cookie_t cookie, size_t sz)
{
	return (NULL);
}

void
tofino_tbus_tx_free(tofino_tbus_cookie_t cookie, void *addr)
{
}

int
tofino_tbus_tx(tofino_tbus_cookie_t cookie, void *addr, size_t sz)
{
	return (0);
}

void
tofino_tbus_rx_done(tofino_tbus_cookie_t cookie, void *addr, size_t sz)
{
}

tf_tbus_handler_t *
tf_tbus_reg_handler(tf_tbus_t *tbp, tofino_rx_handler_t rx,
    tofino_cmp_handler_t cmp, void *arg)
{
	return (NULL);
}

int
tofino_tbus_register(int dev_id, tofino_tbus_cookie_t *cookiep, void *arg,
    tofino_rx_handler_t rx, tofino_cmp_handler_t cmp)
{
	return (0);
}

int
tofino_tbus_unregister(int dev_id, tofino_tbus_cookie_t cookie)
{
	return (0);
}

static int
tofino_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
	tofino_devstate_t *tsp;
	int minor;
	int rval = 0;

	if ((minor = getminor(*devp)) != TOFINO_MINOR_NO) {
		return (ENXIO);
	}

	if (otyp != OTYP_CHR)
		return (EINVAL);

	tsp = ddi_get_soft_state(tofino_statep, minor);
	ASSERT(tsp != NULL);

	mutex_enter(&tsp->ts_mutex);
	tsp->ts_open++;
	mutex_exit(&tsp->ts_mutex);
	cmn_err(CE_NOTE, "%s() %d\n", __func__, tsp->ts_open);

	return (rval);
}

static uint32_t portcnt = 0;

static int
tofino_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp,
    int *rvalp)
{
	tofino_t *tf;
	tofino_devstate_t *tsp;

	tsp = ddi_get_soft_state(tofino_statep, TOFINO_MINOR_NO);
	ASSERT(tsp != NULL);

	tf = ddi_get_driver_private(tofino_dip);
	ASSERT(tf != NULL);

	cmn_err(CE_NOTE, "tfctl_ioctl(%d) - unrecognized command", cmd);
	return (ENOTTY);
}

static int
tofino_read(dev_t dev, struct uio *uiop, cred_t *credp)
{
	return (ENOTSUP);
}

static int
tofino_write(dev_t dev, struct uio *uiop, cred_t *credp)
{
	return (ENOTSUP);
}

static int
tofino_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
	return (0);
}

static int
tofino_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	tofino_devstate_t *tsp;
	int minor;

	minor = getminor(dev);
	tsp = ddi_get_soft_state(tofino_statep, minor);
	ASSERT(tsp != NULL);
	mutex_enter(&tsp->ts_mutex);
	if (tsp->ts_open > 0) {
		tsp->ts_open--;
	}
	mutex_exit(&tsp->ts_mutex);
	cmn_err(CE_NOTE, "%s() %d\n", __func__, tsp->ts_open);
	return (0);
}

static bool
tofino_minor_create(tofino_t *tf)
{
	minor_t m = (minor_t)ddi_get_instance(tf->tf_dip);

	dev_err(tf->tf_dip, CE_NOTE, "creating minor node");
	if (ddi_create_minor_node(tf->tf_dip, "tofino", S_IFCHR, m, DDI_PSEUDO,
	    0) != DDI_SUCCESS) {
		dev_err(tf->tf_dip, CE_WARN, "failed to create minor nodes");
		return (false);
	}

	dev_err(tf->tf_dip, CE_NOTE, "created minor node");
	return (true);
}

static void
tofino_cleanup(tofino_t *tf)
{
	ddi_remove_minor_node(tf->tf_dip, NULL);
	kmem_free(tf, sizeof (*tf));
}

static int
tofino_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	tofino_t *tf;

	dev_err(dip, CE_NOTE, "attaching tofino driver");
	if (cmd != DDI_ATTACH) {
		return (DDI_FAILURE);
	}

	tf = kmem_zalloc(sizeof (*tf), KM_SLEEP);
	tf->tf_dip = dip;

	if (!tofino_minor_create(tf))
		goto cleanup;

	ddi_set_driver_private(dip, tf);
	dev_err(dip, CE_NOTE, "%s(): tofino driver attached", __func__);
	tofino_dip = dip;
	return (DDI_SUCCESS);

cleanup:
	tofino_cleanup(tf);
	return (DDI_FAILURE);
}

static int
tofino_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **resultp)
{
	int error = DDI_FAILURE;

	dev_err(dip, CE_NOTE, "%s(%d)", __func__, cmd);
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		if (getminor((dev_t)arg) == 0 && tofino_dip != NULL) {
			*resultp = (void *) tofino_dip;
			error = DDI_SUCCESS;
		}
		break;

	case DDI_INFO_DEVT2INSTANCE:
		if (getminor((dev_t)arg) == 0) {
			*resultp = (void *)0;
			error = DDI_SUCCESS;
		}
		break;

	default:
		break;
	}

	return (error);
}

static int
tofino_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	tofino_t *tf;

	if (portcnt > 0)
		return (DDI_FAILURE);

	dev_err(dip, CE_NOTE, "%s(%d)", __func__, cmd);
	if (cmd != DDI_DETACH) {
		return (DDI_FAILURE);
	}

	if (dip != tofino_dip) {
		dev_err(dip, CE_WARN, "asked to detach a different dev_info_t");
		return (DDI_FAILURE);
	}

	tf = ddi_get_driver_private(dip);
	if (tf == NULL) {
		dev_err(dip, CE_WARN, "asked to detach but no private data");
		return (DDI_FAILURE);
	}

	ddi_set_driver_private(dip, NULL);
	tofino_cleanup(tf);
	tofino_dip = NULL;
	return (DDI_SUCCESS);
}

static struct cb_ops tofino_cb_ops = {
	.cb_open =			tofino_open,
	.cb_close =			tofino_close,
	.cb_strategy =			nodev,
	.cb_print =			nodev,
	.cb_dump =			nodev,
	.cb_read =			tofino_read,
	.cb_write =			tofino_write,
	.cb_ioctl =			tofino_ioctl,
	.cb_devmap =			nodev,
	.cb_mmap =			nodev,
	.cb_segmap =			nodev,
	.cb_chpoll =			tofino_chpoll,
	.cb_prop_op =			ddi_prop_op,
	.cb_flag =			D_MP | D_DEVMAP,
	.cb_rev =			CB_REV,
	.cb_aread =			nodev,
	.cb_awrite =			nodev
};

static struct dev_ops tofino_dev_ops = {
	.devo_rev =			DEVO_REV,
	.devo_getinfo =			tofino_getinfo,
	.devo_identify =		nulldev,
	.devo_probe =			nulldev,
	.devo_attach =			tofino_attach,
	.devo_detach =			tofino_detach,
	.devo_reset =			nodev,
	.devo_quiesce =			ddi_quiesce_not_supported,
	.devo_cb_ops =			&tofino_cb_ops
};

static struct modldrv tofino_modldrv = {
	.drv_modops =			&mod_driverops,
	.drv_linkinfo =			"Tofino Stub Driver",
	.drv_dev_ops =			&tofino_dev_ops
};

static struct modlinkage tofino_modlinkage = {
	.ml_rev =			MODREV_1,
	.ml_linkage =			{ &tofino_modldrv, NULL },
};

int
_init(void)
{
	tofino_devstate_t *tsp;
	int e;

	if ((e = ddi_soft_state_init(&tofino_statep,
	    sizeof (tofino_devstate_t), 0)) != 0) {
		cmn_err(CE_NOTE, "%s() - failed ddi_soft_state_init()",
		    __func__);
		return (e);
	}

	/*
	 * We only have a single minor node, so we preallocate its state here
	 */
	e = ddi_soft_state_zalloc(tofino_statep, TOFINO_MINOR_NO);
	if (e == DDI_FAILURE) {
		cmn_err(CE_NOTE, "%s() - failed ddi_soft_state_zalloc()",
		    __func__);
		ddi_soft_state_fini(&tofino_statep);
		return (e);
	}
	tsp = ddi_get_soft_state(tofino_statep, TOFINO_MINOR_NO);
	mutex_init(&tsp->ts_mutex, NULL, MUTEX_DRIVER, NULL);

	if ((e = mod_install(&tofino_modlinkage)) != 0) {
		cmn_err(CE_NOTE, "%s() - failed mod_install()", __func__);
	}
	return (e);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&tofino_modlinkage, modinfop));
}

int
_fini(void)
{
	tofino_devstate_t *tsp;

	tsp = ddi_get_soft_state(tofino_statep, TOFINO_MINOR_NO);
	mutex_enter(&tsp->ts_mutex);
	if (tsp->ts_open > 0) {
		mutex_exit(&tsp->ts_mutex);
		return (EBUSY);
	}
	mutex_exit(&tsp->ts_mutex);
	mutex_destroy(&tsp->ts_mutex);
	ddi_soft_state_free(tofino_statep, 0);

	ddi_soft_state_fini(&tofino_statep);
	return (mod_remove(&tofino_modlinkage));
}
