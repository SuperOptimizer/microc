/* PTX text to modules: the module level (variables, function headers) is read when a module is loaded;
   a function's body is decoded into Ins when it is first launched or called from a launched one. */
#include "ptx.h"

#define NOSLOT 0x7fffffff

/* ---- errors ---- */

static char errbuf[600];

void mc_set_error(const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof errbuf)
        n = sizeof errbuf - 1;
    memcpy(errbuf, msg, n);
    errbuf[n] = 0;
}

const char *mc_error(void) { return errbuf; }

/* ---- memory that lives as long as its module ---- */

typedef struct Chunk {
    struct Chunk *next;
    size_t used, cap;
} Chunk;

static void *aalloc(void **arena, size_t n)
{
    Chunk *c = (Chunk *)*arena;
    void *r;
    n = (n + 15) & ~(size_t)15;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        c = (Chunk *)malloc(sizeof(Chunk) + 16 + cap);
        if (!c) {
            fprintf(stderr, "mcuda: out of memory\n");
            exit(1);
        }
        c->next = (Chunk *)*arena;
        c->used = 0;
        c->cap = cap;
        *arena = c;
    }
    r = (char *)(c + 1) + c->used;
    c->used += n;
    memset(r, 0, n);
    return r;
}

static void afree(void **arena)
{
    Chunk *c = (Chunk *)*arena, *n;
    for (; c; c = n) {
        n = c->next;
        free(c);
    }
    *arena = 0;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "mcuda: out of memory\n");
        exit(1);
    }
    return p;
}

/* ---- names ---- */

enum { SY_REG = 1, SY_MEM, SY_EPARAM, SY_GVAR, SY_FUNC };

typedef struct Sym {
    struct Sym *hnext, *snext;
    const char *name;
    u32 len, hash;
    u8 kind, type;
    u8 rd, wr; /* a register: some instruction reads it, writes it */
    s32 slot;
    u32 off;
    u64 size;
    Gvar *g;
    McFunc *fn;
} Sym;

#define NBUCKET 4096
typedef struct Tab {
    Sym *b[NBUCKET];
} Tab;

static u32 hashn(const char *s, u32 n)
{
    u32 h = 2166136261U;
    while (n--)
        h = (h ^ (u8)*s++) * 16777619U;
    return h;
}

static Sym *tab_find(Tab *t, const char *s, u32 n, u32 h)
{
    Sym *y;
    for (y = t->b[h % NBUCKET]; y; y = y->hnext)
        if (y->hash == h && y->len == n && memcmp(y->name, s, n) == 0)
            return y;
    return 0;
}

static void tab_add(Tab *t, Sym *y)
{
    y->hnext = t->b[y->hash % NBUCKET];
    t->b[y->hash % NBUCKET] = y;
}

/* ---- the parser's state ---- */

typedef struct Label {
    struct Label *next;
    const char *name;
    u32 len;
    s32 at;
} Label;

typedef struct Patch {
    const char *name;
    u32 len, ins, line;
    s32 xt; /* for brx.idx: the place in xtra that gets the target, else -1 */
} Patch;

typedef struct BTab {
    struct BTab *next;
    const char *name; /* the label of a .branchtargets list */
    u32 len, n;
    const char **tname;
    u32 *tlen;
} BTab;

typedef struct CEnt {
    u64 val;
    u32 kind;
    s32 slot;
    s32 next;
} CEnt;

enum { O_REG = 1, O_IMM, O_FLT, O_SYM, O_MEM, O_VEC, O_SPECIAL, O_NAME };

typedef struct Opd {
    int kind;
    Sym *sym;
    s32 slot, slot2; /* slot2: the second destination of a|b */
    int type, type2, neg;
    u64 imm;
    double fv;
    s64 off;
    int fx;
    int mbase;     /* O_MEM: O_REG, O_SYM or O_IMM */
    int nvec, vec; /* O_VEC: elements at p->vpool[vec...] */
    const char *name;
    u32 nlen;
} Opd;

typedef struct P {
    McModule *m;
    McFunc *f;
    const char *s;
    u32 line;
    jmp_buf jb;
    Tab *ftab;
    void *tmp; /* arena of one function's decoding */
    Sym *scope[64];
    int depth;
    /* output */
    Ins *code;
    u32 *lines;
    u32 ncode, capcode;
    s32 *xtra;
    u32 nxtra, capxtra;
    CEnt *cents;
    u32 ncent, capcent;
    s32 chash[1024];
    Fix *fix;
    u32 nfix, capfix;
    Label *labels[256];
    Patch *patches;
    BTab *btabs;
    Label *lastlabel;
    u32 npatch, cappatch;
    u32 nreg, mem;
    Sym *unwritten; /* the first register that is read but that no instruction writes */
    u32 nunwritten;
    Opd vpool[64];
    int nvpool;
    McFunc **callees;
    u32 ncallee, capcallee;
    const char *mn; /* the mnemonic being decoded */
} P;

static void perr(P *p, const char *msg, const char *arg)
{
    char b[500];
    const char *mod = p->m && p->m->name ? p->m->name : "ptx";
    if (arg && strlen(arg) > 150)
        arg = "(long name)";
    if (p->f)
        sprintf(b, "%.60s:%u: %.60s: ", mod, p->line, p->f->name);
    else
        sprintf(b, "%.60s:%u: ", mod, p->line);
    sprintf(b + strlen(b), msg, arg ? arg : "");
    mc_set_error(b);
    longjmp(p->jb, 1);
}

/* ---- characters ---- */

static void skipws(P *p)
{
    const char *s = p->s;
    for (;;) {
        if (*s == '\n') {
            p->line++;
            s++;
        } else if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\f' || *s == '\v')
            s++;
        else if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n')
                s++;
        } else if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                if (*s == '\n')
                    p->line++;
                s++;
            }
            if (*s)
                s += 2;
        } else
            break;
    }
    p->s = s;
}

static void skipline(P *p)
{
    while (*p->s && *p->s != '\n')
        p->s++;
}

static int peekc(P *p)
{
    skipws(p);
    return (u8)*p->s;
}

static int accept(P *p, int c)
{
    if (peekc(p) == c) {
        p->s++;
        return 1;
    }
    return 0;
}

static void expect(P *p, int c)
{
    char b[2];
    if (!accept(p, c)) {
        b[0] = (char)c;
        b[1] = 0;
        perr(p, "'%s' expected", b);
    }
}

static int isidc(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '%';
}

/* A mnemonic or directive: identifier characters, dots and "::". Returns its length; the text is in buf. */
static int word(P *p, char *buf, int cap)
{
    const char *s;
    int n = 0;
    skipws(p);
    s = p->s;
    for (;;) {
        if (isidc((u8)*s) || *s == '.')
            ;
        else if (s[0] == ':' && s[1] == ':')
            buf[n++] = *s++;
        else
            break;
        if (n >= cap - 2)
            perr(p, "name too long", 0);
        buf[n++] = *s++;
    }
    buf[n] = 0;
    p->s = s;
    return n;
}

/* An identifier: *name points into the text. */
static u32 ident(P *p, const char **name)
{
    const char *s;
    skipws(p);
    s = p->s;
    *name = s;
    while (isidc((u8)*s))
        s++;
    p->s = s;
    return (u32)(s - *name);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A numeric literal with an optional sign: O_IMM (value in imm) or O_FLT (value in fv). */
static void number(P *p, Opd *o)
{
    const char *s;
    int neg = 0, d;
    u64 v = 0;
    skipws(p);
    s = p->s;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
        while (*s == ' ')
            s++;
        if (*s == '-') { /* an offset below its base, as mcc writes it: [%r1+-28] */
            neg = 1;
            s++;
        }
    }
    while (*s == ' ')
        s++;
    if (*s < '0' || *s > '9') {
        if (*s != '.')
            perr(p, "number expected", 0);
    }
    if (s[0] == '0' && (s[1] == 'f' || s[1] == 'F' || s[1] == 'd' || s[1] == 'D') && hexval((u8)s[2]) >= 0) {
        int dbl = s[1] == 'd' || s[1] == 'D';
        s += 2;
        while ((d = hexval((u8)*s)) >= 0) {
            v = v << 4 | (u64)d;
            s++;
        }
        o->kind = O_FLT;
        o->fv = dbl ? mc_bitsd(v) : (double)mc_bitsf((u32)v);
        o->type = dbl ? T_F64 : T_F32;
        o->imm = v; /* the bits as written: a NaN keeps its payload */
        if (neg)
            o->fv = -o->fv;
        p->s = s;
        return;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while ((d = hexval((u8)*s)) >= 0) {
            v = v << 4 | (u64)d;
            s++;
        }
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && (s[2] == '0' || s[2] == '1')) {
        s += 2;
        while (*s == '0' || *s == '1')
            v = v << 1 | (u64)(*s++ - '0');
    } else {
        const char *q = s;
        while (*q >= '0' && *q <= '9')
            q++;
        if (*q == '.' || *q == 'e' || *q == 'E') {
            char *end;
            o->kind = O_FLT;
            o->fv = strtod(s, &end);
            o->type = T_F64;
            o->imm = mc_dbits(o->fv);
            if (neg)
                o->fv = -o->fv;
            p->s = end;
            return;
        }
        if (s[0] == '0') {
            while (*s >= '0' && *s <= '7')
                v = v << 3 | (u64)(*s++ - '0');
        } else
            while (*s >= '0' && *s <= '9')
                v = v * 10 + (u64)(*s++ - '0');
    }
    if (*s == 'U' || *s == 'u')
        s++;
    o->kind = O_IMM;
    o->imm = neg ? (u64)0 - v : v;
    p->s = s;
}

static u64 uint_lit(P *p)
{
    Opd o;
    memset(&o, 0, sizeof o);
    number(p, &o);
    if (o.kind != O_IMM)
        perr(p, "integer expected", 0);
    return o.imm;
}

/* ---- types ---- */

static const struct {
    const char *name;
    u8 t, bits;
} typetab[] = {{"b8", T_B8, 8},        {"b16", T_B16, 16},        {"b32", T_B32, 32},        {"b64", T_B64, 64},
               {"u8", T_U8, 8},        {"u16", T_U16, 16},        {"u32", T_U32, 32},        {"u64", T_U64, 64},
               {"s8", T_S8, 8},        {"s16", T_S16, 16},        {"s32", T_S32, 32},        {"s64", T_S64, 64},
               {"f16", T_F16, 16},     {"f32", T_F32, 32},        {"f64", T_F64, 64},        {"bf16", T_BF16, 16},
               {"f16x2", T_F16X2, 32}, {"bf16x2", T_BF16X2, 32},  {"tf32", T_TF32, 32},      {"pred", T_PRED, 1},
               {"b128", T_B128, 128},  {"e4m3x2", T_E4M3X2, 16},  {"e5m2x2", T_E5M2X2, 16},  {"e2m1x2", T_E2M1X2, 8},
               {"e2m3x2", T_E2M3X2, 16}, {"e3m2x2", T_E3M2X2, 16}, {"ue8m0x2", T_UE8M0X2, 16}, {0, 0, 0}};

static int type_of(const char *s)
{
    int i;
    for (i = 0; typetab[i].name; i++)
        if (strcmp(typetab[i].name, s) == 0)
            return typetab[i].t;
    return 0;
}

int mc_tbits(int t)
{
    int i;
    for (i = 0; typetab[i].name; i++)
        if (typetab[i].t == t)
            return typetab[i].bits;
    return 0;
}

static const char *tname(int t)
{
    int i;
    for (i = 0; typetab[i].name; i++)
        if (typetab[i].t == t)
            return typetab[i].name;
    return "?";
}

#define tbits mc_tbits
static int tsigned(int t) { return t >= T_S8 && t <= T_S64; }
static int tfloat(int t) { return t == T_F16 || t == T_F32 || t == T_F64 || t == T_BF16; }
static int tint(int t) { return t >= T_B8 && t <= T_S64; }

/* ---- declarations ---- */

typedef struct Decl {
    int type, vec, is_array, unsized;
    u64 align, nelem, count; /* count: the n of %r<n>, else 0 */
    const char *name;
    u32 nlen;
    const char *init;
    u32 init_line;
} Decl;

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void skip_parens(P *p)
{
    int depth = 0;
    do {
        int c = peekc(p);
        if (!c)
            perr(p, "unexpected end of text", 0);
        if (c == '(')
            depth++;
        else if (c == ')')
            depth--;
        p->s++;
    } while (depth);
}

/* What stands between a storage class and the names: .align N, .attribute(...), .v2..v4, the type, .ptr and what
   qualifies it. */
static void decl_type(P *p, Decl *d)
{
    char w[64];
    memset(d, 0, sizeof *d);
    d->vec = 1;
    for (;;) {
        if (peekc(p) != '.')
            perr(p, "type expected", 0);
        word(p, w, sizeof w);
        if (streq(w, ".align"))
            d->align = uint_lit(p);
        else if (streq(w, ".attribute"))
            skip_parens(p);
        else if (streq(w, ".v2"))
            d->vec = 2;
        else if (streq(w, ".v3"))
            d->vec = 3;
        else if (streq(w, ".v4"))
            d->vec = 4;
        else if ((d->type = type_of(w + 1)) != 0)
            break;
        else
            perr(p, "unsupported type %s", w);
    }
    for (;;) {
        const char *save = p->s;
        u32 line = p->line;
        if (peekc(p) != '.')
            break;
        word(p, w, sizeof w);
        if (streq(w, ".ptr") || streq(w, ".global") || streq(w, ".const") || streq(w, ".shared") || streq(w, ".local") ||
            streq(w, ".texref") || streq(w, ".samplerref") || streq(w, ".surfref"))
            continue;
        if (streq(w, ".align")) {
            uint_lit(p);
            continue;
        }
        p->s = save;
        p->line = line;
        break;
    }
}

/* name, name<n>, name[n][m], with an optional initialiser whose text is only skipped here */
static void declarator(P *p, Decl *d)
{
    d->nelem = 1;
    d->is_array = d->unsized = 0;
    d->count = 0;
    d->init = 0;
    d->nlen = ident(p, &d->name);
    if (!d->nlen)
        perr(p, "name expected", 0);
    if (*p->s == '<') {
        p->s++;
        d->count = uint_lit(p);
        expect(p, '>');
    }
    while (accept(p, '[')) {
        d->is_array = 1;
        if (peekc(p) == ']')
            d->unsized = 1, d->nelem = 0;
        else
            d->nelem *= uint_lit(p);
        expect(p, ']');
    }
    if (accept(p, '=')) {
        int depth = 0, c;
        skipws(p);
        d->init = p->s;
        d->init_line = p->line;
        for (;;) {
            c = peekc(p);
            if (!c)
                perr(p, "unexpected end of text in an initialiser", 0);
            if (c == '{')
                depth++;
            else if (c == '}')
                depth--;
            else if ((c == ',' || c == ';') && depth == 0)
                break;
            p->s++;
        }
    }
}

static u64 decl_size(Decl *d)
{
    u64 es = d->type == T_PRED ? 1 : (u64)tbits(d->type) / 8;
    return es * (u64)d->vec * d->nelem;
}

static u64 decl_align(Decl *d)
{
    u64 es = d->type == T_PRED ? 1 : (u64)tbits(d->type) / 8;
    if (es > 8)
        es = 8;
    return d->align > es ? d->align : es;
}

