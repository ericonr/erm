#include <errno.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "erm.h"

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
	if (argc == 0) {
		usage(1);
	}

	file_action action = recursive ? recurse_into : single_file;
	callback_action callback = recursive ? run_queue : NULL;
	const char *err_fmt = recursive ?
		"failed to queue '%s': %s\n" : "failed to remove '%s': %s\n";

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
	if (callback) callback();

	return rv;
}
