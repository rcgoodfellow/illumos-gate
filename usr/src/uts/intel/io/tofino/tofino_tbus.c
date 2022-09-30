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
#include <sys/stdbool.h>
#include <sys/file.h>
#include <sys/errno.h>
#include <sys/open.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/pci.h>
#include <sys/ksynch.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/tofino.h>
#include <sys/tofino_regs.h>
#include "tofino_impl.h"

extern dev_info_t *tofino_dip;
/*
 * Verifies that the driver has been attached, a tbus client has been
 * registered, and that the provided handle matches that registered client.  If
 * all of those conditions are met, return the tofino_t pointer already locked.
 */
static tofino_t *
hdl2tf(tf_tbus_hdl_t tf_hdl)
{
	tofino_t *tf = NULL;

	if (tofino_dip == NULL)
		return (NULL);

	if ((tf = ddi_get_driver_private(tofino_dip)) == NULL) {
		cmn_err(CE_NOTE, "tofino driver has no state");
		return (NULL);
	}

	mutex_enter(&tf->tf_mutex);
	if (tf->tf_tbus_client != (void *)tf_hdl) {
		mutex_exit(&tf->tf_mutex);
		return (NULL);
	}

	return (tf);
}

int
tofino_get_generation(tf_tbus_hdl_t tf_hdl)
{
	tofino_t *tf;
	int gen = -1;

	if ((tf = hdl2tf(tf_hdl)) != NULL) {
		gen = tf->tf_gen;
		mutex_exit(&tf->tf_mutex);
	}

	return (gen);
}

static ddi_dma_attr_t tf_tbus_dma_attr_buf = {
	.dma_attr_version =		DMA_ATTR_V0,
	.dma_attr_addr_lo =		0x0000000000000000,
	.dma_attr_addr_hi =		0xFFFFFFFFFFFFFFFF,
	.dma_attr_count_max =		0x00000000FFFFFFFF,
	.dma_attr_align =		0x0000000000000800,
	.dma_attr_burstsizes =		0x00000FFF,
	.dma_attr_minxfer =		1,
	.dma_attr_maxxfer =		0x00000000FFFFFFFF,
	.dma_attr_seg =			0xFFFFFFFFFFFFFFFF,
	.dma_attr_sgllen =		1,
	.dma_attr_granular =		1,
	.dma_attr_flags =		DDI_DMA_FLAGERR,
};

static ddi_device_acc_attr_t tf_tbus_acc_attr = {
	.devacc_attr_version =		DDI_DEVICE_ATTR_V1,
	.devacc_attr_endian_flags =	DDI_STRUCTURE_LE_ACC,
	.devacc_attr_dataorder =	DDI_STRICTORDER_ACC,
	.devacc_attr_access =		DDI_DEFAULT_ACC,
};

/*
 * Enable or disable all of the tbus interrupts.
 */
