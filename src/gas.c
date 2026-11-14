/* GNU-syntax assembler for arm64 ELF targets: whole files (already
   preprocessed) and the inline assembly of the compiler. One pass; what
   cannot be settled when it is read is patched at the end or becomes a
   relocation. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Val {
    u32 add, sub; /* object symbols + 1, or 0 */
    i64 k;
} Val;

enum { FX_D1, FX_D2, FX_D4, FX_D8, FX_B26, FX_BR19, FX_TB14, FX_ADR, FX_ADRP, FX_LO12, FX_LIT19,
       FX_XPC32, FX_XPLT32, FX_XPC8, FX_X32, FX_X32S, FX_XTPOFF /* x86-64: see x64_asm.h */ };

typedef struct Fix {
    Val v;
    u32 sec, off, line;
    u8 kind;
} Fix;

typedef struct Src {
    char *text;
    size_t pos, len;
    struct Src *up;
    u32 ncond;  /* conditional depth when it was entered */
    int ismacro;
} Src;

#define MAXPARAM 16
typedef struct Macro {
    u32 name;
    char *body;
    u32 nparams;
    u32 pname[MAXPARAM];
    char *pdef[MAXPARAM];
    u8 vararg;
    struct Macro *next;
} Macro;

typedef struct Ext { /* per interned name */
    u32 lcount; /* numeric local label: how many are defined */
    u8 isabs;
    i64 val;
} Ext;

typedef struct Reg {
    u8 cls; /* 'x', 'w', or 'b' 'h' 's' 'd' 'q' */
    u8 n;
    u8 sp;
} Reg;

typedef struct Alias {
    u32 name;
    Reg r;
} Alias;

typedef struct Lit {
    u32 sec, sym, size;
    Val v;
} Lit;

typedef struct Mem {
    u8 base, kind /* 0 offset, 1 register index, 2 :lo12: */, pre, idx, ext, hasamt;
    i64 amt;
    Val off;
} Mem;

static Obj *go;
static const char *as_path;
static u32 as_line;
static char *p;
static u32 cur, prev;
static u32 secstack[32][2], nsecstack;
static u32 subsec[64][3], nsubsec; /* base section, number, section */
static Src *src;
static u8 cond[64];
static u32 ncond;
static Macro *macros;
static u32 macro_serial;
static Ext *ext;
static u32 capext;
static Fix *fixes;
static u32 nfixes, capfixes;
static Alias *aliases;
static u32 naliases, capaliases;
static Lit *lits;
static u32 nlits, caplits;
static char *ops[10];
static int nops;
static u32 dot_sym, dot_sec, dot_off;

static void err(const char *msg, const char *arg)
{
    fprintf(stderr, "%s:%u: error: assembler: %s%s%s\n", as_path, as_line, msg, arg ? " " : "", arg ? arg : "");
    exit(1);
}

static int is_ident(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '$';
}

static void skip_ws(void)
{
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
}

static int accept_ch(int c)
{
    skip_ws();
    if (*p != c)
        return 0;
    p++;
    return 1;
}

static Ext *ext_of(u32 name)
{
    if (name >= capext) {
        u32 cap = capext ? capext : 4096;
        while (cap <= name)
            cap *= 2;
        ext = (Ext *)xrealloc(ext, cap * sizeof(Ext));
        memset(ext + capext, 0, (cap - capext) * sizeof(Ext));
        capext = cap;
    }
    return &ext[name];
}

static Buf *B(void)
{
    return &go->sec[cur];
}

static u32 here(void)
{
    return (u32)go->sec[cur].len;
}

/* ---- expressions ---- */

static Val expr(void);

static u32 local_label(const char *digits, size_t n, u32 count)
{
    char buf[64];
    if (n > 20)
        err("bad label", 0);
    sprintf(buf, ".L%.*s\001%u", (int)n, digits, count);
    return intern_cstr(buf);
}

static int defined(u32 s)
{
    return s && go->syms[s - 1].sec < 0xfff0;
}

static void simplify(Val *v)
{
    if (v->add && v->add == v->sub) {
        v->add = v->sub = 0;
    } else if (defined(v->add) && defined(v->sub) && go->syms[v->add - 1].sec == go->syms[v->sub - 1].sec) {
        v->k += (i64)go->syms[v->add - 1].value - (i64)go->syms[v->sub - 1].value;
        v->add = v->sub = 0;
    }
}

static Val sym_val(u32 name)
{
    Val v;
    Ext *e = ext_of(name);
    v.add = v.sub = 0;
    v.k = 0;
    if (e->isabs)
        v.k = e->val;
    else
        v.add = obj_named_sym(go, name) + 1;
    return v;
}

static Val primary(void)
{
    Val v;
    char *s;
    v.add = v.sub = 0;
    v.k = 0;
    skip_ws();
    if (*p == '(') {
        p++;
        v = expr();
        if (!accept_ch(')'))
            err("expected ')'", 0);
    } else if (*p == '-') {
        p++;
        v = primary();
        if (v.add && v.sub)
            err("invalid expression", 0);
        {
            u32 t = v.add;
            v.add = v.sub;
            v.sub = t;
        }
        v.k = -v.k;
    } else if (*p == '+') {
        p++;
        v = primary();
    } else if (*p == '~' || *p == '!') {
        int c = *p++;
        v = primary();
        if (v.add || v.sub)
            err("invalid expression", 0);
        v.k = c == '~' ? ~v.k : !v.k;
    } else if (*p == '\'') {
        v.k = (u8)p[1];
        p += 2;
        if (*p == '\'')
            p++;
    } else if (*p >= '0' && *p <= '9') {
        u64 n = 0;
        int base = 10, d;
        s = p;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
            base = 2;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
        }
        for (;; p++) {
            d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
                *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 99;
            if (base == 10 && (*p == 'b' || *p == 'f'))
                break;
            if (base == 8 && (*p == 'b' || *p == 'f' || d == 8 || d == 9)) {
                /* "0b", "0f", or a decimal label number with a leading zero */
                base = 10;
                p = s;
                n = 0;
                while (*p >= '0' && *p <= '9')
                    n = n * 10 + (u64)(*p++ - '0');
                break;
            }
            if (d >= base)
                break;
            n = n * (u64)base + (u64)d;
        }
        if ((*p == 'b' || *p == 'f') && !is_ident(p[1]) && base == 10) {
            u32 name = intern(s, (size_t)(p - s));
            u32 c = ext_of(name)->lcount + (*p == 'f');
            if (!c)
                err("no such label before here:", intern_str(name));
            v.add = obj_named_sym(go, local_label(s, (size_t)(p - s), c)) + 1;
            p++;
        } else {
            while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L')
                p++;
            v.k = (i64)n;
        }
    } else if (*p == '.' && !is_ident(p[1])) {
        p++;
        if (!dot_sym || dot_sec != cur || dot_off != here()) {
            dot_sym = obj_add_sym(go, 0, (int)cur, here(), 0) + 1;
            dot_sec = cur;
            dot_off = here();
        }
        v.add = dot_sym;
    } else if (is_ident(*p)) {
        s = p;
        while (is_ident(*p))
            p++;
        v = sym_val(intern(s, (size_t)(p - s)));
    } else {
        err("expected an expression at", p);
    }
    return v;
}

static int binop(int *len)
{
    skip_ws();
    *len = 1;
    switch (*p) {
    case '*': case '/': case '%': return 6 << 8 | *p;
    case '+': case '-': return 4 << 8 | *p;
    case '^': return 5 << 8 | *p;
    case '|':
        if (p[1] == '|') { *len = 2; return 1 << 8 | 'o'; }
        return 5 << 8 | *p;
    case '&':
        if (p[1] == '&') { *len = 2; return 2 << 8 | 'a'; }
        return 5 << 8 | *p;
    case '<':
        if (p[1] == '<') { *len = 2; return 6 << 8 | 'l'; }
        if (p[1] == '=') { *len = 2; return 3 << 8 | 'L'; }
        if (p[1] == '>') { *len = 2; return 3 << 8 | 'n'; }
        return 3 << 8 | '<';
    case '>':
        if (p[1] == '>') { *len = 2; return 6 << 8 | 'r'; }
        if (p[1] == '=') { *len = 2; return 3 << 8 | 'G'; }
        return 3 << 8 | '>';
    case '=':
        if (p[1] == '=') { *len = 2; return 3 << 8 | '='; }
        return 0;
    case '!':
        if (p[1] == '=') { *len = 2; return 3 << 8 | 'n'; }
        return 0;
    }
    return 0;
}

static Val expr_prec(int min)
{
    Val a = primary(), b;
    int op, len;
    for (;;) {
        op = binop(&len);
        if (!op || op >> 8 < min)
            return a;
        p += len;
        b = expr_prec((op >> 8) + 1);
        op &= 0xff;
        if (op == '+') {
            if ((a.add && b.add) || (a.sub && b.sub))
                err("invalid expression", 0);
            a.add |= b.add;
            a.sub |= b.sub;
            a.k += b.k;
            simplify(&a);
            continue;
        }
        if (op == '-') {
            if ((a.add && b.sub) || (a.sub && b.add))
                err("invalid expression", 0);
            a.add |= b.sub;
            a.sub |= b.add;
            a.k -= b.k;
            simplify(&a);
            continue;
        }
        if (a.add || a.sub || b.add || b.sub)
            err("expression is not constant", 0);
        switch (op) {
        case '*': a.k *= b.k; break;
        case '/':
        case '%':
            if (!b.k)
                err("division by zero", 0);
            a.k = op == '/' ? a.k / b.k : a.k % b.k;
            break;
        case 'l': a.k = (i64)((u64)a.k << (b.k & 63)); break;
        case 'r': a.k = (i64)((u64)a.k >> (b.k & 63)); break;
        case '|': a.k |= b.k; break;
        case '&': a.k &= b.k; break;
        case '^': a.k ^= b.k; break;
        case '=': a.k = -(i64)(a.k == b.k); break;
        case 'n': a.k = -(i64)(a.k != b.k); break;
        case '<': a.k = -(i64)(a.k < b.k); break;
        case '>': a.k = -(i64)(a.k > b.k); break;
        case 'L': a.k = -(i64)(a.k <= b.k); break;
        case 'G': a.k = -(i64)(a.k >= b.k); break;
        case 'a': a.k = a.k && b.k; break;
        case 'o': a.k = a.k || b.k; break;
        }
    }
}

static Val expr(void)
{
    return expr_prec(1);
}

static i64 const_expr(void)
{
    Val v = expr();
    if (v.add || v.sub)
        err("expression is not constant", 0);
    return v.k;
}

/* ---- fixups ---- */

static void patch_rel(u8 *at, int kind, i64 d)
{
    u32 insn = get_u32le(at);
    i64 lim = kind == FX_B26 ? (i64)1 << 27 : kind == FX_TB14 ? 1 << 15 : 1 << 20;
    if (d < -lim || d >= lim || (kind != FX_ADR && (d & 3)))
        err("branch target out of range", 0);
    switch (kind) {
    case FX_B26: insn |= (u32)(d >> 2) & 0x3ffffff; break;
    case FX_BR19:
    case FX_LIT19: insn |= ((u32)(d >> 2) & 0x7ffff) << 5; break;
    case FX_TB14: insn |= ((u32)(d >> 2) & 0x3fff) << 5; break;
    default: insn |= (((u32)d & 3) << 29) | ((((u32)d >> 2) & 0x7ffff) << 5); break;
    }
    put_u32le(at, insn);
}

static int x64_settle(Fix *fx, Val v, u8 *at);

/* Settles one fixup if that can be done yet; at the end, everything must be. */
static int settle(Fix *fx, int final)
{
    Val v = fx->v;
    ObjSym *a, *s;
    u8 *at = go->sec[fx->sec].p + fx->off;
    u32 size = fx->kind <= FX_D8 ? (u32)1 << fx->kind : 4, k;
    int type;

    if (v.add && !defined(v.add) && go->syms[v.add - 1].name && ext_of(go->syms[v.add - 1].name)->isabs) {
        v.k += ext[go->syms[v.add - 1].name].val;
        v.add = 0;
    }
    if (v.sub && !defined(v.sub) && go->syms[v.sub - 1].name && ext_of(go->syms[v.sub - 1].name)->isabs) {
        v.k -= ext[go->syms[v.sub - 1].name].val;
        v.sub = 0;
    }
    simplify(&v);
    a = v.add ? &go->syms[v.add - 1] : 0;
    s = v.sub ? &go->syms[v.sub - 1] : 0;
    if (!a && !s) {
        if (fx->kind > FX_D8)
            err("the target is a number, not a label", 0);
        for (k = 0; k < size; k++)
            at[k] = (u8)((u64)v.k >> (8 * k));
        return 1;
    }
    if (!final && ((a && !defined(v.add)) || (s && !defined(v.sub))))
        return 0;
    if (a && !defined(v.add)) {
        const char *n = intern_str(a->name);
        if (!a->name || (n[0] == '.' && n[1] == 'L')) {
            const char *e = strchr(n, 1);
            static char buf[64];
            sprintf(buf, "%.*s", e ? (int)(e - n - 2) : 40, e ? n + 2 : n);
            err("undefined label", buf);
        }
    }
    if (!a || (s && fx->kind > FX_D8))
        err("invalid expression", 0);
    if (fx->kind == FX_D1)
        err("a byte cannot hold an address", 0);
    if (fx->kind >= FX_XPC32)
        return x64_settle(fx, v, at);
    if (fx->kind <= FX_D8) {
        if (s) {
            if (!defined(v.sub) || s->sec != fx->sec)
                err("difference of symbols in different sections", 0);
            type = fx->kind == FX_D8 ? R_PREL64 : fx->kind == FX_D4 ? R_PREL32 : R_PREL16;
            v.k += (i64)fx->off - (i64)s->value;
        } else {
            type = fx->kind == FX_D8 ? R_ABS64 : fx->kind == FX_D4 ? R_ABS32 : R_ABS16;
        }
        obj_add_reloc(go, (int)fx->sec, fx->off, type, v.add - 1, v.k);
        return 1;
    }
    if (fx->kind == FX_ADRP || fx->kind == FX_LO12) {
        obj_add_reloc(go, (int)fx->sec, fx->off, fx->kind == FX_ADRP ? R_PAGE21 : R_PAGEOFF12, v.add - 1, v.k);
        return 1;
    }
    if (defined(v.add) && a->sec == fx->sec && !a->weak) {
        patch_rel(at, fx->kind, (i64)a->value + v.k - (i64)fx->off);
        return 1;
    }
    type = fx->kind == FX_B26 ? (get_u32le(at) >> 31 ? R_CALL26 : R_JUMP26) : fx->kind == FX_BR19 ? R_CONDBR19 :
           fx->kind == FX_TB14 ? R_TSTBR14 : fx->kind == FX_ADR ? R_ADR21 : R_LIT19;
    obj_add_reloc(go, (int)fx->sec, fx->off, type, v.add - 1, v.k);
    return 1;
}

