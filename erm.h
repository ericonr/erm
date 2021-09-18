typedef int (*file_action)(const char *path, int position);
struct task;

/* remove.c */
int recurse_into(const char *, int);
int join_thread(const char *, int);
int single_file(const char *, int);
int run_queue(void);
