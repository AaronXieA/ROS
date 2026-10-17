/* ROS Command - 32-bit C kernel */
#include "types.h"
#include "io.h"
#include "console.h"
#include "string.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "ata.h"
#include "fat.h"
#include "rtc.h"
#include "pck.h"
#include "net.h"

#define VERSION "0.10.0"
#define LINE_MAX 128

struct e820_entry {
    u64 base;
    u64 len;
    u32 type;
    u32 acpi;
};

extern struct e820_entry boot_e820[];
extern u32 boot_e820_count;
extern u32 boot_conv_kb;

static char cmdline[LINE_MAX];
static int aliases_on = 1;
static int fs_ok;

/* ---- line input ---- */
static void read_line(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = kbd_waitchar();
        if (c == '\n') {
            con_putc('\n');
            buf[n] = 0;
            return;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                con_putc('\b');
            }
            continue;
        }
        if (c == 27) {          /* ESC: drop the line */
            while (n > 0) {
                n--;
                con_putc('\b');
            }
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        if (n < max - 1) {
            buf[n++] = (char)c;
            con_putc((char)c);
        }
    }
}

/* ---- helpers ---- */
static u32 total_ram_mb(void)
{
    u64 bytes = 0;
    for (u32 i = 0; i < boot_e820_count; i++) {
        if (boot_e820[i].type == 1)
            bytes += boot_e820[i].len;
    }
    return (u32)(bytes >> 20);
}

static void pad_right(const char *s, int w)
{
    int n = (int)strlen(s);
    con_puts(s);
    while (n++ < w)
        con_putc(' ');
}

static void pad_left_num(u32 v, int w)
{
    char b[12];
    utoa(v, b);
    int n = (int)strlen(b);
    while (n++ < w)
        con_putc(' ');
    con_puts(b);
}

static void split_name(const char *name, char *base, char *ext)
{
    int i = 0, j = 0;
    while (name[i] && name[i] != '.')
        base[j++] = name[i++];
    base[j] = 0;
    ext[0] = 0;
    if (name[i] == '.') {
        i++;
        j = 0;
        while (name[i])
            ext[j++] = name[i++];
        ext[j] = 0;
    }
}

/* ---- commands ---- */
static void cmd_help(void)
{
    con_puts(
        "  help    - show this help\n"
        "  about   - about ROS Command\n"
        "  ver     - show version\n"
        "  clear   - clear screen (alias: cls)\n"
        "  echo    - print text, e.g. echo hello\n"
        "  dir     - list files (alias: ls)\n"
        "  doc     - show file contents (aliases: cat, type)\n"
        "  mkdir   - create a directory\n"
        "  touch   - create an empty file\n"
        "  goto    - change directory (alias: cd)\n"
        "  where   - show current path (alias: pwd)\n"
        "  who     - show current user (alias: whoami)\n"
        "  alias   - alias -a [true|false]\n"
        "  time    - show/set time, e.g. time 12:30:00\n"
        "  date    - show/set date, e.g. date 2026-10-17\n"
        "  mem     - show memory information\n"
        "  pck     - packages: pck list / install / remove\n"
        "  net     - show NE2000 network adapter\n"
        "  ping    - ICMP echo, e.g. ping 10.0.2.2\n"
        "  crash   - trigger a fatal exception (test)\n"
        "  reboot  - reboot the system\n"
        "  halt    - halt the system\n");
}

static void cmd_about(void)
{
    con_puts("ROS Command - the command-line edition of ROS.\n");
    con_puts("A tiny 32-bit kernel written in C.\n");
}

static void cmd_mem(void)
{
    con_printf("%u KB conventional memory\n", boot_conv_kb);
    con_printf("%u MB total memory\n", total_ram_mb());
    con_printf("E820 entries: %u\n", boot_e820_count);
}