static void
tofino_tbus_intr_set(tofino_t *tf, bool enable)
{
	uint32_t en0 = enable ? TBUS_INT0_CPL_EVENT : 0;
	uint32_t en1 = enable ? TBUS_INT1_RX_EVENT : 0;
	uint32_t shadow_msk_base = 0xc0;
	int intr_lo = 32;
	int intr_hi = 63;

	/*
	 * Tofino defines 70 different conditions that can trigger a tbus
	 * interrupt.  We're only looking for a subset of them: those that
	 * indicate a change in the completion and/or rx descriptor rings.
	 */
	for (uint32_t intr = intr_lo; intr <= intr_hi; intr++) {
		/*
		 * XXX: This is the long, canonical way to unmask the
		 * interrupts we care about.  This whole loop works out to
		 * setting reg 0xc4 to 0.
		 */
		uint32_t intr_reg = intr >> 5;
		uint32_t intr_bit = intr & 0x1f;
		uint32_t bit_fld = (1u << intr_bit);

		uint32_t shadow_msk_reg = shadow_msk_base + (4 * intr_reg);
		uint32_t old = tf_read_reg(tf->tf_dip, shadow_msk_reg);

		uint32_t mask = old & ~bit_fld;
		tf_write_reg(tf->tf_dip, shadow_msk_reg, mask);
	}

	if (tf->tf_gen == TOFINO_G_TF1) {
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN0_1, en0);
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN1_1, en1);
	} else {
		ASSERT(tf->tf_gen == TOFINO_G_TF2);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN0_1, en0);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN1_1, en1);
	}

	/*
	 * Unconditionally disable the interrupts we're not looking for
	 */
	if (tf->tf_gen == TOFINO_G_TF1) {
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN2_1, 0);
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN0_0, 0);
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN1_0, 0);
		tf_write_reg(tf->tf_dip, TF_REG_TBUS_INT_EN2_0, 0);
	} else {
		ASSERT(tf->tf_gen == TOFINO_G_TF2);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN2_1, 0);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN0_0, 0);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN1_0, 0);
		tf_write_reg(tf->tf_dip, TF2_REG_TBUS_INT_EN2_0, 0);
	}

	tofino_log(tf, "%s interrupts", enable ? "enabled" : "disabled");
}

/*
 * Allocate a single buffer capable of DMA to/from the Tofino ASIC.
 *
 * The caller is responsible for providing an unused tf_tbus_dma_t structure,
 * which is used for tracking and managing a DMA buffer.  This routine will
 * populate that structure with all the necessary state.  Having the caller
 * provide the state structure lets us allocate them in bulk, rather than one
 * per buffer.
 */
int
tofino_tbus_dma_alloc(tf_tbus_hdl_t tf_hdl, tf_tbus_dma_t *dmap, size_t size,
    int flags)
{
	tofino_t *tf;
	unsigned int count;
	int err;

	if ((tf = hdl2tf(tf_hdl)) == NULL)
		return (-1);

	err = ddi_dma_alloc_handle(tf->tf_dip, &tf_tbus_dma_attr_buf,
	    DDI_DMA_SLEEP, NULL, &dmap->tpd_handle);
	if (err != DDI_SUCCESS) {
		tofino_err(tf, "%s: alloc_handle failed: %d", __func__, err);
		goto fail0;
	}

	err = ddi_dma_mem_alloc(dmap->tpd_handle, size, &tf_tbus_acc_attr,
	    DDI_DMA_STREAMING, DDI_DMA_SLEEP, NULL, &dmap->tpd_addr,
	    &dmap->tpd_len, &dmap->tpd_acchdl);
	if (err != DDI_SUCCESS) {
		tofino_err(tf, "%s: mem_alloc failed", __func__);
		goto fail1;
	}

	err = ddi_dma_addr_bind_handle(dmap->tpd_handle, NULL, dmap->tpd_addr,
	    dmap->tpd_len, flags, DDI_DMA_SLEEP, NULL, &dmap->tpd_cookie,
	    &count);
	if (err != DDI_DMA_MAPPED) {
		tofino_err(tf, "%s: bind_handle failed", __func__);
		goto fail2;
	}

	if (count > 1) {
		tofino_err(tf, "%s: more than one DMA cookie", __func__);
		goto fail2;
	}
	mutex_exit(&tf->tf_mutex);

	return (0);
fail2:
	ddi_dma_mem_free(&dmap->tpd_acchdl);
fail1:
	ddi_dma_free_handle(&dmap->tpd_handle);
fail0:
	mutex_exit(&tf->tf_mutex);
	return (-1);
}

/*
 * This routine frees a DMA buffer and its state, but does not free the
 * tf_tbus_dma_t structure itself.
 */
void
tofino_tbus_dma_free(tf_tbus_dma_t *dmap)
{
	VERIFY3S(ddi_dma_unbind_handle(dmap->tpd_handle), ==, DDI_SUCCESS);
	ddi_dma_mem_free(&dmap->tpd_acchdl);
	ddi_dma_free_handle(&dmap->tpd_handle);
}

