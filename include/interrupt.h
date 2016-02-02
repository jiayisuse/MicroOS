#ifndef INTERUPT_H
#define INTERUPT_H

#include <hardware.h>

void trap_kernel_handler(struct user_context *user_ctx);
void trap_clock_handler(struct user_context *user_ctx);
void trap_illegal_handler(struct user_context *user_ctx);
void trap_memory_handler(struct user_context *user_ctx);
void trap_math_handler(struct user_context *user_ctx);
void trap_tty_receive_handler(struct user_context *user_ctx);
void trap_tty_transmit_handler(struct user_context *user_ctx);
void trap_disk_handler(struct user_context *user_ctx);

typedef void (*interupt_func_t)(struct user_context *);
extern interupt_func_t *interupt_vector;

#endif