static int dir_cb(const struct fat_dirent *de, void *ctx)
{
    int *count = ctx;
    char base[13], ext[5];
    split_name(de->name, base, ext);

    pad_right(base, 8);
    con_putc(' ');
    pad_right(ext, 3);
    con_putc(' ');
    if (de->attr & 0x10) {
        con_puts("<DIR>\n");
    } else {
        pad_left_num(de->size, 10);
        con_putc('\n');
    }
    (*count)++;
    return 0;
}

static void cmd_dir(void)
{
    if (!fs_ok) {
        con_puts("No filesystem.\n");
        return;
    }
    int count = 0;
    if (fs_list(dir_cb, &count) < 0) {
        con_puts("Cannot read directory.\n");
        return;
    }
    con_printf("%d file(s)\n", count);
}

static void cmd_doc(const char *name)
{
    struct fat_dirent de;

    if (!name) {
        con_puts("Usage: doc <filename>\n");
        return;
    }
    if (!fs_ok || fs_find(name, &de) < 0) {
        con_puts("File not found.\n");
        return;
    }
    if (de.attr & 0x10) {
        con_puts("Is a directory.\n");
        return;
    }
    fs_print_file(&de);
    con_putc('\n');
}

static void cmd_touch(const char *name)
{
    if (!name) {
        con_puts("Usage: touch <name>\n");
        return;
    }
    int rc = fs_create(name, NULL, 0);
    if (rc == 0)
        con_puts("File created.\n");
    else if (rc == -2)
        con_puts("Already exists.\n");
    else if (rc == -3)
        con_puts("Directory is full.\n");
    else if (rc == -4)
        con_puts("Disk is full.\n");
    else
        con_puts("Cannot create file.\n");
}

static void cmd_mkdir(const char *name)
{
    if (!name) {
        con_puts("Usage: mkdir <name>\n");
        return;
    }
    int rc = fs_mkdir(name);
    if (rc == 0)
        con_puts("Directory created.\n");
    else if (rc == -2)
        con_puts("Already exists.\n");
    else if (rc == -3)
        con_puts("Directory is full.\n");
    else if (rc == -4)
        con_puts("Disk is full.\n");
    else
        con_puts("Cannot create directory.\n");
}

static void cmd_goto(const char *name)
{
    if (!name) {
        con_puts("Usage: goto <dir>\n");
        return;
    }
    int rc = fs_cd(name);
    if (rc == 0)
        return;
    if (rc == -2)
        con_puts("Not a directory.\n");
    else
        con_puts("Directory not found.\n");
}

/* ---- time / date ---- */
static int two_digits(const char **p, int *out)
{
    if ((*p)[0] < '0' || (*p)[0] > '9' || (*p)[1] < '0' || (*p)[1] > '9')
        return -1;
    *out = ((*p)[0] - '0') * 10 + ((*p)[1] - '0');
    *p += 2;
    return 0;
}

static void cmd_time(const char *arg)
{
    if (!arg) {
        u8 h, m, s, d, mo, y, c;
        rtc_read(&h, &m, &s, &d, &mo, &y, &c);
        con_printf("%02u:%02u:%02u\n", h, m, s);
        return;
    }

    int h, m, s;
    const char *p = arg;
    if (two_digits(&p, &h) < 0 || *p++ != ':' ||
        two_digits(&p, &m) < 0 || *p++ != ':' ||
        two_digits(&p, &s) < 0 || *p != 0) {
        con_puts("Usage: time [HH:MM:SS]\n");
        return;
    }
    if (rtc_set_time((u8)h, (u8)m, (u8)s) < 0) {
        con_puts("Usage: time [HH:MM:SS]\n");
        return;
    }
    con_puts("Time set.\n");
}

