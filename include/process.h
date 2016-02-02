#ifndef PROCESS_H
#define PROCESS_H

#include <stdbool.h>
#include <hardware.h>
#include <list.h>
#include <page.h>
#include <hash.h>

#define MAX_NUM_OPEN		128
#define PROCESS_HASH_BITS	6

#define set_current_state(state_value)			\
	do { current->state = (state_value); }		\
	while (0)
#define current_state() (current->state)
#define task_wake_up(task)				\
	do {						\
		(task)->state = TASK_READY;		\
		ready_enqueue(task);			\
	} while (0)					\

enum task_tatus {
	TASK_RUNNING,
	TASK_READY,
	TASK_PENDING,
	TASK_ZOMBIE,
	TASK_EXIT,
	TASK_NONE,
};

struct task_struct {
	long			state;
	long			counter;
	int			errno;
	int			exit_code;

	unsigned long		pid;
	struct task_struct	*parent;	/* points to its parent */
	struct list_head	children_head;	/* children list head */
	struct list_head	child_link;	/* child link */
	struct list_head	wait_list;	/* listed to the state queues */
	struct list_head	zombie_head;	/* zombie childrens */
#ifdef COW
	struct list_head	cow_list;	/* copy-on-write list */
#endif
	bool			wait_child_flag;
	struct hlist_node	hlist;		/* hashed to the global hash table*/

	struct user_context	ucontext;
	KernelContext		kcontext;

	unsigned int		stack_phy_pages[PAGE_NR(KERNEL_STACK_MAXSIZE)];
	unsigned int		code_start, code_pgn;	/* code page index and page number */
	unsigned int		data_start, data_pgn;	/* data page idnex and page number */
	unsigned long		brk;		/* break address */
	unsigned long		stack_start, stack_pgn;
	unsigned long		arg_start, arg_end;
	//struct vm_area_struct	*mmap;		/* kernel address space mapped to virtual address space in user space */
	struct my_pte		*page_table;
	char			*tty_buf;
	struct utility		*utilities[MAX_NUM_OPEN];
	bool			swapped;
};

struct zombie_task_struct {
	struct list_head	link;
	int long		exit_code;
	unsigned long		pid;
};

extern unsigned int _top_pid;
extern struct task_struct *current;
extern struct task_struct idle_task;
extern struct task_struct init_task;
extern struct task_struct *tty_writing_tasks[NUM_TERMINALS];
extern struct task_struct *tty_reading_tasks[NUM_TERMINALS];
extern DECLARE_HASHTABLE(process_hash_table, PROCESS_HASH_BITS);

extern struct task_struct *alloc_and_init_task(struct task_struct *parent);
extern int task_vm_copy(struct task_struct *to, struct task_struct *from);
extern int task_cow_copy_page(struct task_struct *task,
			unsigned int page_index);
extern void task_vm_expand_stack(struct task_struct *task, int increment);

extern void task_address_space_free(struct task_struct *task);
extern void free_task(struct task_struct *task);
extern struct zombie_task_struct *task_alloc_zombie(struct task_struct *task);
extern void free_zombie(struct zombie_task_struct *zombie);

extern void task_rescue_children(struct task_struct *task);
extern void task_wait_child(struct user_context *user_ctx);
extern void task_info_parent(struct task_struct *parent, int exit_code);

extern void initialize_processes_at_boot(void);
extern void ready_enqueue(struct task_struct *task);
extern void ready_queue_insert(struct task_struct *task);

extern void tty_read_enqueue(struct task_struct *task, unsigned int tty_id);
extern void tty_reading_wake_up(unsigned int tty_id);
extern void tty_read_wake_up_one(int tty_id);

extern void tty_trans_enqueue(struct task_struct *task, unsigned int tty_id);
extern void tty_trans_wake_up_one(unsigned tty_id);

int task_new_utility_id(struct task_struct *task);
struct utility *task_get_utility(struct task_struct *task, unsigned int id);
void task_utilities_copy(struct task_struct *to, struct task_struct *from);

extern void schedule(struct user_context *user_ctx);
extern void rr_schedule(struct user_context *user_ctx);

#endif
