#include "net.h"
#include "io.h"
#include "console.h"
#include "string.h"
#include "timer.h"

/* --- NE2000 registers (page 0 unless noted) --- */
#define NE_CMD      0x00
#define NE_PSTART   0x01
#define NE_PSTOP    0x02
#define NE_BNRY     0x03
#define NE_TPSR     0x04
#define NE_TBCR0    0x05
#define NE_TBCR1    0x06
#define NE_ISR      0x07
#define NE_RSAR0    0x08
#define NE_RSAR1    0x09
#define NE_RBCR0    0x0A
#define NE_RBCR1    0x0B
#define NE_RCR      0x0C
#define NE_TCR      0x0D
#define NE_DCR      0x0E
#define NE_IMR      0x0F
#define NE_DATA     0x10
#define NE_PAR0     0x01        /* page 1 */
#define NE_CURR     0x07        /* page 1 */
#define NE_MAR0     0x08        /* page 1 */

#define TX_PAGE     0x40
#define RX_START    0x46
#define RX_STOP     0x80

static u16 base;
static u8  irq_line;
static u8  mac[6];
static int ready;

static u32 my_ip = 0x0A00020F;  /* 10.0.2.15 */
static u32 gw_ip = 0x0A000202;  /* 10.0.2.2 */

static u8 arp_mac[6];
static u32 arp_ip;
static u32 arp_want;
static int arp_ok;

static int ping_got;
static u8 ping_ttl;

static u8 rxbuf[1600];
static u8 txbuf[1600];

/* ---- TCP client state ---- */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* 32 KB is enough to smooth NE2000 bursts without letting .bss grow into
 * the boot stack at 0x9F000 (see the ASSERT in k32/link.ld). */
#define TCP_RXBUF_SIZE (32 * 1024)
#define TCP_WINDOW     8192

static u32 tcp_remote_ip;
static u16 tcp_remote_port;
static u16 tcp_local_port;
static u32 tcp_snd_nxt;
static u32 tcp_rcv_nxt;
static u8  tcp_peer_mac[6];
static int tcp_state;           /* 0 closed, 1 SYN sent, 2 established */
static int tcp_got_reset;
static int tcp_got_fin;
static u32 tcp_rx_len;
static u8  tcp_rxbuf[TCP_RXBUF_SIZE];

