/* Interrupt descriptor table, PIC, and exception reporting */
#ifndef ROS_IDT_H
#define ROS_IDT_H

#include "types.h"

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags;
} regs_t;

void idt_init(void);
void pic_remap(void);
void irq_mask(u8 irq);
void irq_unmask(u8 irq);
void irq_eoi(u8 irq);
void fatal_exception(regs_t *r);

#endif
