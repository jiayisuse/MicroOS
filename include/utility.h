#ifndef PIPE_H
#define PIPE_H

#include <hardware.h>
#include <process.h>
#include <list.h>

enum utility_type {
	UTILITY_PIPE,
	UTILITY_LOCK,
	UTILITY_CVAR,
};

struct utility {
	unsigned int id;
	unsigned int counter;
	enum utility_type type;
	void *data;
};

struct pipe {
	char *buf;
	size_t len;
	size_t bytes;
	unsigned int read_p;
	unsigned int write_p;
	struct list_head read_queue;
	struct list_head write_queue;
};

struct lock {
	int counter;
	struct list_head wait_queue;
	struct utility *utility;
};

struct cvar {
	struct list_head wait_queue;
};

struct utility *utility_alloc(unsigned int id, enum utility_type type);
void utility_get(struct utility *utility);
int utility_put(struct utility *utility);

int pipe_do_read(struct utility *utility, char *buf, size_t len,
		struct user_context *user_ctx);
int pipe_do_write(struct utility *utility, char *buf, size_t len,
		struct user_context *user_ctx);

int lock_do_acquire(struct utility *utility, struct user_context *user_ctx);
int lock_do_release(struct utility *utility);

int cvar_do_wait(struct utility *cvar_utility, struct utility *lock_utility,
		struct user_context *user_ctx);
int cvar_do_signal(struct utility *utility);
int cvar_do_broadcast(struct utility *utility);

#endif
