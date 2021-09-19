typedef int (*file_action)(const char *path);
typedef int (*callback_action)(void);

/* remove.c */
int recurse_into(const char *);
int single_file(const char *);
int run_queue(void);
