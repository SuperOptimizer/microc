/* arm64 assembler: a practical subset of the usual syntax, enough for
   hand-written runtime code. One pass, with local branch fixups. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { O_NONE, O_REG, O_IMM, O_SYM, O_MEM };
enum { SK_PLAIN, SK_PAGE, SK_PAGEOFF };

typedef struct Operand {
    u8 kind;
    u8 cls; /* 'x', 'w', 'd', 's' */
    u8 reg; /* 0..31 */
    u8 sp;  /* register 31 written as sp */
    u8 symkind;
    u8 mode;   /* O_MEM: 0 offset, 1 pre-index, 2 post-index */
    u8 idx;    /* O_MEM: index register or 0xff */
    u8 has_sym;
    i64 imm;
    u32 sym; /* interned name */
} Operand;

typedef struct AFix {
    u32 sec, off, name, line;
    u8 kind; /* 0: 26-bit, 1: 19-bit, 2: 14-bit, 3: adr */
} AFix;

static const char *as_path;
static u32 as_line;
static const char *p; /* cursor in the current line */
static Obj *ao;
static int sec;
static u32 *sym_of; /* interned name -> object symbol + 1 */
static u32 sym_cap;
static AFix *fixes;
static u32 nfixes, capfixes;

static void as_error(const char *msg)
{
    fprintf(stderr, "%s:%u: error: %s\n", as_path, as_line, msg);
    exit(1);
}

static void skip_ws(void)
{
    while (*p == ' ' || *p == '\t')
        p++;
}

static int is_ident(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '$';
}

static int accept_ch(int c)
{
    skip_ws();
    if (*p != c)
        return 0;
    p++;
    return 1;
}

static void expect_ch(int c)
{
    char msg[32];
    if (!accept_ch(c)) {
        sprintf(msg, "expected '%c'", c);
        as_error(msg);
    }
}

static u32 get_sym(u32 name)
{
    if (name >= sym_cap) {
        u32 cap = sym_cap ? sym_cap : 4096;
        while (cap <= name)
            cap *= 2;
        sym_of = (u32 *)xrealloc(sym_of, cap * sizeof(u32));
        memset(sym_of + sym_cap, 0, (cap - sym_cap) * sizeof(u32));
        sym_cap = cap;
    }
    if (!sym_of[name])
        sym_of[name] = obj_add_sym(ao, name, SEC_UNDEF, 0, 0) + 1;
    return sym_of[name] - 1;
}

static i64 number(void)
{
    i64 v = 0;
    int neg = 0, c;
    skip_ws();
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p == '\'') {
        v = (u8)p[1];
        p += 3;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        for (;;) {
            c = *p;
            if (c >= '0' && c <= '9')
                v = v * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f')
                v = v * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v = v * 16 + (c - 'A' + 10);
            else
                break;
            p++;
        }
    } else {
        if (*p < '0' || *p > '9')
            as_error("expected a number");
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
    }
    return neg ? -v : v;
}

/* constant expression: numbers joined by + - * << */
static i64 const_expr(void)
{
    i64 v = number();
    for (;;) {
        skip_ws();
        if (*p == '+') {
            p++;
            v += number();
        } else if (*p == '-') {
            p++;
            v -= number();
        } else if (*p == '*') {
            p++;
            v *= number();
        } else if (p[0] == '<' && p[1] == '<') {
            p += 2;
            v <<= number();
        } else {
            return v;
        }
    }
}

