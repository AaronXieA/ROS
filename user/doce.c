/* DocEditor (doce) - a full-screen text editor for ROS Command.
 *
 * Runs in ring 3. File I/O, keyboard input and VGA access go through
 * int 0x80 (see k32/syscall.c).
 *
 * Keys: arrows, Home/End, PgUp/PgDn, Enter, Backspace, Delete, Tab
 *       Ctrl+S save, Ctrl+Q quit (press twice when there are unsaved changes)
 */
typedef unsigned char u8;
typedef unsigned int  u32;
typedef unsigned int  size_t;

#define SYS_WRITE      1
#define SYS_EXIT       2
#define SYS_GETCHAR    5
#define SYS_CLEAR      6
#define SYS_PUTAT      7
#define SYS_SETCURSOR  8
#define SYS_READFILE   9
#define SYS_WRITEFILE 10
#define SYS_ARGS      11

#define ROWS       25
#define COLS       80
#define TEXT_ROWS  23
#define LINES     100
#define LINE_MAX   78
#define DOC_MAX  8000

#define C_TEXT   0x07
#define C_STATUS 0x70
#define C_HINT   0x17
#define C_ERROR  0x4F

/* keyboard codes (0x80 | scancode for 0xE0-prefixed keys) */
#define K_UP     0xC8
#define K_DOWN   0xD0
#define K_LEFT   0xCB
#define K_RIGHT  0xCD
#define K_HOME   0xC7
#define K_END    0xCF
#define K_PGUP   0xC9
#define K_PGDN   0xD1
#define K_DEL    0xD3
#define K_CTRL_S 0x13
#define K_CTRL_Q 0x11

/* Big arrays must live in .data (a flat binary has no .bss contents),
 * hence the dummy initializers; _start() clears them. */
static char doc_name[16] = "UNTITLED.TXT";
static char doc[LINES][LINE_MAX + 1] = { { 1 } };
static u8   len[LINES] = { 1 };
static char flat[DOC_MAX] = { 1 };
static char status[48] = { 1 };

static int nlines = 1;
static int cur_line, cur_col, top;
static int dirty, quit_armed, status_err;

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);

