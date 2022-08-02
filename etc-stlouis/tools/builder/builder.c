/*
 * Copyright (c) 2010 Joyent Inc., All rights reserved.
 * Copyright 2022 Oxide Computer Co.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define	MAX_DIRS	10
#define	MAX_LINE_LEN	1024

/* Globals! */
char *search_dirs[MAX_DIRS] = { NULL };

static int exit_status = 0;

/* These are from the generated users.c */
extern uid_t uid_from_name(const char *);
extern gid_t gid_from_name(const char *);

/* From file_cp.c */
extern int file_cp(const char *, const char *);

/* Convert a string like "0755" to the octal mode_t equivalent */
static mode_t
str_to_mode(const char *mode)
{
	long result;
	char *p;
	errno = 0;

	result = strtol(mode, &p, 8);
	if (errno != 0 || *p != 0 || p == mode) {
		perror("converting string to octal");
		return ((mode_t)-1);
	}

	return ((mode_t)result);
}

static void
handle_dir(const char *target, const char *mode,
    const char *user, const char *group)
{
	int r;
	mode_t m;

	m = str_to_mode(mode);
	(void) printf("DIR: [%s][%04o][%s/%d][%s/%d]: ",
	    target, (uint_t)m, user, uid_from_name(user),
	    group, gid_from_name(group));

	r = mkdir(target, m);
	if (r != 0 && errno != EEXIST) {
		perror("mkdir()");
		exit(1);
	}

	r = chown(target, (uid_t)uid_from_name(user),
	    (gid_t)gid_from_name(group));
	if (r != 0) {
		perror("chown()");
		exit(1);
	}

	(void) printf("OK\n");
}

static void
handle_file(const char *source, const char *target, const char *mode,
    const char *user, const char *group)
{
	int i, r;
	mode_t m;
	char *found, testfile[MAX_LINE_LEN];

	if (source == NULL)
		source = target;

	found = NULL;
	m = str_to_mode(mode);
	(void) printf("FILE: [%s->%s][%04o][%s/%d][%s/%d]: ",
	    source, target, (uint_t)m, user, uid_from_name(user),
	    group, gid_from_name(group));

	for (i = 0; i < MAX_DIRS && search_dirs[i] != NULL; i++) {
		r = snprintf(testfile, sizeof (testfile),
		    "%s/%s", search_dirs[i], source);
		if (r < 0) {
			perror("snprintf");
			exit(1);
		}
		if ((size_t)r >= sizeof (testfile)) {
			(void) fprintf(stderr, "file origin %s/%s too long\n",
			    search_dirs[i], source);
			exit(1);
		}

		r = file_cp(target, testfile);
		if (r < 0) {
			if (errno != ENOENT) {
				perror("file_cp()");
				exit(1);
			}
		} else {
			found = search_dirs[i];
			break;
		}
	}

	if (found != NULL) {
		r = chown(target, (uid_t)uid_from_name(user),
		    (gid_t)gid_from_name(group));
		if (r != 0) {
			perror("chown()");
			exit(1);
		}

		r = chmod(target, m);
		if (r != 0) {
			perror("chmod()");
			exit(1);
		}

		/* tell where we found it */
		(void) printf("OK (%s)\n", found);
	} else {
		(void) printf("FAILED\n");
		exit_status = 1;
	}
}

static void
handle_rename(char *target, const char *mode,
    const char *user, const char *group)
{
	const char *dst;

	dst = strsep(&target, "=");
	if (target == NULL || *dst == '\0') {
		(void) printf("invalid renamed file: '%s'\n", dst);
		exit(1);
	}

	handle_file(target, dst, mode, user, group);
}

static void
handle_link(char *target, const char *type,
    int (*linker)(const char *, const char *))
{
	const char *newpath = strsep(&target, "=");

	if (newpath == NULL || *target == '\0') {
		(void) printf("invalid %s target: '%s'\n", type, newpath);
		exit(1);
	}

	(void) printf("LINK(%s): %s => %s: ", type, newpath, target);

	if (linker(target, newpath) != 0) {
		(void) fprintf(stderr, "linker(%s, %s): ", newpath, target);
		perror(type);
		exit(1);
	}

	(void) printf("OK\n");
}