static u64 alignup(u64 v, u64 a) { return a ? (v + a - 1) / a * a : v; }

static char *astrn(void **arena, const char *s, u32 n)
{
    char *r = (char *)aalloc(arena, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static Sym *mod_sym(P *p, const char *name, u32 n)
{
    Sym *y;
    u32 h = hashn(name, n);
    y = tab_find((Tab *)p->m->symtab, name, n, h);
    if (!y) {
        y = (Sym *)aalloc(&p->m->arena, sizeof *y);
        y->name = name;
        y->len = n;
        y->hash = h;
        tab_add((Tab *)p->m->symtab, y);
    }
    return y;
}

/* .global / .const / .shared at module level (also .shared inside a function body) */
static void global_decl(P *p, int space, int is_extern)
{
    Decl d;
    McModule *m = p->m;
    decl_type(p, &d);
    do {
        Sym *y;
        Gvar *g;
        declarator(p, &d);
        y = mod_sym(p, d.name, d.nlen);
        if (y->kind == SY_GVAR) {
            g = y->g;
            if (!is_extern && g->is_extern)
                g->is_extern = 0; /* the definition after a declaration */
            else
                continue;
        } else if (y->kind)
            perr(p, "%s is declared twice", astrn(&m->arena, d.name, d.nlen));
        else {
            g = (Gvar *)aalloc(&m->arena, sizeof *g);
            g->name = astrn(&m->arena, d.name, d.nlen);
            if (m->lastgvar)
                m->lastgvar->next = g;
            else
                m->gvars = g;
            m->lastgvar = g;
            y->kind = SY_GVAR;
            y->g = g;
            g->is_extern = (u8)is_extern;
        }
        g->space = (u8)space;
        g->type = (u8)d.type;
        g->size = decl_size(&d);
        g->nelem = d.nelem * (u64)d.vec;
        g->align = decl_align(&d);
        g->init = d.init;
        g->init_line = d.init_line;
        if (g->is_extern)
            continue;
        if (space == SP_SHARED) {
            g->off = alignup(m->shared_static, g->align);
            m->shared_static = g->off + g->size;
        } else {
            u64 a = g->align < 16 ? 16 : g->align;
            g->addr = (u8 *)aalloc(&m->arena, g->size + a + 16);
            g->addr = (u8 *)alignup((u64)g->addr, a);
            g->owned = 1;
        }
    } while (accept(p, ','));
    expect(p, ';');
}

/* The list of a function header between parentheses. */
static void formals(P *p, McFunc *f, int ret)
{
    Param tmp[256];
    int n = 0;
    char w[64];
    Decl d;
    if (accept(p, ')'))
        return;
    do {
        Param *q = &tmp[n];
        if (n >= 256)
            perr(p, "too many parameters", 0);
        word(p, w, sizeof w);
        memset(q, 0, sizeof *q);
        if (streq(w, ".reg"))
            q->isreg = 1;
        else if (!streq(w, ".param"))
            perr(p, ".param or .reg expected, not %s", w);
        decl_type(p, &d);
        declarator(p, &d);
        q->type = (u8)d.type;
        q->size = (u32)decl_size(&d);
        q->align = (u32)decl_align(&d);
        q->name = d.name;
        q->nlen = d.nlen;
        if (q->isreg) {
            if (f->is_entry)
                perr(p, "a kernel takes .param parameters", 0);
            q->slot = (s32)f->nformal++;
        } else if (f->is_entry) {
            q->off = (u32)alignup(f->param_bytes, q->align);
            f->param_bytes = q->off + q->size;
        } else {
            q->off = (u32)alignup(f->formal_mem, q->align);
            f->formal_mem = q->off + q->size;
        }
        n++;
    } while (accept(p, ','));
    expect(p, ')');
    if (n) {
        Param *v = (Param *)aalloc(&p->m->arena, (size_t)n * sizeof(Param));
        memcpy(v, tmp, (size_t)n * sizeof(Param));
        if (ret)
            f->rets = v, f->nrets = n;
        else
            f->params = v, f->nparams = n;
    }
}

static void skip_body(P *p)
{
    int depth = 1;
    const char *s = p->s;
    while (depth) {
        int c = (u8)*s;
        if (!c) {
            p->s = s;
            perr(p, "unexpected end of text in a function body", 0);
        }
        if (c == '\n')
            p->line++;
        else if (c == '{')
            depth++;
        else if (c == '}')
            depth--;
        else if (c == '"') {
            s++;
            while (*s && *s != '"' && *s != '\n')
                s++;
            if (*s != '"')
                continue;
        } else if (c == '/' && s[1] == '/') {
            while (*s && *s != '\n')
                s++;
            continue;
        } else if (c == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                if (*s == '\n')
                    p->line++;
                s++;
            }
            if (*s)
                s++;
        }
        s++;
    }
    p->s = s;
}

static void func_decl(P *p, int is_entry, int is_extern)
{
    McModule *m = p->m;
    McFunc tmp, *f;
    Sym *y;
    const char *name;
    u32 nlen, i;
    char w[64];
    memset(&tmp, 0, sizeof tmp);
    f = &tmp;
    f->mod = m;
    f->is_entry = (u8)is_entry;
    p->f = 0;
    if (!is_entry && accept(p, '('))
        formals(p, f, 1);
    nlen = ident(p, &name);
    if (!nlen)
        perr(p, "function name expected", 0);
    f->name = astrn(&m->arena, name, nlen);
    if (accept(p, '('))
        formals(p, f, 0);
    for (;;) {
        int c = peekc(p);
        if (c == '{' || c == ';')
            break;
        if (c != '.')
            perr(p, "'{' or ';' expected after the header of %s", f->name);
        word(p, w, sizeof w);
        if (streq(w, ".maxntid") || streq(w, ".reqntid")) {
            u32 *v = w[1] == 'm' ? f->maxntid : f->reqntid;
            v[0] = (u32)uint_lit(p);
            v[1] = v[2] = 1;
            for (i = 1; i < 3 && accept(p, ','); i++)
                v[i] = (u32)uint_lit(p);
        } else if (streq(w, ".minnctapersm") || streq(w, ".maxnctapersm") || streq(w, ".maxnreg") ||
                   streq(w, ".reqnctapercluster") || streq(w, ".maxclusterrank") || streq(w, ".abi_preserve") ||
                   streq(w, ".abi_preserve_control"))
            uint_lit(p);
        else if (streq(w, ".noreturn"))
            f->noreturn = 1;
        else if (streq(w, ".explicitcluster") || streq(w, ".blocksareclusters"))
            ;
        else if (streq(w, ".attribute"))
            skip_parens(p);
        else if (streq(w, ".pragma")) {
            while (*p->s && *p->s != ';')
                p->s++;
            expect(p, ';');
        } else
            perr(p, "unknown directive %s in a function header", w);
    }
    if (accept(p, '{')) {
        f->has_body = 1;
        f->body = p->s;
        f->body_line = p->line;
        skip_body(p);
    } else
        expect(p, ';');
    y = mod_sym(p, name, nlen);
    if (y->kind == SY_FUNC) {
        McFunc *old = y->fn;
        if (old->has_body || !f->has_body)
            return; /* a repeated declaration, or one after the definition */
        tmp.next = old->next;
        *old = tmp;
        old->is_extern = 0;
        return;
    }
    if (y->kind)
        perr(p, "%s is declared twice", f->name);
    f = (McFunc *)aalloc(&m->arena, sizeof *f);
    *f = tmp;
    f->is_extern = (u8)(is_extern && !f->has_body);
    if (m->lastfunc)
        m->lastfunc->next = f;
    else
        m->funcs = f;
    m->lastfunc = f;
    y->kind = SY_FUNC;
    y->fn = f;
}

/* ---- the module level ---- */

/* Set by whoever knows of modules that are not loaded yet: loads them and says whether any was new. Asked when a
   name is found in no loaded module. */
int (*mc_more_modules)(void);

static McModule *modules; /* every loaded module, for names that one declares .extern and another defines */

static Gvar *find_gvar(McModule *m, const char *name)
{
    Sym *y = tab_find((Tab *)m->symtab, name, (u32)strlen(name), hashn(name, (u32)strlen(name)));
    return y && y->kind == SY_GVAR ? y->g : 0;
}

McFunc *mc_module_func(McModule *m, const char *name)
{
    Sym *y = tab_find((Tab *)m->symtab, name, (u32)strlen(name), hashn(name, (u32)strlen(name)));
    return y && y->kind == SY_FUNC ? y->fn : 0;
}

/* The storage of an extern variable: bound by hand, or defined by another loaded module. */
static u8 *gvar_addr(Gvar *g)
{
    McModule *o;
    Gvar *d;
    int again;
    if (g->addr || !g->is_extern)
        return g->addr;
    for (again = 0; again < 2; again++) {
        for (o = modules; o; o = o->next)
            if ((d = find_gvar(o, g->name)) != 0 && !d->is_extern && d->space != SP_SHARED) {
                g->addr = d->addr;
                if (!g->size)
                    g->size = d->size;
                return g->addr;
            }
        if (!mc_more_modules || !mc_more_modules())
            break;
    }
    return g->addr;
}

static void write_elem(u8 *dst, int type, Opd *o, P *p)
{
    u64 v = o->imm;
    u32 w;
    float f;
    if (o->kind == O_FLT) {
        if (type == T_F32) {
            f = (float)o->fv;
            w = o->type == T_F32 ? (u32)o->imm ^ (o->fv < 0 && !((u32)o->imm >> 31) ? 0x80000000U : 0) : mc_fbits(f);
            v = w;
        } else if (type == T_F64)
            v = mc_dbits(o->fv);
        else if (type == T_F16)
            v = mc_enc(&mc_fmt_f16, o->fv, MC_RN, 0);
        else if (type == T_BF16)
            v = mc_enc(&mc_fmt_bf16, o->fv, MC_RN, 0);
        else
            perr(p, "a floating-point value initialises an integer", 0);
    } else if (type == T_F32)
        v = mc_fbits((float)(s64)v);
    else if (type == T_F64)
        v = mc_dbits((double)(s64)v);
    memcpy(dst, &v, type == T_PRED ? 1 : (size_t)tbits(type) / 8);
}

static void init_gvar(P *p, Gvar *g)
{
    u64 i = 0, es = g->type == T_PRED ? 1 : (u64)tbits(g->type) / 8;
    int depth = 0;
    p->s = g->init;
    p->line = g->init_line;
    for (;;) {
        int c = peekc(p);
        Opd o;
        if (c == '{') {
            depth++;
            p->s++;
            continue;
        }
        if (c == '}') {
            depth--;
            p->s++;
            if (depth <= 0)
                break;
            continue;
        }
        if (c == ',') {
            if (depth == 0)
                break;
            p->s++;
            continue;
        }
        if (c == ';' || !c)
            break;
        memset(&o, 0, sizeof o);
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')
            number(p, &o);
        else {
            const char *name;
            u32 n = ident(p, &name);
            int generic = 0;
            Sym *y;
            if (n == 7 && memcmp(name, "generic", 7) == 0 && accept(p, '(')) {
                n = ident(p, &name);
                generic = 1;
            }
            y = tab_find((Tab *)p->m->symtab, name, n, hashn(name, n));
            if (!n || !y)
                perr(p, "unknown name in the initialiser of %s", g->name);
            if (generic)
                expect(p, ')');
            o.kind = O_IMM;
            if (y->kind == SY_FUNC)
                o.imm = (u64)y->fn;
            else if (y->g->space == SP_SHARED)
                o.imm = y->g->off;
            else
                o.imm = (u64)gvar_addr(y->g);
            c = peekc(p);
            if (c == '+' || c == '-') {
                Opd k;
                memset(&k, 0, sizeof k);
                number(p, &k);
                o.imm += k.imm;
            }
        }
        if (i < g->nelem)
            write_elem(g->addr + i * es, g->type, &o, p);
        else
            perr(p, "too many values initialise %s", g->name);
        i++;
        if (depth == 0)
            break;
    }
}

McModule *mc_module_load(const char *ptx, const char *name)
{
    P *p = (P *)calloc(1, sizeof(P));
    McModule *m = (McModule *)calloc(1, sizeof(McModule));
    Gvar *g;
    char w[64];
    int is_extern = 0;
    if (!p || !m || !ptx) {
        mc_set_error("mc_module_load: no memory or no text");
        free(p);
        free(m);
        return 0;
    }
    mc_fp_init();
    m->ptx = ptx;
    m->symtab = calloc(1, sizeof(Tab));
    m->name = astrn(&m->arena, name ? name : "ptx", (u32)strlen(name ? name : "ptx"));
    p->m = m;
    p->s = ptx;
    p->line = 1;
    if (setjmp(p->jb)) {
        free(p);
        free(m->symtab);
        afree(&m->arena);
        free(m);
        return 0;
    }
    for (;;) {
        int c = peekc(p);
        if (!c)
            break;
        if (c != '.')
            perr(p, "a directive is expected at the top level", 0);
        word(p, w, sizeof w);
        if (streq(w, ".version")) {
            skipws(p);
            m->ver = atoi(p->s) * 100;
            while (*p->s >= '0' && *p->s <= '9')
                p->s++;
            if (*p->s == '.') {
                p->s++;
                m->ver += atoi(p->s);
            }
            skipline(p);
        } else if (streq(w, ".target")) {
            skipws(p);
            if (p->s[0] == 's' && p->s[1] == 'm' && p->s[2] == '_')
                m->sm = atoi(p->s + 3);
            skipline(p);
        } else if (streq(w, ".address_size")) {
            if (uint_lit(p) != 64)
                perr(p, "only .address_size 64 is supported", 0);
        } else if (streq(w, ".file") || streq(w, ".loc"))
            skipline(p);
        else if (streq(w, ".section")) {
            while (*p->s && *p->s != '{' && *p->s != '\n')
                p->s++;
            if (*p->s == '{') {
                p->s++;
                skip_body(p);
            }
        } else if (streq(w, ".pragma")) {
            while (*p->s && *p->s != ';')
                p->s++;
            expect(p, ';');
        } else if (streq(w, ".extern")) {
            is_extern = 1;
            continue;
        } else if (streq(w, ".visible") || streq(w, ".weak") || streq(w, ".common"))
            continue;
        else if (streq(w, ".global"))
            global_decl(p, SP_GLOBAL, is_extern);
        else if (streq(w, ".const"))
            global_decl(p, SP_CONST, is_extern);
        else if (streq(w, ".shared"))
            global_decl(p, SP_SHARED, is_extern);
        else if (streq(w, ".entry"))
            func_decl(p, 1, is_extern);
        else if (streq(w, ".func"))
            func_decl(p, 0, is_extern);
        else
            perr(p, "unknown directive %s", w);
        is_extern = 0;
    }
    p->f = 0;
    m->shared_static = alignup(m->shared_static, 16);
    for (g = m->gvars; g; g = g->next)
        if (g->is_extern && g->space == SP_SHARED)
            g->off = m->shared_static; /* the dynamic part */
    for (g = m->gvars; g; g = g->next)
        if (g->init && g->addr)
            init_gvar(p, g);
    free(p);
    mcuda_lock(MC_LOCK_MODULE);
    m->next = modules;
    modules = m;
    mcuda_unlock(MC_LOCK_MODULE);
    return m;
}

static void free_func(McFunc *f)
{
    mc_jit_free(f);
    free(f->code);
    free(f->lines);
    free(f->xtra);
    free(f->ctemplate);
    free(f->fix);
}

void mc_module_free(McModule *m)
{
    McModule **pp;
    McFunc *f;
    if (!m)
        return;
    mcuda_lock(MC_LOCK_MODULE);
    for (pp = &modules; *pp; pp = &(*pp)->next)
        if (*pp == m) {
            *pp = m->next;
            break;
        }
    mcuda_unlock(MC_LOCK_MODULE);
    for (f = m->funcs; f; f = f->next) {
        free_func(f);
        free(f->srefs);
        free(f->callees);
    }
    free(m->symtab);
    afree(&m->arena);
    free(m);
}

void *mc_module_global(McModule *m, const char *name, u64 *size)
{
    Gvar *g = find_gvar(m, name);
    if (!g || g->space == SP_SHARED)
        return 0;
    gvar_addr(g);
    if (size)
        *size = g->size;
    return g->addr;
}

int mc_module_bind(McModule *m, const char *name, void *addr)
{
    Gvar *g = find_gvar(m, name);
    if (!g || g->space == SP_SHARED)
        return -1;
    g->addr = (u8 *)addr;
    return 0;
}

int mc_func_nparams(McFunc *f) { return f->nparams; }

int mc_func_param(McFunc *f, int i, u64 *size, u64 *align, u64 *offset)
{
    if (i < 0 || i >= f->nparams)
        return -1;
    if (size)
        *size = f->params[i].size;
    if (align)
        *align = f->params[i].align;
    if (offset)
        *offset = f->params[i].off;
    return 0;
}

u64 mc_func_param_bytes(McFunc *f) { return f->param_bytes; }
const char *mc_func_name(McFunc *f) { return f->name; }

static u64 shared_used(McFunc *f, u32 stamp);

void mc_func_limits(McFunc *f, u32 maxntid[3], u32 reqntid[3], u64 *static_shared)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (maxntid)
            maxntid[i] = f->maxntid[i];
        if (reqntid)
            reqntid[i] = f->reqntid[i];
    }
    if (static_shared) {
        /* A kernel is charged for the variables it uses, though a block gets room for all of the module's. */
        static u32 stamp = 0x40000000;
        *static_shared = 0;
        mcuda_lock(MC_LOCK_MODULE);
        if (f->decoded || mc_func_decode(f) == 0)
            *static_shared = alignup(shared_used(f, ++stamp), 16);
        mcuda_unlock(MC_LOCK_MODULE);
    }
}

