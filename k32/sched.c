/* Preemptive round-robin task scheduler with ring-3 user processes */
#include "sched.h"
#include "paging.h"
#include "console.h"
#include "string.h"

#define TASK_FREE     0
#define TASK_RUNNABLE 1

#define KSTACK_SIZE   8192
#define QUANTUM       5             /* timer ticks (50 ms at 100 Hz) */
#define KERNEL_STACK  0x9F000       /* boot stack top, used by task 0 */

#define USER_CS       0x1B          /* GDT 0x18 | RPL 3 */
#define USER_DS       0x23          /* GDT 0x20 | RPL 3 */

/* 19 dwords: gs fs es ds edi esi ebp esp ebx edx ecx eax vec err eip cs
 * eflags user_esp user_ss */
#define FRAME_DWORDS  19

struct task {
    u32  esp;
    u32  cr3;
    u32  kstack_top;
    int  pid;
    int  state;
    int  user;
    char name[16];
};

static struct task tasks[TASK_MAX];
static u8 kstacks[TASK_MAX][KSTACK_SIZE] __attribute__((aligned(16)));
static int current;
static u32 tick_count;

/* ---- GDT / TSS ---- */
struct gdt_entry {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct tss {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1;
    u32 ss1;
    u32 esp2;
    u32 ss2;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt;
    u16 trap;
    u16 iomap_base;
    u8  iomap[1024];
} __attribute__((packed));

static struct gdt_entry gdt[6] __attribute__((aligned(8)));
static struct gdt_ptr gdtr;
static struct tss tss;

static void gdt_set(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[i].limit_lo = (u16)(limit & 0xFFFF);
    gdt[i].base_lo = (u16)(base & 0xFFFF);
    gdt[i].base_mid = (u8)((base >> 16) & 0xFF);
    gdt[i].access = access;
    gdt[i].gran = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].base_hi = (u8)((base >> 24) & 0xFF);
}

static void gdt_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;
    tss.esp0 = KERNEL_STACK;
    tss.iomap_base = 104;                   /* offsetof(struct tss, iomap) */
    memset(tss.iomap, 0xFF, sizeof(tss.iomap));    /* no I/O from ring 3 */

    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);     /* 0x08 kernel code */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);     /* 0x10 kernel data */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);     /* 0x18 user code */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);     /* 0x20 user data */
    gdt_set(5, (u32)&tss, sizeof(tss) - 1, 0x89, 0x00);   /* 0x28 TSS */

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (u32)gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr));

    u16 data = 0x10;
    __asm__ volatile("mov %0, %%ds; mov %0, %%es; mov %0, %%fs; "
                     "mov %0, %%gs; mov %0, %%ss" : : "r"(data));
    u16 sel = 0x28;
    __asm__ volatile("ltr %0" : : "r"(sel));
}

/* ---- task management ---- */
static void task_activate(int i)
{
    current = i;
    tss.esp0 = tasks[i].kstack_top;
    u32 cr3 = tasks[i].cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static int task_pick_next(void)
{
    for (int k = 1; k <= TASK_MAX; k++) {
        int i = (current + k) % TASK_MAX;
        if (tasks[i].state == TASK_RUNNABLE)
            return i;
    }
    return current;
}

static int any_user_task(void)
{
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_RUNNABLE)
            return 1;
    }
    return 0;
}

static void init_user_frame(int slot, u32 eip)
{
    u32 top = tasks[slot].kstack_top;
    u32 *sp = (u32 *)(top - FRAME_DWORDS * 4);

    memset(sp, 0, FRAME_DWORDS * 4);
    sp[0] = 0x10;                           /* gs */
    sp[1] = 0x10;                           /* fs */
    sp[2] = 0x10;                           /* es */
    sp[3] = 0x10;                           /* ds */
    sp[14] = eip;                           /* eip */
    sp[15] = USER_CS;
    sp[16] = 0x202;                         /* eflags: IF */
    sp[17] = USER_STACK_VA + PAGE_SIZE;     /* user esp */
    sp[18] = USER_DS;

    tasks[slot].esp = (u32)sp;
}