static int sys_call(int n, int a, int b, int c)
{
    int ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

/* ---- screen ---- */
static void putat(int r, int c, char ch, u8 color)
{
    sys_call(SYS_PUTAT, (r << 8) | c, (u8)ch | ((int)color << 8), 0);
}

static void putstr(int r, int c, const char *s, u8 color)
{
    while (*s && c < COLS)
        putat(r, c++, *s++, color);
}

static void fill_row(int r, u8 color)
{
    for (int c = 0; c < COLS; c++)
        putat(r, c, ' ', color);
}

static void set_cursor(int r, int c)
{
    sys_call(SYS_SETCURSOR, (r << 8) | c, 0, 0);
}

static int putnum(int r, int c, int v, u8 color)
{
    char b[12];
    int n = 0;

    if (v == 0)
        b[n++] = '0';
    while (v > 0) {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0)
        putat(r, c++, b[--n], color);
    return c;
}

static void write_str(const char *s)
{
    sys_call(SYS_WRITE, (int)s, (int)strlen(s), 0);
}

static void set_status(const char *s, int err)
{
    int i = 0;

    while (s[i] && i < (int)sizeof(status) - 1) {
        status[i] = s[i];
        i++;
    }
    status[i] = 0;
    status_err = err;
}

/* ---- document ---- */
static void draw(void)
{
    if (cur_line < top)
        top = cur_line;
    if (cur_line >= top + TEXT_ROWS)
        top = cur_line - TEXT_ROWS + 1;

    for (int r = 0; r < TEXT_ROWS; r++) {
        int li = top + r;
        int c = 0;

        if (li < nlines) {
            for (; c < len[li] && c < COLS; c++)
                putat(r, c, doc[li][c], C_TEXT);
        }
        for (; c < COLS; c++)
            putat(r, c, ' ', C_TEXT);
    }

    /* status bar */
    fill_row(23, C_STATUS);
    putstr(23, 0, " DocEditor ", C_STATUS);
    putstr(23, 11, doc_name, C_STATUS);
    int c = 11 + (int)strlen(doc_name);
    putstr(23, c, "  L", C_STATUS);
    c = putnum(23, c + 3, cur_line + 1, C_STATUS);
    putstr(23, c, " C", C_STATUS);
    c = putnum(23, c + 2, cur_col + 1, C_STATUS);
    putstr(23, c, "  ", C_STATUS);
    putstr(23, c + 2, dirty ? "MOD" : "SAVED", C_STATUS);

    /* message / key hints */
    fill_row(24, C_HINT);
    if (status[0])
        putstr(24, 0, status, status_err ? C_ERROR : C_HINT);
    else
        putstr(24, 0, " ^S save  ^Q quit  arrows  PgUp/Dn  Enter  BS/Del  Tab",
               C_HINT);

    set_cursor(cur_line - top, cur_col);
}

static void insert_char(char ch)
{
    if (len[cur_line] >= LINE_MAX)
        return;
    char *l = doc[cur_line];
    for (int i = len[cur_line]; i > cur_col; i--)
        l[i] = l[i - 1];
    l[cur_col] = ch;
    len[cur_line]++;
    cur_col++;
    dirty = 1;
}

static void insert_newline(void)
{
    if (nlines >= LINES) {
        set_status("Document is full (100 lines)", 1);
        return;
    }

    char *l = doc[cur_line];
    int tail = len[cur_line] - cur_col;

    for (int i = nlines; i > cur_line + 1; i--) {
        memcpy(doc[i], doc[i - 1], LINE_MAX + 1);
        len[i] = len[i - 1];
    }
    for (int i = 0; i < tail; i++)
        doc[cur_line + 1][i] = l[cur_col + i];
    len[cur_line + 1] = (u8)tail;
    len[cur_line] = (u8)cur_col;

    nlines++;
    cur_line++;
    cur_col = 0;
    dirty = 1;
}

static void backspace(void)
{
    if (cur_col > 0) {
        char *l = doc[cur_line];
        for (int i = cur_col - 1; i < len[cur_line] - 1; i++)
            l[i] = l[i + 1];
        len[cur_line]--;
        cur_col--;
        dirty = 1;
        return;
    }
    if (cur_line == 0)
        return;

    int plen = len[cur_line - 1];
    int clen = len[cur_line];
    if (plen + clen > LINE_MAX) {
        set_status("Cannot join: line too long", 1);
        return;
    }
    for (int i = 0; i < clen; i++)
        doc[cur_line - 1][plen + i] = doc[cur_line][i];
    len[cur_line - 1] = (u8)(plen + clen);
    for (int i = cur_line; i < nlines - 1; i++) {
        memcpy(doc[i], doc[i + 1], LINE_MAX + 1);
        len[i] = len[i + 1];
    }
    nlines--;
    cur_line--;
    cur_col = plen;
    dirty = 1;
}

static void delete_char(void)
{
    if (cur_col < len[cur_line]) {
        char *l = doc[cur_line];
        for (int i = cur_col; i < len[cur_line] - 1; i++)
            l[i] = l[i + 1];
        len[cur_line]--;
        dirty = 1;
        return;
    }
    if (cur_line + 1 >= nlines)
        return;

    int clen = len[cur_line];
    int nlen = len[cur_line + 1];
    if (clen + nlen > LINE_MAX) {
        set_status("Cannot join: line too long", 1);
        return;
    }
    for (int i = 0; i < nlen; i++)
        doc[cur_line][clen + i] = doc[cur_line + 1][i];
    len[cur_line] = (u8)(clen + nlen);
    for (int i = cur_line + 1; i < nlines - 1; i++) {
        memcpy(doc[i], doc[i + 1], LINE_MAX + 1);
        len[i] = len[i + 1];
    }
    nlines--;
    dirty = 1;
}

static void tab(void)
{
    int spaces = 4 - (cur_col & 3);

    while (spaces-- > 0)
        insert_char(' ');
}

/* ---- file I/O ---- */
static int load_file(void)
{
    int n = sys_call(SYS_READFILE, (int)doc_name, (int)flat, DOC_MAX);

    nlines = 1;
    len[0] = 0;
    cur_line = cur_col = top = 0;

    if (n == -1) {
        set_status("New file", 0);
        return 0;
    }
    if (n < 0) {
        set_status("Cannot open file", 1);
        return -1;
    }

    int li = 0, col = 0;
    for (int i = 0; i < n; i++) {
        char ch = flat[i];

        if (ch == '\r')
            continue;
        if (ch == '\n') {
            len[li] = (u8)col;
            if (li + 1 < LINES) {
                li++;
                col = 0;
            }
            continue;
        }
        if (ch == '\t') {
            int spaces = 4 - (col & 3);
            while (spaces-- > 0 && col < LINE_MAX)
                doc[li][col++] = ' ';
            continue;
        }
        if (col < LINE_MAX)
            doc[li][col++] = ch;
    }
    len[li] = (u8)col;
    nlines = li + 1;
    return 0;
}

static void save_file(void)
{
    int p = 0;

    for (int i = 0; i < nlines; i++) {
        for (int c = 0; c < len[i]; c++)
            flat[p++] = doc[i][c];
        if (i + 1 < nlines)
            flat[p++] = '\n';
    }

    int rc = sys_call(SYS_WRITEFILE, (int)doc_name, (int)flat, p);
    if (rc == 0) {
        dirty = 0;
        quit_armed = 0;
        set_status("Saved", 0);
    } else if (rc == -2) {
        set_status("Save failed: document too large", 1);
    } else {
        set_status("Save failed (8.3 name or disk full)", 1);
    }
}

/* ---- start / exit ---- */
static int name_ok(const char *s)
{
    int base = 0, ext = 0;

    while (*s && *s != '.') {
        char c = *s++;
        if (c < '!' || c > '~')
            return 0;
        base++;
    }
    if (*s == '.') {
        s++;
        while (*s) {
            char c = *s++;
            if (c < '!' || c > '~')
                return 0;
            ext++;
        }
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

static void leave(const char *msg)
{
    sys_call(SYS_CLEAR, 0, 0, 0);
    if (msg) {
        write_str("DocEditor: ");
        write_str(msg);
        write_str("\n");
    }
    sys_call(SYS_EXIT, 0, 0, 0);
    for (;;)
        ;
}

void _start(void)
{
    char args[64];
    int n = sys_call(SYS_ARGS, (int)args, (int)sizeof(args) - 1, 0);

    if (n > 0) {
        int i = 0, j = 0;

        while (i < n && args[i] == ' ')
            i++;
        while (i < n && args[i] && args[i] != ' ') {
            if (j < 15)
                doc_name[j++] = args[i];
            i++;
        }
        doc_name[j] = 0;
        if (j == 0)
            doc_name[0] = 0;        /* fall back below */
        for (i = 0; i < j; i++) {
            if (doc_name[i] >= 'a' && doc_name[i] <= 'z')
                doc_name[i] = (char)(doc_name[i] - 32);
        }
    }
    if (!doc_name[0])
        memcpy(doc_name, "UNTITLED.TXT", 13);

    memset(doc, 0, sizeof(doc));
    memset(len, 0, sizeof(len));
    memset(status, 0, sizeof(status));
    dirty = quit_armed = status_err = 0;

    if (!name_ok(doc_name)) {
        leave("invalid filename (8.3 format)");
        return;
    }

    sys_call(SYS_CLEAR, 0, 0, 0);
    if (load_file() < 0) {
        leave("cannot read file");
        return;
    }
    draw();

    for (;;) {
        int key = sys_call(SYS_GETCHAR, 0, 0, 0);

        if (key < 0)
            continue;
        if (key != K_CTRL_Q)
            quit_armed = 0;

        switch (key) {
        case K_CTRL_S:
            save_file();
            break;
        case K_CTRL_Q:
            if (!dirty || quit_armed)
                leave(dirty ? "closed with unsaved changes" : "closed");
            quit_armed = 1;
            set_status("Unsaved changes: Ctrl+Q again to quit, Ctrl+S to save",
                       1);
            break;
        case '\n':
            insert_newline();
            break;
        case '\b':
            backspace();
            break;
        case '\t':
            tab();
            break;
        case K_DEL:
            delete_char();
            break;
        case K_LEFT:
            if (cur_col > 0)
                cur_col--;
            break;
        case K_RIGHT:
            if (cur_col < len[cur_line])
                cur_col++;
            break;
        case K_UP:
            if (cur_line > 0)
                cur_line--;
            break;
        case K_DOWN:
            if (cur_line + 1 < nlines)
                cur_line++;
            break;
        case K_HOME:
            cur_col = 0;
            break;
        case K_END:
            cur_col = len[cur_line];
            break;
        case K_PGUP:
            cur_line -= TEXT_ROWS;
            if (cur_line < 0)
                cur_line = 0;
            break;
        case K_PGDN:
            cur_line += TEXT_ROWS;
            if (cur_line >= nlines)
                cur_line = nlines - 1;
            break;
        default:
            if (key >= 32 && key <= 126)
                insert_char((char)key);
            break;
        }

        if (cur_col > len[cur_line])
            cur_col = len[cur_line];
        draw();
    }
}
