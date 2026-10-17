#include "console.h"
#include "io.h"
#include "string.h"
#include <stdarg.h>

#define VGA_W 80
#define VGA_H 25
#define VGA_MEM ((volatile u16 *)0xB8000)

static int row, col;
static u8 attr = 0x07;

static void cursor_update(void)
{
    u16 pos = (u16)(row * VGA_W + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void scroll(void)
{
    if (row < VGA_H)
        return;
    memmove((void *)VGA_MEM, (void *)(VGA_MEM + VGA_W),
            (VGA_H - 1) * VGA_W * 2);
    for (int i = 0; i < VGA_W; i++)
        VGA_MEM[(VGA_H - 1) * VGA_W + i] = (u16)(' ' | (attr << 8));
    row = VGA_H - 1;
}

void con_init(void)
{
    row = col = 0;
    attr = 0x07;
    con_clear();
}

void con_clear(void)
{
    for (int i = 0; i < VGA_W * VGA_H; i++)
        VGA_MEM[i] = (u16)(' ' | (attr << 8));
    row = col = 0;
    cursor_update();
}

void con_set_color(u8 color)
{
    attr = color;
}

void con_putc(char c)
{
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\t') {
        col = (col + 4) & ~3;
        if (col >= VGA_W) {
            col = 0;
            row++;
        }
    } else if (c == '\b') {
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = VGA_W - 1;
        }
        VGA_MEM[row * VGA_W + col] = (u16)(' ' | (attr << 8));
    } else {
        VGA_MEM[row * VGA_W + col] = (u16)((u8)c | (attr << 8));
        col++;
        if (col >= VGA_W) {
            col = 0;
            row++;
        }
    }
    scroll();
    cursor_update();
}

void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void put_uint(u32 v, u32 base, int upper, int width, int zero)
{
    char tmp[36];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    char pad = zero ? '0' : ' ';
    for (int i = n; i < width; i++)
        con_putc(pad);
    while (n)
        con_putc(tmp[--n]);
}

void con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            con_putc(*fmt++);
            continue;
        }
        fmt++;
        int zero = 0, width = 0;
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            con_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            con_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                con_putc('-');
                put_uint((u32)(-v), 10, 0, width, zero);
            } else {
                put_uint((u32)v, 10, 0, width, zero);
            }
            break;
        }
        case 'u':
            put_uint(va_arg(ap, u32), 10, 0, width, zero);
            break;
        case 'x':
            put_uint(va_arg(ap, u32), 16, 0, width, zero);
            break;
        case 'X':
            put_uint(va_arg(ap, u32), 16, 1, width, zero);
            break;
        case '%':
            con_putc('%');
            break;
        default:
            con_putc('%');
            con_putc(*fmt);
            break;
        }
        if (*fmt)
            fmt++;
    }
    va_end(ap);
}

void con_fatal_banner(void)
{
    for (int i = 0; i < VGA_W; i++)
        VGA_MEM[i] = (u16)(' ' | (0x4F << 8));
    const char *s = "FATAL EXCEPTION";
    int len = (int)strlen(s);
    int start = (VGA_W - len) / 2;
    for (int i = 0; i < len; i++)
        VGA_MEM[start + i] = (u16)((u8)s[i] | (0x4F << 8));
    row = 2;
    col = 0;
    attr = 0x07;
    cursor_update();
}
