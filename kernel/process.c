#include <yalnix.h>
#include <process.h>
#include <page.h>
#include <sys.h>
#include <utility.h>
#include "internal.h"

#define INIT_WAIT_QUEUE(q)				\
	do { INIT_LIST_HEAD(&(q)->head); (q)->n = 0; }	\
	while (0)

unsigned int _top_pid;
struct task_struct idle_task;
struct task_struct init_task;
struct task_struct *current;
struct task_struct *tty_writing_tasks[] = { NULL };
struct task_struct *tty_reading_tasks[] = { NULL };
DEFINE_HASHTABLE(process_hash_table, PROCESS_HASH_BITS);

struct task_wait_queue {
	struct list_head head;
	unsigned int n;
};

static struct task_wait_queue ready_queue;
static struct task_wait_queue tty_trans_queues[NUM_TERMINALS];
static struct task_wait_queue tty_read_queues[NUM_TERMINALS];
static unsigned long time_slice;
static unsigned long rr_timeout;


/**
 * allocate and init a child process
 * @parent: the parent who is forking a child
 */
struct task_struct *alloc_and_init_task(struct task_struct *parent)
{
	struct task_struct *task;

	task = (void *)calloc(1, sizeof(struct task_struct));
	if (task == NULL)
		return NULL;

	*task = *parent;
	task->pid = _top_pid++;
	task->parent = parent;
	INIT_LIST_HEAD(&task->children_head);
	INIT_LIST_ELM(&task->child_link);
	INIT_LIST_ELM(&task->wait_list);
	INIT_LIST_HEAD(&task->zombie_head);
#ifdef COW
	INIT_LIST_HEAD(&task->cow_list);
#endif
	task->exit_code = 0;
	task->tty_buf = NULL;
	bzero(task->stack_phy_pages, sizeof(task->stack_phy_pages));
	task->page_table = NULL;

	INIT_HLIST_NODE(&task->hlist);
	hash_add(process_hash_table, &task->hlist, task->pid);

	return task;
}

/**
 * copy a process' address space to another
 * @dest: the process to be copied to
 * @source: the process to be copied from
 */
int task_vm_copy(struct task_struct *dest, struct task_struct *source)
{
	unsigned int page_index_start, page_index_end, i;
	struct my_pte *page_table;
	int ret = 0;

	page_table = (void *)calloc(PAGE_NR(VMEM_1_SIZE),
					sizeof(struct my_pte));
	if (page_table == NULL) {
		_error("%s: page table memory out!\n", __func__);
		return ENOMEM;
	}

#ifdef COW
	for (i = 0; i < PAGE_NR(VMEM_1_SIZE); i++) {
		page_table[i] = source->page_table[i];
		/* change WR pages to read-only */
		if (page_table[i].prot == (PROT_READ | PROT_WRITE)) {
			update_pages_prot(page_table, i, 1, PROT_READ);
			update_pages_prot(source->page_table, i, 1, PROT_READ);
		}
		/* mark page table entries to cow */
		if (page_table[i].valid) {
			update_pages_cow(page_table, i, 1, 1);
			update_pages_cow(source->page_table, i, 1, 1);
		}
	}

	list_add(&source->cow_list, &dest->cow_list);
#else
	/* allocate and copy address space for user text */
	ret = map_pages_and_copy(page_table, source->page_table, source->brk,
			source->code_start, source->code_pgn);
	if (ret) {
		_error("%s: map pages for text segment \
				from pid(%u) to pid(u) error!\n",
				source->pid, dest->pid);
		return ret;
	}
	/* update prot for user text segment */
	ret = update_pages_prot(page_table, source->code_start,
			source->code_pgn, PROT_READ | PROT_EXEC);
	if (ret) {
		_error("%s: map pages for text segment \
				from pid(%u) to pid(u) error!\n",
				source->pid, dest->pid);
		return ret;
	}

	/* allocate and copy address space for user data segment */
	ret = map_pages_and_copy(page_table, source->page_table, source->brk,
			source->data_start,
			PAGE_UINDEX(source->brk) - source->data_start);
	if (ret) {
		_error("%s: map pages for data segment \
				from pid(%u) to pid(u) error!\n",
				source->pid, dest->pid);
		return ret;
	}

	/* allocate and copy address space for user stack */
	ret = map_pages_and_copy(page_table, source->page_table, source->brk,
			source->stack_start, source->stack_pgn);
	if (ret) {
		_error("%s: map pages for stack\
				from pid(%u) to pid(u) error!\n",
				source->pid, dest->pid);
		return ret;
	}
#endif

	dest->page_table = page_table;
	return ret;
}

