#define _XOPEN_SOURCE 500 /* nftw */
#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ftw_cb(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	int rv;
	if (typeflag == FTW_D || typeflag == FTW_DP) rv = rmdir(fpath);
	else rv = unlink(fpath);

	return rv;
}

static int recurse_into(const char *path)
{
	return nftw(path, ftw_cb, 40, FTW_DEPTH | FTW_PHYS);
}

static int single_file(const char *path)
{
	return remove(path);
}

noreturn static void usage(int s)
{
	puts("erm [-reh] [files]");
	exit(s);
}

int main(int argc, char **argv)
{
	bool recursive = false;
	bool stop_at_error = true;

	int opt;
	while ((opt = getopt(argc, argv, "reh")) != -1) {
		switch (opt) {
			case 'r':
				recursive = true;
				break;
			case 'e':
				stop_at_error = false;
				break;
			case 'h':
				usage(0);
			default:
				usage(1);
		}
	}
	argc -= optind;
	argv += optind;

	int (*action)(const char *) = recursive ? recurse_into : single_file;
	const char *err_fmt = recursive ?
		"failed to delve into '%s': %s\n" : "failed to remove '%s': %s\n";

	int rv = 0;
	for (int i = 0; i < argc; i++) {
		const char *path = argv[i];
		if (action(path)) {
			fprintf(stderr, err_fmt, path, strerror(errno));
			if (stop_at_error) {
				return 1;
			} else {
				rv = 1;
			}
		}
	}

	return rv;
}
