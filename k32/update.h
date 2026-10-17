/* system image update over the host hub (plain HTTP) */
#ifndef ROS_UPDATE_H
#define ROS_UPDATE_H

#include "types.h"

/* fetch /os/meta.txt: 0 ok, -1 no NIC, -2 hub unreachable, -3 bad metadata */
int os_update_info(char *version, u32 vlen, u32 *image_size);

/* stream /os/image straight onto the disk:
 * 0 ok, -1 no NIC, -2 hub error, -3 network, -4 disk write,
 * -5 not a bootable image, -6 incomplete / size mismatch */
int os_update_apply(u32 expected_size);

#endif
