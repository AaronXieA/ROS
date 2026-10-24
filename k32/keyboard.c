#include "keyboard.h"
#include "io.h"
#include "sched.h"

#define KBD_BUF 256

static volatile u8 buf[KBD_BUF];
static volatile u32 head, tail;
static int shift, caps, ctrl, e0;

static const char map_lo[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};

static const char map_hi[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

void keyboard_init(void)
{
    head = tail = 0;
    shift = caps = ctrl = e0 = 0;
    while (inb(0x64) & 1)
        (void)inb(0x60);
}

static void push(u8 c)
{
    u32 next = (head + 1) % KBD_BUF;
    if (next != tail) {
        buf[head] = c;
        head = next;
    }
}

void keyboard_irq(void)
{
    u8 sc = inb(0x60);

    /* 0xE0-prefixed keys: arrows, home/end, pgup/pgdn, delete.
     * They are queued as 0x80 | scancode so the shell can ignore them. */
    if (sc == 0xE0) {
        e0 = 1;
        return;
    }
    if (sc == 0xE1) {
        e0 = 0;
        return;
    }
    if (e0) {
        e0 = 0;
        if (!(sc & 0x80))
            push((u8)(0x80 | sc));
        return;
    }

    if (sc == 0x1D) {
        ctrl = 1;
        return;
    }
    if (sc == 0x9D) {
        ctrl = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) {
        shift = 1;
        return;
    }
    if (sc == 0xAA || sc == 0xB6) {
        shift = 0;
        return;
    }
    if (sc == 0x3A) {
        caps ^= 1;
        return;
    }
    if (sc & 0x80)
        return;

    /* Ctrl+letter -> standard control code (Ctrl+S = 0x13, Ctrl+Q = 0x11) */
    if (ctrl) {
        char lc = map_lo[sc];
        if (lc >= 'a' && lc <= 'z') {
            u8 cc = (u8)(lc - 'a' + 1);
            if (cc == 3 && sched_on_ctlc())
                return;             /* Ctrl+C kills the foreground process */
            push(cc);
            return;
        }
    }

    char c = shift ? map_hi[sc] : map_lo[sc];
    if (c >= 'a' && c <= 'z' && caps)
        c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z' && caps && !shift)
        c = (char)(c + 32);
    if (c)
        push((u8)c);
}

int kbd_getchar(void)
{
    if (head == tail)
        return -1;
    int c = buf[tail];
    tail = (tail + 1) % KBD_BUF;
    return c;
}

int kbd_waitchar(void)
{
    int c;
    while ((c = kbd_getchar()) < 0)
        __asm__ volatile("sti; hlt");
    return c;
}