/* ---- decoding a function body: slots, constants, names ---- */

static s32 newreg(P *p) { return (s32)p->nreg++; }

/* The slot of a constant. kind 0: the value itself; else an FX_ kind with val as its offset. */
static s32 cslot(P *p, u32 kind, u64 val)
{
    u32 h = (u32)((val ^ val >> 29 ^ kind * 7919U) * 2654435761U) >> 22;
    s32 i;
    CEnt *c;
    for (i = p->chash[h]; i >= 0; i = p->cents[i].next)
        if (p->cents[i].val == val && p->cents[i].kind == kind)
            return p->cents[i].slot;
    if (p->ncent == p->capcent) {
        p->capcent = p->capcent ? p->capcent * 2 : 64;
        p->cents = (CEnt *)xrealloc(p->cents, p->capcent * sizeof(CEnt));
    }
    c = &p->cents[p->ncent];
    c->val = val;
    c->kind = kind;
    c->slot = -1 - (s32)p->ncent;
    c->next = p->chash[h];
    p->chash[h] = (s32)p->ncent++;
    if (kind) {
        if (p->nfix == p->capfix) {
            p->capfix = p->capfix ? p->capfix * 2 : 16;
            p->fix = (Fix *)xrealloc(p->fix, p->capfix * sizeof(Fix));
        }
        p->fix[p->nfix].slot = c->slot;
        p->fix[p->nfix].kind = kind;
        p->fix[p->nfix].off = val;
        p->nfix++;
    }
    return c->slot;
}

static s32 emit(P *p, int op, int aux, s32 d, s32 a, s32 b, s32 c, s32 e)
{
    Ins *i;
    if (p->ncode == p->capcode) {
        p->capcode = p->capcode ? p->capcode * 2 : 256;
        p->code = (Ins *)xrealloc(p->code, p->capcode * sizeof(Ins));
        p->lines = (u32 *)xrealloc(p->lines, p->capcode * sizeof(u32));
    }
    i = &p->code[p->ncode];
    i->op = (u16)op;
    i->aux = (u16)aux;
    i->d = d;
    i->a = a;
    i->b = b;
    i->c = c;
    i->e = e;
    p->lines[p->ncode] = p->line;
    return (s32)p->ncode++;
}

static s32 xadd(P *p, s32 v)
{
    if (p->nxtra == p->capxtra) {
        p->capxtra = p->capxtra ? p->capxtra * 2 : 64;
        p->xtra = (s32 *)xrealloc(p->xtra, p->capxtra * sizeof(s32));
    }
    p->xtra[p->nxtra] = v;
    return (s32)p->nxtra++;
}

static Sym *declare(P *p, const char *name, u32 n, int kind)
{
    Sym *y = (Sym *)aalloc(&p->tmp, sizeof *y);
    y->name = name;
    y->len = n;
    y->hash = hashn(name, n);
    y->kind = (u8)kind;
    y->rd = y->wr = 0;
    tab_add(p->ftab, y);
    y->snext = p->scope[p->depth];
    p->scope[p->depth] = y;
    return y;
}

static void pop_scope(P *p)
{
    Sym *y;
    for (y = p->scope[p->depth]; y; y = y->snext) {
        p->ftab->b[y->hash % NBUCKET] = y->hnext; /* the newest names of their buckets, newest first */
        if (y->kind == SY_REG && y->rd && !y->wr) {
            p->unwritten = y;
            p->nunwritten++;
        }
    }
    p->scope[p->depth] = 0;
}

static Sym *lookup(P *p, const char *name, u32 n)
{
    u32 h = hashn(name, n);
    Sym *y = tab_find(p->ftab, name, n, h);
    return y ? y : tab_find((Tab *)p->m->symtab, name, n, h);
}

/* .reg / .local / .param inside a body */
static void local_decl(P *p, int kind)
{
    Decl d;
    decl_type(p, &d);
    if (kind == SY_REG && d.vec != 1)
        perr(p, "vector registers are not supported", 0);
    do {
        Sym *y;
        declarator(p, &d);
        if (kind == SY_REG) {
            if (d.is_array)
                perr(p, "a register cannot be an array", 0);
            if (d.count) {
                u64 i;
                for (i = 0; i < d.count; i++) {
                    char b[96];
                    char *nm;
                    if (d.nlen > 60)
                        perr(p, "register name too long", 0);
                    memcpy(b, d.name, d.nlen);
                    sprintf(b + d.nlen, "%lu", i);
                    nm = astrn(&p->tmp, b, (u32)strlen(b));
                    y = declare(p, nm, (u32)strlen(nm), SY_REG);
                    y->type = (u8)d.type;
                    y->slot = newreg(p);
                }
            } else {
                y = declare(p, d.name, d.nlen, SY_REG);
                y->type = (u8)d.type;
                y->slot = newreg(p);
            }
        } else {
            y = declare(p, d.name, d.nlen, SY_MEM);
            y->type = (u8)d.type;
            y->size = decl_size(&d);
            p->mem = (u32)alignup(p->mem, decl_align(&d) < 8 ? 8 : decl_align(&d));
            y->off = p->mem;
            p->mem += (u32)y->size;
            if (d.init)
                perr(p, "initialised .local variables are not supported", 0);
        }
    } while (accept(p, ','));
    expect(p, ';');
}

static const struct {
    const char *name;
    int fx; /* the first of x, y, z */
    int xyz;
} specials[] = {{"%tid", FX_TID_X, 1},
                {"%ntid", FX_NTID_X, 1},
                {"%ctaid", FX_CTAID_X, 1},
                {"%nctaid", FX_NCTAID_X, 1},
                {"%laneid", FX_LANEID, 0},
                {"%warpid", FX_WARPID, 0},
                {"%nwarpid", FX_NWARPID, 0},
                {"%gridid", FX_GRIDID, 0},
                {"%smid", FX_SMID, 0},
                {"%nsmid", FX_NSMID, 0},
                {"%lanemask_eq", FX_LANEMASK_EQ, 0},
                {"%lanemask_le", FX_LANEMASK_LE, 0},
                {"%lanemask_lt", FX_LANEMASK_LT, 0},
                {"%lanemask_ge", FX_LANEMASK_GE, 0},
                {"%lanemask_gt", FX_LANEMASK_GT, 0},
                {"%dynamic_smem_size", FX_DYN_SMEM, 0},
                {"%total_smem_size", FX_TOTAL_SMEM, 0},
                {"%aggr_smem_size", FX_TOTAL_SMEM, 0},
                {"%clock", -1, 0},
                {"%clock64", -1, 0},
                {"%is_explicit_cluster", FX_ZERO, 0},
                {"%cluster_ctarank", FX_ZERO, 0},
                {"%cluster_nctarank", FX_ONE, 0},
                {"%clusterid", FX_ZERO, 1},
                {"%nclusterid", FX_ONE, 1},
                {"%cluster_ctaid", FX_ZERO, 1},
                {"%cluster_nctaid", FX_ONE, 1},
                {"%reserved_smem_offset_begin", FX_ZERO, 0},
                {"%reserved_smem_offset_end", FX_ZERO, 0},
                {"%reserved_smem_offset_cap", FX_ZERO, 0},
                {0, 0, 0}};

/* Is the name a special register? Then o is filled in and the .x / .y / .z behind it is consumed. */
static int special(P *p, const char *name, u32 n, Opd *o)
{
    int i;
    if (name[0] != '%')
        return 0;
    for (i = 0; specials[i].name; i++)
        if (strlen(specials[i].name) == n && memcmp(name, specials[i].name, n) == 0)
            break;
    if (!specials[i].name) {
        if (n >= 8 && memcmp(name, "%envreg", 7) == 0) {
            o->kind = O_SPECIAL;
            o->fx = FX_ZERO;
            return 1;
        }
        if (n == 4 && memcmp(name, "%pm", 3) == 0) {
            o->kind = O_SPECIAL;
            o->fx = FX_ZERO;
            return 1;
        }
        return 0;
    }
    o->kind = O_SPECIAL;
    o->fx = specials[i].fx;
    if (specials[i].xyz) {
        int c;
        if (*p->s != '.' || (c = p->s[1]) < 'x' || c > 'z')
            perr(p, "%s needs .x, .y or .z", specials[i].name);
        p->s += 2;
        if (o->fx != FX_ZERO && o->fx != FX_ONE)
            o->fx += c - 'x';
    }
    return 1;
}

static void operand(P *p, Opd *o);

/* a register, a variable (+ offset) or a number, for the inside of [ ], { } and ( ) too */
static void primary(P *p, Opd *o)
{
    int c = peekc(p);
    memset(o, 0, sizeof *o);
    o->slot2 = NOSLOT;
    if (c == '!') {
        p->s++;
        o->neg = 1;
        c = peekc(p);
    }
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
        number(p, o);
        return;
    }
    if (!isidc(c))
        perr(p, "operand expected", 0);
    o->nlen = ident(p, &o->name);
    if (special(p, o->name, o->nlen, o))
        return;
    o->sym = lookup(p, o->name, o->nlen);
    if (!o->sym) {
        o->kind = O_NAME; /* a label */
        return;
    }
    if (o->sym->kind == SY_REG) {
        o->kind = O_REG;
        o->slot = o->sym->slot;
        o->type = o->sym->type;
        return;
    }
    o->kind = O_SYM;
    c = peekc(p);
    if (c == '+' || c == '-') {
        Opd k;
        memset(&k, 0, sizeof k);
        number(p, &k);
        if (k.kind != O_IMM)
            perr(p, "integer offset expected", 0);
        o->off = (s64)k.imm;
    }
}

static void operand(P *p, Opd *o)
{
    int c = peekc(p);
    if (c == '[') {
        Opd b;
        p->s++;
        primary(p, &b);
        memset(o, 0, sizeof *o);
        *o = b;
        o->mbase = b.kind;
        o->kind = O_MEM;
        if (b.kind == O_REG) {
            c = peekc(p);
            if (c == '+' || c == '-') {
                Opd k;
                memset(&k, 0, sizeof k);
                number(p, &k);
                if (k.kind != O_IMM)
                    perr(p, "integer offset expected", 0);
                o->off = (s64)k.imm;
            }
        } else if (b.kind != O_SYM && b.kind != O_IMM)
            perr(p, "bad address", 0);
        expect(p, ']');
        return;
    }
    if (c == '{' || c == '(') {
        int close = c == '{' ? '}' : ')', n = 0, first = p->nvpool;
        p->s++;
        if (peekc(p) != close)
            do {
                if (p->nvpool >= 64)
                    perr(p, "too many operands", 0);
                primary(p, &p->vpool[p->nvpool++]);
                n++;
            } while (accept(p, ','));
        expect(p, close);
        memset(o, 0, sizeof *o);
        o->kind = O_VEC;
        o->vec = first;
        o->nvec = n;
        o->type = c;
        return;
    }
    primary(p, o);
    if (o->kind == O_REG && peekc(p) == '|') {
        Opd q;
        p->s++;
        primary(p, &q);
        if (q.kind != O_REG)
            perr(p, "register expected after '|'", 0);
        o->slot2 = q.slot;
        o->type2 = q.type;
        if (q.sym)
            q.sym->wr = 1;
    }
}

static void note_sref(P *p, Gvar *g)
{
    McFunc *f = p->f;
    u32 i;
    for (i = 0; i < f->nsrefs; i++)
        if (f->srefs[i] == g)
            return;
    f->srefs = (Gvar **)xrealloc(f->srefs, (f->nsrefs + 1) * sizeof(Gvar *));
    f->srefs[f->nsrefs++] = g;
}

static McFunc *resolve_func(McFunc *f);

/* The constant slot that holds the address a variable operand stands for. */
static s32 symaddr(P *p, Sym *y, s64 off)
{
    switch (y->kind) {
    case SY_MEM:
        return cslot(p, FX_FRAME, (u64)(y->off + off));
    case SY_EPARAM:
        return cslot(p, FX_PARAM, (u64)(y->off + off));
    case SY_GVAR:
        if (y->g->space == SP_SHARED) {
            note_sref(p, y->g);
            return cslot(p, 0, y->g->off + (u64)off);
        }
        if (!gvar_addr(y->g))
            perr(p, "the extern variable %s is defined by no loaded module (mc_module_bind gives it storage)", y->g->name);
        return cslot(p, 0, (u64)y->g->addr + (u64)off);
    case SY_FUNC:
        y->fn->addr_taken = 1;
        return cslot(p, 0, (u64)resolve_func(y->fn));
    }
    perr(p, "bad operand", 0);
    return 0;
}

