/* int 0x80 system calls */
#include "syscall.h"
#include "paging.h"
#include "sched.h"
#include "console.h"
#include "keyboard.h"
#include "fat.h"
#include "string.h"

#define SYS_WRITE      1
#define SYS_EXIT       2
#define SYS_GETPID     3
#define SYS_YIELD      4
#define SYS_GETCHAR    5
#define SYS_CLEAR      6
#define SYS_PUTAT      7
#define SYS_SETCURSOR  8
#define SYS_READFILE   9
#define SYS_WRITEFILE 10
#define SYS_ARGS      11
#define SYS_BRK       12

#define FILE_BUF_MAX (32 * 1024)

static u8 file_buf[FILE_BUF_MAX];
static char app_args[128];

void syscall_set_args(const char *args)
{
    int i = 0;

    if (!args)
        args = "";
    while (args[i] && i < (int)sizeof(app_args) - 1) {
        app_args[i] = args[i];
        i++;
    }
    app_args[i] = 0;
}

static int user_range_ok(u32 va, u32 len)
{
    u32 end = USER_STACK_TOP;

    if (va < USER_CODE_VA || va >= end)
        return 0;
    if (len > end - va)
        return 0;
    return 1;
}

static int copy_from_user(u8 *dst, u32 va, u32 n)
{
    if (!user_range_ok(va, n))
        return -1;
    for (u32 i = 0; i < n; i++) {
        u32 pa = paging_user_phys(va + i);
        if (!pa)
            return -1;
        dst[i] = *(volatile u8 *)pa;
    }
    return 0;
}

static int copy_to_user(u32 va, const u8 *src, u32 n)
{
    if (!user_range_ok(va, n))
        return -1;
    for (u32 i = 0; i < n; i++) {
        u32 pa = paging_user_phys(va + i);
        if (!pa)
            return -1;
        *(volatile u8 *)pa = src[i];
    }
    return 0;
}

static int copy_name(char *dst, u32 va)
{
    if (va < USER_CODE_VA || va >= USER_STACK_TOP)
        return -1;
    for (int i = 0; i < 15; i++) {
        u32 pa = paging_user_phys(va + i);
        if (!pa)
            return -1;
        dst[i] = *(volatile char *)pa;
        if (!dst[i])
            return 0;
    }
    dst[15] = 0;
    return 0;
}

static int name83_ok(const char *name)
{
    int base = 0, ext = 0;

    while (*name && *name != '.') {
        base++;
        name++;
    }
    if (*name == '.') {
        name++;
        while (*name) {
            ext++;
            name++;
        }
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

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

static int sys_read_file(u32 name_va, u32 buf_va, u32 max)
{
    char name[16];
    struct fat_dirent de;

    if (copy_name(name, name_va) < 0)
        return -1;
    if (fs_find(name, &de) < 0 || (de.attr & 0x10))
        return -1;
    if (de.size > max || de.size > sizeof(file_buf))
        return -2;

    int n = fs_read_file(&de, file_buf, sizeof(file_buf));
    if (n != (int)de.size)
        return -3;
    if (copy_to_user(buf_va, file_buf, de.size) < 0)
        return -3;
    return (int)de.size;
}

static int sys_write_file(u32 name_va, u32 buf_va, u32 len)
{
    char name[16];

    if (len > sizeof(file_buf))
        return -2;
    if (copy_name(name, name_va) < 0)
        return -1;
    if (!name83_ok(name))
        return -1;
    if (copy_from_user(file_buf, buf_va, len) < 0)
        return -3;

    fs_delete(name);
    return fs_create(name, file_buf, len) == 0 ? 0 : -4;
}

static int sys_args(u32 buf_va, u32 max)
{
    u32 n = strlen(app_args);

    if (n > max)
        n = max;
    if (copy_to_user(buf_va, (const u8 *)app_args, n) < 0)
        return -1;
    return (int)n;
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
    case SYS_GETCHAR:               /* blocking keyboard read */
        r->eax = (u32)kbd_waitchar();
        return (u32)r;
    case SYS_CLEAR:
        con_clear();
        r->eax = 0;
        return (u32)r;
    case SYS_PUTAT:                 /* ebx = row<<8 | col, ecx = ch | attr<<8 */
        con_putat((int)((r->ebx >> 8) & 0xFF), (int)(r->ebx & 0xFF),
                  (char)(r->ecx & 0xFF), (u8)((r->ecx >> 8) & 0xFF));
        r->eax = 0;
        return (u32)r;
    case SYS_SETCURSOR:             /* ebx = row<<8 | col */
        con_setpos((int)((r->ebx >> 8) & 0xFF), (int)(r->ebx & 0xFF));
        r->eax = 0;
        return (u32)r;
    case SYS_READFILE:
        r->eax = (u32)sys_read_file(r->ebx, r->ecx, r->edx);
        return (u32)r;
    case SYS_WRITEFILE:
        r->eax = (u32)sys_write_file(r->ebx, r->ecx, r->edx);
        return (u32)r;
    case SYS_ARGS:
        r->eax = (u32)sys_args(r->ebx, r->ecx);
        return (u32)r;
    case SYS_BRK:                   /* ebx = new break (0 = query) */
        r->eax = sched_brk(r->ebx);
        return (u32)r;
    default:
        r->eax = 0xFFFFFFFFu;
        return (u32)r;
    }
}
