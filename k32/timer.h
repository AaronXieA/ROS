/* PIT timer (100 Hz) */
#ifndef ROS_TIMER_H
#define ROS_TIMER_H

#include "types.h"

void timer_init(void);
void timer_tick(void);
u32  timer_ticks(void);         /* 10 ms units */
void timer_sleep_ms(u32 ms);

#endif
