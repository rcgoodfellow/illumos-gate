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
 * Copyright 2020 Joyent, Inc.
 * Copyright 2022 Oxide Computer Co.
 */

#include <sys/tsc.h>
#include <sys/prom_debug.h>
#include <sys/clock.h>

/*
 * XXX We can absolutely support this; we have an HPET, we know where it is,
 * it's well-documented, etc.  When we want to implement deep C-states we'll
 * be able to reuse about 75% of what's in i86pc's io/hpet_acpi.c, but for now
 * we're steering clear of it because we don't have ACPI and we do have a
 * reliable PIT for calibration.
 */
static boolean_t
tsc_calibrate_hpet(uint64_t *freqp)
{
	PRM_POINT("HPET TSC calibration not supported on this machine.");

	return (B_FALSE);
}

static tsc_calibrate_t tsc_calibration_hpet = {
	.tscc_source = "HPET",
	.tscc_preference = 1,
	.tscc_calibrate = tsc_calibrate_hpet,
};
TSC_CALIBRATION_SOURCE(tsc_calibration_hpet);
