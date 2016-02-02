#ifndef SWAP_H
#define SWAP_H

#include <process.h>

int swap_out(void);
int swap_in(struct task_struct *task);

#endif