/* The slot of a source operand of an instruction of type t. */
static s32 S(P *p, Opd *o, int t)
{
    u64 v;
    int bits = tbits(t);
    switch (o->kind) {
    case O_REG:
        if (o->neg)
            perr(p, "'!' is not allowed here", 0);
        if (tbits(o->type) < bits && !(t == T_PRED))
            perr(p, "a register is narrower than its instruction (%s)", p->mn);
        return o->slot;
    case O_IMM:
        v = o->imm;
        if (t == T_F32)
            v = mc_fbits((float)(s64)v);
        else if (t == T_F64)
            v = mc_dbits((double)(s64)v);
        else if (t == T_PRED)
            v = v != 0;
        else if (bits < 64)
            v &= (1UL << bits) - 1;
        return cslot(p, 0, v);
    case O_FLT:
        if (t == T_F32)
            v = o->type == T_F32 ? (u64)(u32)o->imm ^ (o->fv < 0 && !(o->imm >> 31 & 1) ? 0x80000000UL : 0)
                                 : (u64)mc_fbits((float)o->fv);
        else if (t == T_F64)
            v = mc_dbits(o->fv);
        else if (t == T_F16)
            v = mc_enc(&mc_fmt_f16, o->fv, MC_RN, 0);
        else if (t == T_BF16)
            v = mc_enc(&mc_fmt_bf16, o->fv, MC_RN, 0);
        else {
            perr(p, "a floating-point constant in an integer instruction (%s)", p->mn);
            v = 0;
        }
        return cslot(p, 0, v);
    case O_SYM:
        return symaddr(p, o->sym, o->off);
    case O_SPECIAL:
        if (o->fx < 0)
            perr(p, "%%clock can only be the source of a mov", 0);
        return cslot(p, (u32)o->fx, 0);
    case O_NAME:
        perr(p, "unknown name %s", astrn(&p->tmp, o->name, o->nlen));
    }
    perr(p, "bad source operand of %s", p->mn);
    return 0;
}

static s32 D(P *p, Opd *o)
{
    if (o->kind != O_REG || o->neg)
        perr(p, "the destination of %s must be a register", p->mn);
    return o->slot;
}

/* An address operand as a base slot and a 32-bit displacement. */
static s32 A(P *p, Opd *o, s32 *disp)
{
    *disp = 0;
    if (o->kind != O_MEM)
        perr(p, "an address in [ ] is expected by %s", p->mn);
    if (o->mbase == O_REG) {
        if (o->off < -0x7fffffffL || o->off > 0x7fffffffL)
            perr(p, "address offset out of range", 0);
        *disp = (s32)o->off;
        if (tbits(o->type) < 32 || tfloat(o->type))
            perr(p, "bad address register in %s", p->mn);
        return o->slot;
    }
    if (o->mbase == O_SYM)
        return symaddr(p, o->sym, o->off);
    return cslot(p, 0, o->imm);
}

/* ---- mnemonics ---- */

typedef struct Mn {
    char buf[200];
    char *tok[24];
    u8 used[24];
    int n;
} Mn;

static void mn_split(P *p, Mn *m, const char *w)
{
    char *s;
    if (strlen(w) >= sizeof m->buf)
        perr(p, "mnemonic too long", 0);
    strcpy(m->buf, w);
    m->n = 0;
    s = m->buf;
    for (;;) {
        if (m->n >= 24)
            perr(p, "too many modifiers in %s", w);
        m->used[m->n] = 0;
        m->tok[m->n++] = s;
        s = strchr(s, '.');
        if (!s)
            break;
        *s++ = 0;
    }
    m->used[0] = 1;
}

/* Is the modifier there? It is then accounted for. */
static int mod(Mn *m, const char *name)
{
    int i;
    for (i = 1; i < m->n; i++)
        if (!m->used[i] && streq(m->tok[i], name)) {
            m->used[i] = 1;
            return 1;
        }
    return 0;
}

/* The next type among the modifiers, or 0. */
static int mtype(Mn *m)
{
    int i, t;
    for (i = 1; i < m->n; i++)
        if (!m->used[i] && (t = type_of(m->tok[i])) != 0) {
            m->used[i] = 1;
            return t;
        }
    return 0;
}

static int need_type(P *p, Mn *m)
{
    int t = mtype(m);
    if (!t)
        perr(p, "%s lacks a type", p->mn);
    return t;
}

static void mn_done(P *p, Mn *m)
{
    int i;
    char b[260];
    for (i = 1; i < m->n; i++)
        if (!m->used[i]) {
            sprintf(b, ".%.40s of %.180s", m->tok[i], p->mn);
            perr(p, "unsupported modifier %s", b);
        }
}

static int rmode(Mn *m)
{
    if (mod(m, "rn"))
        return MC_RN;
    if (mod(m, "rz"))
        return MC_RZ;
    if (mod(m, "rm"))
        return MC_RM;
    if (mod(m, "rp"))
        return MC_RP;
    if (mod(m, "rna"))
        return MC_RNA;
    return -1;
}

static void nops(P *p, int n, int want)
{
    if (n != want)
        perr(p, "wrong number of operands for %s", p->mn);
}

/* ---- instructions ---- */

/* a value of a 16-bit (or 8-bit) integer type as a 32-bit one, for the 32-bit operation that stands in */
static s32 widen(P *p, s32 s, int t)
{
    s32 r;
    if (!tsigned(t))
        return s;
    r = newreg(p);
    emit(p, tbits(t) == 8 ? OP_SX8_32 : OP_SX16_32, 0, r, s, 0, 0, 0);
    return r;
}

/* d = op(a[, b[, c]]) on integers of type t; t2 is the type of the second source if it differs (shift counts) */
static void int_op(P *p, int t, int t2, int op32, int op64, int aux, Opd *o, int nsrc)
{
    int bits = tbits(t), i;
    s32 s[3], d = D(p, &o[0]);
    s[0] = s[1] = s[2] = 0;
    if (!op32 || bits < 16 || bits > 64)
        perr(p, "%s does not exist for this type", p->mn);
    for (i = 0; i < nsrc; i++)
        s[i] = S(p, &o[i + 1], i == 1 && t2 ? t2 : t);
    if (bits == 64) {
        if (!op64)
            perr(p, "%s does not exist for 64-bit types", p->mn);
        emit(p, op64, aux, d, s[0], s[1], s[2], 0);
    } else if (bits == 32)
        emit(p, op32, aux, d, s[0], s[1], s[2], 0);
    else {
        s32 r = newreg(p);
        for (i = 0; i < nsrc; i++)
            if (!(i == 1 && t2))
                s[i] = widen(p, s[i], t);
        emit(p, op32, aux, r, s[0], s[1], s[2], 0);
        emit(p, OP_ZX16, 0, d, r, 0, 0, 0);
    }
}

static const struct IntOp {
    const char *name;
    u8 nsrc, shift;
    u16 u32op, s32op, u64op, s64op;
} intops[] = {{"add", 2, 0, OP_ADD32, OP_ADD32, OP_ADD64, OP_ADD64},
              {"sub", 2, 0, OP_SUB32, OP_SUB32, OP_SUB64, OP_SUB64},
              {"div", 2, 0, OP_DIVU32, OP_DIVS32, OP_DIVU64, OP_DIVS64},
              {"rem", 2, 0, OP_REMU32, OP_REMS32, OP_REMU64, OP_REMS64},
              {"abs", 1, 0, OP_ABS32, OP_ABS32, OP_ABS64, OP_ABS64},
              {"neg", 1, 0, OP_NEG32, OP_NEG32, OP_NEG64, OP_NEG64},
              {"min", 2, 0, OP_MINU32, OP_MINS32, OP_MINU64, OP_MINS64},
              {"max", 2, 0, OP_MAXU32, OP_MAXS32, OP_MAXU64, OP_MAXS64},
              {"shl", 2, 1, OP_SHL32, OP_SHL32, OP_SHL64, OP_SHL64},
              {"shr", 2, 1, OP_SHRU32, OP_SHRS32, OP_SHRU64, OP_SHRS64},
              {"and", 2, 0, OP_AND32, OP_AND32, OP_AND64, OP_AND64},
              {"or", 2, 0, OP_OR32, OP_OR32, OP_OR64, OP_OR64},
              {"xor", 2, 0, OP_XOR32, OP_XOR32, OP_XOR64, OP_XOR64},
              {"not", 1, 0, OP_NOT32, OP_NOT32, OP_NOT64, OP_NOT64},
              {"cnot", 1, 0, OP_CNOT32, OP_CNOT32, OP_CNOT64, OP_CNOT64},
              {"popc", 1, 0, OP_POPC32, OP_POPC32, OP_POPC64, OP_POPC64},
              {"clz", 1, 0, OP_CLZ32, OP_CLZ32, OP_CLZ64, OP_CLZ64},
              {"brev", 1, 0, OP_BREV32, OP_BREV32, OP_BREV64, OP_BREV64},
              {"sad", 3, 0, OP_SADU32, OP_SADS32, OP_SADU64, OP_SADS64},
              {0, 0, 0, 0, 0, 0, 0}};

static const struct FltOp {
    const char *name;
    u8 fg, nsrc;
    u16 op32, op32z, op64;
} fltops[] = {{"add", FG_ADD, 2, OP_FADD32, OP_FADD32Z, OP_FADD64},
              {"sub", FG_SUB, 2, OP_FSUB32, OP_FSUB32Z, OP_FSUB64},
              {"mul", FG_MUL, 2, OP_FMUL32, OP_FMUL32Z, OP_FMUL64},
              {"div", FG_DIV, 2, OP_FDIV32, OP_FDIV32Z, OP_FDIV64},
              {"fma", FG_FMA, 3, OP_FMA32, OP_FMA32Z, OP_FMA64},
              {"mad", FG_FMA, 3, OP_FMA32, OP_FMA32Z, OP_FMA64},
              {"min", FG_MIN, 2, OP_FMIN32, 0, OP_FMIN64},
              {"max", FG_MAX, 2, OP_FMAX32, 0, OP_FMAX64},
              {"abs", FG_ABS, 1, OP_FABS32, 0, OP_FABS64},
              {"neg", FG_NEG, 1, OP_FNEG32, 0, OP_FNEG64},
              {"rcp", FG_RCP, 1, OP_FRCP32, 0, 0},
              {"sqrt", FG_SQRT, 1, OP_FSQRT32, 0, OP_FSQRT64},
              {"rsqrt", FG_RSQRT, 1, OP_FRSQRT32, 0, 0},
              {"sin", FG_SIN, 1, 0, 0, 0},
              {"cos", FG_COS, 1, 0, 0, 0},
              {"lg2", FG_LG2, 1, 0, 0, 0},
              {"ex2", FG_EX2, 1, 0, 0, 0},
              {"tanh", FG_TANH, 1, 0, 0, 0},
              {"copysign", FG_COPYSIGN, 2, 0, 0, 0},
              {0, 0, 0, 0, 0, 0}};

static int is_fp_type(int t) { return tfloat(t) || t == T_F16X2 || t == T_BF16X2; }

static void flt_op(P *p, Mn *m, const struct FltOp *fo, int t, Opd *o, int n)
{
    int rm = rmode(m), ftz = mod(m, "ftz"), sat = mod(m, "sat"), nan = mod(m, "NaN"), xs = mod(m, "xorsign"), ab;
    int nsrc = fo->nsrc, fast;
    u32 flags;
    s32 s[3], d;
    mod(m, "approx");
    mod(m, "full");
    ab = (fo->fg == FG_MIN || fo->fg == FG_MAX) && mod(m, "abs");
    mn_done(p, m);
    if ((fo->fg == FG_MIN || fo->fg == FG_MAX) && n == 4)
        nsrc = 3;
    nops(p, n, nsrc + 1);
    d = D(p, &o[0]);
    s[0] = S(p, &o[1], t);
    s[1] = nsrc > 1 ? S(p, &o[2], t) : 0;
    s[2] = nsrc > 2 ? S(p, &o[3], t) : 0;
    fast = (rm < 0 || rm == MC_RN) && !sat && !nan && !xs && !ab && nsrc == fo->nsrc;
    if (fast && t == T_F32 && !ftz && fo->op32) {
        emit(p, fo->op32, 0, d, s[0], s[1], s[2], 0);
        return;
    }
    if (fast && t == T_F32 && ftz && fo->op32z) {
        emit(p, fo->op32z, 0, d, s[0], s[1], s[2], 0);
        return;
    }
    if (fast && t == T_F64 && fo->op64) {
        emit(p, fo->op64, 0, d, s[0], s[1], s[2], 0);
        return;
    }
    flags = (u32)(rm < 0 ? MC_RN : rm) | (ftz ? FF_FTZ : 0) | (sat ? FF_SAT : 0) | (nan ? FF_NAN : 0) | (ab ? FF_ABS : 0) |
            (xs ? FF_XORSIGN : 0) | (u32)t << FF_TYPE_SHIFT | (nsrc == 3 && fo->fg != FG_FMA ? FF_3SRC : 0);
    emit(p, OP_FGEN, fo->fg, d, s[0], s[1], s[2], (s32)flags);
}

static int cmp_code(P *p, Mn *m, int *force_unsigned)
{
    static const char *const names[] = {"eq", "ne", "lt", "le", "gt", "ge", "equ", "neu", "ltu", "leu", "gtu", "geu", "num", "nan", 0};
    static const char *const un[] = {"lo", "ls", "hi", "hs", 0};
    int i;
    *force_unsigned = 0;
    for (i = 0; names[i]; i++)
        if (mod(m, names[i]))
            return i;
    for (i = 0; un[i]; i++)
        if (mod(m, un[i])) {
            *force_unsigned = 1;
            return CMP_LT + i;
        }
    perr(p, "%s lacks a comparison", p->mn);
    return 0;
}

/* Compare a and b as type t into the predicate slot d. */
static void compare(P *p, int t, int cmp, int funs, s32 d, Opd *a, Opd *b)
{
    s32 sa = S(p, a, t), sb = S(p, b, t);
    int bits = tbits(t), base;
    if (tfloat(t)) {
        if (t == T_F16 || t == T_BF16) {
            s32 ra = newreg(p), rb = newreg(p);
            u32 spec = (u32)T_F32 | (u32)t << 5 | 2U << 19;
            emit(p, OP_CVT, 0, ra, sa, 0, 0, (s32)spec);
            emit(p, OP_CVT, 0, rb, sb, 0, 0, (s32)spec);
            sa = ra;
            sb = rb;
            t = T_F32;
        }
        emit(p, (t == T_F32 ? OP_SETP_F32 : OP_SETP_F64) + cmp, 0, d, sa, sb, 0, 0);
        return;
    }
    if (!tint(t) || bits < 16)
        perr(p, "%s cannot compare this type", p->mn);
    if (cmp > CMP_GE)
        perr(p, "%s: an unordered comparison of integers", p->mn);
    if (bits == 16) {
        if (!funs) {
            sa = widen(p, sa, t);
            sb = widen(p, sb, t);
        }
        bits = 32;
    }
    if (bits == 32)
        base = tsigned(t) && !funs ? OP_SETP_S32 : OP_SETP_U32;
    else
        base = tsigned(t) && !funs ? OP_SETP_S64 : OP_SETP_U64;
    emit(p, base + cmp, 0, d, sa, sb, 0, 0);
}

static int boolop(Mn *m)
{
    if (mod(m, "and"))
        return OP_PAND;
    if (mod(m, "or"))
        return OP_POR;
    if (mod(m, "xor"))
        return OP_PXOR;
    return 0;
}

static s32 pred_src(P *p, Opd *o)
{
    s32 s, r;
    int neg = o->neg;
    o->neg = 0;
    if (o->kind == O_REG && o->type != T_PRED)
        perr(p, "a predicate register is expected by %s", p->mn);
    s = S(p, o, T_PRED);
    if (!neg)
        return s;
    r = newreg(p);
    emit(p, OP_PNOT, 0, r, s, 0, 0, 0);
    return r;
}