/* The last `size` bytes written are to be filled from v. */
static void fixup(int kind, Val v)
{
    Fix fx;
    fx.v = v;
    fx.sec = cur;
    fx.off = here() - (kind <= FX_D8 ? (u32)1 << kind : 4);
    fx.line = as_line;
    fx.kind = (u8)kind;
    if (settle(&fx, 0))
        return;
    if (nfixes == capfixes) {
        capfixes = capfixes ? capfixes * 2 : 256;
        fixes = (Fix *)xrealloc(fixes, capfixes * sizeof(Fix));
    }
    fixes[nfixes++] = fx;
}

static void grow_sec(u64 n, int fill)
{
    Buf *b = B();
    if (go->secflags[cur] & SF_NOBITS) {
        b->len += (size_t)n;
    } else if (!fill) {
        buf_zero(b, (size_t)n);
    } else {
        while (n--)
            buf_u8(b, (u32)fill);
    }
}

static void data(u32 lg, Val v)
{
    if (go->secflags[cur] & SF_NOBITS) {
        B()->len += (size_t)1 << lg;
        return;
    }
    buf_zero(B(), (size_t)1 << lg);
    fixup((int)lg, v);
}

static void put(u32 insn)
{
    buf_u32(B(), insn);
}

static void put_fix(u32 insn, int kind, Val v)
{
    put(insn);
    fixup(kind, v);
}

/* ---- sections ---- */

static u32 sec_base(u32 s)
{
    u32 k;
    for (k = 0; k < nsubsec; k++)
        if (subsec[k][2] == s)
            return subsec[k][0];
    return s;
}

static u32 sec_sub(u32 base, u32 n)
{
    static const char *const std[] = {".text", ".rodata", ".data", ".bss"};
    u32 k;
    if (!n)
        return base;
    for (k = 0; k < nsubsec; k++)
        if (subsec[k][0] == base && subsec[k][1] == n)
            return subsec[k][2];
    if (nsubsec == 64)
        err("too many subsections", 0);
    subsec[nsubsec][0] = base;
    subsec[nsubsec][1] = n;
    k = (u32)obj_section_new(go, base < SEC_COUNT ? intern_cstr(std[base]) : go->secname[base], go->secflags[base]);
    go->align[k] = go->secflags[base] & SF_EXEC ? 4 : 1;
    subsec[nsubsec++][2] = k;
    return k;
}

static void switch_to(u32 s)
{
    prev = cur;
    cur = s;
}

/* name [, "flags" [, @type [, entry size]]]; with_sub: a subsection number may follow the name */
static void section_args(int with_sub)
{
    char *s, *e;
    u32 name, flags = 0, sub = 0, sec, entsize = 0;
    int has_flags = 0;
    skip_ws();
    if (*p == '"') {
        s = ++p;
        while (*p && *p != '"')
            p++;
        e = p;
        if (*p)
            p++;
    } else {
        s = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        e = p;
    }
    name = intern(s, (size_t)(e - s));
    if (accept_ch(',')) {
        skip_ws();
        if (with_sub && *p != '"') {
            sub = (u32)const_expr();
            if (!accept_ch(','))
                goto done;
            skip_ws();
        }
        if (*p == '"') {
            has_flags = 1;
            for (p++; *p && *p != '"'; p++) {
                switch (*p) {
                case 'a': flags |= SF_ALLOC; break;
                case 'w': flags |= SF_WRITE; break;
                case 'x': flags |= SF_EXEC; break;
                case 'M': flags |= SF_MERGE; break;
                case 'T': flags |= SF_TLS; break;
                case 'S': flags |= SF_STRINGS; break;
                }
            }
            if (*p)
                p++;
            if (accept_ch(',')) {
                skip_ws();
                if (*p == '@' || *p == '%') {
                    if (strncmp(p + 1, "nobits", 6) == 0)
                        flags |= SF_NOBITS;
                    while (*p && *p != ',')
                        p++;
                }
                if (accept_ch(','))
                    entsize = (u32)const_expr();
            }
        }
    }
done:
    s = (char *)intern_str(name);
    if (!has_flags) {
        if (strncmp(s, ".text", 5) == 0)
            flags = SF_ALLOC | SF_EXEC;
        else if (strncmp(s, ".data", 5) == 0)
            flags = SF_ALLOC | SF_WRITE;
        else if (strncmp(s, ".rodata", 7) == 0)
            flags = SF_ALLOC;
        else if (strncmp(s, ".bss", 4) == 0)
            flags = SF_ALLOC | SF_WRITE | SF_NOBITS;
    }
    if (strcmp(s, ".text") == 0)
        sec = SEC_TEXT;
    else if (strcmp(s, ".data") == 0)
        sec = SEC_DATA;
    else if (strcmp(s, ".rodata") == 0)
        sec = SEC_CONST;
    else if (strcmp(s, ".bss") == 0)
        sec = SEC_BSS;
    else
        sec = (u32)obj_section(go, name, flags);
    if (entsize)
        go->secent[sec] = entsize;
    switch_to(sec_sub(sec, sub));
    p += strlen(p);
}

/* ---- input ---- */

/* Blanks out comments, and lines that are preprocessor line markers. */
static void strip(char *t, int markers)
{
    int bol = 1;
    for (; *t; t++) {
        if (*t == '"') {
            for (t++; *t && *t != '"' && *t != '\n'; t++)
                if (*t == '\\' && t[1])
                    t++;
            if (!*t)
                break;
        } else if (*t == '\'' && t[1] && t[1] != '\n') {
            t++;
        } else if (t[0] == '/' && t[1] == '/') {
            while (*t && *t != '\n')
                *t++ = ' ';
        } else if (t[0] == '/' && t[1] == '*') {
            while (*t && !(t[0] == '*' && t[1] == '/')) {
                if (*t != '\n')
                    *t = ' ';
                t++;
            }
            if (*t)
                t[0] = t[1] = ' ';
        } else if (*t == '#' && bol && markers) {
            while (*t && *t != '\n')
                *t++ = ' ';
        }
        if (!*t)
            break;
        if (*t == '\n')
            bol = 1;
        else if (*t != ' ' && *t != '\t')
            bol = 0;
    }
}

static void push_src(char *text, int ismacro)
{
    Src *s = (Src *)xmalloc(sizeof(Src));
    s->text = text;
    s->pos = 0;
    s->len = strlen(text);
    s->up = src;
    s->ncond = ncond;
    s->ismacro = ismacro;
    src = s;
}

static void pop_src(void)
{
    Src *s = src;
    src = s->up;
    free(s->text);
    free(s);
}

/* The extent of the statement at s->pos; moves past it. */
static char *stmt_scan(Src *s, char **end)
{
    char *a = s->text + s->pos, *q;
    int str = 0;
    for (q = a; *q; q++) {
        if (*q == '"')
            str = !str;
        else if (*q == '\\' && str && q[1])
            q++;
        else if (*q == '\n' || (*q == ';' && !str))
            break;
    }
    if (*q == '\n' && !s->up)
        as_line++;
    s->pos = (size_t)(q - s->text) + (*q ? 1 : 0);
    *end = q;
    return a;
}

static char *stmt_next(void)
{
    char *a, *e;
    while (src) {
        if (src->pos >= src->len) {
            if (!src->up)
                return 0;
            pop_src();
            continue;
        }
        a = stmt_scan(src, &e);
        *e = 0;
        return a;
    }
    return 0;
}

static int word_is(const char *w, const char *lit)
{
    size_t n = strlen(lit);
    return strncmp(w, lit, n) == 0 && !is_ident(w[n]);
}

/* Copies the statements up to the directive that closes a macro (.endm)
   or a repeat block (.endr), which is consumed. */
static char *collect(int rept)
{
    Src *s = src;
    size_t start = s->pos, at;
    int depth = 1;
    char *w, *e, *body;
    for (;;) {
        at = s->pos;
        if (at >= s->len)
            err(rept ? "missing .endr" : "missing .endm", 0);
        w = stmt_scan(s, &e);
        while (*w == ' ' || *w == '\t')
            w++;
        if (rept ? word_is(w, ".rept") || word_is(w, ".irp") || word_is(w, ".irpc") : word_is(w, ".macro"))
            depth++;
        else if (word_is(w, rept ? ".endr" : ".endm") && --depth == 0)
            break;
    }
    body = (char *)xmalloc(at - start + 1);
    memcpy(body, s->text + start, at - start);
    body[at - start] = 0;
    return body;
}

/* body with each \name replaced by its value */
static char *substitute(const char *body, const u32 *names, char **vals, u32 n, u32 serial)
{
    Buf b;
    const char *q = body, *s;
    u32 k, name;
    char num[16];
    memset(&b, 0, sizeof b);
    while (*q) {
        if (*q != '\\') {
            buf_u8(&b, (u8)*q++);
            continue;
        }
        if (q[1] == '@') {
            sprintf(num, "%u", serial);
            buf_put(&b, num, strlen(num));
            q += 2;
            continue;
        }
        if (q[1] == '(' && q[2] == ')') {
            q += 3;
            continue;
        }
        s = q + 1;
        while (is_ident(*s) && *s != '.' && *s != '$')
            s++;
        name = s > q + 1 ? intern(q + 1, (size_t)(s - q - 1)) : 0;
        for (k = 0; k < n; k++)
            if (names[k] == name)
                break;
        if (k < n && name) {
            buf_put(&b, vals[k], strlen(vals[k]));
            q = s;
        } else {
            buf_u8(&b, (u8)*q++);
        }
    }
    buf_u8(&b, 0);
    return (char *)b.p;
}

static char *dup_range(const char *s, const char *e)
{
    char *r = (char *)xmalloc((size_t)(e - s) + 1);
    memcpy(r, s, (size_t)(e - s));
    r[e - s] = 0;
    return r;
}

static int is_opchar(int c)
{
    return c && strchr("+-*/%<>|&^!~=", c) != 0;
}

/* One macro argument at p: up to a comma, or to a blank that does not sit
   next to an operator. Quotes around a whole argument are dropped. */
static char *macro_arg(int rest)
{
    char *s, *e, *q;
    int depth = 0, str = 0;
    skip_ws();
    s = p;
    for (; *p; p++) {
        if (*p == '"')
            str = !str;
        else if (str)
            continue;
        else if (*p == '(' || *p == '[')
            depth++;
        else if (*p == ')' || *p == ']')
            depth--;
        else if (rest)
            continue;
        else if (*p == ',' && depth <= 0)
            break;
        else if ((*p == ' ' || *p == '\t') && depth <= 0) {
            q = p;
            while (*q == ' ' || *q == '\t')
                q++;
            if (!*q || *q == ',') {
                e = p;
                p = q;
                goto out;
            }
            if (!is_opchar(*q) && !is_opchar(p[-1])) {
                e = p;
                p = q;
                if (e - s >= 2 && *s == '"' && e[-1] == '"')
                    return dup_range(s + 1, e - 1);
                return dup_range(s, e);
            }
            p = q - 1;
        }
    }
    e = p;
out:
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    if (*p == ',')
        p++;
    if (e - s >= 2 && *s == '"' && e[-1] == '"')
        return dup_range(s + 1, e - 1);
    return dup_range(s, e);
}

static void expand(Macro *m)
{
    char *vals[MAXPARAM];
    u32 k, pos = 0;
    char *text;
    memset(vals, 0, sizeof vals);
    for (;;) {
        char *s, *a;
        skip_ws();
        if (!*p)
            break;
        /* name=value */
        s = p;
        while (is_ident(*s))
            s++;
        if (*s == '=' && s > p && s[1] != '=') {
            u32 name = intern(p, (size_t)(s - p));
            for (k = 0; k < m->nparams; k++)
                if (m->pname[k] == name)
                    break;
            if (k < m->nparams) {
                p = s + 1;
                a = macro_arg(m->vararg && k == m->nparams - 1);
                free(vals[k]);
                vals[k] = a;
                continue;
            }
        }
        if (pos >= m->nparams)
            err("too many arguments to macro", intern_str(m->name));
        a = macro_arg(m->vararg && pos == m->nparams - 1);
        vals[pos++] = a;
    }
    for (k = 0; k < m->nparams; k++)
        if (!vals[k])
            vals[k] = dup_range(m->pdef[k] ? m->pdef[k] : "", (m->pdef[k] ? m->pdef[k] : "") + (m->pdef[k] ? strlen(m->pdef[k]) : 0));
    text = substitute(m->body, m->pname, vals, m->nparams, macro_serial++);
    for (k = 0; k < m->nparams; k++)
        free(vals[k]);
    push_src(text, 1);
}

