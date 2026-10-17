/* PS/2 keyboard (scancode set 1) */
#ifndef ROS_KEYBOARD_H
#define ROS_KEYBOARD_H

#include "types.h"

void keyboard_init(void);
void keyboard_irq(void);
int  kbd_getchar(void);         /* -1 if empty */
int  kbd_waitchar(void);        /* blocks, interrupts enabled */

#endif