static void
bad_args(uint_t lineno, const char *kind, const char *text)
{
	(void) printf("Wrong number of arguments for %s on line[%d]: %s\n",
	    kind, lineno, text);
}

int
main(int argc, char *argv[])
{
	FILE *file;
	uint_t i, lineno = 0, pass = 1;
	int args_found;
	char type, line[MAX_LINE_LEN], target[MAX_LINE_LEN];
	char mode[MAX_LINE_LEN], user[MAX_LINE_LEN], group[MAX_LINE_LEN];
	char *manifest, *output;

	/*CSTYLED*/
	static const char *USAGE_FMT =
	"Usage: %s <manifest> <output dir> <dir1> [<dir2> ... <dirX>]\n\n"
	" * Use only absolute paths\n"
	" * Directories are searched in order listed, stop at first match\n"
	" * MAX_DIRS=%d, modify and recompile if you need more\n\n";

	if (geteuid() != 0) {
		(void) printf("euid must be 0 to use this tool.\n");
		exit(1);
	}

	if ((argc < 4) || (argc > (MAX_DIRS + 3))) {
		(void) printf(USAGE_FMT, argv[0], MAX_DIRS);
		exit(1);
	}

	/*
	 * It is possible to invoke this with a umask that does not include
	 * user permissions, which will cause confusing breakage.  This is the
	 * most conservative umask that will work.  Calling software should be
	 * protecting the entire rootfs prior to our invocation, as we will be
	 * creating setXid files.
	 */
	(void) umask(0077);

	manifest = argv[1];
	output = argv[2];
	for (i = 3; i < MAX_DIRS + 3; i++) {
		search_dirs[i - 3] = argv[i];
	}

	(void) printf("MANIFEST:	 %s\n", manifest);
	(void) printf("OUTPUT:		 %s\n", output);

	for (i = 0; i < MAX_DIRS && search_dirs[i] != NULL; i++) {
		(void) printf("SEARCH[%02d]: %s\n", i, search_dirs[i]);
	}

	if (chdir(output) != 0) {
		perror("failed to chdir(<output dir>)");
		exit(1);
	}

	while (pass < 6) {
		file = fopen(manifest, "r");
		if (file == NULL) {
			perror(manifest);
			exit(1);
		}

		while (fgets(line, MAX_LINE_LEN, file) != NULL) {
			lineno++;
			args_found = sscanf(line, "%c %s %s %s %s",
			    &type, target, mode, user, group);
			if (args_found == EOF) {
				perror("scanf");
				exit(1);
			}
			switch (type) {
			case 'd':
				if (args_found != 5) {
					bad_args(lineno, "directory", line);
					exit_status = 1;
					continue;
				}
				if (pass == 1) {
					handle_dir(target, mode, user, group);
				} else if (pass == 5) {
					/*
					 * Set permissions last, in case the
					 * mode is read-only and we need to
					 * populate it.
					 */
					mode_t m;
					m = str_to_mode(mode);
					if (chmod(target, m) != 0) {
						perror("chmod()");
						exit(1);
					}
				}
				break;
			case 'f':
				if (args_found != 5) {
					bad_args(lineno, "file", line);
					exit_status = 1;
					continue;
				}
				if (pass == 2) {
					handle_file(target, target,
					    mode, user, group);
				}
				break;
			case 's':
				if (args_found != 2) {
					bad_args(lineno, "symlink", line);
					exit_status = 1;
					continue;
				}
				if (pass == 3) {
					handle_link(target, "symlink", symlink);
				}
				break;
			case 'h':
				if (args_found != 2) {
					bad_args(lineno, "link", line);
					exit_status = 1;
					continue;
				}
				if (pass == 4) {
					handle_link(target, "link", link);
				}
				break;
			case 'r':	/* Like 'f' but source != target */
				if (args_found != 5) {
					bad_args(lineno, "file-rename", line);
					exit_status = 1;
					continue;
				}
				if (pass == 2) {
					handle_rename(target,
					    mode, user, group);
				}
				break;
			default:
				(void) printf("Invalid type (%c) on line[%d]: "
				    "%s\n", type, lineno, line);
				break;
			}
		}
		fclose(file);
		pass++;
	}

	exit(exit_status);
}