static int parse_reg(Operand *o)
{
    const char *s = p;
    size_t n = 0;
    int c, r;
    while (is_ident(s[n]))
        n++;
    o->sp = 0;
    if (n == 2 && s[0] == 's' && s[1] == 'p') {
        o->cls = 'x'; o->reg = 31; o->sp = 1;
    } else if (n == 3 && memcmp(s, "wsp", 3) == 0) {
        o->cls = 'w'; o->reg = 31; o->sp = 1;
    } else if (n == 3 && (memcmp(s, "xzr", 3) == 0 || memcmp(s, "wzr", 3) == 0)) {
        o->cls = (u8)s[0]; o->reg = 31;
    } else if (n == 2 && s[0] == 'l' && s[1] == 'r') {
        o->cls = 'x'; o->reg = 30;
    } else if (n == 2 && s[0] == 'f' && s[1] == 'p') {
        o->cls = 'x'; o->reg = 29;
    } else if ((n == 2 || n == 3) && (s[0] == 'x' || s[0] == 'w' || s[0] == 'd' || s[0] == 's') && s[1] >= '0' &&
               s[1] <= '9' && (n == 2 || (s[2] >= '0' && s[2] <= '9'))) {
        c = s[0];
        r = n == 2 ? s[1] - '0' : (s[1] - '0') * 10 + (s[2] - '0');
        if (r > 31 || (r == 31 && (c == 'x' || c == 'w')))
            return 0;
        o->cls = (u8)c;
        o->reg = (u8)r;
    } else {
        return 0;
    }
    o->kind = O_REG;
    p += n;
    return 1;
}

static void parse_symref(Operand *o)
{
    const char *s = p;
    while (is_ident(*p))
        p++;
    if (p == s)
        as_error("expected an operand");
    o->sym = intern(s, (size_t)(p - s));
    o->has_sym = 1;
    o->symkind = SK_PLAIN;
    o->imm = 0;
    if (*p == '@') {
        if (strncmp(p, "@PAGEOFF", 8) == 0) {
            o->symkind = SK_PAGEOFF;
            p += 8;
        } else if (strncmp(p, "@PAGE", 5) == 0) {
            o->symkind = SK_PAGE;
            p += 5;
        } else {
            as_error("unknown symbol modifier");
        }
    }
    skip_ws();
    if (*p == '+' || *p == '-')
        o->imm = const_expr();
}

static void parse_operand(Operand *o)
{
    memset(o, 0, sizeof *o);
    o->idx = 0xff;
    skip_ws();
    if (*p == '[') {
        Operand base, idx;
        p++;
        skip_ws();
        if (!parse_reg(&base) || base.cls != 'x')
            as_error("expected a base register");
        o->kind = O_MEM;
        o->reg = base.reg;
        if (accept_ch(',')) {
            skip_ws();
            if (parse_reg(&idx)) {
                o->idx = idx.reg;
            } else {
                accept_ch('#');
                skip_ws();
                if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+')
                    o->imm = const_expr();
                else
                    parse_symref(o);
            }
        }
        expect_ch(']');
        if (*p == '!') {
            p++;
            o->mode = 1;
        } else {
            const char *save = p;
            if (accept_ch(',')) {
                skip_ws();
                if (*p == '#' || *p == '-' || (*p >= '0' && *p <= '9')) {
                    accept_ch('#');
                    o->imm = const_expr();
                    o->mode = 2;
                } else {
                    p = save;
                }
            }
        }
        return;
    }
    if (parse_reg(o))
        return;
    if (*p == '#' || *p == '-' || (*p >= '0' && *p <= '9') || *p == '\'') {
        accept_ch('#');
        o->kind = O_IMM;
        o->imm = const_expr();
        return;
    }
    o->kind = O_SYM;
    parse_symref(o);
}

static void put(u32 insn)
{
    buf_u32(&ao->sec[sec], insn);
}

static void add_fix(u32 name, int kind)
{
    if (nfixes == capfixes) {
        capfixes = capfixes ? capfixes * 2 : 64;
        fixes = (AFix *)xrealloc(fixes, capfixes * sizeof(AFix));
    }
    fixes[nfixes].sec = (u32)sec;
    fixes[nfixes].off = (u32)ao->sec[sec].len;
    fixes[nfixes].name = name;
    fixes[nfixes].line = as_line;
    fixes[nfixes].kind = (u8)kind;
    nfixes++;
}

