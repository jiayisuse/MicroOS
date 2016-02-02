/* yalnix system calls implementation */

#include <sys.h>
#include <process.h>
#include <hardware.h>
#include <timer.h>
#include <page.h>
#include <hash.h>
#include <utility.h>
#include "internal.h"

unsigned long jiffies = 0;

int sys_fork(struct user_context *user_ctx)
{
	struct task_struct *child;
	int err = 0;

	_enter("current = %u\n", current->pid);

	child = alloc_and_init_task(current);
	if (child == NULL) {
		_error("Create child failed! NO MEM!\n");
		current->exit_code = ENOMEM;
		goto out;
	}

	_debug("created a child process = %u\n", child->pid);

	list_add(&current->children_head, &child->child_link);

	err = task_vm_copy(child, current);
	if (err) {
		_error("task vitural memory copy error!\n");
		current->exit_code = ENOMEM;
		goto out;
	}
	task_utilities_copy(child, current);

	current->exit_code = child->pid;

	child->state = TASK_READY;
	ready_queue_insert(child);
	set_current_state(TASK_READY);
	schedule(user_ctx);

out:
	_leave("current = %u, exit_code = %d",
			current->pid, current->exit_code);
	return current->exit_code;
}

/*
 * the same with sys_fork() except it makes text segement,
 * data segment and heap segment sharing with each other
 */
int sys_fork_share(struct user_context *user_ctx)
{
	struct task_struct *child;

	_enter();

	child = alloc_and_init_task(current);
	if (child == NULL) {
		_error("Create child failed! NO MEM!\n");
		return ENOMEM;
	}

	_debug("created a child process = %u\n", child->pid);

	list_add(&current->children_head, &child->child_link);

	task_vm_share_copy(child, current);
	task_utilities_copy(child, current);

	current->exit_code = child->pid;

	child->state = TASK_READY;
	ready_queue_insert(child);
	set_current_state(TASK_READY);
	schedule(user_ctx);

	_leave("current = %u, exit_code = %d",
			current->pid, current->exit_code);
	return current->exit_code;
}

void sys_exec(char *filename, char **argv, struct user_context *user_ctx)
{
	_enter("filename = %s, Current = %u", filename, current->pid);

	sys_load(filename, argv, current);
	*user_ctx = current->ucontext;

	_leave();
}

void sys_exit(int exit_code, struct user_context *user_ctx)
{
	struct zombie_task_struct *zombie;
	struct task_struct *parent = current->parent;

	_enter("pid = %d, exit_code = %d", current->pid, exit_code);

	if (current->pid == 1) {
		_debug("You are going to terminate INIT process, "
				"so it has to halt.\n");
		Halt();
	}

	current->exit_code = exit_code;

	/* rebuild child-parent lists and pointers */
	task_rescue_children(current);
	list_del(&current->child_link);
	list_del(&current->wait_list);
	hash_del(&current->hlist);

	/* allocate zombie to record child exit info */
	zombie = task_alloc_zombie(current);
	if (zombie == NULL) {
		_error("Zombie allocation failed!");
		goto do_schedule;
	}
	list_add_tail(&parent->zombie_head, &zombie->link);
	
	/* info parent who is waiting for children exit */
	if (parent->wait_child_flag)
		task_info_parent(parent, exit_code);

do_schedule:
	_leave("schedule pid = %u out", current->pid);
	set_current_state(TASK_ZOMBIE);
	schedule(user_ctx);
}

int sys_wait(int *status, struct user_context *user_ctx)
{
	struct list_head *list;
	struct zombie_task_struct *zombie;

	_enter();

	/* make sure it has children */
	if (list_empty(&current->children_head) &&
			list_empty(&current->zombie_head)) {
		_error("No children to be waited!\n");
		current->exit_code = ERROR;
		goto out;
	}

	/* suspend process until one child exits */
	while (list_empty(&current->zombie_head))
		task_wait_child(user_ctx);

	list = list_first(&current->zombie_head);
	list_del(list);
	zombie = list_entry(list, struct zombie_task_struct, link);
	*status = zombie->exit_code;
	current->exit_code = zombie->pid;
	free_zombie(zombie);

out:
	_leave("ret = %u", current->exit_code);
	return current->exit_code;
}