static void cmd_date(const char *arg)
{
    if (!arg) {
        u8 h, m, s, d, mo, y, c;
        rtc_read(&h, &m, &s, &d, &mo, &y, &c);
        con_printf("%02u-%02u-%02u\n", (u32)(2000 + y), mo, d);
        return;
    }

    int y1, y2, mo, d;
    const char *p = arg;
    if (two_digits(&p, &y1) < 0 || two_digits(&p, &y2) < 0 || *p++ != '-' ||
        two_digits(&p, &mo) < 0 || *p++ != '-' ||
        two_digits(&p, &d) < 0 || *p != 0) {
        con_puts("Usage: date [YYYY-MM-DD]\n");
        return;
    }
    int full = y1 * 100 + y2;
    if (full < 2000 || full > 2099 ||
        rtc_set_date((u8)(full - 2000), (u8)mo, (u8)d, 20) < 0) {
        con_puts("Usage: date [YYYY-MM-DD]\n");
        return;
    }
    con_puts("Date set.\n");
}

static void cmd_ping(const char *arg)
{
    u32 ip;

    if (!arg) {
        con_puts("Usage: ping <a.b.c.d>\n");
        return;
    }
    if (!net_ready()) {
        con_puts("No NE2000 PCI NIC found.\n");
        return;
    }
    if (net_parse_ip(arg, &ip) < 0) {
        con_puts("Usage: ping <a.b.c.d>\n");
        return;
    }
    net_ping(ip);
}

static void cmd_pck(const char *arg)
{
    if (!arg) {
        con_puts("Usage: pck list | pck install <name> | pck remove <name>\n");
        return;
    }

    char sub[16];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < 15) {
        sub[i] = arg[i];
        i++;
    }
    sub[i] = 0;
    const char *rest = arg + i;
    while (*rest == ' ')
        rest++;
    if (!*rest)
        rest = NULL;

    if (!strcmp(sub, "list")) {
        pck_list();
        return;
    }
    if (!strcmp(sub, "install")) {
        if (!rest) {
            con_puts("Usage: pck install <name>\n");
            return;
        }
        int rc = pck_install(rest);
        if (rc == 0)
            con_puts("Package installed.\n");
        else if (rc == -1)
            con_puts("File not found.\n");
        else if (rc == -4)
            con_puts("Package too large.\n");
        else if (rc == -5)
            con_puts("Invalid package file.\n");
        else
            con_puts("Cannot extract package.\n");
        return;
    }
    if (!strcmp(sub, "remove")) {
        if (!rest) {
            con_puts("Usage: pck remove <name>\n");
            return;
        }
        int rc = pck_remove(rest);
        if (rc == 0)
            con_puts("Package removed.\n");
        else if (rc == -1)
            con_puts("File not found.\n");
        else if (rc == -5)
            con_puts("Invalid package file.\n");
        else
            con_puts("Cannot remove package.\n");
        return;
    }
    con_puts("Usage: pck list | pck install <name> | pck remove <name>\n");
}

static void cmd_alias(const char *arg)
{
    if (!arg || !*arg) {
        con_printf("System aliases: %s\n", aliases_on ? "on" : "off");
        return;
    }
    if (!strcasecmp(arg, "-a")) {
        con_puts("System aliases: ");
        con_puts(aliases_on ? "on\n" : "off\n");
        return;
    }
    if (!strcasecmp(arg, "-a true")) {
        aliases_on = 1;
        con_puts("System aliases: on\n");
        return;
    }
    if (!strcasecmp(arg, "-a false")) {
        aliases_on = 0;
        con_puts("System aliases: off\n");
        return;
    }
    con_puts("Usage: alias -a [true|false]\n");
}

static volatile int crash_zero;

static void cmd_crash(void)
{
    int a = 1, b = crash_zero;
    __asm__ volatile("xorl %%edx, %%edx; idivl %1" : "+a"(a) : "rm"(b));
    (void)a;
}

static void cmd_reboot(void)
{
    con_puts("Rebooting...\n");
    outb(0x64, 0xFE);
    for (;;)
        cpu_halt();
}

