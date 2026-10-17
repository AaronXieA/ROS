#include "fat.h"
#include "ata.h"
#include "string.h"
#include "console.h"

#define SEC 512

static struct {
    u32 part_lba;
    u16 reserved;
    u8  spc;
    u8  nfats;
    u16 root_entries;
    u16 spf;
    u32 fat_lba, root_lba, data_lba;
    u32 root_secs;
    u32 max_cluster;
    u32 cluster_size;
} fs;

static u8 fat_buf[64 * SEC];
static u8 dir_buf[32 * 1024];
static u8 clus_tmp[16 * 1024];

static u16 cwd_cluster;
static char cwd_path[96] = "/";
static u16 parents[16];
static int depth;
static int ready;

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void wr16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

void fs_shutdown(void)
{
    ready = 0;
}

int fs_init(void)
{
    u8 mbr[SEC];
    u8 vbr[SEC];
    ready = 0;
    if (ata_read(0, 1, mbr) < 0)
        return -1;

    u32 part = 0;
    for (int i = 0; i < 4; i++) {
        u8 *e = mbr + 446 + i * 16;
        if (e[4] != 0) {
            part = rd32(e + 8);
            break;
        }
    }
    if (!part)
        part = 2048;
    fs.part_lba = part;

    if (ata_read(part, 1, vbr) < 0)
        return -1;
    if (rd16(vbr + 11) != SEC)
        return -1;

    fs.spc = vbr[13];
    fs.reserved = rd16(vbr + 14);
    fs.nfats = vbr[16];
    fs.root_entries = rd16(vbr + 17);
    fs.spf = rd16(vbr + 22);

    u32 total = rd16(vbr + 19);
    if (!total)
        total = rd32(vbr + 32);

    fs.root_secs = (u32)fs.root_entries * 32 / SEC;
    fs.fat_lba = part + fs.reserved;
    fs.root_lba = fs.fat_lba + (u32)fs.nfats * fs.spf;
    fs.data_lba = fs.root_lba + fs.root_secs;
    fs.cluster_size = (u32)fs.spc * SEC;
    fs.max_cluster = (total - (fs.data_lba - part)) / fs.spc + 1;

    if (!fs.spc || !fs.spf || fs.spf > 64 || fs.root_secs > 64 ||
        fs.cluster_size > sizeof(clus_tmp))
        return -1;

    if (ata_read(fs.fat_lba, fs.spf, fat_buf) < 0)
        return -1;

    cwd_cluster = 0;
    strcpy(cwd_path, "/");
    depth = 0;
    ready = 1;
    return 0;
}

static u16 fat_get(u16 c)
{
    return rd16(fat_buf + (u32)c * 2);
}

static void fat_set(u16 c, u16 v)
{
    wr16(fat_buf + (u32)c * 2, v);
}

static int fat_flush(void)
{
    if (ata_write(fs.fat_lba, fs.spf, fat_buf) < 0)
        return -1;
    if (fs.nfats > 1 && ata_write(fs.fat_lba + fs.spf, fs.spf, fat_buf) < 0)
        return -1;
    return 0;
}

static u32 clust_lba(u16 c)
{
    return fs.data_lba + (u32)(c - 2) * fs.spc;
}

static int read_cluster(u16 c, void *buf)
{
    return ata_read(clust_lba(c), fs.spc, buf);
}

static int write_cluster(u16 c, const void *buf)
{
    return ata_write(clust_lba(c), fs.spc, buf);
}

static u16 alloc_cluster(void)
{
    for (u32 c = 2; c < fs.max_cluster; c++) {
        if (fat_get((u16)c) == 0) {
            fat_set((u16)c, 0xFFFF);
            return (u16)c;
        }
    }
    return 0;
}

static void free_chain(u16 c)
{
    while (c >= 2 && c < 0xFFF8) {
        u16 next = fat_get(c);
        fat_set(c, 0);
        c = next;
    }
}

/* ---- directory buffer ---- */
static u16 dir_first;
static u32 dir_bytes;

static int dir_load(u16 cluster)
{
    dir_first = cluster;

    if (cluster == 0) {
        dir_bytes = fs.root_secs * SEC;
        return ata_read(fs.root_lba, fs.root_secs, dir_buf);
    }

    u32 total = 0;
    u16 c = cluster;
    while (c >= 2 && c < 0xFFF8) {
        if (total + fs.cluster_size > sizeof(dir_buf))
            return -1;
        if (read_cluster(c, dir_buf + total) < 0)
            return -1;
        total += fs.cluster_size;
        c = fat_get(c);
    }
    dir_bytes = total;
    return 0;
}