int inline sys_getpid(void)
{
	_enter();
	_leave("pid = %d", current->pid);
	return current->pid;
}

int sys_brk(unsigned long new_brk)
{
	unsigned int start_page_index, page_nr;
	int ret = 0;

	_enter("current_brk = %p(%u), new_brk = %p(%u)",
			current->brk, PAGE_UINDEX(current->brk),
			new_brk, PAGE_UINDEX(new_brk));

	new_brk = UP_TO_PAGE(new_brk);

	if (new_brk > current->brk) {
		if (PAGE_UINDEX(new_brk) >= current->stack_start) {
			_error("you(#%u) have touched the stack!\n",
					current->pid);
			ret = ERROR;
			goto out;
		}
		start_page_index = PAGE_UINDEX(current->brk);
		page_nr = PAGE_NR(new_brk - current->brk);
		ret = map_pages(current->page_table, start_page_index, page_nr,
				PROT_READ | PROT_WRITE);
		if (ret) {
			_error("expanding Brk for user process (%u) error!\n",
					current->pid);
			goto out;
		}
		current->brk = new_brk;
	} else if (current->brk - new_brk > 0) {
		start_page_index = PAGE_UINDEX(new_brk);
		page_nr = PAGE_NR(current->brk - new_brk);
		ret = unmap_pages(current->page_table,
				start_page_index, page_nr);
		if (ret) {
			_error("shrinking Brk for user process (%u) error!\n",
					current->pid);
			goto out;
		}
		current->brk = new_brk;
	}

out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_delay(unsigned int ticks, struct user_context *user_ctx)
{
	struct timer *timer;
	int ret = 0;

	_enter("ticks = %lu, pid = %u", ticks, current->pid);

	if (ticks < 0) {
		ret = ERROR;
		goto out;
	}

	if (ticks == 0)
		goto out;

	timer = alloc_init_timer(jiffies + ticks, current);
	if (timer == NULL) {
		_error("Memory out!!!\n");
		ret = ERROR;
		goto out;
	}

	add_timer(timer);
	set_current_state(TASK_PENDING);
	schedule(user_ctx);

out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_tty_read(int tty_id, void *buf, size_t len,
		struct user_context *user_ctx)
{
	_enter("tty_id = %d, buf = %p, len = %lu, pid = %u",
			tty_id, buf, len, current->pid);

	len = min(len, TERMINAL_MAX_LINE);

	/* make sure this is only one process reading from @tty_id */
	while (tty_reading_tasks[tty_id]) {
		set_current_state(TASK_PENDING);
		tty_read_enqueue(current, tty_id);
		schedule(user_ctx);
	}

	current->tty_buf = (void *)calloc(1, len);
	if (current->tty_buf == NULL) {
		_error("Allocate buffer for tty reading failed!\n");
		goto mem_err;
	}

	tty_reading_tasks[tty_id] = current;
	current->exit_code = len;
	set_current_state(TASK_PENDING);
	schedule(user_ctx);

	tty_reading_tasks[tty_id] = NULL;
	tty_read_wake_up_one(tty_id);
	memcpy(buf, current->tty_buf, current->exit_code);
	free(current->tty_buf);
	current->tty_buf = NULL;
mem_err:
	_leave("ret = %d, pid = %u", current->exit_code, current->pid);
	return current->exit_code;
}

int sys_tty_write(int tty_id, void *buf, size_t len,
		struct user_context *user_ctx)
{
	size_t commit_len;
	char *commit_buf;
	int ret = 0;

	_enter("tty_id = %d, buf = %p, len = %lu, pid = %u",
			tty_id, buf, len, current->pid);

	/* make sure there is only one process writing @tty_id */
	while (tty_writing_tasks[tty_id]) {
		set_current_state(TASK_PENDING);
		tty_trans_enqueue(current, tty_id);
		schedule(user_ctx);
	}

	commit_buf = (void *)calloc(1, TERMINAL_MAX_LINE);
	if (commit_buf == NULL) {
		_error("allocate buffer for tty transferring failed!\n");
		goto mem_err;
	}

	/* commit tty-writing multiple times if @buf exceeds
	 * @TERMINAL_MAX_LINE */
	len = min(len, strlen(buf));
	while (len) {
		commit_len = min(len, TERMINAL_MAX_LINE);
		memcpy(commit_buf, buf + ret, commit_len);

		set_current_state(TASK_PENDING);
		tty_writing_tasks[tty_id] = current;
		TtyTransmit(tty_id, commit_buf, commit_len);
		schedule(user_ctx);

		len -= commit_len;
		ret += commit_len;
	}

	tty_writing_tasks[tty_id] = NULL;
	tty_trans_wake_up_one(tty_id);
	free(commit_buf);
mem_err:
	_leave("ret = %d, pid = %u", ret, current->pid);
	return ret;
}

/**
 * union interface to initialize pipe, lock and cvar
 * @id: the pointer to record the new utility id
 * @type: specify the utility type for pipe, lock or cvar
 */
static int sys_utility_init(unsigned int *id, enum utility_type type)
{
	struct utility *utility;
	int ret = 0, new_id;

	new_id = task_new_utility_id(current);
	if (new_id < 0) {
		ret = ERROR;
		goto out;
	}

	utility = utility_alloc(new_id, type);
	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	current->utilities[new_id] = utility;
	*id = new_id;
out:
	return ret;
}

int sys_pipe_init(unsigned int *pipe_id)
{
	int ret;
	_enter("pid = %u", current->pid);

	ret = sys_utility_init(pipe_id, UTILITY_PIPE);

	_leave("pid = %u, ret = %d, pipe = %u", current->pid, ret, *pipe_id);
	return ret;
}

int sys_pipe_read(int pipe_id, void *buf, size_t len,
		struct user_context *user_ctx)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("pipe id = %u, buf = %p, len = %lu, pid = %u",
			pipe_id, buf, len, current->pid);

	utility = task_get_utility(current, pipe_id);
	if (utility == NULL) {
		_error("No pipe(#%u) associated with pid(#%u)\n",
				pipe_id, current->pid);
		goto out;
	}

	if (utility->type != UTILITY_PIPE) {
		_error("No pipe(#%u) exists!\n", pipe_id);
		goto out;
	}

	ret = pipe_do_read(utility, buf, len, user_ctx);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_pipe_write(int pipe_id, void *buf, size_t len,
		struct user_context *user_ctx)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("pipe id = %u, buf = %p, len = %lu, pid = %u",
			pipe_id, buf, len, current->pid);

	utility = task_get_utility(current, pipe_id);
	if (utility == NULL) {
		_error("No pipe(#%u) associated with pid(#%u)\n",
				pipe_id, current->pid);
		goto out;
	}

	if (utility->type != UTILITY_PIPE) {
		_error("No pipe(#%u) exists!\n", pipe_id);
		goto out;
	}

	ret = pipe_do_write(utility, buf, len, user_ctx);
out:
	_leave("ret = %d", ret);
	return ret;
}

int inline sys_lock_init(int *lock_id)
{
	int ret;
	_enter("pid = %u", current->pid);

	ret = sys_utility_init(lock_id, UTILITY_LOCK);

	_leave("pid = %u, ret = %d, lock = %u", current->pid, ret, *lock_id);
	return ret;
}

int sys_lock_acquire(int lock_id, struct user_context *user_ctx)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("lock id = %u, pid = %u", lock_id, current->pid);

	utility = task_get_utility(current, lock_id);
	if (utility == NULL) {
		_error("No lock(#%u) associated with pid(#%u)\n",
				lock_id, current->pid);
		goto out;
	}

	if (utility->type != UTILITY_LOCK) {
		_error("No lock(#%u) exists!\n", lock_id);
		goto out;
	}

	ret = lock_do_acquire(utility, user_ctx);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_lock_release(int lock_id)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("lock id = %u, pid = %u", lock_id, current->pid);

	utility = task_get_utility(current, lock_id);
	if (utility == NULL) {
		_error("No lock(#%u) associated with pid(#%u)\n",
				lock_id, current->pid);
		goto out;
	}

	if (utility->type != UTILITY_LOCK) {
		_error("No lock(#%u) exists!\n", lock_id);
		goto out;
	}

	ret = lock_do_release(utility);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_cvar_init(int *cvar_id)
{
	_enter("pid = %u", current->pid);
	int ret;

	ret = sys_utility_init(cvar_id, UTILITY_CVAR);

	_leave("pid = %u, ret = %d, cavl = %u", current->pid, ret, *cvar_id);
	return ret;
}

int sys_cvar_wait(int cvar_id, int lock_id, struct user_context *user_ctx)
{
	struct utility *cvar_u, *lock_u;
	int ret = ERROR;

	_enter("cvar_id = %u, lock id = %u, pid = %u",
			cvar_id, lock_id, current->pid);

	/* get and check cvar utility */
	cvar_u = task_get_utility(current, cvar_id);
	if (cvar_u == NULL) {
		_error("No Utility(#%u) associated with pid(#%u)\n",
				cvar_id, current->pid);
		goto out;
	}
	if (cvar_u->type != UTILITY_CVAR) {
		_error("No Cvar(#%u) exists!\n", cvar_id);
		goto out;
	}

	/* get and check lock utility */
	lock_u = task_get_utility(current, lock_id);
	if (lock_u == NULL) {
		_error("No lock(#%u) associated with pid(#%u)\n",
				lock_id, current->pid);
		goto out;
	}
	if (lock_u->type != UTILITY_LOCK) {
		_error("No lock(#%u) exists!\n", lock_id);
		goto out;
	}

	ret = cvar_do_wait(cvar_u, lock_u, user_ctx);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_cvar_signal(int cvar_id)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("cvar_id = %u, pid = %u", cvar_id, current->pid);

	utility = task_get_utility(current, cvar_id);
	if (utility == NULL) {
		_error("No Utility(#%u) associated with pid(#%u)\n",
				cvar_id, current->pid);
		goto out;
	}
	if (utility->type != UTILITY_CVAR) {
		_error("No Cvar(#%u) exists!\n", cvar_id);
		goto out;
	}

	ret = cvar_do_signal(utility);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_cvar_broadcast(int cvar_id)
{
	struct utility *utility;
	int ret = ERROR;

	_enter("cvar_id = %u, pid = %u", cvar_id, current->pid);

	utility = task_get_utility(current, cvar_id);
	if (utility == NULL) {
		_error("No Utility(#%u) associated with pid(#%u)\n",
				cvar_id, current->pid);
		goto out;
	}
	if (utility->type != UTILITY_CVAR) {
		_error("No Cvar(#%u) exists!\n", cvar_id);
		goto out;
	}

	ret = cvar_do_broadcast(utility);
out:
	_leave("ret = %d", ret);
	return ret;
}

int sys_reclaim(unsigned int id)
{
	struct utility *utility;
	int ret;

	_enter("pid = %u, id = %u", current->pid, id);

	utility = task_get_utility(current, id);
	if (utility == NULL) {
		_error("No pipe(#%u) associated with pid(#%u)\n",
				id, current->pid);
		goto out;
	}

	/* reduce utility reference number and check if it
	 * should be freed */
	ret = utility_put(utility);
	current->utilities[id] = NULL;
out:
	_leave("ret = %d", ret);
	return ret;
}
