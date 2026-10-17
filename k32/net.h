/* NE2000 (RTL8029) network driver, ARP + IPv4/ICMP */
#ifndef ROS_NET_H
#define ROS_NET_H

#include "types.h"

int  net_init(void);
int  net_ready(void);
void net_info(void);
int  net_parse_ip(const char *s, u32 *out);
void net_ip_str(u32 ip, char *out);
int  net_ping(u32 ip);

#endif
