#include <utility.h>
#include <sys.h>
#include <hash.h>

#define PIPE_LEN_DEFAULT	1024

#define IS_LOCKED(lock) ((lock)->counter == 0)
#define LOCK_LOCK(lock)			\
	do { (lock)->counter--; }	\
	while (0)
#define LOCK_UNLOCK(lock)		\
	do { (lock)->counter++; }	\
	while (0)

/*
 * wait queue operations
 */
static inline void
wait_enqueue(struct list_head *queue, struct task_struct *task)
{
	list_del(&task->wait_list);
	list_add_tail(queue, &task->wait_list);

	return;
}

static inline struct task_struct *wait_dequeue(struct list_head *queue)
{
	struct task_struct *task;
	struct list_head *list;

	if (list_empty(queue))
		task = NULL;
	else {
		list = list_first(queue);
		task = list_entry(list, struct task_struct, wait_list);
		list_del(list);
	}

	return task;
}

/*
 * pipe related operations
 * we implemented a ring buffer to manage pipe buffer
 */
static void *pipe_alloc(struct utility *utility)
{
	struct pipe *pipe;

	pipe = (void *)calloc(1, sizeof(struct pipe));
	if (pipe == NULL) {
		_error("Allocating pipe error!\n");
		goto pipe_err;
	}

	pipe->buf = (void *)calloc(1, PIPE_LEN_DEFAULT);
	if (pipe->buf == NULL) {
		_error("Allocating pipe error!\n");
		goto buf_err;
	}
	pipe->len = PIPE_LEN_DEFAULT;
	INIT_LIST_HEAD(&pipe->read_queue);
	INIT_LIST_HEAD(&pipe->write_queue);

	return pipe;
buf_err:
	free(pipe);
pipe_err:
	return NULL;
}

static inline void pipe_wake_up_readers(struct pipe *pipe)
{
	struct task_struct *task;
	task = wait_dequeue(&pipe->read_queue);
	while (task) {
		task_wake_up(task);
		task = wait_dequeue(&pipe->read_queue);
	}

	return;
}

static inline void pipe_wake_up_writers(struct pipe *pipe)
{
	struct task_struct *task;
	task = wait_dequeue(&pipe->write_queue);
	while (task) {
		task_wake_up(task);
		task = wait_dequeue(&pipe->write_queue);
	}

	return;
}

/*
 * it just copies what it has right away, even if the available bytes
 * is less than the requested @len
 */
int pipe_do_read(struct utility *utility, char *buf, size_t len,
		 struct user_context *user_ctx)
{
	int ret, n;
	struct pipe *pipe = utility->data;

	_enter("bytes = %u, read_p = %u, write_p = %u\n",
			pipe->bytes, pipe->read_p, pipe->write_p);

	if (utility == NULL || buf == NULL || len == 0) {
		ret = ERROR;
		goto out;
	}

	/* suspend until pipe->bytes is not zero */
	while (pipe->bytes == 0) {
		set_current_state(TASK_PENDING);
		wait_enqueue(&pipe->read_queue, current);
		schedule(user_ctx);
	}

	/* ring buffer manipulation */
	n = min(pipe->bytes, len);
	if (pipe->len - pipe->read_p >= n) {
		memcpy(buf, pipe->buf + pipe->read_p, n);
		pipe->read_p += n;
	} else {
		/* copy the ring buffer in two segments */
		size_t read_len_1 = pipe->len - pipe->read_p;
		size_t read_len_2 = n - read_len_1;
		memcpy(buf, pipe->buf + pipe->read_p, read_len_1);
		memcpy(buf + read_len_1, pipe->buf, read_len_2);
		pipe->read_p = read_len_2;
	}

	pipe->bytes -= n;
	pipe->read_p %= PIPE_LEN_DEFAULT;
	if (pipe->bytes < pipe->len)
		pipe_wake_up_writers(pipe);
	ret = n;

out:
	_leave("ret = %d", ret);
	return ret;
}

/*
 * it will not return until it writes all the requested @len bytes
 * into the pipe
 */
int pipe_do_write(struct utility *utility, char *buf, size_t len,
		struct user_context *user_ctx)
{
	int ret = 0, n = 0;
	struct pipe *pipe = utility->data;

	_enter();

