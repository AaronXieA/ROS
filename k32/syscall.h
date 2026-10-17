/* int 0x80 system calls */
#ifndef ROS_SYSCALL_H
#define ROS_SYSCALL_H

#include "types.h"
#include "idt.h"

/* eax: 1 = write(ebx=ptr, ecx=len), 2 = exit, 3 = getpid, 4 = yield */
u32 syscall_handler(regs_t *r);

#endif
