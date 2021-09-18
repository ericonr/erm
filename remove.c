#include <dirent.h>
#include <errno.h>
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

struct task {
	char *path;
	struct task *parent;
	/* reference counting */
	atomic_int rc;
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

int queue_add(struct queue *q, const char *path, unsigned char type, struct task *parent)
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

	char *p = strdup(path);
	if (!p) {
		rv = -1;
		goto error;
	}
	struct task t = {.path = p, .type = type, .parent = parent};
	q->tasks[q->len++] = t;

	pthread_cond_signal(&q->cond);

error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

static long nproc;

int queue_remove(struct queue *q, struct task *t)
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
	puts("begin========================");
	for (size_t i=0; i < q->len; i++) {
		printf("item %010zu: '%s'\n", i, q->tasks[i].path);
	}
	puts("end==========================");
	*t = q->tasks[--(q->len)];

	/* the caller owns the path buffer now */
error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

int recurse_into(const char *path, int c)
{
	(void)c;
	return queue_add(&queue, path, DT_UNKNOWN, NULL);
}

static int filter_dir(const struct dirent *d)
{
	return strcmp(".", d->d_name) && strcmp("..", d->d_name);
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

		struct dirent **entries;
		int n;
		while ((n = scandir(t.path, &entries, filter_dir, alphasort)) == -1) {
			if (errno == ENFILE) {
				/* sleep waiting for a closedir elsewhere
				 * TODO: try to broadcast when file operations are done so we don't waste CPU
				 * XXX: bad logic - tool now exits in an arbitrary point when the fd limit is lowered */
				sched_yield();
				continue;
			} else {
				goto end;
			}
		}
		struct task *p = malloc(sizeof *p);
		*p = t;
		t.path = NULL;
		p->rc = n;

		/* TODO: use recursive mutex, lock queue twice, use readdir only */
		while (n--) {
			struct dirent *entry = entries[n];
			const char *name = entry->d_name;
			char buf[PATH_MAX];
			/* deal with too large path? */
			snprintf(buf, PATH_MAX, "%s/%s", p->path, name);

			queue_add(q, buf, entry->d_type, p);
			printf("adding to queue'%s'\n", buf);
			free(entry);
		}
		free(entries);
		continue;

end:
		if (t.parent) {
			struct task *recurse = &t;
			void *free_list = NULL;
			while ((recurse = recurse->parent)) {
				free(free_list); free_list = NULL;

				int rc = atomic_fetch_sub(&recurse->rc, 1);
				printf("parent: %04d '%s'\n", rc, recurse->path);
				if (rc < 1) {
					printf("bad parent: %04d '%s'\n", rc, recurse->path);
					abort();
				}
				if (rc == 1) {
					/* reference counting fell down to 0 */
					if (rmdir(recurse->path)) {
						printf("rec rmdir failed '%s': %m\n", recurse->path);
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

	pthread_mutexattr_t mattr;
	if (pthread_mutexattr_init(&mattr)) return -1;
	if (pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE)) return -1;
	if (pthread_mutex_init(&queue.mtx, &mattr)) return -1;
	pthread_mutexattr_destroy(&mattr);

	if (pthread_cond_init(&queue.cond, NULL)) return -1;

	pthread_attr_t pattr;
	if (pthread_attr_init(&pattr)) return -1;
	if (pthread_attr_setstacksize(&pattr, (1 << 13) - 1024)) return -1;
	if (pthread_attr_setguardsize(&pattr, 1)) return -1;

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

int single_file(const char *path, int c)
{
	(void)c;
	return remove(path);
}