static void do_setp(P *p, Mn *m, Opd *o, int n, int is_set)
{
    int funs, cmp = cmp_code(p, m, &funs), bop = boolop(m), dt = 0, t;
    s32 d, c = 0, r;
    if (is_set)
        dt = need_type(p, m);
    t = need_type(p, m);
    mod(m, "ftz");
    mn_done(p, m);
    nops(p, n, bop ? 4 : 3);
    if (bop)
        c = pred_src(p, &o[3]);
    if (!is_set) {
        d = D(p, &o[0]);
        if (!bop) {
            compare(p, t, cmp, funs, d, &o[1], &o[2]);
            if (o[0].slot2 != NOSLOT)
                emit(p, OP_PNOT, 0, o[0].slot2, d, 0, 0, 0);
            return;
        }
        r = newreg(p);
        compare(p, t, cmp, funs, r, &o[1], &o[2]);
        if (o[0].slot2 != NOSLOT) {
            s32 nr = newreg(p);
            emit(p, OP_PNOT, 0, nr, r, 0, 0, 0);
            emit(p, bop, 0, o[0].slot2, nr, c, 0, 0);
        }
        emit(p, bop, 0, d, r, c, 0, 0);
        return;
    }
    r = newreg(p);
    compare(p, t, cmp, funs, r, &o[1], &o[2]);
    if (bop)
        emit(p, bop, 0, r, r, c, 0, 0);
    d = D(p, &o[0]);
    if (dt == T_F32)
        emit(p, OP_SELP, 0, d, cslot(p, 0, 0x3f800000UL), cslot(p, 0, 0), r, 0);
    else if (tint(dt) && tbits(dt) >= 16)
        emit(p, OP_SELP, 0, d, cslot(p, 0, tbits(dt) == 64 ? ~0UL : (1UL << tbits(dt)) - 1), cslot(p, 0, 0), r, 0);
    else
        perr(p, "%s: unsupported result type", p->mn);
}

/* Extend the low `from` bits of slot s (signed or not) into d as a value of `to` bits. */
static void extend(P *p, s32 d, s32 s, int from, int sgn, int to)
{
    if (from >= to) {
        emit(p, to == 64 ? OP_MOV : to == 32 ? OP_MOV32 : to == 16 ? OP_ZX16 : OP_ZX8, 0, d, s, 0, 0, 0);
        return;
    }
    if (!sgn) {
        emit(p, from == 8 ? OP_ZX8 : from == 16 ? OP_ZX16 : OP_MOV32, 0, d, s, 0, 0, 0);
        return;
    }
    if (to == 64)
        emit(p, from == 8 ? OP_SX8_64 : from == 16 ? OP_SX16_64 : OP_SX32_64, 0, d, s, 0, 0, 0);
    else if (to == 32)
        emit(p, from == 8 ? OP_SX8_32 : OP_SX16_32, 0, d, s, 0, 0, 0);
    else {
        emit(p, OP_SX8_32, 0, d, s, 0, 0, 0);
        emit(p, OP_ZX16, 0, d, d, 0, 0, 0);
    }
}

static void do_cvt(P *p, Mn *m, Opd *o, int n)
{
    int irnd = 0, rm = -1, ftz, sat, relu, satf, pzo, dt, st, db, sb, rb, plain;
    s32 d, a, b = 0;
    u32 spec;
    if (mod(m, "rni"))
        irnd = 1, rm = MC_RN;
    else if (mod(m, "rzi"))
        irnd = 1, rm = MC_RZ;
    else if (mod(m, "rmi"))
        irnd = 1, rm = MC_RM;
    else if (mod(m, "rpi"))
        irnd = 1, rm = MC_RP;
    else
        rm = rmode(m);
    ftz = mod(m, "ftz");
    sat = mod(m, "sat");
    relu = mod(m, "relu");
    satf = mod(m, "satfinite");
    pzo = mod(m, "pzo");
    dt = need_type(p, m);
    st = need_type(p, m);
    mn_done(p, m);
    if (n != 2 && n != 3)
        nops(p, n, 2);
    d = D(p, &o[0]);
    a = S(p, &o[1], st);
    if (n == 3)
        b = S(p, &o[2], st);
    db = tbits(dt);
    sb = tbits(st);
    rb = tbits(o[0].type);
    if (rb < db && !(dt == T_E2M1X2))
        perr(p, "%s: the destination register is too narrow", p->mn);
    plain = !ftz && !sat && !relu && !satf && !pzo && n == 2;
    if (tint(dt) && tint(st) && !sat && n == 2) {
        if (sb < db) {
            if (tsigned(st) && tsigned(dt))
                extend(p, d, a, sb, 1, rb);
            else
                extend(p, d, a, sb, tsigned(st), db);
        } else
            extend(p, d, a, db, tsigned(dt), rb);
        return;
    }
    if (plain && tint(st) && sb >= 32 && (dt == T_F32 || dt == T_F64) && (rm == MC_RN || (dt == T_F64 && sb == 32))) {
        static const u16 ops[2][2][2] = {{{OP_CVT_F32_U32, OP_CVT_F32_S32}, {OP_CVT_F32_U64, OP_CVT_F32_S64}},
                                         {{OP_CVT_F64_U32, OP_CVT_F64_S32}, {OP_CVT_F64_U64, OP_CVT_F64_S64}}};
        emit(p, ops[dt == T_F64][sb == 64][tsigned(st)], 0, d, a, 0, 0, 0);
        return;
    }
    if (irnd && rm == MC_RZ && !sat && n == 2 && tint(dt) && rb == db) {
        int op = 0;
        if (st == T_F32)
            op = dt == T_S32 ? OP_CVT_S32_F32Z : dt == T_U32 ? OP_CVT_U32_F32Z : dt == T_S64 ? OP_CVT_S64_F32Z : 0;
        else if (st == T_F64)
            op = dt == T_S32 ? OP_CVT_S32_F64Z : dt == T_S64 ? OP_CVT_S64_F64Z : 0;
        if (op) {
            emit(p, op, 0, d, a, 0, 0, 0);
            return;
        }
    }
    if (plain && !irnd) {
        int op = 0;
        if (dt == T_F64 && st == T_F32)
            op = OP_CVT_F64_F32;
        else if (dt == T_F32 && st == T_F64 && rm == MC_RN)
            op = OP_CVT_F32_F64;
        else if (dt == T_F32 && st == T_F16)
            op = OP_CVT_F32_F16;
        else if (dt == T_F16 && st == T_F32 && rm == MC_RN)
            op = OP_CVT_F16_F32;
        if (op) {
            emit(p, op, 0, d, a, 0, 0, 0);
            return;
        }
    }
    spec = (u32)dt | (u32)st << 5 | (u32)(rm < 0 ? MC_RN : rm) << 10 | (u32)irnd << 13 | (u32)ftz << 14 | (u32)sat << 15 |
           (u32)relu << 16 | (u32)satf << 17 | (u32)pzo << 18 | (u32)(rb == 8 ? 0 : rb == 16 ? 1 : rb == 32 ? 2 : 3) << 19 |
           (u32)(rm >= 0) << 21 | (u32)(n == 3) << 22;
    if (!mc_cvt_ok(spec))
        perr(p, "%s: this conversion is not supported", p->mn);
    emit(p, OP_CVT, 0, d, a, b, 0, (s32)spec);
}

static int space_of(Mn *m)
{
    if (mod(m, "global"))
        return SP_GLOBAL;
    if (mod(m, "shared"))
        return SP_SHARED;
    if (mod(m, "local"))
        return SP_LOCAL;
    if (mod(m, "param"))
        return SP_PARAM;
    if (mod(m, "const"))
        return SP_CONST;
    return SP_GENERIC;
}

/* the modifiers of memory instructions that change nothing here */
static void mem_mods(Mn *m)
{
    static const char *const ign[] = {"weak", "volatile", "relaxed", "acquire", "release", "mmio", "nc", "cta", "cluster", "gpu", "sys", "unified", 0};
    int i, j;
    for (j = 0; ign[j]; j++)
        mod(m, ign[j]);
    for (i = 1; i < m->n; i++)
        if (!m->used[i] && (strstr(m->tok[i], "::") || (m->tok[i][0] == 'L' && m->tok[i][1] >= '1' && m->tok[i][1] <= '3')))
            m->used[i] = 1;
}

/* the load of one element: 0..8 as OP_LD8 + k */
static int ld_kind(P *p, int t, int regbits)
{
    int bits = tbits(t);
    if (t == T_PRED || bits > 64 || t == T_NONE)
        perr(p, "%s: unsupported type", p->mn);
    switch (bits) {
    case 8:
        return !tsigned(t) ? 0 : regbits == 64 ? 2 : 1;
    case 16:
        return !tsigned(t) ? 3 : regbits == 64 ? 5 : 4;
    case 32:
        return tsigned(t) && regbits == 64 ? 7 : 6;
    }
    return 8;
}

static int vec_n(Mn *m)
{
    if (mod(m, "v2"))
        return 2;
    if (mod(m, "v4"))
        return 4;
    if (mod(m, "v3"))
        return 3;
    return 1;
}

static void do_ld(P *p, Mn *m, Opd *o, int n)
{
    int sp, vn, t, k, sh;
    s32 base, disp, i;
    mem_mods(m);
    sp = space_of(m);
    vn = vec_n(m);
    t = need_type(p, m);
    mn_done(p, m);
    nops(p, n, 2);
    base = A(p, &o[1], &disp);
    sh = sp == SP_SHARED ? OP_LD8_S - OP_LD8 : 0;
    if (vn == 1) {
        if (o[0].kind == O_REG && tbits(o[0].type) < tbits(t))
            perr(p, "%s: the destination register is too narrow", p->mn);
        k = ld_kind(p, t, tbits(o[0].type));
        emit(p, OP_LD8 + k + sh, 0, D(p, &o[0]), base, 0, disp, 0);
        if (tsigned(t) && tbits(o[0].type) == 16 && tbits(t) == 8)
            emit(p, OP_ZX16, 0, o[0].slot, o[0].slot, 0, 0, 0);
        return;
    }
    if (o[0].kind != O_VEC || o[0].nvec != vn)
        perr(p, "%s needs a { } list of its vector's length", p->mn);
    k = ld_kind(p, t, tbits(p->vpool[o[0].vec].type));
    i = xadd(p, D(p, &p->vpool[o[0].vec]));
    for (n = 1; n < vn; n++)
        xadd(p, D(p, &p->vpool[o[0].vec + n]));
    emit(p, OP_LDV + sh, vn | k << 4, 0, base, 0, disp, i);
}

static void do_st(P *p, Mn *m, Opd *o, int n)
{
    int sp, vn, t, bits, sh, k;
    s32 base, disp, i;
    mem_mods(m);
    sp = space_of(m);
    vn = vec_n(m);
    t = need_type(p, m);
    mn_done(p, m);
    nops(p, n, 2);
    bits = tbits(t);
    if (t == T_PRED || bits > 64)
        perr(p, "%s: unsupported type", p->mn);
    k = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
    base = A(p, &o[0], &disp);
    sh = sp == SP_SHARED ? OP_ST8_S - OP_ST8 : 0;
    if (vn == 1) {
        emit(p, OP_ST8 + k + sh, 0, 0, base, S(p, &o[1], t), disp, 0);
        return;
    }
    if (o[1].kind != O_VEC || o[1].nvec != vn)
        perr(p, "%s needs a { } list of its vector's length", p->mn);
    i = xadd(p, S(p, &p->vpool[o[1].vec], t));
    for (n = 1; n < vn; n++)
        xadd(p, S(p, &p->vpool[o[1].vec + n], t));
    emit(p, OP_STV + sh, vn | k << 4, 0, base, 0, disp, i);
}

static void do_atom(P *p, Mn *m, Opd *o, int n, int red)
{
    static const char *const names[] = {"and", "or", "xor", "cas", "exch", "add", "inc", "dec", "min", "max", 0};
    int sp, t, i, op = -1;
    s32 base, disp, d, b, c = 0;
    mem_mods(m);
    mod(m, "noftz");
    sp = space_of(m);
    for (i = 0; names[i]; i++)
        if (mod(m, names[i]))
            op = i;
    t = need_type(p, m);
    mn_done(p, m);
    if (op < 0)
        perr(p, "%s lacks an operation", p->mn);
    if (tbits(t) != 32 && tbits(t) != 64 && !(t == T_F16 || t == T_BF16))
        perr(p, "%s: unsupported type", p->mn);
    if (red) {
        nops(p, n, 2);
        d = newreg(p);
        o--; /* the operands are one place to the left */
    } else {
        nops(p, n, op == AT_CAS ? 4 : 3);
        d = D(p, &o[0]);
    }
    base = A(p, &o[1], &disp);
    b = S(p, &o[2], t);
    if (op == AT_CAS)
        c = S(p, &o[3], t);
    emit(p, sp == SP_SHARED ? OP_ATOM_S : OP_ATOM, op | t << 4, d, base, b, disp, c);
}

static void add_callee(P *p, McFunc *f)
{
    u32 i;
    for (i = 0; i < p->ncallee; i++)
        if (p->callees[i] == f)
            return;
    if (p->ncallee == p->capcallee) {
        p->capcallee = p->capcallee ? p->capcallee * 2 : 8;
        p->callees = (McFunc **)xrealloc(p->callees, p->capcallee * sizeof(McFunc *));
    }
    p->callees[p->ncallee++] = f;
}

/* The function a name stands for: itself, or for a declaration without a body the definition in another
   loaded module, or one of the functions the engine provides (f->builtin is then set). */
static McFunc *resolve_func(McFunc *f)
{
    McModule *o;
    McFunc *g;
    int again;
    if (f->has_body)
        return f;
    if (f->alias)
        return f->alias;
    for (again = 0; again < 2; again++) {
        for (o = modules; o; o = o->next)
            if (o != f->mod && (g = mc_module_func(o, f->name)) != 0 && g->has_body) {
                f->alias = g;
                return g;
            }
        if (mc_builtin_id(f->name) >= 0 || !mc_more_modules || !mc_more_modules())
            break;
    }
    if (!f->builtin)
        f->builtin = (u8)(mc_builtin_id(f->name) + 1);
    return f;
}

static void call_item(P *p, Opd *o, Param *formal)
{
    if (o->kind == O_REG) {
        xadd(p, 0);
        xadd(p, o->slot);
        xadd(p, 8);
    } else if (o->kind == O_SYM && o->sym->kind == SY_MEM) {
        xadd(p, 1);
        xadd(p, (s32)(o->sym->off + o->off));
        xadd(p, (s32)o->sym->size);
    } else if (o->kind == O_IMM || o->kind == O_FLT || o->kind == O_SYM) {
        int t = formal ? formal->type : o->kind == O_FLT ? T_F64 : T_B64;
        if (t == T_B8 || tbits(t) > 64)
            t = T_B64;
        xadd(p, 0);
        xadd(p, S(p, o, o->kind == O_SYM ? T_B64 : t));
        xadd(p, 8);
    } else
        perr(p, "bad argument in %s", p->mn);
}