static int dir_flush(void)
{
    if (dir_first == 0)
        return ata_write(fs.root_lba, fs.root_secs, dir_buf);

    u32 off = 0;
    u16 c = dir_first;
    while (c >= 2 && c < 0xFFF8) {
        if (write_cluster(c, dir_buf + off) < 0)
            return -1;
        off += fs.cluster_size;
        c = fat_get(c);
    }
    return 0;
}

/* ---- names ---- */
static void make83(const char *s, u8 out[11])
{
    memset(out, ' ', 11);

    int n = 0;
    while (*s == ' ')
        s++;
    while (*s && *s != '.' && n < 8) {
        char c = *s++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        out[n++] = (u8)c;
    }
    while (*s && *s != '.')
        s++;
    if (*s == '.') {
        s++;
        n = 8;
        while (*s && n < 11) {
            char c = *s++;
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            out[n++] = (u8)c;
        }
    }
}

static void fmt83(const u8 *raw, char *out)
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

static int is_dot_entry(const u8 *e)
{
    return e[0] == '.' && (e[1] == ' ' || e[1] == '.');
}

/* ---- listing / lookup ---- */
int fs_list_at(u16 dir, int (*cb)(const struct fat_dirent *, void *), void *ctx)
{
    int count = 0;

    if (!ready || dir_load(dir) < 0)
        return -1;

    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
        u8 *e = dir_buf + off;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5)
            continue;
        if (e[11] == 0x0F || (e[11] & 0x08) || is_dot_entry(e))
            continue;

        struct fat_dirent de;
        fmt83(e, de.name);
        de.attr = e[11];
        de.cluster = rd16(e + 26);
        de.size = rd32(e + 28);
        if (cb(&de, ctx) < 0)
            break;
        count++;
    }
    return count;
}

int fs_list(int (*cb)(const struct fat_dirent *, void *), void *ctx)
{
    return fs_list_at(cwd_cluster, cb, ctx);
}

int fs_find_at(u16 dir, const char *name, struct fat_dirent *out)
{
    u8 raw[11];
    make83(name, raw);

    if (!ready || dir_load(dir) < 0)
        return -1;

    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
        u8 *e = dir_buf + off;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5)
            continue;
        if (e[11] == 0x0F)
            continue;
        if (memcmp(e, raw, 11) != 0)
            continue;

        fmt83(e, out->name);
        out->attr = e[11];
        out->cluster = rd16(e + 26);
        out->size = rd32(e + 28);
        return 0;
    }
    return -1;
}

int fs_find(const char *name, struct fat_dirent *out)
{
    return fs_find_at(cwd_cluster, name, out);
}

/* ---- file contents ---- */
int fs_read_file(const struct fat_dirent *de, u8 *dst, u32 max)
{
    u32 got = 0;
    u16 c = de->cluster;

    if (!ready || !c)
        return 0;

    while (c >= 2 && c < 0xFFF8 && got < de->size && got < max) {
        if (read_cluster(c, clus_tmp) < 0)
            break;
        u32 n = de->size - got;
        if (n > fs.cluster_size)
            n = fs.cluster_size;
        if (n > max - got)
            n = max - got;
        memcpy(dst + got, clus_tmp, n);
        got += n;
        c = fat_get(c);
    }
    return (int)got;
}

int fs_print_file(const struct fat_dirent *de)
{
    u32 left = de->size;
    u16 c = de->cluster;
    char last = 0;

    if (!ready || !c)
        return 0;

    while (left && c >= 2 && c < 0xFFF8) {
        if (read_cluster(c, clus_tmp) < 0)
            break;
        u32 n = left > fs.cluster_size ? fs.cluster_size : left;
        for (u32 i = 0; i < n; i++) {
            char ch = (char)clus_tmp[i];
            last = ch;
            if (ch == '\n')
                con_putc('\r');
            con_putc(ch);
        }
        left -= n;
        c = fat_get(c);
    }
    return last == '\n';
}

/* ---- creation / deletion ---- */
static int find_free_slot(u32 *slot, u32 *end_off)
{
    u32 last = 0;
    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
        u8 *e = dir_buf + off;
        if (e[0] == 0x00) {
            *slot = off;
            *end_off = off;
            return 0;
        }
        if (e[0] == 0xE5) {
            *slot = off;
            *end_off = off;
            return 0;
        }
        last = off + 32;
    }
    *slot = last;
    *end_off = last;
    return (last + 32 <= dir_bytes) ? 0 : -1;
}

