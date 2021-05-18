#include <errno.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

noreturn void usage(int s)
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

	int rv = 0;
	if (recursive) {
		/* TODO: descend */
	} else {
		for (int i = 0; i < argc; i++) {
			char *path = argv[i];
			if (remove(path)) {
				fprintf(stderr, "failed to remove '%s': %s\n", path, strerror(errno));
				if (stop_at_error) {
					return 1;
				} else {
					rv = 1;
				}
			}
		}
	}

	return rv;
}