static void cmd_halt(void)
{
    con_puts("System halted. It is now safe to power off.\n");
    for (;;)
        cpu_halt();
}

static void dispatch(char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;

    char *arg = strchr(line, ' ');
    if (arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
        if (!*arg)
            arg = NULL;
    }

    if (!strcmp(line, "help")) {
        cmd_help();
    } else if (!strcmp(line, "about")) {
        cmd_about();
    } else if (!strcmp(line, "ver")) {
        con_printf("ROS Command version %s (32-bit)\n", VERSION);
    } else if (!strcmp(line, "clear") ||
               (!strcmp(line, "cls") && aliases_on)) {
        con_clear();
    } else if (!strcmp(line, "echo")) {
        if (arg)
            con_puts(arg);
        con_putc('\n');
    } else if (!strcmp(line, "dir") || (!strcmp(line, "ls") && aliases_on)) {
        cmd_dir();
    } else if (!strcmp(line, "doc") || (!strcmp(line, "cat") && aliases_on) ||
               (!strcmp(line, "type") && aliases_on)) {
        cmd_doc(arg);
    } else if (!strcmp(line, "touch")) {
        cmd_touch(arg);
    } else if (!strcmp(line, "mkdir")) {
        cmd_mkdir(arg);
    } else if (!strcmp(line, "goto") || (!strcmp(line, "cd") && aliases_on)) {
        cmd_goto(arg);
    } else if (!strcmp(line, "where") ||
               (!strcmp(line, "pwd") && aliases_on)) {
        con_printf("%s\n", fs_cwd());
    } else if (!strcmp(line, "time")) {
        cmd_time(arg);
    } else if (!strcmp(line, "date")) {
        cmd_date(arg);
    } else if (!strcmp(line, "mem")) {
        cmd_mem();
    } else if (!strcmp(line, "who") ||
               (!strcmp(line, "whoami") && aliases_on)) {
        con_puts("root\n");
    } else if (!strcmp(line, "alias")) {
        cmd_alias(arg);
    } else if (!strcmp(line, "pck")) {
        cmd_pck(arg);
    } else if (!strcmp(line, "net")) {
        net_info();
    } else if (!strcmp(line, "ping")) {
        cmd_ping(arg);
    } else if (!strcmp(line, "crash")) {
        cmd_crash();
    } else if (!strcmp(line, "reboot")) {
        cmd_reboot();
    } else if (!strcmp(line, "halt")) {
        cmd_halt();
    } else if (!strcmp(line, "cls") || !strcmp(line, "ls") ||
               !strcmp(line, "cat") || !strcmp(line, "type") ||
               !strcmp(line, "cd") || !strcmp(line, "pwd") ||
               !strcmp(line, "whoami")) {
        con_puts("Unknown command. Type 'help'. This command is not a standard "
                 "ROS command. Use 'alias -a true' to enable system alias.\n");
    } else {
        con_puts("Unknown command. Type 'help'.\n");
    }
}

/* ---- main ---- */
void kmain(void)
{
    con_init();
    pic_remap();
    idt_init();
    timer_init();
    keyboard_init();
    irq_unmask(0);
    irq_unmask(1);
    __asm__ volatile("sti");

    con_puts("============================================\n");
    con_printf(" ROS Command v%s\n", VERSION);
    con_puts(" Lightweight 32-bit kernel by XR Stu - xrst.uk\n");
    con_puts(" Shell: xsh (X Shell)\n");
    con_puts("============================================\n");
    con_puts("Type 'help' to list commands.\n\n");

    fs_ok = (ata_init() == 0 && fs_init() == 0);
    if (!fs_ok)
        con_puts("Warning: no FAT16 filesystem found.\n\n");

    net_init();

    for (;;) {
        con_puts("xsh:ROS");
        con_puts(" ");
        con_puts(fs_cwd());
        con_puts("> ");
        read_line(cmdline, LINE_MAX);
        dispatch(cmdline);
    }
}