static void def_macro(void)
{
    Macro *m = (Macro *)xmalloc(sizeof(Macro));
    char *s;
    memset(m, 0, sizeof *m);
    skip_ws();
    s = p;
    while (is_ident(*p))
        p++;
    m->name = intern(s, (size_t)(p - s));
    for (;;) {
        skip_ws();
        if (*p == ',')
            p++;
        skip_ws();
        if (!*p)
            break;
        if (m->nparams == MAXPARAM)
            err("too many macro parameters", 0);
        s = p;
        while (is_ident(*p))
            p++;
        if (p == s)
            err("bad macro parameter", p);
        m->pname[m->nparams] = intern(s, (size_t)(p - s));
        if (*p == ':') {
            p++;
            if (word_is(p, "vararg"))
                m->vararg = 1;
            while (is_ident(*p))
                p++;
        }
        s = p;
        skip_ws();
        if (*p == '=') {
            p++;
            skip_ws();
            s = p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t')
                p++;
            m->pdef[m->nparams] = dup_range(s, p);
        } else
            p = s;
        m->nparams++;
    }
    m->body = collect(0);
    m->next = macros;
    macros = m;
}

static Macro *find_macro(u32 name)
{
    Macro *m;
    for (m = macros; m; m = m->next)
        if (m->name == name)
            return m;
    return 0;
}

static void repeat(int kind) /* 0 .rept, 1 .irp, 2 .irpc */
{
    Buf out;
    char *body, *one, *val, *list;
    u32 name = 0;
    i64 n = 0;
    memset(&out, 0, sizeof out);
    if (kind == 0) {
        n = const_expr();
    } else {
        char *s;
        skip_ws();
        s = p;
        while (is_ident(*p))
            p++;
        name = intern(s, (size_t)(p - s));
        accept_ch(',');
    }
    list = dup_range(p, p + strlen(p));
    body = collect(1);
    p = list;
    if (kind == 0) {
        while (n-- > 0)
            buf_put(&out, body, strlen(body));
    } else if (kind == 1) {
        for (;;) {
            skip_ws();
            if (!*p)
                break;
            val = macro_arg(0);
            one = substitute(body, &name, &val, 1, 0);
            buf_put(&out, one, strlen(one));
            free(one);
            free(val);
        }
    } else {
        skip_ws();
        for (; *p && *p != ' ' && *p != '\t'; p++) {
            char c[2];
            c[0] = *p;
            c[1] = 0;
            val = c;
            one = substitute(body, &name, &val, 1, 0);
            buf_put(&out, one, strlen(one));
            free(one);
        }
    }
    buf_u8(&out, 0);
    free(body);
    free(list);
    push_src((char *)out.p, 0);
    p = (char *)"";
}

/* ---- conditionals ---- */

#define SKIPPING (ncond && !(cond[ncond - 1] & 1))

static void cond_push(int truth)
{
    int outer = !SKIPPING;
    if (ncond == 64)
        err("conditionals nested too deeply", 0);
    cond[ncond++] = (u8)((outer && truth ? 3 : 0) | (outer ? 4 : 0));
}

/* w: the directive; returns 0 if it is not a conditional */
static int conditional(const char *w)
{
    char *a, *b;
    if (word_is(w, ".endif")) {
        if (!ncond)
            err(".endif without .if", 0);
        ncond--;
    } else if (word_is(w, ".else") || word_is(w, ".elseif") || word_is(w, ".elif")) {
        u8 *c;
        if (!ncond)
            err(".else without .if", 0);
        c = &cond[ncond - 1];
        if (!(*c & 4) || (*c & 2)) {
            *c &= (u8)~1;
        } else if (w[5] == 0 || w[5] == ' ' || w[5] == '\t' || const_expr()) {
            *c |= 3;
        }
    } else if (w[1] != 'i' || w[2] != 'f') {
        return 0;
    } else if (SKIPPING) {
        cond_push(0);
    } else if (word_is(w, ".if") || word_is(w, ".ifne")) {
        cond_push(const_expr() != 0);
    } else if (word_is(w, ".ifeq")) {
        cond_push(const_expr() == 0);
    } else if (word_is(w, ".ifdef") || word_is(w, ".ifndef")) {
        u32 name;
        int def;
        skip_ws();
        a = p;
        while (is_ident(*p))
            p++;
        name = intern(a, (size_t)(p - a));
        def = ext_of(name)->isabs || find_macro(name) != 0;
        if (!def) {
            u32 k = obj_find_sym(go, name);
            def = k && go->syms[k - 1].sec != SEC_UNDEF;
        }
        cond_push(w[3] == 'n' ? !def : def);
    } else if (word_is(w, ".ifb") || word_is(w, ".ifnb")) {
        skip_ws();
        cond_push(w[3] == 'n' ? *p != 0 : *p == 0);
    } else if (word_is(w, ".ifc") || word_is(w, ".ifnc")) {
        a = macro_arg(0);
        b = macro_arg(0);
        cond_push((strcmp(a, b) == 0) == (w[3] == 'c'));
        free(a);
        free(b);
    } else {
        return 0;
    }
    p += strlen(p);
    return 1;
}

/* ---- operands ---- */

