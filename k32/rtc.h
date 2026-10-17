/* CMOS real-time clock */
#ifndef ROS_RTC_H
#define ROS_RTC_H

#include "types.h"

void rtc_read(u8 *h, u8 *m, u8 *s, u8 *day, u8 *mon, u8 *year, u8 *cent);
int  rtc_set_time(u8 h, u8 m, u8 s);
int  rtc_set_date(u8 year, u8 mon, u8 day, u8 cent);
u8   bcd_to_bin(u8 v);
u8   bin_to_bcd(u8 v);

#endif
