/* FAT16 filesystem */
#ifndef ROS_FAT_H
#define ROS_FAT_H

#include "types.h"

struct fat_dirent {
    char name[13];              /* "NAME.EXT" */
    u8   attr;
    u16  cluster;
    u32  size;
};

int  fs_init(void);
int  fs_list(int (*cb)(const struct fat_dirent *, void *), void *ctx);
int  fs_list_at(u16 dir, int (*cb)(const struct fat_dirent *, void *), void *ctx);
int  fs_find(const char *name, struct fat_dirent *out);
int  fs_find_at(u16 dir, const char *name, struct fat_dirent *out);
int  fs_read_file(const struct fat_dirent *de, u8 *dst, u32 max);
int  fs_print_file(const struct fat_dirent *de);
int  fs_create(const char *name, const u8 *data, u32 size);
int  fs_mkdir(const char *name);
int  fs_delete(const char *name);
int  fs_cd(const char *name);
const char *fs_cwd(void);

#endif
