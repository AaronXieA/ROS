/* ATA PIO disk driver (primary master, LBA28) */
#ifndef ROS_ATA_H
#define ROS_ATA_H

#include "types.h"

int  ata_init(void);
int  ata_read(u32 lba, u32 count, void *buf);
int  ata_write(u32 lba, u32 count, const void *buf);

#endif
