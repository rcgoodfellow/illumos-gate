/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2022 Oxide Computer Company
 */

#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/debug.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/policy.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/stdbool.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <sys/ipcc.h>
#include <sys/ipcc_impl.h>

#include <ipcc_debug.h>
#include <ipcc_drv.h>

/*
 * Globals
 */

int ipcc_allow_unload = 1;

static dev_info_t *ipcc_dip;
static char *ipcc_path;
static kmutex_t ipcc_lock, ipcc_state_lock;
int ipcc_max_opens = 32;
static ipcc_t **ipcc_states;

/*
 * Allow multiple opens by allocating each minor a separate entry in the
 * ipcc_states table.
 */
static int
ipcc_open(dev_t *devp, int flag, int otyp, cred_t *cr)
{
	ipcc_t *ipcc = NULL;
	int err;
	uint_t m;

	if (getminor(*devp) != IPCC_MINOR)
		return (ENXIO);

	/*
	 * XXX What does sled agent run as?
	 */
	if (cr != kcred && (err = secpolicy_sys_config(cr, B_FALSE)) != 0)
		return (err);

	mutex_enter(&ipcc_state_lock);

	/* Find a free state slot */
	for (m = 0; m < ipcc_max_opens; m++) {
		if (ipcc_states[m] != NULL)
			continue;

		ipcc_states[m] = kmem_zalloc(sizeof (ipcc_t), KM_SLEEP);
		ipcc = ipcc_states[m];
		break;
	}

	mutex_exit(&ipcc_state_lock);

	if (ipcc == NULL) {
		cmn_err(CE_WARN, "%s: too many opens", IPCC_DRIVER_NAME);
		return (EAGAIN);
	};

	*devp = makedevice(getmajor(*devp), (minor_t)m);

	ipcc->is_cred = cr;
	crhold(cr);

	(void) ldi_ident_from_dev(*devp, &ipcc->is_ldiid);

	return (0);
}

static int
ipcc_close(dev_t dev, int flag, int otyp, cred_t *cr)
{
	uint_t m = (uint_t)getminor(dev);
	ipcc_t *ipcc;

	VERIFY3U(m, <, ipcc_max_opens);

	mutex_enter(&ipcc_state_lock);
	ipcc = ipcc_states[m];
	ipcc_states[m] = NULL;
	mutex_exit(&ipcc_state_lock);

	crfree(ipcc->is_cred);
	ldi_ident_release(ipcc->is_ldiid);
	kmem_free(ipcc, sizeof (*ipcc));

	return (0);
}

static off_t
ipcc_read(void *arg, uint8_t *buf, size_t len)
{
	ipcc_t *ipcc = arg;
	struct uio uio;
	struct iovec iov;
	int err;

	bzero(&uio, sizeof (uio));
	bzero(&iov, sizeof (iov));
	iov.iov_base = (int8_t *)buf;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = 0;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = len;

	err = ldi_read(ipcc->is_ldih, &uio, ipcc->is_cred);
	if (err < 0)
		return (err);

	return (len - iov.iov_len);
}

static off_t
ipcc_write(void *arg, uint8_t *buf, size_t len)
{
	ipcc_t *ipcc = arg;
	struct uio uio;
	struct iovec iov;
	int err;

	bzero(&uio, sizeof (uio));
	bzero(&iov, sizeof (iov));
	iov.iov_base = (int8_t *)buf;
	iov.iov_len = len;
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = 0;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_resid = len;

	err = ldi_write(ipcc->is_ldih, &uio, ipcc->is_cred);
	if (err < 0)
		return (err);

	return (len - iov.iov_len);
}

static void
ipcc_flush(void *arg)
{
	ipcc_t *ipcc = arg;
	int rval;

	(void) ldi_ioctl(ipcc->is_ldih, I_FLUSH, FLUSHRW, FKIOCTL,
	    ipcc->is_cred, &rval);
}

static const ipcc_ops_t ipcc_ops = {
	.io_flush	= ipcc_flush,
	.io_read	= ipcc_read,
	.io_write	= ipcc_write,
	.io_log		= ipcc_dbgmsg,
};


static int
ipcc_ldi_open(ipcc_t *ipcc)
{
	char mbuf[FMNAMESZ + 1];
	int err;

	err = ldi_open_by_name(ipcc_path, LDI_FLAGS,
	    ipcc->is_cred, &ipcc->is_ldih, ipcc->is_ldiid);
	if (err != 0) {
		cmn_err(CE_WARN, "ldi open of '%s' failed", ipcc_path);
		return (err);
	}

	/* XXX - not expecting anything to be autopushed on the dwu uart */
	while (ldi_ioctl(ipcc->is_ldih, I_LOOK, (intptr_t)mbuf, FKIOCTL,
	    ipcc->is_cred, &err) == 0) {
		ipcc_dbgmsg(NULL, "Popping module %s", mbuf);
		if (ldi_ioctl(ipcc->is_ldih, I_POP, 0, FKIOCTL, ipcc->is_cred,
		    &err) != 0) {
			break;
		}
	}

	return (0);
}

