#include <yalnix.h>
#include <interrupt.h>
#include <sys.h>
#include <timer.h>

#define FROM_USER_SPACE(p) ((p) >= VMEM_1_BASE && (p) < VMEM_1_LIMIT)

#define SET_RET(user_ctx, ret)			\
	do { (user_ctx)->regs[0] = (ret); }	\
	while(0)

interupt_func_t *interupt_vector = NULL;

void trap_kernel_handler(struct user_context *user_ctx)
{
	_debug("...... in %s, code = %p\n",
			__func__, user_ctx->code & ~YALNIX_PREFIX);

	switch (user_ctx->code) {
	case YALNIX_FORK:
		SET_RET(user_ctx, sys_fork(user_ctx));
		break;
	case YALNIX_EXEC:
		sys_exec((char *)user_ctx->regs[0], (char **)user_ctx->regs[1],
				user_ctx);
		break;
	case YALNIX_EXIT:
		sys_exit(user_ctx->regs[0], user_ctx);
		break;
	case YALNIX_WAIT:
		if (!FROM_USER_SPACE(user_ctx->regs[0])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_wait((int *)user_ctx->regs[0], user_ctx));
		break;
	case YALNIX_GETPID:
		SET_RET(user_ctx, sys_getpid());
		break;
	case YALNIX_BRK:
		SET_RET(user_ctx, sys_brk(user_ctx->regs[0]));
		break;
	case YALNIX_DELAY:
		SET_RET(user_ctx, sys_delay(user_ctx->regs[0], user_ctx));
		break;
	case YALNIX_TTY_READ:
		if (!FROM_USER_SPACE(user_ctx->regs[1])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_tty_read(user_ctx->regs[0],
				(char *)user_ctx->regs[1],
				user_ctx->regs[2],
				user_ctx));
		break;
	case YALNIX_TTY_WRITE:
		if (!FROM_USER_SPACE(user_ctx->regs[1])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_tty_write(user_ctx->regs[0],
				(char *)user_ctx->regs[1],
				user_ctx->regs[2],
				user_ctx));
		break;
	case YALNIX_PIPE_INIT:
		if (!FROM_USER_SPACE(user_ctx->regs[0])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_pipe_init((unsigned int *)
					user_ctx->regs[0]));
		break;
	case YALNIX_PIPE_READ:
		if (!FROM_USER_SPACE(user_ctx->regs[1])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_pipe_read(user_ctx->regs[0],
					(char *)user_ctx->regs[1],
					user_ctx->regs[2],
					user_ctx));
		break;
	case YALNIX_PIPE_WRITE:
		if (!FROM_USER_SPACE(user_ctx->regs[1])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_pipe_write(user_ctx->regs[0],
					(char *)user_ctx->regs[1],
					user_ctx->regs[2],
					user_ctx));
		break;
	case YALNIX_LOCK_INIT:
		if (!FROM_USER_SPACE(user_ctx->regs[0])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_lock_init((unsigned int *)
					user_ctx->regs[0]));
		break;
	case YALNIX_LOCK_ACQUIRE:
		SET_RET(user_ctx, sys_lock_acquire(user_ctx->regs[0],
						user_ctx));
		break;
	case YALNIX_LOCK_RELEASE:
		SET_RET(user_ctx, sys_lock_release(user_ctx->regs[0]));
		break;
	case YALNIX_CVAR_INIT:
		if (!FROM_USER_SPACE(user_ctx->regs[0])) {
			_error("Bad man! Please pass user space pointer!\n");
			sys_exit(ERROR, user_ctx);
		}
		SET_RET(user_ctx, sys_cvar_init((unsigned int *)
					user_ctx->regs[0]));
		break;
	case YALNIX_CVAR_WAIT:
		SET_RET(user_ctx, sys_cvar_wait(user_ctx->regs[0],
					user_ctx->regs[1],
					user_ctx));
		break;
	case YALNIX_CVAR_SIGNAL:
		SET_RET(user_ctx, sys_cvar_signal(user_ctx->regs[0]));
		break;
	case YALNIX_CVAR_BROADCAST:
		SET_RET(user_ctx, sys_cvar_broadcast(user_ctx->regs[0]));
		break;
	case YALNIX_RECLAIM:
		SET_RET(user_ctx, sys_reclaim(user_ctx->regs[0]));
		break;
	case YALNIX_CUSTOM_0:
		SET_RET(user_ctx, sys_fork_share(user_ctx));
		break;
	}

	return;
}

void trap_clock_handler(struct user_context *user_ctx)
{
	jiffies++;

	/* wake up processes that called Delay() before */
	wake_up_timer(jiffies);

	/* call the Round Robin scheduler */
	rr_schedule(user_ctx);

	return;
}

void trap_illegal_handler(struct user_context *user_ctx)
{
	_enter();

	sys_exit(ERROR, user_ctx);

	_leave();
	return;
}

/*
 * page fault handler
 * - it deals with 4 scenarios:
 *   - expand user space stack
 *   - swap process in
 *   - do copy-on-write
 *   - handle segment fault
 */
void trap_memory_handler(struct user_context *user_ctx)
{
	unsigned long addr = (unsigned long)user_ctx->addr;
	unsigned int page_index = PAGE_UINDEX(addr);
	struct my_pte *ptep = current->page_table + page_index;
	int ret;

	_enter("pid = %u, at %p(%u), code = %d",
			current->pid, addr, page_index, user_ctx->code);

	switch (user_ctx->code) {
	case YALNIX_MAPERR:
		/* expand stack */
		if (!ptep->swap && page_index == current->stack_start - 1) {
			task_vm_expand_stack(current, 1);
			break;
		}

		/* swap process in */
		if (ptep->swap) {
			ret = swap_in(current);
			if (ret == EIO) {
				sys_tty_write(0, "Abort! Swap In error!\n",
						64, user_ctx);
				sys_exit(ret, user_ctx);
			}
		}

		break;
	case YALNIX_ACCERR:
		break;
	default:
#ifdef COW
		/* do copy-on-write for the parent and children */
		if (ptep->cow && ptep->prot == PROT_READ) {
			task_cow_copy_page(current, page_index);
			break;
		}
#endif
		/* if tried to write text segment just abort it */
		if (ptep->prot == (PROT_READ | PROT_EXEC)) {
			sys_tty_write(0, "Abort! Segment Fault!\n",
					64, user_ctx);
			sys_exit(ERROR, user_ctx);
		}

		break;
	}

	_leave();
	return;
}

void trap_math_handler(struct user_context *user_ctx)
{
	_enter();

	sys_exit(ERROR, user_ctx);

	_leave();
	return;
}

void trap_tty_receive_handler(struct user_context *user_ctx)
{
	unsigned int tty_id = user_ctx->code;
	_enter("current = %u, tty# = %d", current->pid, tty_id);

	tty_reading_wake_up(tty_id);

	_leave("current = %u", current->pid);
	return;
}

void trap_tty_transmit_handler(struct user_context *user_ctx)
{
	unsigned int tty_id = user_ctx->code;
	_enter("current = %u, tty# = %u", current->pid, tty_id);

	task_wake_up(tty_writing_tasks[tty_id]);

	_leave("current = %u", current->pid);
	return;
}

void trap_disk_handler(struct user_context *user_ctx)
{
	_enter();

	sys_exit(ERROR, user_ctx);

	_leave();
	return;
}
