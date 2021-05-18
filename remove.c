#define _XOPEN_SOURCE 500 /* nftw */
#include <errno.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "erm.h"

struct task {
	pthread_t thread;
	const char *path;
	int rv;
	int created;
} *task_list = NULL;

struct task *malloc_task_list(size_t n)
{
	return task_list = calloc(sizeof(struct task), n);
}

static int ftw_cb(const char *fpath, const struct stat *sb,
		int typeflag, struct FTW *ftwbuf)
{
	if (typeflag == FTW_D || typeflag == FTW_DP) return rmdir(fpath);
	else return unlink(fpath);
}
static void *run_ftw(void *arg)
{
	struct task *t = arg;
	t->rv = nftw(t->path, ftw_cb, 20, FTW_DEPTH|FTW_PHYS) ? errno : 0;
	return NULL;
}

int recurse_into(const char *path, int c)
{
	task_list[c].path = path;
	int rv = pthread_create(&task_list[c].thread, NULL, run_ftw, &task_list[c]);
	if (!rv) task_list[c].created = 1;
	return rv;
}

int join_thread(const char *path, int c)
{
	if (!task_list[c].created) {
		errno = EINVAL;
		return -1;
	}

	pthread_join(task_list[c].thread, NULL);
	errno = task_list[c].rv;
	return errno ? -1 : 0;
}

int single_file(const char *path, int c)
{
	(void)c;
	return remove(path);
}