	if (utility == NULL || buf == NULL || len == 0) {
		ret = ERROR;
		goto out;
	}

	/* no space to finish write operating */
	if (pipe->bytes == pipe->len)
		goto scedule_out;

continue_write:
	/* ring buffer manipulation */
	n = min(len, pipe->len - pipe->bytes);
	if (pipe->len - pipe->write_p >= n) {
		memcpy(pipe->buf + pipe->write_p, buf, n);
		pipe->write_p += n;
	} else {
		size_t write_len_1 = pipe->len - pipe->write_p;
		size_t write_len_2 = n - write_len_1;
		memcpy(pipe->buf + pipe->write_p, buf, write_len_1);
		write_len_2 = min(pipe->read_p, write_len_2);
		memcpy(pipe->buf, buf + write_len_1, write_len_2);
		pipe->write_p = pipe->write_p + n - pipe->len;
	}

	pipe->bytes += n;
	len -= n;
	ret += n;

	if (pipe->bytes)
		pipe_wake_up_readers(pipe);

	/* if it didn't write enough bytes to the buffer, wait for
	 * the next available buffer space to continue writing */
	if (len) {
scedule_out:
		set_current_state(TASK_PENDING);
		wait_enqueue(&pipe->write_queue, current);
		schedule(user_ctx);
		if (pipe->bytes == pipe->len)
			goto scedule_out;
		goto continue_write;
	}

	pipe->write_p %= PIPE_LEN_DEFAULT;
out:
	_leave("ret = %d", ret);
	return ret;
}

/*
 * Lock related operations
 */
static void *lock_alloc(struct utility *utility)
{
	struct lock *lock;

	lock = (void *)calloc(1, sizeof(struct lock));
	if (lock == NULL) {
		_error("Allocating lock error!\n");
		goto out;
	}
	lock->counter = 1;
	INIT_LIST_HEAD(&lock->wait_queue);

out:
	return lock;
}

static void lock_wakeup(struct lock *lock)
{
	struct task_struct *task;
	task = wait_dequeue(&lock->wait_queue);
	while (task) {
		task_wake_up(task);
		task = wait_dequeue(&lock->wait_queue);
	}

	return;
}

int lock_do_acquire(struct utility *utility, struct user_context *user_ctx)
{
	struct lock *lock;
	int ret = 0;

	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	lock = utility->data;
	if (lock == NULL) {
		ret = ERROR;
		goto out;
	}

	/* suspend until the lock is not locked */
	while (IS_LOCKED(lock)) {
		set_current_state(TASK_PENDING);
		wait_enqueue(&lock->wait_queue, current);
		schedule(user_ctx);
	}
	LOCK_LOCK(lock);
out:
	return ret;
}

int lock_do_release(struct utility *utility)
{
	struct lock *lock;
	int ret = 0;

	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	lock = utility->data;
	if (lock == NULL) {
		ret = ERROR;
		goto out;
	}

	/* if the lock is not locked, just return error */
	if (!IS_LOCKED(lock)) {
		ret = ERROR;
		_error("You should acquire this lock(#%u) first!\n",
				utility->id);
		goto out;
	}
	LOCK_UNLOCK(lock);
	lock_wakeup(lock);
out:
	return ret;
}

/**
 * Cvar related operations
 */
static void *cvar_alloc(struct utility *utility)
{
	struct cvar *cvar;

	cvar = (void *)calloc(1, sizeof(struct cvar));
	if (cvar == NULL) {
		_error("Allocating cvar error!\n");
		goto out;
	}
	INIT_LIST_HEAD(&cvar->wait_queue);
out:
	return cvar;
}

int cvar_do_wait(struct utility *cvar_utility, struct utility *lock_utility,
		struct user_context *user_ctx)
{
	struct cvar *cvar;
	int ret;

	if (cvar_utility == NULL || lock_utility == NULL) {
		ret = ERROR;
		goto out;
	}

	/* get and release @lock */
	utility_get(lock_utility);
	ret = lock_do_release(lock_utility);
	if (ret) {
		_error("Lock(#%u) release error!\n", lock_utility->id);
		goto out;
	}
	
	cvar = cvar_utility->data;
	if (cvar == NULL) {
		ret = ERROR;
		goto out;
	}
	set_current_state(TASK_PENDING);
	wait_enqueue(&cvar->wait_queue, current);
	schedule(user_ctx);

