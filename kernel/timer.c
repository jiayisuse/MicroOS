#include <timer.h>
#include <sys.h>
#include <process.h>

static struct list_head timer_head = { &timer_head, &timer_head };


/**
 * allocate and initialize a timer
 * @timeout: absolute time point when triger this timer
 * @task: the task which is going to sleep
 */
struct timer *alloc_init_timer(unsigned long timeout, struct task_struct *task)
{
	struct timer *timer;
	
	timer = (void *)calloc(1, sizeof(struct timer));
	if (timer == NULL) {
		_error("Allocate timer failed!!!\n");
		return NULL;
	}
	INIT_LIST_HEAD(&timer->list);
	timer->timeout = timeout;
	timer->task = task;
	return timer;
}

/*
 * add a timer to a sorted timer list
 */
int add_timer(struct timer *timer)
{
	struct list_head *p;

	if (timer == NULL) {
		_error("add_timer received NULL!!!\n");
		return ERROR;
	}

	if (list_empty(&timer_head)) {
		list_add(&timer_head, TO_LIST(timer));
		return 0;
	}

	list_for_each(p, &timer_head) {
		struct timer *tp = (struct timer *)p;
		if (timer->timeout < tp->timeout) {
			list_add_tail(TO_LIST(tp), TO_LIST(timer));
			return 0;
		}
	}

	list_add_tail(&timer_head, TO_LIST(timer));

	return 0;
}

/**
 * wake up each of the timers that has been timeout
 * @timeout: the current absolute time point
 */
void wake_up_timer(unsigned long timeout)
{
	struct list_head *p, *t;

	list_for_each_safe(p, t, &timer_head) {
		struct timer *timer = (struct timer *)p;
		if (timer->timeout > timeout)
			break;
		list_del(TO_LIST(timer));
		task_wake_up(timer->task);
		free(timer);
	}

	return;
}