static void do_call(P *p, Mn *m, Opd *o, int n)
{
    Opd *rets = 0, *args = 0, *tg;
    McFunc *f = 0;
    int idx = 0, i, nr, na;
    s32 x;
    mod(m, "uni");
    mn_done(p, m);
    if (n < 1)
        perr(p, "call of nothing", 0);
    if (o[0].kind == O_VEC) {
        rets = &o[0];
        idx = 1;
    }
    if (idx >= n)
        perr(p, "call of nothing", 0);
    tg = &o[idx++];
    if (idx < n && o[idx].kind == O_VEC)
        args = &o[idx++];
    nr = rets ? rets->nvec : 0;
    na = args ? args->nvec : 0;
    if (tg->kind == O_SYM && tg->sym->kind == SY_FUNC) {
        f = resolve_func(tg->sym->fn);
        /* a function of the engine's own takes the call as it is written: a module may declare it in more than one way */
        if (!f->builtin && (f->nrets != nr || f->nparams != na))
            perr(p, "call of %s with the wrong number of arguments or results", f->name);
    } else if (tg->kind != O_REG)
        perr(p, "call of something that is not a function", 0);
    x = xadd(p, (s32)(u32)((u64)f & 0xffffffffUL));
    xadd(p, (s32)(u32)((u64)f >> 32));
    xadd(p, nr);
    xadd(p, na);
    for (i = 0; i < nr; i++)
        call_item(p, &p->vpool[rets->vec + i], f && i < f->nrets ? &f->rets[i] : 0);
    for (i = 0; i < na; i++)
        call_item(p, &p->vpool[args->vec + i], f && i < f->nparams ? &f->params[i] : 0);
    if (!f)
        emit(p, OP_CALLI, 0, 0, x, S(p, tg, T_B64), 0, 0);
    else if (f->has_body) {
        add_callee(p, f);
        emit(p, OP_CALL, 0, 0, x, 0, 0, 0);
    } else if (f->builtin)
        emit(p, OP_CALLB, f->builtin - 1, 0, x, 0, 0, 0);
    else
        emit(p, OP_BADFUNC, 0, 0, x, 0, 0, 0);
}

static s32 mask_slot(P *p, Opd *o) { return S(p, o, T_B32); }

static void do_bar(P *p, Mn *m, Opd *o, int n)
{
    int kind, t = 0;
    s32 d = 0, a, b = NOSLOT, c = 0;
    mod(m, "cta");
    mod(m, "aligned");
    if (mod(m, "warp")) {
        s32 x;
        if (!mod(m, "sync"))
            perr(p, "unknown instruction %s", p->mn);
        mn_done(p, m);
        nops(p, n, 1);
        x = xadd(p, mask_slot(p, &o[0]));
        emit(p, OP_WSYNC, WS_BARWARP, 0, 0, 0, 0, x);
        p->f->uses_sync = 1;
        return;
    }
    if (mod(m, "sync"))
        kind = 0;
    else if (mod(m, "arrive"))
        kind = 1;
    else if (mod(m, "red")) {
        kind = mod(m, "and") ? 2 : mod(m, "or") ? 3 : mod(m, "popc") ? 4 : -1;
        t = need_type(p, m);
        if (kind < 0 || (kind == 4) != (t == T_U32) || (kind != 4 && t != T_PRED))
            perr(p, "unknown instruction %s", p->mn);
    } else {
        perr(p, "unknown instruction %s", p->mn);
        kind = 0;
    }
    mn_done(p, m);
    if (kind >= 2) {
        if (n != 3 && n != 4)
            nops(p, n, 3);
        d = D(p, &o[0]);
        a = S(p, &o[1], T_U32);
        if (n == 4)
            b = S(p, &o[2], T_U32);
        c = pred_src(p, &o[n - 1]);
    } else {
        if (n != 1 && n != 2)
            nops(p, n, 1);
        if (kind == 1)
            nops(p, n, 2);
        a = S(p, &o[0], T_U32);
        if (n == 2)
            b = S(p, &o[1], T_U32);
    }
    emit(p, OP_BAR, kind, d, a, b, c, 0);
    p->f->uses_sync = 1;
}

static void do_shfl(P *p, Mn *m, Opd *o, int n)
{
    int sync = mod(m, "sync"), kind, t;
    s32 x;
    kind = mod(m, "up") ? WS_SHFL_UP : mod(m, "down") ? WS_SHFL_DOWN : mod(m, "bfly") ? WS_SHFL_BFLY : mod(m, "idx") ? WS_SHFL_IDX : -1;
    t = need_type(p, m);
    mn_done(p, m);
    if (kind < 0 || tbits(t) != 32)
        perr(p, "unknown instruction %s", p->mn);
    nops(p, n, sync ? 5 : 4);
    x = xadd(p, sync ? mask_slot(p, &o[4]) : NOSLOT);
    xadd(p, o[0].slot2);
    emit(p, OP_WSYNC, kind, D(p, &o[0]), S(p, &o[1], T_B32), S(p, &o[2], T_B32), S(p, &o[3], T_B32), x);
    p->f->uses_sync = 1;
}

static void do_vote(P *p, Mn *m, Opd *o, int n)
{
    int sync = mod(m, "sync"), kind, t;
    s32 x, a;
    kind = mod(m, "all") ? WS_VOTE_ALL : mod(m, "any") ? WS_VOTE_ANY : (mod(m, "uni") || mod(m, "eq")) ? WS_VOTE_EQ : mod(m, "ballot") ? WS_VOTE_BALLOT : -1;
    t = need_type(p, m);
    mn_done(p, m);
    if (kind < 0 || (kind == WS_VOTE_BALLOT) != (t == T_B32) || (kind != WS_VOTE_BALLOT && t != T_PRED))
        perr(p, "unknown instruction %s", p->mn);
    nops(p, n, sync ? 3 : 2);
    a = pred_src(p, &o[1]);
    x = xadd(p, sync ? mask_slot(p, &o[2]) : NOSLOT);
    emit(p, OP_WSYNC, kind, D(p, &o[0]), a, 0, 0, x);
    p->f->uses_sync = 1;
}

static void do_match(P *p, Mn *m, Opd *o, int n)
{
    int any = mod(m, "any"), all = mod(m, "all"), t;
    s32 x;
    if (!mod(m, "sync") || any == all)
        perr(p, "unknown instruction %s", p->mn);
    t = need_type(p, m);
    mn_done(p, m);
    if (tbits(t) != 32 && tbits(t) != 64)
        perr(p, "unknown instruction %s", p->mn);
    nops(p, n, 3);
    x = xadd(p, mask_slot(p, &o[2]));
    xadd(p, o[0].slot2);
    emit(p, OP_WSYNC, (any ? WS_MATCH_ANY32 : WS_MATCH_ALL32) + (tbits(t) == 64), D(p, &o[0]), S(p, &o[1], t), 0, 0, x);
    p->f->uses_sync = 1;
}

static void do_redux(P *p, Mn *m, Opd *o, int n)
{
    static const char *const names[] = {"add", "min", "max", "and", "or", "xor", 0};
    int i, op = -1, t, ab, nan;
    s32 x;
    if (!mod(m, "sync"))
        perr(p, "unknown instruction %s", p->mn);
    for (i = 0; names[i]; i++)
        if (mod(m, names[i]))
            op = i;
    ab = mod(m, "abs");
    nan = mod(m, "NaN");
    t = need_type(p, m);
    mn_done(p, m);
    if (op < 0 || tbits(t) != 32 || (t == T_F32 && op != 1 && op != 2) || ((ab || nan) && t != T_F32))
        perr(p, "unknown instruction %s", p->mn);
    nops(p, n, 3);
    x = xadd(p, mask_slot(p, &o[2]));
    xadd(p, op | (ab ? 16 : 0) | (nan ? 32 : 0));
    xadd(p, t);
    emit(p, OP_WSYNC, WS_REDUX, D(p, &o[0]), S(p, &o[1], t), 0, 0, x);
    p->f->uses_sync = 1;
}

static int mma_elem(const char *s)
{
    static const char *const names[] = {"f16", "bf16", "tf32", "s8", "u8", "e4m3", "e5m2", "e2m1", "e2m3", "e3m2", "s4", "u4", 0};
    int i;
    for (i = 0; names[i]; i++)
        if (streq(s, names[i]))
            return i;
    return -1;
}

/* xtra of an mma: mask (none), K, element kind of A, of B, accumulator (0 f32, 1 f16 pairs, 2 s32), saturate,
   block scaling (0, or the number of scales per row), registers of A, of B, of C and D, then the slots of
   D, A, B, C, and with block scaling: scale A, byte id, thread id, scale B, byte id, thread id */
static void do_mma(P *p, Mn *m, Opd *o, int n)
{
    int i, k = 0, shape = 0, sat, nt = 0, acc, na, nb, nc, ebits, bscale = 0, kind = 0;
    int ty[5];
    s32 x;
    const char *tn[5];
    if (!mod(m, "sync") || !mod(m, "aligned") || !mod(m, "row") || !mod(m, "col"))
        perr(p, "unsupported instruction %s", p->mn);
    sat = mod(m, "satfinite");
    if (mod(m, "block_scale"))
        bscale = 1;
    for (i = 1; i < m->n; i++) {
        if (m->used[i])
            continue;
        if (strncmp(m->tok[i], "m16n8k", 6) == 0) {
            k = atoi(m->tok[i] + 6);
            shape = 1;
            m->used[i] = 1;
        } else if (strncmp(m->tok[i], "kind::", 6) == 0) {
            kind = streq(m->tok[i] + 6, "mxf8f6f4") ? 1 : streq(m->tok[i] + 6, "mxf4") ? 2 : streq(m->tok[i] + 6, "mxf4nvf4") ? 3 : streq(m->tok[i] + 6, "f8f6f4") ? 4 : -1;
            m->used[i] = 1;
        } else if (strncmp(m->tok[i], "scale_vec::", 11) == 0) {
            bscale = atoi(m->tok[i] + 11);
            m->used[i] = 1;
        } else if (nt < 5 && (mma_elem(m->tok[i]) >= 0 || streq(m->tok[i], "f32") || streq(m->tok[i], "s32") || streq(m->tok[i], "ue8m0"))) {
            tn[nt++] = m->tok[i];
            m->used[i] = 1;
        }
    }
    mn_done(p, m);
    if (!shape || nt < 4 || kind < 0)
        perr(p, "unsupported instruction %s", p->mn);
    if (kind == 1 || kind == 2 || kind == 3) {
        if (nt != 5 || !streq(tn[4], "ue8m0"))
            perr(p, "unsupported instruction %s", p->mn);
        if (bscale == 1 && kind != 1)
            bscale = 2;
    } else if (bscale || nt != 4)
        perr(p, "unsupported instruction %s", p->mn);
    ty[1] = mma_elem(tn[1]);
    ty[2] = mma_elem(tn[2]);
    acc = streq(tn[0], "f32") ? 0 : streq(tn[0], "f16") ? 1 : streq(tn[0], "s32") ? 2 : -1;
    if (acc < 0 || !streq(tn[0], tn[3]) || ty[1] < 0 || ty[2] < 0)
        perr(p, "unsupported instruction %s", p->mn);
    ebits = ty[1] <= 1 ? 16 : ty[1] == 2 ? 32 : ty[1] >= 10 ? 4 : 8;
    if (kind == 2 || kind == 3) {
        if (ty[1] != 7 || ty[2] != 7)
            perr(p, "unsupported instruction %s", p->mn);
        ebits = 4; /* e2m1 packed as nibbles */
    }
    if (ty[1] <= 2 ? (ty[1] != ty[2] || acc == 2 || (acc == 1 && ty[1] != 0))
                   : (ty[1] == 3 || ty[1] == 4 || ty[1] >= 10) ? (acc != 2 || !(ty[2] == 3 || ty[2] == 4 || ty[2] >= 10)) : (acc != 0 || ty[2] < 5 || ty[2] > 9))
        perr(p, "unsupported instruction %s", p->mn);
    na = 16 * k * ebits / 32 / 32;
    nb = 8 * k * ebits / 32 / 32;
    nc = acc == 1 ? 2 : 4;
    if (na * 32 * 32 != 16 * k * ebits || na < 1 || na > 4 || nb < 1)
        perr(p, "unsupported shape in %s", p->mn);
    nops(p, n, bscale ? 8 : 4);
    for (i = 0; i < 4; i++)
        if (o[i].kind != O_VEC || o[i].nvec != (i == 1 ? na : i == 2 ? nb : nc))
            perr(p, "%s: wrong number of registers in a { } list", p->mn);
    x = xadd(p, NOSLOT);
    xadd(p, k);
    xadd(p, ty[1]);
    xadd(p, ty[2]);
    xadd(p, acc);
    xadd(p, sat);
    xadd(p, bscale);
    xadd(p, na);
    xadd(p, nb);
    xadd(p, nc);
    for (i = 0; i < nc; i++)
        xadd(p, D(p, &p->vpool[o[0].vec + i]));
    for (i = 0; i < na; i++)
        xadd(p, S(p, &p->vpool[o[1].vec + i], T_B32));
    for (i = 0; i < nb; i++)
        xadd(p, S(p, &p->vpool[o[2].vec + i], T_B32));
    for (i = 0; i < nc; i++)
        xadd(p, S(p, &p->vpool[o[3].vec + i], acc == 0 ? T_F32 : T_B32));
    if (bscale) {
        for (i = 4; i < 8; i += 2) {
            Opd *v = &p->vpool[o[i + 1].vec];
            if (o[i + 1].kind != O_VEC || o[i + 1].nvec != 2 || v[0].kind != O_IMM || v[1].kind != O_IMM)
                perr(p, "%s: bad scale selector", p->mn);
            xadd(p, S(p, &o[i], T_B32));
            xadd(p, (s32)v[0].imm);
            xadd(p, (s32)v[1].imm);
        }
    }
    emit(p, OP_WSYNC, WS_MMA, 0, 0, 0, 0, x);
    p->f->uses_sync = 1;
}

static void do_ldmatrix(P *p, Mn *m, Opd *o, int n)
{
    int num, trans, t, i, shared;
    s32 x, base, disp;
    if (!mod(m, "sync") || !mod(m, "aligned") || !mod(m, "m8n8"))
        perr(p, "unsupported instruction %s", p->mn);
    num = mod(m, "x1") ? 1 : mod(m, "x2") ? 2 : mod(m, "x4") ? 4 : 0;
    trans = mod(m, "trans");
    mem_mods(m);
    shared = mod(m, "shared") != 0;
    t = need_type(p, m);
    mn_done(p, m);
    if (!num || t != T_B16)
        perr(p, "unsupported instruction %s", p->mn);
    nops(p, n, 2);
    base = A(p, &o[1], &disp);
    x = xadd(p, NOSLOT);
    xadd(p, num | trans << 4 | shared << 8);
    if (o[0].kind == O_VEC) {
        if (o[0].nvec != num)
            perr(p, "%s: wrong number of registers in the { } list", p->mn);
        for (i = 0; i < num; i++)
            xadd(p, D(p, &p->vpool[o[0].vec + i]));
    } else {
        if (num != 1)
            perr(p, "%s: wrong number of registers", p->mn);
        xadd(p, D(p, &o[0]));
    }
    emit(p, OP_WSYNC, WS_LDMATRIX, 0, base, 0, disp, x);
    p->f->uses_sync = 1;
}

