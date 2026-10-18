/* Shared ring-3 runtime for user programs (flat binaries at 0x40000000).
 *
 * Everything is static so programs can pick what they need; unused
 * functions are dropped by the optimizer. A flat binary has no .bss
 * contents, so any static state must be explicitly initialized.
 */
#ifndef ROS_USER_RT_H
#define ROS_USER_RT_H

typedef unsigned char u8;
typedef unsigned int  u32;
typedef unsigned int  size_t;

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

static int sys_call(int n, int a, int b, int c)
{
    int ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

/* ---- console output ---- */
static void print(const char *s)
{
    sys_call(SYS_WRITE, (int)s, (int)strlen(s), 0);
}

static void __attribute__((unused)) print_dec(u32 v)
{
    char b[12];
    int n = 0;

    if (!v)
        b[n++] = '0';
    while (v) {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0) {
        char c = b[--n];

        sys_call(SYS_WRITE, (int)&c, 1, 0);
    }
}

static void print_hex(u32 v)
{
    char b[9];
    int n = 0;

    if (!v)
        b[n++] = '0';
    while (v) {
        u32 d = v & 0xF;

        b[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    while (n > 0) {
        char c = b[--n];

        sys_call(SYS_WRITE, (int)&c, 1, 0);
    }
}

/* ---- heap: brk + first-fit malloc with splitting and coalescing ---- */
static void *sbrk(int delta)
{
    u32 old = (u32)sys_call(SYS_BRK, 0, 0, 0);

    if (!delta)
        return (void *)old;
    if ((u32)sys_call(SYS_BRK, (int)(old + delta), 0, 0) != old + delta)
        return (void *)-1;
    return (void *)old;
}

struct mblk {
    u32 size;               /* payload size in bytes */
    u32 free;
    struct mblk *next;
};

#define MBLK_HDR   ((u32)sizeof(struct mblk))
#define HEAP_CHUNK 4096u    /* grow the heap in whole pages */

static struct mblk *heap_head = 0;

static void *malloc(u32 n)
{
    struct mblk *b, *prev;

    if (!n)
        return 0;
    n = (n + 3) & ~3u;

    prev = 0;
    for (b = heap_head; b; b = b->next) {
        if (b->free && b->next && b->next->free &&
            (u32)b + MBLK_HDR + b->size == (u32)b->next) {
            b->size += MBLK_HDR + b->next->size;    /* merge forward */
            b->next = b->next->next;
            continue;
        }
        if (b->free && b->size >= n)
            goto use;
        prev = b;
    }

    u32 grow = n + MBLK_HDR;

    if (grow < HEAP_CHUNK)
        grow = HEAP_CHUNK;
    b = (struct mblk *)sbrk((int)grow);
    if (b == (void *)-1)
        return 0;
    b->size = grow - MBLK_HDR;
    b->free = 1;
    b->next = 0;
    if (prev)
        prev->next = b;
    else
        heap_head = b;

use:
    if (b->size >= n + MBLK_HDR + 16) {     /* split off the tail */
        struct mblk *t = (struct mblk *)((u32)b + MBLK_HDR + n);

        t->size = b->size - n - MBLK_HDR;
        t->free = 1;
        t->next = b->next;
        b->next = t;
        b->size = n;
    }
    b->free = 0;
    return (void *)((u32)b + MBLK_HDR);
}

static void free(void *p)
{
    if (p)
        ((struct mblk *)((u32)p - MBLK_HDR))->free = 1;
}

#endif
