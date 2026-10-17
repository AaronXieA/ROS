#include "ata.h"
#include "io.h"

#define ATA_IO      0x1F0
#define ATA_CTRL    0x3F6

static int present;

static void delay400(void)
{
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

/* wait until BSY clears (command finished) */
static void wait_idle(void)
{
    for (u32 i = 0; i < 1000000; i++) {
        if (!(inb(ATA_IO + 7) & 0x80))
            return;
    }
}

/* wait until BSY clears and DRQ is set; -1 on error/timeout */
static int wait_drq(void)
{
    for (u32 i = 0; i < 1000000; i++) {
        u8 st = inb(ATA_IO + 7);
        if (st & 0x80)
            continue;
        if (st & 0x01)
            return -1;
        if (st & 0x08)
            return 0;
    }
    return -1;
}

int ata_init(void)
{
    present = 0;
    outb(ATA_IO + 6, 0xA0);         /* master */
    delay400();
    outb(ATA_IO + 2, 0);
    outb(ATA_IO + 3, 0);
    outb(ATA_IO + 4, 0);
    outb(ATA_IO + 5, 0);
    outb(ATA_IO + 7, 0xEC);         /* IDENTIFY */

    u8 st = inb(ATA_IO + 7);
    if (st == 0)
        return -1;
    if (wait_drq() < 0)
        return -1;

    for (int i = 0; i < 256; i++)
        (void)inw(ATA_IO);

    present = 1;
    return 0;
}

int ata_read(u32 lba, u32 count, void *buf)
{
    u16 *p = (u16 *)buf;

    if (!present)
        return -1;

    while (count) {
        u32 n = count > 255 ? 255 : count;

        wait_idle();
        outb(ATA_IO + 6, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
        delay400();
        outb(ATA_IO + 2, (u8)n);
        outb(ATA_IO + 3, (u8)(lba & 0xFF));
        outb(ATA_IO + 4, (u8)((lba >> 8) & 0xFF));
        outb(ATA_IO + 5, (u8)((lba >> 16) & 0xFF));
        outb(ATA_IO + 7, 0x20);     /* READ SECTORS */

        for (u32 s = 0; s < n; s++) {
            if (wait_drq() < 0)
                return -1;
            for (int i = 0; i < 256; i++)
                *p++ = inw(ATA_IO);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(u32 lba, u32 count, const void *buf)
{
    const u16 *p = (const u16 *)buf;

    if (!present)
        return -1;

    while (count) {
        u32 n = count > 255 ? 255 : count;

        wait_idle();
        outb(ATA_IO + 6, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
        delay400();
        outb(ATA_IO + 2, (u8)n);
        outb(ATA_IO + 3, (u8)(lba & 0xFF));
        outb(ATA_IO + 4, (u8)((lba >> 8) & 0xFF));
        outb(ATA_IO + 5, (u8)((lba >> 16) & 0xFF));
        outb(ATA_IO + 7, 0x30);     /* WRITE SECTORS */

        for (u32 s = 0; s < n; s++) {
            if (wait_drq() < 0)
                return -1;
            for (int i = 0; i < 256; i++)
                outw(ATA_IO, *p++);
        }
        outb(ATA_IO + 7, 0xE7);     /* FLUSH CACHE */
        delay400();
        wait_idle();
        lba += n;
        count -= n;
    }
    return 0;
}
