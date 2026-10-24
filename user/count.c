/* count - countdown demo for the sleep/time syscalls.
 * Prints the current RTC time, then counts down from 10 once per second.
 * Ctrl+C kills it mid-count; otherwise it prints "Liftoff!" and exits. */
#include "rt.h"

static void print_two(u32 v)
{
    if (v < 10)
        print("0");
    print_dec(v);
}

void _start(void)
{
    struct ros_time t;

    if (get_time(&t) == 0) {
        print("Now: ");
        print_dec(t.year);
        print("-");
        print_two(t.mon);
        print("-");
        print_two(t.day);
        print(" ");
        print_two(t.hour);
        print(":");
        print_two(t.min);
        print(":");
        print_two(t.sec);
        print("\n");
    }

    for (int n = 10; n > 0; n--) {
        print("T-minus ");
        print_dec((u32)n);
        print("\n");
        sleep_ms(1000);
    }
    print("Liftoff!\n");
    sys_call(SYS_EXIT, 0, 0, 0);
}
