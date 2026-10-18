/* int 0x80 system calls */
#ifndef ROS_SYSCALL_H
#define ROS_SYSCALL_H

#include "types.h"
#include "idt.h"

/* eax: 1 write(ebx=ptr, ecx=len), 2 exit, 3 getpid, 4 yield,
 *      5 getchar, 6 clear, 7 putat(ebx=row<<8|col, ecx=ch|attr<<8),
 *      8 setcursor(ebx=row<<8|col), 9 readfile(ebx=name, ecx=buf, edx=max),
 *      10 writefile(ebx=name, ecx=buf, edx=len), 11 args(ebx=buf, ecx=max),
 *      12 brk(ebx=new break, 0=query; returns the current break) */
u32 syscall_handler(regs_t *r);

/* command-line arguments handed to the next user program */
void syscall_set_args(const char *args);

#endif
