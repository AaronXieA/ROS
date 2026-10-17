/* Preemptive round-robin task scheduler with ring-3 user processes */
#ifndef ROS_SCHED_H
#define ROS_SCHED_H

#include "types.h"
#include "idt.h"

#define TASK_MAX 8

void sched_init(void);
int  sched_spawn(const char *name, const u8 *code, u32 size);
int  sched_kill(int pid);
void sched_ps(void);
int  sched_current_pid(void);
const char *sched_current_name(void);

/* Return the stack pointer to resume from (used by the interrupt stubs) */
u32  sched_on_tick(regs_t *r);
u32  sched_yield(regs_t *r);
u32  sched_exit_current(regs_t *r);
u32  sched_kill_current(regs_t *r);

#endif
