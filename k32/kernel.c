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
#include "paging.h"
#include "sched.h"

#define VERSION "0.12.0pre"
#define LINE_MAX 128

#define USER_MAX      16
#define USER_NAME_MAX 12
#define USER_REC      32
#define USER_FILE     "USER.DAT"

struct user {
    char name[16];
    u32  hash;
    u8   admin;
};

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

static struct user users[USER_MAX];
static int user_count;
static int cur_user;

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

static void read_secret(char *buf, int max)
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
                con_puts("\b \b");
            }
            continue;
        }
        if (c == 27) {
            while (n > 0) {
                n--;
                con_puts("\b \b");
            }
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        if (n < max - 1) {
            buf[n++] = (char)c;
            con_putc('*');
        }
    }
}

/* ---- users ---- */
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 hash_password(const char *s)
{
    u32 h = 5381;
    if (!*s)
        return 0;
    while (*s)
        h = h * 33 + (u8)*s++;
    return h;
}

static int user_find(const char *name)
{
    for (int i = 0; i < user_count; i++) {
        if (!strcasecmp(users[i].name, name))
            return i;
    }
    return -1;
}

static void user_defaults(void)
{
    memset(users, 0, sizeof(users));
    user_count = 0;
    cur_user = 0;

    strcpy(users[0].name, "root");
    users[0].hash = 0;
    users[0].admin = 1;
    user_count = 1;
}

static void user_load(void)
{
    static u8 buf[USER_MAX * USER_REC];
    struct fat_dirent de;

    user_defaults();
    if (!fs_ok)
        return;
    if (fs_find_at(0, USER_FILE, &de) < 0 || (de.attr & 0x10))
        return;

    int n = fs_read_file(&de, buf, sizeof(buf));
    for (int off = 0; off + USER_REC <= n; off += USER_REC) {
        u8 *r = buf + off;
        char name[16];

        if (!r[0])
            continue;
        strncpy(name, (const char *)r, 15);
        name[15] = 0;

        if (!strcasecmp(name, "root")) {
            users[0].hash = rd32(r + 16);
            continue;
        }
        if (user_find(name) >= 0 || user_count >= USER_MAX)
            continue;

        strcpy(users[user_count].name, name);
        users[user_count].hash = rd32(r + 16);
        users[user_count].admin = r[20] ? 1 : 0;
        user_count++;
    }
}

static int user_save(void)
{
    static u8 buf[USER_MAX * USER_REC];

    if (!fs_ok)
        return -1;

    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < user_count; i++) {
        u8 *r = buf + i * USER_REC;
        strncpy((char *)r, users[i].name, 15);
        wr32(r + 16, users[i].hash);
        r[20] = users[i].admin;
    }

    fs_delete_at(0, USER_FILE);
    return fs_create_at(0, USER_FILE, buf, (u32)user_count * USER_REC);
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
static const char *const help_lines[] = {
    "  help    - show this help",
    "  about   - about ROS Command",
    "  ver     - show version",
    "  clear   - clear screen (alias: cls)",
    "  echo    - print text, e.g. echo hello",
    "  dir     - list files (alias: ls)",
    "  doc     - show file contents (aliases: cat, type)",
    "  mkdir   - create a directory",
    "  touch   - create an empty file",
    "  goto    - change directory (alias: cd)",
    "  where   - show current path (alias: pwd)",
    "  who     - show current user (alias: whoami)",
    "  cru     - create a user, e.g. cru alice",
    "  cu      - switch user, e.g. cu alice",
    "  passwd  - set password, e.g. passwd alice",
    "  alias   - alias -a [true|false]",
    "  time    - show/set time, e.g. time 12:30:00",
    "  date    - show/set date, e.g. date 2026-10-17",
    "  mem     - show memory information",
    "  pck     - packages: pck list / install / remove",
    "  net     - show NE2000 network adapter",
    "  ping    - ICMP echo, e.g. ping 10.0.2.2",
    "  run     - run a user program, e.g. run user.bin",
    "  ps      - list processes",
    "  kill    - kill a process, e.g. kill 2",
    "  vm      - show paging / virtual memory info",
    "  crash   - trigger a fatal exception (test)",
    "  reboot  - reboot the system",
    "  halt    - halt the system",
    NULL,
};

