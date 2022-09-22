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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2011, 2018 by Delphix. All rights reserved.
 * Copyright 2020 Joyent, Inc.
 * Copyright 2022 Oxide Computer Company
 */

#include <mdb/mdb_ctf.h>
#include <sys/mdb_modapi.h>
#include <sys/list.h>

static int
dbgmsg_cb(uintptr_t addr, const void *unknown, void *arg)
{
	static mdb_ctf_id_t id;
	static boolean_t gotid;
	static ulong_t off, tsoff;

	time_t timestamp;
	char buf[1024];

	if (!gotid) {
		if (mdb_ctf_lookup_by_name("ipcc_dbgmsg_t", &id) == -1) {
			mdb_warn("couldn't find struct ipcc_dbgmsg");
			return (WALK_ERR);
		}

		gotid = TRUE;

		if (mdb_ctf_offsetof(id, "idm_msg", &off) == -1) {
			mdb_warn("couldn't find idm_msg");
			return (WALK_ERR);
		}
		off /= 8;

		if (mdb_ctf_offsetof(id, "idm_timestamp", &tsoff) == -1) {
			mdb_warn("couldn't find idm_timestamp");
			return (WALK_ERR);
		}
		tsoff /= 8;
	}

	if (mdb_vread(&timestamp, sizeof (timestamp), addr + tsoff) == -1) {
		mdb_warn("failed to read idm_timestamp at %p\n", addr + tsoff);
		return (DCMD_ERR);
	}

	if (mdb_readstr(buf, sizeof (buf), addr + off) == -1) {
		mdb_warn("failed to read idm_msg at %p\n", addr + off);
		return (DCMD_ERR);
	}

	mdb_printf("%Y ", timestamp);
	mdb_printf("%s\n", buf);

	return (WALK_NEXT);
}

static int
dbgmsg(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	GElf_Sym sym;

	if (mdb_lookup_by_name("ipcc_dbgmsgs", &sym)) {
		mdb_warn("failed to find ipcc_dbgmsgs");
		return (DCMD_ERR);
	}

	if (mdb_pwalk("list", dbgmsg_cb, NULL, sym.st_value) != 0) {
		mdb_warn("can't walk ipcc_dbgmsgs");
		return (DCMD_ERR);
	}

	return (DCMD_OK);
}

static const mdb_dcmd_t dcmds[] = {
	{ "ipcc_dbgmsg", "",
	    "print ipcc debug message log", dbgmsg},
	{ NULL }
};


static const mdb_modinfo_t modinfo = {
	.mi_dvers	= MDB_API_VERSION,
	.mi_dcmds	= dcmds,
	.mi_walkers	= NULL,
};

const mdb_modinfo_t *
_mdb_init(void)
{
	return (&modinfo);
}
