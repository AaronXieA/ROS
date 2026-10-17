#include "pck.h"
#include "fat.h"
#include "console.h"
#include "string.h"
#include "net.h"
#include "hub.h"

#define PKG_MAX (64 * 1024)

static u8 pkg_buf[PKG_MAX];
static u8 dl_buf[PKG_MAX];

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void fmt_name(const u8 *raw, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = (char)raw[i];
    }
    out[n] = 0;
}

static void base_name(const char *name, char *out)
{
    int i = 0;
    while (name[i] && name[i] != '.') {
        out[i] = name[i];
        i++;
    }
    out[i] = 0;
}

static int find_pck_dir(struct fat_dirent *d)
{
    if (fs_find_at(0, "PCK", d) < 0)
        return -1;
    if (!(d->attr & 0x10))
        return -1;
    return 0;
}

/* "demo" or "demo.pck" -> "DEMO.PCK" */
static void pkg_file_name(const char *name, char *out)
{
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        out[j++] = c;
    }
    out[j++] = '.';
    out[j++] = 'P';
    out[j++] = 'C';
    out[j++] = 'K';
    out[j] = 0;
}

static int list_cb(const struct fat_dirent *de, void *ctx)
{
    int *n = ctx;
    char base[13];
    base_name(de->name, base);
    con_printf("%s\n", base);
    (*n)++;
    return 0;
}

void pck_list(void)
{
    struct fat_dirent d;
    int n = 0;

    if (find_pck_dir(&d) < 0) {
        con_puts("No PCK directory.\n");
        return;
    }
    if (fs_list_at(d.cluster, list_cb, &n) < 0) {
        con_puts("Cannot read PCK directory.\n");
        return;
    }
    con_printf("%d package(s)\n", n);
}

static int load_pkg(const char *name, u32 *size_out)
{
    struct fat_dirent dir, pkg;
    char fname[16];

    if (find_pck_dir(&dir) < 0)
        return -1;
    pkg_file_name(name, fname);
    if (fs_find_at(dir.cluster, fname, &pkg) < 0)
        return -1;
    if (pkg.attr & 0x10)
        return -1;
    if (pkg.size > PKG_MAX)
        return -2;
    if (fs_read_file(&pkg, pkg_buf, PKG_MAX) != (int)pkg.size)
        return -2;
    if (memcmp(pkg_buf, "PCK1", 4) != 0)
        return -3;
    *size_out = pkg.size;
    return 0;
}

static int download_pkg(const char *name)
{
    struct fat_dirent dir;
    char fname[16], path[24];
    u32 size = 0;
    int n = 1;

    if (!net_ready())
        return -7;

    pkg_file_name(name, fname);
    path[0] = '/';
    for (int i = 0; fname[i] && n < 20; i++)
        path[n++] = fname[i];
    path[n] = 0;

    if (net_http_get(HUB_IP, HUB_PORT, path, dl_buf, PKG_MAX, &size) < 0)
        return -8;
    if (size < 6 || memcmp(dl_buf, "PCK1", 4) != 0)
        return -5;
    if (find_pck_dir(&dir) < 0)
        return -9;

    fs_delete_at(dir.cluster, fname);
    if (fs_create_at(dir.cluster, fname, dl_buf, size) != 0)
        return -9;
    return 0;
}

int pck_install(const char *name)
{
    u32 size;
    int rc = load_pkg(name, &size);

    if (rc == -1) {                 /* not cached locally: fetch from the hub */
        rc = download_pkg(name);
        if (rc < 0)
            return rc;
        rc = load_pkg(name, &size);
    }
    if (rc == -2)
        return -4;
    if (rc == -3)
        return -5;
    if (rc < 0)
        return -1;

    u16 count = rd16(pkg_buf + 4);
    u32 off = 6;

    for (u16 i = 0; i < count; i++) {
        if (off + 15 > size)
            return -5;
        u8 *raw = pkg_buf + off;
        u32 len = rd32(raw + 11);
        off += 15;
        if (off + len > size)
            return -5;

        char disp[13];
        fmt_name(raw, disp);
        fs_delete(disp);            /* replace if it already exists */
        if (fs_create(disp, pkg_buf + off, len) != 0)
            return -6;
        off += len;
    }
    return 0;
}

static char pck_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int pck_contains(const char *hay, u32 len, const char *needle)
{
    u32 n = strlen(needle);

    if (!n)
        return 1;
    if (n > len)
        return 0;
    for (u32 i = 0; i + n <= len; i++) {
        u32 j = 0;
        while (j < n && pck_lower(hay[i + j]) == pck_lower(needle[j]))
            j++;
        if (j == n)
            return 1;
    }
    return 0;
}

int pck_search(const char *keyword)
{
    static char idx[4096];
    u32 n = 0, i = 0;
    int hits = 0;

    if (!net_ready())
        return -7;
    if (net_http_get(HUB_IP, HUB_PORT, "/index.txt", (u8 *)idx,
                     sizeof(idx) - 1, &n) < 0)
        return -8;

    while (i < n) {
        u32 start = i;
        while (i < n && idx[i] != '\n')
            i++;
        u32 end = i;
        if (i < n)
            i++;
        if (end > start && idx[end - 1] == '\r')
            end--;

        if (end > start &&
            (!keyword || pck_contains(idx + start, end - start, keyword))) {
            for (u32 j = start; j < end; j++)
                con_putc(idx[j]);
            con_putc('\n');
            hits++;
        }
    }

    con_printf("%d package(s)\n", hits);
    return 0;
}

int pck_remove(const char *name)
{
    u32 size;
    int rc = load_pkg(name, &size);
    if (rc == -2)
        return -4;
    if (rc == -3)
        return -5;
    if (rc < 0)
        return -1;

    u16 count = rd16(pkg_buf + 4);
    u32 off = 6;

    for (u16 i = 0; i < count; i++) {
        if (off + 15 > size)
            return -5;
        u8 *raw = pkg_buf + off;
        u32 len = rd32(raw + 11);
        off += 15;
        if (off + len > size)
            return -5;

        char disp[13];
        fmt_name(raw, disp);
        fs_delete(disp);
        off += len;
    }
    return 0;
}
