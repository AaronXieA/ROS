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
static int arp_ok;

static u32 ping_ip;
static int ping_got;
static u8 ping_ttl;

static u8 rxbuf[1600];
static u8 txbuf[128];

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

/* copy one frame from the ring; returns 1 if a frame was copied */
static int ne_poll(u16 *out_len)
{
    if (!(ne_in(NE_ISR) & 0x01))
        return 0;
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
            if (spa == ping_ip) {
                memcpy(arp_mac, rxbuf + 22, 6);
                arp_ip = spa;
                arp_ok = 1;
            }
        }
        return;
    }

    if (rxbuf[12] != 0x08 || rxbuf[13] != 0x00)
        return;
    if (len < 42)
        return;
    if (rxbuf[23] != 1)                 /* ICMP */
        return;

    u32 dst = ((u32)rxbuf[30] << 24) | ((u32)rxbuf[31] << 16) |
              ((u32)rxbuf[32] << 8) | rxbuf[33];
    if (dst != my_ip)
        return;
    if (rxbuf[34] != 0)                 /* echo reply */
        return;
    if (rxbuf[38] != 0x52 || rxbuf[39] != 0x53)
        return;

    ping_ttl = rxbuf[22];
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

/* ---- commands ---- */
int net_ping(u32 ip)
{
    char ipstr[20];
    int replies = 0;

    if (!ready)
        return -1;

    ping_ip = ip;
    arp_ok = 0;

    net_ip_str(ip, ipstr);
    con_printf("PING %s 32 data bytes\n", ipstr);

    for (int seq = 0; seq < 4; seq++) {
        /* resolve */
        if (!arp_ok) {
            for (int attempt = 0; attempt < 3 && !arp_ok; attempt++) {
                u16 len;
                build_arp_request(ip, &len);
                ne_send(txbuf, len);
                for (int slice = 0; slice < 10 && !arp_ok; slice++)
                    pump_ms(100);
            }
            if (!arp_ok) {
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
