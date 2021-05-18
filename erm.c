#define _XOPEN_SOURCE 500 /* nftw */
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ftw_cb(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	if (typeflag == FTW_D || typeflag == FTW_DP) return rmdir(fpath);
	else return unlink(fpath);
}

struct task {
	pthread_t thread;
	const char *path;
	int rv;
} *task_list = NULL;

void *run_ftw(void *arg)
{
	struct task *t = arg;
	t->rv = nftw(t->path, ftw_cb, 20, FTW_DEPTH|FTW_PHYS);
	return NULL;
}
static int recurse_into(const char *path, int c)
{
	task_list[c].path = path;
	return pthread_create(&task_list[c].thread, NULL, run_ftw, &task_list[c]);
}
static int join_thread(const char *path, int c)
{
	pthread_join(task_list[c].thread, NULL);
	return task_list[c].rv;
}

static int single_file(const char *path, int c)
{
	(void)c;
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

	if (recursive) {
		task_list = calloc(sizeof(*task_list), argc);
		if (!task_list) {
			perror("malloc");
			return 5;
		}
	}
	int (*action)(const char *, int) = recursive ? recurse_into : single_file;
	int (*callback)(const char *, int) = recursive ? join_thread : NULL;
	const char *err_fmt = recursive ?
		"failed to delve into '%s': %s\n" : "failed to remove '%s': %s\n";

	int rv = 0;
	for (int i = 0; i < argc; i++) {
		const char *path = argv[i];
		if (action(path, i)) {
			fprintf(stderr, err_fmt, path, strerror(errno));
			if (stop_at_error) {
				return 1;
			} else {
				rv = 1;
			}
		}
	}
	if (callback) {
		for (int i = 0; i < argc; i++) {
			const char *path = argv[i];
			if (callback(path, i)) {
				fprintf(stderr, err_fmt, path, strerror(errno));
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
