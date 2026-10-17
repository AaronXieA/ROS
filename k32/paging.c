/* Paging, virtual memory and physical frame allocation */
#include "paging.h"
#include "string.h"

#define MAX_MEM        (64u * 1024 * 1024)
#define MAX_FRAMES     (MAX_MEM / PAGE_SIZE)
#define FIRST_FRAME    256                  /* 1 MB: kernel lives below */
#define USER_PD_INDEX  (USER_CODE_VA >> 22)

struct e820_entry {
    u64 base;
    u64 len;
    u32 type;
    u32 acpi;
};

extern struct e820_entry boot_e820[];
extern u32 boot_e820_count;

/* Page directory used by the kernel and as the template for every process */
static u32 kernel_pd[1024] __attribute__((aligned(PAGE_SIZE)));

/* One bit per 4 KB physical frame: 1 = used */
static u8 frame_bitmap[MAX_FRAMES / 8];
static u32 frame_count;
static u32 frame_used_count;

static int frame_used(u32 f)
{
    return (frame_bitmap[f >> 3] >> (f & 7)) & 1;
}

static void frame_set(u32 f, int used)
{
    u8 bit = (u8)(1u << (f & 7));
    u8 *p = &frame_bitmap[f >> 3];

    if (used) {
        if (!(*p & bit)) {
            *p |= bit;
            frame_used_count++;
        }
    } else {
        if (*p & bit) {
            *p &= (u8)~bit;
            frame_used_count--;
        }
    }
}

static u32 frame_alloc(void)
{
    for (u32 f = FIRST_FRAME; f < frame_count; f++) {
        if (frame_used(f))
            continue;
        frame_set(f, 1);
        u32 addr = f * PAGE_SIZE;
        memset((void *)addr, 0, PAGE_SIZE);
        return addr;
    }
    return 0;
}

static u32 identity_pdes(void)
{
    return (frame_count * PAGE_SIZE + 0x3FFFFF) / 0x400000;
}

void paging_init(void)
{
    u64 max_end = 0;

    memset(kernel_pd, 0, sizeof(kernel_pd));
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    frame_used_count = 0;

    for (u32 i = 0; i < boot_e820_count; i++) {
        if (boot_e820[i].type != 1)
            continue;
        u64 end = boot_e820[i].base + boot_e820[i].len;
        if (end > max_end)
            max_end = end;
    }
    if (max_end > MAX_MEM)
        max_end = MAX_MEM;

    frame_count = (u32)(max_end / PAGE_SIZE);
    frame_used_count = frame_count;

    /* Free everything the firmware reported as usable, above 1 MB */
    for (u32 i = 0; i < boot_e820_count; i++) {
        if (boot_e820[i].type != 1)
            continue;
        u64 s = boot_e820[i].base;
        u64 e = boot_e820[i].base + boot_e820[i].len;
        if (s < (u64)FIRST_FRAME * PAGE_SIZE)
            s = (u64)FIRST_FRAME * PAGE_SIZE;
        if (e > max_end)
            e = max_end;
        for (u32 f = (u32)(s / PAGE_SIZE); f < (u32)(e / PAGE_SIZE); f++)
            frame_set(f, 0);
    }

    /* Identity-map all physical memory with 4 MB supervisor pages */
    u32 pdes = identity_pdes();
    for (u32 i = 0; i < pdes; i++)
        kernel_pd[i] = (i * 0x400000) | 0x83;   /* P | RW | PS */

    u32 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10;                                /* CR4.PSE: 4 MB pages */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    u32 cr3 = (u32)kernel_pd;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;                         /* CR0.PG */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* Release the page directory, page table and user frames of a process */
void paging_free_user_space(u32 cr3)
{
    if (!cr3 || cr3 == (u32)kernel_pd)
        return;

    u32 *pd = (u32 *)cr3;
    u32 pde = pd[USER_PD_INDEX];
    if (pde & 1) {
        u32 pt_phys = pde & 0xFFFFF000u;
        u32 *pt = (u32 *)pt_phys;
        for (u32 i = 0; i < 1024; i++) {
            if (!(pt[i] & 1))
                continue;
            u32 f = (pt[i] & 0xFFFFF000u) / PAGE_SIZE;
            if (f >= FIRST_FRAME && f < frame_count)
                frame_set(f, 0);
        }
        frame_set(pt_phys / PAGE_SIZE, 0);
    }
    frame_set(cr3 / PAGE_SIZE, 0);
}

u32 paging_kernel_cr3(void)
{
    return (u32)kernel_pd;
}

u32 paging_frame_total(void)
{
    return frame_count;
}

u32 paging_frame_used(void)
{
    return frame_used_count;
}

/* Walk the current page directory. Returns 0 if the page is not mapped. */
u32 paging_user_phys(u32 va)
{
    u32 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    u32 *pd = (u32 *)cr3;
    u32 pde = pd[va >> 22];
    if (!(pde & 1))
        return 0;
    if (pde & 0x80)                             /* 4 MB page */
        return (pde & 0xFFC00000u) | (va & 0x3FFFFF);

    u32 *pt = (u32 *)(pde & 0xFFFFF000u);
    u32 pte = pt[(va >> 12) & 0x3FF];
    if (!(pte & 1))
        return 0;
    return (pte & 0xFFFFF000u) | (va & 0xFFF);
}

/* Build a private address space: kernel identity map + code + stack */
u32 paging_new_user_space(const u8 *code, u32 size)
{
    u32 max_pages = USER_PT_INDEX;
    u32 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!size || pages > max_pages)
        return 0;

    u32 pd_phys = frame_alloc();
    u32 pt_phys = frame_alloc();
    if (!pd_phys || !pt_phys)
        return 0;

    u32 *pd = (u32 *)pd_phys;
    u32 *pt = (u32 *)pt_phys;

    u32 pdes = identity_pdes();
    for (u32 i = 0; i < pdes; i++)
        pd[i] = kernel_pd[i];
    pd[USER_PD_INDEX] = pt_phys | 0x7;          /* P | RW | US */

    for (u32 i = 0; i < pages; i++) {
        u32 f = frame_alloc();
        if (!f)
            return 0;
        u32 n = size - i * PAGE_SIZE;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        memcpy((void *)f, code + i * PAGE_SIZE, n);
        pt[i] = f | 0x7;
    }

    u32 stack = frame_alloc();
    if (!stack)
        return 0;
    pt[USER_PT_INDEX] = stack | 0x7;

    return pd_phys;
}