/**
 * copy a process' address to another and make the text segment,
 * data segment and heap segment sharing with each other
 * @dest: the process to be copied to
 * @source: the process to be copied from
 */
int task_vm_share_copy(struct task_struct *dest, struct task_struct *source)
{
	unsigned int page_index_start, page_index_end, i;
	struct my_pte *page_table;
	int ret = 0;

	page_table = (void *)calloc(PAGE_NR(VMEM_1_SIZE), sizeof(struct my_pte));
	if (page_table == NULL) {
		_error("%s: page table memory out!\n", __func__);
		return ENOMEM;
	}

	for (i = 0; i < source->stack_start; i++) {
		page_table[i] = source->page_table[i];
		if (page_table[i].valid) {
			update_pages_cow(page_table, i, 1, 1);
			update_pages_cow(source->page_table, i, 1, 1);
		}
	}

	/* allocate and copy address space for user stack */
	ret = map_pages_and_copy(page_table, source->page_table, source->brk,
			source->stack_start, source->stack_pgn);
	if (ret) {
		_error("%s: map pages for stack\
				from pid(%u) to pid(u) error!\n",
				source->pid, dest->pid);
		return ret;
	}

	list_add(&source->cow_list, &dest->cow_list);
	dest->page_table = page_table;

	return ret;
}

/**
 * expand a task's stack
 * @task: the task to be expaned
 * @increment: number of pages to expand with
 */
void task_vm_expand_stack(struct task_struct *task, int increment)
{
	unsigned int new_stack_start;

	if (task == NULL || increment == 0)
		return;

	new_stack_start = task->stack_start - increment;
	if (new_stack_start <= PAGE_UINDEX(task->brk) ||
			new_stack_start >= PAGE_UINDEX(VMEM_1_LIMIT))
		return;

	if (increment > 0)
		map_pages(task->page_table, new_stack_start, increment,
				PROT_READ | PROT_WRITE);
	else
		unmap_pages(task->page_table, task->stack_start, -increment);

	task->stack_start -= increment;
	task->stack_pgn += increment;

	return;
}

#ifdef COW
/**
 * assign a separate page copy to each process who is sharing this page
 * @task: any one of the processes who are sharing this page
 * @page_index: the page which is being shared
 */
int task_cow_copy_page(struct task_struct *task, unsigned int page_index)
{
	struct list_head *list;
	struct task_struct *cow_task;
	list_for_each(list, &task->cow_list) {
		cow_task = list_entry(list, struct task_struct, cow_list);
		page_cow_copy(cow_task->page_table, task->page_table,
				task->brk, page_index);
	}
	update_pages_prot(task->page_table, page_index, 1,
			PROT_READ | PROT_WRITE);
	update_pages_cow(task->page_table, page_index, 1, 0);

	return 0;
}
#endif

/*
 * on a process exiting, transfer its alive children and zombie-children
 * to init_task(task 1)
 */
void inline task_rescue_children(struct task_struct *task)
{
	struct list_head *pos, *tmp;

	list_for_each_safe(pos, tmp, &task->children_head) {
		struct task_struct *child;
		child = list_entry(pos, struct task_struct, child_link);
		list_del(pos);
		list_add_tail(&init_task.children_head, pos);
		child->parent = &init_task;
	}

	list_for_each_safe(pos, tmp, &task->zombie_head) {
		list_del(pos);
		list_add_tail(&init_task.zombie_head, pos);
	}

	return;
}

/*
 * if a parent does not have any exited child, call this
 * function to sleep
 */
