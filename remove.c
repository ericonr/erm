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
	/* file type as reported by readdir() */
	unsigned char type;
};

struct queue {
	pthread_mutex_t mtx;
	pthread_cond_t cond;
	size_t len, size;
	struct task *tasks;
	/* number of free threads */
	long free;
};
static struct queue queue = {0};

pthread_mutex_t fd_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t fd_cond = PTHREAD_COND_INITIALIZER;

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

static inline int queue_add(struct queue *q, char *path, unsigned char type, struct task *parent)
{
	int rv = 0;

	pthread_mutex_lock(&q->mtx);
	if (q->len + 1 > q->size) {
		q->size *= 2;
		if (q->size == 0) q->size = 32;
		void *t = realloc(q->tasks, q->size * sizeof(struct task));
		if (!t) {
			rv = -1;
			goto error;
		}
		q->tasks = t;
	}

	struct task t = {.path = path, .type = type, .parent = parent};
	q->tasks[q->len++] = t;

	pthread_cond_signal(&q->cond);

error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

static long nproc;

static inline int queue_remove(struct queue *q, struct task *t)
{
	int rv = 0;
	pthread_mutex_lock(&q->mtx);
	while (q->len == 0) {
		if (q->free == nproc - 1) {
			/* we are done removing things */
			exit(0);
		}
		q->free++;
		pthread_cond_wait(&q->cond, &q->mtx);
		q->free--;
	}
	if (q->len == 0) {
		rv = EAGAIN;
		goto error;
	}
	queue_print(q);
	*t = q->tasks[--(q->len)];

	/* the caller owns the path buffer now */
error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

static inline void recurse_into_parents(struct task *t)
{
	struct task *recurse = t;
	void *free_list = NULL;
	while ((recurse = recurse->parent)) {
		free(free_list); free_list = NULL;

		unsigned rc = atomic_fetch_add_explicit(&recurse->removed_count, 1, memory_order_acq_rel);
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

		if (t.type == DT_DIR) goto remove_dir;
		if (unlink(t.path)) {
			if (errno == EISDIR) {
remove_dir:
				if (rmdir(t.path)) {
					printf("rmdir failed '%s': %m\n", t.path);
					/* fall through to opening directory */
				} else {
					/* missing error checking here and in opendir below */
					printf("rmdir succeeded '%s'\n", t.path);
					goto end;
				}
			} else {
				/* actual error - add -f flag? */
				fprintf(stderr, "unlink failed '%s': %m\n", t.path);
				exit(1);
			}
		} else {
			printf("unlink succeeded '%s'\n", t.path);
			goto end;
		}

		DIR *d;
		while (!(d = opendir(t.path))) {
			if (errno == EMFILE) {
				pthread_mutex_lock(&fd_mtx);
				pthread_cond_wait(&fd_cond, &fd_mtx);
				pthread_mutex_unlock(&fd_mtx);
				continue;
			} else {
				break;
			}
		}
		int dfd = dirfd(d);

		struct task *p = NULL;
		unsigned n = 0;
		size_t plen = strlen(t.path);
		struct dirent *entry;
		while ((entry = readdir(d))) {
			if (strcmp(".", entry->d_name)==0 || strcmp("..", entry->d_name)==0) continue;

			/* fast path to avoid allocations */
			int trv;
			if (entry->d_type == DT_DIR) goto fast_path_dir;
			if ((trv = unlinkat(dfd, entry->d_name, 0)) && errno == EISDIR) {
fast_path_dir:
				trv = unlinkat(dfd, entry->d_name, AT_REMOVEDIR);
			}
			if (!trv) continue;

			n++;

			/* lazy allocation of p */
			if (!p) {
				p = malloc(sizeof *p);
				*p = t;
				/* access happens only after mutex lock and release */
				atomic_store_explicit(&p->removed_count, ACQUIRED, memory_order_relaxed);
			}

			size_t nlen = strlen(entry->d_name);
			char *buf = malloc(plen + nlen + 2);
			memcpy(buf, p->path, plen);
			buf[plen] = '/';
			memcpy(buf+plen+1, entry->d_name, nlen);
			buf[plen+nlen+1] = '\0';

			printf("adding to queue'%s'\n", buf);
			queue_add(q, buf, entry->d_type, p);
		}
		closedir(d);
		pthread_mutex_lock(&fd_mtx);
		pthread_cond_signal(&fd_cond);
		pthread_mutex_unlock(&fd_mtx);

		if (p) {
			p->files = n-1; /* other thread will compare against removed_count-1 */
			unsigned rc = atomic_fetch_and_explicit(&p->removed_count, ~ACQUIRED, memory_order_acq_rel);
			if (rc == (n|ACQUIRED)) {
				free(p);
				if (rmdir(t.path)) {
					fprintf(stderr, "atomic rmdir failed '%s': %m\n", t.path);
				} else {
					printf("atomic rmdir succeeded '%s'\n", t.path);
				}
			} else {
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

end:
		if (t.parent) recurse_into_parents(&t);
		/* we took ownership of the buffer */
		free(t.path);
	}

	return NULL;
}

int run_queue(void)
{
	nproc = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc < 1) nproc = 1;
	if (nproc > 64) nproc = 64;

	if (pthread_mutex_init(&queue.mtx, NULL)) return -1;
	if (pthread_cond_init(&queue.cond, NULL)) return -1;

	pthread_attr_t pattr;
	if (pthread_attr_init(&pattr)) return -1;
#if defined(PTHREAD_STACK_MIN)
	if (pthread_attr_setstacksize(&pattr, PTHREAD_STACK_MIN)) return -1;
	if (pthread_attr_setguardsize(&pattr, 1)) return -1;
#endif

	pthread_t *threads = calloc(sizeof(pthread_t), nproc);
	if (!threads) return -1;

	int i, j = 0;
	for (i = 0; i < nproc; i++) {
		if (pthread_create(threads+i, &pattr, process_queue_item, &queue)) {
			j = 1;
			break;
		}
	}
	pthread_attr_destroy(&pattr);
	/* if creating threads fails, cancell all the already created ones */
	if (j) for (j = 0; j < i; j++) {
		pthread_cancel(threads[j]);
	}
	for (j = 0; j < i; j++) {
		pthread_join(threads[j], NULL);
	}

	return 0;
}

int single_file(const char *path)
{
	return remove(path);
}

int recurse_into(const char *path)
{
	return queue_add(&queue, strdup(path), DT_UNKNOWN, NULL);
}
