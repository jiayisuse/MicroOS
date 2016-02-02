#ifndef SYSCALL_H
#define SYSCALL_H

#include <yalnix.h>
#include <hardware.h>
#include <process.h>
#include <list.h>

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define _debug(FMT, ...) TracePrintf(3, "Debug:  "FMT, ##__VA_ARGS__)
#define _error(FMT, ...)						\
	do {								\
		TracePrintf(1, "******************************\n");	\
		TracePrintf(1, "ERROR at line %d of %s() in \"%s\":  "	\
				FMT,__LINE__, __func__, __FILE__,	\
				##__VA_ARGS__);	\
		TracePrintf(1, "******************************\n");	\
	} while (0)

#define _enter(FMT, ...) _debug(">>>>>  %s(): "FMT"\n", __func__, ##__VA_ARGS__)
#define _leave(FMT, ...) _debug("<<<<<  %s(): "FMT"\n", __func__, ##__VA_ARGS__)

#define UPDATE_VM1_AND_FLUSH_TLB(page_table)				\
	do {								\
		WriteRegister(REG_PTBR1, (unsigned int)(page_table));	\
		WriteRegister(REG_PTLR1, PAGE_NR(VMEM_1_SIZE));		\
		WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1); 		\
	} while (0)


extern unsigned long jiffies;


int sys_fork(struct user_context *user_ctx);
int sys_fork_share(struct user_context *user_ctx);
void sys_exec(char *filename, char **argv, struct user_context *user_ctx);
void sys_exit(int exit_code, struct user_context *user_ctx);
int sys_wait(int *status, struct user_context *user_ctx);
int sys_getpid(void);

int sys_brk(unsigned long new_brk);
int sys_delay(unsigned int ticks, struct user_context *user_context);

int sys_tty_read(int tty_id, void *buf, size_t len,
		struct user_context *user_cxt);
int sys_tty_write(int tty_id, void *buf, size_t len,
		struct user_context *user_cxt);

int sys_pipe_init(unsigned int *pipe_id);
int sys_pipe_read(int pipe_id, void *buf, size_t len,
		struct user_context *user_ctx);
int sys_pipe_write(int pipe_id, void *buf, size_t len,
		struct user_context *user_ctx);

int sys_lock_init(int *lock_id);
int sys_lock_acquire(int lock_id, struct user_context *user_ctx);
int sys_lock_release(int lock_id);

int sys_cvar_init(int *cvar_id);
int sys_cvar_signal(int cvar_id);
int sys_cvar_broadcast(int cvar_id);
int sys_cvar_wait(int cvar_id, int lock_id, struct user_context *user_ctx);

int sys_reclaim(unsigned int id);

int sys_load(char *filename, char **args, struct task_struct *task);

#endif
