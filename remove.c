#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "erm.h"

#ifndef DEBUG
#define printf(...)
#define puts(s)
#endif

#define ACQUIRED (1U<<31)

struct task {
	char *path;
	struct task *parent;
	/* reference counting */
	unsigned files;
	atomic_uint removed_count;
};

static struct queue {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	struct task *tasks;
	size_t len, size;
	/* number of free threads */
	unsigned free;
} queue = {.mtx = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static pthread_mutex_t fd_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fd_cond = PTHREAD_COND_INITIALIZER;

static unsigned nproc;

static inline void queue_print(struct queue *q)
{
#ifdef DEBUGP
	puts("begin========================");
	for (size_t i=0; i < q->len; i++) {
		printf("item %010zu: '%s'\n", i, q->tasks[i].path);
	}
	puts("end==========================");
#else
	(void)q;
#endif
}

static inline void queue_add(struct queue *q, char *path, struct task *parent)
{
	pthread_mutex_lock(&q->mtx);
	if (q->len + 1 > q->size) {
		q->size *= 2;
		if (q->size == 0) q->size = 32;
		q->tasks = realloc(q->tasks, q->size * sizeof(struct task));
		if (!q->tasks) {
			fprintf(stderr, "queue memory exhaustion: %m\n");
			exit(1);
		}
	}

	q->tasks[q->len++] = (struct task){.path = path, .parent = parent};

	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mtx);
}

static inline void queue_remove(struct queue *q, struct task *t)
{
	pthread_mutex_lock(&q->mtx);
	while (q->len == 0) {
		if (q->free == nproc - 1) {
			/* we are done removing things */
			free(q->tasks);
			exit(0);
		}
		q->free++;
		pthread_cond_wait(&q->cond, &q->mtx);
		q->free--;
	}
	queue_print(q);
	*t = q->tasks[--(q->len)];

	/* the caller owns the path buffer now */

	pthread_mutex_unlock(&q->mtx);
}

static inline void recurse_into_parents(struct task *t)
{
	struct task *recurse = t;
	void *free_list = NULL;
	while ((recurse = recurse->parent)) {
		free(free_list); free_list = NULL;

		unsigned rc = atomic_fetch_add_explicit(&recurse->removed_count, 1, memory_order_acquire);
		if (rc & ACQUIRED) break;

		printf("parent: removed %04d total %04d '%s'\n", rc, recurse->files, recurse->path);
		if (rc == recurse->files) {
			/* we have removed all files in the directory */
			if (rmdir(recurse->path)) {
				fprintf(stderr, "rec rmdir failed '%s': %m\n", recurse->path);
			} else {
				printf("rec rmdir succeeded '%s'\n", recurse->path);
			}
			free(recurse->path);
			/* can't free now because the while condition uses it */
			free_list = recurse;
		} else {
			/* if we haven't removed this directory yet,
			 * there's no reason to recurse further */
			break;
		}
	}
	/* catch any stragglers, in case the loop doesn't iterate once more */
	free(free_list);
}