void inline task_wait_child(struct user_context *user_ctx)
{
	current->wait_child_flag = true;
	set_current_state(TASK_PENDING);
	schedule(user_ctx);

	return;
}

void inline task_info_parent(struct task_struct *parent, int exit_code)
{
	parent->wait_child_flag = false;
	task_wake_up(parent);

	return;
}

/*
 * unmap a process' address space
 */
void inline task_address_space_unmap(struct task_struct *task)
{
#ifdef COW
	/* If it is the only one in the cow list, clear all the cow flag
	 * in its page table, so that to really free the physical frames */
	if (list_empty(&task->cow_list)) {
		update_pages_cow(task->page_table, task->code_start,
				task->code_pgn, 0);
		update_pages_cow(task->page_table, task->data_start,
				PAGE_UINDEX(task->brk) - task->data_start, 0);
		update_pages_cow(task->page_table, task->stack_start,
				task->stack_pgn, 0);
	}
#endif
	unmap_pages(task->page_table, task->code_start, task->code_pgn);
	unmap_pages(task->page_table, task->data_start,
			PAGE_UINDEX(task->brk) - task->data_start);
	unmap_pages(task->page_table, task->stack_start, task->stack_pgn);

#ifdef COW
	list_del_init(&task->cow_list);
#endif

	return;
}

/*
 * free the task_struct of a process
 */
void free_task(struct task_struct *task)
{
	if (task) {
		int i;
		/* unmap user page table */
		task_address_space_unmap(task);

		/* unmap kernel stack */
		collect_back_pages(task->stack_phy_pages,
				PAGE_NR(KERNEL_STACK_MAXSIZE));
		/* put utilities owned by this process */
		for (i = 0; i < ARRAY_SIZE(task->utilities); i++)
			if (task->utilities[i])
				utility_put(task->utilities[i]);
		free(task->tty_buf);
		free(task->page_table);
		free(task);
	}

	return;
}

struct zombie_task_struct *task_alloc_zombie(struct task_struct *task)
{
	struct zombie_task_struct *zombie;

	zombie = (void *)calloc(1, sizeof(struct zombie_task_struct));
	if (zombie == NULL)
		goto out;

	INIT_LIST_ELM(&zombie->link);
	zombie->exit_code = task->exit_code;
	zombie->pid = task->pid;

out:
	return zombie;
}

inline void free_zombie(struct zombie_task_struct *zombie)
{
	if (zombie)
		free(zombie);

	return;
}

/*
 * the idle process whose pid is 0
 */
static void do_idle(void)
{
	while (1) {
		_debug("...... in %s() ....\n", __func__);
		Pause();
	}

	return;
}

static inline void init_idle_task()
{
	idle_task.pid = 0;
	idle_task.ucontext.vector = TRAP_KERNEL;
	idle_task.ucontext.code = YALNIX_NOP;
	idle_task.ucontext.pc = do_idle;
	idle_task.ucontext.sp = (void *)_kstack_base;
	idle_task.ucontext.ebp = (void *)_kstack_base;
	idle_task.state = TASK_READY;
	
	return;
}

static inline void init_init_task()
{
	int i;

	bzero(&init_task, sizeof(struct task_struct));
	INIT_LIST_HEAD(&init_task.children_head);
	INIT_LIST_ELM(&init_task.child_link);
	INIT_LIST_ELM(&init_task.wait_list);
	INIT_LIST_HEAD(&init_task.zombie_head);
#ifdef COW
	INIT_LIST_HEAD(&init_task.cow_list);
#endif
	init_task.wait_child_flag = false;
	init_task.pid = 1;

	for (i = 0; i < PAGE_NR(KERNEL_STACK_MAXSIZE); i++)
		init_task.stack_phy_pages[i] =
			i + PAGE_KINDEX(KERNEL_STACK_BASE);

	INIT_HLIST_NODE(&init_task.hlist);
	hash_add(process_hash_table, &init_task.hlist, init_task.pid);

	return;
}

/*
 * initialize idle_task, init_task, ready_queue, tty_trans_queues,
 * tty_read_queues, time_slice and rr_schedule timeout here
 */