	/* put and lock @lock */
	utility_put(lock_utility);
	ret = lock_do_acquire(lock_utility, user_ctx);
	if (ret) {
		_error("Lock(#%u) acquire error!\n", lock_utility->id);
		goto out;
	}
out:
	return ret;
}

static inline int cvar_signal(struct cvar *cvar)
{
	struct task_struct *task;
	int ret = 0;

	task = wait_dequeue(&cvar->wait_queue);
	if (task)
		task_wake_up(task);

	return ret;
}

static inline int cvar_broadcast(struct cvar *cvar)
{
	struct task_struct *task;
	int ret = 0;

	task = wait_dequeue(&cvar->wait_queue);
	while (task) {
		task_wake_up(task);
		task = wait_dequeue(&cvar->wait_queue);
	}

	return ret;
}

int cvar_do_signal(struct utility *utility)
{
	struct cvar *cvar;
	int ret;

	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	cvar = utility->data;
	if (cvar == NULL) {
		ret = ERROR;
		goto out;
	}

	ret = cvar_signal(cvar);
out:
	return ret;
}

int cvar_do_broadcast(struct utility *utility)
{
	struct cvar *cvar;
	int ret;

	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	cvar = utility->data;
	if (cvar == NULL) {
		ret = ERROR;
		goto out;
	}

	ret = cvar_broadcast(cvar);
out:
	return ret;
}

/*
 * use function pointer to implement the common allocation
 * function interface
 */
typedef void *(*alloc_func_t)(struct utility *);
alloc_func_t alloc_funcs[] = {
	pipe_alloc,
	lock_alloc,
	cvar_alloc,
};

static int pipe_free(void *data)
{
	struct pipe *pipe = data;
	int ret = 0;

	_enter();

	if (pipe == NULL) {
		ret = ERROR;
		goto out;
	}

	free(pipe->buf);
	free(pipe);
out:
	_leave("ret = %d", ret);
	return ret;
}

static int lock_free(void *data)
{
	struct lock *lock = data;
	int ret = 0;

	_enter();

	if (lock == NULL) {
		ret = ERROR;
		goto out;
	}

	if (IS_LOCKED(lock)) {
		_error("You should release the lock first!\n");
		ret = ERROR;
		goto out;
	}

	free(lock);
out:
	_leave("ret = %d", ret);
	return ret;
}

static int cvar_free(void *data)
{
	struct cvar *cvar = data;
	int ret;

	_enter();

	if (cvar == NULL) {
		ret = ERROR;
		goto out;
	}
	free(cvar);
out:
	_leave("ret = %d", ret);
	return 0;
}

/*
 * use function pointer to implement the common free
 * function interface
 */
typedef int (*free_func_t)(void *);
free_func_t free_data_funcs[] = {
	pipe_free,
	lock_free,
	cvar_free,
};

struct utility *utility_alloc(unsigned int id, enum utility_type type)
{
	struct utility *utility;
	struct pipe *pipe;
	void *data;
	
	utility = (void *)calloc(1, sizeof(struct utility));
	if (utility == NULL) {
		_error("Allocating uitlity error!\n");
		goto uti_err;
	}

	utility->data = alloc_funcs[type](utility);
	if (utility->data == NULL) {
		_error("Allocating utility data error!\n");
		goto data_err;
	}

	utility->type = type;
	utility->counter = 1;

	return utility;

data_err:
	free(utility);
uti_err:
	return NULL;
}

/*
 * increase the utility's reference count
 */
void inline utility_get(struct utility *utility)
{
	if (utility)
		utility->counter++;
}

/*
 * decrease the utility's reference count. if the count reaches 0,
 * free the utility
 */
int inline utility_put(struct utility *utility)
{
	int ret = 0;

	if (utility == NULL) {
		ret = ERROR;
		goto out;
	}

	_enter("type = %d, count = %u, id = %u",
			utility->type, utility->counter, utility->id);

	if (--utility->counter)
		goto out;
	ret = free_data_funcs[utility->type](utility->data);
	if (ret) {
		utility->counter++;
		ret = ERROR;
		goto out;
	}

	free(utility);
out:
	_leave();
	return ret;
}
