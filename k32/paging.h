/* Paging, virtual memory and physical frame allocation */
#ifndef ROS_PAGING_H
#define ROS_PAGING_H

#include "types.h"

#define PAGE_SIZE      4096

/* User address space layout (each process gets its own page directory,
 * one page table covers the whole 4 MB user region):
 *   0x40000000  code + data (up to USER_CODE_MAX)
 *   code end    heap, grows up on demand via SYS_BRK
 *   0x403FE000  stack (USER_STACK_PAGES pages, grows down) */
#define USER_CODE_VA      0x40000000u
#define USER_STACK_TOP    0x40400000u
#define USER_STACK_PAGES  2
#define USER_STACK_VA     (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_CODE_MAX     (256u * PAGE_SIZE)

void paging_init(void);
u32  paging_kernel_cr3(void);
u32  paging_user_phys(u32 va);
u32  paging_new_user_space(const u8 *code, u32 size);
void paging_free_user_space(u32 cr3);
void paging_user_map(u32 cr3, u32 va, u32 pages);
void paging_user_unmap(u32 cr3, u32 va, u32 pages);
u32  paging_frame_total(void);
u32  paging_frame_used(void);

#endif
