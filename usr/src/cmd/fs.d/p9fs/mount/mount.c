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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libintl.h>
#include <errno.h>
#include <err.h>
#include <sys/fstyp.h>
#include <sys/fsid.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#include <sys/mount.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <fslib.h>

static void usage(void);

static char optbuf[MAX_MNTOPT_STR] = { '\0', };
static int optsize = 0;

static void
selfname(const char *fstyp)
{
	char buf[64];
	snprintf(buf, sizeof (buf), "mount %s", fstyp);
	setprogname(buf);
}

/*
 * usage: mount [-Ormq] [-o options] special mountp
 *
 * This mount program is exec'ed by /usr/sbin/mount if '-F p9fs' is
 * specified.
 */
int
main(int argc, char *argv[])
{
	int c;
	char *special;		/* Entity being mounted */
	char *mountp;		/* Entity being mounted on */
	char *savedoptbuf;
	int flags = 0;
	int errflag = 0;
	int qflg = 0;

	selfname(MNTTYPE_P9FS);

	while ((c = getopt(argc, argv, "o:rmOq")) != EOF) {
		switch (c) {
		case '?':
			errflag++;
			break;

		case 'o':
			if (strlcpy(optbuf, optarg, sizeof (optbuf)) >=
			    sizeof (optbuf)) {
				errx(2, gettext("Invalid argument: %s"),
				    optarg);
			}
			optsize = strlen(optbuf);
			break;
		case 'O':
			flags |= MS_OVERLAY;
			break;
		case 'r':
			flags |= MS_RDONLY;
			break;

		case 'm':
			flags |= MS_NOMNTTAB;
			break;

		case 'q':
			qflg = 1;
			break;

		default:
			usage();
		}
	}
	if ((argc - optind != 2) || errflag) {
		usage();
	}
	special = argv[argc - 2];
	mountp = argv[argc - 1];

	if ((savedoptbuf = strdup(optbuf)) == NULL) {
		err(2, "out of memory");
	}
	if (mount(special, mountp, flags | MS_OPTIONSTR, MNTTYPE_P9FS, NULL, 0,
	    optbuf, MAX_MNTOPT_STR) != 0) {
		err(3, "mount: %s: ", special);
	}
	if (optsize != 0 && !qflg) {
		cmp_requested_to_actual_options(savedoptbuf, optbuf,
		    special, mountp);
	}
	return (0);
}

void
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: mount [-Ormq] [-o options] special mountpoint\n");
	exit(10);
}