static int
ipcc_ldi_close(ipcc_t *ipcc)
{
	return (ldi_close(ipcc->is_ldih, LDI_FLAGS, ipcc->is_cred));
}

static int
ipcc_ioctl(dev_t dev, int cmd, intptr_t data, int mode, cred_t *cr, int *rv)
{
	uint_t m = (uint_t)getminor(dev);
	void *datap = (void *)data;
	ipcc_t *ipcc;
	int err = 0;

	if (m >= ipcc_max_opens)
		return (ENXIO);

	ipcc = ipcc_states[m];
	VERIFY3P(ipcc, !=, NULL);

	switch (cmd) {
	case IPCC_GET_VERSION:
		*rv = IPCC_VERSION;
		return (0);
	}

	mutex_enter(&ipcc_lock);

	err = ipcc_ldi_open(ipcc);
	if (err != 0) {
		mutex_exit(&ipcc_lock);
		return (err);
	}

	switch (cmd) {
	case IPCC_REBOOT:
		err = ipcc_reboot(&ipcc_ops, ipcc);
		break;
	case IPCC_POWEROFF:
		err = ipcc_poweroff(&ipcc_ops, ipcc);
		break;
	case IPCC_IDENT: {
		ipcc_ident_t ident;

		err = ipcc_ident(&ipcc_ops, ipcc, &ident);
		if (err != 0)
			break;

		if (ddi_copyout(&ident, datap, sizeof (ident), mode) != 0)
			err = EFAULT;

		break;
	}
	case IPCC_MACS: {
		ipcc_mac_t mac;

		err = ipcc_macs(&ipcc_ops, ipcc, &mac);
		if (err != 0)
			break;

		if (ddi_copyout(&mac, datap, sizeof (mac), mode) != 0)
			err = EFAULT;

		break;
	}
	case IPCC_ROT: {
		ipcc_rot_t *rot;

		rot = kmem_zalloc(sizeof (*rot), KM_NOSLEEP);
		if (rot == NULL) {
			err = ENOMEM;
			break;
		}

		if (ddi_copyin(datap, rot, sizeof (*rot), mode) != 0) {
			err = EFAULT;
			goto rot_done;
		}

		err = ipcc_rot(&ipcc_ops, ipcc, rot);
		if (err != 0)
			goto rot_done;

		if (ddi_copyout(rot, datap, sizeof (*rot), mode) != 0)
			err = EFAULT;

rot_done:
		kmem_free(rot, sizeof (*rot));
		break;
	}
	default:
		err = ENOTTY;
		break;
	}

	(void) ipcc_ldi_close(ipcc);
	mutex_exit(&ipcc_lock);
	return (err);
}

static int
ipcc_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	char *path;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	ipcc_states = kmem_zalloc(ipcc_max_opens * sizeof (ipcc_t *), KM_SLEEP);

	if (ddi_create_minor_node(dip, IPCC_NODE_NAME, S_IFCHR,
	    IPCC_MINOR, DDI_PSEUDO, 0) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "%s: Unable to create minor node",
		    IPCC_NODE_NAME);
		goto fail;
	}

	if ((ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "path", &path)) != DDI_PROP_SUCCESS) {
		cmn_err(CE_WARN, "Could not retrieve 'path' property");
		ddi_remove_minor_node(dip, NULL);
		goto fail;
	}

	ipcc_path = ddi_strdup(path, KM_SLEEP);
	if (ipcc_path == NULL) {
		ddi_remove_minor_node(dip, NULL);
		goto fail;
	}

	ipcc_dbgmsg_init();
	ddi_report_dev(dip);

	ipcc_dbgmsg(NULL, "Using '%s'", ipcc_path);

	ipcc_dip = dip;
	return (DDI_SUCCESS);

fail:
	kmem_free(ipcc_states, ipcc_max_opens * sizeof (ipcc_t *));
	return (DDI_FAILURE);
}

static int
ipcc_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	if (ipcc_allow_unload == 0)
		return (EBUSY);

	ddi_remove_minor_node(dip, NULL);
	ipcc_dip = NULL;
	strfree(ipcc_path);
	ipcc_path = NULL;
	kmem_free(ipcc_states, ipcc_max_opens * sizeof (ipcc_t *));

	ipcc_dbgmsg_fini();

	return (DDI_SUCCESS);
}

