/* Paging, virtual memory and physical frame allocation */
#ifndef ROS_PAGING_H
#define ROS_PAGING_H

#include "types.h"

#define PAGE_SIZE      4096

/* User address space layout (each process gets its own page directory) */
#define USER_CODE_VA   0x40000000u
#define USER_STACK_VA  0x40080000u
#define USER_PT_INDEX  ((USER_STACK_VA >> 12) & 0x3FF)

void paging_init(void);
u32  paging_kernel_cr3(void);
u32  paging_user_phys(u32 va);
u32  paging_new_user_space(const u8 *code, u32 size);
void paging_free_user_space(u32 cr3);
u32  paging_frame_total(void);
u32  paging_frame_used(void);

#endif
