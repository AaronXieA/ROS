#include "update.h"
#include "net.h"
#include "ata.h"
#include "string.h"
#include "hub.h"

#define META_MAX 512
#define IMG_BUF_SIZE (32 * 1024)

static char meta[META_MAX];
static u8 img_buf[IMG_BUF_SIZE];

/* find "KEY value" in the metadata text; returns a pointer to value */
static const char *meta_field(const char *buf, const char *key)
{
    u32 klen = strlen(key);
    const char *p = buf;

    while (*p) {
        if (!strncmp(p, key, klen) && p[klen] == ' ') {
            p += klen + 1;
            while (*p == ' ')
                p++;
            return p;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return NULL;
}

int os_update_info(char *version, u32 vlen, u32 *image_size)
{
    u32 n = 0;
    const char *v, *s;
    u32 i = 0;

    if (!net_ready())
        return -1;
    if (net_http_get(HUB_IP, HUB_PORT, "/os/meta.txt", (u8 *)meta,
                     META_MAX - 1, &n) < 0)
        return -2;
    meta[n] = 0;

    v = meta_field(meta, "VERSION");
    if (!v)
        return -3;
    while (v[i] && v[i] != '\r' && v[i] != '\n' && i < vlen - 1) {
        version[i] = v[i];
        i++;
    }
    version[i] = 0;

    *image_size = 0;
    s = meta_field(meta, "SIZE");
    if (s) {
        u32 size = 0;
        while (*s >= '0' && *s <= '9')
            size = size * 10 + (u32)(*s++ - '0');
        *image_size = size;
    }
    return version[0] ? 0 : -3;
}

/* ---- streaming disk sink ---- */
struct disk_sink {
    u32 lba;
    u32 used;
    u32 total;
    int err;
};

static int disk_flush(struct disk_sink *d)
{
    u32 secs = d->used / 512;

    if (!secs)
        return 0;
    if (d->lba == 0 && d->used >= 512 &&
        (img_buf[510] != 0x55 || img_buf[511] != 0xAA))
        return -2;                      /* no MBR signature: refuse to write */
    if (ata_write(d->lba, secs, img_buf) < 0)
        return -1;
    d->lba += secs;
    d->used = 0;
    return 0;
}

static int disk_sink_fn(void *ctx, const u8 *data, u32 len)
{
    struct disk_sink *d = ctx;

    while (len) {
        u32 space = IMG_BUF_SIZE - d->used;
        u32 n = len < space ? len : space;

        memcpy(img_buf + d->used, data, n);
        d->used += n;
        d->total += n;
        data += n;
        len -= n;

        if (d->used == IMG_BUF_SIZE) {
            int rc = disk_flush(d);
            if (rc < 0) {
                d->err = (rc == -2) ? -5 : -4;
                return -1;
            }
        }
    }
    return 0;
}

int os_update_apply(u32 expected_size)
{
    struct disk_sink d;
    u32 total = 0;
    int rc;

    if (!net_ready())
        return -1;

    d.lba = 0;
    d.used = 0;
    d.total = 0;
    d.err = 0;

    rc = net_http_get_stream(HUB_IP, HUB_PORT, "/os/image",
                             disk_sink_fn, &d, &total);
    if (rc == -1)
        return -1;
    if (rc == -2)
        return -2;
    if (rc == -3 || rc == -4)
        return -3;
    if (rc == -6)
        return d.err ? d.err : -6;

    int fr = disk_flush(&d);
    if (fr == -2)
        return -5;
    if (fr < 0)
        return -4;

    if (d.total == 0 || (d.total % 512) != 0)
        return -6;
    if (expected_size && d.total != expected_size)
        return -6;
    return 0;
}