static void label_ref(P *p, Opd *o, s32 ins)
{
    if (o->kind != O_NAME)
        perr(p, "a label is expected by %s", p->mn);
    if (p->npatch == p->cappatch) {
        p->cappatch = p->cappatch ? p->cappatch * 2 : 64;
        p->patches = (Patch *)xrealloc(p->patches, p->cappatch * sizeof(Patch));
    }
    p->patches[p->npatch].name = o->name;
    p->patches[p->npatch].len = o->nlen;
    p->patches[p->npatch].ins = (u32)ins;
    p->patches[p->npatch].line = p->line;
    p->patches[p->npatch].xt = -1;
    p->npatch++;
}

/* One instruction: w is its mnemonic, the operands are still to be read. pred: the slot of the guard, or NOSLOT. */
/* Notes which registers an instruction reads and writes: the first operand (alone, or the registers of a list)
   is what it writes, except where that is an address. Only used to tell about registers nothing writes. */
static void note_rw(P *p, Opd *o, int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        if (o[i].kind == O_VEC) {
            for (k = 0; k < o[i].nvec; k++) {
                Opd *e = &p->vpool[o[i].vec + k];
                if (e->kind == O_REG && e->sym) {
                    if (i == 0)
                        e->sym->wr = 1;
                    else
                        e->sym->rd = 1;
                }
            }
        } else if (o[i].kind == O_REG && o[i].sym) {
            if (i == 0)
                o[i].sym->wr = 1;
            else
                o[i].sym->rd = 1;
        } else if (o[i].kind == O_MEM && o[i].mbase == O_REG && o[i].sym)
            o[i].sym->rd = 1;
    }
}

static void instruction(P *p, const char *w, s32 pred, int pneg)
{
    Opd ops[21], *o = ops + 1;
    Mn mm, *m = &mm;
    int n = 0, i, t;
    s32 skip = -1;
    const char *b;
    p->mn = w;
    p->nvpool = 0;
    mn_split(p, m, w);
    b = m->tok[0];
    if (peekc(p) != ';')
        do {
            if (n >= 20)
                perr(p, "too many operands", 0);
            operand(p, &o[n++]);
        } while (accept(p, ','));
    expect(p, ';');
    note_rw(p, o, n);
    if (streq(b, "bra")) {
        mod(m, "uni");
        mn_done(p, m);
        nops(p, n, 1);
        if (pred == NOSLOT)
            label_ref(p, &o[0], emit(p, OP_BRA, 0, 0, 0, 0, 0, 0));
        else
            label_ref(p, &o[0], emit(p, pneg ? OP_BRF : OP_BRT, 0, 0, pred, 0, 0, 0));
        return;
    }
    if (pred != NOSLOT)
        skip = emit(p, pneg ? OP_SKIPT : OP_SKIPF, 0, 0, pred, 0, 0, 0);
    if (streq(b, "brx")) {
        BTab *bt;
        s32 x, at;
        u32 k;
        if (!mod(m, "idx"))
            perr(p, "unsupported instruction %s", w);
        mod(m, "uni");
        mn_done(p, m);
        nops(p, n, 2);
        if (o[1].kind != O_NAME)
            perr(p, "a label is expected by %s", w);
        for (bt = p->btabs; bt; bt = bt->next)
            if (bt->len == o[1].nlen && memcmp(bt->name, o[1].name, bt->len) == 0)
                break;
        if (!bt)
            perr(p, "%s: no .branchtargets list of that name before this", w);
        x = xadd(p, (s32)bt->n);
        at = emit(p, OP_BRX, 0, 0, S(p, &o[0], T_U32), 0, 0, x);
        for (k = 0; k < bt->n; k++) {
            Opd lo;
            memset(&lo, 0, sizeof lo);
            lo.kind = O_NAME;
            lo.name = bt->tname[k];
            lo.nlen = bt->tlen[k];
            label_ref(p, &lo, at);
            p->patches[p->npatch - 1].xt = xadd(p, 0);
        }
        goto done;
    }
    /* the big dispatch; every branch ends the function or falls to done */
    if (streq(b, "mov")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 2);
        if (o[1].kind == O_SPECIAL && o[1].fx < 0)
            emit(p, OP_CLOCK, 0, D(p, &o[0]), 0, 0, 0, 0);
        else if (o[1].kind == O_SYM || o[1].kind == O_SPECIAL)
            emit(p, OP_MOV, 0, D(p, &o[0]), S(p, &o[1], t), 0, 0, 0);
        else if (t == T_PRED || tbits(t) <= 64)
            emit(p, tbits(t) == 32 ? OP_MOV32 : OP_MOV, 0, D(p, &o[0]), S(p, &o[1], t), 0, 0, 0);
        else
            perr(p, "unsupported instruction %s", w);
    } else if (streq(b, "ld") || streq(b, "ldu"))
        do_ld(p, m, o, n);
    else if (streq(b, "st"))
        do_st(p, m, o, n);
    else if (streq(b, "setp"))
        do_setp(p, m, o, n, 0);
    else if (streq(b, "set"))
        do_setp(p, m, o, n, 1);
    else if (streq(b, "cvt"))
        do_cvt(p, m, o, n);
    else if (streq(b, "selp")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        emit(p, tbits(t) <= 32 ? OP_SELP32 : OP_SELP, 0, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), pred_src(p, &o[3]), 0);
    } else if (streq(b, "slct")) {
        int ftz = mod(m, "ftz"), ct;
        t = need_type(p, m);
        ct = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        if (ct != T_S32 && ct != T_F32)
            perr(p, "unsupported instruction %s", w);
        emit(p, ct == T_S32 ? OP_SLCT_S32 : ftz ? OP_SLCT_F32FTZ : OP_SLCT_F32, 0, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t),
             S(p, &o[3], ct), 0);
    } else if (streq(b, "mul") || streq(b, "mad")) {
        int mad = b[1] == 'a', lo = mod(m, "lo"), hi = mod(m, "hi"), wide = mod(m, "wide"), bits, sg;
        t = 0;
        for (i = 1; i < m->n; i++)
            if (!m->used[i] && type_of(m->tok[i]))
                t = type_of(m->tok[i]);
        if (is_fp_type(t)) {
            mtype(m);
            flt_op(p, m, &fltops[mad ? 5 : 2], t, o, n);
            goto done;
        }
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, mad ? 4 : 3);
        bits = tbits(t);
        sg = tsigned(t);
        if (lo + hi + wide != 1 || !tint(t) || bits < 16)
            perr(p, "unsupported instruction %s", w);
        if (wide) {
            if (bits == 32)
                emit(p, mad ? (sg ? OP_MADWS32 : OP_MADWU32) : (sg ? OP_MULWS32 : OP_MULWU32), 0, D(p, &o[0]), S(p, &o[1], t),
                     S(p, &o[2], t), mad ? S(p, &o[3], sg ? T_S64 : T_U64) : 0, 0);
            else if (bits == 16) {
                s32 r = mad ? newreg(p) : D(p, &o[0]);
                emit(p, OP_MUL32, 0, r, widen(p, S(p, &o[1], t), t), widen(p, S(p, &o[2], t), t), 0, 0);
                if (mad)
                    emit(p, OP_ADD32, 0, D(p, &o[0]), r, S(p, &o[3], sg ? T_S32 : T_U32), 0, 0);
            } else
                perr(p, "unsupported instruction %s", w);
        } else if (lo)
            int_op(p, t, 0, mad ? OP_MAD32 : OP_MUL32, mad ? OP_MAD64 : OP_MUL64, 0, o, mad ? 3 : 2);
        else if (bits == 16) {
            s32 r = newreg(p);
            emit(p, OP_MUL32, 0, r, widen(p, S(p, &o[1], t), t), widen(p, S(p, &o[2], t), t), 0, 0);
            emit(p, sg ? OP_SHRS32 : OP_SHRU32, 0, r, r, cslot(p, 0, 16), 0, 0);
            if (mad)
                emit(p, OP_ADD32, 0, r, r, S(p, &o[3], t), 0, 0);
            emit(p, OP_ZX16, 0, D(p, &o[0]), r, 0, 0, 0);
        } else if (mad)
            int_op(p, t, 0, sg ? OP_MADHIS32 : OP_MADHIU32, sg ? OP_MADHIS64 : OP_MADHIU64, 0, o, 3);
        else
            int_op(p, t, 0, sg ? OP_MULHIS32 : OP_MULHIU32, sg ? OP_MULHIS64 : OP_MULHIU64, 0, o, 2);
    } else if (streq(b, "mul24") || streq(b, "mad24")) {
        int mad = b[1] == 'a', lo = mod(m, "lo"), hi = mod(m, "hi"), sg;
        s32 r;
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, mad ? 4 : 3);
        sg = tsigned(t);
        if (lo + hi != 1 || tbits(t) != 32)
            perr(p, "unsupported instruction %s", w);
        r = mad ? newreg(p) : D(p, &o[0]);
        emit(p, lo ? OP_MUL24 : sg ? OP_MUL24HIS : OP_MUL24HIU, sg, r, S(p, &o[1], t), S(p, &o[2], t), 0, 0);
        if (mad)
            emit(p, OP_ADD32, 0, D(p, &o[0]), r, S(p, &o[3], t), 0, 0);
    } else if (streq(b, "addc") || streq(b, "subc") || ((streq(b, "add") || streq(b, "sub")) && mod(m, "cc"))) {
        int sub = b[0] == 's', carry_in = b[3] == 'c', cc = carry_in ? mod(m, "cc") : 1;
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 3);
        if (tbits(t) != 32 && tbits(t) != 64)
            perr(p, "unsupported instruction %s", w);
        if (carry_in)
            int_op(p, t, 0, sub ? OP_SUBC32 : OP_ADDC32, sub ? OP_SUBC64 : OP_ADDC64, cc, o, 2);
        else
            int_op(p, t, 0, sub ? OP_SUBCC32 : OP_ADDCC32, sub ? OP_SUBCC64 : OP_ADDCC64, 0, o, 2);
    } else if (streq(b, "bfind")) {
        int sh = mod(m, "shiftamt");
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 2);
        if (tbits(t) < 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, tbits(t) == 32 ? (tsigned(t) ? OP_BFINDS32 : OP_BFINDU32) : (tsigned(t) ? OP_BFINDS64 : OP_BFINDU64), sh,
             D(p, &o[0]), S(p, &o[1], t), 0, 0, 0);
    } else if (streq(b, "bfe")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        if (tbits(t) < 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, tbits(t) == 32 ? (tsigned(t) ? OP_BFES32 : OP_BFEU32) : (tsigned(t) ? OP_BFES64 : OP_BFEU64), 0, D(p, &o[0]),
             S(p, &o[1], t), S(p, &o[2], T_U32), S(p, &o[3], T_U32), 0);
    } else if (streq(b, "bfi")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 5);
        if (tbits(t) < 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, tbits(t) == 32 ? OP_BFI32 : OP_BFI64, 0, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), S(p, &o[3], T_U32),
             S(p, &o[4], T_U32));
    } else if (streq(b, "shf")) {
        int l = mod(m, "l"), r = mod(m, "r"), clamp = mod(m, "clamp"), wrap = mod(m, "wrap");
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        if (l + r != 1 || clamp + wrap != 1 || tbits(t) != 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, l ? OP_SHFL : OP_SHFR, clamp, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), S(p, &o[3], T_U32), 0);
    } else if (streq(b, "prmt")) {
        static const char *const modes[] = {"f4e", "b4e", "rc8", "ecl", "ecr", "rc16", 0};
        int mode = 0;
        for (i = 0; modes[i]; i++)
            if (mod(m, modes[i]))
                mode = i + 1;
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        if (tbits(t) != 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, OP_PRMT, mode, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), S(p, &o[3], T_B32), 0);
    } else if (streq(b, "dp4a") || streq(b, "dp2a")) {
        int two = b[2] == '2', hi = two ? mod(m, "hi") : 0, t2;
        if (two && !hi && !mod(m, "lo"))
            perr(p, "unsupported instruction %s", w);
        t = need_type(p, m);
        t2 = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        if (tbits(t) != 32 || tbits(t2) != 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, two ? OP_DP2A : OP_DP4A, tsigned(t) | tsigned(t2) << 1 | hi << 2, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t2),
             S(p, &o[3], T_B32), 0);
    } else if (streq(b, "fns")) {
        need_type(p, m);
        mn_done(p, m);
        nops(p, n, 4);
        emit(p, OP_FNS, 0, D(p, &o[0]), S(p, &o[1], T_B32), S(p, &o[2], T_B32), S(p, &o[3], T_B32), 0);
    } else if (streq(b, "szext") || streq(b, "bmsk")) {
        int clamp = mod(m, "clamp"), wrap = mod(m, "wrap");
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 3);
        if (clamp + wrap != 1 || tbits(t) != 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, b[0] == 's' ? OP_SZEXT : OP_BMSK, clamp | tsigned(t) << 1, D(p, &o[0]), S(p, &o[1], T_B32), S(p, &o[2], T_B32), 0, 0);
    } else if (streq(b, "lop3")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 5);
        if (tbits(t) != 32 || o[4].kind != O_IMM)
            perr(p, "unsupported instruction %s", w);
        emit(p, OP_LOP3, (int)(o[4].imm & 0xff), D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), S(p, &o[3], t), 0);
    } else if (streq(b, "testp")) {
        static const char *const names[] = {"finite", "infinite", "number", "notanumber", "normal", "subnormal", 0};
        int k = -1;
        for (i = 0; names[i]; i++)
            if (mod(m, names[i]))
                k = i;
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 2);
        if (k < 0 || (t != T_F32 && t != T_F64))
            perr(p, "unsupported instruction %s", w);
        emit(p, OP_TESTP, k | (t == T_F64) << 4, D(p, &o[0]), S(p, &o[1], t), 0, 0, 0);
    } else if (streq(b, "cvta")) {
        int to = mod(m, "to"), sp = space_of(m);
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 2);
        if (tbits(t) != 64 && tbits(t) != 32)
            perr(p, "unsupported instruction %s", w);
        emit(p, sp != SP_SHARED ? OP_MOV : to ? OP_CVTA_TO_SHARED : OP_CVTA_SHARED, 0, D(p, &o[0]), S(p, &o[1], T_B64 == t ? t : t), 0, 0, 0);
    } else if (streq(b, "isspacep")) {
        int sp = space_of(m);
        mn_done(p, m);
        nops(p, n, 2);
        emit(p, OP_ISSPACEP, sp, D(p, &o[0]), S(p, &o[1], T_B64), 0, 0, 0);
    } else if (streq(b, "atom"))
        do_atom(p, m, o, n, 0);
    else if (streq(b, "red") && !(m->n > 1 && streq(m->tok[1], "async")))
        do_atom(p, m, o, n, 1);
    else if (streq(b, "call"))
        do_call(p, m, o, n);
    else if (streq(b, "ret")) {
        mod(m, "uni");
        mn_done(p, m);
        nops(p, n, 0);
        emit(p, OP_RET, 0, 0, 0, 0, 0, 0);
    } else if (streq(b, "exit")) {
        mn_done(p, m);
        emit(p, OP_EXIT, 0, 0, 0, 0, 0, 0);
    } else if (streq(b, "trap")) {
        mn_done(p, m);
        emit(p, OP_TRAP, 0, 0, 0, 0, 0, 0);
    } else if (streq(b, "brkpt") || streq(b, "nop") || streq(b, "membar") || streq(b, "fence") || streq(b, "prefetch") ||
               streq(b, "prefetchu") || streq(b, "pmevent") || streq(b, "nanosleep") || streq(b, "griddepcontrol")) {
        /* nothing to do: one thread at a time, no caches, no events */
    } else if (streq(b, "bar") || streq(b, "barrier"))
        do_bar(p, m, o, n);
    else if (streq(b, "shfl"))
        do_shfl(p, m, o, n);
    else if (streq(b, "vote"))
        do_vote(p, m, o, n);
    else if (streq(b, "match"))
        do_match(p, m, o, n);
    else if (streq(b, "redux"))
        do_redux(p, m, o, n);
    else if (streq(b, "activemask")) {
        need_type(p, m);
        mn_done(p, m);
        nops(p, n, 1);
        /* the threads of the warp that are at this instruction together: so they have to meet here */
        emit(p, OP_WSYNC, WS_ACTIVEMASK, D(p, &o[0]), 0, 0, 0, xadd(p, NOSLOT));
        p->f->uses_sync = 1;
    } else if (streq(b, "mma"))
        do_mma(p, m, o, n);
    else if (streq(b, "ldmatrix"))
        do_ldmatrix(p, m, o, n);
    else if (streq(b, "cp") && m->n > 1 && streq(m->tok[1], "async")) {
        m->used[1] = 1;
        if (mod(m, "commit_group") || mod(m, "wait_group") || mod(m, "wait_all")) {
            mn_done(p, m); /* copies are done at once */
        } else {
            s32 db, dd, sb, sd, x;
            if (!mod(m, "ca"))
                mod(m, "cg");
            mem_mods(m);
            if (!mod(m, "shared") || !mod(m, "global"))
                perr(p, "unsupported instruction %s", w);
            mn_done(p, m);
            if (n != 3 && n != 4)
                nops(p, n, 3);
            db = A(p, &o[0], &dd);
            sb = A(p, &o[1], &sd);
            x = xadd(p, S(p, &o[2], T_U32));
            xadd(p, n == 4 ? S(p, &o[3], T_U32) : NOSLOT);
            emit(p, OP_CPASYNC, 0, dd, db, sb, sd, x);
        }
    } else if (streq(b, "stacksave")) {
        need_type(p, m);
        mn_done(p, m);
        nops(p, n, 1);
        emit(p, OP_STACKSAVE, 0, D(p, &o[0]), 0, 0, 0, 0);
    } else if (streq(b, "stackrestore")) {
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, 1);
        emit(p, OP_STACKRESTORE, 0, 0, S(p, &o[0], t), 0, 0, 0);
    } else if (streq(b, "alloca")) {
        t = need_type(p, m);
        mn_done(p, m);
        if (n != 2 && n != 3)
            nops(p, n, 2);
        if (n == 3 && o[2].kind != O_IMM)
            perr(p, "%s: the alignment must be a constant", w);
        emit(p, OP_ALLOCA, 0, D(p, &o[0]), S(p, &o[1], t), n == 3 ? (s32)o[2].imm : 8, 0, 0);
    } else {
        const struct IntOp *io;
        const struct FltOp *fo;
        t = 0;
        for (i = 1; i < m->n; i++)
            if (!m->used[i] && type_of(m->tok[i]))
                t = type_of(m->tok[i]);
        if (is_fp_type(t)) {
            for (fo = fltops; fo->name; fo++)
                if (streq(fo->name, b))
                    break;
            if (!fo->name)
                perr(p, "unknown instruction %s", w);
            for (i = 1; i < m->n; i++)
                if (!m->used[i] && type_of(m->tok[i]) == t)
                    m->used[i] = 1;
            flt_op(p, m, fo, t, o, n);
            goto done;
        }
        for (io = intops; io->name; io++)
            if (streq(io->name, b))
                break;
        if (!io->name)
            perr(p, "unknown instruction %s", w);
        if ((streq(b, "add") || streq(b, "sub")) && mod(m, "sat")) {
            t = need_type(p, m);
            mn_done(p, m);
            nops(p, n, 3);
            if (t != T_S32)
                perr(p, "unsupported instruction %s", w);
            emit(p, b[0] == 'a' ? OP_ADDSAT32 : OP_SUBSAT32, 0, D(p, &o[0]), S(p, &o[1], t), S(p, &o[2], t), 0, 0);
            goto done;
        }
        t = need_type(p, m);
        mn_done(p, m);
        nops(p, n, io->nsrc + 1);
        if (t == T_PRED) {
            int op = streq(b, "and") ? OP_PAND : streq(b, "or") ? OP_POR : streq(b, "xor") ? OP_PXOR : streq(b, "not") ? OP_PNOT : 0;
            if (!op)
                perr(p, "unsupported instruction %s", w);
            emit(p, op, 0, D(p, &o[0]), S(p, &o[1], t), io->nsrc > 1 ? S(p, &o[2], t) : 0, 0, 0);
            goto done;
        }
        if (!tint(t))
            perr(p, "unsupported instruction %s", w);
        if (tsigned(t))
            int_op(p, t, io->shift ? T_U32 : 0, io->s32op, io->s64op, 0, o, io->nsrc);
        else
            int_op(p, t, io->shift ? T_U32 : 0, io->u32op, io->u64op, 0, o, io->nsrc);
    }