void sched_init(void)
{
    gdt_init();

    memset(tasks, 0, sizeof(tasks));
    for (int i = 0; i < TASK_MAX; i++)
        tasks[i].state = TASK_FREE;

    tasks[0].pid = 0;
    tasks[0].state = TASK_RUNNABLE;
    tasks[0].user = 0;
    tasks[0].cr3 = paging_kernel_cr3();
    tasks[0].kstack_top = KERNEL_STACK;
    tasks[0].esp = 0;
    strcpy(tasks[0].name, "shell");

    current = 0;
    tick_count = 0;
    task_activate(0);
}

int sched_spawn(const char *name, const u8 *code, u32 size)
{
    int slot = -1;

    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    if (!size || size > USER_PT_INDEX * PAGE_SIZE)
        return -2;

    u32 cr3 = paging_new_user_space(code, size);
    if (!cr3)
        return -3;

    tasks[slot].pid = slot;
    tasks[slot].state = TASK_RUNNABLE;
    tasks[slot].user = 1;
    tasks[slot].cr3 = cr3;
    tasks[slot].kstack_top = (u32)(kstacks[slot] + KSTACK_SIZE);
    strncpy(tasks[slot].name, name, sizeof(tasks[slot].name) - 1);
    tasks[slot].name[sizeof(tasks[slot].name) - 1] = 0;
    init_user_frame(slot, USER_CODE_VA);

    return slot;
}

int sched_kill(int pid)
{
    if (pid <= 0 || pid >= TASK_MAX)
        return -1;
    if (tasks[pid].state == TASK_FREE)
        return -2;
    if (pid == current)
        return -3;

    u32 cr3 = tasks[pid].cr3;
    int user = tasks[pid].user;
    tasks[pid].state = TASK_FREE;
    tasks[pid].cr3 = 0;
    tasks[pid].esp = 0;
    if (user)
        paging_free_user_space(cr3);
    return 0;
}

int sched_current_pid(void)
{
    return tasks[current].pid;
}

const char *sched_current_name(void)
{
    return tasks[current].name;
}

void sched_ps(void)
{
    con_puts(" PID  TYPE    STATE    NAME             CR3\n");
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_FREE)
            continue;
        con_printf("%d\t%s\t%s\t%s\t0x%x\n", i,
                   tasks[i].user ? "user" : "kernel",
                   i == current ? "running" : "ready",
                   tasks[i].name, tasks[i].cr3);
    }
}

/* ---- scheduling entry points ---- */
u32 sched_on_tick(regs_t *r)
{
    if (!any_user_task()) {
        tick_count = 0;
        return (u32)r;
    }
    if (++tick_count % QUANTUM)
        return (u32)r;

    tasks[current].esp = (u32)r;
    int next = task_pick_next();
    if (next == current)
        return (u32)r;

    task_activate(next);
    return tasks[next].esp;
}

u32 sched_yield(regs_t *r)
{
    tasks[current].esp = (u32)r;
    int next = task_pick_next();
    if (next == current)
        return (u32)r;

    task_activate(next);
    return tasks[next].esp;
}

u32 sched_exit_current(regs_t *r)
{
    int dead = current;
    u32 dead_cr3 = tasks[dead].cr3;
    int was_user = tasks[dead].user;

    tasks[dead].state = TASK_FREE;

    int next = task_pick_next();
    if (next == dead) {
        tasks[dead].state = TASK_RUNNABLE;
        return (u32)r;
    }

    task_activate(next);
    tasks[dead].cr3 = 0;
    tasks[dead].esp = 0;
    if (was_user)
        paging_free_user_space(dead_cr3);
    return tasks[next].esp;
}

u32 sched_kill_current(regs_t *r)
{
    return sched_exit_current(r);
}
