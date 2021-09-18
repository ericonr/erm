#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "erm.h"

struct task {
	pthread_t thread;
	char *path;
	int rv;
	int created;
	/* include priority so it doesn't deadlock */
	int priority;
};

struct queue {
	pthread_mutex_t mtx;
	size_t len, size;
	struct task *tasks;
	/* add a counter to be decremented by each thread that can't add more stuff until we know we can stop searching? */
};
static struct queue queue = {.mtx = PTHREAD_MUTEX_INITIALIZER};

int queue_add(struct queue *q, const char *path, int priority)
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
	struct task t = {.path = p, .priority = priority};
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
		errno = EAGAIN;
		rv = -1;
		goto error;
	}
	puts("begin========================");
	for (size_t i=0; i < q->len; i++) {
		printf("item %010zu: %04d '%s'\n", i, q->tasks[i].priority, q->tasks[i].path);
	}
	puts("end==========================");
	size_t pos = q->len-1;
	/* the last position being a 0 is the best case */
	if (q->tasks[pos].priority == 0) {
		*t = q->tasks[pos];
		q->len=pos;
	} else {
		int min_priority = INT_MAX;
		for (size_t i=0; i < q->len; i++) {
			int priority = q->tasks[i].priority;
			/* if it's a zero we don't have to scan the rest */
			if (priority == 0) {
				pos = i;
				break;
			} else if (priority < min_priority) {
				pos = i;
				min_priority = priority;
			}
		}
		*t = q->tasks[pos];
		memmove(q->tasks+pos, q->tasks+pos+1, (q->len-pos-1)*sizeof(struct task));
		q->len--;
	}

	/* the caller owns the path buffer now */
error:
	pthread_mutex_unlock(&q->mtx);
	return rv;
}

int recurse_into(const char *path, int c)
{
	(void)c;
	return queue_add(&queue, path, 0);
}

static void *process_queue_item(void *arg)
{
	struct queue *q = arg;
	struct task t;
	while (1) {
		int rv = queue_remove(q, &t);
		if (rv) {
			if (errno == EAGAIN) {
				sched_yield();
				continue;
			}
			/* doesn't happen yet, can be used to signal nothing is needed anymore? */
			else break;
		}

		if (unlink(t.path)) {
			if (errno == EISDIR) {
				if (rmdir(t.path)) {
					printf("rmdir failed '%s': %m\n", t.path);
					if (t.priority > 0) {
						/* we have already scanned the directory */
						goto end;
					}
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

		DIR *d;
		while (!(d = opendir(t.path))) {
			if (errno == ENFILE) {
				/* sleep waiting for a closedir elsewhere */
				sched_yield();
				continue;
			} else {
				break;
			}
		}

		if (d) {
			struct dirent *entry;
			while ((entry = readdir(d))) {
				const char *name = entry->d_name;
				if (!strcmp(".", name) || !strcmp("..", name)) continue;
				char buf[PATH_MAX];
				/* deal with too large path? */
				snprintf(buf, PATH_MAX, "%s/%s", t.path, name);

				queue_add(q, buf, 0);
				printf("adding to queue'%s'\n", buf);
			}
		}

		/* try self again - but the other entries are more important */
		queue_add(q, t.path, t.priority+1);

end:
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
	//nproc = 1;

	/* warning with sizeof(*threads) ? */
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
