#include "idt.h"
#include "io.h"
#include "console.h"
#include "timer.h"
#include "keyboard.h"

extern void (*isr_table[48])(void);

struct idt_entry {
    u16 off_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 off_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

static void idt_set(int n, void (*fn)(void))
{
    u32 a = (u32)fn;
    idt[n].off_lo = a & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].zero = 0;
    idt[n].flags = 0x8E;        /* present, ring0, 32-bit interrupt gate */
    idt[n].off_hi = (a >> 16) & 0xFFFF;
}

void pic_remap(void)
{
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();
    outb(0x21, 0xFF);           /* mask everything for now */
    outb(0xA1, 0xFF);
}

void irq_mask(u8 irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = irq & 7;
    outb(port, inb(port) | (1 << bit));
}

void irq_unmask(u8 irq)
{
    u16 port = (irq < 8) ? 0x21 : 0xA1;
    u8 bit = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}

void irq_eoi(u8 irq)
{
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_table[i]);
    for (int i = 48; i < 256; i++)
        idt_set(i, isr_table[0]);   /* unused: #DE */

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (u32)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
}

static const char *exc_name(u32 v)
{
    switch (v) {
    case 0:  return "#DE Divide error";
    case 1:  return "#DB Debug";
    case 2:  return "NMI";
    case 3:  return "#BP Breakpoint";
    case 4:  return "#OF Overflow";
    case 5:  return "#BR Bound range";
    case 6:  return "#UD Invalid opcode";
    case 7:  return "#NM Device not available";
    case 8:  return "#DF Double fault";
    case 10: return "#TS Invalid TSS";
    case 11: return "#NP Segment not present";
    case 12: return "#SS Stack fault";
    case 13: return "#GP General protection";
    case 14: return "#PF Page fault";
    case 16: return "#MF x87 FPU error";
    case 17: return "#AC Alignment check";
    default: return "Unknown CPU exception";
    }
}

void fatal_exception(regs_t *r)
{
    con_clear();
    con_fatal_banner();
    con_printf("\nA fatal exception has occurred.\n\n");
    con_printf("Reason: %s\n", exc_name(r->vec));
    con_printf("Vector: 0x%x  Error code: 0x%x\n", r->vec, r->err);
    con_printf("CS:EIP = 0x%x:0x%x\n", r->cs, r->eip);
    con_printf("EFLAGS = 0x%x\n", r->eflags);
    con_printf("EAX=0x%x EBX=0x%x ECX=0x%x EDX=0x%x\n",
               r->eax, r->ebx, r->ecx, r->edx);
    con_printf("ESI=0x%x EDI=0x%x EBP=0x%x ESP=0x%x\n",
               r->esi, r->edi, r->ebp, r->esp_dummy);
    con_printf("DS=0x%x ES=0x%x FS=0x%x GS=0x%x\n",
               r->ds, r->es, r->fs, r->gs);
    con_printf("\nSystem halted. Press reset to reboot.\n");
    for (;;)
        cpu_halt();
}

void isr_handler(regs_t *r)
{
    if (r->vec < 32) {
        fatal_exception(r);
    } else {
        u32 irq = r->vec - 32;
        if (irq == 0)
            timer_tick();
        else if (irq == 1)
            keyboard_irq();
        else if (irq == 7 || irq == 15)
            return;             /* spurious */
        irq_eoi((u8)irq);
    }
}