done:
    if (skip >= 0)
        p->code[skip].b = (s32)p->ncode - skip - 1;
}

/* ---- a function body ---- */

static void body(P *p)
{
    char w[220];
    s32 pred = NOSLOT;
    int pneg = 0;
    for (;;) {
        int c = peekc(p);
        if (!c)
            perr(p, "unexpected end of text", 0);
        if (c == '{') {
            p->s++;
            if (++p->depth >= 64)
                perr(p, "blocks nested too deeply", 0);
            p->scope[p->depth] = 0;
            continue;
        }
        if (c == '}') {
            p->s++;
            pop_scope(p);
            if (p->depth-- == 0)
                return;
            continue;
        }
        if (c == ';') {
            p->s++;
            continue;
        }
        if (c == '@') {
            Opd o;
            p->s++;
            pneg = accept(p, '!');
            primary(p, &o);
            if (o.kind != O_REG || o.type != T_PRED)
                perr(p, "a predicate register is expected after '@'", 0);
            pred = o.slot;
            o.sym->rd = 1;
            continue;
        }
        if (!word(p, w, sizeof w))
            perr(p, "an instruction is expected", 0);
        if (w[0] == '.') {
            if (streq(w, ".reg"))
                local_decl(p, SY_REG);
            else if (streq(w, ".local") || streq(w, ".param"))
                local_decl(p, SY_MEM);
            else if (streq(w, ".loc") || streq(w, ".file"))
                skipline(p);
            else if (streq(w, ".branchtargets")) {
                BTab *bt = (BTab *)aalloc(&p->tmp, sizeof *bt);
                char nm[220];
                u32 cap = 8;
                if (!p->lastlabel || p->lastlabel->at != (s32)p->ncode)
                    perr(p, ".branchtargets without a label", 0);
                bt->name = p->lastlabel->name;
                bt->len = p->lastlabel->len;
                bt->n = 0;
                bt->tname = (const char **)aalloc(&p->tmp, cap * sizeof(char *));
                bt->tlen = (u32 *)aalloc(&p->tmp, cap * sizeof(u32));
                do {
                    if (!word(p, nm, sizeof nm))
                        perr(p, "a label is expected in .branchtargets", 0);
                    if (bt->n == cap) {
                        const char **tn = (const char **)aalloc(&p->tmp, 2 * cap * sizeof(char *));
                        u32 *tl = (u32 *)aalloc(&p->tmp, 2 * cap * sizeof(u32));
                        memcpy(tn, bt->tname, cap * sizeof(char *));
                        memcpy(tl, bt->tlen, cap * sizeof(u32));
                        bt->tname = tn;
                        bt->tlen = tl;
                        cap *= 2;
                    }
                    bt->tlen[bt->n] = (u32)strlen(nm);
                    bt->tname[bt->n++] = astrn(&p->tmp, nm, (u32)strlen(nm));
                } while (accept(p, ','));
                expect(p, ';');
                bt->next = p->btabs;
                p->btabs = bt;
            } else if (streq(w, ".pragma") || streq(w, ".calltargets") || streq(w, ".callprototype")) {
                while (*p->s && *p->s != ';') {
                    if (*p->s == '\n')
                        p->line++;
                    p->s++;
                }
                expect(p, ';');
            } else
                perr(p, "unknown directive %s", w);
            continue;
        }
        if (peekc(p) == ':' && p->s[1] != ':') {
            Label *l = (Label *)aalloc(&p->tmp, sizeof *l);
            u32 n = (u32)strlen(w), h = hashn(w, n) & 255;
            Label *q;
            p->s++;
            for (q = p->labels[h]; q; q = q->next)
                if (q->len == n && memcmp(q->name, w, n) == 0)
                    perr(p, "the label %s is defined twice", w);
            l->name = astrn(&p->tmp, w, n);
            l->len = n;
            l->at = (s32)p->ncode;
            l->next = p->labels[h];
            p->labels[h] = l;
            p->lastlabel = l;
            continue;
        }
        instruction(p, w, pred, pneg);
        pred = NOSLOT;
    }
}

static void free_parse(P *p)
{
    free(p->code);
    free(p->lines);
    free(p->xtra);
    free(p->cents);
    free(p->fix);
    free(p->patches);
    free(p->callees);
    free(p->ftab);
    afree(&p->tmp);
    free(p);
}

int mc_func_decode(McFunc *f)
{
    P *p;
    u32 i;
    int k;
    McFunc *g;
    if (f->decoded)
        return 0;
    if (f->decoding)
        return 0; /* recursion: the caller up the stack finishes it */
    if (!f->has_body) {
        char b[300];
        sprintf(b, "%.100s: the function %.100s has no body", f->mod->name, f->name);
        mc_set_error(b);
        return -1;
    }
    p = (P *)calloc(1, sizeof(P));
    if (!p || !(p->ftab = (Tab *)calloc(1, sizeof(Tab)))) {
        mc_set_error("out of memory");
        free(p);
        return -1;
    }
    p->m = f->mod;
    p->f = f;
    p->s = f->body;
    p->line = f->body_line;
    for (i = 0; i < 1024; i++)
        p->chash[i] = -1;
    f->decoding = 1;
    if (setjmp(p->jb)) {
        f->decoding = 0;
        free(f->srefs);
        f->srefs = 0;
        f->nsrefs = 0;
        free_parse(p);
        return -1;
    }
    p->nreg = f->nformal;
    p->mem = (u32)alignup(f->formal_mem, 8);
    for (k = 0; k < f->nrets + f->nparams; k++) {
        Param *q = k < f->nrets ? &f->rets[k] : &f->params[k - f->nrets];
        Sym *y = declare(p, q->name, q->nlen, q->isreg ? SY_REG : f->is_entry ? SY_EPARAM : SY_MEM);
        y->type = q->type;
        y->slot = q->slot;
        y->off = q->off;
        y->size = q->size;
        y->wr = 1;
    }
    p->unwritten = 0;
    p->nunwritten = 0;
    body(p);
    if (p->unwritten)
        fprintf(stderr, "mcuda: %.60s: %.80s: the register %.*s is read but no instruction writes it (%u such)\n",
                p->m->name ? p->m->name : "ptx", f->name, (int)p->unwritten->len, p->unwritten->name, p->nunwritten);
    emit(p, f->is_entry ? OP_EXIT : OP_RET, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < p->npatch; i++) {
        Patch *pa = &p->patches[i];
        Label *l;
        Ins *ins = &p->code[pa->ins];
        for (l = p->labels[hashn(pa->name, pa->len) & 255]; l; l = l->next)
            if (l->len == pa->len && memcmp(l->name, pa->name, pa->len) == 0)
                break;
        if (!l) {
            p->line = pa->line;
            perr(p, "undefined label %s", astrn(&p->tmp, pa->name, pa->len));
        }
        if (pa->xt >= 0)
            p->xtra[pa->xt] = l->at;
        else if (ins->op == OP_BRA)
            ins->a = l->at - (s32)pa->ins;
        else
            ins->b = l->at - (s32)pa->ins;
    }
    f->nreg = p->nreg;
    f->nconst = p->ncent;
    f->ctemplate = (Slot *)calloc(p->ncent + 1, sizeof(Slot));
    for (i = 0; i < p->ncent; i++)
        if (!p->cents[i].kind)
            f->ctemplate[p->ncent - 1 - i].u = p->cents[i].val;
    for (i = 0; i < p->nfix; i++)
        if (p->fix[i].kind == FX_FRAME)
            p->fix[i].off += (u64)p->nreg * 8;
    f->fix = p->fix;
    f->nfix = p->nfix;
    p->fix = 0;
    f->mem_bytes = p->mem;
    f->frame_bytes = (u32)alignup((u64)(f->nconst + f->nreg) * 8 + p->mem, 16);
    f->code = p->code;
    f->lines = p->lines;
    f->ncode = p->ncode;
    f->xtra = p->xtra;
    f->nxtra = p->nxtra;
    p->code = 0;
    p->lines = 0;
    p->xtra = 0;
    f->callees = p->callees;
    f->ncallees = p->ncallee;
    p->callees = 0;
    free_parse(p);
    f->decoded = 1;
    f->decoding = 0;
    for (i = 0; i < f->ncallees; i++)
        if (mc_func_decode(f->callees[i]) < 0)
            goto undo;
    /* functions whose address was taken may be called through a register */
    for (g = f->mod->funcs; g; g = g->next)
        if (g->addr_taken && g->has_body && !g->decoded && !g->decoding)
            if (mc_func_decode(g) < 0)
                goto undo;
    mc_jit_compile(f);
    return 0;
undo: /* so that the next launch finds the same error */
    free_func(f);
    free(f->srefs);
    free(f->callees);
    f->code = 0;
    f->lines = 0;
    f->xtra = 0;
    f->ctemplate = 0;
    f->fix = 0;
    f->srefs = 0;
    f->callees = 0;
    f->nsrefs = f->ncallees = 0;
    f->decoded = 0;
    return -1;
}

McModule *mc_modules(void) { return modules; }

/* The kernel of that name in any loaded module (one translation unit may launch a kernel another defines). */
McFunc *mc_find_kernel(const char *name)
{
    McModule *o;
    McFunc *g;
    int again;
    for (again = 0; again < 2; again++) {
        for (o = modules; o; o = o->next)
            if ((g = mc_module_func(o, name)) != 0 && g->has_body && g->is_entry)
                return g;
        if (!mc_more_modules || !mc_more_modules())
            break;
    }
    return 0;
}

int mc_func_defined(McFunc *f) { return f && f->has_body; }

/* The bytes of the .shared variables that f and what it calls name. */
static u64 shared_used(McFunc *f, u32 stamp)
{
    u64 n = 0;
    u32 i;
    if (f->stamp == stamp)
        return 0;
    f->stamp = stamp;
    for (i = 0; i < f->nsrefs; i++)
        if (f->srefs[i]->stamp != stamp && !f->srefs[i]->is_extern) {
            f->srefs[i]->stamp = stamp;
            n = alignup(n, f->srefs[i]->align) + f->srefs[i]->size;
        }
    for (i = 0; i < f->ncallees; i++)
        n += shared_used(f->callees[i], stamp);
    return n;
}
