/* jvm - minimal Java Virtual Machine for ROS Command user space.
 *
 * Loads a single .class file and runs its public static void main(String[]).
 * Handles class files up to major version 52 (Java 8).
 *
 * Supported instructions: constants, loads/stores, integer arithmetic,
 * branches, method calls, static fields, new, arrays, ldc of strings/ints.
 * Native stubs: java/lang/Object.<init>, java/lang/System.out,
 *               java/io/PrintStream.{print,println} over String/int.
 *
 * Build: clang -target i386-elf ... -c user/jvm.c
 * Link:  ld.lld -m elf_i386 -T user/user.ld
 */
#include "rt.h"

/* rt.h provides u8/u32 only */
typedef unsigned short u16;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

/* ================= utilities ================= */
static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void memcpy_(void *d, const void *s, u32 n) {
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
}
static void memset_(void *d, int c, u32 n) {
    u8 *dd = (u8 *)d;
    while (n--) *dd++ = (u8)c;
}
static int memeq_(const void *a, const void *b, u32 n) {
    const u8 *aa = (const u8 *)a, *bb = (const u8 *)b;
    while (n--) if (*aa++ != *bb++) return 0;
    return 1;
}
static u32 slen_(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void die(const char *msg) {
    print("jvm: ");
    print(msg);
    print("\n");
    sys_call(SYS_EXIT, 1, 0, 0);
    for (;;) ;
}

/* ================= class file ================= */
#define CP_UTF8  1
#define CP_INT   3
#define CP_FLT   4
#define CP_LONG  5
#define CP_DBL   6
#define CP_CLASS 7
#define CP_STR   8
#define CP_FLD   9
#define CP_MTH   10
#define CP_IMTH  11
#define CP_NT    12

struct cp_entry { u8 tag; u32 a, b; };
struct method_t {
    u16 access, name_idx, desc_idx;
    u16 max_stack, max_locals;
    u8 *code;
    u32 code_len;
};
struct field_t { u16 access, name_idx, desc_idx; u32 val; };

#define MAX_CP 256
#define MAX_M  32
#define MAX_F  16

struct clazz {
    u8 *data;
    u32 size;
    struct cp_entry *cp;
    u16 cp_count;
    u16 this_class, super_class;
    struct method_t m[MAX_M];
    u16 mcount;
    struct field_t f[MAX_F];
    u16 fcount;
};

static struct clazz g_cl;
static u8 *g_filebuf;
static volatile __attribute__((used)) u8 g_force_data = 1;  /* keep .data PROGBITS */

static const char *cp_utf8(struct clazz *c, u16 i, u32 *l) {
    struct cp_entry *e;
    if (!i || i >= c->cp_count) return 0;
    e = &c->cp[i];
    if (e->tag != CP_UTF8) return 0;
    *l = e->b;
    return (const char *)e->a;
}

static int utf8_eq(const char *p, u32 plen, const char *s) {
    u32 sl = slen_(s);
    if (sl != plen) return 0;
    return memeq_(p, s, plen);
}

static const char *cp_clsname(struct clazz *c, u16 i, u32 *l) {
    struct cp_entry *e;
    if (!i || i >= c->cp_count) return 0;
    e = &c->cp[i];
    if (e->tag != CP_CLASS) return 0;
    return cp_utf8(c, (u16)e->a, l);
}

static int parse_class(u8 *b, u32 sz, struct clazz *c) {
    u8 *p, *end;
    u16 i;

    if (sz < 12 || rd32(b) != 0xCAFEBABEu) return -1;
    if (rd16(b + 6) > 52) return -2;
    c->data = b; c->size = sz;
    c->cp_count = rd16(b + 8);
    if (!c->cp_count || c->cp_count > MAX_CP) return -3;
    c->cp = (struct cp_entry *)malloc(sizeof(struct cp_entry) * c->cp_count);
    if (!c->cp) return -4;
    memset_(c->cp, 0, sizeof(struct cp_entry) * c->cp_count);

    p = b + 10; end = b + sz;
    for (i = 1; i < c->cp_count; i++) {
        u8 tag;
        if (p >= end) return -5;
        tag = *p++;
        c->cp[i].tag = tag;
        switch (tag) {
        case CP_UTF8: {
            u16 l = rd16(p); p += 2;
            c->cp[i].a = (u32)p;
            c->cp[i].b = l;
            p += l;
            break;
        }
        case CP_INT: case CP_FLT:
            c->cp[i].a = rd32(p); p += 4;
            break;
        case CP_LONG: case CP_DBL:
            c->cp[i].a = rd32(p);
            c->cp[i].b = rd32(p + 4);
            p += 8;
            i++;
            break;
        case CP_CLASS: case CP_STR:
            c->cp[i].a = rd16(p); p += 2;
            break;
        case CP_FLD: case CP_MTH: case CP_IMTH: case CP_NT:
            c->cp[i].a = rd16(p);
            c->cp[i].b = rd16(p + 2);
            p += 4;
            break;
        default:
            return -6;
        }
    }

    if (p + 8 > end) return -7;
    p += 2;                                    /* access flags */
    c->this_class = rd16(p); p += 2;
    c->super_class = rd16(p); p += 2;
    { u16 n = rd16(p); p += 2 + (u32)n * 2; }  /* interfaces */

    /* fields */
    {
        u16 n = rd16(p); p += 2;
        if (n > MAX_F) return -8;
        c->fcount = n;
        for (i = 0; i < n; i++) {
            struct field_t *f = &c->f[i];
            u16 na, j;
            f->access = rd16(p); p += 2;
            f->name_idx = rd16(p); p += 2;
            f->desc_idx = rd16(p); p += 2;
            f->val = 0;
            na = rd16(p); p += 2;
            for (j = 0; j < na; j++) {
                u32 al;
                p += 2;
                al = rd32(p); p += 4 + al;
            }
        }
    }

    /* methods */
    {
        u16 n = rd16(p); p += 2;
        if (n > MAX_M) return -9;
        c->mcount = n;
        for (i = 0; i < n; i++) {
            struct method_t *m = &c->m[i];
            u16 na, j;
            memset_(m, 0, sizeof(*m));
            m->access = rd16(p); p += 2;
            m->name_idx = rd16(p); p += 2;
            m->desc_idx = rd16(p); p += 2;
            na = rd16(p); p += 2;
            for (j = 0; j < na; j++) {
                u16 an = rd16(p); p += 2;
                u32 al = rd32(p); p += 4;
                u8 *ae = p + al;
                u32 nl; const char *nm = cp_utf8(c, an, &nl);
                if (nm && nl == 4 && memeq_(nm, "Code", 4)) {
                    m->max_stack = rd16(p); p += 2;
                    m->max_locals = rd16(p); p += 2;
                    m->code_len = rd32(p); p += 4;
                    m->code = p;
                }
                p = ae;
            }
        }
    }
    return 0;
}

/* ================= objects + mark-sweep GC ================= */
#define OK_STR  1
#define OK_IARR 2
#define OK_BARR 3
#define OK_NAT  4

struct ohdr { u8 kind, mark; u16 pad; };
struct sobj { struct ohdr h; u32 len; char data[1]; };
struct aobj { struct ohdr h; u32 len; u32 data[1]; };

/* GC tracks every live object in a fixed-capacity table. When the table is
 * full we run a mark-sweep collection before allocating more. */
#define GC_MAX 256

static u32 g_gc_refs[GC_MAX];
static u16 g_gc_count;
static u32 g_gc_collections;

static struct ohdr *gc_alloc(u8 kind, u32 payload);

static u32 obj_str(const char *s, u32 n) {
    struct sobj *o = (struct sobj *)gc_alloc(OK_STR, sizeof(u32) + n + 1);
    o->len = n;
    memcpy_(o->data, s, n);
    o->data[n] = 0;
    return (u32)o;
}

static u32 obj_nat(u32 tag) {
    struct ohdr *o = gc_alloc(OK_NAT, 4);
    *(u32 *)((u8 *)o + sizeof(*o)) = tag;
    return (u32)o;
}

static u32 obj_iarr(u32 n) {
    struct aobj *o = (struct aobj *)gc_alloc(OK_IARR, sizeof(u32) + (n ? n : 1) * 4);
    o->len = n;
    memset_(o->data, 0, n * 4);
    return (u32)o;
}

static u32 obj_barr(u32 n) {
    struct aobj *o = (struct aobj *)gc_alloc(OK_BARR, sizeof(u32) + (n ? n : 1));
    o->len = n;
    memset_(o->data, 0, n);
    return (u32)o;
}

/* ================= frames / interpreter ================= */
#define MAX_FR 16
#define MAX_OS 64
#define MAX_LV 32

struct frame {
    struct method_t *m;
    u8 *pc, *cs;
    u32 lv[MAX_LV];
    u32 st[MAX_OS];
    u32 *sp;
};

static struct frame g_fr[MAX_FR];
static int g_dep;

static struct frame *fr_push(struct method_t *m) {
    struct frame *f;
    if (g_dep >= MAX_FR) die("stack overflow");
    f = &g_fr[g_dep++];
    memset_(f, 0, sizeof(*f));
    f->m = m; f->pc = m->code; f->cs = m->code; f->sp = f->st;
    return f;
}

static struct frame *fr_pop(void) {
    if (g_dep > 0) g_dep--;
    return g_dep > 0 ? &g_fr[g_dep - 1] : 0;
}

/* ================= mark-sweep GC ================= */
/* Conservative roots: locals/stack slots may hold ints or refs, and we can't
 * tell them apart. A word is treated as a reference only if it is 4-byte
 * aligned, points inside the mapped heap, and reads back a known kind tag. */
static void gc_mark(u32 ref) {
    struct ohdr *o;
    if (!ref) return;
    if (ref & 3) return;
    if (ref < 0x40000000u) return;
    if (ref >= (u32)sbrk(0)) return;
    o = (struct ohdr *)ref;
    if (o->kind < OK_STR || o->kind > OK_NAT) return;
    if (o->mark) return;
    o->mark = 1;
}

static void gc_collect(struct clazz *c) {
    u16 i, k;
    u16 w;

    /* mark */
    for (i = 0; i < (u16)g_dep; i++) {
        struct frame *f = &g_fr[i];
        for (k = 0; k < MAX_LV; k++)
            gc_mark(f->lv[k]);
        for (k = 0; k < (u16)(f->sp - f->st); k++)
            gc_mark(f->st[k]);
    }
    for (i = 0; i < c->fcount; i++)
        gc_mark(c->f[i].val);

    /* sweep */
    w = 0;
    for (i = 0; i < g_gc_count; i++) {
        struct ohdr *o = (struct ohdr *)g_gc_refs[i];
        if (o->mark) {
            o->mark = 0;                       /* clear for next cycle */
            g_gc_refs[w++] = (u32)o;
        } else {
            free(o);                            /* return block to malloc pool */
        }
    }
    g_gc_count = w;
    g_gc_collections++;
}

static struct ohdr *gc_alloc(u8 kind, u32 payload) {
    struct ohdr *o;
    if (g_gc_count >= GC_MAX) {
        gc_collect(&g_cl);
        if (g_gc_count >= GC_MAX) die("gc: object table full");
    }
    o = (struct ohdr *)malloc(sizeof(*o) + payload);
    if (!o) die("oom");
    o->kind = kind;
    o->mark = 0;
    o->pad = 0;
    g_gc_refs[g_gc_count++] = (u32)o;
    return o;
}

static struct method_t *find_method(struct clazz *c,
                                    const char *name, u32 nl,
                                    const char *desc, u32 dl) {
    u16 i;
    for (i = 0; i < c->mcount; i++) {
        u32 l1, l2;
        const char *n = cp_utf8(c, c->m[i].name_idx, &l1);
        const char *d = cp_utf8(c, c->m[i].desc_idx, &l2);
        if (n && d && l1 == nl && l2 == dl &&
            memeq_(n, name, nl) && memeq_(d, desc, dl))
            return &c->m[i];
    }
    return 0;
}

static int count_args(const char *d, u32 dl) {
    int n = 0;
    u32 i = 1;
    while (i < dl && d[i] != ')') {
        while (i < dl && d[i] == '[') i++;
        if (i < dl && d[i] == 'L') {
            while (i < dl && d[i] != ';') i++;
            i++;
            n++;
        } else if (i < dl) {
            if (d[i] == 'J' || d[i] == 'D') n += 2;
            else n++;
            i++;
        }
    }
    return n;
}

static void native_print_str(u32 r, int nl) {
    if (r) {
        struct sobj *s = (struct sobj *)r;
        if (s->h.kind == OK_STR && s->len)
            sys_call(SYS_WRITE, (int)s->data, (int)s->len, 0);
    }
    if (nl) print("\n");
}

static void native_print_int(s32 v, int nl) {
    char b[12];
    int n = 0;
    u32 u;
    if (v < 0) { print("-"); u = (u32)(-v); }
    else u = (u32)v;
    if (!u) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    while (n > 0) {
        char c = b[--n];
        sys_call(SYS_WRITE, (int)&c, 1, 0);
    }
    if (nl) print("\n");
}

static int try_native(struct frame **pf,
                      const char *cls, u32 cl,
                      const char *name, u32 nl,
                      const char *desc, u32 dl) {
    struct frame *f = *pf;

    if (utf8_eq(cls, cl, "java/lang/Object") &&
        utf8_eq(name, nl, "<init>")) {
        f->sp--;
        return 1;
    }
    if (utf8_eq(cls, cl, "java/io/PrintStream")) {
        if (utf8_eq(name, nl, "println")) {
            if (dl >= 4 && desc[1] == 'I') {
                s32 v = (s32)*--f->sp;
                f->sp--;
                native_print_int(v, 1);
                return 1;
            }
            if (dl >= 4 && desc[1] == 'L') {
                u32 r = *--f->sp;
                f->sp--;
                native_print_str(r, 1);
                return 1;
            }
            if (dl == 3) {
                f->sp--;
                print("\n");
                return 1;
            }
        }
        if (utf8_eq(name, nl, "print")) {
            if (dl >= 4 && desc[1] == 'I') {
                s32 v = (s32)*--f->sp;
                f->sp--;
                native_print_int(v, 0);
                return 1;
            }
            if (dl >= 4 && desc[1] == 'L') {
                u32 r = *--f->sp;
                f->sp--;
                native_print_str(r, 0);
                return 1;
            }
        }
    }
    return 0;
}

static void do_invoke(struct clazz *c, struct frame **pf, u16 cpidx) {
    struct cp_entry *e, *nt;
    const char *cn, *nm, *ds;
    u32 cl, nl, dl;
    struct method_t *m;
    int na, is_static, total, i;
    struct frame *cf, *nf;

    e = &c->cp[cpidx];
    if (e->tag != CP_MTH && e->tag != CP_IMTH) die("bad methodref");
    cn = cp_clsname(c, (u16)e->a, &cl);
    nt = &c->cp[(u16)e->b];
    nm = cp_utf8(c, (u16)nt->a, &nl);
    ds = cp_utf8(c, (u16)nt->b, &dl);
    if (!cn || !nm || !ds) die("bad methodref data");

    if (try_native(pf, cn, cl, nm, nl, ds, dl)) return;

    m = find_method(c, nm, nl, ds, dl);
    if (!m) die("no such method");
    if (!m->code) die("no code");

    na = count_args(ds, dl);
    is_static = (m->access & 8) != 0;
    total = na + (is_static ? 0 : 1);

    cf = *pf;
    nf = fr_push(m);
    for (i = total - 1; i >= 0; i--)
        nf->lv[i] = *--cf->sp;
    *pf = nf;
}

static void do_getstatic(struct clazz *c, struct frame *f, u16 cpidx) {
    struct cp_entry *e, *nt;
    const char *cn, *nm;
    u32 cl, nl;
    u16 i;

    e = &c->cp[cpidx];
    if (e->tag != CP_FLD) die("getstatic: not a field");
    cn = cp_clsname(c, (u16)e->a, &cl);
    nt = &c->cp[(u16)e->b];
    nm = cp_utf8(c, (u16)nt->a, &nl);

    /* java/lang/System.out -> native PrintStream stub */
    if (utf8_eq(cn, cl, "java/lang/System") && utf8_eq(nm, nl, "out")) {
        *f->sp++ = obj_nat(1);
        return;
    }

    for (i = 0; i < c->fcount; i++) {
        u32 l1;
        const char *n = cp_utf8(c, c->f[i].name_idx, &l1);
        if (n && l1 == nl && memeq_(n, nm, nl)) {
            *f->sp++ = c->f[i].val;
            return;
        }
    }
    die("getstatic: no such field");
}

static void do_putstatic(struct clazz *c, struct frame *f, u16 cpidx) {
    struct cp_entry *e, *nt;
    const char *nm;
    u32 nl;
    u16 i;

    e = &c->cp[cpidx];
    if (e->tag != CP_FLD) die("putstatic: not a field");
    nt = &c->cp[(u16)e->b];
    nm = cp_utf8(c, (u16)nt->a, &nl);

    for (i = 0; i < c->fcount; i++) {
        u32 l1;
        const char *n = cp_utf8(c, c->f[i].name_idx, &l1);
        if (n && l1 == nl && memeq_(n, nm, nl)) {
            c->f[i].val = *--f->sp;
            return;
        }
    }
    die("putstatic: no such field");
}

static void do_ldc(struct clazz *c, struct frame *f, u16 i) {
    struct cp_entry *e = &c->cp[i];
    if (e->tag == CP_STR) {
        u32 l;
        const char *p = cp_utf8(c, (u16)e->a, &l);
        *f->sp++ = obj_str(p, l);
    } else if (e->tag == CP_INT) {
        *f->sp++ = e->a;
    } else {
        die("ldc: unsupported");
    }
}

static void execute(struct clazz *c, struct method_t *main_m) {
    struct frame *f = fr_push(main_m);
    f->lv[0] = 0;

    for (;;) {
        u8 *opc = f->pc;
        u8 op = *f->pc++;

        switch (op) {
        case 0x00: break;                                        /* nop */
        case 0x01: *f->sp++ = 0; break;                          /* aconst_null */
        case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x06: case 0x07: case 0x08:
            *f->sp++ = (u32)(op - 0x03); break;                  /* iconst_* */
        case 0x10: { s8 v = (s8)*f->pc++; *f->sp++ = (u32)(s32)v; break; }
        case 0x11: { s16 v = (s16)rd16(f->pc); f->pc += 2;
                     *f->sp++ = (u32)(s32)v; break; }
        case 0x12: do_ldc(c, f, *f->pc++); break;
        case 0x13: { u16 i = rd16(f->pc); f->pc += 2;
                     do_ldc(c, f, i); break; }
        case 0x15: *f->sp++ = f->lv[*f->pc++]; break;            /* iload */
        case 0x19: *f->sp++ = f->lv[*f->pc++]; break;            /* aload */
        case 0x1a: case 0x1b: case 0x1c: case 0x1d:
            *f->sp++ = f->lv[op - 0x1a]; break;                  /* iload_ */
        case 0x2a: case 0x2b: case 0x2c: case 0x2d:
            *f->sp++ = f->lv[op - 0x2a]; break;                  /* aload_ */
        case 0x2e: {                                             /* iaload */
            s32 i = (s32)*--f->sp;
            struct aobj *a = (struct aobj *)*--f->sp;
            *f->sp++ = a->data[i];
            break;
        }
        case 0x33: {                                             /* baload */
            s32 i = (s32)*--f->sp;
            struct aobj *a = (struct aobj *)*--f->sp;
            *f->sp++ = ((u8 *)a->data)[i];
            break;
        }
        case 0x36: f->lv[*f->pc++] = *--f->sp; break;            /* istore */
        case 0x3a: f->lv[*f->pc++] = *--f->sp; break;            /* astore */
        case 0x3b: case 0x3c: case 0x3d: case 0x3e:
            f->lv[op - 0x3b] = *--f->sp; break;                  /* istore_ */
        case 0x4b: case 0x4c: case 0x4d: case 0x4e:
            f->lv[op - 0x4b] = *--f->sp; break;                  /* astore_ */
        case 0x4f: {                                             /* iastore */
            u32 v = *--f->sp;
            s32 i = (s32)*--f->sp;
            struct aobj *a = (struct aobj *)*--f->sp;
            a->data[i] = v;
            break;
        }
        case 0x54: {                                             /* bastore */
            u32 v = *--f->sp;
            s32 i = (s32)*--f->sp;
            struct aobj *a = (struct aobj *)*--f->sp;
            ((u8 *)a->data)[i] = (u8)v;
            break;
        }
        case 0x57: --f->sp; break;                               /* pop */
        case 0x59: { u32 v = f->sp[-1]; *f->sp++ = v; break; }   /* dup */
        case 0x60: { s32 b=(s32)*--f->sp, a=(s32)*--f->sp;
                     *f->sp++ = (u32)(a+b); break; }
        case 0x64: { s32 b=(s32)*--f->sp, a=(s32)*--f->sp;
                     *f->sp++ = (u32)(a-b); break; }
        case 0x68: { s32 b=(s32)*--f->sp, a=(s32)*--f->sp;
                     *f->sp++ = (u32)(a*b); break; }
        case 0x6c: { s32 b=(s32)*--f->sp, a=(s32)*--f->sp;
                     *f->sp++ = b ? (u32)(a/b) : 0; break; }
        case 0x70: { s32 b=(s32)*--f->sp, a=(s32)*--f->sp;
                     *f->sp++ = b ? (u32)(a%b) : 0; break; }
        case 0x74: { s32 a=(s32)*--f->sp; *f->sp++ = (u32)-a; break; }
        case 0x84: { u8 i = *f->pc++; s8 d = (s8)*f->pc++;
                     f->lv[i] = (u32)((s32)f->lv[i] + (s32)d); break; }
        case 0x99: case 0x9a: case 0x9b:
        case 0x9c: case 0x9d: case 0x9e: {
            s16 off = (s16)rd16(f->pc);
            s32 v = (s32)*--f->sp;
            int take;
            switch (op) {
            case 0x99: take = (v == 0); break;
            case 0x9a: take = (v != 0); break;
            case 0x9b: take = (v < 0);  break;
            case 0x9c: take = (v >= 0); break;
            case 0x9d: take = (v > 0);  break;
            default:   take = (v <= 0); break;
            }
            f->pc = take ? opc + off : opc + 3;
            break;
        }
        case 0x9f: case 0xa0: case 0xa1:
        case 0xa2: case 0xa3: case 0xa4: {
            s16 off = (s16)rd16(f->pc);
            s32 b = (s32)*--f->sp, a = (s32)*--f->sp;
            int take;
            switch (op) {
            case 0x9f: take = (a == b); break;
            case 0xa0: take = (a != b); break;
            case 0xa1: take = (a < b);  break;
            case 0xa2: take = (a >= b); break;
            case 0xa3: take = (a > b);  break;
            default:   take = (a <= b); break;
            }
            f->pc = take ? opc + off : opc + 3;
            break;
        }
        case 0xa7: { s16 off = (s16)rd16(f->pc);
                     f->pc = opc + off; break; }                 /* goto */
        case 0xac: { u32 v = *--f->sp;                           /* ireturn */
                     f = fr_pop();
                     if (!f) return;
                     *f->sp++ = v;
                     break; }
        case 0xad: { u32 v = *--f->sp;                           /* lreturn (treat as int) */
                     f = fr_pop();
                     if (!f) return;
                     *f->sp++ = v;
                     break; }
        case 0xb0: { u32 v = *--f->sp;                           /* areturn */
                     f = fr_pop();
                     if (!f) return;
                     *f->sp++ = v;
                     break; }
        case 0xb1: f = fr_pop(); if (!f) return; break;          /* return */
        case 0xb2: { u16 i = rd16(f->pc); f->pc += 2;
                     do_getstatic(c, f, i); break; }
        case 0xb3: { u16 i = rd16(f->pc); f->pc += 2;
                     do_putstatic(c, f, i); break; }
        case 0xb6: { u16 i = rd16(f->pc); f->pc += 2;
                     do_invoke(c, &f, i); break; }
        case 0xb7: { u16 i = rd16(f->pc); f->pc += 2;
                     do_invoke(c, &f, i); break; }
        case 0xb8: { u16 i = rd16(f->pc); f->pc += 2;
                     do_invoke(c, &f, i); break; }
        case 0xbb: { u16 i = rd16(f->pc); f->pc += 2;            /* new */
                     *f->sp++ = obj_nat(i); break; }
        case 0xbc: { u8 atype = *f->pc++;                        /* newarray */
                     s32 n = (s32)*--f->sp;
                     if (n < 0) die("negative array size");
                     if (atype == 10) *f->sp++ = obj_iarr((u32)n);
                     else if (atype == 8) *f->sp++ = obj_barr((u32)n);
                     else die("newarray: unsupported type");
                     break; }
        case 0xbe: { u32 r = *--f->sp;                           /* arraylength */
                     *f->sp++ = ((struct aobj *)r)->len; break; }
        default:
            print("jvm: unknown opcode 0x");
            print_hex(op);
            print("\n");
            die("bad opcode");
        }
    }
}

/* ================= entry ================= */
void _start(void) {
    char args[128];
    int n, sz, rc;
    struct method_t *main_m;

    /* flat binary: BSS is not zeroed by the loader, init explicitly */
    g_gc_count = 0;
    g_gc_collections = 0;
    g_dep = 0;

    n = sys_call(SYS_ARGS, (int)args, sizeof(args) - 1, 0);
    if (n <= 0) {
        print("usage: JVM <FILE.CLS>\n");
        sys_call(SYS_EXIT, 1, 0, 0);
    }
    args[n] = 0;

    g_filebuf = (u8 *)malloc(32 * 1024);
    if (!g_filebuf) die("oom filebuf");

    sz = sys_call(SYS_READFILE, (int)args, (int)g_filebuf, 32 * 1024);
    if (sz <= 0) {
        print("jvm: cannot read ");
        print(args);
        print("\n");
        sys_call(SYS_EXIT, 1, 0, 0);
    }

    memset_(&g_cl, 0, sizeof(g_cl));
    rc = parse_class(g_filebuf, (u32)sz, &g_cl);
    if (rc) {
        print("jvm: class parse error ");
        print_dec((u32)-rc);
        print("\n");
        sys_call(SYS_EXIT, 1, 0, 0);
    }

    main_m = find_method(&g_cl, "main", 4, "([Ljava/lang/String;)V", 22);
    if (!main_m) die("no main([Ljava/lang/String;)V");

    execute(&g_cl, main_m);
    print("[GC collections=");
    print_dec(g_gc_collections);
    print(", live=");
    print_dec(g_gc_count);
    print("]\n");
    sys_call(SYS_EXIT, 0, 0, 0);
}
