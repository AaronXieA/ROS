#include "timer.h"
#include "io.h"

static volatile u32 ticks;

void timer_init(void)
{
    u32 divisor = 1193182 / 100;    /* 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));
}

void timer_tick(void)
{
    ticks++;
}

u32 timer_ticks(void)
{
    return ticks;
}

void timer_sleep_ms(u32 ms)
{
    u32 start = ticks;
    u32 want = ms / 10;
    if (want == 0)
        want = 1;
    while (ticks - start < want)
        __asm__ volatile("sti; hlt");
}
