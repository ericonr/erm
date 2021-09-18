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
	/* save on syscalls when possible
	 * TODO: actually use */
	unsigned char type;
};

struct queue {
	pthread_mutex_t mtx;
	size_t len, size;
	struct task *tasks;
	/* add a counter to be decremented by each thread that can't add more stuff until we know we can stop searching? */
};
static struct queue queue = {.mtx = PTHREAD_MUTEX_INITIALIZER};

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

error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

static long nproc;

int queue_remove(struct queue *q, struct task *t)
{
	int rv = 0;
	pthread_mutex_lock(&q->mtx);
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
		int rv = queue_remove(q, &t);
		if (rv == EAGAIN) {
			puts("yield");
			sched_yield();
			continue;
			/* no other errors yet, can be used to signal nothing is needed anymore? */
		}

		if (unlink(t.path)) {
			if (errno == EISDIR) {
				if (rmdir(t.path)) {
					printf("rmdir failed '%s': %m\n", t.path);
					/* fall through to opening directory */
				} else {
					/* missing error checking here and in opendir below */
					printf("rmdir succeeded '%s'\n", t.path);
					goto end;
				}
			} else {
				/* break ? */
				printf("unlink failed '%s': %m\n", t.path);
				goto end;
			}
		} else {
			printf("unlink succeeded '%s'\n", t.path);
			goto end;
		}

		struct dirent **entries;
		int n;
		while ((n = scandir(t.path, &entries, filter_dir, alphasort)) == -1) {
			if (errno == ENFILE) {
				/* sleep waiting for a closedir elsewhere */
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

	pthread_t *threads = calloc(sizeof(pthread_t), nproc);
	if (!threads) return -1;

	int i, j = 0;
	for (i = 0; i < nproc; i++) {
		if (pthread_create(threads+i, NULL, process_queue_item, &queue)) {
			j = 1;
			break;
		}
	}
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