int fs_create_at(u16 dir, const char *name, const u8 *data, u32 size)
{
    u8 raw[11];
    make83(name, raw);

    if (!ready || dir_load(dir) < 0)
        return -1;

    struct fat_dirent tmp;
    if (fs_find_at(dir, name, &tmp) == 0)
        return -2;              /* already exists */

    if (dir_load(dir) < 0)      /* fs_find_at reloaded the buffer */
        return -1;

    u32 slot, end;
    if (find_free_slot(&slot, &end) < 0)
        return -3;              /* directory full */

    u16 first = 0, prev = 0;
    const u8 *p = data;
    u32 left = size;

    while (left) {
        u16 c = alloc_cluster();
        if (!c) {
            if (first)
                free_chain(first);
            return -4;          /* disk full */
        }
        if (!first)
            first = c;
        else
            fat_set(prev, c);
        prev = c;

        u32 n = left > fs.cluster_size ? fs.cluster_size : left;
        memset(clus_tmp, 0, fs.cluster_size);
        memcpy(clus_tmp, p, n);
        if (write_cluster(c, clus_tmp) < 0)
            return -5;
        p += n;
        left -= n;
    }

    u8 *e = dir_buf + slot;
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = 0x20;
    wr16(e + 26, first);
    wr32(e + 28, size);

    if (dir_flush() < 0)
        return -5;
    if (fat_flush() < 0)
        return -6;
    return 0;
}

int fs_create(const char *name, const u8 *data, u32 size)
{
    return fs_create_at(cwd_cluster, name, data, size);
}

int fs_mkdir(const char *name)
{
    u8 raw[11];
    make83(name, raw);

    if (!ready || dir_load(cwd_cluster) < 0)
        return -1;

    struct fat_dirent tmp;
    if (fs_find(name, &tmp) == 0)
        return -2;

    if (dir_load(cwd_cluster) < 0)
        return -1;

    u32 slot, end;
    if (find_free_slot(&slot, &end) < 0)
        return -3;

    u16 c = alloc_cluster();
    if (!c)
        return -4;

    memset(clus_tmp, 0, fs.cluster_size);
    memset(clus_tmp, ' ', 11);
    clus_tmp[0] = '.';
    clus_tmp[11] = 0x10;
    wr16(clus_tmp + 26, c);

    memset(clus_tmp + 32, ' ', 11);
    clus_tmp[32] = '.';
    clus_tmp[33] = '.';
    clus_tmp[32 + 11] = 0x10;
    wr16(clus_tmp + 32 + 26, cwd_cluster);

    if (write_cluster(c, clus_tmp) < 0)
        return -5;

    u8 *e = dir_buf + slot;
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = 0x10;
    wr16(e + 26, c);
    wr32(e + 28, 0);

    if (dir_flush() < 0)
        return -5;
    if (fat_flush() < 0)
        return -6;
    return 0;
}

int fs_delete_at(u16 dir, const char *name)
{
    u8 raw[11];
    make83(name, raw);

    if (!ready || dir_load(dir) < 0)
        return -1;

    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
        u8 *e = dir_buf + off;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;
        if (memcmp(e, raw, 11) != 0)
            continue;

        u16 c = rd16(e + 26);
        e[0] = 0xE5;
        if (c)
            free_chain(c);

        if (dir_flush() < 0 || fat_flush() < 0)
            return -2;
        return 0;
    }
    return -1;
}

int fs_delete(const char *name)
{
    return fs_delete_at(cwd_cluster, name);
}

/* ---- current directory ---- */
const char *fs_cwd(void)
{
    return cwd_path;
}

int fs_cd(const char *name)
{
    if (!ready)
        return -1;

    if (!name || !*name || !strcmp(name, "/")) {
        cwd_cluster = 0;
        strcpy(cwd_path, "/");
        depth = 0;
        return 0;
    }

    if (!strcmp(name, "..")) {
        if (depth > 0) {
            cwd_cluster = parents[--depth];
            char *p = strrchr(cwd_path, '/');
            if (!p || p == cwd_path)
                strcpy(cwd_path, "/");
            else
                *p = 0;
        }
        return 0;
    }

    if (dir_load(cwd_cluster) < 0)
        return -1;

    u8 raw[11];
    make83(name, raw);
    for (u32 off = 0; off + 32 <= dir_bytes; off += 32) {
        u8 *e = dir_buf + off;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5 || e[11] == 0x0F)
            continue;
        if (memcmp(e, raw, 11) != 0)
            continue;
        if (!(e[11] & 0x10))
            return -2;          /* not a directory */

        if (depth < 16)
            parents[depth++] = cwd_cluster;
        cwd_cluster = rd16(e + 26);

        char disp[13];
        fmt83(e, disp);
        size_t len = strlen(cwd_path);
        if (len && cwd_path[len - 1] != '/') {
            cwd_path[len++] = '/';
            cwd_path[len] = 0;
        }
        strcpy(cwd_path + len, disp);
        return 0;
    }
    return -1;
}
