#ifndef TIMER_H
#define TIMER_H

#include <list.h>
#include <process.h>

struct timer {
	struct list_head list;
	unsigned long timeout;
	struct task_struct *task;
};

struct timer *alloc_init_timer(unsigned long timeout, struct task_struct *task);
int add_timer(struct timer *timer);
void wake_up_timer(unsigned long timeout);

#endif