int
tofino_tbus_register_softint(tf_tbus_hdl_t tf_hdl, ddi_softint_handle_t softint)
{
	tofino_t *tf;
	int rval = 0;

	if ((tf = hdl2tf(tf_hdl)) == NULL)
		return (ENXIO);

	if (tf->tf_tbus_client->tbc_tbus_softint != NULL) {
		rval = EBUSY;
	} else {
		tf->tf_tbus_client->tbc_tbus_softint = softint;
		tofino_tbus_intr_set(tf, true);
	}
	mutex_exit(&tf->tf_mutex);

	return (rval);
}

int
tofino_tbus_unregister_softint(tf_tbus_hdl_t tf_hdl,
    ddi_softint_handle_t softint)
{
	tofino_t *tf;
	int rval = 0;

	if ((tf = hdl2tf(tf_hdl)) == NULL)
		return (ENXIO);

	if (softint == tf->tf_tbus_client->tbc_tbus_softint) {
		tf->tf_tbus_client->tbc_tbus_softint = NULL;
		tofino_tbus_intr_set(tf, false);
	} else {
		rval = EINVAL;
	}
	mutex_exit(&tf->tf_mutex);

	return (rval);
}

uint32_t
tofino_read_reg(tf_tbus_hdl_t tf_hdl, size_t offset)
{
	tofino_t *tf;
	uint32_t rval;

	if ((tf = hdl2tf(tf_hdl)) == NULL) {
		rval = (ENXIO);
	} else {
		rval = tf_read_reg(tf->tf_dip, offset);
		mutex_exit(&tf->tf_mutex);
	}

	return (rval);
}

void
tofino_write_reg(tf_tbus_hdl_t tf_hdl, size_t offset, uint32_t val)
{
	tofino_t *tf;

	if ((tf = hdl2tf(tf_hdl)) != NULL) {
		tf_write_reg(tf->tf_dip, offset, val);
		mutex_exit(&tf->tf_mutex);
	}
}


/*
 * If we ever support multiple tofino ASICs in a single system, this interface
 * will need to indicate for which ASIC the caller is registering.
 */
int
tofino_tbus_register(tf_tbus_hdl_t *tf_hdl)
{
	tofino_t *tf;
	int rval = 0;

	cmn_err(CE_NOTE, "%s()", __func__);

	if (tofino_dip == NULL) {
		cmn_err(CE_NOTE, "no tofino_dip");
		return (ENXIO);
	}

	tf = ddi_get_driver_private(tofino_dip);
	if (tf == NULL) {
		cmn_err(CE_NOTE, "%s() tofino driver has no state", __func__);
		return (ENXIO);
	}

	mutex_enter(&tf->tf_mutex);
	if (tf->tf_tbus_client != NULL) {
		/* someone else is already handling the packets */
		cmn_err(CE_NOTE, "tbus already registered");
		rval = EBUSY;
	} else {
		tf->tf_tbus_client = kmem_zalloc(sizeof (tofino_tbus_client_t),
		    KM_SLEEP);
		*tf_hdl = (void *)tf->tf_tbus_client;
		cmn_err(CE_NOTE, "%s - registered %p", __func__, *tf_hdl);
	}
	mutex_exit(&tf->tf_mutex);

	return (rval);
}

int
tofino_tbus_unregister(tf_tbus_hdl_t tf_hdl)
{
	tofino_t *tf;

	cmn_err(CE_NOTE, "%s(%p)", __func__, tf_hdl);

	if ((tf = hdl2tf(tf_hdl)) == NULL) {
		cmn_err(CE_NOTE, "nonesuch");
		return (ENXIO);
	}

	kmem_free(tf->tf_tbus_client, sizeof (tofino_tbus_client_t));
	tf->tf_tbus_client = NULL;

	mutex_exit(&tf->tf_mutex);

	return (0);
}
