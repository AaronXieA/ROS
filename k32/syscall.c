/* int 0x80 system calls */
#include "syscall.h"
#include "paging.h"
#include "sched.h"
#include "console.h"

#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_GETPID 3
#define SYS_YIELD  4

static void sys_write(u32 va, u32 len)
{
    if (len > 1024)
        len = 1024;

    for (u32 i = 0; i < len; i++) {
        u32 pa = paging_user_phys(va + i);
        if (!pa)
            return;
        char c = *(char *)pa;
        if (c == '\n')
            con_putc('\r');
        con_putc(c);
    }
}

u32 syscall_handler(regs_t *r)
{
    switch (r->eax) {
    case SYS_WRITE:
        sys_write(r->ebx, r->ecx);
        r->eax = 0;
        return (u32)r;
    case SYS_EXIT:
        return sched_exit_current(r);
    case SYS_GETPID:
        r->eax = (u32)sched_current_pid();
        return (u32)r;
    case SYS_YIELD:
        return sched_yield(r);
    default:
        r->eax = 0xFFFFFFFFu;
        return (u32)r;
    }
}