static void *process_queue_item(void *arg)
{
	struct queue *q = arg;
	struct task t;
	while (1) {
		queue_remove(q, &t);

		int dfd;
		while ((dfd = open(t.path, O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)) < 0) {
			if (errno == EMFILE) {
				pthread_mutex_lock(&fd_mtx);
				pthread_cond_wait(&fd_cond, &fd_mtx);
				pthread_mutex_unlock(&fd_mtx);
				continue;
			} else {
				fprintf(stderr, "couldn't open '%s': %m\n", t.path);
				exit(1);
			}
		}
		DIR *d = fdopendir(dfd);
		if (!d) {
			fprintf(stderr, "couldn't create directory stream: %m\n");
			exit(1);
		}

		struct task *p = NULL;
		unsigned n = 0;
		size_t plen;
		struct dirent *entry;
		while ((entry = readdir(d))) {
			if (entry->d_name[0] == '.' &&
					(entry->d_name[1] == '\0' ||
					 (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
				continue;

			/* fast path to avoid allocations */
			int trv;
			if (entry->d_type == DT_DIR) goto fast_path_dir;
			if ((trv = unlinkat(dfd, entry->d_name, 0)) && errno == EISDIR) {
fast_path_dir:
				trv = unlinkat(dfd, entry->d_name, AT_REMOVEDIR);
			}
			if (!trv) continue;

			n++;

			/* lazy allocation of p and other operations */
			if (!p) {
				p = malloc(sizeof *p);
				*p = t;
				/* access happens only after mutex lock and release */
				atomic_store_explicit(&p->removed_count, ACQUIRED, memory_order_relaxed);

				plen = strlen(t.path);
			}

			size_t nlen = strlen(entry->d_name);
			char *buf = malloc(plen + nlen + 2);
			memcpy(buf, p->path, plen);
			buf[plen] = '/';
			memcpy(buf+plen+1, entry->d_name, nlen);
			buf[plen+nlen+1] = '\0';

			printf("adding to queue'%s'\n", buf);
			queue_add(q, buf, p);
		}
		closedir(d);
		pthread_mutex_lock(&fd_mtx);
		pthread_cond_signal(&fd_cond);
		pthread_mutex_unlock(&fd_mtx);

		if (p) {
			p->files = n-1; /* other thread will compare against removed_count-1 */
			unsigned rc = atomic_fetch_and_explicit(&p->removed_count, ~ACQUIRED, memory_order_release);
			if (rc == (n|ACQUIRED)) {
				free(p);
				/* this branch is taken when other threads have already removed all of p's children */
				if (rmdir(t.path)) {
					fprintf(stderr, "atomic rmdir failed '%s': %m\n", t.path);
				} else {
					printf("atomic rmdir succeeded '%s'\n", t.path);
				}
			} else {
				/* we can't recurse into p's parent if p still has children that need to be removed */
				continue;
			}
		} else {
			/* p wasn't set because we could delete everything inside it */
			if (rmdir(t.path)) {
				fprintf(stderr, "fast path rmdir failed '%s': %m\n", t.path);
			} else {
				printf("fast path rmdir succeeded '%s'\n", t.path);
			}
		}

		if (t.parent) recurse_into_parents(&t);
		/* we took ownership of the buffer */
		free(t.path);
	}

	return NULL;
}

static void exit_init(void)
{
	fprintf(stderr, "thread initialization failed: %m\n");
	exit(1);
}

void run_queue(void)
{
	long nproc_l = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc_l < 1) nproc_l = 1;
	if (nproc_l > 64) nproc_l = 64;
	nproc = nproc_l;


	/* main thread will also be a task */
	unsigned nproc1 = nproc - 1;

	if (nproc1) {
		pthread_attr_t pattr;
		if (pthread_attr_init(&pattr)) exit_init();
#if defined(PTHREAD_STACK_MIN)
		if (pthread_attr_setstacksize(&pattr, PTHREAD_STACK_MIN) ||
				pthread_attr_setguardsize(&pattr, 1)) exit_init();
#endif

		for (unsigned i = 0; i < nproc1; i++) {
			pthread_t thread;
			if (pthread_create(&thread, &pattr, process_queue_item, &queue)) exit_init();
			pthread_detach(thread);
		}

		pthread_attr_destroy(&pattr);
	}

	/* become one of the worker threads */
	process_queue_item(&queue);
}

static void fail_single_file(const char *path)
{
	fprintf(stderr, "failed to remove '%s': %m\n", path);
}

int single_file(const char *path)
{
	int rv = remove(path);
	if (rv) fail_single_file(path);
	return rv;
}

void recurse_into(const char *path, int stop_at_error)
{
	if (!remove(path)) {
		return;
	} else if (errno == ENOTEMPTY) {
		queue_add(&queue, strdup(path), NULL);
		return;
	} else {
		fail_single_file(path);
		if (stop_at_error) exit(1);
	}
}