static void split_ops(void)
{
    int depth = 0, str = 0;
    char *e;
    nops = 0;
    skip_ws();
    if (!*p)
        return;
    ops[nops++] = p;
    for (; *p; p++) {
        if (*p == '"')
            str = !str;
        else if (str)
            continue;
        else if (*p == '[' || *p == '(')
            depth++;
        else if (*p == ']' || *p == ')')
            depth--;
        else if (*p == ',' && depth <= 0) {
            *p = 0;
            if (nops == 10)
                err("too many operands", 0);
            ops[nops++] = p + 1;
        }
    }
    for (depth = 0; depth < nops; depth++) {
        while (*ops[depth] == ' ' || *ops[depth] == '\t')
            ops[depth]++;
        e = ops[depth] + strlen(ops[depth]);
        while (e > ops[depth] && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            *--e = 0;
    }
}

/* the length of the register name at s, or 0 */
static size_t reg_at(const char *s, Reg *r)
{
    size_t n = 0;
    u32 k, name;
    char b[8];
    int num;
    while (is_ident(s[n]))
        n++;
    if (!n)
        return 0;
    if (naliases) {
        name = intern(s, n);
        for (k = naliases; k-- > 0;)
            if (aliases[k].name == name) {
                *r = aliases[k].r;
                return n;
            }
    }
    if (n > 4)
        return 0;
    for (k = 0; k < n; k++)
        b[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
    b[n] = 0;
    r->sp = 0;
    r->cls = 'x';
    r->n = 31;
    if (strcmp(b, "sp") == 0) {
        r->sp = 1;
    } else if (strcmp(b, "wsp") == 0) {
        r->sp = 1;
        r->cls = 'w';
    } else if (strcmp(b, "xzr") == 0) {
        ;
    } else if (strcmp(b, "wzr") == 0) {
        r->cls = 'w';
    } else if (strcmp(b, "lr") == 0) {
        r->n = 30;
    } else if (strcmp(b, "fp") == 0) {
        r->n = 29;
    } else if (n >= 2 && n <= 3 && strchr("xwbhsdqv", b[0]) && b[1] >= '0' && b[1] <= '9' &&
               (n == 2 || (b[2] >= '0' && b[2] <= '9'))) {
        num = n == 2 ? b[1] - '0' : (b[1] - '0') * 10 + (b[2] - '0');
        if (num > 31 || (num == 31 && (b[0] == 'x' || b[0] == 'w')))
            return 0;
        r->cls = (u8)b[0];
        r->n = (u8)num;
    } else {
        return 0;
    }
    return n;
}

static int is_reg(int k)
{
    Reg r;
    return k < nops && reg_at(ops[k], &r) == strlen(ops[k]);
}

static Reg R(int k)
{
    Reg r;
    if (k >= nops)
        err("missing operand", 0);
    if (reg_at(ops[k], &r) != strlen(ops[k]))
        err("expected a register, not", ops[k]);
    return r;
}

static Val V(int k)
{
    Val v;
    if (k >= nops)
        err("missing operand", 0);
    p = ops[k];
    accept_ch('#');
    v = expr();
    skip_ws();
    if (*p)
        err("unexpected text in operand:", p);
    return v;
}

static i64 K(int k)
{
    Val v = V(k);
    if (v.add || v.sub)
        err("operand is not constant:", ops[k]);
    return v.k;
}

/* Does operand k start with ":name:"? If so that is skipped. */
static int modifier(int k, const char *name)
{
    char *s;
    size_t n = strlen(name);
    if (k >= nops)
        return 0;
    s = ops[k];
    if (*s == '#')
        s++;
    if (*s != ':' || strncmp(s + 1, name, n) != 0 || s[n + 1] != ':')
        return 0;
    ops[k] = s + n + 2;
    return 1;
}

/* shift or extension at s: 0 none, 1 shift, 2 extension */
static int shift_at(const char *s, int *type, i64 *amt, int *has)
{
    static const char *const names[] = {"lsl", "lsr", "asr", "ror", "uxtb", "uxth", "uxtw", "uxtx",
                                        "sxtb", "sxth", "sxtw", "sxtx"};
    int k;
    for (k = 0; k < 12; k++) {
        size_t n = strlen(names[k]);
        size_t j;
        for (j = 0; j < n; j++)
            if ((s[j] | 32) != names[k][j])
                break;
        if (j == n && !is_ident(s[n])) {
            p = (char *)s + n;
            skip_ws();
            *has = *p != 0;
            *amt = 0;
            if (*has) {
                accept_ch('#');
                *amt = const_expr();
            }
            *type = k < 4 ? k : k - 4;
            return k < 4 ? 1 : 2;
        }
    }
    return 0;
}

static int shift_op(int k, int *type, i64 *amt)
{
    int has;
    *type = 0;
    *amt = 0;
    return k < nops ? shift_at(ops[k], type, amt, &has) : 0;
}

static Mem M(int k)
{
    Mem m;
    Reg r;
    size_t n;
    char *s, *e;
    int type, has;
    memset(&m, 0, sizeof m);
    if (k >= nops || ops[k][0] != '[')
        err("expected a memory operand", 0);
    s = ops[k] + 1;
    while (*s == ' ')
        s++;
    n = reg_at(s, &r);
    if (!n)
        err("expected a base register:", ops[k]);
    m.base = r.n;
    e = strrchr(s, ']');
    if (!e)
        err("expected ']'", 0);
    if (e[1] == '!')
        m.pre = 1;
    *e = 0;
    s += n;
    while (*s == ' ')
        s++;
    if (*s == ',') {
        s++;
        while (*s == ' ')
            s++;
        n = reg_at(s, &r);
        if (n && (s[n] == 0 || s[n] == ',' || s[n] == ' ')) {
            m.kind = 1;
            m.idx = r.n;
            m.ext = 3;
            s += n;
            while (*s == ' ')
                s++;
            if (*s == ',') {
                s++;
                while (*s == ' ')
                    s++;
                switch (shift_at(s, &type, &m.amt, &has)) {
                case 1: break;
                case 2: m.ext = (u8)type; break;
                default: err("bad index:", s);
                }
                m.hasamt = (u8)has;
            }
        } else {
            p = s;
            accept_ch('#');
            if (strncmp(p, ":lo12:", 6) == 0) {
                p += 6;
                m.kind = 2;
            }
            m.off = expr();
            skip_ws();
            if (*p)
                err("unexpected text in operand:", p);
        }
    }
    return m;
}

static int cond_of(const char *s)
{
    static const char names[] = "eqnecsccmiplvsvchilsgeltgtlealnvhslo";
    int k;
    if (strlen(s) != 2)
        return -1;
    for (k = 0; k < 18; k++)
        if ((s[0] | 32) == names[2 * k] && (s[1] | 32) == names[2 * k + 1])
            return k == 16 ? 2 : k == 17 ? 3 : k;
    return -1;
}

static u32 C(int k)
{
    int c = k < nops ? cond_of(ops[k]) : -1;
    if (c < 0)
        err("expected a condition", 0);
    return (u32)c;
}

static void lower(char *d, const char *s, size_t cap)
{
    size_t k;
    for (k = 0; s[k] && k + 1 < cap; k++)
        d[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
    d[k] = 0;
}

/* ---- system registers and operations ---- */

#define S(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

typedef struct Named {
    const char *name;
    u16 enc;
} Named;

static const Named sysregs[] = {
    {"midr_el1", S(3, 0, 0, 0, 0)}, {"mpidr_el1", S(3, 0, 0, 0, 5)}, {"revidr_el1", S(3, 0, 0, 0, 6)},
    {"id_aa64pfr0_el1", S(3, 0, 0, 4, 0)}, {"id_aa64pfr1_el1", S(3, 0, 0, 4, 1)},
    {"id_aa64zfr0_el1", S(3, 0, 0, 4, 4)}, {"id_aa64dfr0_el1", S(3, 0, 0, 5, 0)},
    {"id_aa64dfr1_el1", S(3, 0, 0, 5, 1)}, {"id_aa64isar0_el1", S(3, 0, 0, 6, 0)},
    {"id_aa64isar1_el1", S(3, 0, 0, 6, 1)}, {"id_aa64isar2_el1", S(3, 0, 0, 6, 2)},
    {"id_aa64mmfr0_el1", S(3, 0, 0, 7, 0)}, {"id_aa64mmfr1_el1", S(3, 0, 0, 7, 1)},
    {"id_aa64mmfr2_el1", S(3, 0, 0, 7, 2)},
    {"sctlr_el1", S(3, 0, 1, 0, 0)}, {"actlr_el1", S(3, 0, 1, 0, 1)}, {"cpacr_el1", S(3, 0, 1, 0, 2)},
    {"ttbr0_el1", S(3, 0, 2, 0, 0)}, {"ttbr1_el1", S(3, 0, 2, 0, 1)}, {"tcr_el1", S(3, 0, 2, 0, 2)},
    {"spsr_el1", S(3, 0, 4, 0, 0)}, {"elr_el1", S(3, 0, 4, 0, 1)}, {"sp_el0", S(3, 0, 4, 1, 0)},
    {"spsel", S(3, 0, 4, 2, 0)}, {"currentel", S(3, 0, 4, 2, 2)}, {"pan", S(3, 0, 4, 2, 3)},
    {"uao", S(3, 0, 4, 2, 4)}, {"daif", S(3, 3, 4, 2, 1)}, {"nzcv", S(3, 3, 4, 2, 0)},
    {"fpcr", S(3, 3, 4, 4, 0)}, {"fpsr", S(3, 3, 4, 4, 1)}, {"dspsr_el0", S(3, 3, 4, 5, 0)},
    {"dlr_el0", S(3, 3, 4, 5, 1)},
    {"afsr0_el1", S(3, 0, 5, 1, 0)}, {"afsr1_el1", S(3, 0, 5, 1, 1)}, {"esr_el1", S(3, 0, 5, 2, 0)},
    {"far_el1", S(3, 0, 6, 0, 0)}, {"par_el1", S(3, 0, 7, 4, 0)},
    {"mair_el1", S(3, 0, 10, 2, 0)}, {"amair_el1", S(3, 0, 10, 3, 0)},
    {"vbar_el1", S(3, 0, 12, 0, 0)}, {"rvbar_el1", S(3, 0, 12, 0, 1)}, {"isr_el1", S(3, 0, 12, 1, 0)},
    {"contextidr_el1", S(3, 0, 13, 0, 1)}, {"tpidr_el1", S(3, 0, 13, 0, 4)},
    {"tpidr_el0", S(3, 3, 13, 0, 2)}, {"tpidrro_el0", S(3, 3, 13, 0, 3)}, {"tpidr_el2", S(3, 4, 13, 0, 2)},
    {"cntkctl_el1", S(3, 0, 14, 1, 0)}, {"cntfrq_el0", S(3, 3, 14, 0, 0)}, {"cntpct_el0", S(3, 3, 14, 0, 1)},
    {"cntvct_el0", S(3, 3, 14, 0, 2)}, {"cntp_tval_el0", S(3, 3, 14, 2, 0)},
    {"cntp_ctl_el0", S(3, 3, 14, 2, 1)}, {"cntp_cval_el0", S(3, 3, 14, 2, 2)},
    {"cntv_tval_el0", S(3, 3, 14, 3, 0)}, {"cntv_ctl_el0", S(3, 3, 14, 3, 1)},
    {"cntv_cval_el0", S(3, 3, 14, 3, 2)},
    {"ccsidr_el1", S(3, 1, 0, 0, 0)}, {"clidr_el1", S(3, 1, 0, 0, 1)}, {"aidr_el1", S(3, 1, 0, 0, 7)},
    {"csselr_el1", S(3, 2, 0, 0, 0)}, {"ctr_el0", S(3, 3, 0, 0, 1)}, {"dczid_el0", S(3, 3, 0, 0, 7)},
    {"mdccint_el1", S(2, 0, 0, 2, 0)}, {"mdscr_el1", S(2, 0, 0, 2, 2)}, {"osdtrrx_el1", S(2, 0, 0, 0, 2)},
    {"osdtrtx_el1", S(2, 0, 0, 3, 2)}, {"oseccr_el1", S(2, 0, 0, 6, 2)}, {"mdrar_el1", S(2, 0, 1, 0, 0)},
    {"oslar_el1", S(2, 0, 1, 0, 4)}, {"oslsr_el1", S(2, 0, 1, 1, 4)}, {"osdlr_el1", S(2, 0, 1, 3, 4)},
    {"dbgprcr_el1", S(2, 0, 1, 4, 4)}, {"mdccsr_el0", S(2, 3, 0, 1, 0)}, {"dbgdtr_el0", S(2, 3, 0, 4, 0)},
    {"dbgdtrrx_el0", S(2, 3, 0, 5, 0)}, {"dbgdtrtx_el0", S(2, 3, 0, 5, 0)},
    {"vpidr_el2", S(3, 4, 0, 0, 0)}, {"vmpidr_el2", S(3, 4, 0, 0, 5)},
    {"sctlr_el2", S(3, 4, 1, 0, 0)}, {"actlr_el2", S(3, 4, 1, 0, 1)}, {"hcr_el2", S(3, 4, 1, 1, 0)},
    {"mdcr_el2", S(3, 4, 1, 1, 1)}, {"cptr_el2", S(3, 4, 1, 1, 2)}, {"hstr_el2", S(3, 4, 1, 1, 3)},
    {"hacr_el2", S(3, 4, 1, 1, 7)}, {"ttbr0_el2", S(3, 4, 2, 0, 0)}, {"tcr_el2", S(3, 4, 2, 0, 2)},
    {"vttbr_el2", S(3, 4, 2, 1, 0)}, {"vtcr_el2", S(3, 4, 2, 1, 2)}, {"dacr32_el2", S(3, 4, 3, 0, 0)},
    {"spsr_el2", S(3, 4, 4, 0, 0)}, {"elr_el2", S(3, 4, 4, 0, 1)}, {"sp_el1", S(3, 4, 4, 1, 0)},
    {"ifsr32_el2", S(3, 4, 5, 0, 1)}, {"afsr0_el2", S(3, 4, 5, 1, 0)}, {"afsr1_el2", S(3, 4, 5, 1, 1)},
    {"esr_el2", S(3, 4, 5, 2, 0)}, {"fpexc32_el2", S(3, 4, 5, 3, 0)}, {"far_el2", S(3, 4, 6, 0, 0)},
    {"hpfar_el2", S(3, 4, 6, 0, 4)}, {"mair_el2", S(3, 4, 10, 2, 0)}, {"amair_el2", S(3, 4, 10, 3, 0)},
    {"vbar_el2", S(3, 4, 12, 0, 0)}, {"rvbar_el2", S(3, 4, 12, 0, 1)}, {"contextidr_el2", S(3, 4, 13, 0, 1)},
    {"cntvoff_el2", S(3, 4, 14, 0, 3)}, {"cnthctl_el2", S(3, 4, 14, 1, 0)},
    {"cnthp_tval_el2", S(3, 4, 14, 2, 0)}, {"cnthp_ctl_el2", S(3, 4, 14, 2, 1)},
    {"cnthp_cval_el2", S(3, 4, 14, 2, 2)}, {"dbgvcr32_el2", S(2, 4, 0, 7, 0)},
    {"sctlr_el12", S(3, 5, 1, 0, 0)}, {"cpacr_el12", S(3, 5, 1, 0, 2)}, {"ttbr0_el12", S(3, 5, 2, 0, 0)},
    {"ttbr1_el12", S(3, 5, 2, 0, 1)}, {"tcr_el12", S(3, 5, 2, 0, 2)}, {"spsr_el12", S(3, 5, 4, 0, 0)},
    {"elr_el12", S(3, 5, 4, 0, 1)}, {"esr_el12", S(3, 5, 5, 2, 0)}, {"far_el12", S(3, 5, 6, 0, 0)},
    {"mair_el12", S(3, 5, 10, 2, 0)}, {"vbar_el12", S(3, 5, 12, 0, 0)}, {"cntkctl_el12", S(3, 5, 14, 1, 0)},
    {"sctlr_el3", S(3, 6, 1, 0, 0)}, {"scr_el3", S(3, 6, 1, 1, 0)}, {"cptr_el3", S(3, 6, 1, 1, 2)},
    {"mdcr_el3", S(3, 6, 1, 3, 1)}, {"ttbr0_el3", S(3, 6, 2, 0, 0)}, {"tcr_el3", S(3, 6, 2, 0, 2)},
    {"spsr_el3", S(3, 6, 4, 0, 0)}, {"elr_el3", S(3, 6, 4, 0, 1)}, {"sp_el2", S(3, 6, 4, 1, 0)},
    {"esr_el3", S(3, 6, 5, 2, 0)}, {"far_el3", S(3, 6, 6, 0, 0)}, {"mair_el3", S(3, 6, 10, 2, 0)},
    {"vbar_el3", S(3, 6, 12, 0, 0)}, {"tpidr_el3", S(3, 6, 13, 0, 2)},
    {"pmintenset_el1", S(3, 0, 9, 14, 1)}, {"pmintenclr_el1", S(3, 0, 9, 14, 2)},
    {"pmcr_el0", S(3, 3, 9, 12, 0)}, {"pmcntenset_el0", S(3, 3, 9, 12, 1)},
    {"pmcntenclr_el0", S(3, 3, 9, 12, 2)}, {"pmovsclr_el0", S(3, 3, 9, 12, 3)},
    {"pmswinc_el0", S(3, 3, 9, 12, 4)}, {"pmselr_el0", S(3, 3, 9, 12, 5)},
    {"pmceid0_el0", S(3, 3, 9, 12, 6)}, {"pmceid1_el0", S(3, 3, 9, 12, 7)},
    {"pmccntr_el0", S(3, 3, 9, 13, 0)}, {"pmxevtyper_el0", S(3, 3, 9, 13, 1)},
    {"pmxevcntr_el0", S(3, 3, 9, 13, 2)}, {"pmuserenr_el0", S(3, 3, 9, 14, 0)},
    {"pmovsset_el0", S(3, 3, 9, 14, 3)}, {"pmccfiltr_el0", S(3, 3, 14, 15, 7)},
    {0, 0}
};

/* dc, ic, at, tlbi: the first letter of the instruction, then the operation */
static const Named sysops[] = {
    {"i:ialluis", S(1, 0, 7, 1, 0)}, {"i:iallu", S(1, 0, 7, 5, 0)}, {"i:ivau", S(1, 3, 7, 5, 1)},
    {"d:zva", S(1, 3, 7, 4, 1)}, {"d:ivac", S(1, 0, 7, 6, 1)}, {"d:isw", S(1, 0, 7, 6, 2)},
    {"d:cvac", S(1, 3, 7, 10, 1)}, {"d:csw", S(1, 0, 7, 10, 2)}, {"d:cvau", S(1, 3, 7, 11, 1)},
    {"d:cvap", S(1, 3, 7, 12, 1)}, {"d:cvadp", S(1, 3, 7, 13, 1)}, {"d:civac", S(1, 3, 7, 14, 1)},
    {"d:cisw", S(1, 0, 7, 14, 2)}, {"d:gva", S(1, 3, 7, 4, 3)}, {"d:gzva", S(1, 3, 7, 4, 4)},
    {"a:s1e1r", S(1, 0, 7, 8, 0)}, {"a:s1e1w", S(1, 0, 7, 8, 1)}, {"a:s1e0r", S(1, 0, 7, 8, 2)},
    {"a:s1e0w", S(1, 0, 7, 8, 3)}, {"a:s1e1rp", S(1, 0, 7, 9, 0)}, {"a:s1e1wp", S(1, 0, 7, 9, 1)},
    {"a:s1e2r", S(1, 4, 7, 8, 0)}, {"a:s1e2w", S(1, 4, 7, 8, 1)}, {"a:s12e1r", S(1, 4, 7, 8, 4)},
    {"a:s12e1w", S(1, 4, 7, 8, 5)}, {"a:s12e0r", S(1, 4, 7, 8, 6)}, {"a:s12e0w", S(1, 4, 7, 8, 7)},
    {"a:s1e3r", S(1, 6, 7, 8, 0)}, {"a:s1e3w", S(1, 6, 7, 8, 1)},
    {"t:vmalle1is", S(1, 0, 8, 3, 0)}, {"t:vae1is", S(1, 0, 8, 3, 1)}, {"t:aside1is", S(1, 0, 8, 3, 2)},
    {"t:vaae1is", S(1, 0, 8, 3, 3)}, {"t:vale1is", S(1, 0, 8, 3, 5)}, {"t:vaale1is", S(1, 0, 8, 3, 7)},
    {"t:vmalle1", S(1, 0, 8, 7, 0)}, {"t:vae1", S(1, 0, 8, 7, 1)}, {"t:aside1", S(1, 0, 8, 7, 2)},
    {"t:vaae1", S(1, 0, 8, 7, 3)}, {"t:vale1", S(1, 0, 8, 7, 5)}, {"t:vaale1", S(1, 0, 8, 7, 7)},
    {"t:rvae1is", S(1, 0, 8, 2, 1)}, {"t:rvaae1is", S(1, 0, 8, 2, 3)}, {"t:rvale1is", S(1, 0, 8, 2, 5)},
    {"t:rvaale1is", S(1, 0, 8, 2, 7)}, {"t:rvae1", S(1, 0, 8, 6, 1)}, {"t:rvaae1", S(1, 0, 8, 6, 3)},
    {"t:rvale1", S(1, 0, 8, 6, 5)}, {"t:rvaale1", S(1, 0, 8, 6, 7)},
    {"t:ipas2e1is", S(1, 4, 8, 0, 1)}, {"t:ipas2le1is", S(1, 4, 8, 0, 5)}, {"t:alle2is", S(1, 4, 8, 3, 0)},
    {"t:vae2is", S(1, 4, 8, 3, 1)}, {"t:alle1is", S(1, 4, 8, 3, 4)}, {"t:vale2is", S(1, 4, 8, 3, 5)},
    {"t:vmalls12e1is", S(1, 4, 8, 3, 6)}, {"t:ipas2e1", S(1, 4, 8, 4, 1)}, {"t:ipas2le1", S(1, 4, 8, 4, 5)},
    {"t:alle2", S(1, 4, 8, 7, 0)}, {"t:vae2", S(1, 4, 8, 7, 1)}, {"t:alle1", S(1, 4, 8, 7, 4)},
    {"t:vale2", S(1, 4, 8, 7, 5)}, {"t:vmalls12e1", S(1, 4, 8, 7, 6)}, {"t:alle3is", S(1, 6, 8, 3, 0)},
    {"t:vae3is", S(1, 6, 8, 3, 1)}, {"t:vale3is", S(1, 6, 8, 3, 5)}, {"t:alle3", S(1, 6, 8, 7, 0)},
    {"t:vae3", S(1, 6, 8, 7, 1)}, {"t:vale3", S(1, 6, 8, 7, 5)},
    {0, 0}
};

static const Named pstates[] = { /* op1 << 3 | op2 */
    {"spsel", 0 << 3 | 5}, {"daifset", 3 << 3 | 6}, {"daifclr", 3 << 3 | 7}, {"pan", 0 << 3 | 4},
    {"uao", 0 << 3 | 3}, {"dit", 3 << 3 | 2}, {"ssbs", 3 << 3 | 1}, {"tco", 3 << 3 | 4}, {0, 0}
};

static const Named barriers[] = {
    {"sy", 15}, {"st", 14}, {"ld", 13}, {"ish", 11}, {"ishst", 10}, {"ishld", 9}, {"nsh", 7}, {"nshst", 6},
    {"nshld", 5}, {"osh", 3}, {"oshst", 2}, {"oshld", 1}, {0, 0}
};

static int named(const Named *t, const char *name)
{
    for (; t->name; t++)
        if (strcmp(t->name, name) == 0)
            return t->enc;
    return -1;
}

static int num_at(const char **s, u32 *v)
{
    const char *q = *s;
    if (*q < '0' || *q > '9')
        return 0;
    *v = 0;
    while (*q >= '0' && *q <= '9')
        *v = *v * 10 + (u32)(*q++ - '0');
    *s = q;
    return 1;
}

static u32 sysreg(int k)
{
    char b[48];
    const char *q;
    u32 o0, o1, cn, cm, o2, n;
    int e;
    if (k >= nops)
        err("missing operand", 0);
    lower(b, ops[k], sizeof b);
    q = b + 1;
    if (b[0] == 's' && num_at(&q, &o0) && *q++ == '_' && num_at(&q, &o1) && *q++ == '_' && *q++ == 'c' &&
        num_at(&q, &cn) && *q++ == '_' && *q++ == 'c' && num_at(&q, &cm) && *q++ == '_' && num_at(&q, &o2) && !*q)
        return S(o0, o1, cn, cm, o2);
    e = named(sysregs, b);
    if (e < 0) {
        /* the numbered debug registers: dbgbvr3_el1 and the like */
        q = b + 6;
        if (strncmp(b, "dbg", 3) == 0 && (b[3] == 'b' || b[3] == 'w') && (b[4] == 'v' || b[4] == 'c') &&
            b[5] == 'r' && num_at(&q, &n) && n < 16 && strcmp(q, "_el1") == 0)
            return S(2, 0, 0, n, (b[3] == 'b' ? 4 : 6) + (b[4] == 'c'));
        err("unknown system register", ops[k]);
    }
    return (u32)e;
}

/* ---- instructions ---- */

#define SF(r) ((r).cls == 'x' ? 0x80000000U : 0)

static void addsub(u32 sf, u32 op, Reg rd, Reg rn, int k)
{
    int type, st;
    i64 amt, imm;
    st = shift_op(k + 1, &type, &amt);
    if (!is_reg(k)) {
        u32 sh = 0;
        if (modifier(k, "lo12")) {
            put_fix(sf | op | 0x11000000 | (u32)rn.n << 5 | rd.n, FX_LO12, V(k));
            return;
        }
        imm = K(k);
        if (st == 1 && type == 0 && amt == 12)
            sh = 1;
        else if (st)
            err("bad shift of an immediate", 0);
        if (imm < 0) {
            imm = -imm;
            op ^= 0x40000000;
        }
        if (!sh && imm >= 4096 && !(imm & 0xfff)) {
            imm >>= 12;
            sh = 1;
        }
        if (imm >= 4096)
            err("immediate out of range", 0);
        put(sf | op | 0x11000000 | sh << 22 | (u32)imm << 10 | (u32)rn.n << 5 | rd.n);
    } else {
        Reg rm = R(k);
        if (st == 2 || rd.sp || rn.sp) {
            u32 option = st == 2 ? (u32)type : sf ? 3 : 2;
            put(sf | op | 0x0b200000 | (u32)rm.n << 16 | option << 13 | ((u32)amt & 7) << 10 | (u32)rn.n << 5 | rd.n);
        } else {
            put(sf | op | 0x0b000000 | (u32)type << 22 | (u32)rm.n << 16 | ((u32)amt & 63) << 10 | (u32)rn.n << 5 |
                rd.n);
        }
    }
}

static void logical(u32 sf, u32 op, Reg rd, Reg rn, int k)
{
    int type;
    i64 amt;
    if (!is_reg(k)) {
        u64 v = (u64)K(k);
        u32 enc;
        if (op & 0x00200000)
            v = ~v;
        if (!sf)
            v &= 0xffffffffUL;
        if (!arm64_logical_imm(v, sf != 0, &enc))
            err("immediate cannot be encoded:", ops[k]);
        put(sf | (op & 0x60000000) | 0x12000000 | enc << 10 | (u32)rn.n << 5 | rd.n);
    } else {
        Reg rm = R(k);
        shift_op(k + 1, &type, &amt);
        put(sf | op | (u32)type << 22 | (u32)rm.n << 16 | ((u32)amt & 63) << 10 | (u32)rn.n << 5 | rd.n);
    }
}

static void mov_imm(Reg rd, u64 v)
{
    u32 sf = SF(rd), hw, enc, n = sf ? 4 : 2;
    u64 nv;
    if (!sf)
        v &= 0xffffffffUL;
    nv = sf ? ~v : ~v & 0xffffffffUL;
    for (hw = 0; hw < n; hw++)
        if (!(v & ~((u64)0xffff << 16 * hw))) {
            put(sf | 0x52800000 | hw << 21 | (u32)(v >> 16 * hw) << 5 | rd.n);
            return;
        }
    for (hw = 0; hw < n; hw++)
        if (!(nv & ~((u64)0xffff << 16 * hw))) {
            put(sf | 0x12800000 | hw << 21 | (u32)(nv >> 16 * hw) << 5 | rd.n);
            return;
        }
    if (!arm64_logical_imm(v, sf != 0, &enc))
        err("immediate needs movz and movk", 0);
    put(sf | 0x320003e0 | enc << 10 | rd.n);
}

static int load_store(const char *m)
{
    const char *q = m + 2;
    int var = 0, load = m[0] == 'l', prf = 0;
    u32 size, opc, v = 0, base, rt, scale;
    Reg r;
    Mem mem;
    i64 imm;

    r.cls = 'x';
    if (strncmp(m, "prf", 3) == 0) {
        prf = 1;
        q = m + 3;
        if (*q == 'u') {
            var = 1;
            q++;
        }
        if (strcmp(q, "m") != 0)
            return 0;
    } else {
        if (strncmp(m, "ld", 2) != 0 && strncmp(m, "st", 2) != 0)
            return 0;
        if (*q == 'u') {
            var = 1;
            q++;
        } else if (*q == 't') {
            var = 2;
            q++;
        }
        if (*q++ != 'r')
            return 0;
    }
    if (prf) {
        static const char *const kinds[] = {"pld", "pli", "pst"};
        char b[16];
        u32 k;
        if (nops < 1)
            err("missing operand", 0);
        lower(b, ops[0], sizeof b);
        size = 3;
        opc = 2;
        for (k = 0; k < 3; k++)
            if (strncmp(b, kinds[k], 3) == 0 && b[3] == 'l' && b[4] >= '1' && b[4] <= '3' &&
                (strcmp(b + 5, "keep") == 0 || strcmp(b + 5, "strm") == 0))
                break;
        rt = k < 3 ? k << 3 | (u32)(b[4] - '1') << 1 | (b[5] == 's') : (u32)K(0);
    } else {
        r = R(0);
        rt = r.n;
        opc = (u32)load;
        if (!*q) {
            size = r.cls == 'x' || r.cls == 'd' ? 3 : r.cls == 'h' ? 1 : r.cls == 'b' || r.cls == 'q' ? 0 : 2;
            v = r.cls != 'x' && r.cls != 'w';
            if (r.cls == 'q')
                opc |= 2;
        } else if (strcmp(q, "b") == 0 || strcmp(q, "h") == 0) {
            size = *q == 'h';
        } else if (load && (strcmp(q, "sb") == 0 || strcmp(q, "sh") == 0)) {
            size = q[1] == 'h';
            opc = r.cls == 'x' ? 2 : 3;
        } else if (load && strcmp(q, "sw") == 0) {
            size = 2;
            opc = 2;
        } else {
            return 0;
        }
    }
    base = size << 30 | 0x39000000 | v << 26 | opc << 22;
    scale = r.cls == 'q' ? 16 : (u32)1 << size;
    if (nops >= 2 && ops[1][0] != '[') {
        u32 lit = prf ? 0xd8000000 : v ? (r.cls == 's' ? 0x1c000000U : r.cls == 'd' ? 0x5c000000U : 0x9c000000U) :
                  opc == 2 ? 0x98000000 : size == 3 ? 0x58000000 : 0x18000000;
        Val val;
        if (ops[1][0] == '=') {
            Lit *l;
            u32 k;
            if (nlits == caplits) {
                caplits = caplits ? caplits * 2 : 64;
                lits = (Lit *)xrealloc(lits, caplits * sizeof(Lit));
            }
            p = ops[1] + 1;
            val = expr();
            /* one entry serves every use of the same number or symbol */
            for (k = 0; k < nlits; k++) {
                l = &lits[k];
                if (l->sec == cur && l->size == (size == 3 ? 3U : 2U) && !val.sub && !l->v.sub &&
                    l->v.add == val.add && l->v.k == val.k && (!val.add || !val.k))
                    break;
            }
            if (k == nlits) {
                l = &lits[nlits++];
                l->sec = cur;
                l->size = size == 3 ? 3 : 2;
                l->v = val;
                l->sym = obj_add_sym(go, 0, SEC_UNDEF, 0, 0);
            }
            val.add = l->sym + 1;
            val.sub = 0;
            val.k = 0;
        } else {
            val = V(1);
        }
        put_fix(lit | rt, FX_LIT19, val);
        return 1;
    }
    mem = M(1);
    base |= (u32)mem.base << 5 | rt;
    if (nops == 3) {
        imm = K(2);
        if (imm < -256 || imm > 255)
            err("offset out of range", 0);
        put((base & ~0x01000000U) | 0x400 | ((u32)imm & 0x1ff) << 12);
    } else if (mem.kind == 2) {
        put_fix(base, FX_LO12, mem.off);
    } else if (mem.kind == 1) {
        put((base & ~0x01000000U) | 0x00200800 | (u32)mem.idx << 16 | (u32)mem.ext << 13 |
            (u32)(mem.hasamt && mem.amt != 0) << 12);
    } else {
        if (mem.off.add || mem.off.sub)
            err("offset is not constant", 0);
        imm = mem.off.k;
        if (!mem.pre && var == 0 && imm >= 0 && imm % scale == 0 && imm / scale < 4096) {
            put(base | (u32)(imm / scale) << 10);
        } else {
            if (imm < -256 || imm > 255)
                err("offset out of range", 0);
            put((base & ~0x01000000U) | (mem.pre ? 0xc00U : var == 2 ? 0x800U : 0) | ((u32)imm & 0x1ff) << 12);
        }
    }
    return 1;
}

static void pair(const char *m)
{
    Reg r = R(0), r2 = R(1);
    Mem mem = M(2);
    u32 base, scale = 8;
    i64 imm;
    switch (r.cls) {
    case 'x': base = 0xa9000000; break;
    case 'w': base = 0x29000000; scale = 4; break;
    case 'q': base = 0xad000000; scale = 16; break;
    case 'd': base = 0x6d000000; break;
    default: base = 0x2d000000; scale = 4; break;
    }
    if (m[0] == 'l')
        base |= 0x00400000;
    if (strcmp(m, "ldpsw") == 0) {
        base = 0x69400000;
        scale = 4;
    }
    if (mem.off.add || mem.off.sub)
        err("offset is not constant", 0);
    imm = mem.off.k;
    if (m[2] == 'n')
        base &= ~0x01000000U;
    else if (nops == 4) {
        base = (base & ~0x01000000U) | 0x00800000;
        imm = K(3);
    } else if (mem.pre)
        base |= 0x00800000;
    if (imm % scale || imm / scale < -64 || imm / scale > 63)
        err("pair offset out of range", 0);
    put(base | ((u32)(imm / scale) & 0x7f) << 15 | (u32)r2.n << 10 | (u32)mem.base << 5 | r.n);
}

enum {
    F_ADDSUB, F_CMP, F_NEG, F_LOGIC, F_TST, F_MVN, F_MOV, F_MOVW, F_SHIFT, F_BFM, F_BFX, F_BFI, F_EXT, F_EXTR,
    F_RRR, F_RRRR, F_RR, F_REV, F_CSEL, F_CSET, F_CINC, F_CCMP, F_B, F_CBZ, F_TBZ, F_BR, F_FIXED, F_EXC, F_HINT,
    F_BARRIER, F_ADR, F_LDX, F_STX, F_LDXP, F_STXP, F_MRS, F_MSR, F_SYS, F_SYSL, F_SYSOP, F_PAIR, F_NGC, F_FMOV, F_FP1, F_FP2,
    F_FCVTI, F_ICVTF, F_FCSEL, F_FCMP, F_FCCMP, F_FCVT, F_FP3
};

typedef struct Op {
    const char *name;
    u8 fmt;
    u32 enc;
} Op;

static const Op optab[] = {
    {"add", F_ADDSUB, 0}, {"adds", F_ADDSUB, 0x20000000}, {"sub", F_ADDSUB, 0x40000000},
    {"subs", F_ADDSUB, 0x60000000}, {"cmp", F_CMP, 0x60000000}, {"cmn", F_CMP, 0x20000000},
    {"neg", F_NEG, 0x40000000}, {"negs", F_NEG, 0x60000000},
    {"and", F_LOGIC, 0x0a000000}, {"bic", F_LOGIC, 0x0a200000}, {"orr", F_LOGIC, 0x2a000000},
    {"orn", F_LOGIC, 0x2a200000}, {"eor", F_LOGIC, 0x4a000000}, {"eon", F_LOGIC, 0x4a200000},
    {"ands", F_LOGIC, 0x6a000000}, {"bics", F_LOGIC, 0x6a200000}, {"tst", F_TST, 0x6a000000},
    {"mvn", F_MVN, 0x2a200000}, {"mov", F_MOV, 0},
    {"movz", F_MOVW, 0x52800000}, {"movn", F_MOVW, 0x12800000}, {"movk", F_MOVW, 0x72800000},
    {"lsl", F_SHIFT, 0}, {"lsr", F_SHIFT, 1}, {"asr", F_SHIFT, 2}, {"ror", F_SHIFT, 3},
    {"ubfm", F_BFM, 0x53000000}, {"sbfm", F_BFM, 0x13000000}, {"bfm", F_BFM, 0x33000000},
    {"ubfx", F_BFX, 0x53000000}, {"sbfx", F_BFX, 0x13000000}, {"bfxil", F_BFX, 0x33000000},
    {"ubfiz", F_BFI, 0x53000000}, {"sbfiz", F_BFI, 0x13000000}, {"bfi", F_BFI, 0x33000000},
    {"sxtb", F_EXT, 0x13000000 | 7 << 10}, {"sxth", F_EXT, 0x13000000 | 15 << 10},
    {"sxtw", F_EXT, 0x13000000 | 31 << 10}, {"uxtb", F_EXT, 0x53000000 | 7 << 10},
    {"uxth", F_EXT, 0x53000000 | 15 << 10}, {"uxtw", F_EXT, 0x53000000 | 31 << 10},
    {"extr", F_EXTR, 0x13800000},
    {"crc32b", F_RRR, 0x1ac04000}, {"crc32h", F_RRR, 0x1ac04400}, {"crc32w", F_RRR, 0x1ac04800},
    {"crc32x", F_RRR, 0x9ac04c00}, {"crc32cb", F_RRR, 0x1ac05000}, {"crc32ch", F_RRR, 0x1ac05400},
    {"crc32cw", F_RRR, 0x1ac05800}, {"crc32cx", F_RRR, 0x9ac05c00},
    {"udiv", F_RRR, 0x1ac00800}, {"sdiv", F_RRR, 0x1ac00c00}, {"lslv", F_RRR, 0x1ac02000},
    {"lsrv", F_RRR, 0x1ac02400}, {"asrv", F_RRR, 0x1ac02800}, {"rorv", F_RRR, 0x1ac02c00},
    {"adc", F_RRR, 0x1a000000}, {"adcs", F_RRR, 0x3a000000}, {"sbc", F_RRR, 0x5a000000},
    {"sbcs", F_RRR, 0x7a000000}, {"mul", F_RRR, 0x1b007c00}, {"mneg", F_RRR, 0x1b00fc00},
    {"smulh", F_RRR, 0x9b407c00}, {"umulh", F_RRR, 0x9bc07c00}, {"smull", F_RRR, 0x9b207c00},
    {"umull", F_RRR, 0x9ba07c00}, {"smnegl", F_RRR, 0x9b20fc00}, {"umnegl", F_RRR, 0x9ba0fc00},
    {"madd", F_RRRR, 0x1b000000}, {"msub", F_RRRR, 0x1b008000}, {"smaddl", F_RRRR, 0x9b200000},
    {"umaddl", F_RRRR, 0x9ba00000}, {"smsubl", F_RRRR, 0x9b208000}, {"umsubl", F_RRRR, 0x9ba08000},
    {"ngc", F_NGC, 0x5a0003e0}, {"ngcs", F_NGC, 0x7a0003e0},
    {"clz", F_RR, 0x5ac01000}, {"cls", F_RR, 0x5ac01400}, {"rbit", F_RR, 0x5ac00000},
    {"rev16", F_RR, 0x5ac00400}, {"rev32", F_RR, 0x5ac00800}, {"rev", F_REV, 0},
    {"csel", F_CSEL, 0x1a800000}, {"csinc", F_CSEL, 0x1a800400}, {"csinv", F_CSEL, 0x5a800000},
    {"csneg", F_CSEL, 0x5a800400}, {"cset", F_CSET, 0x1a9f07e0}, {"csetm", F_CSET, 0x5a9f03e0},
    {"cinc", F_CINC, 0x1a800400}, {"cinv", F_CINC, 0x5a800000}, {"cneg", F_CINC, 0x5a800400},
    {"ccmp", F_CCMP, 0x7a400000}, {"ccmn", F_CCMP, 0x3a400000},
    {"b", F_B, 0x14000000}, {"bl", F_B, 0x94000000},
    {"cbz", F_CBZ, 0x34000000}, {"cbnz", F_CBZ, 0x35000000},
    {"tbz", F_TBZ, 0x36000000}, {"tbnz", F_TBZ, 0x37000000},
    {"br", F_BR, 0xd61f0000}, {"blr", F_BR, 0xd63f0000}, {"ret", F_BR, 0xd65f0000},
    {"nop", F_FIXED, 0xd503201f}, {"yield", F_FIXED, 0xd503203f}, {"wfe", F_FIXED, 0xd503205f},
    {"wfi", F_FIXED, 0xd503207f}, {"sev", F_FIXED, 0xd503209f}, {"sevl", F_FIXED, 0xd50320bf},
    {"eret", F_FIXED, 0xd69f03e0}, {"drps", F_FIXED, 0xd6bf03e0}, {"csdb", F_FIXED, 0xd503229f},
    {"esb", F_FIXED, 0xd503221f}, {"paciasp", F_FIXED, 0xd503233f}, {"autiasp", F_FIXED, 0xd50323bf},
    {"paciaz", F_FIXED, 0xd503231f}, {"autiaz", F_FIXED, 0xd503239f}, {"pacibsp", F_FIXED, 0xd503237f},
    {"autibsp", F_FIXED, 0xd50323ff}, {"xpaclri", F_FIXED, 0xd50320ff}, {"ssbb", F_FIXED, 0xd503309f},
    {"pssbb", F_FIXED, 0xd503349f}, {"sb", F_FIXED, 0xd50330ff},
    {"svc", F_EXC, 0xd4000001}, {"hvc", F_EXC, 0xd4000002}, {"smc", F_EXC, 0xd4000003},
    {"brk", F_EXC, 0xd4200000}, {"hlt", F_EXC, 0xd4400000}, {"udf", F_EXC, 0},
    {"hint", F_HINT, 0xd503201f}, {"bti", F_HINT, 0xd503241f},
    {"dsb", F_BARRIER, 0xd503309f}, {"dmb", F_BARRIER, 0xd50330bf}, {"isb", F_BARRIER, 0xd50330df},
    {"clrex", F_BARRIER, 0xd503305f},
    {"adr", F_ADR, 0x10000000}, {"adrp", F_ADR, 0x90000000},
    {"ldxr", F_LDX, 0x085f7c00}, {"ldxrb", F_LDX, 0x085f7c00}, {"ldxrh", F_LDX, 0x085f7c00},
    {"ldaxr", F_LDX, 0x085ffc00}, {"ldaxrb", F_LDX, 0x085ffc00}, {"ldaxrh", F_LDX, 0x085ffc00},
    {"ldar", F_LDX, 0x08dffc00}, {"ldarb", F_LDX, 0x08dffc00}, {"ldarh", F_LDX, 0x08dffc00},
    {"stlr", F_LDX, 0x089ffc00}, {"stlrb", F_LDX, 0x089ffc00}, {"stlrh", F_LDX, 0x089ffc00},
    {"stxr", F_STX, 0x08007c00}, {"stxrb", F_STX, 0x08007c00}, {"stxrh", F_STX, 0x08007c00},
    {"stlxr", F_STX, 0x0800fc00}, {"stlxrb", F_STX, 0x0800fc00}, {"stlxrh", F_STX, 0x0800fc00},
    {"ldxp", F_LDXP, 0x887f0000}, {"ldaxp", F_LDXP, 0x887f8000},
    {"stxp", F_STXP, 0x88200000}, {"stlxp", F_STXP, 0x88208000},
    {"mrs", F_MRS, 0}, {"msr", F_MSR, 0}, {"sys", F_SYS, 0}, {"sysl", F_SYSL, 0},
    {"dc", F_SYSOP, 0}, {"ic", F_SYSOP, 0}, {"at", F_SYSOP, 0}, {"tlbi", F_SYSOP, 0},
    {"ldp", F_PAIR, 0}, {"stp", F_PAIR, 0}, {"ldpsw", F_PAIR, 0}, {"ldnp", F_PAIR, 0}, {"stnp", F_PAIR, 0},
    {"fmov", F_FMOV, 0},
    {"fabs", F_FP1, 0x1e20c000}, {"fneg", F_FP1, 0x1e214000}, {"fsqrt", F_FP1, 0x1e21c000},
    {"fadd", F_FP2, 0x1e202800}, {"fsub", F_FP2, 0x1e203800}, {"fmul", F_FP2, 0x1e200800},
    {"fdiv", F_FP2, 0x1e201800},
    {"fmax", F_FP2, 0x1e204800}, {"fmin", F_FP2, 0x1e205800}, {"fmaxnm", F_FP2, 0x1e206800},
    {"fminnm", F_FP2, 0x1e207800}, {"fnmul", F_FP2, 0x1e208800},
    {"frintn", F_FP1, 0x1e244000}, {"frintp", F_FP1, 0x1e24c000}, {"frintm", F_FP1, 0x1e254000},
    {"frintz", F_FP1, 0x1e25c000}, {"frinta", F_FP1, 0x1e264000}, {"frintx", F_FP1, 0x1e274000},
    {"frinti", F_FP1, 0x1e27c000},
    {"fcvtns", F_FCVTI, 0x1e200000}, {"fcvtnu", F_FCVTI, 0x1e210000}, {"fcvtas", F_FCVTI, 0x1e240000},
    {"fcvtau", F_FCVTI, 0x1e250000}, {"fcvtps", F_FCVTI, 0x1e280000}, {"fcvtpu", F_FCVTI, 0x1e290000},
    {"fcvtms", F_FCVTI, 0x1e300000}, {"fcvtmu", F_FCVTI, 0x1e310000}, {"fcvtzs", F_FCVTI, 0x1e380000},
    {"fcvtzu", F_FCVTI, 0x1e390000},
    {"scvtf", F_ICVTF, 0x1e220000}, {"ucvtf", F_ICVTF, 0x1e230000},
    {"fcsel", F_FCSEL, 0x1e200c00}, {"fcmp", F_FCMP, 0x1e202000}, {"fcmpe", F_FCMP, 0x1e202010},
    {"fccmp", F_FCCMP, 0x1e200400}, {"fccmpe", F_FCCMP, 0x1e200410}, {"fcvt", F_FCVT, 0},
    {"fmadd", F_FP3, 0x1f000000}, {"fmsub", F_FP3, 0x1f008000}, {"fnmadd", F_FP3, 0x1f200000},
    {"fnmsub", F_FP3, 0x1f208000},
    {0, 0, 0}
};

static void instruction(char *m)
{
    const Op *o;
    Reg a, b, c, zr;
    u32 sf, e, bits;
    i64 x, y;
    int cc, type;

    split_ops();
    zr.cls = 'x';
    zr.n = 31;
    zr.sp = 0;
    for (o = optab; o->name; o++)
        if (strcmp(o->name, m) == 0)
            break;
    if (!o->name) {
        /* b.eq and beq */
        if (m[0] == 'b' && (cc = cond_of(m + 1 + (m[1] == '.'))) >= 0) {
            put_fix(0x54000000 | (u32)cc, FX_BR19, V(0));
            return;
        }
        if (load_store(m))
            return;
        err("unknown instruction", m);
    }
    e = o->enc;
    switch (o->fmt) {
    case F_ADDSUB:
        a = R(0);
        b = R(1);
        addsub(SF(a) | SF(b), e, a, b, 2);
        break;
    case F_CMP:
        a = R(0);
        zr.cls = a.cls;
        addsub(SF(a), e, zr, a, 1);
        break;
    case F_NEG:
        a = R(0);
        zr.cls = a.cls;
        addsub(SF(a), e, a, zr, 1);
        break;
    case F_LOGIC:
        a = R(0);
        b = R(1);
        logical(SF(a), e, a, b, 2);
        break;
    case F_TST:
        a = R(0);
        logical(SF(a), e, zr, a, 1);
        break;
    case F_MVN:
        a = R(0);
        logical(SF(a), e, a, zr, 1);
        break;
    case F_MOV:
        a = R(0);
        if (!is_reg(1)) {
            mov_imm(a, (u64)K(1));
        } else {
            b = R(1);
            if (a.sp || b.sp)
                put(SF(a) | 0x11000000 | (u32)b.n << 5 | a.n);
            else
                put(SF(a) | 0x2a0003e0 | (u32)b.n << 16 | a.n);
        }
        break;
    case F_MOVW: {
        static const char *const mods[] = {"abs_g0_nc", "abs_g1_nc", "abs_g2_nc", "abs_g3", "abs_g0_s", "abs_g1_s",
                                           "abs_g2_s", "abs_g0", "abs_g1", "abs_g2"};
        u32 k, hw = 0;
        a = R(0);
        for (k = 0; k < 10; k++)
            if (modifier(1, mods[k]))
                break;
        x = K(1);
        if (k < 10) {
            hw = k < 4 ? k : k < 7 ? k - 4 : k - 7;
            if (k >= 4 && k < 7 && x < 0) {
                /* a signed value: movz turns into movn */
                x = ~x;
                e = 0x12800000;
            }
            x = (i64)((u64)x >> 16 * hw & 0xffff);
        } else if (shift_op(2, &type, &y) == 1) {
            hw = (u32)y / 16;
        }
        if (x < 0 || x > 0xffff)
            err("immediate out of range", 0);
        put(SF(a) | e | hw << 21 | (u32)x << 5 | a.n);
        break;
    }
    case F_SHIFT:
        a = R(0);
        b = R(1);
        sf = SF(a);
        bits = sf ? 64 : 32;
        if (is_reg(2)) {
            put(sf | (0x1ac02000 + (e << 10)) | (u32)R(2).n << 16 | (u32)b.n << 5 | a.n);
        } else {
            u32 s = (u32)K(2) & (bits - 1), n = sf ? 0x80400000U : 0;
            if (e == 0)
                put(n | 0x53000000 | ((bits - s) & (bits - 1)) << 16 | (bits - 1 - s) << 10 | (u32)b.n << 5 | a.n);
            else if (e == 3)
                put(n | 0x13800000 | (u32)b.n << 16 | s << 10 | (u32)b.n << 5 | a.n);
            else
                put(n | (e == 1 ? 0x53000000U : 0x13000000U) | s << 16 | (bits - 1) << 10 | (u32)b.n << 5 | a.n);
        }
        break;
    case F_BFM:
    case F_BFX:
    case F_BFI:
        a = R(0);
        b = R(1);
        sf = SF(a);
        bits = sf ? 64 : 32;
        x = K(2);
        y = K(3);
        if (o->fmt == F_BFX) {
            y = x + y - 1;
        } else if (o->fmt == F_BFI) {
            x = (bits - x) & (bits - 1);
            y = y - 1;
        }
        put((sf ? 0x80400000U : 0) | e | ((u32)x & 63) << 16 | ((u32)y & 63) << 10 | (u32)b.n << 5 | a.n);
        break;
    case F_EXT:
        a = R(0);
        b = R(1);
        put((a.cls == 'x' && !(e & 0x40000000) ? 0x80400000U : 0) | e | (u32)b.n << 5 | a.n);
        break;
    case F_EXTR:
        a = R(0);
        put((SF(a) ? 0x80400000U : 0) | e | (u32)R(2).n << 16 | ((u32)K(3) & 63) << 10 | (u32)R(1).n << 5 | a.n);
        break;
    case F_RRR:
        a = R(0);
        put(SF(a) | e | (u32)R(2).n << 16 | (u32)R(1).n << 5 | a.n);
        break;
    case F_RRRR:
        a = R(0);
        put(SF(a) | e | (u32)R(2).n << 16 | (u32)R(3).n << 10 | (u32)R(1).n << 5 | a.n);
        break;
    case F_NGC:
        a = R(0);
        put(SF(a) | e | (u32)R(1).n << 16 | a.n);
        break;
    case F_RR:
        a = R(0);
        put(SF(a) | e | (u32)R(1).n << 5 | a.n);
        break;
    case F_REV:
        a = R(0);
        put((SF(a) ? 0xdac00c00U : 0x5ac00800U) | (u32)R(1).n << 5 | a.n);
        break;
    case F_CSEL:
        a = R(0);
        put(SF(a) | e | (u32)R(2).n << 16 | C(3) << 12 | (u32)R(1).n << 5 | a.n);
        break;
    case F_CSET:
        a = R(0);
        put(SF(a) | e | (C(1) ^ 1) << 12 | a.n);
        break;
    case F_CINC:
        a = R(0);
        b = R(1);
        put(SF(a) | e | (u32)b.n << 16 | (C(2) ^ 1) << 12 | (u32)b.n << 5 | a.n);
        break;
    case F_CCMP:
        a = R(0);
        if (is_reg(1))
            e |= (u32)R(1).n << 16;
        else
            e |= 0x800 | ((u32)K(1) & 31) << 16;
        put(SF(a) | e | C(3) << 12 | (u32)a.n << 5 | ((u32)K(2) & 15));
        break;
    case F_B:
        put_fix(e, FX_B26, V(0));
        break;
    case F_CBZ:
        a = R(0);
        put_fix(SF(a) | e | a.n, FX_BR19, V(1));
        break;
    case F_TBZ:
        a = R(0);
        x = K(1);
        put_fix(e | ((u32)x >> 5) << 31 | ((u32)x & 31) << 19 | a.n, FX_TB14, V(2));
        break;
    case F_BR:
        put(e | (nops ? (u32)R(0).n : 30) << 5);
        break;
    case F_FIXED:
        put(e);
        break;
    case F_EXC:
        put(e | ((u32)K(0) & 0xffff) << (e ? 5 : 0));
        break;
    case F_HINT:
        if (m[0] == 'b') {
            /* bti, bti c, bti j, bti jc */
            if (nops)
                e |= (strchr(ops[0], 'c') ? 0x40U : 0) | (strchr(ops[0], 'j') ? 0x80U : 0);
            put(e);
        } else {
            put(e | ((u32)K(0) & 127) << 5);
        }
        break;
    case F_BARRIER:
        x = 15;
        if (nops) {
            char nm[16];
            lower(nm, ops[0], sizeof nm);
            x = named(barriers, nm);
            if (x < 0)
                x = K(0);
        }
        put((e & ~0xf00U) | ((u32)x & 15) << 8);
        break;
    case F_ADR:
        a = R(0);
        put_fix(e | a.n, e >> 31 ? FX_ADRP : FX_ADR, V(1));
        break;
    case F_LDX:
    case F_STX:
    case F_LDXP:
    case F_STXP: {
        int k = o->fmt == F_STX || o->fmt == F_STXP;
        size_t n = strlen(m);
        a = R(k);
        if (o->fmt == F_LDX || o->fmt == F_STX) {
            e |= m[n - 1] == 'b' ? 0 : m[n - 1] == 'h' ? 0x40000000U : a.cls == 'x' ? 0xc0000000U : 0x80000000U;
            b = zr;
        } else {
            e |= a.cls == 'x' ? 0x40000000U : 0;
            b = R(k + 1);
            e |= (u32)b.n << 10;
        }
        if (k)
            e |= (u32)R(0).n << 16;
        put(e | (u32)M(nops - 1).base << 5 | a.n);
        break;
    }
    case F_MRS:
        put(0xd5200000 | sysreg(1) << 5 | R(0).n);
        break;
    case F_MSR:
        if (!is_reg(1)) {
            char nm[16];
            lower(nm, ops[0], sizeof nm);
            cc = named(pstates, nm);
            if (cc < 0)
                err("unknown processor state field", ops[0]);
            put(0xd500401f | ((u32)cc >> 3) << 16 | ((u32)K(1) & 15) << 8 | ((u32)cc & 7) << 5);
        } else {
            put(0xd5000000 | sysreg(0) << 5 | R(1).n);
        }
        break;
    case F_SYS:
    case F_SYSL: {
        int k = o->fmt == F_SYSL;
        u32 cn = 0, cm = 0;
        const char *q;
        if (nops < 4 + k)
            err("missing operand", 0);
        q = ops[k + 1] + 1;
        if ((ops[k + 1][0] | 32) != 'c' || !num_at(&q, &cn))
            err("expected Cn", 0);
        q = ops[k + 2] + 1;
        if ((ops[k + 2][0] | 32) != 'c' || !num_at(&q, &cm))
            err("expected Cm", 0);
        put((k ? 0xd5280000U : 0xd5080000U) | ((u32)K(k) & 7) << 16 | cn << 12 | cm << 8 |
            ((u32)K(k + 3) & 7) << 5 | (k ? (u32)R(0).n : nops > 4 ? (u32)R(4).n : 31));
        break;
    }
    case F_SYSOP: {
        char nm[32];
        if (!nops)
            err("missing operand", 0);
        nm[0] = m[0];
        nm[1] = ':';
        lower(nm + 2, ops[0], sizeof nm - 2);
        cc = named(sysops, nm);
        if (cc < 0)
            err("unknown system operation", ops[0]);
        put(0xd5000000 | (u32)cc << 5 | (nops > 1 ? (u32)R(1).n : 31));
        break;
    }
    case F_PAIR:
        pair(m);
        break;
    case F_FP1:
        a = R(0);
        put(e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)R(1).n << 5 | a.n);
        break;
    case F_FP2:
        a = R(0);
        put(e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)R(2).n << 16 | (u32)R(1).n << 5 | a.n);
        break;
    case F_FCVTI:
        a = R(0);
        b = R(1);
        put(SF(a) | e | (b.cls == 'd' ? 0x00400000 : 0) | (u32)b.n << 5 | a.n);
        break;
    case F_ICVTF:
        a = R(0);
        b = R(1);
        put(SF(b) | e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)b.n << 5 | a.n);
        break;
    case F_FCSEL:
        a = R(0);
        put(e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)R(2).n << 16 | C(3) << 12 | (u32)R(1).n << 5 | a.n);
        break;
    case F_FCMP:
        a = R(0);
        e |= (a.cls == 'd' ? 0x00400000 : 0) | (u32)a.n << 5;
        put(is_reg(1) ? e | (u32)R(1).n << 16 : e | 8); /* else #0.0 */
        break;
    case F_FCCMP:
        a = R(0);
        put(e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)R(1).n << 16 | C(3) << 12 | (u32)a.n << 5 | ((u32)K(2) & 15));
        break;
    case F_FCVT:
        a = R(0);
        put((a.cls == 'd' ? 0x1e22c000 : 0x1e624000) | (u32)R(1).n << 5 | a.n);
        break;
    case F_FP3:
        a = R(0);
        put(e | (a.cls == 'd' ? 0x00400000 : 0) | (u32)R(2).n << 16 | (u32)R(3).n << 10 | (u32)R(1).n << 5 | a.n);
        break;
    case F_FMOV:
        a = R(0);
        b = R(1);
        c = a.cls == 'x' || a.cls == 'w' ? b : a;
        e = c.cls == 'd' ? 0x00400000 : 0;
        if (a.cls == b.cls)
            put(0x1e204000 | e | (u32)b.n << 5 | a.n);
        else if (c.cls == a.cls)
            put((SF(b) ? 0x80000000U : 0) | 0x1e270000 | e | (u32)b.n << 5 | a.n);
        else
            put((SF(a) ? 0x80000000U : 0) | 0x1e260000 | e | (u32)b.n << 5 | a.n);
        break;
    }
}

