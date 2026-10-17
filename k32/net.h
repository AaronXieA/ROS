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

/* minimal blocking HTTP/1.1 GET client (plain HTTP) */
int  net_http_get(u32 ip, u16 port, const char *path, u8 *out, u32 max,
                  u32 *out_len);

/* streamed variant: body bytes are passed to sink() as they arrive */
typedef int (*net_sink_fn)(void *ctx, const u8 *data, u32 len);
int  net_http_get_stream(u32 ip, u16 port, const char *path,
                         net_sink_fn sink, void *ctx, u32 *out_len);

#endif