/* ---- PCI config space ---- */
static u32 pci_read(u32 addr)
{
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write(u32 addr, u32 val)
{
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

/* ---- register helpers ---- */
static void ne_out(u8 reg, u8 v)
{
    outb((u16)(base + reg), v);
}

static u8 ne_in(u8 reg)
{
    return inb((u16)(base + reg));
}

static int wait_isr(u8 bit, u32 timeout)
{
    for (u32 i = 0; i < timeout; i++) {
        if (ne_in(NE_ISR) & bit)
            return 0;
    }
    return -1;
}

/* ---- remote DMA ---- */
static void dma_setup(u16 addr, u16 len)
{
    ne_out(NE_CMD, 0x21);       /* stop, page 0 */
    ne_out(NE_ISR, 0xFF);
    ne_out(NE_RSAR0, (u8)(addr & 0xFF));
    ne_out(NE_RSAR1, (u8)(addr >> 8));
    ne_out(NE_RBCR0, (u8)(len & 0xFF));
    ne_out(NE_RBCR1, (u8)(len >> 8));
}

static void dma_write(u16 addr, const u8 *p, u16 len)
{
    dma_setup(addr, len);
    ne_out(NE_CMD, 0x12);       /* remote write */
    for (u16 i = 0; i < len; i++)
        outb((u16)(base + NE_DATA), p[i]);
    wait_isr(0x40, 1000000);
    ne_out(NE_ISR, 0x40);
    ne_out(NE_CMD, 0x22);       /* start again */
}

static void dma_read(u16 addr, u8 *p, u16 len)
{
    dma_setup(addr, len);
    ne_out(NE_CMD, 0x0A);       /* remote read */
    for (u16 i = 0; i < len; i++)
        p[i] = inb((u16)(base + NE_DATA));
    ne_out(NE_ISR, 0x40);
    ne_out(NE_CMD, 0x22);
}

/* ---- card setup ---- */
static void hw_reset(void)
{
    ne_out(NE_CMD, 0x21);
    ne_out(NE_ISR, 0xFF);
    for (volatile int i = 0; i < 0x2000; i++)
        ;
}

static void read_mac(void)
{
    u8 prom[32];

    ne_out(NE_CMD, 0x21);
    ne_out(NE_DCR, 0x48);       /* byte transfers */
    ne_out(NE_ISR, 0xFF);
    ne_out(NE_RSAR0, 0);
    ne_out(NE_RSAR1, 0);
    ne_out(NE_RBCR0, 32);
    ne_out(NE_RBCR1, 0);
    ne_out(NE_CMD, 0x0A);
    for (int i = 0; i < 32; i++)
        prom[i] = inb((u16)(base + NE_DATA));
    ne_out(NE_CMD, 0x21);

    /* some cards (and QEMU) return each PROM byte twice */
    if (prom[0] == prom[1] && prom[2] == prom[3] && prom[4] == prom[5]) {
        for (int i = 0; i < 6; i++)
            mac[i] = prom[i * 2];
    } else {
        for (int i = 0; i < 6; i++)
            mac[i] = prom[i];
    }
}

static void chip_init(void)
{
    ne_out(NE_DCR, 0x48);
    ne_out(NE_RBCR0, 0);
    ne_out(NE_RBCR1, 0);
    ne_out(NE_RCR, 0x04);       /* accept broadcast */
    ne_out(NE_TCR, 0x00);
    ne_out(NE_TPSR, TX_PAGE);
    ne_out(NE_PSTART, RX_START);
    ne_out(NE_PSTOP, RX_STOP);
    ne_out(NE_BNRY, RX_START);
    ne_out(NE_IMR, 0x00);       /* polling only */

    ne_out(NE_CMD, 0x61);       /* stop, page 1 */
    for (int i = 0; i < 6; i++)
        ne_out((u8)(NE_PAR0 + i), mac[i]);
    ne_out(NE_CURR, RX_START + 1);
    for (int i = 0; i < 8; i++)
        ne_out((u8)(NE_MAR0 + i), 0xFF);

    ne_out(NE_CMD, 0x22);       /* start, page 0 */
}

int net_init(void)
{
    u32 devfn = 0;
    int found = 0;

    for (u32 dev = 0; dev < 32; dev++) {
        u32 a = 0x80000000u | (dev << 11);
        u32 id = pci_read(a);
        if ((id & 0xFFFF) == 0x10EC && (id >> 16) == 0x8029) {
            devfn = a;
            found = 1;
            break;
        }
    }
    if (!found)
        return -1;

    u32 bar = pci_read(devfn | 0x10);
    u16 io = (u16)(bar & 0xFFFC);
    if (!io) {
        pci_write(devfn | 0x10, 0x00000301);
        io = 0x0300;
    }
    base = io;

    u32 cmd = pci_read(devfn | 0x04);
    pci_write(devfn | 0x04, cmd | 0x0001);

    irq_line = (u8)(pci_read(devfn | 0x3C) & 0xFF);

    hw_reset();
    read_mac();
    chip_init();

    ready = 1;
    return 0;
}

int net_ready(void)
{
    return ready;
}

/* ---- output helpers ---- */
void net_ip_str(u32 ip, char *out)
{
    char num[12];
    int n = 0;

    for (int i = 3; i >= 0; i--) {
        utoa((ip >> (i * 8)) & 0xFF, num);
        for (int j = 0; num[j]; j++)
            out[n++] = num[j];
        if (i)
            out[n++] = '.';
    }
    out[n] = 0;
}

int net_parse_ip(const char *s, u32 *out)
{
    u32 ip = 0;

    for (int part = 0; part < 4; part++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            digits++;
            if (v > 255)
                return -1;
        }
        if (!digits)
            return -1;
        ip = (ip << 8) | (u32)v;
        if (part < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s && *s != ' ')
        return -1;
    *out = ip;
    return 0;
}

void net_info(void)
{
    char ip[20];

    if (!ready) {
        con_puts("No NE2000 PCI NIC found.\n");
        return;
    }

    con_puts("\nNetwork adapter\n");
    con_puts("  PCI:      10EC:8029\n");
    con_printf("  I/O base: 0x%x\n", (u32)base);
    con_printf("  IRQ:      %u\n", (u32)irq_line);
    con_printf("  MAC:      %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    net_ip_str(my_ip, ip);
    con_printf("  IP:       %s\n", ip);
    net_ip_str(gw_ip, ip);
    con_printf("  Gateway:  %s\n", ip);
    con_puts("  Mode:     polled PIO, no interrupts\n");
}

/* ---- frame transmit / receive ---- */
static int ne_send(const u8 *frame, u16 len)
{
    dma_write(TX_PAGE * 256, frame, len);

    ne_out(NE_TPSR, TX_PAGE);
    ne_out(NE_TBCR0, (u8)(len & 0xFF));
    ne_out(NE_TBCR1, (u8)(len >> 8));
    ne_out(NE_ISR, 0x02);
    ne_out(NE_CMD, 0x26);       /* nodma | transmit */

    if (wait_isr(0x02, 1000000) < 0)
        return -1;
    ne_out(NE_ISR, 0x02);
    return 0;
}

/* true when the receive ring holds unread frames (BNRY+1 != CURR) */
static int ne_has_frame(void)
{
    u8 bnry = ne_in(NE_BNRY);

    ne_out(NE_CMD, 0x62);       /* start, page 1 */
    u8 curr = ne_in(NE_CURR);
    ne_out(NE_CMD, 0x22);       /* start, page 0 */
    return (u8)(bnry + 1) != curr;
}

/* copy one frame from the ring; returns 1 if a frame was copied */
static int ne_poll(u16 *out_len)
{
    if (!ne_has_frame()) {
        ne_out(NE_ISR, 0x01);
        return 0;
    }
    ne_out(NE_ISR, 0x01);

    u8 page = (u8)(ne_in(NE_BNRY) + 1);
    if (page >= RX_STOP)
        page = RX_START;

    u8 hdr[4];
    dma_read((u16)(page * 256), hdr, 4);

    u16 len = (u16)(hdr[2] | (hdr[3] << 8));
    u16 next = hdr[1];
    if (next <= RX_START)
        next = RX_STOP;

    /* advance boundary no matter what */
    u8 bnry = (u8)(next - 1);
    if (bnry < RX_START)
        bnry = RX_STOP - 1;
    ne_out(NE_BNRY, bnry);

    if (len <= 4 || len > 1514)
        return 0;

    u16 plen = (u16)(len - 4);
    u16 addr = (u16)(page * 256 + 4);
    u16 ring_end = (u16)(RX_STOP * 256);

    if ((u32)addr + plen > ring_end) {
        u16 first = (u16)(ring_end - addr);
        dma_read(addr, rxbuf, first);
        dma_read((u16)(RX_START * 256), rxbuf + first, (u16)(plen - first));
    } else {
        dma_read(addr, rxbuf, plen);
    }

    *out_len = plen;
    return 1;
}

/* ---- checksum ---- */
static u16 csum16(const u8 *p, u32 len)
{
    u32 sum = 0;
    while (len > 1) {
        sum += (u32)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/* ---- frame builders ---- */
static void ip_bytes(u32 ip, u8 *out)
{
    out[0] = (u8)(ip >> 24);
    out[1] = (u8)(ip >> 16);
    out[2] = (u8)(ip >> 8);
    out[3] = (u8)ip;
}

static u16 rd16be(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

static u32 rd32be(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void wr16be(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void wr32be(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u16 tcp_csum(u32 src, u32 dst, const u8 *seg, u16 len)
{
    u32 sum = 0;
    u16 left = len;

    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;
    sum += 6;                   /* protocol TCP */
    sum += len;

    while (left > 1) {
        sum += (u32)((seg[0] << 8) | seg[1]);
        seg += 2;
        left -= 2;
    }
    if (left)
        sum += (u32)(seg[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

/* build and transmit one TCP segment (no options, 20-byte header) */
static int tcp_send(u8 flags, const u8 *data, u16 len)
{
    u16 tcp_len = (u16)(20 + len);
    u16 total = (u16)(20 + tcp_len);

    memset(txbuf, 0, (size_t)(14 + total));
    memcpy(txbuf, tcp_peer_mac, 6);
    memcpy(txbuf + 6, mac, 6);
    txbuf[12] = 0x08;
    txbuf[13] = 0x00;

    u8 *ip = txbuf + 14;
    ip[0] = 0x45;
    ip[2] = (u8)(total >> 8);
    ip[3] = (u8)total;
    ip[6] = 0x40;               /* don't fragment */
    ip[8] = 64;                 /* TTL */
    ip[9] = 6;                  /* TCP */
    ip_bytes(my_ip, ip + 12);
    ip_bytes(tcp_remote_ip, ip + 16);
    u16 c = csum16(ip, 20);
    ip[10] = (u8)(c >> 8);
    ip[11] = (u8)c;

    u8 *t = ip + 20;
    wr16be(t + 0, tcp_local_port);
    wr16be(t + 2, tcp_remote_port);
    wr32be(t + 4, tcp_snd_nxt);
    wr32be(t + 8, tcp_rcv_nxt);
    t[12] = 5 << 4;             /* data offset */
    t[13] = flags;
    wr16be(t + 14, TCP_WINDOW);
    if (len)
        memcpy(t + 20, data, len);

    u16 tc = tcp_csum(my_ip, tcp_remote_ip, t, tcp_len);
    t[16] = (u8)(tc >> 8);
    t[17] = (u8)tc;

    return ne_send(txbuf, (u16)(14 + total));
}

static void build_arp_request(u32 target, u16 *len)
{
    memset(txbuf, 0, 64);
    memset(txbuf, 0xFF, 6);
    memcpy(txbuf + 6, mac, 6);
    txbuf[12] = 0x08; txbuf[13] = 0x06;
    txbuf[14] = 0x00; txbuf[15] = 0x01;
    txbuf[16] = 0x08; txbuf[17] = 0x00;
    txbuf[18] = 6;    txbuf[19] = 4;
    txbuf[20] = 0x00; txbuf[21] = 0x01;   /* request */
    memcpy(txbuf + 22, mac, 6);
    ip_bytes(my_ip, txbuf + 28);
    ip_bytes(target, txbuf + 38);
    *len = 60;
}

static void build_arp_reply(const u8 *req)
{
    memset(txbuf, 0, 64);
    memcpy(txbuf, req + 22, 6);            /* requester MAC */
    memcpy(txbuf + 6, mac, 6);
    txbuf[12] = 0x08; txbuf[13] = 0x06;
    txbuf[14] = 0x00; txbuf[15] = 0x01;
    txbuf[16] = 0x08; txbuf[17] = 0x00;
    txbuf[18] = 6;    txbuf[19] = 4;
    txbuf[20] = 0x00; txbuf[21] = 0x02;   /* reply */
    memcpy(txbuf + 22, mac, 6);
    ip_bytes(my_ip, txbuf + 28);
    memcpy(txbuf + 32, req + 22, 6);
    memcpy(txbuf + 38, req + 28, 4);
}

static void build_icmp(u32 dst, u16 seq, u16 *len)
{
    memset(txbuf, 0, 80);
    memcpy(txbuf, arp_mac, 6);
    memcpy(txbuf + 6, mac, 6);
    txbuf[12] = 0x08; txbuf[13] = 0x00;

    u8 *ip = txbuf + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    ip[2] = 0; ip[3] = 60;             /* total length */
    ip[4] = (u8)(seq >> 8); ip[5] = (u8)seq;
    ip[6] = 0; ip[7] = 0;
    ip[8] = 64;                        /* ttl */
    ip[9] = 1;                         /* ICMP */
    ip[10] = 0; ip[11] = 0;            /* checksum */
    ip_bytes(my_ip, ip + 12);
    ip_bytes(dst, ip + 16);

    u8 *icmp = txbuf + 34;
    icmp[0] = 8;                       /* echo request */
    icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0;
    icmp[4] = 0x52; icmp[5] = 0x53;    /* id 'RS' */
    icmp[6] = (u8)(seq >> 8); icmp[7] = (u8)seq;
    for (int i = 8; i < 40; i++)
        icmp[i] = 0x41;

    u16 c = csum16(ip, 20);
    ip[10] = (u8)(c >> 8);
    ip[11] = (u8)c;

    c = csum16(icmp, 40);
    icmp[2] = (u8)(c >> 8);
    icmp[3] = (u8)c;

    *len = 74;
}

static void tcp_input(const u8 *ip, u16 ip_len);

/* ---- incoming frames ---- */
static void handle_frame(u16 len)
{
    if (len < 14)
        return;

    if (rxbuf[12] == 0x08 && rxbuf[13] == 0x06) {
        if (len < 42)
            return;
        u16 oper = (u16)((rxbuf[20] << 8) | rxbuf[21]);
        if (oper == 1) {
            /* request for us? */
            u32 tpa = ((u32)rxbuf[38] << 24) | ((u32)rxbuf[39] << 16) |
                      ((u32)rxbuf[40] << 8) | rxbuf[41];
            if (tpa == my_ip) {
                build_arp_reply(rxbuf);
                ne_send(txbuf, 60);
            }
        } else if (oper == 2) {
            u32 spa = ((u32)rxbuf[28] << 24) | ((u32)rxbuf[29] << 16) |
                      ((u32)rxbuf[30] << 8) | rxbuf[31];
            if (spa == arp_want) {
                memcpy(arp_mac, rxbuf + 22, 6);
                arp_ip = spa;
                arp_ok = 1;
            }
        }
        return;
    }

    if (rxbuf[12] != 0x08 || rxbuf[13] != 0x00)
        return;
    if (len < 34)
        return;

    const u8 *ip = rxbuf + 14;
    u16 ip_len = (u16)(((u16)ip[2] << 8) | ip[3]);
    if (ip_len > (u16)(len - 14))
        ip_len = (u16)(len - 14);

    if (ip[9] == 6) {                   /* TCP */
        tcp_input(ip, ip_len);
        return;
    }
    if (ip[9] != 1 || ip_len < 28)      /* ICMP */
        return;

    u32 dst = ((u32)ip[16] << 24) | ((u32)ip[17] << 16) |
              ((u32)ip[18] << 8) | ip[19];
    if (dst != my_ip)
        return;
    if (ip[20] != 0)                    /* echo reply */
        return;
    if (ip[24] != 0x52 || ip[25] != 0x53)
        return;

    ping_ttl = ip[8];
    ping_got = 1;
}

static void pump_ms(u32 ms)
{
    u32 start = timer_ticks();
    u32 want = ms / 10;
    if (!want)
        want = 1;
    while (timer_ticks() - start < want) {
        u16 len;
        if (ne_poll(&len))
            handle_frame(len);
    }
}

static int pump_once(void)
{
    int got = 0;
    u16 len;

    /* stop while there is still room to store the next burst */
    while (tcp_rx_len + 4096 < TCP_RXBUF_SIZE && ne_poll(&len)) {
        handle_frame(len);
        got = 1;
    }
    return got;
}

/* ---- ARP / TCP / HTTP ---- */
static int arp_resolve(u32 ip)
{
    arp_want = ip;
    arp_ok = 0;

    for (int attempt = 0; attempt < 3 && !arp_ok; attempt++) {
        u16 len;
        build_arp_request(ip, &len);
        ne_send(txbuf, len);
        for (int slice = 0; slice < 10 && !arp_ok; slice++)
            pump_ms(100);
    }
    return arp_ok ? 0 : -1;
}

static void tcp_input(const u8 *ip, u16 ip_len)
{
    if (tcp_state == 0)
        return;

    u32 src = rd32be(ip + 12);
    u32 dst = rd32be(ip + 16);
    if (src != tcp_remote_ip || dst != my_ip)
        return;

    u16 ihl = (u16)((ip[0] & 0x0F) * 4);
    if (ip_len < ihl + 20)
        return;

    const u8 *t = ip + ihl;
    u16 sport = rd16be(t + 0);
    u16 dport = rd16be(t + 2);
    if (sport != tcp_remote_port || dport != tcp_local_port)
        return;

    u32 seq = rd32be(t + 4);
    u32 ack = rd32be(t + 8);
    u8 flags = t[13];
    u16 doff = (u16)((t[12] >> 4) * 4);
    if (doff < 20 || ip_len < ihl + doff)
        return;
    u16 seg_len = (u16)(ip_len - ihl - doff);
    const u8 *data = t + doff;

    if (flags & TCP_RST) {
        tcp_got_reset = 1;
        tcp_state = 0;
        return;
    }

    if (flags & TCP_SYN) {
        if (tcp_state == 1 && (flags & TCP_ACK) && ack == tcp_snd_nxt + 1) {
            tcp_snd_nxt = ack;
            tcp_rcv_nxt = seq + 1;
            tcp_state = 2;
            tcp_send(TCP_ACK, NULL, 0);
        }
        return;
    }

    if (tcp_state != 2)
        return;

    if (seg_len) {
        if (seq == tcp_rcv_nxt) {
            if (TCP_RXBUF_SIZE - tcp_rx_len < seg_len)
                return;                 /* no room: drop, the sender retries */
            memcpy(tcp_rxbuf + tcp_rx_len, data, seg_len);
            tcp_rx_len += seg_len;
            tcp_rcv_nxt += seg_len;
            tcp_send(TCP_ACK, NULL, 0);
        } else if ((s32)(seq - tcp_rcv_nxt) < 0) {
            tcp_send(TCP_ACK, NULL, 0);     /* retransmit: re-ACK */
        }
    }

    if (flags & TCP_FIN) {
        if (seq + seg_len == tcp_rcv_nxt) {
            tcp_rcv_nxt++;
            tcp_got_fin = 1;
            tcp_send(TCP_ACK, NULL, 0);
        }
    }
    (void)ack;
}

static int tcp_connect(void)
{
    if (tcp_send(TCP_SYN, NULL, 0) < 0) {
        tcp_state = 0;
        return -1;
    }

    u32 start = timer_ticks();
    while (tcp_state == 1 && timer_ticks() - start < 300)
        pump_ms(5);

    return tcp_state == 2 ? 0 : -1;
}

static int find_header_end(u32 *end)
{
    for (u32 i = 0; i + 3 < tcp_rx_len; i++) {
        if (tcp_rxbuf[i] == '\r' && tcp_rxbuf[i + 1] == '\n' &&
            tcp_rxbuf[i + 2] == '\r' && tcp_rxbuf[i + 3] == '\n') {
            *end = i + 4;
            return 0;
        }
    }
    return -1;
}

static int ci_match(const u8 *p, const char *s)
{
    while (*s) {
        char a = (char)*p++;
        char b = *s++;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

static int http_status(void)
{
    if (tcp_rx_len < 12 || !ci_match(tcp_rxbuf, "http/"))
        return -1;

    u32 i = 0;
    while (i < tcp_rx_len && tcp_rxbuf[i] != ' ')
        i++;
    i++;
    if (i + 2 >= tcp_rx_len)
        return -1;
    if (tcp_rxbuf[i] < '0' || tcp_rxbuf[i] > '9')
        return -1;
    return (tcp_rxbuf[i] - '0') * 100 + (tcp_rxbuf[i + 1] - '0') * 10 +
           (tcp_rxbuf[i + 2] - '0');
}

/* 0xFFFFFFFF when the header is absent */
static u32 http_content_length(u32 end)
{
    u32 i = 0;

    while (i < end) {
        u32 line = i;
        while (i < end && tcp_rxbuf[i] != '\n')
            i++;
        u32 e = i;
        if (e > line && tcp_rxbuf[e - 1] == '\r')
            e--;
        if (e > line && ci_match(tcp_rxbuf + line, "content-length:")) {
            u32 p = line + 15;
            u32 v = 0;
            while (p < e && tcp_rxbuf[p] == ' ')
                p++;
            while (p < e && tcp_rxbuf[p] >= '0' && tcp_rxbuf[p] <= '9')
                v = v * 10 + (u32)(tcp_rxbuf[p++] - '0');
            return v;
        }
        i++;
    }
    return 0xFFFFFFFFu;
}

static void str_add(char *b, int *n, const char *s)
{
    while (*s)
        b[(*n)++] = *s++;
}

int net_http_get_stream(u32 ip, u16 port, const char *path,
                        net_sink_fn sink, void *ctx, u32 *out_len)
{
    char ipstr[20], portstr[12];
    char req[320];
    int n = 0;

    if (!ready)
        return -1;

    u32 hop = ((ip ^ my_ip) & 0xFFFFFF00u) ? gw_ip : ip;
    if (arp_resolve(hop) < 0)
        return -1;

    tcp_remote_ip = ip;
    tcp_remote_port = port;
    memcpy(tcp_peer_mac, arp_mac, 6);
    tcp_local_port = (u16)(40000 + (timer_ticks() & 0x3FFF));
    tcp_snd_nxt = (timer_ticks() * 0x9E3779B1u) ^ 0x5A5A1234u;
    tcp_rcv_nxt = 0;
    tcp_rx_len = 0;
    tcp_got_reset = 0;
    tcp_got_fin = 0;
    tcp_state = 1;

    if (tcp_connect() < 0)
        return -3;

    net_ip_str(ip, ipstr);
    utoa(port, portstr);
    str_add(req, &n, "GET ");
    str_add(req, &n, path);
    str_add(req, &n, " HTTP/1.1\r\nHost: ");
    str_add(req, &n, ipstr);
    str_add(req, &n, ":");
    str_add(req, &n, portstr);
    str_add(req, &n, "\r\nConnection: close\r\nUser-Agent: ros-update\r\n\r\n");

    if (tcp_send(TCP_PSH | TCP_ACK, (const u8 *)req, (u16)n) < 0) {
        tcp_state = 0;
        return -3;
    }
    tcp_snd_nxt += (u32)n;

    int hdr = -1;
    u32 body = 0, clen = 0, total = 0;
    int have_cl = 0, rc = -4;
    u32 last = timer_ticks();

    for (;;) {
        if (tcp_got_reset) {
            rc = -4;
            break;
        }

        if (hdr < 0) {
            u32 end;
            if (find_header_end(&end) == 0) {
                if (http_status() != 200) {
                    rc = -2;
                    break;
                }
                hdr = (int)end;
                body = end;
                clen = http_content_length(end);
                have_cl = (clen != 0xFFFFFFFFu);
                if (have_cl && clen == 0) {
                    rc = 0;
                    break;
                }
            }
        }

        if (hdr >= 0 && body < tcp_rx_len) {
            u32 chunk = tcp_rx_len - body;
            if (sink(ctx, tcp_rxbuf + body, chunk) < 0) {
                rc = -6;
                break;
            }
            total += chunk;
            tcp_rx_len = 0;         /* header is no longer needed */
            body = 0;
            last = timer_ticks();
            if (have_cl && total >= clen) {
                rc = (total == clen) ? 0 : -6;
                break;
            }
        }

        if (tcp_got_fin) {
            rc = (hdr >= 0 && !have_cl) ? 0 : -4;
            break;
        }
        if (timer_ticks() - last > 2000) {      /* 20 s without progress */
            rc = -4;
            break;
        }
        if (pump_once())
            last = timer_ticks();
    }

    if (rc == 0 && have_cl && total != clen)
        rc = -6;
    if (out_len)
        *out_len = total;

    if (tcp_state == 2) {
        tcp_send(TCP_FIN | TCP_ACK, NULL, 0);
        tcp_snd_nxt++;
    }
    tcp_state = 0;
    return rc;
}

struct mem_sink {
    u8 *out;
    u32 max;
    u32 len;
    int over;
};

static int mem_sink_fn(void *ctx, const u8 *data, u32 len)
{
    struct mem_sink *m = ctx;

    if (m->len + len > m->max) {
        m->over = 1;
        return -1;
    }
    if (len)
        memcpy(m->out + m->len, data, len);
    m->len += len;
    return 0;
}

int net_http_get(u32 ip, u16 port, const char *path, u8 *out, u32 max,
                 u32 *out_len)
{
    struct mem_sink m;
    u32 total = 0;
    int rc;

    m.out = out;
    m.max = max;
    m.len = 0;
    m.over = 0;

    rc = net_http_get_stream(ip, port, path, mem_sink_fn, &m, &total);
    if (out_len)
        *out_len = m.len;
    if (rc == -6 && m.over)
        return -5;
    return rc;
}

/* ---- commands ---- */
int net_ping(u32 ip)
{
    char ipstr[20];
    int replies = 0;

    if (!ready)
        return -1;

    arp_ok = 0;

    net_ip_str(ip, ipstr);
    con_printf("PING %s 32 data bytes\n", ipstr);

    for (int seq = 0; seq < 4; seq++) {
        /* resolve */
        if (!arp_ok) {
            if (arp_resolve(ip) < 0) {
                con_puts("ARP failed: destination host unreachable.\n");
                return -2;
            }
        }

        u16 len;
        build_icmp(ip, (u16)seq, &len);
        ping_got = 0;
        ne_send(txbuf, len);

        for (int slice = 0; slice < 100 && !ping_got; slice++)
            pump_ms(10);

        if (ping_got) {
            replies++;
            con_printf("Reply from %s: seq=%d bytes=32 ttl=%u\n",
                       ipstr, seq, (u32)ping_ttl);
        } else {
            con_puts("Request timed out.\n");
        }
    }

    con_printf("\n4 packets transmitted, %d received\n", replies);
    return replies;
}