static int
ipcc_info(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **result)
{
	switch (cmd) {
	case DDI_INFO_DEVT2DEVINFO:
		if (getminor((dev_t)arg) != IPCC_MINOR)
			return (DDI_FAILURE);
		*result = ipcc_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = NULL;
		return (DDI_SUCCESS);
	default:
		return (DDI_FAILURE);
	}
}

static struct cb_ops ipcc_cb_ops = {
	.cb_open		= ipcc_open,
	.cb_close		= ipcc_close,
	.cb_strategy		= nulldev,
	.cb_print		= nulldev,
	.cb_dump		= nodev,
	.cb_read		= nodev,
	.cb_write		= nodev,
	.cb_ioctl		= ipcc_ioctl,
	.cb_devmap		= nodev,
	.cb_mmap		= nodev,
	.cb_segmap		= nodev,
	.cb_chpoll		= nochpoll,
	.cb_prop_op		= ddi_prop_op,
	.cb_str			= NULL,
	.cb_flag		= D_MP,
	.cb_rev			= CB_REV,
	.cb_aread		= nodev,
	.cb_awrite		= nodev,
};

static struct dev_ops ipcc_dev_ops = {
	.devo_rev		= DEVO_REV,
	.devo_refcnt		= 0,
	.devo_getinfo		= ipcc_info,
	.devo_identify		= nulldev,
	.devo_probe		= nulldev,
	.devo_attach		= ipcc_attach,
	.devo_detach		= ipcc_detach,
	.devo_reset		= nodev,
	.devo_cb_ops		= &ipcc_cb_ops,
	.devo_bus_ops		= NULL,
	.devo_power		= nodev,
	.devo_quiesce		= ddi_quiesce_not_needed,
};

static struct modldrv ipcc_modldrv = {
	.drv_modops		= &mod_driverops,
	.drv_linkinfo		= "SP/Host Comms Driver",
	.drv_dev_ops		= &ipcc_dev_ops
};

#ifdef IPCC_STREAMS
static struct module_info ipcc_minfo = {
	.mi_idnum		= 0,
	.mi_idname		= "ipcc",
	.mi_minpsz		= 1,
	.mi_maxpsz		= INFPSZ,
	.mi_hiwat		= 1,
	.mi_lowat		= 0,
};

static struct qinit ipcc_r_qinit = {
	.qi_putp		= ipcc_s_rput,
	.qi_srvp		= NULL,
	.qi_qopen		= NULL,
	.qi_qclose		= NULL,
	.qi_qadmin		= NULL,
	.qi_minfo		= &ipcc_minfo,
	.qi_mstat		= NULL,
	.qi_rwp			= NULL,
	.qi_infop		= NULL,
	.qi_struiot		= NULL,
};

static struct qinit ipcc_w_qinit = {
	.qi_putp		= ipcc_s_wput,
	.qi_srvp		= NULL,
	.qi_qopen		= NULL,
	.qi_qclose		= NULL,
	.qi_qadmin		= NULL,
	.qi_minfo		= &ipcc_minfo,
	.qi_mstat		= NULL,
	.qi_rwp			= NULL,
	.qi_infop		= NULL,
	.qi_struiot		= NULL,
};

static struct streamtab ipcc_strtab = {
	.st_rdinit		= &ipcc_r_qinit,
	.st_wrinit		= &ipcc_w_qinit,
	.st_muxrinit		= NULL,
	.st_muxwinit		= NULL,
};

static struct fmodsw ipcc_fmodfsw = {
	.f_name			= "ipcc",
	.f_str			= &ipcc_strtab,
	.f_flag			= D_NEW | D_MP,
};

static struct modlstrmod ipcc_modlstrmod = {
	.strmod_modops		= &mod_strmodops,
	.strmod_linkinfo	= "Oxide IPCC Driver",
	.strmod_fmodsw		= &ipcc_fmodfsw,
}
#endif

static struct modlinkage ipcc_modlinkage = {
	.ml_rev		= MODREV_1,
	.ml_linkage	= {
		&ipcc_modldrv,
#ifdef IPCC_STREAMS
		&ipcc_modlstrmod,
#endif
		NULL
	}
};

int
_init(void)
{
	int err;

	mutex_init(&ipcc_lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&ipcc_state_lock, NULL, MUTEX_DRIVER, NULL);

	err = mod_install(&ipcc_modlinkage);
	if (err != 0) {
		mutex_destroy(&ipcc_lock);
		mutex_destroy(&ipcc_state_lock);
	}

	return (err);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ipcc_modlinkage, modinfop));
}

int
_fini(void)
{
	int err;

	if (ipcc_allow_unload == 0)
		return (EBUSY);

	err = mod_remove(&ipcc_modlinkage);
	if (err == 0) {
		mutex_destroy(&ipcc_lock);
		mutex_destroy(&ipcc_state_lock);
	}

	return (err);
}
