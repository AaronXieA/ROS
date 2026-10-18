/* memtest - exercises the user heap: SYS_BRK, malloc, free, coalescing.
 * Prints "HEAP OK" and exits when every check passes. */
#include "rt.h"

static void fail(const char *what, u32 at)
{
    print("HEAP FAIL: ");
    print(what);
    print(" at ");
    print_hex(at);
    print("\n");
    sys_call(SYS_EXIT, 1, 0, 0);
    for (;;)
        ;
}

static void fill(u8 *p, u32 n, u8 seed)
{
    for (u32 i = 0; i < n; i++)
        p[i] = (u8)(seed + i * 31);
}

static void verify(const char *what, const u8 *p, u32 n, u8 seed)
{
    for (u32 i = 0; i < n; i++) {
        if (p[i] != (u8)(seed + i * 31))
            fail(what, (u32)p + i);
    }
}

void _start(void)
{
    u8 *a, *b, *c, *d, *huge;

    print("memtest: heap base = 0x");
    print_hex((u32)sbrk(0));
    print("\n");

    a = (u8 *)malloc(400000);
    b = (u8 *)malloc(1200000);
    c = (u8 *)malloc(600000);
    if (!a || !b || !c)
        fail("malloc", 0);
    print("memtest: a=0x");
    print_hex((u32)a);
    print(" b=0x");
    print_hex((u32)b);
    print(" c=0x");
    print_hex((u32)c);
    print("\n");

    fill(a, 400000, 0x11);
    fill(b, 1200000, 0x42);
    fill(c, 600000, 0x77);
    verify("a", a, 400000, 0x11);
    verify("b", b, 1200000, 0x42);
    verify("c", c, 600000, 0x77);
    print("memtest: fill+verify passed (2.2 MB)\n");

    free(a);
    d = (u8 *)malloc(300000);
    if (!d)
        fail("reuse malloc", 0);
    print("memtest: reuse block at 0x");
    print_hex((u32)d);
    print(d == a ? " (reused)\n" : " (fresh)\n");
    fill(d, 300000, 0x99);
    verify("d", d, 300000, 0x99);

    /* neighbours must be untouched by the reuse */
    verify("b after reuse", b, 1200000, 0x42);
    verify("c after reuse", c, 600000, 0x77);

    huge = (u8 *)malloc(64u * 1024 * 1024);
    if (huge)
        fail("oversized malloc succeeded", (u32)huge);
    print("memtest: oversized malloc rejected\n");

    free(b);
    free(c);
    free(d);

    print("memtest: break after free = 0x");
    print_hex((u32)sbrk(0));
    print("\nHEAP OK\n");
    sys_call(SYS_EXIT, 0, 0, 0);
}