static int cond_code(const char *s, size_t n)
{
    static const char *const names[] = { "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                         "hi", "ls", "ge", "lt", "gt", "le", "al" };
    int k;
    if (n != 2)
        return -1;
    if (memcmp(s, "hs", 2) == 0)
        return 2;
    if (memcmp(s, "lo", 2) == 0)
        return 3;
    for (k = 0; k < 15; k++)
        if (memcmp(s, names[k], 2) == 0)
            return k;
    return -1;
}

static int parse_cond(void)
{
    const char *s;
    int c;
    skip_ws();
    s = p;
    while (is_ident(*p))
        p++;
    c = cond_code(s, (size_t)(p - s));
    if (c < 0)
        as_error("expected a condition");
    return c;
}

static u32 parse_shift(void)
{
    /* optional ", lsl #n" */
    const char *save = p;
    if (accept_ch(',')) {
        skip_ws();
        if (strncmp(p, "lsl", 3) == 0 && !is_ident(p[3])) {
            p += 3;
            accept_ch('#');
            return (u32)const_expr();
        }
        p = save;
    }
    return 0;
}

static void need(Operand *o, int kind)
{
    if (o->kind != kind)
        as_error("invalid operand");
}

static int streq(const char *s, size_t n, const char *lit)
{
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

static void load_store(const char *m, size_t n, Operand *rt, Operand *mem)
{
    u32 op, size;
    int store = m[0] == 's';
    need(rt, O_REG);
    need(mem, O_MEM);
    if (streq(m, n, "ldrb")) { op = 0x39400000; size = 1; }
    else if (streq(m, n, "strb")) { op = 0x39000000; size = 1; }
    else if (streq(m, n, "ldrsb")) { op = rt->cls == 'x' ? 0x39800000 : 0x39c00000; size = 1; }
    else if (streq(m, n, "ldrh")) { op = 0x79400000; size = 2; }
    else if (streq(m, n, "strh")) { op = 0x79000000; size = 2; }
    else if (streq(m, n, "ldrsh")) { op = rt->cls == 'x' ? 0x79800000 : 0x79c00000; size = 2; }
    else if (streq(m, n, "ldrsw")) { op = 0xb9800000; size = 4; }
    else {
        switch (rt->cls) {
        case 'x': op = store ? 0xf9000000 : 0xf9400000; size = 8; break;
        case 'w': op = store ? 0xb9000000 : 0xb9400000; size = 4; break;
        case 'd': op = store ? 0xfd000000 : 0xfd400000; size = 8; break;
        default: op = store ? 0xbd000000 : 0xbd400000; size = 4; break;
        }
    }
    if (mem->has_sym) {
        if (mem->symkind != SK_PAGEOFF)
            as_error("expected sym@PAGEOFF");
        obj_add_reloc(ao, sec, (u32)ao->sec[sec].len, R_PAGEOFF12, get_sym(mem->sym), mem->imm);
        put(op | ((u32)mem->reg << 5) | rt->reg);
    } else if (mem->idx != 0xff) {
        put((op & ~0x01000000U) | 0x00206800 | ((u32)mem->idx << 16) | ((u32)mem->reg << 5) | rt->reg);
    } else if (mem->mode) {
        if (mem->imm < -256 || mem->imm > 255)
            as_error("index offset out of range");
        put((op & ~0x01000000U) | (mem->mode == 1 ? 0xc00U : 0x400U) | (((u32)mem->imm & 0x1ff) << 12) |
            ((u32)mem->reg << 5) | rt->reg);
    } else if (mem->imm >= 0 && mem->imm % size == 0 && mem->imm / size < 4096) {
        put(op | ((u32)(mem->imm / size) << 10) | ((u32)mem->reg << 5) | rt->reg);
    } else if (mem->imm >= -256 && mem->imm <= 255) {
        put((op & ~0x01000000U) | (((u32)mem->imm & 0x1ff) << 12) | ((u32)mem->reg << 5) | rt->reg);
    } else {
        as_error("offset out of range");
    }
}

static void instruction(const char *m, size_t n)
{
    Operand o[4];
    int nops = 0, cc;
    u32 sf, sh;

    memset(o, 0, sizeof o);
    /* b.cond */
    if (n == 4 && m[0] == 'b' && m[1] == '.' && (cc = cond_code(m + 2, 2)) >= 0) {
        parse_operand(&o[0]);
        need(&o[0], O_SYM);
        add_fix(o[0].sym, 1);
        put(0x54000000 | (u32)cc);
        return;
    }
    skip_ws();
    while (*p && nops < 4) {
        /* conditions and shifts are parsed by the instructions that take them */
        if (nops == 3 && (streq(m, n, "csel") || streq(m, n, "csinc")))
            break;
        if (nops == 1 && streq(m, n, "cset"))
            break;
        parse_operand(&o[nops++]);
        if (nops >= 2 && (streq(m, n, "movz") || streq(m, n, "movk") || streq(m, n, "movn")))
            break;
        if (nops == 3 && (streq(m, n, "add") || streq(m, n, "sub") || streq(m, n, "adds") || streq(m, n, "subs")))
            break;
        {
            const char *save = p;
            if (!accept_ch(','))
                break;
            skip_ws();
            if (strncmp(p, "lsl", 3) == 0 && !is_ident(p[3])) {
                p = save;
                break;
            }
        }
    }
    sf = o[0].kind == O_REG && (o[0].cls == 'x' || o[0].cls == 'd') ? 0x80000000U : 0;

#define R(k) ((u32)o[k].reg)
    if (streq(m, n, "nop")) {
        put(0xd503201f);
    } else if (streq(m, n, "ret")) {
        put(0xd65f0000 | ((nops ? R(0) : 30) << 5));
    } else if (streq(m, n, "svc")) {
        need(&o[0], O_IMM);
        put(0xd4000001 | ((u32)o[0].imm << 5));
    } else if (streq(m, n, "brk")) {
        put(0xd4200000 | ((u32)o[0].imm << 5));
    } else if (streq(m, n, "b") || streq(m, n, "bl")) {
        need(&o[0], O_SYM);
        add_fix(o[0].sym, 0);
        put(n == 1 ? 0x14000000 : 0x94000000);
    } else if (streq(m, n, "br")) {
        put(0xd61f0000 | (R(0) << 5));
    } else if (streq(m, n, "blr")) {
        put(0xd63f0000 | (R(0) << 5));
    } else if (streq(m, n, "cbz") || streq(m, n, "cbnz")) {
        need(&o[1], O_SYM);
        add_fix(o[1].sym, 1);
        put(sf | (n == 3 ? 0x34000000U : 0x35000000U) | R(0));
    } else if (streq(m, n, "tbz") || streq(m, n, "tbnz")) {
        need(&o[1], O_IMM);
        need(&o[2], O_SYM);
        add_fix(o[2].sym, 2);
        put((n == 3 ? 0x36000000U : 0x37000000U) | (((u32)o[1].imm >> 5) << 31) | (((u32)o[1].imm & 31) << 19) | R(0));
    } else if (streq(m, n, "mov")) {
        if (o[1].kind == O_IMM) {
            u64 v = (u64)o[1].imm;
            u32 enc;
            if (!sf)
                v &= 0xffffffffUL;
            if (v < 65536)
                put(sf | 0x52800000 | ((u32)v << 5) | R(0));
            else if ((sf ? ~v : (~v & 0xffffffffUL)) < 65536)
                put(sf | 0x12800000 | ((u32)(~v & 0xffff) << 5) | R(0));
            else if ((v & 0xffff) == 0 && v >> 16 < 65536)
                put(sf | 0x52a00000 | ((u32)(v >> 16) << 5) | R(0));
            else if (arm64_logical_imm(v, sf != 0, &enc))
                put(sf | 0x320003e0 | (enc << 10) | R(0));
            else
                as_error("immediate needs movz/movk");
        } else if (o[0].sp || o[1].sp) {
            put(sf | 0x11000000 | (R(1) << 5) | R(0));
        } else if (o[0].cls == 'd' || o[0].cls == 's') {
            put((o[0].cls == 'd' ? 0x1e604000U : 0x1e204000U) | (R(1) << 5) | R(0));
        } else {
            put(sf | 0x2a0003e0 | (R(1) << 16) | R(0));
        }
    } else if (streq(m, n, "movz") || streq(m, n, "movk") || streq(m, n, "movn")) {
        u32 op = m[3] == 'z' ? 0x52800000 : m[3] == 'k' ? 0x72800000 : 0x12800000;
        need(&o[1], O_IMM);
        sh = parse_shift();
        put(sf | op | ((sh / 16) << 21) | (((u32)o[1].imm & 0xffff) << 5) | R(0));
    } else if (streq(m, n, "add") || streq(m, n, "sub") || streq(m, n, "adds") || streq(m, n, "subs") ||
               streq(m, n, "cmp") || streq(m, n, "cmn")) {
        int sub = m[0] == 's' || streq(m, n, "cmp"), flags = n == 4 || m[0] == 'c';
        Operand *rd = &o[0], *rn = &o[1], *rm = &o[2], zr;
        if (m[0] == 'c') {
            memset(&zr, 0, sizeof zr);
            zr.kind = O_REG;
            zr.reg = 31;
            rm = &o[1];
            rn = &o[0];
            rd = &zr;
        }
        sh = parse_shift();
        if (rm->kind == O_IMM) {
            u64 v = (u64)rm->imm;
            if (rm->imm < 0) {
                v = 0 - v;
                sub = !sub;
            }
            if (sh == 12 || (v >= 4096 && (v & 0xfff) == 0)) {
                if (sh != 12)
                    v >>= 12;
                sh = 1;
            } else {
                sh = 0;
            }
            if (v >= 4096)
                as_error("immediate out of range");
            put(sf | (sub ? 0x40000000U : 0) | (flags ? 0x20000000U : 0) | 0x11000000 | (sh << 22) | ((u32)v << 10) |
                ((u32)rn->reg << 5) | rd->reg);
        } else if (rm->kind == O_SYM) {
            if (rm->symkind != SK_PAGEOFF)
                as_error("expected sym@PAGEOFF");
            obj_add_reloc(ao, sec, (u32)ao->sec[sec].len, R_PAGEOFF12, get_sym(rm->sym), rm->imm);
            put(0x91000000 | ((u32)rn->reg << 5) | rd->reg);
        } else if (rn->sp || rd->sp) {
            put(sf | (sub ? 0x40000000U : 0) | (flags ? 0x20000000U : 0) | 0x0b206000 | ((u32)rm->reg << 16) |
                ((u32)rn->reg << 5) | rd->reg);
        } else {
            put(sf | (sub ? 0x40000000U : 0) | (flags ? 0x20000000U : 0) | 0x0b000000 | ((u32)rm->reg << 16) |
                (sh << 10) | ((u32)rn->reg << 5) | rd->reg);
        }
    } else if (streq(m, n, "neg")) {
        put(sf | 0x4b0003e0 | (R(1) << 16) | R(0));
    } else if (streq(m, n, "mvn")) {
        put(sf | 0x2a2003e0 | (R(1) << 16) | R(0));
    } else if (streq(m, n, "and") || streq(m, n, "orr") || streq(m, n, "eor") || streq(m, n, "ands") ||
               streq(m, n, "bic") || streq(m, n, "tst")) {
        u32 op = streq(m, n, "and") ? 0x0a000000 : streq(m, n, "orr") ? 0x2a000000 : streq(m, n, "eor") ? 0x4a000000 :
                 streq(m, n, "bic") ? 0x0a200000 : 0x6a000000;
        u32 rd = R(0), rn = R(1);
        Operand *rm = &o[2];
        if (m[0] == 't') {
            rd = 31;
            rn = R(0);
            rm = &o[1];
        }
        if (rm->kind == O_IMM) {
            u32 enc = 0;
            if (streq(m, n, "bic") || !arm64_logical_imm((u64)rm->imm, sf != 0, &enc))
                as_error("immediate cannot be encoded");
            put(sf | ((op & 0x60000000U) | 0x12000000) | (enc << 10) | (rn << 5) | rd);
        } else {
            put(sf | op | ((u32)rm->reg << 16) | (rn << 5) | rd);
        }
    } else if (streq(m, n, "mul")) {
        put(sf | 0x1b007c00 | (R(2) << 16) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "madd") || streq(m, n, "msub")) {
        put(sf | (m[1] == 'a' ? 0x1b000000U : 0x1b008000U) | (R(2) << 16) | (R(3) << 10) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "sdiv") || streq(m, n, "udiv")) {
        put(sf | (m[0] == 's' ? 0x1ac00c00U : 0x1ac00800U) | (R(2) << 16) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "lsl") || streq(m, n, "lsr") || streq(m, n, "asr")) {
        if (o[2].kind == O_IMM) {
            u32 bits = sf ? 64 : 32, s = (u32)o[2].imm, immr, imms;
            u32 op = m[0] == 'a' ? 0x13000000 : 0x53000000;
            if (sf)
                op |= 0x80400000U;
            if (m[2] == 'l') {
                immr = (bits - s) % bits;
                imms = bits - 1 - s;
            } else {
                immr = s;
                imms = bits - 1;
            }
            put(op | (immr << 16) | (imms << 10) | (R(1) << 5) | R(0));
        } else {
            u32 op = m[2] == 'l' ? 0x1ac02000 : m[0] == 'l' ? 0x1ac02400 : 0x1ac02800;
            put(sf | op | (R(2) << 16) | (R(1) << 5) | R(0));
        }
    } else if (streq(m, n, "sxtw")) {
        put(0x93407c00 | (R(1) << 5) | R(0));
    } else if (streq(m, n, "sxtb") || streq(m, n, "sxth")) {
        put((sf ? 0x93400000U : 0x13000000U) | (m[3] == 'b' ? 0x1c00U : 0x3c00U) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "uxtb") || streq(m, n, "uxth")) {
        put(0x53000000 | (m[3] == 'b' ? 0x1c00U : 0x3c00U) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "csel") || streq(m, n, "csinc")) {
        cc = parse_cond();
        put(sf | (n == 4 ? 0x1a800000U : 0x1a800400U) | (R(2) << 16) | ((u32)cc << 12) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "cset")) {
        cc = parse_cond();
        put(sf | 0x1a9f07e0 | ((u32)(cc ^ 1) << 12) | R(0));
    } else if (streq(m, n, "ldp") || streq(m, n, "stp")) {
        u32 op, scale = 8;
        i64 imm;
        need(&o[2], O_MEM);
        if (o[0].cls == 'x')
            op = 0xa9000000;
        else if (o[0].cls == 'd')
            op = 0x6d000000;
        else {
            op = o[0].cls == 'w' ? 0x29000000 : 0x2d000000;
            scale = 4;
        }
        if (m[0] == 'l')
            op |= 0x00400000;
        if (o[2].mode == 1)
            op |= 0x00800000;
        else if (o[2].mode == 2)
            op = (op & ~0x01000000U) | 0x00800000;
        imm = o[2].imm / scale;
        if (o[2].imm % scale || imm < -64 || imm > 63)
            as_error("pair offset out of range");
        put(op | (((u32)imm & 0x7f) << 15) | (R(1) << 10) | (R(2) << 5) | R(0));
    } else if (n >= 3 && (memcmp(m, "ldr", 3) == 0 || memcmp(m, "str", 3) == 0)) {
        load_store(m, n, &o[0], &o[1]);
    } else if (streq(m, n, "adrp")) {
        need(&o[1], O_SYM);
        obj_add_reloc(ao, sec, (u32)ao->sec[sec].len, R_PAGE21, get_sym(o[1].sym), o[1].imm);
        put(0x90000000 | R(0));
    } else if (streq(m, n, "adr")) {
        need(&o[1], O_SYM);
        add_fix(o[1].sym, 3);
        put(0x10000000 | R(0));
    } else if (streq(m, n, "fadd") || streq(m, n, "fsub") || streq(m, n, "fmul") || streq(m, n, "fdiv")) {
        u32 op = m[1] == 'a' ? 0x1e202800 : m[1] == 's' ? 0x1e203800 : m[1] == 'm' ? 0x1e200800 : 0x1e201800;
        put(op | (o[0].cls == 'd' ? 0x00400000U : 0) | (R(2) << 16) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "fneg") || streq(m, n, "fabs") || streq(m, n, "fsqrt")) {
        u32 op = m[1] == 'n' ? 0x1e214000 : m[1] == 'a' ? 0x1e20c000 : 0x1e21c000;
        put(op | (o[0].cls == 'd' ? 0x00400000U : 0) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "fcmp")) {
        put(0x1e202000 | (o[0].cls == 'd' ? 0x00400000U : 0) | (R(1) << 16) | (R(0) << 5));
    } else if (streq(m, n, "fmov")) {
        int fd = o[0].cls == 'd' || o[0].cls == 's', fs = o[1].cls == 'd' || o[1].cls == 's';
        if (fd && fs)
            put((o[0].cls == 'd' ? 0x1e604000U : 0x1e204000U) | (R(1) << 5) | R(0));
        else if (fd)
            put((o[0].cls == 'd' ? 0x9e670000U : 0x1e270000U) | (R(1) << 5) | R(0));
        else
            put((o[1].cls == 'd' ? 0x9e660000U : 0x1e260000U) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "scvtf") || streq(m, n, "ucvtf")) {
        put((o[1].cls == 'x' ? 0x80000000U : 0) | (m[0] == 's' ? 0x1e220000U : 0x1e230000U) |
            (o[0].cls == 'd' ? 0x00400000U : 0) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "fcvtzs") || streq(m, n, "fcvtzu")) {
        put((o[0].cls == 'x' ? 0x80000000U : 0) | (m[5] == 's' ? 0x1e380000U : 0x1e390000U) |
            (o[1].cls == 'd' ? 0x00400000U : 0) | (R(1) << 5) | R(0));
    } else if (streq(m, n, "fcvt")) {
        put((o[0].cls == 'd' ? 0x1e22c000U : 0x1e624000U) | (R(1) << 5) | R(0));
    } else {
        char msg[80];
        sprintf(msg, "unknown instruction '%.*s'", (int)(n < 40 ? n : 40), m);
        as_error(msg);
    }
#undef R
    skip_ws();
    if (*p)
        as_error("unexpected text after instruction");
}

static void string_data(int zero)
{
    Buf *b = &ao->sec[sec];
    expect_ch('"');
    while (*p && *p != '"') {
        int c = (u8)*p++;
        if (c == '\\') {
            c = (u8)*p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = 0; break;
            }
        }
        buf_u8(b, (u32)c);
    }
    expect_ch('"');
    if (zero)
        buf_u8(b, 0);
}

static void directive(const char *m, size_t n)
{
    Buf *b = &ao->sec[sec];
    if (streq(m, n, ".text")) {
        sec = SEC_TEXT;
    } else if (streq(m, n, ".data")) {
        sec = SEC_DATA;
    } else if (streq(m, n, ".const") || streq(m, n, ".rodata") || streq(m, n, ".cstring")) {
        sec = SEC_CONST;
    } else if (streq(m, n, ".section")) {
        skip_ws();
        sec = strncmp(p, "__TEXT,__text", 13) == 0 ? SEC_TEXT : strncmp(p, "__TEXT", 6) == 0 ? SEC_CONST : SEC_DATA;
        p += strlen(p);
    } else if (streq(m, n, ".globl") || streq(m, n, ".global")) {
        Operand o;
        u32 s;
        parse_operand(&o);
        need(&o, O_SYM);
        s = get_sym(o.sym); /* may move ao->syms */
        ao->syms[s].global = 1;
    } else if (streq(m, n, ".align") || streq(m, n, ".p2align") || streq(m, n, ".balign")) {
        u32 a = (u32)const_expr();
        if (m[1] != 'b')
            a = (u32)1 << a;
        if (a > ao->align[sec])
            ao->align[sec] = a;
        if (sec == SEC_BSS)
            b->len = (size_t)align_up(b->len, a);
        else if (sec == SEC_TEXT && a >= 4)
            while (b->len % a)
                buf_u32(b, 0xd503201f);
        else
            buf_align(b, a);
    } else if (streq(m, n, ".quad") || streq(m, n, ".long") || streq(m, n, ".word") || streq(m, n, ".short") ||
               streq(m, n, ".byte")) {
        u32 size = m[1] == 'q' ? 8 : m[1] == 'l' || m[1] == 'w' ? 4 : m[1] == 's' ? 2 : 1, k;
        do {
            Operand o;
            parse_operand(&o);
            if (o.kind == O_SYM) {
                if (size != 8)
                    as_error("symbol values need .quad");
                obj_add_reloc(ao, sec, (u32)b->len, R_ABS64, get_sym(o.sym), o.imm);
                buf_u64(b, 0);
            } else {
                need(&o, O_IMM);
                for (k = 0; k < size; k++)
                    buf_u8(b, (u32)((u64)o.imm >> (8 * k)));
            }
        } while (accept_ch(','));
    } else if (streq(m, n, ".ascii")) {
        string_data(0);
    } else if (streq(m, n, ".asciz") || streq(m, n, ".string")) {
        string_data(1);
    } else if (streq(m, n, ".zero") || streq(m, n, ".space") || streq(m, n, ".skip")) {
        buf_zero(b, (size_t)const_expr());
    } else if (streq(m, n, ".subsections_via_symbols") || streq(m, n, ".build_version") || streq(m, n, ".file")) {
        p += strlen(p);
    } else {
        as_error("unknown directive");
    }
}

Obj *as_file(const char *path)
{
    size_t len;
    char *src = (char *)os_read_file(path, &len), *line, *nl;
    u32 k;

    if (!src)
        die("cannot read %s", path);
    as_path = path;
    as_line = 0;
    ao = obj_new(path);
    sec = SEC_TEXT;
    ao->align[SEC_TEXT] = 4;
    if (sym_of)
        memset(sym_of, 0, sym_cap * sizeof(u32));
    nfixes = 0;

    for (line = src; line < src + len; line = nl + 1) {
        char *c;
        const char *m;
        size_t n;
        int in_str = 0;
        as_line++;
        nl = strchr(line, '\n');
        if (!nl)
            nl = src + len;
        *nl = 0;
        /* strip comments: "//" and ";" */
        for (c = line; *c; c++) {
            if (*c == '"')
                in_str = !in_str;
            if (!in_str && (*c == ';' || (c[0] == '/' && c[1] == '/'))) {
                *c = 0;
                break;
            }
        }
        p = line;
        for (;;) {
            skip_ws();
            m = p;
            while (is_ident(*p))
                p++;
            n = (size_t)(p - m);
            if (n && *p == ':') {
                u32 s = get_sym(intern(m, n));
                if (ao->syms[s].sec != SEC_UNDEF)
                    as_error("label defined twice");
                ao->syms[s].sec = (u16)sec;
                ao->syms[s].value = ao->sec[sec].len;
                p++;
                continue;
            }
            break;
        }
        if (n == 0) {
            skip_ws();
            if (*p)
                as_error("syntax error");
            continue;
        }
        if (m[0] == '.')
            directive(m, n);
        else
            instruction(m, n);
    }

    /* branches to labels of this file are resolved now; others become relocations */
    for (k = 0; k < nfixes; k++) {
        AFix *fx = &fixes[k];
        u32 s = get_sym(fx->name);
        ObjSym *sy = &ao->syms[s];
        u8 *at = ao->sec[fx->sec].p + fx->off;
        u32 insn = get_u32le(at);
        as_line = fx->line;
        if (sy->sec == fx->sec) {
            i64 d = (i64)sy->value - (i64)fx->off;
            switch (fx->kind) {
            case 0: insn |= (u32)(d >> 2) & 0x3ffffff; break;
            case 1: insn |= ((u32)(d >> 2) & 0x7ffff) << 5; break;
            case 2: insn |= ((u32)(d >> 2) & 0x3fff) << 5; break;
            default: insn |= (((u32)d & 3) << 29) | ((((u32)d >> 2) & 0x7ffff) << 5); break;
            }
            put_u32le(at, insn);
        } else if (fx->kind == 0) {
            obj_add_reloc(ao, (int)fx->sec, fx->off, R_CALL26, s, 0);
        } else {
            as_error("branch target must be a label in the same section");
        }
    }
    /* undefined symbols are references to other objects */
    for (k = 0; k < ao->nsyms; k++)
        if (ao->syms[k].sec == SEC_UNDEF)
            ao->syms[k].global = 1;
    free(src);
    return ao;
}

int as_main(int argc, char **argv)
{
    const char *out_path = 0, *in = 0;
    Buf out;
    int i;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (argv[i][0] == '-')
            die("as: unknown option %s", argv[i]);
        else
            in = argv[i];
    }
    if (!in)
        die("as: no input file");
    memset(&out, 0, sizeof out);
    macho_write_obj(&out, as_file(in));
    os_write_file(out_path ? out_path : "a.o", out.p, out.len, 0);
    return 0;
}