/* ---- directives ---- */

static void do_align(u64 a, int fill)
{
    Buf *b = B();
    u64 to;
    if (a < 2)
        return;
    if (a > go->align[cur])
        go->align[cur] = (u32)a;
    to = align_up(b->len, a);
    if ((go->secflags[cur] & SF_EXEC) && !fill && !(go->secflags[cur] & SF_NOBITS) && opt_arch == ARCH_X64) {
        while (b->len < to)
            buf_u8(b, 0x90);
    } else if ((go->secflags[cur] & SF_EXEC) && !fill && !(go->secflags[cur] & SF_NOBITS)) {
        while (b->len < to && (b->len & 3))
            buf_u8(b, 0);
        while (b->len < to)
            buf_u32(b, 0xd503201f);
    } else {
        grow_sec(to - b->len, fill);
    }
}

static void strings(int zero)
{
    Buf *b = B();
    do {
        skip_ws();
        if (*p != '"')
            err("expected a string", 0);
        for (p++; *p && *p != '"';) {
            u32 c = (u8)*p++;
            if (c == '\\') {
                c = (u8)*p++;
                switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'x':
                    for (c = 0;; p++) {
                        int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ?
                                (*p | 32) - 'a' + 10 : -1;
                        if (d < 0)
                            break;
                        c = c * 16 + (u32)d;
                    }
                    break;
                default:
                    if (c >= '0' && c <= '7') {
                        int n = 1;
                        c -= '0';
                        while (n++ < 3 && *p >= '0' && *p <= '7')
                            c = c * 8 + (u32)(*p++ - '0');
                    }
                }
            }
            buf_u8(b, c & 0xff);
        }
        if (*p)
            p++;
        if (zero)
            buf_u8(b, 0);
    } while (accept_ch(','));
}

