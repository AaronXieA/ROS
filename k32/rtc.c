#include "rtc.h"
#include "io.h"

static u8 cmos_read(u8 reg)
{
    outb(0x70, (u8)(reg | 0x80));   /* keep NMI disabled while reading */
    return inb(0x71);
}

static void cmos_write(u8 reg, u8 val)
{
    outb(0x70, (u8)(reg | 0x80));
    outb(0x71, val);
}

static void rtc_wait_ready(void)
{
    for (u32 i = 0; i < 1000000; i++) {
        if (!(cmos_read(0x0A) & 0x80))
            return;
    }
}

u8 bcd_to_bin(u8 v)
{
    return (u8)((v >> 4) * 10 + (v & 0x0F));
}

u8 bin_to_bcd(u8 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

void rtc_read(u8 *h, u8 *m, u8 *s, u8 *day, u8 *mon, u8 *year, u8 *cent)
{
    u8 s1, s2, m1, m2, h1, h2, d1, d2, mo1, mo2, y1, y2, c1, c2;

    rtc_wait_ready();
    do {
        s1 = cmos_read(0x00);
        m1 = cmos_read(0x02);
        h1 = cmos_read(0x04);
        d1 = cmos_read(0x07);
        mo1 = cmos_read(0x08);
        y1 = cmos_read(0x09);
        c1 = cmos_read(0x32);
        s2 = cmos_read(0x00);
        m2 = cmos_read(0x02);
        h2 = cmos_read(0x04);
        d2 = cmos_read(0x07);
        mo2 = cmos_read(0x08);
        y2 = cmos_read(0x09);
        c2 = cmos_read(0x32);
    } while (s1 != s2 || m1 != m2 || h1 != h2 || d1 != d2 || mo1 != mo2 ||
             y1 != y2 || c1 != c2);

    u8 b = cmos_read(0x0B);
    int bcd = !(b & 0x04);

    *s = bcd ? bcd_to_bin(s2) : s2;
    *m = bcd ? bcd_to_bin(m2) : m2;
    *h = bcd ? bcd_to_bin(h2) : h2;
    *day = bcd ? bcd_to_bin(d2) : d2;
    *mon = bcd ? bcd_to_bin(mo2) : mo2;
    *year = bcd ? bcd_to_bin(y2) : y2;
    *cent = bcd ? bcd_to_bin(c2) : c2;
}

static void rtc_begin_set(void)
{
    u8 b = cmos_read(0x0B);
    cmos_write(0x0B, (u8)(b | 0x80));
}

static void rtc_end_set(void)
{
    u8 b = cmos_read(0x0B);
    cmos_write(0x0B, (u8)(b & ~0x80));
}

int rtc_set_time(u8 h, u8 m, u8 s)
{
    if (h > 23 || m > 59 || s > 59)
        return -1;
    rtc_begin_set();
    cmos_write(0x00, bin_to_bcd(s));
    cmos_write(0x02, bin_to_bcd(m));
    cmos_write(0x04, bin_to_bcd(h));
    rtc_end_set();
    return 0;
}

int rtc_set_date(u8 year, u8 mon, u8 day, u8 cent)
{
    if (year > 99 || mon < 1 || mon > 12 || day < 1 || day > 31)
        return -1;
    rtc_begin_set();
    cmos_write(0x07, bin_to_bcd(day));
    cmos_write(0x08, bin_to_bcd(mon));
    cmos_write(0x09, bin_to_bcd(year));
    if (cent == 20)
        cmos_write(0x32, bin_to_bcd(cent));
    rtc_end_set();
    return 0;
}
