/* VGA text console */
#ifndef ROS_CONSOLE_H
#define ROS_CONSOLE_H

#include "types.h"

void con_init(void);
void con_clear(void);
void con_putc(char c);
void con_puts(const char *s);
void con_printf(const char *fmt, ...);
void con_set_color(u8 color);
void con_fatal_banner(void);

/* output capture, used by shell redirection and pipes */
void con_capture_begin(char *buf, u32 max, u32 used);
u32  con_capture_end(int *truncated);
int  con_capture_active(void);

#endif