static u32 name_arg(void)
{
    char *s;
    skip_ws();
    s = p;
    while (is_ident(*p))
        p++;
    if (p == s)
        err("expected a name", 0);
    return intern(s, (size_t)(p - s));
}

static void set_symbol(u32 name, Val v)
{
    if (!v.add && !v.sub) {
        Ext *e = ext_of(name);
        e->isabs = 1;
        e->val = v.k;
    } else if (v.add && !v.sub && defined(v.add)) {
        u32 s = obj_named_sym(go, name);
        go->syms[s].sec = go->syms[v.add - 1].sec;
        go->syms[s].value = go->syms[v.add - 1].value + (u64)v.k;
        go->syms[s].kind = go->syms[v.add - 1].kind;
    } else {
        err("cannot give this value to a symbol:", intern_str(name));
    }
}

static void pool(void)
{
    u32 k, n = 0;
    for (k = 0; k < nlits; k++) {
        Lit *l = &lits[k];
        if (l->sec != cur) {
            lits[n++] = *l;
            continue;
        }
        buf_align(B(), (size_t)1 << l->size);
        go->syms[l->sym].sec = (u16)cur;
        go->syms[l->sym].value = here();
        data(l->size, l->v);
    }
    nlits = n;
}

static void directive(const char *d)
{
    static const char *const ignored[] = {".arch", ".arch_extension", ".cpu", ".file", ".ident", ".loc", ".local",
                                          ".warning", ".print", ".end", ".eject", ".list", ".nolist", ".title",
                                          ".addrsig", ".addrsig_sym", ".protected", ".internal", 0};
    static const char *const sized[] = {".byte", ".hword", ".short", ".2byte", ".word", ".long", ".int", ".4byte",
                                        ".quad", ".xword", ".8byte", ".dword", 0};
    static const u8 sizes[] = {0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
    u32 k, name, s;
    i64 n, x, y;

    for (k = 0; sized[k]; k++)
        if (strcmp(d, sized[k]) == 0) {
            do
                data(sizes[k], expr());
            while (accept_ch(','));
            return;
        }
    if (strcmp(d, ".macro") == 0) {
        def_macro();
        p = (char *)"";
    } else if (strcmp(d, ".rept") == 0 || strcmp(d, ".irp") == 0 || strcmp(d, ".irpc") == 0) {
        repeat(d[1] == 'r' ? 0 : d[4] ? 2 : 1);
    } else if (strcmp(d, ".purgem") == 0) {
        Macro **m;
        name = name_arg();
        for (m = &macros; *m; m = &(*m)->next)
            if ((*m)->name == name) {
                *m = (*m)->next; /* the body may still be running: it is not freed */
                break;
            }
    } else if (strcmp(d, ".exitm") == 0) {
        while (src->up) {
            int was = src->ismacro;
            ncond = src->ncond;
            pop_src();
            if (was)
                break;
        }
    } else if (strcmp(d, ".section") == 0) {
        section_args(0);
    } else if (strcmp(d, ".pushsection") == 0) {
        if (nsecstack == 32)
            err("sections nested too deeply", 0);
        secstack[nsecstack][0] = cur;
        secstack[nsecstack++][1] = prev;
        section_args(1);
    } else if (strcmp(d, ".popsection") == 0) {
        if (!nsecstack)
            err(".popsection without .pushsection", 0);
        cur = secstack[--nsecstack][0];
        prev = secstack[nsecstack][1];
    } else if (strcmp(d, ".previous") == 0) {
        switch_to(prev);
    } else if (strcmp(d, ".subsection") == 0) {
        switch_to(sec_sub(sec_base(cur), (u32)const_expr()));
    } else if (strcmp(d, ".text") == 0) {
        switch_to(SEC_TEXT);
    } else if (strcmp(d, ".data") == 0) {
        switch_to(SEC_DATA);
    } else if (strcmp(d, ".bss") == 0) {
        switch_to(SEC_BSS);
    } else if (strcmp(d, ".align") == 0 || strcmp(d, ".p2align") == 0 || strcmp(d, ".balign") == 0) {
        n = const_expr();
        x = accept_ch(',') && (skip_ws(), *p != ',') ? const_expr() : 0;
        do_align(d[1] == 'b' ? (u64)n : (u64)1 << n, (int)x);
        p += strlen(p);
    } else if (strcmp(d, ".org") == 0) {
        Val v = expr();
        if (v.sub || (v.add && (!defined(v.add) || go->syms[v.add - 1].sec != cur)))
            err("bad .org", 0);
        n = v.k + (v.add ? (i64)go->syms[v.add - 1].value : 0);
        if (n < (i64)here())
            err(".org moves backwards", 0);
        grow_sec((u64)n - here(), 0);
        p += strlen(p);
    } else if (strcmp(d, ".space") == 0 || strcmp(d, ".skip") == 0 || strcmp(d, ".zero") == 0) {
        n = const_expr();
        grow_sec((u64)n, accept_ch(',') ? (int)const_expr() : 0);
    } else if (strcmp(d, ".fill") == 0) {
        n = const_expr();
        x = accept_ch(',') ? const_expr() : 1;
        y = accept_ch(',') ? const_expr() : 0;
        while (n-- > 0)
            for (k = 0; k < (u32)x; k++)
                grow_sec(1, 0), B()->p[here() - 1] = (u8)(k < 8 ? (u64)y >> 8 * k : 0);
    } else if (strcmp(d, ".incbin") == 0) {
        char path[1024];
        size_t len = 0, skip, count;
        u8 *bytes;
        skip_ws();
        if (*p != '"')
            err("a file name is expected after .incbin", 0);
        for (p++; *p && *p != '"' && len + 1 < sizeof path; p++)
            path[len++] = *p;
        path[len] = 0;
        if (*p == '"')
            p++;
        bytes = os_read_file(path, &len);
        if (!bytes)
            err("cannot read", path);
        skip = accept_ch(',') ? (size_t)const_expr() : 0;
        if (skip > len)
            skip = len;
        count = accept_ch(',') ? (size_t)const_expr() : len - skip;
        if (count > len - skip)
            count = len - skip;
        buf_put(B(), bytes + skip, count);
    } else if (strcmp(d, ".ascii") == 0) {
        strings(0);
    } else if (strcmp(d, ".asciz") == 0 || strcmp(d, ".string") == 0) {
        strings(1);
    } else if (strcmp(d, ".inst") == 0) {
        do
            put((u32)const_expr());
        while (accept_ch(','));
    } else if (strcmp(d, ".globl") == 0 || strcmp(d, ".global") == 0 || strcmp(d, ".weak") == 0 ||
               strcmp(d, ".hidden") == 0) {
        do {
            s = obj_named_sym(go, name_arg());
            if (d[1] == 'w')
                go->syms[s].weak = 1;
            else if (d[1] == 'h')
                go->syms[s].hidden = 1;
            else
                go->syms[s].global = 1;
        } while (accept_ch(','));
    } else if (strcmp(d, ".type") == 0) {
        s = obj_named_sym(go, name_arg());
        accept_ch(',');
        skip_ws();
        if (*p)
            go->syms[s].kind = p[1] == 'f' ? 1 : p[1] == 'o' ? 2 : 0;
        p += strlen(p);
    } else if (strcmp(d, ".size") == 0) {
        s = obj_named_sym(go, name_arg());
        accept_ch(',');
        go->syms[s].size = (u64)const_expr();
    } else if (strcmp(d, ".set") == 0 || strcmp(d, ".equ") == 0 || strcmp(d, ".equiv") == 0) {
        name = name_arg();
        accept_ch(',');
        set_symbol(name, expr());
    } else if (strcmp(d, ".unreq") == 0) {
        name = name_arg();
        for (k = naliases; k-- > 0;)
            if (aliases[k].name == name) {
                aliases[k] = aliases[--naliases];
                break;
            }
    } else if (strcmp(d, ".ltorg") == 0 || strcmp(d, ".pool") == 0) {
        pool();
    } else if (strcmp(d, ".err") == 0 || strcmp(d, ".error") == 0) {
        err(".error", p);
    } else if (strncmp(d, ".cfi_", 5) == 0) {
        p += strlen(p);
    } else {
        for (k = 0; ignored[k]; k++)
            if (strcmp(d, ignored[k]) == 0) {
                p += strlen(p);
                return;
            }
        err("unknown directive", d);
    }
    skip_ws();
    if (*p)
        err("unexpected text after directive:", p);
}

#include "x64_asm.h"

static void statement(char *s)
{
    char *w, *q, mn[32];
    size_t n;
    u32 name;
    Macro *m;

    p = s;
    for (;;) {
        skip_ws();
        w = p;
        while (is_ident(*p))
            p++;
        n = (size_t)(p - w);
        if (!n || *p != ':')
            break;
        p++;
        if (SKIPPING)
            continue;
        if (w[0] >= '0' && w[0] <= '9') {
            name = intern(w, n);
            name = local_label(w, n, ++ext_of(name)->lcount);
        } else {
            name = intern(w, n);
        }
        name = obj_named_sym(go, name);
        if (go->syms[name].sec != SEC_UNDEF)
            err("label defined twice:", intern_str(go->syms[name].name));
        go->syms[name].sec = (u16)cur;
        go->syms[name].value = here();
    }
    if (!n) {
        skip_ws();
        if (*p && !SKIPPING)
            err("syntax error at", p);
        return;
    }
    if (w[0] == '.' && conditional(w))
        return;
    if (SKIPPING)
        return;
    q = p;
    while (*q == ' ' || *q == '\t')
        q++;
    if (*q == '=' && q[1] != '=') {
        p = q + 1;
        set_symbol(intern(w, n), expr());
        return;
    }
    if (word_is(q, ".req")) {
        Reg r;
        q += 4;
        while (*q == ' ' || *q == '\t')
            q++;
        if (!reg_at(q, &r))
            err("expected a register", 0);
        if (naliases == capaliases) {
            capaliases = capaliases ? capaliases * 2 : 32;
            aliases = (Alias *)xrealloc(aliases, capaliases * sizeof(Alias));
        }
        aliases[naliases].name = intern(w, n);
        aliases[naliases++].r = r;
        return;
    }
    m = macros ? find_macro(intern(w, n)) : 0;
    if (m) {
        expand(m);
        return;
    }
    if (n >= sizeof mn)
        err("unknown instruction", w);
    memcpy(mn, w, n);
    mn[n] = 0;
    lower(mn, mn, sizeof mn);
    if (mn[0] == '.')
        directive(mn);
    else if (opt_arch == ARCH_X64)
        x64_instruction(mn);
    else
        instruction(mn);
}

/* ---- entry points ---- */

void gas_begin(Obj *o)
{
    go = o;
    cur = prev = SEC_TEXT;
    nsecstack = nsubsec = ncond = 0;
    nfixes = naliases = nlits = 0;
    dot_sym = 0;
    macro_serial = 0;
    while (macros) {
        Macro *m = macros;
        u32 k;
        macros = m->next;
        for (k = 0; k < m->nparams; k++)
            free(m->pdef[k]);
        free(m->body);
        free(m);
    }
    if (ext)
        memset(ext, 0, capext * sizeof(Ext));
}

/* Assembles text, starting in section sec. A whole file (file != 0) may
   leave other sections current; inline assembly must not. */
void gas_text(const char *text, size_t len, int sec, const char *path, u32 line, int file)
{
    char *t = (char *)xmalloc(len + 1), *s;
    u32 depth = nsecstack, conds = ncond;
    memcpy(t, text, len);
    t[len] = 0;
    strip(t, file);
    as_path = path;
    as_line = line;
    cur = prev = (u32)sec;
    src = 0;
    push_src(t, 0);
    while ((s = stmt_next()) != 0)
        statement(s);
    pop_src();
    if (ncond != conds)
        err("missing .endif", 0);
    if (!file && (nsecstack != depth || cur != (u32)sec))
        err("inline assembly ends in a different section", 0);
}

/* The symbol + 1 that the k-th unsettled reference is to, 0 past the last. */
u32 gas_pending(u32 k)
{
    return k < nfixes ? fixes[k].v.add : 0;
}

void gas_end(void)
{
    u32 k;
    /* literals not placed by .ltorg go at the end of their sections */
    while (nlits) {
        cur = lits[0].sec;
        pool();
    }
    /* the name of a section stands for its start */
    for (k = SEC_NSTORE; k < go->nsec; k++) {
        u32 y = go->secname[k] ? obj_find_sym(go, go->secname[k]) : 0;
        if (y && go->syms[y - 1].sec == SEC_UNDEF) {
            go->syms[y - 1].sec = (u16)k;
            go->syms[y - 1].value = 0;
            go->syms[y - 1].global = 0;
        }
    }
    /* subsections join the end of their sections */
    for (k = 0; k < nsubsec; k++) {
        u32 base = subsec[k][0], s = subsec[k][2], off, j;
        if (!go->sec[s].len)
            continue;
        cur = base;
        do_align(go->align[s], 0);
        off = here();
        if (go->secflags[base] & SF_NOBITS)
            go->sec[base].len += go->sec[s].len;
        else
            buf_put(&go->sec[base], go->sec[s].p, go->sec[s].len);
        go->sec[s].len = 0;
        for (j = 0; j < go->nsyms; j++)
            if (go->syms[j].sec == s) {
                go->syms[j].sec = (u16)base;
                go->syms[j].value += off;
            }
        for (j = 0; j < go->nrel[s]; j++) {
            ObjReloc r = go->rel[s][j];
            obj_add_reloc(go, (int)base, r.off + off, (int)r.type, r.sym, r.addend);
        }
        go->nrel[s] = 0;
        for (j = 0; j < nfixes; j++)
            if (fixes[j].sec == s) {
                fixes[j].sec = base;
                fixes[j].off += off;
            }
    }
    nsubsec = 0;
    for (k = 0; k < nfixes; k++) {
        as_line = fixes[k].line;
        settle(&fixes[k], 1);
    }
    nfixes = 0;
    /* assembler-local labels do not reach the symbol table */
    for (k = 0; k < go->nsyms; k++) {
        const char *n = intern_str(go->syms[k].name);
        if (n[0] == '.' && n[1] == 'L' && !go->syms[k].global)
            go->syms[k].name = 0;
    }
}

/* A .s file, or with cpp a .S one, which goes through the preprocessor first. */
Obj *gas_file(const char *path, int cpp)
{
    size_t len;
    char *text;
    Obj *o;
    Buf b;
    memset(&b, 0, sizeof b);
    if (cpp) {
        pp_asm = 1;
        pp_define("__ASSEMBLER__");
        pp_open(path);
        pp_text(&b);
        pp_asm = 0;
        text = (char *)b.p;
        len = b.len;
    } else {
        text = (char *)os_read_file(path, &len);
        if (!text)
            die("cannot read %s", path);
    }
    o = obj_new(path);
    o->align[SEC_TEXT] = 4;
    gas_begin(o);
    gas_text(text, len, SEC_TEXT, path, 1, 1);
    gas_end();
    free(text);
    return o;
}
