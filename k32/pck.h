/* pck package manager (PCK1 format) */
#ifndef ROS_PCK_H
#define ROS_PCK_H

#include "types.h"

void pck_list(void);
int  pck_search(const char *keyword);
int  pck_install(const char *name);
int  pck_remove(const char *name);

#endif