static void cmd_help(void)
{
    const int per_page = 20;
    int shown = 0;

    con_clear();
    for (int i = 0; help_lines[i]; i++) {
        if (shown == per_page) {
            con_puts("-- more -- (any key: next, q: quit)");
            int c = kbd_waitchar();
            con_clear();
            if (c == 27 || c == 'q' || c == 'Q')
                return;
            shown = 0;
        }
        con_puts(help_lines[i]);
        con_putc('\n');
        shown++;
    }
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

/* ---- users ---- */
static void cmd_cru(const char *name)
{
    char pw[32], pw2[32], ans[8];

    if (!name) {
        con_puts("Usage: cru <username>\n");
        return;
    }
    if (!fs_ok) {
        con_puts("No filesystem.\n");
        return;
    }
    if (!users[cur_user].admin) {
        con_puts("Permission denied. Only an admin can create users.\n");
        return;
    }
    if (strlen(name) > USER_NAME_MAX) {
        con_puts("Username too long (max 12 characters).\n");
        return;
    }
    if (user_find(name) >= 0) {
        con_puts("User already exists.\n");
        return;
    }
    if (user_count >= USER_MAX) {
        con_puts("User table is full.\n");
        return;
    }

    con_puts("Password (empty for none): ");
    read_secret(pw, sizeof(pw));
    if (pw[0]) {
        con_puts("Confirm password: ");
        read_secret(pw2, sizeof(pw2));
        if (strcmp(pw, pw2) != 0) {
            con_puts("Passwords do not match.\n");
            return;
        }
    }

    con_puts("Make admin? (y/N): ");
    read_line(ans, sizeof(ans));

    struct user *u = &users[user_count];
    memset(u, 0, sizeof(*u));
    strcpy(u->name, name);
    u->hash = hash_password(pw);
    u->admin = (ans[0] == 'y' || ans[0] == 'Y');
    user_count++;

    if (user_save() < 0) {
        user_count--;
        con_puts("Cannot save user database.\n");
        return;
    }
    con_printf("User '%s' created.\n", u->name);
}

static void cmd_cu(const char *name)
{
    if (!name) {
        con_puts("Usage: cu <username>\n");
        return;
    }

    int idx = user_find(name);
    if (idx < 0) {
        con_puts("No such user.\n");
        return;
    }

    if (users[idx].hash) {
        char pw[32];
        con_puts("Password: ");
        read_secret(pw, sizeof(pw));
        if (hash_password(pw) != users[idx].hash) {
            con_puts("Login incorrect.\n");
            return;
        }
    }

    cur_user = idx;
    con_printf("Switched to '%s'.\n", users[cur_user].name);
}

static void cmd_passwd(const char *name)
{
    char old[32], pw[32], pw2[32];

    if (!fs_ok) {
        con_puts("No filesystem.\n");
        return;
    }

    int idx = name ? user_find(name) : cur_user;
    if (idx < 0) {
        con_puts("No such user.\n");
        return;
    }
    if (idx != cur_user && !users[cur_user].admin) {
        con_puts("Permission denied.\n");
        return;
    }

    if (idx == cur_user && users[idx].hash) {
        con_puts("Current password: ");
        read_secret(old, sizeof(old));
        if (hash_password(old) != users[idx].hash) {
            con_puts("Incorrect password.\n");
            return;
        }
    }

    con_puts("New password (empty for none): ");
    read_secret(pw, sizeof(pw));
    if (pw[0]) {
        con_puts("Confirm password: ");
        read_secret(pw2, sizeof(pw2));
        if (strcmp(pw, pw2) != 0) {
            con_puts("Passwords do not match.\n");
            return;
        }
    }

    users[idx].hash = hash_password(pw);
    if (user_save() < 0) {
        con_puts("Cannot save user database.\n");
        return;
    }
    con_printf("Password for '%s' changed.\n", users[idx].name);
}

/* ---- processes ---- */
static u8 prog_buf[32 * 1024];

static void cmd_run(const char *arg)
{
    struct fat_dirent de;

    if (!arg) {
        con_puts("Usage: run <file>\n");
        return;
    }
    if (!fs_ok || fs_find(arg, &de) < 0) {
        con_puts("File not found.\n");
        return;
    }
    if (de.attr & 0x10) {
        con_puts("Is a directory.\n");
        return;
    }
    if (de.size > sizeof(prog_buf)) {
        con_puts("Program too large.\n");
        return;
    }

    int n = fs_read_file(&de, prog_buf, sizeof(prog_buf));
    if (n != (int)de.size) {
        con_puts("Cannot read program.\n");
        return;
    }

    int pid = sched_spawn(arg, prog_buf, (u32)n);
    if (pid < 0) {
        if (pid == -1)
            con_puts("Process table is full.\n");
        else if (pid == -2)
            con_puts("Invalid program.\n");
        else
            con_puts("Out of memory.\n");
        return;
    }
    con_printf("Started '%s' as pid %d.\n", arg, pid);
}

static void cmd_kill(const char *arg)
{
    if (!arg) {
        con_puts("Usage: kill <pid>\n");
        return;
    }
    int rc = sched_kill(atoi(arg));
    if (rc == 0)
        con_printf("Process %d killed.\n", atoi(arg));
    else if (rc == -2)
        con_puts("No such process.\n");
    else
        con_puts("Cannot kill this process.\n");
}

static void cmd_vm(void)
{
    con_puts("Paging: enabled (CR0.PG)\n");
    con_printf("Kernel CR3: 0x%x\n", paging_kernel_cr3());
    con_printf("Frames: %u used / %u total (%u KB free)\n",
               paging_frame_used(), paging_frame_total(),
               (paging_frame_total() - paging_frame_used()) * 4);
    con_printf("User code VA:  0x%x\n", USER_CODE_VA);
    con_printf("User stack VA: 0x%x\n", USER_STACK_VA);
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
        con_printf("%s\n", users[cur_user].name);
    } else if (!strcmp(line, "cru")) {
        cmd_cru(arg);
    } else if (!strcmp(line, "cu")) {
        cmd_cu(arg);
    } else if (!strcmp(line, "passwd")) {
        cmd_passwd(arg);
    } else if (!strcmp(line, "alias")) {
        cmd_alias(arg);
    } else if (!strcmp(line, "pck")) {
        cmd_pck(arg);
    } else if (!strcmp(line, "net")) {
        net_info();
    } else if (!strcmp(line, "ping")) {
        cmd_ping(arg);
    } else if (!strcmp(line, "run")) {
        cmd_run(arg);
    } else if (!strcmp(line, "ps")) {
        sched_ps();
    } else if (!strcmp(line, "kill")) {
        cmd_kill(arg);
    } else if (!strcmp(line, "vm")) {
        cmd_vm();
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
    paging_init();
    sched_init();
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

    user_load();
    net_init();

    for (;;) {
        con_puts("xsh:ROS");
        con_puts(" ");
        con_puts(users[cur_user].name);
        con_puts(" ");
        con_puts(fs_cwd());
        con_puts("> ");
        read_line(cmdline, LINE_MAX);
        dispatch(cmdline);
    }
}