void initialize_processes_at_boot(void)
{
	int i;

	hash_init(process_hash_table);
	init_idle_task();
	init_init_task();
	_top_pid = 2;

	current = NULL;

	INIT_WAIT_QUEUE(&ready_queue);
	for (i = 0; i < NUM_TERMINALS; i++) {
		INIT_WAIT_QUEUE(tty_trans_queues + i);
		INIT_WAIT_QUEUE(tty_read_queues + i);
	}

	time_slice = 1;
	rr_timeout = jiffies + time_slice;
}

/*
 * the common operations for task_wait_queue
 */
static inline void
task_enqueue(struct task_wait_queue *queue, struct task_struct *task)
{
	list_del(&task->wait_list);
	list_add_tail(TO_LIST(queue), &task->wait_list);
	queue->n++;

	return;
}

static inline void
task_queue_insert(struct task_wait_queue *queue, struct task_struct *task)
{
	list_del(&task->wait_list);
	list_add(TO_LIST(queue), &task->wait_list);
	queue->n++;

	return;
}

static inline struct task_struct *task_dequeue(struct task_wait_queue *queue)
{
	struct task_struct *task;
	struct list_head *list;

	if (list_empty(TO_LIST(queue)))
		task = NULL;
	else {
		list = list_first(TO_LIST(queue));
		task = list_entry(list, struct task_struct, wait_list);
		list_del(list);
		queue->n--;
	}

	return task;
}

/*
 * operations of ready_queue
 */
inline void ready_enqueue(struct task_struct *task)
{
	task_enqueue(&ready_queue, task);

	return;
}

static struct task_struct *ready_dequeue()
{
	struct task_struct *task;
	struct list_head *list;

	if (list_empty(TO_LIST(&ready_queue)))
		task = NULL;
	else {
repick:
		list = list_first(TO_LIST(&ready_queue));
		task = list_entry(list, struct task_struct, wait_list);
		list_del(list);
		ready_queue.n--;
		if (task == &idle_task && ready_queue.n) {
			ready_enqueue(task);
			goto repick;
		}
	}

	return task;
}

inline void ready_queue_insert(struct task_struct *task)
{
	task_queue_insert(&ready_queue, task);
	return;
}

/*
 * operations of TTY transition queues
 */
inline void tty_trans_enqueue(struct task_struct *task, unsigned int tty_id)
{
	return task_enqueue(tty_trans_queues + tty_id, task);
}

static inline struct task_struct *tty_trans_dequeue(unsigned int tty_id)
{
	return task_dequeue(tty_trans_queues + tty_id);
}

inline void tty_trans_wake_up_one(unsigned int tty_id)
{
	struct task_struct *task;

	task = tty_trans_dequeue(tty_id);
	if (task) {
		task->state = TASK_READY;
		ready_queue_insert(task);
	}

	return;
}

/*
 * operations for TTY read queues
 */
inline void tty_read_enqueue(struct task_struct *task, unsigned int tty_id)
{
	return task_enqueue(tty_read_queues + tty_id, task);
}

static inline void tty_read_queue_insert(struct task_struct *task,
					 unsigned int tty_id)
{
	return task_queue_insert(tty_read_queues + tty_id, task);
}

static inline struct task_struct *tty_read_dequeue(unsigned int tty_id)
{
	return task_dequeue(tty_read_queues + tty_id);
}

inline void tty_reading_wake_up(unsigned int tty_id)
{
	struct task_struct *task;
	int ret;

	task = tty_reading_tasks[tty_id];
	ret = TtyReceive(tty_id, task->tty_buf, task->exit_code);
	task->exit_code = ret;
	task_wake_up(task);

	return;
}

inline void tty_read_wake_up_one(int tty_id)
{
	struct task_struct *task;
	int ret;

	task = tty_read_dequeue(tty_id);
	if (task) {
		task->state = TASK_READY;
		ready_queue_insert(task);
	}

	return;
}

/*
 * utility related operations
 */
