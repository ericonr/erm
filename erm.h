typedef int (*file_action)(const char *path, int position);
struct task;

/* remove.c */
struct task *malloc_task_list(size_t);
int recurse_into(const char *, int);
int join_thread(const char *, int);
int single_file(const char *, int);