int inline task_new_utility_id(struct task_struct *task)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(task->utilities); i++)
		if (task->utilities[i] == NULL)
			return i;

	_error("Process(#%u) has reached its maximum pipe/lock/cval number!\n",
			task->pid);
	return ERROR;
}

inline struct utility *task_get_utility(struct task_struct *task,
					unsigned int id)
{
	if (id >= MAX_NUM_OPEN)
		return NULL;
	return task->utilities[id];
}

void task_utilities_copy(struct task_struct *to, struct task_struct *from)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(from->utilities); i++) {
		to->utilities[i] = from->utilities[i];
		if (to->utilities[i])
			utility_get(to->utilities[i]);
	}

	return;
}

/**
 * callback of KernelContextSwitch
 * It handle three things:
 * - Make a copy of parent kernel stack to child process
 * - Update kernel page table
 * - If old process is a zombie one, free it
 */
static KernelContext *kernel_context_switch(KernelContext *kernel_ctx,
						void *a, void *b)
{
	struct task_struct *curr_task, *next_task;

	curr_task = a;
	next_task = b;

	_debug("@@@@@@@@@@@@@@@@@@@@@@@@@@, kernel_ctx = %p\n", kernel_ctx);
	_debug("\t curr_task = %u, %p\n", curr_task->pid, &curr_task->kcontext);
	_debug("\t next_task = %u, %p\n", next_task->pid, &next_task->kcontext);
	_debug("\t ready_queue n = %d\n", ready_queue.n);

	if (curr_task->state != TASK_ZOMBIE)
		curr_task->kcontext = *kernel_ctx;

	next_task->state = TASK_RUNNING;
	current = next_task;

	/* fork child case */
	if (next_task->stack_phy_pages[0] == 0) {
		_debug("\tChild kernel context switching...\n");
		next_task->ucontext = curr_task->ucontext;
		next_task->kcontext = curr_task->kcontext;
		get_free_pages_and_copy(next_task->stack_phy_pages,
				page_table_0, _kbrk,
				PAGE_KINDEX(KERNEL_STACK_BASE),
				PAGE_NR(KERNEL_STACK_MAXSIZE));
	}

	update_pages_indexes(page_table_0,
			PAGE_KINDEX(KERNEL_STACK_BASE),
			PAGE_NR(KERNEL_STACK_MAXSIZE),
			next_task->stack_phy_pages);

	if (curr_task->state == TASK_ZOMBIE)
		free_task(curr_task);

	return &next_task->kcontext;
}

/**
 * switch running context
 * @task: the task to be scheduled to run
 * @user_ctx: user context to be updated
 */
static inline void
__context_switch(struct task_struct *task, struct user_context *user_ctx)
{
	if (current->state != TASK_ZOMBIE)
		current->ucontext = *user_ctx;

	KernelContextSwitch(kernel_context_switch,
			(void *)current, (void *)task);

	set_current_state(TASK_RUNNING);
	*user_ctx = current->ucontext;
	if (current != &idle_task)
		UPDATE_VM1_AND_FLUSH_TLB(current->page_table);

	return;
}

/*
 * process calls this function to relinquish cpu
 */
void schedule(struct user_context *user_ctx)
{
	struct task_struct *task;

	/* shrink stack here */
	if (PAGE_UINDEX(user_ctx->sp) > current->stack_start)
		task_vm_expand_stack(current, current->stack_start -
				PAGE_UINDEX(user_ctx->sp));

	task = ready_dequeue();
	if (task == NULL) {
		_debug("^^^ RRRRRR Empty queue current = %d\n", current->pid);
		set_current_state(TASK_RUNNING);
		return;
	}

	if (current_state() == TASK_READY)
		ready_enqueue(current);

	__context_switch(task, user_ctx);

	return;
}

/*
 * Round Robin schduler
 */
void inline rr_schedule(struct user_context *user_ctx)
{
	if (jiffies >= rr_timeout) {
		rr_timeout = jiffies + time_slice;

		_enter("current = %u is going to be scheduled\n", current->pid);

		set_current_state(TASK_READY);
		schedule(user_ctx);

		_leave("current = %u\n", current->pid);
	}

	return;
}
