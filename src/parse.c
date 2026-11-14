/* C89 parser. There is no syntax tree: declarations build types and
   symbols, statements and expressions build IR directly. */
#include "cc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOSYM 0xffffffffU

/* attributes mcc acts on */
#define AT_PACKED 1
#define AT_WEAK 2
#define AT_NORETURN 4
#define AT_ALWAYS_INLINE 8
#define AT_NOINLINE 16
#define AT_USED 32
#define AT_GNU_INLINE 64
#define AT_TRANSPARENT 128
#define AT_NAKED 256
#define AT_ERROR 1024
#define AT_REGISTER 512 /* declared register: with a name from asm(), it is that register */
#define AT_GLOBAL 2048    /* CUDA: a kernel */
#define AT_DEVICE 4096    /* CUDA: runs on the device */
#define AT_HOST 8192      /* CUDA: runs on the host (as well) */
#define AT_SHARED 16384   /* CUDA: lives in the block's shared memory */
#define AT_CONSTANT 32768 /* CUDA: device constant memory */
#define AT_GUARD 65536    /* C++: a lock guard type */
#define AT_CTOR 131072     /* called before main */

enum { SK_VAR, SK_TYPEDEF, SK_ENUM, SK_TEMPLATE };

struct Tmpl;

typedef struct Sym {
    u32 name;
    u8 kind;
    u8 local;     /* lives in a stack slot */
    u8 defined;
    u8 tentative;
    u8 is_static; /* internal linkage */
    u8 byref;     /* parameter passed by reference: the slot holds its address */
    u8 is_inline, is_extern;
    u8 is_tls;    /* thread-local */
    u32 aflags;   /* AT_* from attributes */
    u32 maxntid;  /* CUDA: from launch_bounds, 0 if none */
    u32 section, alias, asmname; /* interned strings from attributes, 0 if none */
    Type *ty;
    u32 slot;
    u32 objsym;
    i64 val; /* enumeration constant */
    u32 level;
    struct Sym *shadow;
    struct Sym *scope_next;
    struct Sym *real;      /* block-scope declaration of a global: the global */
    struct Sym *tent_next; /* list of tentative definitions */
    /* C++ */
    u8 cv;       /* a constant known by its value: 1 an integer (val), 2 floating (fval) */
    u8 c_link;   /* a function with C linkage: its name is not mangled */
    u8 is_cx;    /* a constexpr function */
    u8 is_ctmpl; /* SK_TEMPLATE: a class template */
    double fval;
    struct Sym *over;    /* the next function or function template of the same name */
    struct Sym *cs_next; /* the next member of the same class */
    void *cls;           /* the class it is a member of */
    struct Tmpl *itm;    /* a function made from a template (or a member of a class made from one): the template */
    struct TInst *iti;   /* and its arguments */
    struct Tmpl *tmpl;   /* SK_TEMPLATE */
    Token **pdef;        /* default arguments, by parameter (0: none) */
    u32 *npdef;
    Token *body;         /* constexpr function: its tokens from '{' to '}' */
    u32 nbody;
} Sym;

typedef struct Tag {
    u32 name;
    Type *ty;
    u32 level;
    struct Tag *shadow;
    struct Tag *scope_next;
} Tag;

typedef struct Scope {
    Sym *syms;
    Tag *tags;
} Scope;

typedef struct Expr {
    Type *ty;
    u32 v; /* value; address for lvalues, arrays, functions and records */
    u32 hi; /* 128-bit integers: the upper half (v is the lower) */
    u8 lval;
    u8 bitoff, bitwidth;
    u8 konst; /* an integer constant expression as the language defines it, not merely a value that folds */
} Expr;

typedef struct Label {
    u32 name;
    u32 blk;
    u32 loc;
    int defined;
    u32 index; /* its number as the value of &&label, 0 if that was never asked for */
    u32 local; /* declared by __label__: the scope level it belongs to */
    struct Label *next;
} Label;

typedef struct Case {
    i64 val, hi; /* val ... hi */
    u32 blk;
} Case;

typedef struct Switch {
    u32 first_case; /* index into cases[] */
    u32 default_blk;
    Type *ty;
    struct Switch *outer;
} Switch;

#define MAX_SCOPES 256

static Token tok, ptok;
static int has_ptok;
static Arena A;
static Obj *O;
static Func *F, *fn_func, *const_func;
static Sym **idents, **gsyms;
static Tag **tags;
static u32 names_cap;
static Scope scopes[MAX_SCOPES];
static u32 level;
static Type *cur_ret;
static u32 sret_slot;
static u32 cur_named_gr, cur_named_fr; /* argument registers taken by the named parameters */
static Type *va_list_type; /* Linux: the record behind __builtin_va_list */
static Label *labels;
static u32 nlabel_index; /* labels of this function whose address was taken */
static u32 cgoto_blk, cgoto_slot; /* where every `goto *p` goes, and the slot that carries p there */
static u32 break_blk, cont_blk;
static Switch *cur_switch;
static Case *cases;
static u32 ncases, cases_cap;
static Sym *tentatives;
static int opt_dump_ir;
static u32 id_va_start, id_va_arg, id_va_end, id_nullptr, id_true, id_false;

static u32 id_template, id_typename, id_class, id_constexpr, id_namespace, id_using, id_new, id_delete, id_decltype,
    id_public, id_private, id_protected, id_static_cast, id_reinterpret_cast, id_const_cast, id_noexcept, id_throw, id_mutable,
    id_explicit, id_friend, id_virtual, id_operator, id_is_same, id_main;
static Tag *find_tag(u32 name);
static void do_cast(Expr *e, Type *t, u32 loc);
static void compound_literal(Expr *e, Type *t);
static void expr(Expr *e);
static void assign_expr(Expr *e);
static void cast_expr(Expr *e);
static void unary(Expr *e);
static Type *declspec(int *storage);
static Type *declarator(Type *base, u32 *name);
static Type *type_name(void);
static void statement(void);
static void compound(int new_scope);
static struct Label *get_label(u32 name);
static int declaration(int file_scope);
static i64 const_int_expr(void);
static Type *unqual(Type *t);
static int string_literal(Buf *b);
static void initializer_for(Expr *e, Type *t);
static void block_item(void);
static void static_init_sym(struct Sym *s);
static int atomic_able(Type *t);
static u32 atomic_call(int kind, Type *t, u32 *args, u32 nargs, int relaxed);
static u32 atomic_raw(Type *t, u32 v);
static u32 atomic_value(Type *t, u32 raw);
static void dead_expr(Expr *e);

static int special_convert(Expr *e, Type *to);
static int special_pair(Type *to, Type *from);
static void lambda_to_fn(Expr *e, Type *to);
static u32 load_val(Type *t, u32 addr);
static void store_val(Type *t, u32 addr, u32 v);
static u32 rt_call(const char *name, Type *ret, Type *pt, u32 arg);
static u32 new_temp(Type *t);
static u32 id_complex, id_float16, id_bf16, id_real, id_imag, id_alignas, id_alignas2;

/* 1: a GNU vector type, 2: a complex type, else 0 */
#define VK(t) ((t)->kind == TY_STRUCT ? (t)->canon->vkind : 0)

static const char *cur_fn_name = "";
static void nlazy_reset(void);
static u32 shared_seq;
static int cur_fn_dev;  /* CUDA: the function being read is device code */
static u32 nsw_val, nsw_blk; /* the last sum or difference of ints, and the block it was worked out in */
static int cur_fn_kernel; /* CUDA: and a kernel, whose code is only ever PTX */
static int launch_stop; /* CUDA: reading the <<<...>>> of a kernel launch, where >> ends an expression */
static struct Launch {
    int on;
    u32 grid, block, shm, stream;
} launch;
static Expr sv_val;            /* value of the last statement of a statement expression */
static int sv_pending, sv_has; /* the next statement is asked for its value; it had one */

/* ---- tokens ---- */

/* Loop conditions are parsed twice (once to guard the loop, once at its
   bottom), so their tokens are recorded and played back. */
/* Recordings nest: a loop condition can call a lambda (or a function read in place) whose body has loops of its own,
   and an initializer with an inferred type is recorded while it may do the same. rec / nrec / caprec are the
   innermost recording; the enclosing ones wait in rec_outer and are given every token too. Each recording has an
   id (rec_ids counts them); a token put back by replay() carries the last id given out (playgen), and is taken
   again only by recordings that started after it was put back: those that were running had it already. */
static Token *rec, *play;
static u32 nrec, caprec, nplay, iplay, capplay;
static u32 *playgen;
static int recording;
static u32 rec_id, rec_ids;
typedef struct RecLevel {
    Token *rec;
    u32 nrec, caprec, id;
} RecLevel;
static RecLevel rec_outer[32];
static u32 nrec_outer;

static void rec_add(Token **b, u32 *n, u32 *cap, const Token *t)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *b = (Token *)xrealloc(*b, *cap * sizeof(Token));
    }
    (*b)[(*n)++] = *t;
}

/* The token t, put back when recording id gen was the last started (0: read from the source), to every recording
   that does not have it yet. */
static void rec_put_gen(const Token *t, u32 gen)
{
    u32 k;
    if (rec_id > gen)
        rec_add(&rec, &nrec, &caprec, t);
    for (k = 0; k < nrec_outer; k++)
        if (rec_outer[k].id > gen)
            rec_add(&rec_outer[k].rec, &rec_outer[k].nrec, &rec_outer[k].caprec, t);
}

static void rec_put(const Token *t)
{
    rec_put_gen(t, 0);
}

static Token pushback;
static int has_pushback;
static u8 *ns_ids; /* C++: by identifier, whether it names a namespace */
static u32 ns_cap;

static void ns_add(u32 id)
{
    if (id >= ns_cap) {
        u32 cap = ns_cap ? ns_cap : 1024;
        while (cap <= id)
            cap *= 2;
        ns_ids = (u8 *)xrealloc(ns_ids, cap);
        memset(ns_ids + ns_cap, 0, cap - ns_cap);
        ns_cap = cap;
    }
    ns_ids[id] = 1;
}

static Token attrq[64]; /* the tokens a [[gnu::...]] attribute was turned into */
static u32 attrq_n, attrq_i;

static void fetch(Token *t)
{
    if (iplay < nplay) {
        *t = play[iplay];
        if (recording)
            rec_put_gen(t, playgen[iplay]);
        iplay++;
        return;
    }
    for (;;) {
        if (attrq_i < attrq_n) {
            *t = attrq[attrq_i++];
            break;
        }
        if (has_pushback) {
            *t = pushback;
            has_pushback = 0;
        } else {
            pp_next(t);
        }
        if (t->kind == T_LBRACK && (opt_c23 || opt_cxx)) {
            /* [[attributes]]: those written gnu::name become __attribute__((name)), the standard ones are dropped */
            pp_next(&pushback);
            if (pushback.kind == T_LBRACK) {
                int depth = 2, gnu = 0, any = 0, start = 1;
                u32 loc = t->loc;
                Token x;
                attrq_n = attrq_i = 0;
#define AQ(K, V) do { \
        if (attrq_n >= 60) error_at(loc, "attribute too long"); \
        memset(&x, 0, sizeof x); x.kind = (u8)(K); x.v = (V); x.loc = loc; attrq[attrq_n++] = x; \
    } while (0)
                AQ(T_IDENT, intern_cstr("__attribute__"));
                AQ(T_LPAREN, 0);
                AQ(T_LPAREN, 0);
                while (depth > 0) {
                    pp_next(t);
                    if (t->kind == T_LBRACK)
                        depth++;
                    else if (t->kind == T_RBRACK)
                        depth--;
                    else if (t->kind == T_EOF)
                        error_at(t->loc, "unterminated attribute");
                    if (depth < 2)
                        continue;
                    if (depth == 2 && t->kind == T_COMMA) {
                        start = 1;
                        gnu = 0;
                        continue;
                    }
                    if (start && t->kind == T_IDENT && strcmp(intern_str(t->v), "gnu") == 0) {
                        gnu = 1; /* then "::", as one token or two */
                        start = 0;
                        continue;
                    }
                    start = 0;
                    if (gnu == 1 && (t->kind == T_COLON || t->kind == T_SCOPE))
                        continue;
                    if (gnu) {
                        if (gnu == 1 && any)
                            AQ(T_COMMA, 0);
                        gnu = 2;
                        any = 1;
                        if (attrq_n >= 60)
                            error_at(loc, "attribute too long");
                        attrq[attrq_n++] = *t;
                    }
                }
                AQ(T_RPAREN, 0);
                AQ(T_RPAREN, 0);
#undef AQ
                if (!any)
                    attrq_n = 0;
                continue;
            }
            has_pushback = 1;
        }
        if (opt_cxx && t->kind == T_IDENT && t->v < ns_cap && ns_ids[t->v] && !has_pushback) {
            /* namespaces are not kept apart: "std::" and the like are dropped */
            pp_next(&pushback);
            if (pushback.kind == T_SCOPE)
                continue;
            has_pushback = 1;
        }
        break;
    }
    if (t->kind == T_IDENT && t->v <= KW_COUNT + 32) {
        int k = lex_keyword(t->v);
        if (k)
            t->kind = (u8)k;
    }
    if (recording)
        rec_put(t);
}

/* While a loop that is to be unrolled is read, its tokens are kept as they are passed. */
static Token *grabbed;
static u32 ngrab, capgrab;
static int grabbing;
#define UNROLL_AUTO_N 32      /* a loop unrolled unasked runs at most this often */
#define UNROLL_AUTO_SIZE 1200 /* and is at most this many tokens when unrolled */
#define UNROLL_AUTO_INS 600   /* and this many instructions, as the body is first read */
static u32 au_n;
static u32 auto_n; /* loops unrolled unasked so far (debugging: MCC_AUTOMAX, MCC_AUTOLOG) */
static u32 unroll_req; /* the for statement that follows was asked to be unrolled: so many times at most */
static u32 id_mcc_unroll;

static void next(void)
{
    if (grabbing) {
        if (ngrab == capgrab) {
            capgrab = capgrab ? capgrab * 2 : 256;
            grabbed = (Token *)xrealloc(grabbed, capgrab * sizeof(Token));
        }
        grabbed[ngrab++] = tok;
    }
    if (has_ptok) {
        tok = ptok;
        has_ptok = 0;
    } else {
        fetch(&tok);
    }
    ir_loc = tok.loc; /* what is made now is for this place in the source (-g) */
}

static Token *peek(void)
{
    if (!has_ptok) {
        fetch(&ptok);
        has_ptok = 1;
    }
    return &ptok;
}

/* Starts recording at the current token. */
static void rec_start(void)
{
    if (recording) {
        if (nrec_outer == sizeof rec_outer / sizeof rec_outer[0])
            error_at(tok.loc, "loops and inferred types nested too deeply");
        rec_outer[nrec_outer].rec = rec;
        rec_outer[nrec_outer].nrec = nrec;
        rec_outer[nrec_outer].caprec = caprec;
        rec_outer[nrec_outer++].id = rec_id;
        rec = 0;
        caprec = 0;
    }
    nrec = 0;
    rec_id = ++rec_ids;
    /* the current token (and the one looked at) were read before: the enclosing recordings have them */
    rec_add(&rec, &nrec, &caprec, &tok);
    if (has_ptok)
        rec_add(&rec, &nrec, &caprec, &ptok);
    recording = 1;
}

/* Stops recording and returns a copy of the first n recorded tokens. */
static Token *rec_stop(u32 n)
{
    Token *t = (Token *)arena_alloc(&A, (n ? n : 1) * sizeof(Token));
    memcpy(t, rec, n * sizeof(Token));
    if (nrec_outer) {
        free(rec);
        nrec_outer--;
        rec = rec_outer[nrec_outer].rec;
        nrec = rec_outer[nrec_outer].nrec;
        caprec = rec_outer[nrec_outer].caprec;
        rec_id = rec_outer[nrec_outer].id;
    } else {
        recording = 0;
        rec_id = 0;
    }
    return t;
}

/* Makes the n tokens at t the next ones read; the current token follows them. */
static void replay(const Token *t, u32 n)
{
    u32 rest = nplay - iplay, need = n + 2 + rest, k = 0, j;
    Token *b = (Token *)xmalloc(need * sizeof(Token));
    u32 *g = (u32 *)xmalloc((need ? need : 1) * sizeof(u32));
    memcpy(b, t, n * sizeof(Token));
    k = n;
    b[k++] = tok;
    if (has_ptok)
        b[k++] = ptok;
    /* what is put back here is not new to the recordings running now (a second reading of a recording
       will be given these again); recordings started later take it */
    for (j = 0; j < k; j++)
        g[j] = rec_ids;
    memcpy(b + k, play + iplay, rest * sizeof(Token));
    if (rest)
        memcpy(g + k, playgen + iplay, rest * sizeof(u32));
    k += rest;
    free(play);
    free(playgen);
    play = b;
    playgen = g;
    capplay = need;
    nplay = k;
    iplay = 0;
    has_ptok = 0;
    next();
}

/* Collects the tokens from the current one up to a ',' or ')' outside any
   brackets, which becomes the current token. */
static u32 skim(Token **out)
{
    Token *buf = 0;
    u32 n = 0, cap = 0, depth = 0;
    for (;;) {
        int k = tok.kind;
        if (k == T_EOF)
            error_at(tok.loc, "unexpected end of file");
        if (!depth && (k == T_COMMA || k == T_RPAREN || k == T_RBRACE))
            break;
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE)
            depth++;
        else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE)
            depth--;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            buf = (Token *)xrealloc(buf, cap * sizeof(Token));
        }
        buf[n++] = tok;
        next();
    }
    *out = (Token *)arena_alloc(&A, (n ? n : 1) * sizeof(Token));
    memcpy(*out, buf, n * sizeof(Token));
    free(buf);
    return n;
}

static const char *tok_text(const Token *t)
{
    static char buf[64];
    Buf b;
    size_t n;
    if (t->kind == T_EOF)
        return "end of file";
    memset(&b, 0, sizeof b);
    tok_spell(t, &b);
    n = b.len < sizeof buf - 1 ? b.len : sizeof buf - 1;
    memcpy(buf, b.p, n);
    buf[n] = 0;
    buf_free(&b);
    return buf;
}

static void expect(int kind)
{
    if (tok.kind != kind)
        error_at(tok.loc, "expected '%s' before '%s'", tok_names[kind], tok_text(&tok));
    next();
}

static int accept(int kind)
{
    if (tok.kind != kind)
        return 0;
    next();
    return 1;
}

#define KW(k) (T_KEYWORD + (k))

/* ---- scopes and symbols ---- */

static void names_reserve(u32 name)
{
    if (name >= names_cap) {
        u32 cap = names_cap ? names_cap : 4096;
        while (cap <= name)
            cap *= 2;
        idents = (Sym **)xrealloc(idents, cap * sizeof(Sym *));
        gsyms = (Sym **)xrealloc(gsyms, cap * sizeof(Sym *));
        tags = (Tag **)xrealloc(tags, cap * sizeof(Tag *));
        memset(idents + names_cap, 0, (cap - names_cap) * sizeof(Sym *));
        memset(gsyms + names_cap, 0, (cap - names_cap) * sizeof(Sym *));
        memset(tags + names_cap, 0, (cap - names_cap) * sizeof(Tag *));
        names_cap = cap;
    }
}

/* C++: while a template or a lambda is read again where it is used, the block scopes
   in (hide_lo, hide_hi] are those of the user and cannot be seen */
typedef struct Hide {
    u32 n;
    u32 lo[8], hi[8];
} Hide;

static Hide hide;

static int hidden(u32 lv)
{
    u32 k;
    for (k = 0; k < hide.n; k++)
        if (lv > hide.lo[k] && lv <= hide.hi[k])
            return 1;
    return 0;
}

static void hide_push(u32 lo, u32 hi)
{
    if (hide.n == 8)
        error_at(tok.loc, "templates and lambdas nested too deeply");
    hide.lo[hide.n] = lo;
    hide.hi[hide.n++] = hi;
}

static Sym *lookup(u32 name)
{
    Sym *s = name < names_cap ? idents[name] : 0;
    while (s && hide.n && hidden(s->level))
        s = s->shadow;
    return s;
}

static Sym *new_sym(u32 name, int kind, Type *ty)
{
    Sym *s = (Sym *)arena_alloc(&A, sizeof(Sym));
    s->name = name;
    s->kind = (u8)kind;
    s->ty = ty;
    s->objsym = NOSYM;
    return s;
}

static int binding_params; /* -g: the parameters of the function being defined are bound */
static int inl_on(void);

static Sym *bind(Sym *s)
{
    if (opt_debug && s->local && s->kind == SK_VAR && s->name && F == fn_func && !inl_on() && !s->cv)
        dwarf_var(F->sym, s->name, s->ty, s->slot, binding_params, s->byref, tok.loc);
    names_reserve(s->name);
    s->level = level;
    s->shadow = idents[s->name];
    idents[s->name] = s;
    s->scope_next = scopes[level].syms;
    scopes[level].syms = s;
    return s;
}

static void push_scope(void)
{
    if (++level >= MAX_SCOPES)
        error_at(tok.loc, "blocks nested too deeply");
    scopes[level].syms = 0;
    scopes[level].tags = 0;
}

static void pop_scope(void)
{
    Sym *s;
    Tag *t;
    for (s = scopes[level].syms; s; s = s->scope_next)
        idents[s->name] = s->shadow;
    for (t = scopes[level].tags; t; t = t->scope_next)
        tags[t->name] = t->shadow;
    {
        Label *l;
        for (l = labels; l; l = l->next)
            if (l->local == level && l->name)
                l->name = 0; /* a local label goes out of scope */
    }
    level--;
}

/* Records what alignment the code may assume for an object's symbol. */
static void sym_align(u32 objsym, Type *t)
{
    u32 al, n = 0;
    if (t->kind != TY_FUNC && t->kind != TY_VOID && type_complete(t))
        O->syms[objsym].dsize = type_size(t);
    while (t->kind == TY_ARRAY)
        t = t->base;
    if (t->kind == TY_FUNC || t->kind == TY_VOID || !type_complete(t))
        return;
    al = type_align(t);
    while (n < 4 && ((u32)2 << n) <= al)
        n++;
    O->syms[objsym].align = (u8)n;
}

static u32 *deferred, ndeferred, cap_deferred; /* static functions whose code has not been generated yet */

/* The object-file symbol of a global, created on first use. */
static u32 sym_obj(Sym *s)
{
    if (s->objsym == NOSYM) {
        char tmp[256];
        const char *n = intern_str(s->name);
        size_t len = intern_len(s->name);
        char *buf = len + 2 <= sizeof tmp ? tmp : (char *)xmalloc(len + 2);
        u32 lname;
        buf[0] = '_';
        memcpy(buf + 1, n, len + 1);
        /* ELF names are the C names; an asm("name") is taken as it stands */
        lname = s->asmname ? s->asmname : opt_target == TARGET_LINUX ? s->name : intern(buf, len + 1);
        /* assembly refers to the same symbols by name */
        s->objsym = obj_named_sym(O, lname);
        O->syms[s->objsym].global = (u8)!s->is_static;
        if (s->aflags & AT_WEAK)
            O->syms[s->objsym].weak = 1;
        if (s->aflags & AT_ERROR)
            O->syms[s->objsym].trap = 1;
        sym_align(s->objsym, s->ty);
        if (buf != tmp)
            free(buf);
    }
    return s->objsym;
}

/* ---- attributes ---- */


typedef struct Attr {
    u32 flags;
    u32 align;   /* 0: none given */
    u32 section, alias, asmname;
    u32 mode;    /* size in bytes asked for by mode(), 0 if none */
    u32 vecsize; /* bytes asked for by vector_size(), 0 if none */
    u32 maxntid; /* CUDA, launch_bounds(): the most threads a block of the kernel is launched with */
} Attr;

static Attr ds_attr;  /* attributes among the last declaration specifiers */
static Attr dcl_attr; /* attributes met inside declarators since it was last cleared */
static int ds_inline; /* and whether they had "inline" */
static int ds_tls;    /* or _Thread_local */

static void skip_parens(void)
{
    u32 depth = 1;
    expect(T_LPAREN);
    while (depth) {
        if (tok.kind == T_EOF)
            error_at(tok.loc, "unterminated attribute");
        if (tok.kind == T_LPAREN)
            depth++;
        else if (tok.kind == T_RPAREN)
            depth--;
        next();
    }
}

static u32 attr_string(void)
{
    Buf b;
    u32 id;
    memset(&b, 0, sizeof b);
    expect(T_LPAREN);
    if (tok.kind != T_STR)
        error_at(tok.loc, "expected a string");
    string_literal(&b);
    expect(T_RPAREN);
    id = intern((const char *)b.p, b.len - 1);
    buf_free(&b);
    return id;
}

/* Zero or more __attribute__((...)); what mcc acts on goes into *a. */
static void attributes(Attr *a)
{
    while (tok.kind == KW(KW_ATTRIBUTE)) {
        next();
        expect(T_LPAREN);
        expect(T_LPAREN);
        while (tok.kind != T_RPAREN) {
            char name[64];
            const char *n;
            size_t len;
            if (accept(T_COMMA))
                continue;
            if (tok.kind != T_IDENT && tok.kind < T_KEYWORD)
                error_at(tok.loc, "expected attribute name before '%s'", tok_text(&tok));
            n = tok.kind == T_IDENT ? intern_str(tok.v) : tok_text(&tok);
            len = strlen(n);
            if (len > 4 && n[0] == '_' && n[1] == '_' && n[len - 1] == '_' && n[len - 2] == '_') {
                n += 2;
                len -= 4;
            }
            if (len >= sizeof name)
                len = sizeof name - 1;
            memcpy(name, n, len);
            name[len] = 0;
            next();
            if (strcmp(name, "packed") == 0)
                a->flags |= AT_PACKED;
            else if (strcmp(name, "weak") == 0)
                a->flags |= AT_WEAK;
            else if (strcmp(name, "noreturn") == 0)
                a->flags |= AT_NORETURN;
            else if (strcmp(name, "always_inline") == 0)
                a->flags |= AT_ALWAYS_INLINE;
            else if (strcmp(name, "noinline") == 0)
                a->flags |= AT_NOINLINE;
            else if (strcmp(name, "used") == 0)
                a->flags |= AT_USED;
            else if (strcmp(name, "constructor") == 0)
                a->flags |= AT_CTOR | AT_USED;
            else if (strcmp(name, "gnu_inline") == 0)
                a->flags |= AT_GNU_INLINE;
            else if (strcmp(name, "transparent_union") == 0)
                a->flags |= AT_TRANSPARENT;
            else if (strcmp(name, "naked") == 0)
                a->flags |= AT_NAKED;
            else if (strcmp(name, "error") == 0)
                a->flags |= AT_ERROR;
            else if (strcmp(name, "mcc_guard") == 0)
                a->flags |= AT_GUARD;
            else if (strcmp(name, "global") == 0)
                a->flags |= AT_GLOBAL;
            else if (strcmp(name, "device") == 0)
                a->flags |= AT_DEVICE;
            else if (strcmp(name, "host") == 0)
                a->flags |= AT_HOST;
            else if (strcmp(name, "shared") == 0)
                a->flags |= AT_SHARED;
            else if (strcmp(name, "constant") == 0)
                a->flags |= AT_CONSTANT;
            else if (strcmp(name, "aligned") == 0) {
                a->align = 16;
                if (accept(T_LPAREN)) {
                    i64 n2 = const_int_expr();
                    if (n2 <= 0 || (n2 & (n2 - 1)))
                        error_at(tok.loc, "alignment is not a power of two");
                    a->align = (u32)n2;
                    expect(T_RPAREN);
                }
                continue;
            } else if (strcmp(name, "vector_size") == 0) {
                expect(T_LPAREN);
                a->vecsize = (u32)const_int_expr();
                expect(T_RPAREN);
                continue;
            } else if (strcmp(name, "launch_bounds") == 0) {
                /* (threads of a block[, blocks wanted on a processor[, at most]]): the first is a promise the
                   assembler of device code uses, to give each thread more registers */
                expect(T_LPAREN);
                a->maxntid = (u32)const_int_expr() & 0xffff;
                if (accept(T_COMMA))
                    a->maxntid |= ((u32)const_int_expr() & 0xffff) << 16; /* (kept in the upper half) */
                while (accept(T_COMMA))
                    const_int_expr();
                expect(T_RPAREN);
                continue;
            } else if (strcmp(name, "section") == 0) {
                a->section = attr_string();
                continue;
            } else if (strcmp(name, "alias") == 0) {
                a->alias = attr_string();
                continue;
            } else if (strcmp(name, "mode") == 0) {
                const char *m;
                expect(T_LPAREN);
                m = tok.kind == T_IDENT ? intern_str(tok.v) : "";
                if (m[0] == '_' && m[1] == '_')
                    m += 2;
                a->mode = m[0] == 'Q' ? 1 : m[0] == 'H' ? 2 : m[0] == 'S' ? 4 : m[0] == 'T' ? 16 : 8;
                next();
                expect(T_RPAREN);
                continue;
            }
            if (tok.kind == T_LPAREN)
                skip_parens();
        }
        next();
        expect(T_RPAREN);
    }
}

/* What may follow a declarator: an assembler name and attributes. */
static void decl_suffix(Attr *a)
{
    for (;;) {
        if (tok.kind == KW(KW_ASM)) {
            next();
            a->asmname = attr_string();
        } else if (tok.kind == KW(KW_ATTRIBUTE)) {
            attributes(a);
        } else {
            break;
        }
    }
}

/* mode() and aligned() as they change a type. */
static Type *type_vector(Type *elem, u32 n, int vkind);

static Type *attr_type(Type *t, const Attr *a)
{
    if (a->mode && type_is_int(t)) {
        int uns = type_is_unsigned(t);
        switch (a->mode) {
        case 1: t = uns ? ty_uchar : ty_schar; break;
        case 2: t = uns ? ty_ushort : ty_short; break;
        case 4: t = uns ? ty_uint : ty_int; break;
        case 8: t = uns ? ty_ulong : ty_long; break;
        default: error_at(tok.loc, "128-bit integers are not supported");
        }
    }
    if (a->vecsize && !VK(t)) {
        if (!type_is_arith(t) || a->vecsize % type_size(t))
            error_at(tok.loc, "invalid vector type");
        return type_vector(t, (u32)(a->vecsize / type_size(t)), 1); /* its alignment stays that of the element */
    }
    if (a->align)
        t = type_aligned(t, a->align);
    return t;
}

static int is_typename(const Token *t)
{
    Sym *s;
    switch (t->kind) {
    case KW(KW_VOID): case KW(KW_CHAR): case KW(KW_SHORT): case KW(KW_INT): case KW(KW_LONG):
    case KW(KW_FLOAT): case KW(KW_DOUBLE): case KW(KW_SIGNED): case KW(KW_UNSIGNED):
    case KW(KW_STRUCT): case KW(KW_UNION): case KW(KW_ENUM): case KW(KW_CONST): case KW(KW_VOLATILE):
    case KW(KW_TYPEOF): case KW(KW_BOOL): case KW(KW_INT128): case KW(KW_RESTRICT): case KW(KW_ATTRIBUTE):
    case KW(KW_AUTO_TYPE): case KW(KW_ATOMIC):
        return 1;
    case T_IDENT:
        s = lookup(t->v);
        if (!s && (t->v == id_complex || t->v == id_float16 || t->v == id_bf16 || t->v == id_alignas || t->v == id_alignas2))
            return 1;
        if (opt_cxx) {
            if (!s)
                return find_tag(t->v) || t->v == id_typename || t->v == id_decltype || t->v == id_class;
            if (s->kind == SK_TEMPLATE)
                return s->is_ctmpl;
        }
        return s && s->kind == SK_TYPEDEF;
    }
    return 0;
}

static int is_declstart(const Token *t)
{
    switch (t->kind) {
    case KW(KW_TYPEDEF): case KW(KW_EXTERN): case KW(KW_STATIC): case KW(KW_AUTO): case KW(KW_REGISTER):
    case KW(KW_INLINE): case KW(KW_NORETURN): case KW(KW_STATIC_ASSERT): case KW(KW_THREAD_LOCAL):
        return 1;
    case T_IDENT:
        if (opt_cxx && (t->v == id_constexpr || t->v == id_using) && !lookup(t->v))
            return 1;
    }
    return is_typename(t);
}

/* ---- C++: names ---- */

static int extern_c; /* inside extern "C" */
static int anon_ns;  /* inside namespace { } */

/* A template is kept as its tokens, which are read again for each set of arguments it is used with. */
#define MAX_TARGS 12

typedef struct TArg {
    Type *ty; /* a type, or 0: the value val */
    i64 val;
} TArg;

typedef struct TArgs {
    u32 n;
    TArg a[MAX_TARGS];
} TArgs;

typedef struct TParam {
    u32 name;
    u8 is_type;
    Type *vty;   /* the type of a value parameter */
    Token *def;  /* the default, if any */
    u32 ndef;
} TParam;

typedef struct TInst {
    u32 key; /* the arguments, as they are spelt in symbols */
    TArgs args;
    Sym *fn;
    Type *ty;
    u8 queued, spec;
    u8 ext; /* named by an "extern template": another unit generates it */
    struct TInst *next;
} TInst;

typedef struct Tmpl {
    u32 name;
    TParam *params;
    u32 nparams;
    Token *toks; /* the declaration after template<...> */
    Token *head; /* a function: what comes before its body, then a ';' */
    u32 ntoks;
    u32 nhead;   /* a function: the tokens before its body or its ';' */
    u8 is_class, has_body, rkind, tried;
    u32 nfparams;
    Type *pattern; /* a function's type with the parameters left open, for deduction */
    TInst *insts;
    Hide hide;
} Tmpl;

/* Members that belong to a class or an enumeration rather than to its objects: constants, types. */
typedef struct CScope {
    void *key; /* the record's type, or the enumeration's tag */
    u32 id;
    Sym *syms;
    struct CScope *next;
} CScope;

static CScope *cscopes;
static u32 cscope_seq;

static CScope *cs_get(void *key, int create)
{
    CScope *c;
    for (c = cscopes; c; c = c->next)
        if (c->key == key)
            return c;
    if (!create)
        return 0;
    c = (CScope *)arena_alloc(&A, sizeof(CScope));
    c->key = key;
    c->id = cscope_seq++;
    c->next = cscopes;
    cscopes = c;
    return c;
}

static Sym *cs_find(void *key, u32 name)
{
    CScope *c = cs_get(key, 0);
    Sym *s;
    for (s = c ? c->syms : 0; s; s = s->cs_next)
        if (s->name == name)
            return s;
    return 0;
}

static void cs_add(void *key, Sym *s)
{
    CScope *c = cs_get(key, 1);
    s->cs_next = c->syms;
    s->cls = key;
    c->syms = s;
}

static void mg_s(Buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

static void mg_u(Buf *b, u64 n)
{
    char t[32];
    sprintf(t, "%lu", (unsigned long)n);
    mg_s(b, t);
}

/* A type as part of a symbol's name; the letters follow the usual C++ ABI where that is easy. */
static void mg_type(Buf *b, Type *t)
{
    static const char *const code[] = {"v", "b", "c", "a", "h", "s", "t", "i", "j", "l", "m", "x", "y", "n", "o",
                                       "DF16b", "Dh", "f", "d", "e"};
    u32 k;
    if (t->quals & Q_CONST)
        mg_s(b, "K");
    switch (t->kind) {
    case TY_PTR: mg_s(b, "P"); mg_type(b, t->base); return;
    case TY_REF: mg_s(b, "R"); mg_type(b, t->base); return;
    case TY_ARRAY: mg_s(b, "A"); mg_u(b, t->len); mg_s(b, "_"); mg_type(b, t->base); return;
    case TY_TPARAM: mg_s(b, "T"); mg_u(b, t->len); mg_s(b, "_"); return;
    case TY_FUNC:
        mg_s(b, "F");
        mg_type(b, t->base);
        for (k = 0; k < t->nparams; k++)
            mg_type(b, t->params[k]);
        mg_s(b, "E");
        return;
    case TY_STRUCT:
    case TY_UNION:
        if (t->canon->vkind) {
            mg_s(b, t->canon->vkind == 1 ? "Dv" : "C");
            if (t->canon->vkind == 1) {
                mg_u(b, t->canon->len);
                mg_s(b, "_");
            }
            mg_type(b, t->canon->base);
        } else if (t->canon->tag) {
            mg_u(b, intern_len(t->canon->tag));
            mg_s(b, intern_str(t->canon->tag));
        } else {
            if (!t->canon->lname) {
                /* never named: no other unit can refer to it */
                static u32 seq;
                char tmp[32];
                sprintf(tmp, "__anon%u", seq++);
                t->canon->lname = intern_cstr(tmp);
            }
            mg_u(b, intern_len(t->canon->lname));
            mg_s(b, intern_str(t->canon->lname));
        }
        return;
    }
    mg_s(b, t->kind <= TY_LDOUBLE ? code[t->kind] : "u");
}

/* The symbol of a function with C++ linkage: its name, what a template was given, its parameters. */
static u32 mg_func(u32 name, const char *targs, Type *ft)
{
    Buf b;
    u32 k, id;
    memset(&b, 0, sizeof b);
    mg_s(&b, "_Z");
    mg_u(&b, intern_len(name));
    mg_s(&b, intern_str(name));
    if (targs) {
        mg_s(&b, "I");
        mg_s(&b, targs);
        mg_s(&b, "E");
    }
    for (k = 0; k < ft->nparams; k++)
        mg_type(&b, ft->params[k]);
    if (!ft->nparams)
        mg_s(&b, "v");
    id = intern((const char *)b.p, b.len);
    buf_free(&b);
    return id;
}

/* ---- expression helpers ---- */

static void set_expr(Expr *e, Type *ty, u32 v, int lval)
{
    e->ty = ty;
    e->v = v;
    e->lval = (u8)lval;
    e->bitoff = e->bitwidth = 0;
    e->konst = 0;
    e->hi = 0;
}

static void set_int(Expr *e, Type *ty, u64 v)
{
    set_expr(e, ty, ir_const(F, type_ir(ty), v), 0);
    e->konst = 1;
}

static u32 bit_extract(Type *ty, u32 unit, int bitoff, int bitwidth)
{
    int it = type_ir(ty), bits = it == IT_I64 ? 64 : 32;
    u32 v = ir_ins(F, OP_SHL, it, unit, ir_const(F, it, (u64)(bits - bitoff - bitwidth)));
    return ir_ins(F, type_is_unsigned(ty) ? OP_SHR : OP_SAR, it, v, ir_const(F, it, (u64)(bits - bitwidth)));
}

/* Lvalue conversion and array/function decay. */
static int strlit_byte(u32 sym, i64 off, u32 *byte);

static void rvalue(Expr *e)
{
    Type *t = e->ty;
    u32 byte;
    if (t->kind == TY_ARRAY) {
        e->ty = type_ptr(t->base);
        e->lval = 0;
        return;
    }
    if (t->kind == TY_FUNC) {
        e->ty = type_ptr(t);
        e->lval = 0;
        return;
    }
    if (!e->lval)
        return;
    e->lval = 0;
    if (type_is_record(t))
        return;
    if (t->kind == TY_VOID)
        return;
    if (type_is_128(t))
        e->hi = ir_load(F, IT_I64, M_64, ir_ins(F, OP_ADD, IT_I64, e->v, ir_const(F, IT_I64, 8)));
    if ((t->quals & (Q_ATOMIC | Q_VOLATILE)) && !e->bitwidth && atomic_able(t) && ((t->quals & Q_ATOMIC) || !opt_cxx)) {
        /* a volatile object is read like an atomic one with no ordering asked: a plain load, but one
           the optimiser cannot move, merge or drop */
        e->v = atomic_value(t, atomic_call(ATOM_LOAD, t, &e->v, 1, !(t->quals & Q_ATOMIC)));
        e->ty = unqual(t);
        return;
    }
    if (type_size(t) == 1 && F->ins[e->v].op == OP_ADDR && strlit_byte(F->ins[e->v].a, (i64)(i32)F->ins[e->v].b, &byte)) {
        e->v = ir_const(F, type_ir(t), !type_is_unsigned(t) ? (u64)(i64)(signed char)byte : byte);
        e->ty = unqual(t);
        return;
    }
    e->v = load_val(t, e->v);
    if (e->bitwidth) {
        e->v = bit_extract(t, e->v, e->bitoff, e->bitwidth);
        e->bitwidth = 0;
    }
    e->ty = unqual(t);
}

static Type *unqual(Type *t)
{
    if (!t->quals && !t->xalign)
        return t;
    switch (t->kind) {
    case TY_VOID: return ty_void;
    case TY_CHAR: return ty_char;
    case TY_SCHAR: return ty_schar;
    case TY_UCHAR: return ty_uchar;
    case TY_SHORT: return ty_short;
    case TY_USHORT: return ty_ushort;
    case TY_INT: return ty_int;
    case TY_UINT: return ty_uint;
    case TY_LONG: return ty_long;
    case TY_ULONG: return ty_ulong;
    case TY_LLONG: return ty_llong;
    case TY_ULLONG: return ty_ullong;
    case TY_BOOL: return ty_bool;
    case TY_INT128: return ty_int128;
    case TY_UINT128: return ty_uint128;
    case TY_FLOAT16: return ty_float16;
    case TY_BF16: return ty_bf16;
    case TY_FLOAT: return ty_float;
    case TY_DOUBLE: return ty_double;
    case TY_LDOUBLE: return ty_ldouble;
    case TY_PTR: return type_ptr(t->base);
    case TY_STRUCT:
    case TY_UNION: return t->canon;
    }
    return t;
}

/* Convert an rvalue to type "to". */
static void convert(Expr *e, Type *to)
{
    Type *from = e->ty;
    int fi, ti;
    to = unqual(to);
    if (special_convert(e, to))
        return;
    from = e->ty;
    if (to->kind == TY_VOID || type_is_record(to) || from->kind == TY_VOID) {
        e->ty = to;
        return;
    }
    if (type_is_128(from) && !type_is_128(to)) {
        if (to->kind == TY_BOOL)
            e->v = ir_ins(F, OP_OR, IT_I64, e->v, e->hi);
        else if (type_is_float(to))
            error_at(tok.loc, "128-bit integers cannot be converted to floating point");
        e->hi = 0;
        e->ty = from = ty_ulong; /* the lower half */
    } else if (type_is_128(to)) {
        if (!type_is_128(from)) {
            int sgn = !type_is_unsigned(from);
            if (!type_is_int(from) && from->kind != TY_PTR)
                error_at(tok.loc, "invalid conversion to a 128-bit integer");
            convert(e, sgn ? ty_long : ty_ulong);
            e->hi = sgn ? ir_ins(F, OP_SAR, IT_I64, e->v, ir_const(F, IT_I64, 63)) : ir_const(F, IT_I64, 0);
        }
        e->ty = to;
        return;
    }
    fi = type_ir(from);
    ti = type_ir(to);
    if (to->kind == TY_BOOL) {
        if (from->kind != TY_BOOL)
            e->v = type_is_float(from) ? ir_ins(F, OP_FNE, IT_I32, e->v, ir_fconst(F, fi, 0.0))
                                       : ir_ins(F, OP_NE, IT_I32, e->v, ir_const(F, fi, 0));
        e->ty = to;
        return;
    }
    if (type_is_float(to)) {
        if (type_is_float(from)) {
            if (fi != ti)
                e->v = ir_ins(F, ti == IT_F64 ? OP_FEXT : OP_FTRUNC, ti, e->v, 0);
        } else {
            e->v = ir_ins(F, type_is_unsigned(from) ? OP_UITOF : OP_SITOF, ti, e->v, 0);
        }
        e->ty = to;
        return;
    }
    if (type_is_float(from)) {
        e->v = ir_ins(F, type_is_unsigned(to) ? OP_FTOUI : OP_FTOSI, ti, e->v, 0);
    } else if (fi == IT_I32 && ti == IT_I64) {
        e->v = ir_ins(F, type_is_unsigned(from) ? OP_ZEXT32 : OP_SEXT32, IT_I64, e->v, 0);
    } else if (fi == IT_I64 && ti == IT_I32) {
        e->v = ir_ins(F, OP_TRUNC, IT_I32, e->v, 0);
    }
    if (from->kind != to->kind) {
        switch (to->kind) {
        case TY_CHAR:
        case TY_SCHAR:
            e->v = ir_ins(F, to->kind == TY_CHAR && type_is_unsigned(to) ? OP_ZEXT8 : OP_SEXT8, IT_I32, e->v, 0);
            break;
        case TY_UCHAR:
            e->v = ir_ins(F, OP_ZEXT8, IT_I32, e->v, 0);
            break;
        case TY_SHORT:
            if (from->kind != TY_CHAR && from->kind != TY_SCHAR && from->kind != TY_UCHAR)
                e->v = ir_ins(F, OP_SEXT16, IT_I32, e->v, 0);
            break;
        case TY_USHORT:
            if (from->kind != TY_UCHAR)
                e->v = ir_ins(F, OP_ZEXT16, IT_I32, e->v, 0);
            break;
        }
    }
    e->ty = to;
}

static u32 to_bool(Expr *e)
{
    int it = type_ir(e->ty);
    if (!type_is_scalar(e->ty))
        error_at(tok.loc, "scalar value required");
    if (type_is_128(e->ty))
        return ir_ins(F, OP_NE, IT_I32, ir_ins(F, OP_OR, IT_I64, e->v, e->hi), ir_const(F, IT_I64, 0));
    if (it == IT_F32 || it == IT_F64)
        return ir_ins(F, OP_FNE, IT_I32, e->v, ir_fconst(F, it, 0.0));
    if (F == const_func && F->ins[e->v].op == OP_ADDR)
        return ir_const(F, IT_I32, 1);
    return ir_ins(F, OP_NE, IT_I32, e->v, ir_const(F, it, 0));
}

static int is_null_const(Expr *e)
{
    u64 c;
    return e->konst && ir_is_const(F, e->v, &c) && c == 0 &&
           (type_is_int(e->ty) || (e->ty->kind == TY_PTR && e->ty->base->kind == TY_VOID));
}

/* A block copy or clear of size bytes; align: what both addresses are known to be multiples of (mem, for
   the targets that may not use wider loads and stores than that). */
static u32 emit_mem(int op, u32 dst, u32 src, u64 size, u32 align)
{
    u32 m = ir_raw(F, op, IT_VOID, dst, src);
    F->ins[m].c = (u32)size;
    F->ins[m].mem = (u8)(align > 16 ? 16 : align);
    if (F->cur)
        ir_append(F, F->cur, m);
    return m;
}

static void store(Expr *lv, u32 val)
{
    Type *t = lv->ty;
    if (type_is_record(t)) {
        emit_mem(OP_MEMCPY, lv->v, val, type_size(t), type_align(t));
        return;
    }
    if ((t->quals & (Q_ATOMIC | Q_VOLATILE)) && !lv->bitwidth && atomic_able(t) && ((t->quals & Q_ATOMIC) || !opt_cxx)) {
        u32 args[2];
        args[0] = lv->v;
        args[1] = atomic_raw(t, val);
        atomic_call(ATOM_STORE, t, args, 2, !(t->quals & Q_ATOMIC));
        return;
    }
    if (lv->bitwidth) {
        int it = type_ir(t);
        u64 mask = (lv->bitwidth >= 64 ? ~(u64)0 : (((u64)1 << lv->bitwidth) - 1)) << lv->bitoff;
        u32 unit = ir_load(F, it, type_mem(t), lv->v);
        u32 sh = ir_ins(F, OP_SHL, it, val, ir_const(F, it, lv->bitoff));
        sh = ir_ins(F, OP_AND, it, sh, ir_const(F, it, mask));
        unit = ir_ins(F, OP_AND, it, unit, ir_const(F, it, ~mask));
        val = ir_ins(F, OP_OR, it, unit, sh);
    }
    store_val(t, lv->v, val);
}

static void store_expr(Expr *lv, Expr *val)
{
    store(lv, val->v);
    if (type_is_128(lv->ty))
        ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, lv->v, ir_const(F, IT_I64, 8)), val->hi);
}

static void need_lvalue(Expr *e)
{
    if (!e->lval)
        error_at(tok.loc, "lvalue required");
    if (e->ty->kind == TY_ARRAY || e->ty->kind == TY_FUNC || !type_complete(e->ty))
        error_at(tok.loc, "cannot assign to this type");
}

/* lhs = rhs; leaves the assigned value in *lhs as an rvalue. */
static void binop(int op, Expr *l, Expr *r, u32 loc);
static void cx_binop(int op, Expr *l, Expr *r, u32 loc);
static void vec_binop(int op, Expr *l, Expr *r, u32 loc);

static void assign(Expr *lhs, Expr *rhs)
{
    need_lvalue(lhs);
    if (special_pair(lhs->ty, rhs->ty))
        convert(rhs, lhs->ty);
    if (type_is_record(lhs->ty)) {
        if (!type_is_record(rhs->ty) || lhs->ty->canon != rhs->ty->canon)
            error_at(tok.loc, "incompatible types in assignment");
    } else if (!type_is_scalar(rhs->ty) || (type_is_record(rhs->ty))) {
        error_at(tok.loc, "incompatible types in assignment");
    } else if ((lhs->ty->kind == TY_PTR) != (rhs->ty->kind == TY_PTR) &&
               (type_is_float(lhs->ty) || type_is_float(rhs->ty))) {
        error_at(tok.loc, "incompatible types in assignment");
    }
    convert(rhs, lhs->ty);
    store_expr(lhs, rhs);
    if (lhs->bitwidth)
        rhs->v = bit_extract(lhs->ty, ir_ins(F, OP_SHL, type_ir(lhs->ty), rhs->v,
                                             ir_const(F, type_ir(lhs->ty), lhs->bitoff)),
                             lhs->bitoff, lhs->bitwidth);
    *lhs = *rhs;
    lhs->lval = 0;
    lhs->bitwidth = 0;
    lhs->konst = 0;
}

static u32 select64(u32 c, u32 a, u32 b)
{
    u32 v = ir_raw(F, OP_SELECT, IT_I64, c, a);
    F->ins[v].c = b;
    if (F->cur)
        ir_append(F, F->cur, v);
    return v;
}

#define I64OP(op, a, b) ir_ins(F, op, IT_I64, a, b)
#define K64(x) ir_const(F, IT_I64, (u64)(x))

/* Arithmetic on 128-bit integers, done on the two halves. */
static void binop128(int op, Expr *l, Expr *r, u32 loc)
{
    u32 a = 0, ah = 0, b, bh, lo = 0, hi = 0, c;
    int uns;
    if (op == T_SHL || op == T_SHR) {
        u32 n, big, fill, inv;
        if (!type_is_128(l->ty)) {
            binop(op, l, r, loc);
            return;
        }
        uns = type_is_unsigned(l->ty);
        convert(r, ty_long);
        n = r->v;
        a = l->v;
        ah = l->hi;
        big = ir_ins(F, OP_NE, IT_I32, I64OP(OP_AND, n, K64(64)), K64(0));
        inv = I64OP(OP_SUB, K64(63), n); /* taken modulo 64 by the shift */
        if (op == T_SHR) {
            fill = uns ? K64(0) : I64OP(OP_SAR, ah, K64(63));
            hi = I64OP(uns ? OP_SHR : OP_SAR, ah, n);
            lo = I64OP(OP_OR, I64OP(OP_SHR, a, n), I64OP(OP_SHL, I64OP(OP_SHL, ah, K64(1)), inv));
            l->v = select64(big, hi, lo);
            l->hi = select64(big, fill, hi);
        } else {
            lo = I64OP(OP_SHL, a, n);
            hi = I64OP(OP_OR, I64OP(OP_SHL, ah, n), I64OP(OP_SHR, I64OP(OP_SHR, a, K64(1)), inv));
            l->hi = select64(big, lo, hi);
            l->v = select64(big, K64(0), lo);
        }
        return;
    }
    {
        Type *t = type_usual(l->ty, r->ty);
        if (!type_is_int(l->ty) || !type_is_int(r->ty))
            error_at(loc, "invalid operands to a 128-bit operation");
        convert(l, t);
        convert(r, t);
        uns = type_is_unsigned(t);
    }
    a = l->v;
    ah = l->hi;
    b = r->v;
    bh = r->hi;
    switch (op) {
    case T_PLUS:
        lo = I64OP(OP_ADD, a, b);
        c = ir_ins(F, OP_ZEXT32, IT_I64, ir_ins(F, OP_ULT, IT_I32, lo, a), 0);
        hi = I64OP(OP_ADD, I64OP(OP_ADD, ah, bh), c);
        break;
    case T_MINUS:
        lo = I64OP(OP_SUB, a, b);
        c = ir_ins(F, OP_ZEXT32, IT_I64, ir_ins(F, OP_ULT, IT_I32, a, b), 0);
        hi = I64OP(OP_SUB, I64OP(OP_SUB, ah, bh), c);
        break;
    case T_STAR:
        lo = I64OP(OP_MUL, a, b);
        hi = I64OP(OP_ADD, I64OP(OP_UMULH, a, b), I64OP(OP_ADD, I64OP(OP_MUL, a, bh), I64OP(OP_MUL, ah, b)));
        break;
    case T_AMP: lo = I64OP(OP_AND, a, b); hi = I64OP(OP_AND, ah, bh); break;
    case T_PIPE: lo = I64OP(OP_OR, a, b); hi = I64OP(OP_OR, ah, bh); break;
    case T_CARET: lo = I64OP(OP_XOR, a, b); hi = I64OP(OP_XOR, ah, bh); break;
    case T_EQ:
    case T_NE:
        lo = I64OP(OP_OR, I64OP(OP_XOR, a, b), I64OP(OP_XOR, ah, bh));
        set_expr(l, ty_int, ir_ins(F, op == T_EQ ? OP_EQ : OP_NE, IT_I32, lo, K64(0)), 0);
        return;
    case T_LT: case T_GT: case T_LE: case T_GE: {
        int strict, lohalf;
        u32 x = a, xh = ah, y = b, yh = bh, t;
        if (op == T_GT || op == T_LE) { /* a > b is b < a; a <= b is !(b < a) */
            x = b; xh = bh; y = a; yh = ah;
        }
        strict = uns ? OP_ULT : OP_SLT;
        lohalf = OP_ULT;
        t = ir_ins(F, OP_OR, IT_I32, ir_ins(F, strict, IT_I32, xh, yh),
                   ir_ins(F, OP_AND, IT_I32, ir_ins(F, OP_EQ, IT_I32, xh, yh), ir_ins(F, lohalf, IT_I32, x, y)));
        if (op == T_LE || op == T_GE)
            t = ir_ins(F, OP_EQ, IT_I32, t, ir_const(F, IT_I32, 0));
        set_expr(l, ty_int, t, 0);
        return;
    }
    case T_SLASH:
    case T_PERCENT: {
        /* by the C library: quotient and remainder into four doublewords */
        u32 slot = ir_new_slot(F, 32, 8), args[6], id = intern_cstr("__mcc_divmod128"), off = op == T_SLASH ? 0 : 16;
        Sym *ls;
        names_reserve(id);
        ls = gsyms[id];
        if (!ls) {
            ls = new_sym(id, SK_VAR, type_func(ty_void, 0, 0, 0, 0, 1));
            gsyms[id] = ls;
        }
        args[0] = ir_slot_addr(F, slot, 0);
        args[1] = a;
        args[2] = ah;
        args[3] = b;
        args[4] = bh;
        args[5] = ir_const(F, IT_I32, (u64)!uns);
        ir_call(F, IT_VOID, ir_addr(F, sym_obj(ls), 0), args, 6, 0xffffffffU, 0);
        lo = ir_load(F, IT_I64, M_64, ir_slot_addr(F, slot, (i32)off));
        hi = ir_load(F, IT_I64, M_64, ir_slot_addr(F, slot, (i32)off + 8));
        break;
    }
    default:
        error_at(loc, "'%s' is not supported on 128-bit integers", tok_names[op]);
    }
    l->v = lo;
    l->hi = hi;
}

static void binop(int op, Expr *l, Expr *r, u32 loc)
{
    Type *t;
    int it, irop = 0, isf, uns;

    if (type_is_128(l->ty) || (type_is_128(r->ty) && op != T_SHL && op != T_SHR && type_is_int(l->ty))) {
        binop128(op, l, r, loc);
        return;
    }
    if (VK(l->ty) || VK(r->ty)) {
        if (VK(l->ty) == 2 || VK(r->ty) == 2) {
            cx_binop(op, l, r, loc);
        } else {
            vec_binop(op, l, r, loc);
        }
        return;
    }

    if (op == T_PLUS || op == T_MINUS) {
        if (l->ty->kind == TY_PTR || r->ty->kind == TY_PTR) {
            u64 size;
            u32 vsize = 0; /* the size of what is pointed to, when only the running program knows it */
            if (op == T_PLUS && l->ty->kind != TY_PTR) {
                Expr tmp = *l;
                *l = *r;
                *r = tmp;
            }
            if (l->ty->base->kind == TY_VOID || l->ty->base->kind == TY_FUNC)
                size = 1;
            else if (l->ty->base->kind == TY_ARRAY && l->ty->base->vslot)
                vsize = ir_load(F, IT_I64, M_64, ir_slot_addr(F, l->ty->base->vslot - 1, 0));
            else if (!type_complete(l->ty->base))
                error_at(loc, "arithmetic on pointer to incomplete type");
            size = l->ty->base->kind == TY_VOID || l->ty->base->kind == TY_FUNC ? 1 : type_size(l->ty->base);
            if (r->ty->kind == TY_PTR) {
                if (op != T_MINUS)
                    error_at(loc, "invalid operands to binary +");
                l->v = ir_ins(F, OP_SUB, IT_I64, l->v, r->v);
                if (vsize)
                    l->v = ir_ins(F, OP_SDIV, IT_I64, l->v, vsize);
                else if (size != 1)
                    l->v = ir_ins(F, OP_SDIV, IT_I64, l->v, ir_const(F, IT_I64, size));
                l->ty = ty_long;
                return;
            }
            if (!type_is_int(r->ty))
                error_at(loc, "invalid operands to pointer arithmetic");
            {
                /* device code: in p + (i + k), where i + k was an int's sum just now (one that does not wrap, or
                   nothing is promised), the constant is taken out of the index, to go into the address */
                Ins *ri = &F->ins[r->v];
                u64 k;
                if (opt_dev_parse && !opt_noselk && !vsize && r->ty->kind == TY_INT && r->v == nsw_val && nsw_blk == F->cur &&
                    ri->op == OP_ADD && ri->ty == IT_I32 && ir_is_const(F, ri->b, &k)) {
                    u32 x = ir_ins(F, OP_SEXT32, IT_I64, ri->a, 0);
                    i64 off = (i64)(i32)(u32)k * (i64)size;
                    x = ir_ins(F, OP_MUL, IT_I64, x, ir_const(F, IT_I64, size));
                    l->v = ir_ins(F, op == T_PLUS ? OP_ADD : OP_SUB, IT_I64, l->v, x);
                    l->v = ir_ins(F, OP_ADD, IT_I64, l->v, ir_const(F, IT_I64, (u64)(op == T_PLUS ? off : -off)));
                    return;
                }
            }
            convert(r, ty_long);
            r->v = ir_ins(F, OP_MUL, IT_I64, r->v, vsize ? vsize : ir_const(F, IT_I64, size));
            l->v = ir_ins(F, op == T_PLUS ? OP_ADD : OP_SUB, IT_I64, l->v, r->v);
            return;
        }
    }
    if (op == T_SHL || op == T_SHR) {
        if (!type_is_int(l->ty) || !type_is_int(r->ty))
            error_at(loc, "invalid operands to shift");
        convert(l, type_promote(l->ty));
        convert(r, type_ir(l->ty) == IT_I64 ? ty_long : ty_int);
        it = type_ir(l->ty);
        l->v = ir_ins(F, op == T_SHL ? OP_SHL : type_is_unsigned(l->ty) ? OP_SHR : OP_SAR, it, l->v, r->v);
        return;
    }
    if (op >= T_LT && op <= T_NE && (l->ty->kind == TY_PTR || r->ty->kind == TY_PTR)) {
        if (l->ty->kind != TY_PTR)
            convert(l, r->ty);
        if (r->ty->kind != TY_PTR)
            convert(r, l->ty);
        switch (op) {
        case T_LT: irop = OP_ULT; break;
        case T_GT: irop = OP_UGT; break;
        case T_LE: irop = OP_ULE; break;
        case T_GE: irop = OP_UGE; break;
        case T_EQ: irop = OP_EQ; break;
        default: irop = OP_NE; break;
        }
        if (F == const_func && (irop == OP_EQ || irop == OP_NE)) {
            /* in a constant: the address of an object is not null */
            u64 c;
            if ((F->ins[l->v].op == OP_ADDR && ir_is_const(F, r->v, &c) && c == 0) ||
                (F->ins[r->v].op == OP_ADDR && ir_is_const(F, l->v, &c) && c == 0)) {
                set_int(l, ty_int, irop == OP_NE);
                return;
            }
        }
        l->v = ir_ins(F, irop, IT_I32, l->v, r->v);
        l->ty = ty_int;
        return;
    }
    if (!type_is_arith(l->ty) || !type_is_arith(r->ty))
        error_at(loc, "invalid operands to binary '%s'", tok_names[op]);
    t = type_usual(l->ty, r->ty);
    isf = type_is_float(t);
    uns = type_is_unsigned(t);
    if (isf && (op == T_PERCENT || op == T_AMP || op == T_PIPE || op == T_CARET))
        error_at(loc, "invalid operands to binary '%s'", tok_names[op]);
    convert(l, t);
    convert(r, t);
    it = type_ir(t);
    switch (op) {
    case T_STAR: irop = isf ? OP_FMUL : OP_MUL; break;
    case T_SLASH: irop = isf ? OP_FDIV : uns ? OP_UDIV : OP_SDIV; break;
    case T_PERCENT: irop = uns ? OP_UREM : OP_SREM; break;
    case T_PLUS: irop = isf ? OP_FADD : OP_ADD; break;
    case T_MINUS: irop = isf ? OP_FSUB : OP_SUB; break;
    case T_AMP: irop = OP_AND; break;
    case T_PIPE: irop = OP_OR; break;
    case T_CARET: irop = OP_XOR; break;
    case T_LT: irop = isf ? OP_FLT : uns ? OP_ULT : OP_SLT; break;
    case T_GT: irop = isf ? OP_FGT : uns ? OP_UGT : OP_SGT; break;
    case T_LE: irop = isf ? OP_FLE : uns ? OP_ULE : OP_SLE; break;
    case T_GE: irop = isf ? OP_FGE : uns ? OP_UGE : OP_SGE; break;
    case T_EQ: irop = isf ? OP_FEQ : OP_EQ; break;
    case T_NE: irop = isf ? OP_FNE : OP_NE; break;
    }
    if (op >= T_LT && op <= T_NE) {
        l->v = ir_ins(F, irop, IT_I32, l->v, r->v);
        l->ty = ty_int;
    } else {
        l->v = ir_ins(F, irop, it, l->v, r->v);
        if (irop == OP_ADD || irop == OP_SUB) {
            nsw_val = t->kind == TY_INT ? l->v : 0;
            nsw_blk = F->cur;
        }
        if (t->kind == TY_FLOAT16)
            l->v = rt_call("__mcc_fround16", ty_float, ty_float, l->v);
        else if (t->kind == TY_BF16)
            l->v = rt_call("__mcc_froundbf", ty_float, ty_float, l->v);
    }
}

/* ---- expressions ---- */

static u32 new_temp(Type *t)
{
    return ir_slot_addr(F, ir_new_slot(F, type_size(t), type_align(t)), 0);
}

/* ---- half floats, vectors and complex numbers ---- */

/* A call of a library function of one argument. */
static u32 rt_call(const char *name, Type *ret, Type *pt, u32 arg)
{
    u32 id = intern_cstr(name);
    Sym *s;
    names_reserve(id);
    s = gsyms[id];
    if (!s) {
        Type **ps = (Type **)arena_alloc(&A, sizeof(Type *));
        ps[0] = pt;
        s = new_sym(id, SK_VAR, type_func(ret, ps, 0, 1, 0, 0));
        gsyms[id] = s;
    }
    return ir_call(F, type_ir(ret), ir_addr(F, sym_obj(s), 0), &arg, 1, 0xffffffffU, 0);
}

/* A _Float16 lives in memory as its 16 bits and in registers as a float. */
static u32 load_val(Type *t, u32 addr)
{
    if (t->kind == TY_FLOAT16)
        return rt_call("__mcc_h2f", ty_float, ty_uint, ir_load(F, IT_I32, M_16U, addr));
    if (t->kind == TY_BF16)
        return rt_call("__mcc_bf2f", ty_float, ty_uint, ir_load(F, IT_I32, M_16U, addr));
    return ir_load(F, type_ir(t), type_mem(t), addr);
}

static void store_val(Type *t, u32 addr, u32 v)
{
    if (t->kind == TY_FLOAT16)
        v = rt_call("__mcc_f2h", ty_uint, ty_float, v);
    else if (t->kind == TY_BF16)
        v = rt_call("__mcc_f2bf", ty_uint, ty_float, v);
    ir_store(F, type_mem(t), addr, v);
}

typedef struct VecType {
    Type *elem, *ty;
    u32 n;
    int vkind;
    struct VecType *next;
} VecType;

static VecType *vectypes;

/* The vector (vkind 1) or complex (2) type of n elements: a struct holding an array. */
static Type *type_vector(Type *elem, u32 n, int vkind)
{
    VecType *v;
    Field *f;
    Type *t;
    elem = unqual(elem);
    for (v = vectypes; v; v = v->next)
        if (v->elem == elem && v->n == n && v->vkind == vkind)
            return v->ty;
    if (!type_is_arith(elem) || n == 0)
        error_at(tok.loc, "invalid vector type");
    t = type_record(TY_STRUCT, 0);
    f = (Field *)arena_alloc(&A, sizeof(Field));
    memset(f, 0, sizeof *f);
    f->name = intern_cstr("__v");
    f->ty = type_array(elem, n, 1);
    type_layout(t, f, 0, 0);
    t->vkind = (u8)vkind;
    t->base = elem;
    t->len = n;
    v = (VecType *)arena_alloc(&A, sizeof(VecType));
    v->elem = elem;
    v->n = n;
    v->vkind = vkind;
    v->ty = t;
    v->next = vectypes;
    vectypes = v;
    return t;
}

/* Element i of the vector or complex value v, as an lvalue. */
static void vec_elem(const Expr *v, u32 i, Expr *out)
{
    Type *et = v->ty->canon->base;
    set_expr(out, et, ir_ins(F, OP_ADD, IT_I64, v->v, ir_const(F, IT_I64, i * type_size(et))), 1);
}

static void vec_put(u32 tmp, Type *et, u32 i, u32 v)
{
    Expr d;
    set_expr(&d, et, ir_ins(F, OP_ADD, IT_I64, tmp, ir_const(F, IT_I64, i * type_size(et))), 1);
    store(&d, v);
}

static Type *int_of_size(u64 size)
{
    return size == 1 ? ty_schar : size == 2 ? ty_short : size == 4 ? ty_int : ty_long;
}

/* Vector arithmetic, element by element; a scalar operand stands for a vector of it. */
static void vec_binop(int op, Expr *l, Expr *r, u32 loc)
{
    Type *vt = VK(l->ty) ? l->ty->canon : r->ty->canon, *rt = vt;
    u32 n = (u32)vt->len, i, tmp;
    int cmp = op >= T_LT && op <= T_NE;
    if (VK(l->ty) && VK(r->ty) && l->ty->canon->len != r->ty->canon->len)
        error_at(loc, "vector operands differ in length");
    if ((!VK(l->ty) && !type_is_arith(l->ty)) || (!VK(r->ty) && !type_is_arith(r->ty)))
        error_at(loc, "invalid operands to binary '%s'", tok_names[op]);
    if (cmp)
        rt = type_vector(int_of_size(type_size(vt->base)), n, 1);
    tmp = new_temp(rt);
    for (i = 0; i < n; i++) {
        Expr a, b;
        if (VK(l->ty)) {
            vec_elem(l, i, &a);
            rvalue(&a);
        } else {
            a = *l;
        }
        if (VK(r->ty)) {
            vec_elem(r, i, &b);
            rvalue(&b);
        } else {
            b = *r;
        }
        binop(op, &a, &b, loc);
        if (cmp) {
            a.v = ir_ins(F, OP_NEG, IT_I32, a.v, 0);
            a.ty = ty_int;
        }
        convert(&a, rt->base);
        vec_put(tmp, rt->base, i, a.v);
    }
    set_expr(l, rt, tmp, 0);
}

static void vec_unary(int op, Expr *e)
{
    Type *vt = e->ty->canon, *et = vt->base;
    u32 n = (u32)vt->len, i, tmp = new_temp(vt);
    for (i = 0; i < n; i++) {
        Expr a;
        vec_elem(e, i, &a);
        rvalue(&a);
        if (op == T_MINUS)
            a.v = ir_ins(F, type_is_float(et) ? OP_FNEG : OP_NEG, type_ir(et), a.v, 0);
        else if (op == T_TILDE)
            a.v = ir_ins(F, OP_NOT, type_ir(et), a.v, 0);
        vec_put(tmp, et, i, a.v);
    }
    set_expr(e, vt, tmp, 0);
}

/* The real and imaginary parts of e, complex or real, as values of type et. */
static void cx_parts(Expr *e, Type *et, u32 *re, u32 *im)
{
    Expr a;
    if (VK(e->ty) == 2) {
        vec_elem(e, 0, &a);
        rvalue(&a);
        convert(&a, et);
        *re = a.v;
        vec_elem(e, 1, &a);
        rvalue(&a);
        convert(&a, et);
        *im = a.v;
    } else {
        a = *e;
        if (!type_is_arith(a.ty))
            error_at(tok.loc, "invalid operand to complex arithmetic");
        convert(&a, et);
        *re = a.v;
        *im = ir_fconst(F, type_ir(et), 0.0);
    }
}

static void cx_make(Expr *e, Type *ct, u32 re, u32 im)
{
    u32 tmp = new_temp(ct);
    vec_put(tmp, ct->base, 0, re);
    vec_put(tmp, ct->base, 1, im);
    set_expr(e, ct, tmp, 0);
}

static void cx_binop(int op, Expr *l, Expr *r, u32 loc)
{
    Type *lt = VK(l->ty) == 2 ? l->ty->canon->base : l->ty, *rt = VK(r->ty) == 2 ? r->ty->canon->base : r->ty, *et;
    u32 a, b, c, d, re, im;
    int it;
    if (VK(l->ty) == 1 || VK(r->ty) == 1 || !type_is_arith(lt) || !type_is_arith(rt))
        error_at(loc, "invalid operands to binary '%s'", tok_names[op]);
    et = type_usual(lt, rt);
    if (!type_is_float(et) || et->kind <= TY_FLOAT16)
        et = ty_double;
    it = type_ir(et);
    cx_parts(l, et, &a, &b);
    cx_parts(r, et, &c, &d);
#define FOP(o, x, y) ir_ins(F, o, it, x, y)
    switch (op) {
    case T_PLUS:
        re = FOP(OP_FADD, a, c);
        im = FOP(OP_FADD, b, d);
        break;
    case T_MINUS:
        re = FOP(OP_FSUB, a, c);
        im = FOP(OP_FSUB, b, d);
        break;
    case T_STAR:
        re = FOP(OP_FSUB, FOP(OP_FMUL, a, c), FOP(OP_FMUL, b, d));
        im = FOP(OP_FADD, FOP(OP_FMUL, a, d), FOP(OP_FMUL, b, c));
        break;
    case T_SLASH: {
        u32 den = FOP(OP_FADD, FOP(OP_FMUL, c, c), FOP(OP_FMUL, d, d));
        re = FOP(OP_FDIV, FOP(OP_FADD, FOP(OP_FMUL, a, c), FOP(OP_FMUL, b, d)), den);
        im = FOP(OP_FDIV, FOP(OP_FSUB, FOP(OP_FMUL, b, c), FOP(OP_FMUL, a, d)), den);
        break;
    }
    case T_EQ:
    case T_NE:
        re = ir_ins(F, OP_AND, IT_I32, ir_ins(F, OP_FEQ, IT_I32, a, c), ir_ins(F, OP_FEQ, IT_I32, b, d));
        if (op == T_NE)
            re = ir_ins(F, OP_XOR, IT_I32, re, ir_const(F, IT_I32, 1));
        set_expr(l, ty_int, re, 0);
        return;
    default:
        error_at(loc, "invalid operands to binary '%s'", tok_names[op]);
        return;
    }
#undef FOP
    cx_make(l, type_vector(et, 2, 2), re, im);
}

/* One of the two types is complex and they are not the same type: a conversion is due. */
static int special_pair(Type *to, Type *from)
{
    if (opt_cxx && to->kind == TY_PTR && to->base->kind == TY_FUNC && from->kind == TY_STRUCT && from->canon->lam)
        return 1;
    if (VK(to) != 2 && VK(from) != 2)
        return 0;
    return !(VK(to) == 2 && VK(from) == 2 && to->canon == from->canon);
}

/* The conversions convert() leaves to this: to and from complex types, and half floats. */
static int special_convert(Expr *e, Type *to)
{
    Type *from = e->ty;
    if (opt_cxx && to->kind == TY_PTR && to->base->kind == TY_FUNC && from->kind == TY_STRUCT && from->canon->lam) {
        lambda_to_fn(e, to);
        return 1;
    }
    if (VK(to) == 2) {
        u32 re, im;
        if (VK(from) == 2 && from->canon == to->canon) {
            e->ty = to;
            return 1;
        }
        cx_parts(e, to->canon->base, &re, &im);
        cx_make(e, to->canon, re, im);
        return 1;
    }
    if (VK(from) == 2 && (type_is_arith(to) || to->kind == TY_PTR)) {
        Expr a;
        vec_elem(e, 0, &a);
        rvalue(&a);
        convert(&a, to);
        *e = a;
        return 1;
    }
    if (to->kind == TY_BF16) {
        if (from->kind == TY_BF16) {
            e->ty = to;
            return 1;
        }
        if (!type_is_arith(from))
            return 0;
        convert(e, ty_float);
        e->v = rt_call("__mcc_froundbf", ty_float, ty_float, e->v);
        e->ty = to;
        return 1;
    }
    if (to->kind == TY_FLOAT16) {
        if (from->kind == TY_FLOAT16) {
            e->ty = to;
            return 1;
        }
        if (!type_is_arith(from))
            return 0;
        if (from->kind == TY_DOUBLE || from->kind == TY_LDOUBLE) {
            e->v = rt_call("__mcc_dround16", ty_float, ty_double, e->v);
        } else {
            convert(e, ty_float);
            e->v = rt_call("__mcc_fround16", ty_float, ty_float, e->v);
        }
        e->ty = to;
        return 1;
    }
    if (from->kind == TY_FLOAT16 || from->kind == TY_BF16)
        e->ty = ty_float; /* the same value */
    return 0;
}

/* An anonymous read-only object holding a string literal. */
typedef struct StrLit {
    u32 hash, sym, len;
    u64 off;
} StrLit;

static StrLit *strlits; /* literals of this translation unit, hashed: equal ones share storage */
static u32 nstrlits, cap_strlits;

static u32 string_sym(const u8 *data, size_t len)
{
    u32 h = HASH_INIT, k, mask;
    u64 off;
    StrLit *e;
    for (k = 0; k < len; k++)
        h = HASH_STEP(h, data[k]);
    if (nstrlits * 2 >= cap_strlits) {
        u32 cap = cap_strlits ? cap_strlits * 2 : 256;
        StrLit *tab = (StrLit *)xmalloc(cap * sizeof(StrLit));
        memset(tab, 0, cap * sizeof(StrLit));
        for (k = 0; k < cap_strlits; k++) {
            u32 j;
            if (!strlits[k].sym)
                continue;
            for (j = strlits[k].hash & (cap - 1); tab[j].sym; j = (j + 1) & (cap - 1))
                ;
            tab[j] = strlits[k];
        }
        free(strlits);
        strlits = tab;
        cap_strlits = cap;
    }
    mask = cap_strlits - 1;
    for (k = h & mask; strlits[k].sym; k = (k + 1) & mask) {
        e = &strlits[k];
        if (e->hash == h && e->len == len && memcmp(O->sec[SEC_CONST].p + e->off, data, len) == 0)
            return e->sym - 1;
    }
    off = obj_reserve(O, SEC_CONST, len, 1);
    buf_put(&O->sec[SEC_CONST], data, len);
    e = &strlits[k];
    e->hash = h;
    e->len = (u32)len;
    e->off = off;
    e->sym = obj_add_sym(O, 0, SEC_CONST, off, 0) + 1;
    O->syms[e->sym - 1].dsize = len;
    nstrlits++;
    return e->sym - 1;
}

/* The byte at an offset in a string literal, so that "abc"[1] is a constant. */
static int strlit_byte(u32 sym, i64 off, u32 *byte)
{
    u32 k;
    if (O->syms[sym].name || O->syms[sym].sec != SEC_CONST || off < 0)
        return 0;
    for (k = 0; k < cap_strlits; k++)
        if (strlits[k].sym == sym + 1 && (u64)off < strlits[k].len) {
            *byte = O->sec[SEC_CONST].p[strlits[k].off + (u64)off];
            return 1;
        }
    return 0;
}

/* Reads adjacent string literal tokens into b, including the terminator. */
static int string_literal(Buf *b)
{
    int wide = 0;
    while (tok.kind == T_STR) {
        if (tok.flags & TF_WIDE)
            wide = 1;
        lex_string(&tok, b);
        next();
    }
    if (wide)
        buf_u32(b, 0);
    else
        buf_u8(b, 0);
    return wide;
}

/* Linux passes a record of up to 16 bytes as its one or two doublewords,
   in registers if both fit, and returns it in x0 and x1. The number of
   doublewords, or 0 if the record goes by address. */
static u32 x64_rec(Type *t, int *sse);

static u32 abi_words(Type *t)
{
    u64 size;
    if (opt_target != TARGET_LINUX || !type_is_record(t) || !type_complete(t))
        return 0;
    if (opt_arch == ARCH_X64)
        return x64_rec(t, 0);
    size = type_size(t);
    return size > 16 ? 0 : (u32)((size + 7) / 8);
}

/* The doubleword at off of the size bytes at addr; nothing past them is read. */
static u32 record_word(u32 addr, u64 size, u32 off)
{
    static const u8 piece[] = {4, 2, 1}, mem[] = {M_32, M_16U, M_8U};
    u64 rem = size - off, pos = 0;
    u32 v = 0, k;
    if (rem >= 8)
        return ir_load(F, IT_I64, M_64, ir_ins(F, OP_ADD, IT_I64, addr, ir_const(F, IT_I64, off)));
    for (k = 0; k < 3; k++)
        if (rem - pos >= piece[k]) {
            u32 x = ir_load(F, IT_I32, mem[k], ir_ins(F, OP_ADD, IT_I64, addr, ir_const(F, IT_I64, off + pos)));
            x = ir_ins(F, OP_ZEXT32, IT_I64, x, 0);
            if (pos)
                x = ir_ins(F, OP_SHL, IT_I64, x, ir_const(F, IT_I64, 8 * pos));
            v = v ? ir_ins(F, OP_OR, IT_I64, v, x) : x;
            pos += piece[k];
        }
    return v;
}

/* Linux leaves the upper bits of a narrow argument or result undefined: whoever reads it extends it. */
static u32 abi_extend(Type *t, u32 v)
{
    u64 size;
    if (opt_target != TARGET_LINUX || !type_is_int(t) || (size = type_size(t)) > 2)
        return v;
    if (t->kind == TY_BOOL || type_is_unsigned(t))
        return ir_ins(F, size == 1 ? OP_ZEXT8 : OP_ZEXT16, IT_I32, v, 0);
    return ir_ins(F, size == 1 ? OP_SEXT8 : OP_SEXT16, IT_I32, v, 0);
}

/* ---- x86-64: the System V calling convention ----
   A record of up to 16 bytes is one or two eightbytes, each INTEGER or SSE
   by what lies in it, and travels in registers of those classes if enough
   are left for all of it; anything else is copied to the stack (OP_BYVAL at
   a call, a block parameter in the function called). */

/* Marks the classes of the eightbytes t covers at off: 1 SSE, 2 INTEGER; 0 if something is not aligned. */
static int x64_classify(Type *t, u64 off, u8 *cls)
{
    Field *fl;
    u64 k;
    if (type_is_record(t)) {
        for (fl = t->canon->fields; fl; fl = fl->next) {
            if (fl->bitwidth) {
                cls[(off + fl->off + fl->bitoff / 8) / 8] = 2;
                continue;
            }
            if (!type_complete(fl->ty))
                continue; /* a flexible array member */
            if (!x64_classify(fl->ty, off + fl->off, cls))
                return 0;
        }
        return 1;
    }
    if (t->kind == TY_ARRAY) {
        for (k = 0; k < t->len; k++)
            if (!x64_classify(t->base, off + k * type_size(t->base), cls))
                return 0;
        return 1;
    }
    if (off % type_align(t))
        return 0;
    if (type_is_float(t)) {
        if (!cls[off / 8])
            cls[off / 8] = 1;
    } else {
        cls[off / 8] = 2;
        if (type_is_128(t))
            cls[off / 8 + 1] = 2;
    }
    return 1;
}

/* The number of eightbytes of record t if it travels in registers (sse[k]: eightbyte k is of class SSE), else 0. */
static u32 x64_rec(Type *t, int *sse)
{
    u8 cls[4];
    u64 size;
    if (!type_is_record(t) || !type_complete(t))
        return 0;
    size = type_size(t);
    if (size == 0 || size > 16)
        return 0;
    memset(cls, 0, sizeof cls);
    if (!x64_classify(t, 0, cls))
        return 0;
    if (sse) {
        sse[0] = cls[0] == 1;
        sse[1] = size > 8 && cls[1] == 1;
    }
    return size > 8 ? 2 : 1;
}

/* Eightbyte k of the record of size bytes at addr, as the value that goes in a register. */
static u32 x64_piece(u32 addr, u64 size, u32 k, int sse)
{
    if (!sse)
        return record_word(addr, size, 8 * k);
    addr = ir_ins(F, OP_ADD, IT_I64, addr, ir_const(F, IT_I64, 8 * k));
    return size - 8 * k >= 8 ? ir_load(F, IT_F64, M_F64, addr) : ir_load(F, IT_F32, M_F32, addr);
}

/* The size bytes at addr as an argument that is copied to the stack. */
static u32 x64_byval(u32 addr, u64 size, u32 align)
{
    u32 v = ir_raw(F, OP_BYVAL, IT_I64, addr, (u32)size);
    F->ins[v].c = align;
    if (F->cur)
        ir_append(F, F->cur, v);
    return v;
}

/* A record or 128-bit argument joins args; *ngr and *nfr count the registers taken so far. */
static u32 x64_arg(Expr *a, u32 *args, u32 nargs, u32 *ngr, u32 *nfr)
{
    int sse[2];
    u32 words, k, gr = 0, fr = 0;
    u64 size = type_size(a->ty);
    if (type_is_128(a->ty)) {
        if (*ngr + 2 <= 6) {
            args[nargs++] = a->v;
            args[nargs++] = a->hi;
            *ngr += 2;
        } else {
            u32 tmp = new_temp(a->ty);
            ir_store(F, M_64, tmp, a->v);
            ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, tmp, ir_const(F, IT_I64, 8)), a->hi);
            args[nargs++] = x64_byval(tmp, 16, 16);
        }
        return nargs;
    }
    if (!size)
        return nargs; /* an empty record is not passed at all */
    words = x64_rec(a->ty, sse);
    for (k = 0; k < words; k++) {
        gr += !sse[k];
        fr += sse[k];
    }
    if (words && *ngr + gr <= 6 && *nfr + fr <= 8) {
        for (k = 0; k < words; k++)
            args[nargs++] = x64_piece(a->v, size, k, sse[k]);
        *ngr += gr;
        *nfr += fr;
    } else {
        args[nargs++] = x64_byval(a->v, size, type_align(a->ty));
    }
    return nargs;
}

/* The parameter p of type pt, a record or a 128-bit integer, of the function being started. */
/* Is there a pointer member at byte off of record t, filling the doubleword there? (A kernel's: an address of
   global memory, which loads through it can say.) */
static int rec_ptr_at(Type *t, u64 off)
{
    Field *fl;
    if (t->kind == TY_PTR)
        return off == 0;
    if (t->kind == TY_ARRAY && t->base && type_size(t->base))
        return rec_ptr_at(t->base, off % type_size(t->base));
    if (!type_is_record(t) || t->vkind)
        return 0;
    for (fl = t->fields; fl; fl = fl->next)
        if (!fl->bitwidth && off >= fl->off && off < fl->off + type_size(fl->ty))
            return rec_ptr_at(fl->ty, off - fl->off);
    return 0;
}

static void x64_param(Sym *p, Type *pt)
{
    int sse[2];
    u64 size = type_size(pt), off;
    u32 words = x64_rec(pt, sse), k, gr = 0, fr = 0, v;
    if (type_is_128(pt)) {
        words = 2;
        sse[0] = sse[1] = 0;
    }
    for (k = 0; k < words; k++) {
        gr += !sse[k];
        fr += sse[k];
    }
    if (words && cur_named_gr + gr <= 6 && cur_named_fr + fr <= 8) {
        /* in registers: put together in a slot that holds all of every eightbyte */
        p->slot = ir_new_slot(F, 8 * words, type_align(pt) > 8 ? type_align(pt) : 8);
        for (k = 0; k < words; k++) {
            int it = !sse[k] ? IT_I64 : size - 8 * k >= 8 ? IT_F64 : IT_F32;
            v = ir_add_param(F, it);
            if (cur_fn_kernel && it == IT_I64 && rec_ptr_at(pt, 8 * k))
                F->ins[v].mem = PARAM_GLOBAL;
            ir_store(F, it == IT_I64 ? M_64 : it == IT_F64 ? M_F64 : M_F32, ir_slot_addr(F, p->slot, 8 * k), v);
        }
        cur_named_gr += gr;
        cur_named_fr += fr;
        return;
    }
    if (cur_fn_kernel) {
        /* a kernel's: the doublewords of the block are parameters of their own, put together in a slot, so
           that the members can become values */
        u32 nw = (u32)((size + 7) / 8);
        p->slot = ir_new_slot(F, 8 * nw, type_align(pt) > 8 ? type_align(pt) : 8);
        for (k = 0; k < nw; k++) {
            v = ir_add_param(F, IT_I64);
            if (k)
                F->param_ty[F->nparams - 1] = 0x40;
            else
                F->param_ty[F->nparams - 1] |= 0x80 | (type_align(pt) > 8 ? 0x20 : 0);
            if (rec_ptr_at(pt, 8 * k))
                F->ins[v].mem = PARAM_GLOBAL;
            ir_store(F, M_64, ir_slot_addr(F, p->slot, 8 * k), v);
        }
        return;
    }
    /* on the stack: the parameter is the block itself */
    v = ir_add_param(F, IT_I64);
    F->param_ty[F->nparams - 1] |= 0x80 | (type_align(pt) > 8 ? 0x20 : 0);
    for (off = 8; off < size; off += 8) {
        ir_add_param(F, IT_I64);
        F->param_ty[F->nparams - 1] = 0x40;
    }
    p->slot = ir_new_slot(F, 8, 8);
    p->byref = 1;
    ir_store(F, M_64, ir_slot_addr(F, p->slot, 0), v);
}

/* va_start(ap, ...): how far into the saved registers the unnamed arguments begin, where the ones on the
   stack are, and where the registers were saved. */
static void x64_va_start(Expr *ap)
{
    u32 gr = cur_named_gr > 6 ? 6 : cur_named_gr, fr = cur_named_fr > 8 ? 8 : cur_named_fr, k, p;
    rvalue(ap);
    p = ap->v;
    ir_store(F, M_32, p, ir_const(F, IT_I32, 8 * gr));
    ir_store(F, M_32, ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 4)), ir_const(F, IT_I32, 48 + 16 * fr));
    for (k = 0; k < 2; k++) {
        u32 v = ir_raw(F, OP_VASTART, IT_I64, k, 0);
        if (F->cur)
            ir_append(F, F->cur, v);
        ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 8 + 8 * k)), v);
    }
}

/* va_arg(ap, t): from the saved registers of its class (or classes) while they last, else from the stack. */
static void x64_va_arg(Expr *e, Expr *ap, Type *t)
{
    int sse[2], rec = type_is_record(t);
    u64 size = type_size(t);
    u32 words = rec ? x64_rec(t, sse) : type_is_128(t) ? 2 : 1, gr = 0, fr = 0, k, p, a;
    u32 slot, gp_at, fp_at, stk_at, gp = 0, fp = 0, ok = 0, c;
    u32 reg, stk = 0, done = 0;
    u64 step = rec ? (size + 7) & ~(u64)7 : type_is_128(t) ? 16 : 8;
    rvalue(ap);
    p = ap->v;
    if (rec && !size) {
        set_expr(e, t, new_temp(t), 1);
        return;
    }
    if (!rec) {
        sse[0] = type_is_float(t);
        sse[1] = 0;
    }
    for (k = 0; k < words; k++) {
        gr += !sse[k];
        fr += sse[k];
    }
    slot = new_temp(type_ptr(ty_char));
    gp_at = p;
    fp_at = ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 4));
    stk_at = ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 8));
    if (words && F->cur) {
        u32 base, gpx, fpx;
        reg = ir_new_block(F);
        stk = ir_new_block(F);
        done = ir_new_block(F);
        if (gr) {
            gp = ir_load(F, IT_I32, M_32, gp_at);
            ok = ir_ins(F, OP_ULE, IT_I32, gp, ir_const(F, IT_I32, 48 - 8 * gr));
        }
        if (fr) {
            fp = ir_load(F, IT_I32, M_32, fp_at);
            c = ir_ins(F, OP_ULE, IT_I32, fp, ir_const(F, IT_I32, 176 - 16 * fr));
            ok = ok ? ir_ins(F, OP_AND, IT_I32, ok, c) : c;
        }
        ir_br(F, ok, reg, stk);
        F->cur = reg;
        base = ir_load(F, IT_I64, M_64, ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 16)));
        gpx = gr ? ir_ins(F, OP_ADD, IT_I64, base, ir_ins(F, OP_ZEXT32, IT_I64, gp, 0)) : 0;
        fpx = fr ? ir_ins(F, OP_ADD, IT_I64, base, ir_ins(F, OP_ZEXT32, IT_I64, fp, 0)) : 0;
        if (words == 1 || !fr) {
            a = gr ? gpx : fpx; /* it lies there as it is */
        } else {
            /* an eightbyte from each kind of register, or two xmm registers (saved 16 bytes apart) */
            u32 ng = 0, nf = 0;
            a = ir_slot_addr(F, ir_new_slot(F, 16, 16), 0);
            for (k = 0; k < words; k++) {
                u32 src = sse[k] ? ir_ins(F, OP_ADD, IT_I64, fpx, ir_const(F, IT_I64, 16 * nf++))
                                 : ir_ins(F, OP_ADD, IT_I64, gpx, ir_const(F, IT_I64, 8 * ng++));
                ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, a, ir_const(F, IT_I64, 8 * k)), ir_load(F, IT_I64, M_64, src));
            }
        }
        if (gr)
            ir_store(F, M_32, gp_at, ir_ins(F, OP_ADD, IT_I32, gp, ir_const(F, IT_I32, 8 * gr)));
        if (fr)
            ir_store(F, M_32, fp_at, ir_ins(F, OP_ADD, IT_I32, fp, ir_const(F, IT_I32, 16 * fr)));
        ir_store(F, M_64, slot, a);
        ir_jmp(F, done);
        F->cur = stk;
    }
    if (F->cur) {
        a = ir_load(F, IT_I64, M_64, stk_at);
        if (type_align(t) > 8)
            a = ir_ins(F, OP_AND, IT_I64, ir_ins(F, OP_ADD, IT_I64, a, ir_const(F, IT_I64, 15)), ir_const(F, IT_I64, (u64)-16));
        ir_store(F, M_64, stk_at, ir_ins(F, OP_ADD, IT_I64, a, ir_const(F, IT_I64, step)));
        ir_store(F, M_64, slot, a);
        if (done) {
            ir_jmp(F, done);
            F->cur = done;
        }
    }
    set_expr(e, t, ir_load(F, IT_I64, M_64, slot), 1);
}

/* IF_SSE0 and IF_SSE1 for a call that returns a record of type t in registers. */
static int x64_ret_flags(Type *t)
{
    int sse[2];
    if (opt_arch != ARCH_X64 || !x64_rec(t, sse))
        return 0;
    return (sse[0] ? IF_SSE0 : 0) | (sse[1] ? IF_SSE1 : 0);
}

/* ---- C++: calls ---- */

/* A lambda: its tokens, read again as code wherever it is called. Its captures are whatever
   was in scope where it was written, which is still in scope where it is called. */
typedef struct Lambda {
    Token *params; /* the parameter list, through its ')' */
    u32 nparams;
    Token *body;   /* from '{' through '}' */
    u32 nbody;
    u32 level;     /* the scope it was written in */
    Type *ret;     /* the declared result type, or 0 */
    int captures;
    Hide hide;     /* what could not be seen where it was written */
    Sym *fn;       /* as a function of its own, once it was converted to a pointer */
} Lambda;

#define LAM(t) ((t)->kind == TY_STRUCT ? (Lambda *)(t)->canon->lam : (Lambda *)0)

/* A body being read in place of a call to it: where its returns go. */
static struct Inl {
    int on;
    u32 end;    /* the block after the body */
    Type *ty;   /* the result type, once known */
    u32 slot;   /* 1 + the slot results are stored in */
    u32 start;  /* the block the body began in */
    u32 nret;
    int direct; /* the one return was reached unconditionally: its value is the result as it stands */
    u32 ret_cur;
    int cx;     /* evaluating a constexpr function: branches on constants are followed, not compiled */
    Expr val;
} inl;

static int inl_on(void)
{
    return inl.on;
}

static Sym *cv_lvalue; /* the constant whose address the expression being read takes */
static Expr *call_pre; /* arguments read before the function was known, for the next call() */
static u32 call_npre;
static Sym *call_sym;  /* the function the next call() calls, when it is known by name */

/* Collects the tokens from the current one, an opening bracket, through the one that closes it. */
static Token *grab_balanced(u32 *np)
{
    Token *buf = 0, *out;
    u32 n = 0, cap = 0, depth = 0;
    for (;;) {
        int k = tok.kind;
        if (k == T_EOF)
            error_at(tok.loc, "unexpected end of file");
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            buf = (Token *)xrealloc(buf, cap * sizeof(Token));
        }
        buf[n++] = tok;
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE)
            depth++;
        else if ((k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) && --depth == 0)
            break;
        next();
    }
    next();
    out = (Token *)arena_alloc(&A, n * sizeof(Token));
    memcpy(out, buf, n * sizeof(Token));
    free(buf);
    *np = n;
    return out;
}

/* Makes *a the address of an object of type bt that a reference can be bound to: the
   object itself if *a is one, else a temporary holding its value. */
static void ref_addr(Expr *a, Type *bt)
{
    Expr lv;
    u32 slot;
    if (a->lval && !a->bitwidth) {
        if (type_is_record(a->ty) ? type_is_record(bt) && a->ty->canon == bt->canon
                                  : a->ty->kind == TY_ARRAY || a->ty->kind == TY_FUNC ||
                                        type_compatible(unqual(bt), unqual(a->ty)))
            return;
    }
    rvalue(a);
    slot = ir_new_slot(F, type_size(bt), type_align(bt));
    set_expr(&lv, unqual(bt), ir_slot_addr(F, slot, 0), 1);
    assign(&lv, a);
    set_expr(a, bt, ir_slot_addr(F, slot, 0), 1);
}

/* Reads the arguments of a call, the '(' already read, through the ')'; they are left as written. */
static u32 parse_args(Expr *av, u32 max)
{
    u32 n = 0;
    int saved_stop = launch_stop;
    launch_stop = 0;
    while (tok.kind != T_RPAREN) {
        if (n == max)
            error_at(tok.loc, "too many arguments");
        assign_expr(&av[n++]);
        if (!accept(T_COMMA))
            break;
    }
    expect(T_RPAREN);
    launch_stop = saved_stop;
    return n;
}

/* How far an argument is from a parameter's type: 0 the same, more for each kind of conversion, -1 impossible. */
static int conv_cost(Expr *a, Type *pt)
{
    Type *at = a->ty;
    int ref = 0;
    if (pt->kind == TY_REF) {
        pt = pt->base;
        if (!a->lval && !(pt->quals & Q_CONST))
            return -1; /* a temporary binds to a const reference only */
        ref = 1;
        if (pt->kind == TY_ARRAY) {
            /* a reference to an array: the lengths must agree, then as for the elements' addresses */
            if (at->kind != TY_ARRAY || at->len != pt->len)
                return -1;
            pt = type_ptr(pt->base);
        }
    }
    if (at->kind == TY_ARRAY)
        at = type_ptr(at->base);
    else if (at->kind == TY_FUNC)
        at = type_ptr(at);
    at = unqual(at);
    pt = unqual(pt);
    if (pt->kind == TY_TPARAM)
        return 0;
    if (type_is_record(pt) || type_is_record(at)) {
        if (LAM(at) && pt->kind == TY_PTR && pt->base->kind == TY_FUNC)
            return 1;
        return type_is_record(pt) && type_is_record(at) && pt->canon == at->canon ? 0 : -1;
    }
    if (pt->kind == TY_PTR) {
        if (at->kind == TY_PTR) {
            if (type_compatible(pt, at))
                return 0;
            if (type_compatible(unqual(pt->base), unqual(at->base)))
                return (at->base->quals & ~pt->base->quals) ? -1 : 1;
            if (pt->base->kind == TY_VOID)
                return 2;
            if (at->base->kind == TY_VOID)
                return 4; /* as in C; nullptr comes this way too */
            return -1;
        }
        if (type_is_int(at) && a->konst)
            return 3; /* a null pointer constant, presumably */
        return -1;
    }
    if (at->kind == TY_PTR)
        return pt->kind == TY_BOOL ? 4 : -1;
    if (!type_is_arith(pt) || !type_is_arith(at))
        return -1;
    if (ref && pt->kind != at->kind)
        return 5;
    if (pt->kind == at->kind)
        return 0;
    if (type_is_int(pt) && type_is_int(at))
        return pt->kind == TY_INT && type_size(at) < 4 ? 1 : 2;
    if (type_is_float(pt) && type_is_float(at))
        return pt->kind == TY_DOUBLE && at->kind == TY_FLOAT ? 1 : 2;
    return 3;
}

static Sym *tmpl_candidate(Sym *ts, TArgs *given, Expr *av, u32 n);

/* Picks the function of a set that the arguments fit best. given: template arguments written out, or 0. */
static Sym *overload_pick(Sym *set, TArgs *given, Expr *av, u32 n, u32 loc)
{
    Sym *s, *best = 0, *c;
    int best_cost = 0, best_t = 0;
    for (s = set; s; s = s->over) {
        int cost = 0, is_t = s->kind == SK_TEMPLATE;
        u32 k;
        Type *ft;
        c = s;
        if (is_t) {
            c = tmpl_candidate(s, given, av, n);
            if (!c)
                continue;
        } else if (given) {
            continue;
        }
        ft = c->ty;
        if (ft->kind != TY_FUNC)
            continue;
        if (n > ft->nparams && !ft->variadic && !ft->oldstyle)
            continue;
        for (k = n; k < ft->nparams && !ft->oldstyle; k++)
            if (!c->pdef || !c->pdef[k])
                break;
        if (k < ft->nparams && !ft->oldstyle)
            continue; /* too few arguments */
        for (k = 0; k < n && k < ft->nparams && cost >= 0; k++) {
            int cc = conv_cost(&av[k], ft->params[k]);
            cost = cc < 0 ? -1 : cost + cc;
        }
        if (cost < 0)
            continue;
        if (n > ft->nparams)
            cost += 10; /* through the "..." */
        if (!best || cost < best_cost || (cost == best_cost && best_t && !is_t)) {
            best = c;
            best_cost = cost;
            best_t = is_t;
        }
    }
    if (!best)
        error_at(loc, "no function '%s' takes these arguments", intern_str(set->name));
    return best;
}

static void call(Expr *e)
{
    Type *ft = e->ty, *ret;
    u32 args[128], nargs = 0, np = 0, callee, v, loc = tok.loc, ngr = 0, nfr = 0, words;
    int flags = 0, lx = opt_target == TARGET_LINUX, saved_stop = launch_stop, more;
    struct Launch lc = launch;
    Expr a, *pre = call_pre;
    u32 npre = call_npre, pi = 0;
    Sym *csym = call_sym;

    call_pre = 0;
    call_sym = 0;
    launch.on = 0;
    launch_stop = 0;
    rvalue(e);
    if (e->ty->kind != TY_PTR || e->ty->base->kind != TY_FUNC)
        error_at(loc, "called object is not a function");
    ft = e->ty->base;
    ret = ft->base;
    callee = e->v;
    if (type_is_record(ret)) {
        if (!type_complete(ret))
            error_at(loc, "function returns an incomplete type");
        /* a result that comes back in registers is stored as two doublewords */
        args[nargs++] = abi_words(ret) ? ir_slot_addr(F, ir_new_slot(F, 16, 8), 0) : new_temp(ret);
        flags = IF_SRET | x64_ret_flags(ret);
        if (opt_arch == ARCH_X64 && !abi_words(ret))
            ngr = 1; /* the address for the result is the first argument */
    }
    if (type_is_128(ret)) {
        /* both halves come back in registers, as a record of two doublewords would */
        args[nargs++] = ir_slot_addr(F, ir_new_slot(F, 16, 8), 0);
        flags = IF_SRET;
    }
    more = !pre && tok.kind != T_RPAREN;
    for (;;) {
        if (pre && pi < npre) {
            a = pre[pi++];
        } else if (more) {
            assign_expr(&a);
            more = accept(T_COMMA);
        } else if (csym && csym->pdef && !ft->oldstyle && np < ft->nparams && csym->pdef[np]) {
            /* C++: a default argument */
            replay(csym->pdef[np], csym->npdef[np] + 1);
            if (tok.kind == T_LBRACE && ft->params[np]->kind != TY_REF)
                compound_literal(&a, unqual(ft->params[np]));
            else
                assign_expr(&a);
            expect(T_SEMI);
        } else {
            break;
        }
        if (nargs >= 124)
            error_at(loc, "too many arguments");
        if (!ft->oldstyle && np < ft->nparams && ft->params[np]->kind == TY_REF) {
            ref_addr(&a, ft->params[np]->base);
            args[nargs++] = a.v;
            ngr++;
            np++;
            continue;
        }
        rvalue(&a);
        if (!ft->oldstyle && np < ft->nparams) {
            Type *pt = ft->params[np];
            if (type_is_record(pt) && !type_is_record(a.ty) && (pt->canon ? pt->canon : pt)->transparent &&
                (pt->canon ? pt->canon : pt)->fields)
                pt = (pt->canon ? pt->canon : pt)->fields->ty; /* a member's value goes as the member would */
            if (special_pair(pt, a.ty))
                convert(&a, pt);
            if (type_is_record(pt) != type_is_record(a.ty))
                error_at(loc, "incompatible type for argument %u", np + 1);
            convert(&a, pt);
        } else {
            if (!ft->oldstyle && !ft->variadic)
                error_at(loc, "too many arguments to function");
            if (type_is_int(a.ty))
                convert(&a, type_promote(a.ty));
            else if (a.ty->kind == TY_FLOAT || a.ty->kind == TY_FLOAT16 || a.ty->kind == TY_BF16)
                convert(&a, ty_double);
        }
        if (a.ty->kind == TY_VOID)
            error_at(loc, "void value used as an argument");
        words = abi_words(a.ty);
        if (opt_arch == ARCH_X64 && (type_is_record(a.ty) || type_is_128(a.ty))) {
            nargs = x64_arg(&a, args, nargs, &ngr, &nfr);
        } else if (lx && type_is_record(a.ty) && !type_size(a.ty)) {
            /* an empty record is not passed at all */
        } else if (words || type_is_128(a.ty)) {
            int big = type_is_128(a.ty);
            if (big)
                words = 2;
            if (big && (ngr & 1) && ngr < 8) {
                args[nargs++] = ir_const(F, IT_I64, 0); /* starts at an even register */
                ngr++;
            }
            if (words == 2 && ngr == 7) {
                args[nargs++] = ir_const(F, IT_I64, 0); /* never split between x7 and the stack */
                ngr++;
            }
            args[nargs++] = big ? a.v : record_word(a.v, type_size(a.ty), 0);
            if (words == 2)
                args[nargs++] = big ? a.hi : record_word(a.v, type_size(a.ty), 8);
            ngr += words;
        } else {
            if (type_is_record(a.ty)) {
                u32 tmp = new_temp(a.ty);
                emit_mem(OP_MEMCPY, tmp, a.v, type_size(a.ty), type_align(a.ty));
                a.v = tmp;
            }
            if (!type_is_float(a.ty))
                ngr++;
            else
                nfr++;
            args[nargs++] = a.v;
        }
        np++;
    }
    if (!pre)
        expect(T_RPAREN);
    launch_stop = saved_stop;
    if (!ft->oldstyle && np < ft->nparams)
        error_at(loc, "too few arguments to function");
    if (lc.on) {
        /* a kernel launch: the runtime gets the kernel's name and the address of each argument */
        u32 la[7], arr, k, nm;
        const char *kn;
        Ins *ci = &F->ins[callee];
        if (flags || ci->op != OP_ADDR || ci->b)
            error_at(loc, "a launch needs the name of a kernel that returns nothing");
        kn = O->syms[ci->a].name ? intern_str(O->syms[ci->a].name) : "";
        if (opt_target != TARGET_LINUX && kn[0] == '_')
            kn++;
        nm = string_sym((const u8 *)kn, strlen(kn) + 1);
        arr = ir_slot_addr(F, ir_new_slot(F, 8 * (nargs ? nargs : 1), 8), 0);
        for (k = 0; k < nargs; k++) {
            int it = F->ins[args[k]].ty;
            u32 cell;
            if (F->ins[args[k]].op == OP_BYVAL) {
                /* x86-64, an argument that is a block of memory: the kernel is given a copy of the block itself,
                   in whole doublewords, as it declares the parameter */
                u32 size = F->ins[args[k]].b, al = F->ins[args[k]].c, src = F->ins[args[k]].a;
                cell = ir_slot_addr(F, ir_new_slot(F, (size + 7) & ~7U, al > 8 ? al : 8), 0);
                ir_remove(F, args[k]);
                emit_mem(OP_MEMCPY, cell, src, size, al);
                ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, arr, ir_const(F, IT_I64, 8 * k)), cell);
                continue;
            }
            cell = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0);
            ir_store(F, it == IT_I32 ? M_32 : it == IT_I64 ? M_64 : it == IT_F32 ? M_F32 : M_F64, cell, args[k]);
            ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, arr, ir_const(F, IT_I64, 8 * k)), cell);
        }
        la[0] = ir_addr(F, ptx_module(O), 0);
        la[1] = ir_addr(F, nm, 0);
        la[2] = lc.grid;
        la[3] = lc.block;
        la[4] = lc.shm;
        la[5] = lc.stream;
        la[6] = arr;
        {
            u32 id = intern_cstr("__mcuda_launch");
            Sym *ls;
            names_reserve(id);
            ls = gsyms[id];
            if (!ls) {
                ls = new_sym(id, SK_VAR, type_func(ty_int, 0, 0, 0, 0, 1));
                gsyms[id] = ls;
            }
            ir_call(F, IT_I32, ir_addr(F, sym_obj(ls), 0), la, 7, 0xffffffffU, 0);
        }
        set_expr(e, ty_void, 0, 0);
        return;
    }
    /* (x86-64: a function without a prototype may be variadic too, and is called like one) */
    v = ir_call(F, flags ? IT_VOID : type_ir(ret), callee, args, nargs,
                ft->variadic ? ft->nparams + (flags ? 1 : 0) : opt_arch == ARCH_X64 && ft->oldstyle ? nargs : 0xffffffffU, flags);
    if (flags && type_is_128(ret)) {
        F->ins[v].mem = 16;
        v = ir_load(F, IT_I64, M_64, args[0]);
        set_expr(e, ret, v, 0);
        e->hi = ir_load(F, IT_I64, M_64, ir_ins(F, OP_ADD, IT_I64, args[0], ir_const(F, IT_I64, 8)));
    } else if (flags) {
        if (abi_words(ret))
            F->ins[v].mem = (u8)type_size(ret);
        set_expr(e, ret, args[0], 0);
    } else if (ret->kind == TY_REF) {
        set_expr(e, ret->base, v, 1); /* C++: the object the function returned */
    } else {
        set_expr(e, ret, abi_extend(ret, v), 0);
    }
}

static void member(Expr *e, int arrow)
{
    Field *f;
    Type *t;
    u64 off = 0;
    if (arrow) {
        rvalue(e);
        if (e->ty->kind != TY_PTR)
            error_at(tok.loc, "'->' applied to a non-pointer");
        e->ty = e->ty->base;
        e->lval = 1;
    }
    if (!type_is_record(e->ty))
        error_at(tok.loc, "member access on a non-structure");
    if (!type_complete(e->ty))
        error_at(tok.loc, "member access on an incomplete type");
    if (tok.kind != T_IDENT)
        error_at(tok.loc, "expected member name");
    f = type_member(e->ty, tok.v, &off);
    if (!f)
        error_at(tok.loc, "no member named '%s'", intern_str(tok.v));
    next();
    t = f->ty;
    if (e->ty->quals & ~Q_ATOMIC)
        t = type_qual(t, e->ty->quals & ~Q_ATOMIC);
    e->v = ir_ins(F, OP_ADD, IT_I64, e->v, ir_const(F, IT_I64, off));
    e->ty = t;
    e->lval = 1;
    e->bitoff = f->bitoff;
    e->bitwidth = f->bitwidth;
}

/* ---- atomic objects ---- */

/* Can an object of type t be read, written and updated atomically? */
static int atomic_able(Type *t)
{
    u64 n;
    if (!type_is_scalar(t) || type_is_128(t) || !type_complete(t))
        return 0;
    n = type_size(t);
    return n == 1 || n == 2 || n == 4 || n == 8;
}

/* One atomic operation on an object of type t (see IF_ATOMIC); the result is the old contents, as bits. */
static u32 atomic_call(int kind, Type *t, u32 *args, u32 nargs, int relaxed)
{
    u64 n = t ? type_size(t) : 4;
    u32 lg = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : 3;
    return ir_call(F, kind == ATOM_STORE || kind == ATOM_FENCE ? IT_VOID : lg == 3 ? IT_I64 : IT_I32,
                   ir_const(F, IT_I64, (u64)kind | lg << 8 | (relaxed ? 0x1000 : 0)), args, nargs, nargs, IF_ATOMIC);
}

/* A value of type t as the bits an atomic operation works on: zero-extended, floating values moved over. */
static u32 atomic_raw(Type *t, u32 v)
{
    int it = type_ir(t);
    if (it == IT_F32 || it == IT_F64) {
        u32 slot = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0);
        ir_store(F, it == IT_F32 ? M_F32 : M_F64, slot, v);
        return ir_load(F, it == IT_F32 ? IT_I32 : IT_I64, it == IT_F32 ? M_32 : M_64, slot);
    }
    if (type_size(t) == 1)
        return ir_ins(F, OP_ZEXT8, IT_I32, v, 0);
    if (type_size(t) == 2)
        return ir_ins(F, OP_ZEXT16, IT_I32, v, 0);
    return v;
}

/* And back. */
static u32 atomic_value(Type *t, u32 raw)
{
    int it = type_ir(t);
    if (it == IT_F32 || it == IT_F64) {
        u32 slot = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0);
        ir_store(F, it == IT_F32 ? M_32 : M_64, slot, raw);
        return ir_load(F, it, it == IT_F32 ? M_F32 : M_F64, slot);
    }
    if (type_size(t) <= 2 && !type_is_unsigned(t) && t->kind != TY_BOOL)
        return ir_ins(F, type_size(t) == 1 ? OP_SEXT8 : OP_SEXT16, IT_I32, raw, 0);
    return raw;
}

static int atomic_lvalue(Expr *e)
{
    return e->lval && (e->ty->quals & Q_ATOMIC) && !e->bitwidth && atomic_able(e->ty);
}

/* lv op= r on an atomic object, as one atomic step. *lv becomes the new value, or with post the old one. */
static void atomic_rmw(Expr *lv, int op, Expr *r, u32 loc, int post)
{
    Type *t = unqual(lv->ty);
    Expr old, nw, d;
    u32 args[3], kind = 0;
    switch (op) {
    case T_PLUS: kind = ATOM_ADD; break;
    case T_MINUS: kind = ATOM_SUB; break;
    case T_AMP: kind = ATOM_AND; break;
    case T_PIPE: kind = ATOM_OR; break;
    case T_CARET: kind = ATOM_XOR; break;
    }
    if (kind && type_is_int(r->ty) && !type_is_128(r->ty) &&
        ((type_is_int(t) && t->kind != TY_BOOL) || (t->kind == TY_PTR && kind <= ATOM_SUB))) {
        /* the processor's own exchange loop does these */
        d = *r;
        if (t->kind == TY_PTR) {
            u64 size = t->base->kind == TY_VOID || t->base->kind == TY_FUNC ? 1 : type_size(t->base);
            convert(&d, ty_long);
            d.v = ir_ins(F, OP_MUL, IT_I64, d.v, ir_const(F, IT_I64, size));
        } else {
            convert(&d, t);
        }
        args[0] = lv->v;
        args[1] = d.v;
        set_expr(&old, t, atomic_value(t, atomic_call((int)kind, t, args, 2, 0)), 0);
        if (!post) {
            if (t->kind == TY_PTR) {
                old.v = ir_ins(F, kind == ATOM_ADD ? OP_ADD : OP_SUB, IT_I64, old.v, d.v);
            } else {
                binop(op, &old, &d, loc);
                convert(&old, t);
            }
        }
        *lv = old;
        return;
    }
    /* anything else: work the new value out and put it there if the old one still is */
    {
        int rit = type_size(t) == 8 ? IT_I64 : IT_I32, rmem = rit == IT_I64 ? M_64 : M_32, live = F->cur != 0;
        u32 slot = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0), loop = ir_new_block(F), done = ir_new_block(F);
        u32 prev, oraw;
        args[0] = lv->v;
        ir_store(F, rmem, slot, atomic_call(ATOM_LOAD, t, args, 1, 0));
        if (live) {
            ir_jmp(F, loop);
            F->cur = loop;
        }
        oraw = ir_load(F, rit, rmem, slot);
        set_expr(&old, t, atomic_value(t, oraw), 0);
        nw = old;
        d = *r;
        binop(op, &nw, &d, loc);
        convert(&nw, t);
        args[0] = lv->v;
        args[1] = oraw;
        args[2] = atomic_raw(t, nw.v);
        prev = atomic_call(ATOM_CAS, t, args, 3, 0);
        ir_store(F, rmem, slot, prev);
        if (live && F->cur) {
            ir_br(F, ir_ins(F, OP_EQ, IT_I32, prev, oraw), done, loop);
            F->cur = done;
        }
        *lv = post ? old : nw;
        lv->konst = 0;
    }
}

/* Is the memory order that follows the relaxed one? (Any other is treated as the strictest.) */
static int atomic_order(void)
{
    Expr x;
    u64 c;
    assign_expr(&x);
    rvalue(&x);
    return ir_is_const(F, x.v, &c) && c == 0;
}

static int atomic_opname(const char *s, size_t n)
{
    static const char *const names[] = {"add", "sub", "and", "or", "xor", "nand"};
    int k;
    for (k = 0; k < 6; k++)
        if (strlen(names[k]) == n && strncmp(s, names[k], n) == 0)
            return ATOM_ADD + k;
    return -1;
}

/* A pointer argument of an atomic builtin; *t becomes the type of what it points to. */
static u32 atomic_ptr(Type **t)
{
    Expr a;
    assign_expr(&a);
    rvalue(&a);
    if (a.ty->kind != TY_PTR || !atomic_able(a.ty->base))
        error_at(tok.loc, "atomic operation on an object that is not a scalar of 1, 2, 4 or 8 bytes");
    *t = unqual(a.ty->base);
    return a.v;
}

/* A value argument, converted to t, as bits. */
static u32 atomic_arg(Type *t, Expr *x)
{
    assign_expr(x);
    rvalue(x);
    if (!type_is_scalar(x->ty))
        error_at(tok.loc, "scalar value required");
    convert(x, t);
    return atomic_raw(t, x->v);
}

/* The value after an update of kind kind, from the value before. */
static u32 atomic_after(int kind, Type *t, u32 old, u32 v)
{
    static const u8 ops[] = {OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_AND};
    int it = type_ir(t);
    Expr x;
    u32 r = ir_ins(F, ops[kind - ATOM_ADD], it, old, v);
    if (kind == ATOM_NAND)
        r = ir_ins(F, OP_NOT, it, r, 0);
    set_expr(&x, it == IT_I64 ? ty_ulong : ty_int, r, 0);
    convert(&x, t);
    return x.v;
}

/* The __atomic_ and __sync_ builtins; the current token is their name, nm. */
static int atomic_builtin(Expr *e, const char *nm)
{
    int sync = nm[2] == 's', kind = -1, after = 0, relaxed = 0;
    const char *b = nm + (sync ? 7 : 9);
    size_t n = strlen(b);
    Type *t;
    Expr x, y;
    u32 args[3], raw;

    if (strncmp(nm, "__atomic_", 9) != 0 && strncmp(nm, "__sync_", 7) != 0)
        return 0;
    if (sync && n > 10 && strncmp(b, "fetch_and_", 10) == 0) {
        kind = atomic_opname(b + 10, n - 10);
    } else if (sync && n > 10 && strcmp(b + n - 10, "_and_fetch") == 0) {
        kind = atomic_opname(b, n - 10);
        after = 1;
    } else if (!sync && n > 6 && strncmp(b, "fetch_", 6) == 0) {
        kind = atomic_opname(b + 6, n - 6);
    } else if (!sync && n > 6 && strcmp(b + n - 6, "_fetch") == 0) {
        kind = atomic_opname(b, n - 6);
        after = 1;
    }
    if (kind >= 0) {
        next();
        expect(T_LPAREN);
        args[0] = atomic_ptr(&t);
        expect(T_COMMA);
        args[1] = atomic_arg(t, &x);
        if (!sync) {
            expect(T_COMMA);
            relaxed = atomic_order();
        }
        raw = atomic_value(t, atomic_call(kind, t, args, 2, relaxed));
        set_expr(e, t, after ? atomic_after(kind, t, raw, x.v) : raw, 0);
    } else if (sync ? strcmp(b, "synchronize") == 0 : strcmp(b, "thread_fence") == 0 || strcmp(b, "signal_fence") == 0) {
        next();
        expect(T_LPAREN);
        if (!sync)
            atomic_order();
        if (b[0] != 's' || sync)
            atomic_call(ATOM_FENCE, 0, args, 0, 0);
        set_expr(e, ty_void, 0, 0);
    } else if (!sync && (strcmp(b, "always_lock_free") == 0 || strcmp(b, "is_lock_free") == 0)) {
        next();
        expect(T_LPAREN);
        dead_expr(&x);
        expect(T_COMMA);
        dead_expr(&x);
        set_int(e, ty_int, 1);
    } else if (!sync && (strcmp(b, "load_n") == 0 || strcmp(b, "load") == 0)) {
        next();
        expect(T_LPAREN);
        args[0] = atomic_ptr(&t);
        expect(T_COMMA);
        if (b[4]) {
            relaxed = atomic_order();
            set_expr(e, t, atomic_value(t, atomic_call(ATOM_LOAD, t, args, 1, relaxed)), 0);
        } else {
            assign_expr(&y);
            rvalue(&y);
            expect(T_COMMA);
            relaxed = atomic_order();
            ir_store(F, type_mem(t), y.v, atomic_value(t, atomic_call(ATOM_LOAD, t, args, 1, relaxed)));
            set_expr(e, ty_void, 0, 0);
        }
    } else if (sync ? strcmp(b, "lock_release") == 0 : strcmp(b, "store_n") == 0 || strcmp(b, "store") == 0 ||
                                                         strcmp(b, "clear") == 0) {
        next();
        expect(T_LPAREN);
        args[0] = atomic_ptr(&t);
        if (sync || b[0] == 'c') {
            args[1] = ir_const(F, type_size(t) == 8 ? IT_I64 : IT_I32, 0);
        } else {
            expect(T_COMMA);
            if (b[5]) {
                args[1] = atomic_arg(t, &x);
            } else {
                assign_expr(&y);
                rvalue(&y);
                args[1] = atomic_raw(t, ir_load(F, type_ir(t), type_mem(t), y.v));
            }
        }
        if (!sync) {
            expect(T_COMMA);
            relaxed = atomic_order();
        }
        atomic_call(ATOM_STORE, t, args, 2, relaxed);
        set_expr(e, ty_void, 0, 0);
    } else if (sync ? strcmp(b, "lock_test_and_set") == 0 : strcmp(b, "exchange_n") == 0 || strcmp(b, "exchange") == 0 ||
                                                              strcmp(b, "test_and_set") == 0) {
        int generic = !sync && b[0] == 'e' && !b[8];
        next();
        expect(T_LPAREN);
        args[0] = atomic_ptr(&t);
        if (!sync && b[0] == 't') {
            args[1] = ir_const(F, type_size(t) == 8 ? IT_I64 : IT_I32, 1);
        } else {
            expect(T_COMMA);
            if (generic) {
                assign_expr(&y);
                rvalue(&y);
                args[1] = atomic_raw(t, ir_load(F, type_ir(t), type_mem(t), y.v));
                expect(T_COMMA);
                assign_expr(&y);
                rvalue(&y);
            } else {
                args[1] = atomic_arg(t, &x);
            }
        }
        if (!sync) {
            expect(T_COMMA);
            relaxed = atomic_order();
        }
        raw = atomic_value(t, atomic_call(ATOM_XCHG, t, args, 2, relaxed));
        if (generic) {
            ir_store(F, type_mem(t), y.v, raw);
            set_expr(e, ty_void, 0, 0);
        } else if (!sync && b[0] == 't') {
            set_expr(e, ty_bool, ir_ins(F, OP_NE, IT_I32, raw, ir_const(F, type_ir(t), 0)), 0);
        } else {
            set_expr(e, t, raw, 0);
        }
    } else if (sync ? strcmp(b, "bool_compare_and_swap") == 0 || strcmp(b, "val_compare_and_swap") == 0
                    : strcmp(b, "compare_exchange_n") == 0 || strcmp(b, "compare_exchange") == 0) {
        u32 ok, expected = 0;
        next();
        expect(T_LPAREN);
        args[0] = atomic_ptr(&t);
        expect(T_COMMA);
        if (sync) {
            args[1] = atomic_arg(t, &x);
        } else {
            assign_expr(&y);
            rvalue(&y);
            if (y.ty->kind != TY_PTR)
                error_at(tok.loc, "pointer to the expected value required");
            expected = y.v;
            args[1] = atomic_raw(t, ir_load(F, type_ir(t), type_mem(t), expected));
        }
        expect(T_COMMA);
        if (!sync && !b[16]) {
            assign_expr(&y);
            rvalue(&y);
            args[2] = atomic_raw(t, ir_load(F, type_ir(t), type_mem(t), y.v));
        } else {
            args[2] = atomic_arg(t, &x);
        }
        if (!sync) {
            expect(T_COMMA);
            dead_expr(&x); /* weak: the strong form serves */
            expect(T_COMMA);
            relaxed = atomic_order();
            expect(T_COMMA);
            atomic_order();
        }
        raw = atomic_call(ATOM_CAS, t, args, 3, relaxed);
        ok = ir_ins(F, OP_EQ, IT_I32, raw, args[1]);
        if (!sync && F->cur) {
            /* the value found goes back to the caller when it was not the expected one */
            u32 fail = ir_new_block(F), done = ir_new_block(F);
            ir_br(F, ok, done, fail);
            F->cur = fail;
            ir_store(F, type_mem(t), expected, atomic_value(t, raw));
            ir_jmp(F, done);
            F->cur = done;
        }
        if (sync && b[0] == 'v')
            set_expr(e, t, atomic_value(t, raw), 0);
        else
            set_expr(e, ty_bool, ok, 0);
    } else {
        return 0;
    }
    while (accept(T_COMMA))
        dead_expr(&x); /* the __sync_ forms take a list of variables to no purpose */
    expect(T_RPAREN);
    return 1;
}

static void incdec(Expr *e, int op, int post)
{
    Expr lv, old, one;
    need_lvalue(e);
    if (atomic_lvalue(e)) {
        set_int(&one, ty_int, 1);
        atomic_rmw(e, op, &one, tok.loc, post);
        return;
    }
    lv = *e;
    rvalue(e);
    old = *e;
    if (!type_is_scalar(e->ty))
        error_at(tok.loc, "invalid operand to increment");
    set_int(&one, ty_int, 1);
    binop(op, e, &one, tok.loc);
    assign(&lv, e);
    *e = post ? old : lv;
}

/* Parses an expression that is not to be evaluated. */
static void dead_expr(Expr *e)
{
    u32 saved = F->cur;
    F->cur = 0;
    assign_expr(e);
    F->cur = saved;
}

/* ({ ... }): the current token is the brace. */
static void stmt_expr(Expr *e)
{
    Expr saved_val = sv_val;
    int saved_has = sv_has;
    next();
    push_scope();
    sv_has = 0;
    while (tok.kind != T_RBRACE) {
        if (tok.kind == T_EOF)
            error_at(tok.loc, "expected '}'");
        sv_has = 0;
        sv_pending = 1;
        block_item();
        sv_pending = 0;
    }
    next();
    pop_scope();
    if (sv_has) {
        *e = sv_val;
        e->konst = 0;
    } else
        set_expr(e, ty_void, 0, 0);
    sv_val = saved_val;
    sv_has = saved_has;
}

/* Could v become a constant when the function is inlined? Only if it is
   computed from constants and local variables alone. */
static int pure_tree(u32 v, int depth)
{
    Ins *i = &F->ins[v];
    u32 *ops, n, k;
    if (depth > 48)
        return 0;
    switch (i->op) {
    case OP_CONST: case OP_FCONST: case OP_SLOT: case OP_ADDR:
        return 1;
    case OP_LOAD:
        return F->ins[i->a].op == OP_SLOT;
    }
    if (i->op < OP_ADD || i->op > OP_POPCNT)
        return 0;
    IR_OPS(F, i, ops, n);
    for (k = 0; k < n; k++)
        if (!pure_tree(ops[k], depth + 1))
            return 0;
    return 1;
}

static Sym *implicit_func(u32 name, Type *ret)
{
    Sym *s;
    names_reserve(name);
    s = gsyms[name];
    if (!s) {
        s = new_sym(name, SK_VAR, type_func(ret, 0, 0, 0, 0, 1));
        gsyms[name] = s;
    }
    return s;
}

/* Handles the current identifier if it names a builtin: returns 1 with the
   result in *e, or 0 (possibly after renaming the token to the library
   function the builtin stands for). */
static int builtin(Expr *e)
{
    const char *nm = intern_str(tok.v), *b;
    u32 loc = tok.loc;
    Expr a, x;
    if (nm[0] != '_' || nm[1] != '_')
        return 0;
    if (nm[2] == 'a' || nm[2] == 's')
        return atomic_builtin(e, nm);
    if (strcmp(nm, "__func__") == 0 || strcmp(nm, "__FUNCTION__") == 0 || strcmp(nm, "__PRETTY_FUNCTION__") == 0) {
        size_t len = strlen(cur_fn_name) + 1;
        next();
        set_expr(e, type_array(ty_char, len, 1), ir_addr(F, string_sym((const u8 *)cur_fn_name, len), 0), 1);
        return 1;
    }
    if (strncmp(nm, "__builtin_", 10) != 0)
        return 0;
    b = nm + 10;
    if (strcmp(b, "va_start") == 0 || strcmp(b, "va_arg") == 0 || strcmp(b, "va_end") == 0)
        return 0;
    if (strcmp(b, "expect") == 0 || strcmp(b, "expect_with_probability") == 0 || strcmp(b, "assume_aligned") == 0) {
        int keep = b[0] == 'a';
        next();
        expect(T_LPAREN);
        assign_expr(e);
        rvalue(e);
        if (!keep)
            convert(e, ty_long);
        while (accept(T_COMMA))
            dead_expr(&x);
        expect(T_RPAREN);
        return 1;
    }
    if (strcmp(b, "mcuda_device") == 0) {
        /* CUDA: is the code being made device code? */
        u32 id = intern_cstr("__mcuda_is_device");
        Sym *ls;
        next();
        expect(T_LPAREN);
        expect(T_RPAREN);
        if (!cur_fn_dev) {
            set_expr(e, ty_int, ir_const(F, IT_I32, 0), 0);
            return 1;
        }
        /* the function may be made for both sides, or put into a host function: each back end answers */
        names_reserve(id);
        ls = gsyms[id];
        if (!ls) {
            ls = new_sym(id, SK_VAR, type_func(ty_int, 0, 0, 0, 0, 0));
            gsyms[id] = ls;
        }
        O->syms[sym_obj(ls)].isdev = 1;
        set_expr(e, ty_int, ir_call(F, IT_I32, ir_addr(F, sym_obj(ls), 0), 0, 0, 0xffffffffU, 0), 0);
        return 1;
    }
    if (strcmp(b, "complex") == 0) {
        Expr im;
        Type *et;
        next();
        expect(T_LPAREN);
        assign_expr(e);
        rvalue(e);
        expect(T_COMMA);
        assign_expr(&im);
        rvalue(&im);
        expect(T_RPAREN);
        et = type_usual(e->ty, im.ty);
        if (!type_is_float(et) || et->kind <= TY_FLOAT16)
            et = ty_double;
        convert(e, et);
        convert(&im, et);
        cx_make(e, type_vector(et, 2, 2), e->v, im.v);
        return 1;
    }
    if (strcmp(b, "convertvector") == 0) {
        Type *vt;
        u32 n, i, tmp;
        next();
        expect(T_LPAREN);
        assign_expr(e);
        rvalue(e);
        expect(T_COMMA);
        vt = type_name();
        expect(T_RPAREN);
        if (VK(e->ty) != 1 || VK(vt) != 1 || e->ty->canon->len != vt->canon->len)
            error_at(loc, "invalid vector conversion");
        vt = vt->canon;
        n = (u32)vt->len;
        tmp = new_temp(vt);
        for (i = 0; i < n; i++) {
            vec_elem(e, i, &a);
            rvalue(&a);
            convert(&a, vt->base);
            vec_put(tmp, vt->base, i, a.v);
        }
        set_expr(e, vt, tmp, 0);
        return 1;
    }
    if (strcmp(b, "shufflevector") == 0) {
        Expr v2;
        Type *et, *rt;
        i64 idx[64];
        u32 n = 0, i, tmp, len;
        next();
        expect(T_LPAREN);
        assign_expr(e);
        rvalue(e);
        expect(T_COMMA);
        assign_expr(&v2);
        rvalue(&v2);
        if (VK(e->ty) != 1 || VK(v2.ty) != 1 || e->ty->canon != v2.ty->canon)
            error_at(loc, "invalid operands to __builtin_shufflevector");
        while (accept(T_COMMA)) {
            if (n >= 64)
                error_at(loc, "too many vector elements");
            idx[n++] = const_int_expr();
        }
        expect(T_RPAREN);
        et = e->ty->canon->base;
        len = (u32)e->ty->canon->len;
        rt = type_vector(et, n, 1);
        tmp = new_temp(rt);
        for (i = 0; i < n; i++) {
            if (idx[i] < 0) {
                set_expr(&a, et, type_is_float(et) ? ir_fconst(F, type_ir(et), 0.0) : ir_const(F, type_ir(et), 0), 0);
            } else {
                if (idx[i] >= 2 * (i64)len)
                    error_at(loc, "vector index out of range");
                vec_elem(idx[i] < (i64)len ? e : &v2, (u32)(idx[i] % len), &a);
                rvalue(&a);
            }
            vec_put(tmp, et, i, a.v);
        }
        set_expr(e, rt, tmp, 0);
        return 1;
    }
    if (strcmp(b, "constant_p") == 0) {
        Token *t;
        u32 n;
        u64 c;
        next();
        expect(T_LPAREN);
        n = skim(&t);
        replay(t, n);
        dead_expr(&a);
        {
            u32 saved = F->cur;
            F->cur = 0;
            rvalue(&a);
            F->cur = saved;
        }
        if (F->ins[a.v].op == OP_CONST || F->ins[a.v].op == OP_FCONST || !type_is_scalar(a.ty)) {
            c = type_is_scalar(a.ty);
            set_int(e, ty_int, c);
        } else if (F == const_func || !F->cur || !pure_tree(a.v, 0)) {
            set_int(e, ty_int, 0);
        } else {
            /* decided after inlining: evaluate it for real (it has no side effects) */
            replay(t, n);
            assign_expr(&a);
            rvalue(&a);
            set_expr(e, ty_int, ir_ins(F, OP_ISCONST, IT_I32, a.v, 0), 0);
        }
        expect(T_RPAREN);
        return 1;
    }
    if (strcmp(b, "choose_expr") == 0) {
        i64 c;
        next();
        expect(T_LPAREN);
        c = const_int_expr();
        expect(T_COMMA);
        if (c)
            assign_expr(e);
        else
            dead_expr(&x);
        expect(T_COMMA);
        if (c)
            dead_expr(&x);
        else
            assign_expr(e);
        expect(T_RPAREN);
        return 1;
    }
    if (strcmp(b, "types_compatible_p") == 0) {
        Type *t1, *t2;
        next();
        expect(T_LPAREN);
        t1 = type_name();
        expect(T_COMMA);
        t2 = type_name();
        expect(T_RPAREN);
        set_int(e, ty_int, (u64)type_compatible(unqual(t1), unqual(t2)));
        return 1;
    }
    if (strcmp(b, "offsetof") == 0) {
        Type *t;
        u32 v;
        next();
        expect(T_LPAREN);
        t = type_name();
        expect(T_COMMA);
        v = ir_const(F, IT_I64, 0);
        for (;;) {
            Field *f;
            u64 off = 0;
            if (tok.kind != T_IDENT || !type_is_record(t) || !type_complete(t))
                error_at(tok.loc, "invalid member designator");
            f = type_member(t, tok.v, &off);
            if (!f)
                error_at(tok.loc, "no member named '%s'", intern_str(tok.v));
            next();
            v = ir_ins(F, OP_ADD, IT_I64, v, ir_const(F, IT_I64, off));
            t = f->ty;
            while (accept(T_LBRACK)) {
                if (t->kind != TY_ARRAY)
                    error_at(tok.loc, "subscript of a member that is not an array");
                expr(&x);
                rvalue(&x);
                convert(&x, ty_long);
                expect(T_RBRACK);
                t = t->base;
                v = ir_ins(F, OP_ADD, IT_I64, v, ir_ins(F, OP_MUL, IT_I64, x.v, ir_const(F, IT_I64, type_size(t))));
            }
            if (!accept(T_DOT))
                break;
        }
        expect(T_RPAREN);
        set_expr(e, ty_ulong, v, 0);
        return 1;
    }
    if (strcmp(b, "object_size") == 0 || strcmp(b, "dynamic_object_size") == 0) {
        i64 c;
        next();
        expect(T_LPAREN);
        dead_expr(&x);
        expect(T_COMMA);
        c = const_int_expr();
        expect(T_RPAREN);
        set_int(e, ty_ulong, c & 2 ? 0 : ~(u64)0);
        return 1;
    }
    if (strcmp(b, "frame_address") == 0 || strcmp(b, "return_address") == 0) {
        u32 v;
        next();
        expect(T_LPAREN);
        if (const_int_expr() != 0)
            error_at(loc, "only the current frame can be asked for");
        expect(T_RPAREN);
        v = ir_raw(F, OP_FRAMEADDR, IT_I64, b[0] == 'r', 0);
        if (F->cur)
            ir_append(F, F->cur, v);
        set_expr(e, type_ptr(ty_void), v, 0);
        return 1;
    }
    if (strcmp(b, "unreachable") == 0 || strcmp(b, "prefetch") == 0) {
        next();
        expect(T_LPAREN);
        while (tok.kind != T_RPAREN) {
            dead_expr(&x);
            if (!accept(T_COMMA))
                break;
        }
        expect(T_RPAREN);
        set_expr(e, ty_void, 0, 0);
        return 1;
    }
    {
        /* clz ctz ffs popcount bswap, with l/ll or a width for a suffix */
        static const char *const bits[] = {"clz", "ctz", "ffs", "popcount", "bswap", 0};
        int k;
        for (k = 0; bits[k]; k++) {
            size_t n = strlen(bits[k]);
            const char *suf = b + n;
            int wide, op, it, generic = 0, narrow = 0, has_zero = 0;
            Type *at;
            Expr zv;
            u32 v;
            if (strncmp(b, bits[k], n) != 0)
                continue;
            if (k == 4) {
                if (strcmp(suf, "16") && strcmp(suf, "32") && strcmp(suf, "64"))
                    continue;
                wide = suf[0] == '6';
            } else if (strcmp(suf, "g") == 0 && k != 2) {
                generic = 1; /* C23's stdbit.h is built on these: the operand's own type decides the width */
                wide = 0;
            } else {
                if (suf[0] && strcmp(suf, "l") && strcmp(suf, "ll"))
                    continue;
                wide = suf[0] == 'l';
            }
            next();
            expect(T_LPAREN);
            assign_expr(&a);
            rvalue(&a);
            if (generic) {
                if (!type_is_int(a.ty) || type_is_128(a.ty))
                    error_at(loc, "the operand must be an integer of at most 64 bits");
                wide = type_size(a.ty) == 8;
                narrow = type_size(a.ty) < 4 ? 32 - (int)type_size(a.ty) * 8 : 0;
                if (narrow)
                    convert(&a, type_size(a.ty) == 1 ? ty_uchar : ty_ushort); /* zeros above */
                if (k != 3 && accept(T_COMMA)) {
                    /* the result for 0 */
                    assign_expr(&zv);
                    rvalue(&zv);
                    convert(&zv, ty_int);
                    has_zero = 1;
                }
            }
            expect(T_RPAREN);
            at = wide ? ty_ulong : ty_uint;
            it = wide ? IT_I64 : IT_I32;
            convert(&a, at);
            op = k == 0 ? OP_CLZ : k == 3 ? OP_POPCNT : k == 4 ? OP_BSWAP : OP_CTZ;
            if (k == 3 && F->ins[a.v].op != OP_CONST) {
                /* no instruction counts bits in a general register */
                u64 m1 = 0x5555555555555555UL, m2 = 0x3333333333333333UL, m4 = 0x0f0f0f0f0f0f0f0fUL;
                u32 x = a.v;
#define C(c) ir_const(F, it, wide ? (c) : (u64)(u32)(c))
                x = ir_ins(F, OP_SUB, it, x, ir_ins(F, OP_AND, it, ir_ins(F, OP_SHR, it, x, C(1)), C(m1)));
                x = ir_ins(F, OP_ADD, it, ir_ins(F, OP_AND, it, x, C(m2)),
                           ir_ins(F, OP_AND, it, ir_ins(F, OP_SHR, it, x, C(2)), C(m2)));
                x = ir_ins(F, OP_AND, it, ir_ins(F, OP_ADD, it, x, ir_ins(F, OP_SHR, it, x, C(4))), C(m4));
                x = ir_ins(F, OP_SHR, it, ir_ins(F, OP_MUL, it, x, C(0x0101010101010101UL)), C(wide ? 56 : 24));
#undef C
                v = x;
            } else
                v = ir_ins(F, op, it, a.v, 0);
            if (k == 2) {
                /* ffs: one more than ctz, and 0 for 0 */
                u32 sel = ir_raw(F, OP_SELECT, it, ir_ins(F, OP_NE, IT_I32, a.v, ir_const(F, it, 0)),
                                 ir_ins(F, OP_ADD, it, v, ir_const(F, it, 1)));
                u64 c;
                F->ins[sel].c = ir_const(F, it, 0);
                if (ir_is_const(F, a.v, &c))
                    v = c ? F->ins[sel].b : F->ins[sel].c;
                else {
                    if (F->cur)
                        ir_append(F, F->cur, sel);
                    v = sel;
                }
            }
            if (narrow && k == 0)
                v = ir_ins(F, OP_SUB, it, v, ir_const(F, it, (u64)narrow));
            if (has_zero) {
                u32 slot, tb = ir_new_block(F), eb = ir_new_block(F), end = ir_new_block(F);
                if (wide)
                    v = ir_ins(F, OP_TRUNC, IT_I32, v, 0);
                slot = new_temp(ty_int);
                ir_br(F, ir_ins(F, OP_NE, IT_I32, a.v, ir_const(F, it, 0)), tb, eb);
                F->cur = tb;
                store_val(ty_int, slot, v);
                ir_jmp(F, end);
                F->cur = eb;
                store_val(ty_int, slot, zv.v);
                ir_jmp(F, end);
                F->cur = end;
                set_expr(e, ty_int, load_val(ty_int, slot), 0);
                return 1;
            }
            set_expr(e, at, v, 0);
            e->konst = a.konst;
            if (k == 4) {
                if (suf[0] == '1') {
                    e->v = ir_ins(F, OP_SHR, IT_I32, e->v, ir_const(F, IT_I32, 16));
                    e->ty = ty_ushort;
                }
            } else {
                convert(e, ty_int);
            }
            return 1;
        }
    }
    if (strcmp(b, "nan") == 0 || strcmp(b, "nanf") == 0 || strcmp(b, "inf") == 0 || strcmp(b, "inff") == 0 ||
        strcmp(b, "huge_val") == 0 || strcmp(b, "huge_valf") == 0) {
        /* values no constant in the language's own notation gives */
        double zero = 0, big = 1e300, val;
        Type *ft = b[strlen(b) - 1] == 'f' && strcmp(b, "inf") != 0 ? ty_float : ty_double;
        val = b[0] == 'n' ? zero / zero : big * big;
        if (b[0] == 'n' && val < 0)
            val = -val; /* the quiet one with the sign clear */
        next();
        expect(T_LPAREN);
        while (tok.kind == T_STR)
            next();
        expect(T_RPAREN);
        set_expr(e, ft, ir_fconst(F, type_ir(ft), val), 0);
        return 1;
    }
    if (strcmp(b, "extract_return_addr") == 0 || strcmp(b, "frob_return_addr") == 0) {
        next();
        expect(T_LPAREN);
        assign_expr(e);
        rvalue(e);
        expect(T_RPAREN);
        return 1;
    }
    if (strcmp(b, "add_overflow") == 0 || strcmp(b, "sub_overflow") == 0 || strcmp(b, "mul_overflow") == 0) {
        /* The exact result as a 128-bit number (hi, lo): each operand is
           widened with its own sign, and the sum, difference or product of
           two such numbers always fits. It overflows the type it is stored
           in if truncating it there changes its value. */
        Expr r;
        Type *t;
        u32 al, bl, ah, bh, lo, hi, zero = ir_const(F, IT_I64, 0), s, ov, bits;
        int op = b[0];
        next();
        expect(T_LPAREN);
        assign_expr(&a);
        rvalue(&a);
        expect(T_COMMA);
        assign_expr(&x);
        rvalue(&x);
        expect(T_COMMA);
        assign_expr(&r);
        rvalue(&r);
        expect(T_RPAREN);
        if (!type_is_int(a.ty) || !type_is_int(x.ty) || type_is_128(a.ty) || type_is_128(x.ty) ||
            r.ty->kind != TY_PTR || !type_is_int(r.ty->base) || type_is_128(r.ty->base))
            error_at(loc, "unsupported operands of an overflow builtin");
        t = r.ty->base;
        al = type_ir(a.ty) == IT_I64 ? a.v : ir_ins(F, type_is_unsigned(a.ty) ? OP_ZEXT32 : OP_SEXT32, IT_I64, a.v, 0);
        bl = type_ir(x.ty) == IT_I64 ? x.v : ir_ins(F, type_is_unsigned(x.ty) ? OP_ZEXT32 : OP_SEXT32, IT_I64, x.v, 0);
        ah = type_is_unsigned(a.ty) ? zero : ir_ins(F, OP_SAR, IT_I64, al, ir_const(F, IT_I64, 63));
        bh = type_is_unsigned(x.ty) ? zero : ir_ins(F, OP_SAR, IT_I64, bl, ir_const(F, IT_I64, 63));
        if (op == 'a') {
            lo = ir_ins(F, OP_ADD, IT_I64, al, bl);
            hi = ir_ins(F, OP_ADD, IT_I64, ir_ins(F, OP_ADD, IT_I64, ah, bh),
                        ir_ins(F, OP_ZEXT32, IT_I64, ir_ins(F, OP_ULT, IT_I32, lo, al), 0));
        } else if (op == 's') {
            lo = ir_ins(F, OP_SUB, IT_I64, al, bl);
            hi = ir_ins(F, OP_SUB, IT_I64, ir_ins(F, OP_SUB, IT_I64, ah, bh),
                        ir_ins(F, OP_ZEXT32, IT_I64, ir_ins(F, OP_ULT, IT_I32, al, bl), 0));
        } else {
            lo = ir_ins(F, OP_MUL, IT_I64, al, bl);
            hi = ir_ins(F, OP_UMULH, IT_I64, al, bl);
            hi = ir_ins(F, OP_SUB, IT_I64, hi, ir_ins(F, OP_AND, IT_I64, ah, bl));
            hi = ir_ins(F, OP_SUB, IT_I64, hi, ir_ins(F, OP_AND, IT_I64, bh, al));
        }
        bits = (u32)type_size(t) * 8;
        if (t->kind == TY_BOOL)
            bits = 1;
        s = type_ir(t) == IT_I64 ? lo : ir_ins(F, OP_TRUNC, IT_I32, lo, 0);
        if (t->kind == TY_BOOL)
            s = ir_ins(F, OP_AND, IT_I32, s, ir_const(F, IT_I32, 1));
        ir_store(F, type_mem(t), r.v, s);
        if (type_is_unsigned(t) || t->kind == TY_BOOL) {
            ov = ir_ins(F, OP_NE, IT_I32, hi, zero);
            if (bits < 64)
                ov = ir_ins(F, OP_OR, IT_I32, ov,
                            ir_ins(F, OP_NE, IT_I32, ir_ins(F, OP_SHR, IT_I64, lo, ir_const(F, IT_I64, bits)), zero));
        } else {
            s = lo;
            if (bits < 64) {
                s = ir_ins(F, OP_SHL, IT_I64, lo, ir_const(F, IT_I64, 64 - bits));
                s = ir_ins(F, OP_SAR, IT_I64, s, ir_const(F, IT_I64, 64 - bits));
            }
            ov = ir_ins(F, OP_OR, IT_I32, ir_ins(F, OP_NE, IT_I32, lo, s),
                        ir_ins(F, OP_NE, IT_I32, hi, ir_ins(F, OP_SAR, IT_I64, s, ir_const(F, IT_I64, 63))));
        }
        set_expr(e, ty_int, ov, 0);
        return 1;
    }
    if (strcmp(b, "va_copy") == 0) {
        next();
        expect(T_LPAREN);
        assign_expr(&a);
        if (opt_arch != ARCH_X64)
            need_lvalue(&a);
        expect(T_COMMA);
        assign_expr(&x);
        rvalue(&x);
        expect(T_RPAREN);
        if (opt_arch == ARCH_X64) {
            rvalue(&a); /* both are the addresses of records of 24 bytes */
            emit_mem(OP_MEMCPY, a.v, x.v, 24, 8);
        } else if (type_is_record(x.ty))
            emit_mem(OP_MEMCPY, a.v, x.v, type_size(x.ty), type_align(x.ty));
        else
            ir_store(F, M_64, a.v, x.v);
        set_expr(e, ty_void, 0, 0);
        return 1;
    }
    if (strcmp(b, "alloca") == 0 || strcmp(b, "alloca_uninitialized") == 0) {
        u32 arg;
        next();
        expect(T_LPAREN);
        assign_expr(&x);
        rvalue(&x);
        convert(&x, ty_ulong);
        expect(T_RPAREN);
        arg = x.v;
        set_expr(e, type_ptr(ty_void), F->cur ? ir_call(F, IT_I64, ir_const(F, IT_I64, 0), &arg, 1, 1, IF_ALLOCA) : arg, 0);
        return 1;
    }
    {
        /* the rest are functions: library ones under another name, or
           ones the code generator has to supply */
        static const char *const lib[] = {"memcpy", "memmove", "memset", "memchr", "strcpy", "strncpy", "strcat",
                                          "strncat", "strchr", "strrchr", "strstr", "malloc", 0};
        static const char *const libint[] = {"memcmp", "strcmp", "strncmp", "abs", "printf", "sprintf", "snprintf",
                                             "vsnprintf", "puts", "putchar", "abort", "free", "exit", "bcmp", 0};
        int k;
        for (k = 0; lib[k]; k++)
            if (strcmp(b, lib[k]) == 0) {
                tok.v = intern_cstr(b);
                if (!lookup(tok.v))
                    implicit_func(tok.v, type_ptr(ty_void));
                return 0;
            }
        for (k = 0; libint[k]; k++)
            if (strcmp(b, libint[k]) == 0) {
                tok.v = intern_cstr(b);
                return 0;
            }
        if (strcmp(b, "strlen") == 0) {
            tok.v = intern_cstr(b);
            if (!lookup(tok.v))
                implicit_func(tok.v, ty_ulong);
            return 0;
        }
        {
            /* the mathematical library under its builtin names: sqrt, sqrtf and the like */
            static const char *const m1[] = {"rint", "nearbyint", "round", "trunc", "floor", "ceil", "sqrt", "fabs", "sin",
                                             "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "exp", "exp2",
                                             "expm1", "log", "log2", "log10", "log1p", "cbrt", "erf", "erfc", "lgamma",
                                             "tgamma", "logb", 0};
            static const char *const m2[] = {"pow", "fmod", "fmin", "fmax", "copysign", "atan2", "hypot", "remainder", "fdim",
                                             "nextafter", 0};
            size_t bl = strlen(b);
            int isf = bl > 1 && b[bl - 1] == 'f', np = 0;
            for (k = 0; m1[k] && !np; k++)
                if (strlen(m1[k]) == bl - (size_t)isf && strncmp(b, m1[k], bl - (size_t)isf) == 0)
                    np = 1;
            for (k = 0; m2[k] && !np; k++)
                if (strlen(m2[k]) == bl - (size_t)isf && strncmp(b, m2[k], bl - (size_t)isf) == 0)
                    np = 2;
            if (strcmp(b, "fma") == 0 || strcmp(b, "fmaf") == 0)
                np = 3;
            if (np) {
                u32 name = intern_cstr(b);
                tok.v = name;
                if (!lookup(name)) {
                    Type *ft = isf ? ty_float : ty_double;
                    Type **ps = (Type **)arena_alloc(&A, 3 * sizeof(Type *));
                    Sym *fs = implicit_func(name, ft);
                    ps[0] = ps[1] = ps[2] = ft;
                    if (fs->ty->oldstyle)
                        fs->ty = type_func(ft, ps, 0, (u32)np, 0, 0);
                }
                return 0;
            }
        }
        if (opt_cxx)
            error_at(tok.loc, "'%s' is not declared", intern_str(tok.v));
        implicit_func(tok.v, ty_int);
    }
    return 0;
}

/* ---- C++: lambdas, bodies read in place of a call, templates ---- */

static void postfix_ops(Expr *e);
static void sym_attrs(Sym *s, const Attr *a, int is_inline, int storage);
static int ds_auto, ds_constexpr;
static Sym *cxx_member;
static Type *inst_rec;
static u32 inst_rec_name;
static Type *cxx_named_type(void);
static Token **last_pdef;
static u32 *last_npdef;

/* Work that waits for the end of the declaration being read: functions to generate from templates and lambdas. */
typedef struct Pend {
    Tmpl *t;
    TInst *ti;
    Lambda *lam;
    Sym *fn;
    Token *raw; /* or: a declaration made up here */
    u32 nraw;
    Token *mbody; /* or: the body of fn, a member function of a class (an instance ti of t, if t) */
    u32 nmbody;
    struct Pend *next;
} Pend;

static Pend *pend_head, **pend_tail = &pend_head;

static Tmpl *cinst_t; /* the class template whose instance cinst_ti is being read */
static TInst *cinst_ti;

static Pend *pend_add(Tmpl *t, TInst *ti, Lambda *lam, Sym *fn)
{
    Pend *p = (Pend *)arena_alloc(&A, sizeof(Pend));
    p->t = t;
    p->ti = ti;
    p->lam = lam;
    p->fn = fn;
    *pend_tail = p;
    pend_tail = &p->next;
    return p;
}

/* Brings the members of a class into the current scope, as its member functions see them. */
static void cs_bind(void *cls)
{
    CScope *c = cs_get(cls, 0);
    Sym *s, *q;
    for (s = c ? c->syms : 0; s; s = s->cs_next) {
        for (q = c->syms; q != s && q->name != s->name; q = q->cs_next)
            ;
        if (q == s) {
            /* a copy, since the member may be in scope already */
            Sym *c2 = (Sym *)arena_alloc(&A, sizeof(Sym));
            *c2 = *s;
            bind(c2);
        }
    }
}

/* A copy of n tokens with a ';' after them, so that reading them again stops where they end. */
static Token *with_semi(const Token *t, u32 n)
{
    Token *out = (Token *)arena_alloc(&A, (n + 1) * sizeof(Token));
    if (n)
        memcpy(out, t, n * sizeof(Token));
    out[n].kind = T_SEMI;
    out[n].loc = n ? t[n - 1].loc : tok.loc;
    return out;
}

/* [captures](parameters) -> type { body } */
static void lambda_expr(Expr *e)
{
    Lambda *lam = (Lambda *)arena_alloc(&A, sizeof(Lambda));
    Type *t;
    Field *f;
    static const Token rp = {T_RPAREN, 0, 0, 0, 0};
    next();
    while (tok.kind != T_RBRACK) {
        if (tok.kind == T_EOF)
            error_at(tok.loc, "unterminated lambda");
        lam->captures = 1;
        next();
    }
    next();
    if (tok.kind == T_LPAREN) {
        lam->params = grab_balanced(&lam->nparams);
        lam->params++; /* without the '(' */
        lam->nparams--;
    } else {
        lam->params = (Token *)arena_alloc(&A, sizeof(Token));
        lam->params[0] = rp;
        lam->nparams = 1;
    }
    while (tok.kind != T_LBRACE) {
        if (tok.kind == T_EOF || tok.kind == T_SEMI)
            error_at(tok.loc, "expected the body of the lambda");
        if (tok.kind == T_ARROW) {
            next();
            lam->ret = type_name();
        } else if (tok.kind == KW(KW_ATTRIBUTE)) {
            Attr a;
            memset(&a, 0, sizeof a);
            attributes(&a);
        } else {
            next(); /* mutable, noexcept */
        }
    }
    lam->body = grab_balanced(&lam->nbody);
    lam->level = level;
    lam->hide = hide;
    t = type_record(TY_STRUCT, 0);
    f = (Field *)arena_alloc(&A, sizeof(Field));
    f->name = intern_cstr("__l");
    f->ty = ty_char;
    type_layout(t, f, 0, 0);
    t->canon->lam = lam;
    set_expr(e, t, new_temp(t), 1);
}

/* A lambda that captures nothing can be had as a function: it is generated after the current declaration. */
static void lambda_to_fn(Expr *e, Type *to)
{
    Lambda *lam = (Lambda *)e->ty->canon->lam;
    if (!lam->fn) {
        static u32 seq;
        char nm[40];
        sprintf(nm, "__lambda%u", seq++);
        lam->fn = new_sym(intern_cstr(nm), SK_VAR, to->base);
        lam->fn->is_static = 1;
        lam->fn->c_link = 1;
        pend_add(0, 0, lam, lam->fn);
    }
    set_expr(e, to, ir_addr(F, sym_obj(lam->fn), 0), 0);
}

static void bind_inline_param(u32 name, Type *t, Expr *a, int cx)
{
    Sym *p;
    Expr x = *a, lv;
    if (t->kind == TY_REF) {
        ref_addr(&x, t->base);
        p = new_sym(name, SK_VAR, t->base);
        p->local = 1;
        p->byref = 1;
        p->slot = ir_new_slot(F, 8, 8);
        ir_store(F, M_64, ir_slot_addr(F, p->slot, 0), x.v);
    } else {
        rvalue(&x);
        if (cx && type_is_scalar(t) && type_is_scalar(x.ty)) {
            u64 iv = 0;
            double fv = 0;
            convert(&x, unqual(t));
            p = new_sym(name, SK_VAR, unqual(t));
            if (type_is_int(t) && ir_is_const(F, x.v, &iv)) {
                p->cv = 1;
                p->val = (i64)iv;
            } else if (type_is_float(t) && ir_is_fconst(F, x.v, &fv)) {
                p->cv = 2;
                p->fval = fv;
            }
            if (p->cv) {
                if (name)
                    bind(p);
                return;
            }
        }
        p = new_sym(name, SK_VAR, t);
        p->local = 1;
        p->slot = ir_new_slot(F, type_size(t), type_align(t));
        set_expr(&lv, unqual(t), ir_slot_addr(F, p->slot, 0), 1);
        assign(&lv, &x);
    }
    if (name)
        bind(p);
}

/* Reads a body in place of a call to it. fty: the function's type, or 0 when the parameters
   are tokens (through their ')'). The caller has set what the body may see. */
static void inline_run(Expr *res, Type *fty, Token *ptoks, u32 np, Token *body, u32 nb, Type *ret, Expr *av, u32 n,
                       int cx)
{
    struct Inl saved = inl;
    u32 s_break = break_blk, s_cont = cont_blk, k = 0, loc = tok.loc;
    Switch *s_sw = cur_switch;
    int s_stop = launch_stop;

    push_scope();
    launch_stop = 0;
    if (fty) {
        if (n != fty->nparams)
            error_at(loc, "wrong number of arguments");
        for (k = 0; k < n; k++)
            bind_inline_param(fty->pnames[k], fty->params[k], &av[k], cx);
    } else {
        replay(ptoks, np);
        while (tok.kind != T_RPAREN) {
            int st;
            Type *t = declspec(&st);
            u32 name = 0;
            if (k >= n)
                error_at(loc, "too few arguments to the lambda");
            if (ds_auto) {
                Type *at = av[k].ty;
                at = at->kind == TY_ARRAY ? type_ptr(at->base) : at->kind == TY_FUNC ? type_ptr(at) : unqual(at);
                t = t->quals ? type_qual(at, t->quals) : at;
            }
            t = declarator(t, &name);
            if (t->kind == TY_ARRAY)
                t = type_ptr(t->base);
            bind_inline_param(name, t, &av[k], 0);
            k++;
            if (!accept(T_COMMA))
                break;
        }
        if (tok.kind != T_RPAREN)
            error_at(tok.loc, "expected ')'");
        next();
        if (k != n)
            error_at(loc, "too many arguments to the lambda");
    }
    memset(&inl, 0, sizeof inl);
    inl.on = 1;
    inl.cx = cx;
    inl.ty = ret;
    inl.start = F->cur;
    break_blk = cont_blk = 0;
    cur_switch = 0;
    replay(body, nb);
    expect(T_LBRACE);
    compound(0);
    if (inl.direct) {
        F->cur = inl.ret_cur;
        *res = inl.val;
    } else {
        if (inl.end) {
            ir_jmp(F, inl.end);
            F->cur = inl.end;
        }
        if (inl.ty && inl.ty->kind != TY_VOID && inl.slot) {
            if (type_is_record(inl.ty)) {
                set_expr(res, inl.ty, ir_slot_addr(F, inl.slot - 1, 0), 0);
            } else {
                set_expr(res, inl.ty, ir_slot_addr(F, inl.slot - 1, 0), 1);
                rvalue(res);
            }
        } else {
            set_expr(res, ty_void, 0, 0);
        }
    }
    pop_scope();
    inl = saved;
    break_blk = s_break;
    cont_blk = s_cont;
    cur_switch = s_sw;
    launch_stop = s_stop;
}

/* A return statement, after its keyword, in a body read in place of a call. */
static void inline_return(void)
{
    Expr e, lv;
    if (tok.kind == T_SEMI) {
        next();
        if (!inl.ty)
            inl.ty = ty_void;
    } else {
        expr(&e);
        expect(T_SEMI);
        if (inl.direct)
            return; /* unreachable: the body has already returned */
        rvalue(&e);
        if (!inl.ty)
            inl.ty = e.ty;
        if (inl.ty->kind == TY_REF)
            error_at(tok.loc, "a lambda that returns a reference is not supported");
        if (inl.ty->kind != TY_VOID) {
            if (!inl.nret && F->cur == inl.start) {
                if (!type_is_record(inl.ty))
                    convert(&e, inl.ty);
                inl.val = e;
                inl.direct = 1;
                inl.ret_cur = F->cur;
                inl.nret++;
                F->cur = 0;
                return;
            }
            if (inl.direct)
                return; /* unreachable: the body has already returned */
            if (!inl.slot)
                inl.slot = 1 + ir_new_slot(F, type_size(inl.ty), type_align(inl.ty));
            set_expr(&lv, inl.ty, ir_slot_addr(F, inl.slot - 1, 0), 1);
            assign(&lv, &e);
        }
    }
    inl.nret++;
    if (!inl.end)
        inl.end = ir_new_block(F);
    ir_jmp(F, inl.end);
}

/* ---- templates ---- */

static u32 targs_key(const TArgs *a)
{
    Buf b;
    u32 k, id;
    memset(&b, 0, sizeof b);
    for (k = 0; k < a->n; k++) {
        if (a->a[k].ty) {
            mg_type(&b, a->a[k].ty);
        } else {
            mg_s(&b, "L");
            if (a->a[k].val < 0) {
                mg_s(&b, "n");
                mg_u(&b, (u64)-a->a[k].val);
            } else {
                mg_u(&b, (u64)a->a[k].val);
            }
            mg_s(&b, "E");
        }
    }
    if (!b.len)
        mg_s(&b, "v");
    id = intern((const char *)b.p, b.len);
    buf_free(&b);
    return id;
}

/* The '>' that closes template arguments; ">>" closes two lists. */
static void close_angle(void)
{
    if (tok.kind == T_GT)
        next();
    else if (tok.kind == T_SHR)
        tok.kind = T_GT;
    else if (tok.kind == T_GE)
        tok.kind = T_ASSIGN;
    else
        error_at(tok.loc, "expected '>' after template arguments");
}

static void one_targ(TArg *x)
{
    x->ty = 0;
    x->val = 0;
    if (tok.kind == T_IDENT && is_typename(&tok)) {
        Type *t = cxx_named_type();
        if (!t) {
            x->ty = type_name();
        } else if (cxx_member) {
            Sym *m = cxx_member;
            if (m->kind != SK_ENUM && m->cv != 1)
                error_at(tok.loc, "a template argument must be a type or an integer constant");
            x->val = m->val;
        } else {
            u32 nm = 0;
            x->ty = declarator(t, &nm);
        }
    } else if (is_typename(&tok)) {
        x->ty = type_name();
    } else {
        x->val = const_int_expr();
    }
}

/* <arguments>, the current token being the '<' */
static void parse_targs(TArgs *a)
{
    int saved = launch_stop;
    next();
    a->n = 0;
    launch_stop = 1; /* a '>' ends the list, it does not compare */
    while (tok.kind != T_GT && tok.kind != T_SHR && tok.kind != T_GE) {
        if (a->n == MAX_TARGS)
            error_at(tok.loc, "too many template arguments");
        one_targ(&a->a[a->n++]);
        if (!accept(T_COMMA))
            break;
    }
    launch_stop = saved;
    close_angle();
}

/* Binds the first n parameters of a template to their arguments, in the current scope. */
static void bind_targs(Tmpl *t, const TArgs *a, u32 n)
{
    u32 k;
    for (k = 0; k < n; k++) {
        TParam *p = &t->params[k];
        Sym *s;
        if (!p->name)
            continue;
        if (p->is_type) {
            if (!a->a[k].ty)
                error_at(tok.loc, "template parameter '%s' needs a type", intern_str(p->name));
            s = new_sym(p->name, SK_TYPEDEF, a->a[k].ty);
        } else {
            if (a->a[k].ty)
                error_at(tok.loc, "template parameter '%s' needs a value", intern_str(p->name));
            s = new_sym(p->name, SK_VAR, p->vty ? unqual(p->vty) : ty_int);
            s->cv = 1;
            s->val = s->ty->kind == TY_BOOL ? a->a[k].val != 0 : a->a[k].val;
        }
        bind(s);
    }
}

/* Gives the next argument its parameter's default; 0 if it has none. */
static int targ_default(Tmpl *t, TArgs *a)
{
    TParam *p = &t->params[a->n];
    Hide saved = hide;
    if (!p->def)
        return 0;
    hide = t->hide;
    hide_push(0, level);
    push_scope();
    bind_targs(t, a, a->n);
    replay(p->def, p->ndef + 1);
    one_targ(&a->a[a->n]);
    expect(T_SEMI);
    pop_scope();
    hide = saved;
    a->n++;
    return 1;
}

/* Completes the arguments from the parameters' defaults; 0 if one is missing. */
static int targs_defaults(Tmpl *t, TArgs *a)
{
    while (a->n < t->nparams)
        if (!targ_default(t, a))
            return 0;
    return 1;
}

static TInst *inst_find(Tmpl *t, const TArgs *a, int create)
{
    u32 key = targs_key(a);
    TInst *ti;
    for (ti = t->insts; ti; ti = ti->next)
        if (ti->key == key)
            return ti;
    if (!create)
        return 0;
    ti = (TInst *)arena_alloc(&A, sizeof(TInst));
    ti->key = key;
    ti->args = *a;
    ti->next = t->insts;
    t->insts = ti;
    return ti;
}

/* The class a class template gives for these arguments, defined if the template is. */
static Type *class_inst(Tmpl *t, TArgs *a)
{
    TInst *ti;
    Hide saved = hide;
    Sym *self;
    Buf b;
    if (a->n > t->nparams || !targs_defaults(t, a))
        error_at(tok.loc, "wrong number of arguments for template '%s'", intern_str(t->name));
    ti = inst_find(t, a, 1);
    if (ti->ty)
        return ti->ty;
    memset(&b, 0, sizeof b);
    mg_s(&b, intern_str(t->name));
    mg_s(&b, "I");
    mg_s(&b, intern_str(ti->key));
    mg_s(&b, "E");
    ti->ty = type_record(t->rkind, intern((const char *)b.p, b.len));
    buf_free(&b);
    if (!t->toks)
        return ti->ty; /* declared only */
    hide = t->hide;
    hide_push(0, level);
    push_scope();
    bind_targs(t, a, a->n);
    self = new_sym(t->name, SK_TYPEDEF, ti->ty);
    bind(self);
    inst_rec = ti->ty;
    inst_rec_name = t->name;
    replay(t->toks, t->ntoks);
    {
        Tmpl *st = cinst_t;
        TInst *sti = cinst_ti;
        cinst_t = t;
        cinst_ti = ti;
        declspec(0);
        cinst_t = st;
        cinst_ti = sti;
    }
    inst_rec = 0;
    expect(T_SEMI);
    pop_scope();
    hide = saved;
    return ti->ty;
}

/* A class template's name was read: its arguments follow. */
static Type *tmpl_class_use(Sym *s)
{
    TArgs a;
    a.n = 0;
    if (tok.kind == T_LT)
        parse_targs(&a);
    return class_inst(s->tmpl, &a);
}

#define TP_SENT 0x40000000UL /* a value parameter not yet known stands as this + its number */
/* A function template's type with its parameters left open. */
static Type *tmpl_pattern(Tmpl *t)
{
    Hide saved = hide;
    u32 k, name = 0;
    int st;
    Type *ty;
    if (t->tried)
        return t->pattern;
    t->tried = 1;
    hide = t->hide;
    hide_push(0, level);
    push_scope();
    for (k = 0; k < t->nparams; k++) {
        TParam *p = &t->params[k];
        Sym *s;
        if (!p->name)
            continue;
        if (p->is_type) {
            s = new_sym(p->name, SK_TYPEDEF, type_tparam(k));
        } else {
            s = new_sym(p->name, SK_VAR, p->vty ? unqual(p->vty) : ty_int);
            s->cv = 1;
            s->val = (i64)(TP_SENT + k); /* so that an array's length shows which parameter gives it */
        }
        bind(s);
    }
    replay(t->head, t->nhead + 1);
    ty = declspec(&st);
    ty = declarator(ty, &name);
    while (tok.kind != T_SEMI && tok.kind != T_EOF)
        next();
    expect(T_SEMI);
    pop_scope();
    hide = saved;
    if (ty->kind == TY_FUNC)
        t->pattern = ty;
    return t->pattern;
}

/* Finds the type parameters that make the pattern p fit the type at. */
static void unify(Type *p, Type *at, TArgs *a, u8 *known)
{
    if (p->kind == TY_TPARAM) {
        u32 k = (u32)p->len;
        if (k < MAX_TARGS && !known[k]) {
            int q = at->quals & ~p->quals;
            Type *u = unqual(at);
            if (u->quals) { /* a kind unqual() leaves alone */
                Type *c = (Type *)arena_alloc(&A, sizeof(Type));
                *c = *u;
                c->quals = 0;
                c->ptr_to = 0;
                u = c;
            }
            a->a[k].ty = q ? type_qual(u, q) : u;
            a->a[k].val = 0;
            known[k] = 1;
        }
        return;
    }
    if (p->kind == TY_PTR && (at->kind == TY_PTR || at->kind == TY_ARRAY))
        unify(p->base, at->base, a, known);
    if (p->kind == TY_ARRAY && at->kind == TY_ARRAY) {
        if (p->len >= TP_SENT && p->len < TP_SENT + MAX_TARGS && !known[p->len - TP_SENT]) {
            u32 k = (u32)(p->len - TP_SENT);
            a->a[k].ty = 0;
            a->a[k].val = (i64)at->len;
            known[k] = 1;
        }
        unify(p->base, at->base, a, known);
    }
}

static Sym *fn_inst(Tmpl *t, TArgs *a)
{
    TInst *ti = inst_find(t, a, 1);
    Hide saved = hide;
    Type *ty;
    Attr at;
    int storage, is_inline, is_cx;
    u32 name = 0;
    Sym *fn;
    if (ti->fn)
        return ti->fn;
    hide = t->hide;
    hide_push(0, level);
    push_scope();
    bind_targs(t, a, a->n);
    replay(t->head, t->nhead + 1);
    memset(&dcl_attr, 0, sizeof dcl_attr);
    last_pdef = 0;
    ty = declspec(&storage);
    at = ds_attr;
    is_inline = ds_inline;
    is_cx = ds_constexpr;
    ty = declarator(ty, &name);
    at.flags |= dcl_attr.flags;
    decl_suffix(&at);
    while (tok.kind != T_SEMI && tok.kind != T_EOF)
        next();
    expect(T_SEMI);
    pop_scope();
    hide = saved;
    if (ty->kind != TY_FUNC)
        error_at(tok.loc, "template '%s' is not a function", intern_str(t->name));
    fn = new_sym(t->name, SK_VAR, ty);
    fn->asmname = mg_func(t->name, intern_str(ti->key), ty);
    fn->is_static = t->has_body;
    fn->is_cx = (u8)is_cx;
    fn->itm = t;
    fn->iti = ti;
    fn->pdef = last_pdef;
    fn->npdef = last_npdef;
    last_pdef = 0;
    sym_attrs(fn, &at, is_inline, storage);
    ti->fn = fn;
    if (t->has_body) {
        ti->queued = 1;
        pend_add(t, ti, 0, fn);
    }
    return fn;
}

/* The function a template gives for a call with these arguments, or 0 if it does not fit.
   given: the arguments written out, if any. av: the call's arguments, or 0 if it is not called. */
static Sym *tmpl_candidate(Sym *ts, TArgs *given, Expr *av, u32 n)
{
    Tmpl *t = ts->tmpl;
    TArgs a;
    u8 known[MAX_TARGS];
    u32 k;
    memset(known, 0, sizeof known);
    memset(&a, 0, sizeof a);
    if (given) {
        if (given->n > t->nparams)
            return 0;
        a = *given;
        for (k = 0; k < a.n; k++) {
            if ((a.a[k].ty != 0) != (t->params[k].is_type != 0))
                return 0;
            known[k] = 1;
        }
    }
    if (a.n < t->nparams && av) {
        Type *pat = tmpl_pattern(t);
        if (pat) {
            if (n > pat->nparams && !pat->variadic)
                return 0;
            for (k = 0; k < n && k < pat->nparams; k++) {
                Type *p = pat->params[k], *at = av[k].ty;
                if (p->kind == TY_REF) {
                    unify(p->base, at, &a, known);
                } else {
                    at = at->kind == TY_ARRAY ? type_ptr(at->base) : at->kind == TY_FUNC ? type_ptr(at) : unqual(at);
                    unify(p, at, &a, known);
                }
            }
        }
    }
    while (a.n < t->nparams) {
        /* deduced, or its default; a parameter after a default can still be deduced */
        if (known[a.n])
            a.n++;
        else if (!targ_default(t, &a))
            return 0;
    }
    if (av && n != t->nfparams) {
        /* cheap rejection by the number of parameters, before the declaration is read */
        Type *pat = t->tried ? t->pattern : 0;
        if (pat && n > pat->nparams && !pat->variadic)
            return 0;
    }
    return fn_inst(t, &a);
}

/* A call of a constexpr function with constant arguments is worked out here, by reading
   its body with the parameters as constants. Returns 0 if that cannot be done. */
static int cx_try(Expr *e, Sym *f, Expr *av, u32 n)
{
    u32 k;
    Hide saved = hide;
    Hide none;
    Token *body = f->body;
    u32 nbody = f->nbody;
    if (f->is_cx && !body && f->itm && !f->cls && f->itm->has_body && !f->iti->spec) {
        body = f->itm->toks + f->itm->nhead;
        nbody = f->itm->ntoks - f->itm->nhead;
    }
    if (!f->is_cx || !body || n != f->ty->nparams)
        return 0;
    for (k = 0; k < n; k++) {
        Expr x = av[k];
        u64 iv;
        double fv;
        if (x.lval || !type_is_scalar(x.ty) || !(ir_is_const(F, x.v, &iv) || ir_is_fconst(F, x.v, &fv)))
            return 0;
    }
    memset(&none, 0, sizeof none);
    hide = f->itm ? f->itm->hide : none;
    hide_push(0, level);
    push_scope();
    if (f->itm) {
        bind_targs(f->itm, &f->iti->args, f->iti->args.n);
        if (f->cls)
            bind(new_sym(f->itm->name, SK_TYPEDEF, f->iti->ty));
    }
    if (f->cls)
        cs_bind(f->cls);
    inline_run(e, f->ty, 0, 0, body, nbody, f->ty->base->kind == TY_REF ? 0 : unqual(f->ty->base), av, n, 1);
    pop_scope();
    hide = saved;
    return 1;
}

/* A function type with a parameter that is a lambda that captures: its instances are only ever read in place. */
static int lam_param(Type *fty)
{
    u32 k;
    for (k = 0; fty && fty->kind == TY_FUNC && k < fty->nparams; k++)
        if (LAM(fty->params[k]) && LAM(fty->params[k])->captures)
            return 1;
    return 0;
}

/* A function template called with a lambda that captures: its instance is read in place of the call, so that the
   lambda, read again where the template calls it, still sees the locals it captured (a function of its own would not). */
static int lam_try(Expr *e, Sym *f, Expr *av, u32 n)
{
    u32 k;
    int any = 0;
    Hide saved = hide;
    Hide none;
    Token *body;
    u32 nbody;
    if (!f->itm || f->cls || !f->itm->has_body || f->iti->spec || n != f->ty->nparams || f->ty->variadic)
        return 0;
    for (k = 0; k < n; k++)
        if (LAM(av[k].ty) && LAM(av[k].ty)->captures)
            any = 1;
    if (!any)
        return 0;
    body = f->itm->toks + f->itm->nhead;
    nbody = f->itm->ntoks - f->itm->nhead;
    memset(&none, 0, sizeof none);
    hide = f->itm->hide;
    hide_push(0, level);
    push_scope();
    bind_targs(f->itm, &f->iti->args, f->iti->args.n);
    inline_run(e, f->ty, 0, 0, body, nbody, f->ty->base->kind == TY_REF ? 0 : unqual(f->ty->base), av, n, 0);
    pop_scope();
    hide = saved;
    (void)none;
    return 1;
}

static void launch_config(u32 loc);
static Sym *ctor_set(Type *t);
static void ctor_call(Expr *e, Sym *set, Expr *av, u32 n, u32 loc);

/* Expressions that only C++ has. Returns 0, having read nothing, if the current token starts none. */
static int cxx_primary(Expr *e)
{
    u32 loc = tok.loc;
    Sym *s, *ms = 0;
    Type *t;
    switch (tok.kind) {
    case T_LBRACK:
        lambda_expr(e);
        return 1;
    case KW(KW_CHAR): case KW(KW_SHORT): case KW(KW_INT): case KW(KW_LONG): case KW(KW_FLOAT): case KW(KW_DOUBLE):
    case KW(KW_SIGNED): case KW(KW_UNSIGNED): case KW(KW_BOOL):
        goto type;
    case T_IDENT:
        break;
    default:
        return 0;
    }
    s = lookup(tok.v);
    if (!s) {
        if (tok.v == id_static_cast || tok.v == id_reinterpret_cast || tok.v == id_const_cast) {
            next();
            if (tok.kind != T_LT)
                error_at(tok.loc, "expected '<'");
            next();
            t = type_name();
            close_angle();
            expect(T_LPAREN);
            expr(e);
            expect(T_RPAREN);
            do_cast(e, t, loc);
            return 1;
        }
        if (tok.v == id_is_same) {
            Type *a, *b;
            next();
            expect(T_LPAREN);
            a = type_name();
            expect(T_COMMA);
            b = type_name();
            expect(T_RPAREN);
            set_int(e, ty_bool, a->quals == b->quals && type_compatible(unqual(a), unqual(b)));
            e->konst = 1;
            return 1;
        }
        if (find_tag(tok.v) || tok.v == id_typename || tok.v == id_decltype)
            goto type;
        return 0;
    }
    if (s->kind == SK_TYPEDEF || (s->kind == SK_TEMPLATE && s->is_ctmpl))
        goto type;
    if (s->real)
        return 0;
    if (s->cv && s == cv_lvalue) {
        u32 addr = new_temp(s->ty);
        next();
        cv_lvalue = 0;
        store_val(s->ty, addr, s->cv == 1 ? ir_const(F, type_ir(s->ty), (u64)s->val) : ir_fconst(F, type_ir(s->ty), s->fval));
        set_expr(e, s->ty, addr, 1);
        return 1;
    }
    if (s->cv) {
        next();
        if (s->cv == 1)
            set_int(e, s->ty, (u64)s->val);
        else
            set_expr(e, s->ty, ir_fconst(F, type_ir(s->ty), s->fval), 0);
        e->konst = s->cv == 1;
        return 1;
    }
    if (s->kind == SK_VAR && LAM(s->ty)) {
        next();
        set_expr(e, s->ty, new_temp(s->ty), 1);
        return 1;
    }
fcall:
    if (ms || s->kind == SK_TEMPLATE || (s->kind == SK_VAR && !s->local && s->ty->kind == TY_FUNC &&
                                         (s->over || s->pdef || s->is_cx))) {
        TArgs given;
        Expr av[64];
        Sym *f = 0, *c;
        int has_given = 0, any_t = 0;
        u32 n;
        for (c = s; c; c = c->over)
            if (c->kind == SK_TEMPLATE)
                any_t = 1;
        if (!ms)
            next();
        if (any_t && tok.kind == T_LT) {
            parse_targs(&given);
            has_given = 1;
        }
        if (opt_cuda && tok.kind == T_SHL && peek()->kind == T_LT)
            launch_config(loc); /* through the '(' */
        else if (tok.kind == T_LPAREN)
            next();
        else {
            /* named, not called: it must be clear which one is meant */
            for (c = s; c && !f; c = c->over) {
                if (c->kind == SK_TEMPLATE && has_given)
                    f = tmpl_candidate(c, &given, 0, 0);
                else if (c->kind == SK_VAR && !has_given)
                    f = c;
            }
            if (!f)
                error_at(loc, "cannot tell which '%s' is meant", intern_str(s->name));
            set_expr(e, f->ty, ir_addr(F, sym_obj(f), 0), 0);
            return 1;
        }
        {
            struct Launch lc = launch;
            launch.on = 0;
            n = parse_args(av, 64);
            f = overload_pick(s, has_given ? &given : 0, av, n, loc);
            if (!lc.on && cx_try(e, f, av, n))
                return 1;
            if (!lc.on && lam_try(e, f, av, n))
                return 1;
            launch = lc;
        }
        set_expr(e, f->ty, ir_addr(F, sym_obj(f), 0), 0);
        call_pre = av;
        call_npre = n;
        call_sym = f;
        call(e);
        return 1;
    }
    return 0;
type:
    t = tok.kind == T_IDENT ? cxx_named_type() : 0;
    if (!t)
        t = declspec(0);
    if (cxx_member) {
        Sym *m = cxx_member;
        cxx_member = 0;
        if (m->kind == SK_VAR && m->ty->kind == TY_FUNC) {
            s = ms = m;
            goto fcall;
        }
        if (m->kind == SK_ENUM || m->cv == 1) {
            set_int(e, m->ty, (u64)m->val);
            e->konst = 1;
        } else if (m->cv == 2) {
            set_expr(e, m->ty, ir_fconst(F, type_ir(m->ty), m->fval), 0);
        } else {
            set_expr(e, m->ty, ir_addr(F, m->objsym, 0), 1);
        }
        return 1;
    }
    if (tok.kind == T_LPAREN && ctor_set(t)) {
        Expr av[32];
        u32 n;
        next();
        n = parse_args(av, 32);
        ctor_call(e, ctor_set(t), av, n, loc);
        return 1;
    }
    if (tok.kind == T_LPAREN) {
        int saved_stop = launch_stop;
        launch_stop = 0;
        next();
        if (tok.kind == T_RPAREN) {
            if (!type_is_scalar(t))
                error_at(loc, "a value of this type cannot be made from nothing");
            set_int(e, ty_int, 0);
        } else {
            expr(e);
        }
        launch_stop = saved_stop;
        expect(T_RPAREN);
        do_cast(e, t, loc);
    } else if (tok.kind == T_LBRACE) {
        compound_literal(e, t);
    } else {
        error_at(loc, "expected '(' after a type name");
    }
    return 1;
}

/* CUDA: <<<grid, block[, shared bytes[, stream]]>>> and the '(' after it; the call that follows is a launch. */
static void launch_config(u32 loc)
{
    Expr c[4];
    int n = 0, k, saved_stop;
    u32 dims[2];
    next();
    next();
    saved_stop = launch_stop;
    launch_stop = 2; /* only the closing >>> ends an expression */
    for (;;) {
        if (n == 4)
            error_at(loc, "too many launch parameters");
        assign_expr(&c[n]);
        rvalue(&c[n]);
        n++;
        if (!accept(T_COMMA))
            break;
    }
    launch_stop = saved_stop;
    expect(T_SHR);
    expect(T_GT);
    if (n < 2)
        error_at(loc, "a launch needs a grid and a block size");
    for (k = 0; k < 2; k++) {
        if (type_is_record(c[k].ty)) {
            if (type_size(c[k].ty) != 12)
                error_at(loc, "a launch size must be a dim3 or an integer");
            dims[k] = c[k].v;
        } else {
            u32 t = ir_slot_addr(F, ir_new_slot(F, 12, 4), 0);
            convert(&c[k], ty_uint);
            ir_store(F, M_32, t, c[k].v);
            ir_store(F, M_32, ir_ins(F, OP_ADD, IT_I64, t, ir_const(F, IT_I64, 4)), ir_const(F, IT_I32, 1));
            ir_store(F, M_32, ir_ins(F, OP_ADD, IT_I64, t, ir_const(F, IT_I64, 8)), ir_const(F, IT_I32, 1));
            dims[k] = t;
        }
    }
    if (n > 2)
        convert(&c[2], ty_ulong);
    if (n > 3)
        convert(&c[3], type_ptr(ty_void));
    if (tok.kind != T_LPAREN)
        error_at(tok.loc, "expected '(' after the launch parameters");
    next();
    launch.on = 1;
    launch.grid = dims[0];
    launch.block = dims[1];
    launch.shm = n > 2 ? c[2].v : ir_const(F, IT_I64, 0);
    launch.stream = n > 3 ? c[3].v : ir_const(F, IT_I64, 0);
}

static void primary(Expr *e)
{
    Sym *s;
    u32 loc = tok.loc;
    if (opt_cxx && cxx_primary(e))
        return;
    switch (tok.kind) {
    case T_NUM: {
        u64 iv;
        double fv;
        int flags;
        if (lex_number(&tok, &iv, &fv, &flags)) {
            Type *t = flags & NUM_FLOAT ? ty_float : flags & NUM_LONG ? ty_ldouble : ty_double;
            set_expr(e, t, ir_fconst(F, type_ir(t), fv), 0);
        } else {
            Type *t;
            int dec = flags & NUM_DECIMAL;
            if (flags & NUM_LLONG)
                t = (flags & NUM_UNSIGNED) || iv > 0x7fffffffffffffffUL ? ty_ullong : ty_llong;
            else if (flags & NUM_UNSIGNED)
                t = (flags & NUM_LONG) || iv > 0xffffffffUL ? ty_ulong : ty_uint;
            else if (flags & NUM_LONG)
                t = iv > 0x7fffffffffffffffUL ? ty_ulong : ty_long;
            else if (iv <= 0x7fffffffUL)
                t = ty_int;
            else if (!dec && iv <= 0xffffffffUL)
                t = ty_uint;
            else if (iv <= 0x7fffffffffffffffUL)
                t = ty_long;
            else
                t = ty_ulong;
            set_int(e, t, iv);
        }
        next();
        return;
    }
    case T_CHAR:
        set_int(e, ty_int, (u64)(i64)lex_char(&tok));
        next();
        return;
    case T_STR: {
        Buf b;
        int wide;
        memset(&b, 0, sizeof b);
        wide = string_literal(&b);
        set_expr(e, type_array(wide ? ty_int : ty_char, wide ? b.len / 4 : b.len, 1),
                 ir_addr(F, string_sym(b.p, b.len), 0), 1);
        buf_free(&b);
        return;
    }
    case T_LPAREN: {
        int saved_stop = launch_stop;
        launch_stop = 0;
        next();
        if (tok.kind == T_LBRACE)
            stmt_expr(e);
        else
            expr(e);
        launch_stop = saved_stop;
        expect(T_RPAREN);
        return;
    }
    case KW(KW_GENERIC): {
        Expr c, r;
        Type *ct;
        Token *dflt = 0;
        u32 ndflt = 0;
        int done = 0;
        next();
        expect(T_LPAREN);
        dead_expr(&c);
        {
            u32 saved = F->cur;
            F->cur = 0;
            rvalue(&c);
            F->cur = saved;
        }
        ct = unqual(c.ty);
        expect(T_COMMA);
        for (;;) {
            if (accept(KW(KW_DEFAULT))) {
                expect(T_COLON);
                ndflt = skim(&dflt);
                if (!dflt)
                    error_at(loc, "empty default association");
            } else {
                Type *t = type_name();
                expect(T_COLON);
                if (!done && type_compatible(t, ct) && t->quals == 0) {
                    assign_expr(e);
                    done = 1;
                } else {
                    dead_expr(&r);
                }
            }
            if (!accept(T_COMMA))
                break;
        }
        if (!done) {
            if (!dflt)
                error_at(loc, "no association matches the type");
            replay(dflt, ndflt);
            assign_expr(e);
        }
        expect(T_RPAREN);
        return;
    }
    case T_IDENT:
        if (builtin(e))
            return;
        break;
    default:
        error_at(loc, "expected expression before '%s'", tok_text(&tok));
    }

    if (tok.v == id_va_start || tok.v == id_va_end) {
        int start = tok.v == id_va_start;
        Expr ap, dummy;
        next();
        expect(T_LPAREN);
        assign_expr(&ap);
        if (start && opt_arch == ARCH_X64) {
            x64_va_start(&ap);
            if (accept(T_COMMA))
                assign_expr(&dummy);
        } else if (start) {
            u32 k, n = opt_target == TARGET_LINUX ? 3 : 1;
            need_lvalue(&ap);
            /* Linux: where the stack arguments begin, then the ends of the two
               areas of saved registers, then how far below those ends the
               first unnamed argument is */
            for (k = 0; k < n; k++) {
                u32 v = ir_raw(F, OP_VASTART, IT_I64, k, 0);
                if (F->cur)
                    ir_append(F, F->cur, v);
                ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, ap.v, ir_const(F, IT_I64, 8 * k)), v);
            }
            if (n == 3) {
                u32 gr = cur_named_gr > 8 ? 8 : cur_named_gr, fr = cur_named_fr > 8 ? 8 : cur_named_fr;
                ir_store(F, M_32, ir_ins(F, OP_ADD, IT_I64, ap.v, ir_const(F, IT_I64, 24)),
                         ir_const(F, IT_I32, (u64)(-(i64)(8 - gr) * 8)));
                ir_store(F, M_32, ir_ins(F, OP_ADD, IT_I64, ap.v, ir_const(F, IT_I64, 28)),
                         ir_const(F, IT_I32, opt_no_fp ? 0 : (u64)(-(i64)(8 - fr) * 16)));
            }
            if (accept(T_COMMA))
                assign_expr(&dummy);
        }
        expect(T_RPAREN);
        set_expr(e, ty_void, 0, 0);
        return;
    }
    if (tok.v == id_va_arg) {
        Expr ap;
        Type *t;
        u32 p;
        next();
        expect(T_LPAREN);
        assign_expr(&ap);
        if (opt_arch != ARCH_X64)
            need_lvalue(&ap);
        expect(T_COMMA);
        t = type_name();
        expect(T_RPAREN);
        if (opt_arch == ARCH_X64) {
            x64_va_arg(e, &ap, t);
            return;
        }
        if (opt_target == TARGET_LINUX) {
            /* from the saved registers while they last, then from the stack */
            int fp = type_is_float(t);
            u32 words = type_is_record(t) ? abi_words(t) : type_is_128(t) ? 2 : 1;
            u32 step = (fp ? 16 : 8) * (words ? words : 1), nstack = words ? words : 1;
            u32 offs_at = ir_ins(F, OP_ADD, IT_I64, ap.v, ir_const(F, IT_I64, fp ? 28 : 24));
            u32 top_at = ir_ins(F, OP_ADD, IT_I64, ap.v, ir_const(F, IT_I64, fp ? 16 : 8));
            u32 slot = new_temp(type_ptr(ty_char)), offs, next_offs, a;
            u32 try = ir_new_block(F), reg = ir_new_block(F), stk = ir_new_block(F), done = ir_new_block(F);
            offs = ir_load(F, IT_I32, M_32, offs_at);
            next_offs = ir_ins(F, OP_ADD, IT_I32, offs, ir_const(F, IT_I32, step));
            if (F->cur) {
                ir_br(F, ir_ins(F, OP_SLT, IT_I32, offs, ir_const(F, IT_I32, 0)), try, stk);
                F->cur = try;
                ir_store(F, M_32, offs_at, next_offs);
                ir_br(F, ir_ins(F, OP_SLE, IT_I32, next_offs, ir_const(F, IT_I32, 0)), reg, stk);
                F->cur = reg;
                a = ir_ins(F, OP_ADD, IT_I64, ir_load(F, IT_I64, M_64, top_at), ir_ins(F, OP_SEXT32, IT_I64, offs, 0));
                ir_store(F, M_64, slot, a);
                ir_jmp(F, done);
                F->cur = stk;
                a = ir_load(F, IT_I64, M_64, ap.v);
                ir_store(F, M_64, ap.v, ir_ins(F, OP_ADD, IT_I64, a, ir_const(F, IT_I64, 8 * nstack)));
                ir_store(F, M_64, slot, a);
                ir_jmp(F, done);
                F->cur = done;
            }
            p = ir_load(F, IT_I64, M_64, slot);
            if (type_is_record(t) && !words)
                p = ir_load(F, IT_I64, M_64, p); /* a large record is passed by its address */
            set_expr(e, t, p, 1);
            return;
        }
        p = ir_load(F, IT_I64, M_64, ap.v);
        ir_store(F, M_64, ap.v, ir_ins(F, OP_ADD, IT_I64, p, ir_const(F, IT_I64, 8)));
        if (type_is_record(t))
            set_expr(e, t, ir_load(F, IT_I64, M_64, p), 0);
        else
            set_expr(e, t, p, 1);
        return;
    }

    s = lookup(tok.v);
    if (!s && (opt_c23 || opt_cxx)) {
        if (tok.v == id_nullptr) {
            next();
            set_int(e, ty_long, 0);
            convert(e, type_ptr(ty_void));
            e->konst = 1;
            return;
        }
        if (tok.v == id_true || tok.v == id_false) {
            int v = tok.v == id_true;
            next();
            set_int(e, opt_cxx ? ty_bool : ty_int, (u64)v);
            return;
        }
    }
    if (!s) {
        /* C89: a call to an undeclared function declares it as int f() */
        if (peek()->kind != T_LPAREN)
            error_at(loc, "'%s' undeclared", intern_str(tok.v));
        names_reserve(tok.v);
        s = gsyms[tok.v];
        if (!s) {
            s = new_sym(tok.v, SK_VAR, type_func(ty_int, 0, 0, 0, 0, 1));
            gsyms[tok.v] = s;
        }
    }
    next();
    if (s->real)
        s = s->real;
    switch (s->kind) {
    case SK_ENUM:
        set_int(e, s->ty, (u64)s->val);
        return;
    case SK_TYPEDEF:
        error_at(loc, "unexpected type name '%s'", intern_str(s->name));
    }
    if (s->local) {
        u32 addr = ir_slot_addr(F, s->slot, 0);
        if (s->byref)
            addr = ir_load(F, IT_I64, M_64, addr);
        set_expr(e, s->ty, addr, 1);
    } else if ((s->aflags & AT_REGISTER) && s->asmname && strcmp(intern_str(s->asmname), "sp") == 0) {
        /* register unsigned long current_stack_pointer asm("sp") */
        u32 v = ir_raw(F, OP_FRAMEADDR, IT_I64, 3, 0);
        if (F->cur)
            ir_append(F, F->cur, v);
        set_expr(e, s->ty, v, 0);
    } else if (s->is_tls) {
        /* this thread's copy */
        set_expr(e, s->ty, ir_ins(F, OP_TLS, IT_I64, ir_addr(F, sym_obj(s), 0), 0), 1);
    } else {
        set_expr(e, s->ty, ir_addr(F, sym_obj(s), 0), s->ty->kind != TY_FUNC);
    }
}

static void postfix_ops(Expr *e)
{
    for (;;) {
        u32 loc = tok.loc;
        switch (tok.kind) {
        case T_SHL:
            if (!opt_cuda || peek()->kind != T_LT || e->ty->kind != TY_FUNC)
                return;
            launch_config(loc);
            call(e);
            break;
        case T_LBRACK: {
            Expr idx;
            int saved_stop = launch_stop;
            next();
            rvalue(e);
            launch_stop = 0;
            expr(&idx);
            launch_stop = saved_stop;
            rvalue(&idx);
            expect(T_RBRACK);
            if (VK(e->ty) == 1) {
                e->ty = type_ptr(e->ty->canon->base); /* the elements, as an array */
                e->lval = 0;
            }
            binop(T_PLUS, e, &idx, loc);
            if (e->ty->kind != TY_PTR)
                error_at(loc, "subscripted value is not an array or pointer");
            e->ty = e->ty->base;
            e->lval = 1;
            break;
        }
        case T_LPAREN:
            next();
            if (opt_cxx && LAM(e->ty)) {
                Expr av[32];
                Lambda *lam = LAM(e->ty);
                u32 n = parse_args(av, 32);
                Hide saved = hide;
                hide = lam->hide;
                hide_push(lam->level, level);
                inline_run(e, 0, lam->params, lam->nparams, lam->body, lam->nbody, lam->ret, av, n, 0);
                hide = saved;
                break;
            }
            call(e);
            break;
        case T_DOT:
            next();
            member(e, 0);
            break;
        case T_ARROW:
            next();
            member(e, 1);
            break;
        case T_INC:
        case T_DEC:
            incdec(e, tok.kind == T_INC ? T_PLUS : T_MINUS, 1);
            next();
            break;
        default:
            return;
        }
    }
}

static void postfix(Expr *e)
{
    primary(e);
    postfix_ops(e);
}

/* (type){ ... }: the current token is the brace. */
static void compound_literal(Expr *e, Type *t)
{
    if (F == const_func) {
        Sym *s = new_sym(0, SK_VAR, t);
        s->is_static = 1;
        s->objsym = obj_add_sym(O, 0, SEC_UNDEF, 0, 0);
        static_init_sym(s);
        sym_align(s->objsym, s->ty);
        set_expr(e, s->ty, ir_addr(F, s->objsym, 0), 1);
    } else {
        initializer_for(e, t);
    }
}

static void unary(Expr *e)
{
    u32 loc = tok.loc;
    int it;
    if (tok.kind == T_IDENT && (tok.v == id_real || tok.v == id_imag) && !lookup(tok.v)) {
        int im = tok.v == id_imag;
        Expr a;
        next();
        cast_expr(e);
        if (VK(e->ty) == 2) {
            vec_elem(e, (u32)im, &a);
            *e = a;
        } else {
            rvalue(e);
            if (!type_is_arith(e->ty))
                error_at(loc, "invalid operand");
            if (im)
                set_expr(e, e->ty, type_is_float(e->ty) ? ir_fconst(F, type_ir(e->ty), 0.0) : ir_const(F, type_ir(e->ty), 0), 0);
        }
        return;
    }
    switch (tok.kind) {
    case T_INC:
    case T_DEC: {
        int op = tok.kind == T_INC ? T_PLUS : T_MINUS;
        next();
        unary(e);
        incdec(e, op, 0);
        return;
    }
    case T_ANDAND: {
        /* GNU: the address of a label. A label declared with __label__ gives "about here", which is all
           such labels are used for. Any other gives a small number that stands for the label: `goto *p`
           finds the label by that number, and being a constant it can sit in a static table. */
        u32 v;
        Label *l;
        next();
        if (tok.kind != T_IDENT)
            error_at(tok.loc, "expected a label name");
        l = get_label(tok.v);
        next();
        if (l->local) {
            v = ir_raw(F, OP_FRAMEADDR, IT_I64, 2, 0);
            if (F->cur)
                ir_append(F, F->cur, v);
        } else {
            if (!l->index)
                l->index = ++nlabel_index;
            v = ir_const(F, IT_I64, l->index);
        }
        set_expr(e, type_ptr(ty_void), v, 0);
        return;
    }
    case T_AMP:
        next();
        if (opt_cxx && tok.kind == T_IDENT) {
            Sym *cs = lookup(tok.v);
            if (cs && cs->cv)
                cv_lvalue = cs; /* a constant with no storage is given some for the occasion */
        }
        cast_expr(e);
        cv_lvalue = 0;
        if (e->ty->kind == TY_FUNC) {
            e->ty = type_ptr(e->ty);
            return;
        }
        if (!e->lval || e->bitwidth)
            error_at(loc, "cannot take the address of this expression");
        e->ty = type_ptr(e->ty);
        e->lval = 0;
        return;
    case T_STAR:
        next();
        cast_expr(e);
        rvalue(e);
        if (e->ty->kind != TY_PTR)
            error_at(loc, "indirection requires a pointer");
        e->ty = e->ty->base;
        e->lval = e->ty->kind != TY_FUNC;
        return;
    case T_PLUS:
    case T_MINUS:
    case T_TILDE: {
        int op = tok.kind;
        next();
        cast_expr(e);
        rvalue(e);
        if (VK(e->ty) == 2 && op != T_TILDE) {
            if (op == T_MINUS)
                vec_unary(op, e);
            return;
        }
        if (VK(e->ty) == 1) {
            vec_unary(op, e);
            return;
        }
        if (!type_is_arith(e->ty) || (op == T_TILDE && !type_is_int(e->ty)))
            error_at(loc, "invalid operand to unary '%s'", tok_names[op]);
        convert(e, type_promote(e->ty));
        if (type_is_128(e->ty)) {
            /* both halves: -x is ~x + 1, the carry going up when the low half is 0 */
            if (op == T_MINUS) {
                u32 z = ir_ins(F, OP_ZEXT32, IT_I64, ir_ins(F, OP_EQ, IT_I32, e->v, K64(0)), 0);
                e->hi = I64OP(OP_ADD, ir_ins(F, OP_NOT, IT_I64, e->hi, 0), z);
                e->v = ir_ins(F, OP_NEG, IT_I64, e->v, 0);
            } else {
                e->v = ir_ins(F, OP_NOT, IT_I64, e->v, 0);
                e->hi = ir_ins(F, OP_NOT, IT_I64, e->hi, 0);
            }
            return;
        }
        it = type_ir(e->ty);
        if (op == T_MINUS)
            e->v = ir_ins(F, type_is_float(e->ty) ? OP_FNEG : OP_NEG, it, e->v, 0);
        else if (op == T_TILDE)
            e->v = ir_ins(F, OP_NOT, it, e->v, 0);
        return;
    }
    case T_NOT:
        next();
        cast_expr(e);
        rvalue(e);
        e->v = ir_ins(F, OP_EQ, IT_I32, to_bool(e), ir_const(F, IT_I32, 0));
        e->ty = ty_int;
        return;
    case KW(KW_EXTENSION):
        next();
        cast_expr(e);
        return;
    case KW(KW_ALIGNOF):
    case KW(KW_SIZEOF): {
        u32 saved = F->cur;
        int al = tok.kind == KW(KW_ALIGNOF);
        Type *t;
        next();
        F->cur = 0; /* the operand is not evaluated */
        if (tok.kind == T_LPAREN && is_typename(peek())) {
            next();
            t = type_name();
            expect(T_RPAREN);
            if (tok.kind == T_LBRACE) {
                compound_literal(e, t);
                postfix_ops(e);
                t = e->ty;
            }
        } else {
            unary(e);
            if (e->bitwidth && !al)
                error_at(loc, "sizeof applied to a bit-field");
            t = e->ty;
        }
        F->cur = saved;
        if (t->kind == TY_FUNC || t->kind == TY_VOID) {
            set_int(e, ty_ulong, 1); /* GNU */
            return;
        }
        if (!al && t->kind == TY_ARRAY && t->vslot) {
            set_expr(e, ty_ulong, ir_load(F, IT_I64, M_64, ir_slot_addr(F, t->vslot - 1, 0)), 0);
            return;
        }
        if (!type_complete(t) && !(al && t->kind == TY_ARRAY && type_complete(t->base)))
            error_at(loc, "sizeof applied to an incomplete type");
        set_int(e, ty_ulong, al ? type_align(t) : type_size(t));
        return;
    }
    }
    postfix(e);
}

static void do_cast(Expr *e, Type *t, u32 loc);

static void cast_expr(Expr *e)
{
    if (tok.kind == T_LPAREN && is_typename(peek())) {
        u32 loc = tok.loc;
        Type *t;
        next();
        if (opt_cxx) {
            /* "(T)x", but also "(T(x) + 1)" and "(T::member * 2)" */
            Sym *al = 0;
            u32 nm = 0;
            t = tok.kind == T_IDENT ? cxx_named_type() : 0;
            if (!t) {
                cxx_member = 0;
                t = declspec(0);
            }
            if (cxx_member) {
                al = (Sym *)arena_alloc(&A, sizeof(Sym));
                *al = *cxx_member;
            } else if (tok.kind == T_LBRACE) {
                al = new_sym(0, SK_TYPEDEF, t);
            } else if (tok.kind == T_LPAREN) {
                Token *pk = peek();
                if (pk->kind != T_STAR && pk->kind != T_AMP && pk->kind != T_ANDAND && pk->kind != T_RPAREN &&
                    pk->kind != T_LBRACK && !is_typename(pk))
                    al = new_sym(0, SK_TYPEDEF, t);
            }
            if (al) {
                /* not a cast: go on with a name made up for what was read */
                static u32 seq;
                char tmp[32];
                Token it;
                sprintf(tmp, "__cxx_n%u", seq++);
                al->name = intern_cstr(tmp);
                al->real = 0;
                bind(al);
                memset(&it, 0, sizeof it);
                it.kind = T_IDENT;
                it.v = al->name;
                it.loc = loc;
                replay(&it, 1);
                expr(e);
                expect(T_RPAREN);
                postfix_ops(e);
                return;
            }
            t = declarator(t, &nm);
        } else {
            t = type_name();
        }
        expect(T_RPAREN);
        if (tok.kind == T_LBRACE) {
            compound_literal(e, t);
            postfix_ops(e);
            return;
        }
        cast_expr(e);
        do_cast(e, t, loc);
        return;
    }
    unary(e);
}

static void do_cast(Expr *e, Type *t, u32 loc)
{
    {
        rvalue(e);
        if (type_is_record(t) && type_is_record(e->ty) && t->canon == e->ty->canon) {
            e->ty = t;
            return;
        }
        if (special_pair(t, e->ty)) {
            convert(e, t);
            return;
        }
        if (VK(t) == 1 && VK(e->ty) == 1 && type_size(t) == type_size(e->ty)) {
            e->ty = t->canon; /* the same bits */
            return;
        }
        if (t->kind == TY_UNION && F != const_func) {
            /* GNU: a cast to a union makes one whose member of the operand's type holds the operand */
            Field *f, *pick = 0;
            for (f = t->fields; f && !pick; f = f->next)
                if (f->ty->canon && f->ty->canon == e->ty->canon)
                    pick = f;
            for (f = t->fields; f && !pick; f = f->next)
                if (type_is_scalar(f->ty) && type_is_scalar(e->ty) && type_size(f->ty) == type_size(e->ty) &&
                    type_is_float(f->ty) == type_is_float(e->ty))
                    pick = f;
            if (pick) {
                Expr lv;
                u32 addr = new_temp(t);
                set_expr(&lv, pick->ty, addr, 1);
                assign(&lv, e);
                set_expr(e, t, addr, 1);
                return;
            }
        }
        if (t->kind != TY_VOID) {
            if (!type_is_scalar(t) || !type_is_scalar(e->ty))
                error_at(loc, "invalid cast");
            if ((type_is_float(t) && e->ty->kind == TY_PTR) || (t->kind == TY_PTR && type_is_float(e->ty)))
                error_at(loc, "invalid cast");
        }
        convert(e, t);
        return;
    }
}

static int binary_prec(int kind)
{
    if (launch_stop == 1 && (kind == T_SHR || kind == T_GT || kind == T_GE))
        return 0;
    if (launch_stop == 2 && kind == T_SHR && peek()->kind == T_GT)
        return 0;
    switch (kind) {
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    case T_PLUS: case T_MINUS: return 9;
    case T_SHL: case T_SHR: return 8;
    case T_LT: case T_GT: case T_LE: case T_GE: return 7;
    case T_EQ: case T_NE: return 6;
    case T_AMP: return 5;
    case T_CARET: return 4;
    case T_PIPE: return 3;
    case T_ANDAND: return 2;
    case T_OROR: return 1;
    }
    return 0;
}

static void binary(Expr *e, int minprec)
{
    cast_expr(e);
    for (;;) {
        int op = tok.kind, prec = binary_prec(op), k = e->konst;
        u32 loc = tok.loc;
        Expr r;
        if (prec == 0 || prec < minprec)
            return;
        next();
        rvalue(e);
        if (op == T_ANDAND || op == T_OROR) {
            u32 l = to_bool(e), slot, rhs, end, saved;
            u64 c;
            if (ir_is_const(F, l, &c)) {
                int decided = op == T_ANDAND ? c == 0 : c != 0;
                saved = F->cur;
                if (decided)
                    F->cur = 0;
                binary(&r, prec + 1);
                rvalue(&r);
                rhs = to_bool(&r);
                if (decided) {
                    F->cur = saved;
                    set_int(e, ty_int, op == T_OROR);
                } else {
                    set_expr(e, ty_int, rhs, 0);
                }
                e->konst = (u8)(k && r.konst);
                continue;
            }
            slot = new_temp(ty_int);
            rhs = ir_new_block(F);
            end = ir_new_block(F);
            ir_store(F, M_32, slot, ir_const(F, IT_I32, op == T_OROR));
            if (op == T_ANDAND)
                ir_br(F, l, rhs, end);
            else
                ir_br(F, l, end, rhs);
            F->cur = rhs;
            binary(&r, prec + 1);
            rvalue(&r);
            ir_store(F, M_32, slot, to_bool(&r));
            ir_jmp(F, end);
            F->cur = end;
            set_expr(e, ty_int, ir_load(F, IT_I32, M_32, slot), 0);
            continue;
        }
        binary(&r, prec + 1);
        rvalue(&r);
        binop(op, e, &r, loc);
        e->konst = (u8)(k && r.konst);
    }
}

static int is_assign_op(int k)
{
    switch (k) {
    case T_ASSIGN: case T_MULEQ: case T_DIVEQ: case T_MODEQ: case T_ADDEQ: case T_SUBEQ: case T_SHLEQ:
    case T_SHREQ: case T_ANDEQ: case T_XOREQ: case T_OREQ:
        return 1;
    }
    return 0;
}

/* C++: the expression being read is bound to a reference, so of "c ? a : b" with two objects it is the
   object that is wanted. Only the outermost conditional (and those that are its arms) sees it set. */
static int ref_bind;

static void cond_expr(Expr *e)
{
    Expr a, b, la, lb;
    Type *t;
    u32 c, loc, tb, eb, end, ta, tbend, slot = 0, saved;
    u64 cv;
    int is_const, kc, want = ref_bind;

    ref_bind = 0;
    binary(e, 1);
    if (tok.kind != T_QUESTION)
        return;
    loc = tok.loc;
    next();
    rvalue(e);
    a = *e;
    kc = e->konst;
    c = to_bool(e);
    is_const = ir_is_const(F, c, &cv);
    saved = F->cur;
    tb = ir_new_block(F);
    eb = ir_new_block(F);
    end = ir_new_block(F);
    if (is_const) {
        if (!cv)
            F->cur = 0;
    } else {
        ir_br(F, c, tb, eb);
        F->cur = tb;
    }
    la.lval = 0;
    if (tok.kind != T_COLON) { /* GNU: "a ?: b" is a if a is true */
        ref_bind = want;
        expr(&a);
        ref_bind = 0;
        la = a;
        rvalue(&a);
    }
    ta = F->cur;
    expect(T_COLON);
    if (is_const)
        F->cur = cv ? 0 : saved;
    else
        F->cur = eb;
    ref_bind = want;
    cond_expr(&b);
    lb = b;
    rvalue(&b);
    tbend = F->cur;
    if (opt_cxx && la.lval && lb.lval && !la.bitwidth && !lb.bitwidth && la.ty->kind != TY_ARRAY &&
        la.ty->kind != TY_FUNC && type_compatible(unqual(la.ty), unqual(lb.ty)) &&
        (want || is_assign_op(tok.kind) || (tok.kind == T_RPAREN && is_assign_op(peek()->kind)))) {
        /* C++: assigned to, so it is the object that is chosen, not its value */
        if (is_const) {
            F->cur = cv ? ta : tbend;
            *e = cv ? la : lb;
            return;
        }
        slot = new_temp(type_ptr(la.ty));
        F->cur = ta;
        ir_store(F, M_64, slot, la.v);
        ir_jmp(F, end);
        F->cur = tbend;
        ir_store(F, M_64, slot, lb.v);
        ir_jmp(F, end);
        F->cur = end;
        set_expr(e, la.ty, ir_load(F, IT_I64, M_64, slot), 1);
        return;
    }

    if (type_is_arith(a.ty) && type_is_arith(b.ty))
        t = type_usual(a.ty, b.ty);
    else if (a.ty->kind == TY_VOID || b.ty->kind == TY_VOID)
        t = ty_void;
    else if (a.ty->kind == TY_PTR && is_null_const(&b))
        t = a.ty;
    else if (b.ty->kind == TY_PTR && is_null_const(&a))
        t = b.ty;
    else if (a.ty->kind == TY_PTR && b.ty->kind == TY_PTR)
        t = a.ty->base->kind == TY_VOID ? a.ty : b.ty->base->kind == TY_VOID ? b.ty : a.ty;
    else if (type_is_record(a.ty) && type_is_record(b.ty) && a.ty->canon == b.ty->canon)
        t = a.ty;
    else {
        error_at(loc, "incompatible operands to '?:'");
        return;
    }
    if (is_const) {
        /* only one arm is live; convert it where it was evaluated */
        F->cur = cv ? ta : tbend;
        if (cv) {
            convert(&a, t);
            *e = a;
        } else {
            convert(&b, t);
            *e = b;
        }
        if (cv)
            F->cur = ta;
        e->konst = (u8)(kc && a.konst && b.konst);
        return;
    }
    if (t->kind != TY_VOID)
        slot = new_temp(type_is_record(t) ? type_ptr(t) : t);
    F->cur = ta;
    convert(&a, t);
    if (slot) {
        if (type_is_record(t))
            ir_store(F, M_64, slot, a.v);
        else
            store_val(t, slot, a.v);
        if (type_is_128(t))
            ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, slot, ir_const(F, IT_I64, 8)), a.hi);
    }
    ir_jmp(F, end);
    F->cur = tbend;
    convert(&b, t);
    if (slot) {
        if (type_is_record(t))
            ir_store(F, M_64, slot, b.v);
        else
            store_val(t, slot, b.v);
        if (type_is_128(t))
            ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, slot, ir_const(F, IT_I64, 8)), b.hi);
    }
    ir_jmp(F, end);
    F->cur = end;
    if (slot)
        set_expr(e, t, type_is_record(t) ? ir_load(F, IT_I64, M_64, slot) : load_val(t, slot), 0);
    else
        set_expr(e, t, 0, 0);
    if (slot && type_is_128(t))
        e->hi = ir_load(F, IT_I64, M_64, ir_ins(F, OP_ADD, IT_I64, slot, ir_const(F, IT_I64, 8)));
}

static void assign_expr(Expr *e)
{
    int op;
    u32 loc;
    Expr r, lv;
    cond_expr(e);
    loc = tok.loc;
    switch (tok.kind) {
    case T_ASSIGN:
        next();
        if (opt_cxx && tok.kind == T_LBRACE && e->lval)
            compound_literal(&r, unqual(e->ty)); /* x = {...}: a value of x's type */
        else
            assign_expr(&r);
        rvalue(&r);
        assign(e, &r);
        return;
    case T_MULEQ: op = T_STAR; break;
    case T_DIVEQ: op = T_SLASH; break;
    case T_MODEQ: op = T_PERCENT; break;
    case T_ADDEQ: op = T_PLUS; break;
    case T_SUBEQ: op = T_MINUS; break;
    case T_SHLEQ: op = T_SHL; break;
    case T_SHREQ: op = T_SHR; break;
    case T_ANDEQ: op = T_AMP; break;
    case T_XOREQ: op = T_CARET; break;
    case T_OREQ: op = T_PIPE; break;
    default:
        return;
    }
    next();
    need_lvalue(e);
    if (atomic_lvalue(e)) {
        assign_expr(&r);
        rvalue(&r);
        atomic_rmw(e, op, &r, loc, 0);
        return;
    }
    lv = *e;
    rvalue(e);
    assign_expr(&r);
    rvalue(&r);
    binop(op, e, &r, loc);
    assign(&lv, e);
    *e = lv;
}

static void expr(Expr *e)
{
    assign_expr(e);
    while (accept(T_COMMA)) {
        assign_expr(e);
        rvalue(e);
        e->konst = 0;
    }
}

/* Constant expressions are evaluated by the IR folder. */
static i64 const_int_expr(void)
{
    Expr e;
    u64 v = 0;
    u32 loc = tok.loc, saved = F->cur;
    F->cur = 0;
    cond_expr(&e);
    rvalue(&e);
    F->cur = saved;
    if (!type_is_int(e.ty) || !ir_is_const(F, e.v, &v))
        error_at(loc, "integer constant expression required");
    return (i64)v;
}

/* ---- declarations ---- */

static Tag *find_tag(u32 name)
{
    Tag *t = name < names_cap ? tags[name] : 0;
    while (t && hide.n && hidden(t->level))
        t = t->shadow;
    return t;
}

static Tag *bind_tag(u32 name, Type *ty)
{
    Tag *t = (Tag *)arena_alloc(&A, sizeof(Tag));
    names_reserve(name);
    t->name = name;
    t->ty = ty;
    t->level = level;
    t->shadow = tags[name];
    tags[name] = t;
    t->scope_next = scopes[level].tags;
    scopes[level].tags = t;
    return t;
}

static void static_assert(void);
static void static_init(Sym *s);
static void ctor_decl(Type *t);
static Type *rec_resume; /* C++: the type whose body the next record_spec() reads */
static Type *record_spec(void);
static Type *inst_rec; /* C++: the type the next record specifier named inst_rec_name defines */
static u32 inst_rec_name;

/* C++: reads the constant initializer after an '='; 1 if it gave s a value known here. */
static int cv_parse(Sym *s, Type *ty)
{
    Expr e;
    u32 saved = F->cur;
    u64 iv = 0;
    double fv = 0;
    F->cur = 0;
    assign_expr(&e);
    rvalue(&e);
    if (type_is_scalar(ty) && type_is_scalar(e.ty))
        convert(&e, unqual(ty));
    F->cur = saved;
    if ((type_is_int(ty) && ir_is_const(F, e.v, &iv)) || (ty->kind == TY_PTR && ir_is_const(F, e.v, &iv) && !iv)) {
        s->cv = 1;
        s->val = (i64)iv;
        return 1;
    }
    if (type_is_float(ty) && ir_is_fconst(F, e.v, &fv)) {
        s->cv = 2;
        s->fval = fv;
        return 1;
    }
    return 0;
}

/* using namespace N;  using N::name;  using Name = type;   cls: the class it is a member of, if any */
static void cxx_using(void *cls)
{
    next();
    if (tok.kind == T_IDENT && peek()->kind == T_ASSIGN) {
        u32 name = tok.v;
        Sym *s;
        next();
        next();
        s = new_sym(name, SK_TYPEDEF, type_name());
        bind(s);
        if (cls)
            cs_add(cls, s);
    } else {
        while (tok.kind != T_SEMI && tok.kind != T_EOF)
            next();
    }
    expect(T_SEMI);
}

/* C++: the static members and member types that follow these declaration specifiers. */
static void class_statics(void *cls, Type *base, int storage)
{
    Attr at0 = ds_attr;
    int is_cx = ds_constexpr;
    for (;;) {
        u32 name = 0, loc = tok.loc;
        Type *ty;
        Sym *s;
        memset(&dcl_attr, 0, sizeof dcl_attr);
        last_pdef = 0;
        ty = declarator(base, &name);
        if (!name)
            error_at(loc, "expected a member name");
        if (ty->kind == TY_FUNC && storage != KW(KW_TYPEDEF)) {
            /* a static member function: a function of its own, which sees the other members */
            Sym *old = cs_find(cls, name);
            Attr at = at0;
            char nm[300];
            u32 tag = ((Type *)cls)->tag;
            sprintf(nm, "%.120s_c%u_%.120s", tag ? intern_str(tag) : "anon", cs_get(cls, 1)->id, intern_str(name));
            at.flags |= dcl_attr.flags;
            decl_suffix(&at);
            s = new_sym(name, SK_VAR, ty);
            s->asmname = mg_func(intern_cstr(nm), 0, ty);
            s->is_static = 1;
            s->is_cx = (u8)is_cx;
            s->itm = cinst_t;
            s->iti = cinst_ti;
            s->pdef = last_pdef;
            s->npdef = last_npdef;
            last_pdef = 0;
            sym_attrs(s, &at, 1, 0);
            if (old && old->kind == SK_VAR && old->ty->kind == TY_FUNC)
                s->over = old;
            bind(s);
            cs_add(cls, s);
            if (tok.kind == T_LBRACE) {
                Pend *pd;
                u32 nb;
                Token *body = grab_balanced(&nb);
                if (is_cx) {
                    s->body = body;
                    s->nbody = nb;
                }
                pd = pend_add(cinst_t, cinst_ti, 0, s);
                pd->mbody = body;
                pd->nmbody = nb;
                return;
            }
            if (!accept(T_COMMA))
                break;
            continue;
        }
        if (storage == KW(KW_TYPEDEF)) {
            s = new_sym(name, SK_TYPEDEF, ty);
        } else {
            s = new_sym(name, SK_VAR, ty);
            if (tok.kind != T_ASSIGN)
                error_at(loc, "a static member needs its value in the class");
            if (type_is_scalar(ty)) {
                next();
                if (!cv_parse(s, ty))
                    error_at(loc, "the value of a static member must be a constant");
                s->ty = unqual(ty);
            } else {
                s->is_static = 1;
                s->objsym = obj_add_sym(O, 0, SEC_UNDEF, 0, 0);
                sym_align(s->objsym, ty);
                next();
                static_init(s);
            }
        }
        bind(s);
        cs_add(cls, s);
        if (!accept(T_COMMA))
            break;
    }
    expect(T_SEMI);
}

static Type *record_spec(void)
{
    int kind = tok.kind == KW(KW_STRUCT) ? TY_STRUCT : TY_UNION;
    u32 name = 0, loc = tok.loc;
    Tag *tag = 0;
    Type *t;
    Field *head = 0, **tail = &head;
    Attr at;

    memset(&at, 0, sizeof at);
    if (rec_resume) {
        /* C++: the body of an explicit specialization, whose head has been read */
        t = rec_resume;
        rec_resume = 0;
        kind = t->kind;
        name = t->tag;
        goto body;
    }
    next();
    attributes(&at);
    if (tok.kind == T_IDENT && inst_rec && tok.v == inst_rec_name) {
        /* C++: the class a template is being instantiated as */
        name = tok.v;
        next();
        t = inst_rec;
        inst_rec = 0;
    } else if (tok.kind == T_IDENT) {
        name = tok.v;
        next();
        tag = find_tag(name);
        if (tok.kind == T_LBRACE || tok.kind == T_SEMI) {
            if (tag && tag->level != level)
                tag = 0; /* a new type in this scope */
        }
        if (!tag)
            tag = bind_tag(name, type_record(kind, name));
        t = tag->ty;
        if (t->kind != kind && !opt_cxx)
            error_at(loc, "'%s' declared as a different kind of tag", intern_str(name));
    } else {
        t = type_record(kind, 0);
        if (tok.kind != T_LBRACE)
            error_at(loc, "expected a tag or '{'");
    }
body:
    if (!accept(T_LBRACE))
        return t;
    if (t->complete)
        error_at(loc, "redefinition of '%s'", intern_str(name));
    if (opt_cxx)
        push_scope(); /* where the members that are constants and types can be seen by the rest of the body */
    while (tok.kind != T_RBRACE) {
        Type *base;
        Attr fa0;
        int storage = 0;
        if (accept(T_SEMI))
            continue;
        if (tok.kind == KW(KW_STATIC_ASSERT)) {
            static_assert();
            continue;
        }
        if (opt_cxx && tok.kind == T_IDENT && !lookup(tok.v)) {
            if ((tok.v == id_public || tok.v == id_private || tok.v == id_protected) && peek()->kind == T_COLON) {
                next();
                next();
                continue;
            }
            if (tok.v == id_using) {
                cxx_using(t->canon);
                continue;
            }
        }
        base = opt_cxx ? declspec(&storage) : declspec(0);
        fa0 = ds_attr;
        if (opt_cxx && (storage || ds_constexpr)) {
            class_statics(t->canon, base, storage);
            continue;
        }
        if (opt_cxx && tok.kind == T_LPAREN && type_is_record(base) && base->canon == t->canon) {
            ctor_decl(t);
            continue;
        }
        for (;;) {
            Field *f = (Field *)arena_alloc(&A, sizeof(Field));
            u32 fname = 0;
            Type *ft = base;
            Attr fa = fa0;
            if (tok.kind != T_COLON && tok.kind != T_SEMI)
                ft = declarator(base, &fname);
            else if (tok.kind == T_SEMI && !type_is_record(base) && !opt_cxx)
                error_at(tok.loc, "declaration declares no member");
            else if (opt_cxx && tok.kind == T_SEMI && !(type_is_record(base) && !base->canon->tag))
                break; /* a nested type or an enumeration, not a member */
            if (opt_cxx && (ft->kind == TY_FUNC || tok.kind == T_ASSIGN || tok.kind == T_LBRACE))
                error_at(tok.loc, "C++ member functions and member initializers are not supported");
            decl_suffix(&fa);
            f->name = fname;
            if (accept(T_COLON)) {
                i64 w = const_int_expr();
                if (!type_is_int(ft) || w < 0 || (u64)w > type_size(ft) * 8)
                    error_at(tok.loc, "invalid bit-field");
                if (w == 0)
                    f->bitoff = 255;
                f->bitwidth = (u8)w;
                decl_suffix(&fa);
            } else if (ft->kind == TY_ARRAY && !ft->complete && type_complete(ft->base)) {
                ; /* flexible array member: no size of its own */
            } else if (!type_complete(ft) || ft->kind == TY_FUNC) {
                error_at(tok.loc, "member has an incomplete type");
            }
            f->ty = fa.mode ? attr_type(ft, &fa) : ft;
            f->packed = (fa.flags & AT_PACKED) != 0;
            f->align = fa.align;
            *tail = f;
            tail = &f->next;
            if (!accept(T_COMMA))
                break;
        }
        expect(T_SEMI);
    }
    next();
    if (opt_cxx) {
        Tag *nt;
        for (nt = scopes[level].tags; nt; nt = nt->scope_next)
            if (nt->name && !cs_find(t->canon, nt->name))
                cs_add(t->canon, new_sym(nt->name, SK_TYPEDEF, nt->ty)); /* Class::Nested */
        pop_scope();
    }
    attributes(&at);
    type_layout(t, head, (at.flags & AT_PACKED) != 0, at.align);
    if (at.flags & AT_GUARD)
        t->canon->guard = 1;
    return t;
}

static Type *enum_spec(void)
{
    i64 val = 0;
    Attr at;
    Tag *tag = 0;
    Type *base = ty_int;
    int scoped = 0;
    memset(&at, 0, sizeof at);
    next();
    attributes(&at);
    if (opt_cxx && (tok.kind == KW(KW_STRUCT) || (tok.kind == T_IDENT && tok.v == id_class && !lookup(tok.v)))) {
        scoped = 1; /* enum class: the enumerators are reached through the type's name only */
        next();
    }
    if (tok.kind == T_IDENT) {
        tag = find_tag(tok.v);
        if (!tag || (opt_cxx && tag->level != level))
            tag = bind_tag(tok.v, ty_int);
        next();
    }
    if (opt_cxx && accept(T_COLON)) {
        base = unqual(declspec(0));
        if (!type_is_int(base))
            error_at(tok.loc, "the type under an enumeration must be an integer type");
        if (tag)
            tag->ty = base;
    } else if (tag) {
        base = tag->ty;
    }
    if (!accept(T_LBRACE))
        return base;
    while (tok.kind != T_RBRACE) {
        Sym *s;
        if (tok.kind != T_IDENT)
            error_at(tok.loc, "expected enumerator name");
        s = new_sym(tok.v, SK_ENUM, base);
        next();
        attributes(&at);
        if (accept(T_ASSIGN))
            val = const_int_expr();
        if (base == ty_int && val != (i64)(i32)val)
            s->ty = val == (i64)(u32)val ? ty_uint : ty_long; /* GNU: a constant too big for int */
        s->val = val++;
        if (!scoped)
            bind(s);
        if (opt_cxx && tag)
            cs_add(tag, s);
        if (!accept(T_COMMA))
            break;
    }
    expect(T_RBRACE);
    attributes(&at);
    return base;
}

static int ds_auto; /* set by declspec: the declared type comes from the initializer */
static int ds_constexpr; /* set by declspec: C++ constexpr */
static Sym *cxx_member; /* set by cxx_named_type: the name ended in this member, which is not a type */
static Type *tmpl_class_use(Sym *s);

/* C++: the type a name stands for: a typedef, a class or enumeration by its tag, a class template with its
   arguments, then any members after "::". Returns 0, having read nothing, if the identifier is not a type. */
static Type *cxx_named_type(void)
{
    Sym *s;
    Tag *tag;
    Type *ty;
    void *key = 0;
    u32 loc = tok.loc;
    cxx_member = 0;
    if (tok.kind != T_IDENT)
        return 0;
    s = lookup(tok.v);
    if (!s && tok.v == id_typename) {
        next();
        if (tok.kind != T_IDENT)
            error_at(loc, "expected a type name");
        s = lookup(tok.v);
    }
    if (!s && tok.v == id_decltype) {
        Expr e;
        u32 saved = F->cur;
        next();
        expect(T_LPAREN);
        F->cur = 0; /* the operand is not evaluated */
        expr(&e);
        F->cur = saved;
        expect(T_RPAREN);
        ty = e.ty;
    } else if (s && s->kind == SK_TYPEDEF) {
        ty = s->ty;
        next();
    } else if (s && s->kind == SK_TEMPLATE && s->is_ctmpl) {
        next();
        ty = tmpl_class_use(s);
    } else if (!s && (tag = find_tag(tok.v)) != 0) {
        ty = tag->ty;
        if (!type_is_record(ty))
            key = tag;
        next();
    } else {
        return 0;
    }
    while (tok.kind == T_SCOPE) {
        Sym *m;
        next();
        if (tok.kind != T_IDENT)
            error_at(tok.loc, "expected a member name after '::'");
        if (!key && type_is_record(ty))
            key = ty->canon;
        m = key ? cs_find(key, tok.v) : 0;
        if (!m)
            error_at(tok.loc, "the type has no member '%s'", intern_str(tok.v));
        next();
        if (m->kind != SK_TYPEDEF) {
            cxx_member = m;
            break;
        }
        ty = m->ty;
        key = 0;
    }
    return ty;
}

/* The type of the initializer that follows the current '=', which stays the current token. */
static Type *auto_type(void)
{
    Expr e;
    u32 saved = F->cur, n;
    Token *ct;
    if (tok.kind != T_ASSIGN)
        error_at(tok.loc, "a declaration with an inferred type needs an initializer");
    rec_start();
    next();
    F->cur = 0; /* a dry run; the tokens are read again for the real thing */
    assign_expr(&e);
    rvalue(&e);
    F->cur = saved;
    n = nrec - 1 - (has_ptok ? 1 : 0);
    ct = rec_stop(n);
    replay(ct, n);
    return unqual(e.ty);
}

static Type *declspec(int *storage)
{
    Type *ty = 0;
    int quals = 0, seen = 0;
    int n_void = 0, n_char = 0, n_short = 0, n_int = 0, n_long = 0, n_float = 0, n_double = 0, n_signed = 0,
        n_unsigned = 0, n_bool = 0, n_128 = 0, is_inline = 0, is_auto = 0, is_complex = 0, is_cx = 0, tls = 0;
    u32 loc = tok.loc;
    Attr at;

    memset(&at, 0, sizeof at);
    if (storage)
        *storage = 0;
    for (;;) {
        switch (tok.kind) {
        case KW(KW_EXTENSION): case KW(KW_RESTRICT): next(); continue;
        case KW(KW_INLINE): is_inline = 1; next(); continue;
        case KW(KW_NORETURN): at.flags |= AT_NORETURN; next(); continue;
        case KW(KW_ATTRIBUTE): attributes(&at); continue;
        case KW(KW_BOOL): n_bool++; break;
        case KW(KW_INT128): n_128++; break;
        case KW(KW_AUTO_TYPE): is_auto = 1; next(); continue;
        case KW(KW_TYPEOF): {
            if (seen)
                error_at(tok.loc, "invalid type specifier");
            next();
            expect(T_LPAREN);
            if (is_typename(&tok)) {
                ty = type_name();
            } else {
                Expr e;
                u32 saved = F->cur;
                F->cur = 0; /* the operand is not evaluated */
                expr(&e);
                F->cur = saved;
                ty = e.ty;
            }
            expect(T_RPAREN);
            seen = 1;
            continue;
        }
        case KW(KW_TYPEDEF): case KW(KW_EXTERN): case KW(KW_STATIC): case KW(KW_AUTO): case KW(KW_REGISTER):
            if (!storage || *storage)
                error_at(tok.loc, "unexpected storage class");
            *storage = tok.kind;
            next();
            continue;
        case KW(KW_CONST): quals |= Q_CONST; next(); continue;
        case KW(KW_VOLATILE): quals |= Q_VOLATILE; next(); continue;
        case KW(KW_THREAD_LOCAL): tls = 1; next(); continue;
        case KW(KW_ATOMIC):
            next();
            if (tok.kind == T_LPAREN) { /* _Atomic(type) */
                if (seen)
                    error_at(tok.loc, "invalid type specifier");
                next();
                ty = type_qual(type_name(), Q_ATOMIC);
                expect(T_RPAREN);
                seen = 1;
            } else {
                quals |= Q_ATOMIC;
            }
            continue;
        case KW(KW_VOID): n_void++; break;
        case KW(KW_CHAR): n_char++; break;
        case KW(KW_SHORT): n_short++; break;
        case KW(KW_INT): n_int++; break;
        case KW(KW_LONG): n_long++; break;
        case KW(KW_FLOAT): n_float++; break;
        case KW(KW_DOUBLE): n_double++; break;
        case KW(KW_SIGNED): n_signed++; break;
        case KW(KW_UNSIGNED): n_unsigned++; break;
        case KW(KW_STRUCT):
        case KW(KW_UNION):
            if (seen)
                error_at(tok.loc, "invalid type specifier");
            ty = record_spec();
            seen = 1;
            continue;
        case KW(KW_ENUM):
            if (seen)
                error_at(tok.loc, "invalid type specifier");
            ty = enum_spec();
            seen = 1;
            continue;
        case T_IDENT: {
            Sym *s = lookup(tok.v);
            if (!s && tok.v == id_complex) {
                is_complex = 1;
                next();
                continue;
            }
            if (!s && (tok.v == id_alignas || tok.v == id_alignas2)) {
                u32 al;
                next();
                expect(T_LPAREN);
                if (is_typename(&tok))
                    al = type_align(type_name());
                else
                    al = (u32)const_int_expr();
                expect(T_RPAREN);
                if (al > at.align)
                    at.align = al;
                continue;
            }
            if (opt_cxx && !s) {
                if (tok.v == id_constexpr) {
                    is_cx = 1;
                    quals |= Q_CONST;
                    next();
                    continue;
                }
                if (tok.v == id_mutable || tok.v == id_explicit || tok.v == id_friend || tok.v == id_virtual) {
                    next();
                    continue;
                }
                if (tok.v == id_class) {
                    tok.kind = KW(KW_STRUCT);
                    continue;
                }
            }
            if (seen)
                goto done;
            if (opt_cxx) {
                Type *nt = cxx_named_type();
                if (nt) {
                    if (cxx_member)
                        error_at(loc, "'%s' is not a type", intern_str(cxx_member->name));
                    ty = nt;
                    seen = 1;
                    continue;
                }
            }
            if (!s && (tok.v == id_float16 || tok.v == id_bf16)) {
                ty = tok.v == id_float16 ? ty_float16 : ty_bf16;
                seen = 1;
                next();
                continue;
            }
            if (!s || s->kind != SK_TYPEDEF)
                goto done;
            ty = s->ty;
            seen = 1;
            next();
            continue;
        }
        default:
            goto done;
        }
        if (ty)
            error_at(tok.loc, "invalid type specifier");
        seen = 1;
        next();
    }
done:
    /* C23: auto with no type specifier asks for the type of the initializer */
    if ((opt_c23 || opt_cxx) && storage && *storage == KW(KW_AUTO) && !ty &&
        !(n_void + n_char + n_short + n_int + n_long + n_float + n_double + n_signed + n_unsigned + n_bool + n_128)) {
        is_auto = 1;
        *storage = 0;
    }
    ds_auto = is_auto;
    if (!ty) {
        if (n_void + n_char + n_short + n_float + n_double + n_bool > 1 || n_long > 2 || n_int > 1 ||
            (n_signed && n_unsigned) || ((n_void || n_float || n_double || n_bool) && (n_signed || n_unsigned)) ||
            ((n_void || n_float || n_char || n_bool) && (n_long || n_short)) || (n_short && n_long) ||
            (n_double && (n_short || n_long > 1)))
            error_at(loc, "invalid combination of type specifiers");
        if (n_void)
            ty = ty_void;
        else if (n_128)
            ty = n_unsigned ? ty_uint128 : ty_int128;
        else if (n_bool)
            ty = ty_bool;
        else if (n_long > 1)
            ty = n_unsigned ? ty_ullong : ty_llong;
        else if (n_char)
            ty = n_unsigned ? ty_uchar : n_signed ? ty_schar : ty_char;
        else if (n_short)
            ty = n_unsigned ? ty_ushort : ty_short;
        else if (n_float)
            ty = ty_float;
        else if (n_double)
            ty = n_long ? ty_ldouble : ty_double;
        else if (n_long)
            ty = n_unsigned ? ty_ulong : ty_long;
        else
            ty = n_unsigned ? ty_uint : ty_int; /* includes the implicit int */
    }
    if (is_complex) {
        if (!type_is_float(ty) || ty->kind <= TY_FLOAT16)
            error_at(loc, "invalid complex type");
        ty = type_vector(ty, 2, 2);
    }
    if (at.mode || at.align || at.vecsize)
        ty = attr_type(ty, &at);
    ds_attr = at;
    ds_inline = is_inline;
    ds_tls = tls;
    ds_constexpr = is_cx;
    return quals ? type_qual(ty, quals) : ty;
}

/* Declarator pieces, in the order they apply to the base type. */
enum { D_PTR, D_ARRAY, D_FUNC, D_REF };

typedef struct DOp {
    u8 kind;
    u8 quals;
    u8 complete, variadic, oldstyle;
    u64 len;
    Type **params;
    u32 *pnames;
    u32 nparams;
    u32 vla; /* the length of an array, when it is not a constant */
    struct DOp *next;
} DOp;

static u32 vla_len; /* set by declarator: the outermost array's length, if not constant */

static int explicit_inst, spec_fn, has_decl_targs;
static TArgs decl_targs;
static Sym *decl_member; /* the declarator named this member of a class */

static int decl_qualified(void)
{
    Sym *s = lookup(tok.v);
    int k = peek()->kind;
    if (s && s->kind == SK_TEMPLATE && s->is_ctmpl)
        return k == T_LT;
    if (k != T_SCOPE)
        return 0;
    return s ? s->kind == SK_TYPEDEF : find_tag(tok.v) != 0;
}
static Token **last_pdef; /* C++: the default arguments of the last parameter list that had any */
static u32 *last_npdef;

static void param_list(DOp *op)
{
    Type *ptypes[128];
    u32 pnames[128], n = 0;
    Token **pd = 0;
    u32 *npd = 0;

    op->kind = D_FUNC;
    if (tok.kind == T_RPAREN) {
        op->oldstyle = (u8)!opt_cxx;
    } else if (tok.kind == KW(KW_VOID) && peek()->kind == T_RPAREN) {
        next();
    } else if (tok.kind == T_IDENT && !is_typename(&tok)) {
        op->oldstyle = 1; /* K&R identifier list */
        for (;;) {
            if (tok.kind != T_IDENT)
                error_at(tok.loc, "expected parameter name");
            if (n >= 127)
                error_at(tok.loc, "too many parameters");
            ptypes[n] = ty_int;
            pnames[n++] = tok.v;
            next();
            if (!accept(T_COMMA))
                break;
        }
    } else {
        for (;;) {
            Type *t;
            u32 name = 0;
            int storage;
            if (accept(T_ELLIPSIS)) {
                op->variadic = 1;
                break;
            }
            if (n >= 127)
                error_at(tok.loc, "too many parameters");
            t = declspec(&storage);
            t = declarator(t, &name);
            {
                Attr pa;
                memset(&pa, 0, sizeof pa);
                decl_suffix(&pa);
            }
            if (t->kind == TY_ARRAY)
                t = type_ptr(t->base);
            else if (t->kind == TY_FUNC)
                t = type_ptr(t);
            else if (t->kind == TY_VOID)
                error_at(tok.loc, "parameter has type void");
            if (opt_cxx && accept(T_ASSIGN)) {
                /* a default argument: kept as its tokens */
                if (!pd) {
                    pd = (Token **)arena_alloc(&A, 128 * sizeof(Token *));
                    npd = (u32 *)arena_alloc(&A, 128 * sizeof(u32));
                }
                npd[n] = skim(&pd[n]);
                pd[n] = with_semi(pd[n], npd[n]);
            }
            ptypes[n] = t;
            pnames[n++] = name;
            if (!accept(T_COMMA))
                break;
        }
    }
    expect(T_RPAREN);
    while (opt_cxx && tok.kind == T_IDENT && (tok.v == id_noexcept || tok.v == id_throw) && !lookup(tok.v)) {
        /* noexcept, noexcept(condition), throw(): nothing is thrown here either way */
        next();
        if (tok.kind == T_LPAREN) {
            int depth = 0;
            do {
                if (tok.kind == T_EOF)
                    error_at(tok.loc, "expected ')'");
                depth += tok.kind == T_LPAREN ? 1 : tok.kind == T_RPAREN ? -1 : 0;
                next();
            } while (depth);
        }
    }
    if (pd) {
        last_pdef = pd;
        last_npdef = npd;
    }
    op->nparams = n;
    if (n) {
        op->params = (Type **)arena_alloc(&A, n * sizeof(Type *));
        op->pnames = (u32 *)arena_alloc(&A, n * sizeof(u32));
        memcpy(op->params, ptypes, n * sizeof(Type *));
        memcpy(op->pnames, pnames, n * sizeof(u32));
    }
}

/* An array's length. In a function body it need not be constant: *vla is then the
   value, and the array is one whose size is found when the declaration is reached. */
static i64 array_len(u32 *vla)
{
    Expr e;
    u64 v = 0;
    u32 loc = tok.loc;
    if (!F->cur || !level || !opt_gnu)
        return const_int_expr();
    cond_expr(&e);
    rvalue(&e);
    if (!type_is_int(e.ty))
        error_at(loc, "integer constant expression required");
    if (ir_is_const(F, e.v, &v))
        return (i64)v;
    convert(&e, ty_ulong);
    *vla = e.v;
    return 0;
}

static DOp *declarator_ops(u32 *name)
{
    DOp *head = 0, **tail = &head, *suffix = 0, *inner = 0, *op;

    while (tok.kind == T_STAR || (opt_cxx && (tok.kind == T_AMP || tok.kind == T_ANDAND))) {
        op = (DOp *)arena_alloc(&A, sizeof(DOp));
        op->kind = tok.kind == T_STAR ? D_PTR : D_REF;
        next();
        for (;;) {
            if (accept(KW(KW_CONST)))
                op->quals |= Q_CONST;
            else if (accept(KW(KW_VOLATILE)))
                op->quals |= Q_VOLATILE;
            else if (accept(KW(KW_ATOMIC)))
                op->quals |= Q_ATOMIC;
            else if (accept(KW(KW_RESTRICT)))
                ;
            else if (tok.kind == KW(KW_ATTRIBUTE))
                attributes(&dcl_attr); /* "void *__section(x) p": they are the variable's */
            else
                break;
        }
        *tail = op;
        tail = &op->next;
    }
    if (tok.kind == KW(KW_ATTRIBUTE))
        attributes(&dcl_attr);
    if (tok.kind == T_LPAREN) {
        Token *p = peek();
        if (p->kind == T_STAR || p->kind == T_LPAREN || p->kind == T_LBRACK || p->kind == KW(KW_ATTRIBUTE) ||
            (opt_cxx && (p->kind == T_AMP || p->kind == T_ANDAND)) || (p->kind == T_IDENT && !is_typename(p))) {
            next();
            inner = declarator_ops(name);
            expect(T_RPAREN);
        }
    } else if (tok.kind == T_IDENT && opt_cxx && decl_qualified()) {
        /* Class::member or Class<args>::member, defined outside its class */
        cxx_named_type();
        if (!cxx_member)
            error_at(tok.loc, "expected the name of a member");
        decl_member = cxx_member;
        cxx_member = 0;
        *name = decl_member->name;
    } else if (tok.kind == T_IDENT) {
        *name = tok.v;
        next();
        if (opt_cxx && tok.kind == T_LT && (explicit_inst || spec_fn)) {
            parse_targs(&decl_targs);
            has_decl_targs = 1;
        }
    }
    for (;;) {
        if (accept(T_LBRACK)) {
            op = (DOp *)arena_alloc(&A, sizeof(DOp));
            op->kind = D_ARRAY;
            while (tok.kind == KW(KW_STATIC) || tok.kind == KW(KW_CONST) || tok.kind == KW(KW_VOLATILE) ||
                   tok.kind == KW(KW_RESTRICT))
                next(); /* C99, in parameters */
            if (tok.kind != T_RBRACK) {
                i64 n = array_len(&op->vla);
                if (n < 0)
                    error_at(tok.loc, "array size is negative");
                op->len = (u64)n;
                op->complete = !op->vla;
            }
            expect(T_RBRACK);
        } else if (tok.kind == T_LPAREN) {
            if (opt_cxx && level > 0 && !inner && !suffix && F != const_func) {
                Token *pk = peek();
                if (!is_typename(pk) && pk->kind != T_RPAREN && pk->kind != T_ELLIPSIS)
                    break; /* C++: "type name(arguments)" gives the variable its value */
            }
            next();
            op = (DOp *)arena_alloc(&A, sizeof(DOp));
            param_list(op);
        } else {
            break;
        }
        op->next = suffix; /* suffixes apply right to left */
        suffix = op;
    }
    *tail = suffix;
    while (*tail)
        tail = &(*tail)->next;
    *tail = inner;
    return head;
}

static Type *declarator(Type *base, u32 *name)
{
    DOp *op = declarator_ops(name);
    vla_len = 0;
    for (; op; op = op->next) {
        vla_len = op->kind == D_ARRAY ? op->vla : 0;
        switch (op->kind) {
        case D_PTR:
            base = type_ptr(base);
            if (op->quals)
                base = type_qual(base, op->quals);
            break;
        case D_REF:
            if (base->kind != TY_REF)
                base = type_ref(base);
            break;
        case D_ARRAY:
            if (base->kind == TY_FUNC || (!type_complete(base) && !(base->kind == TY_ARRAY && base->vslot)))
                error_at(tok.loc, "array of an incomplete or function type");
            if ((op->vla || (base->kind == TY_ARRAY && base->vslot && op->complete)) && F->cur && level > 0) {
                /* a length known only when the program runs: the type's size is kept in a slot */
                u32 esz = base->kind == TY_ARRAY && base->vslot
                              ? ir_load(F, IT_I64, M_64, ir_slot_addr(F, base->vslot - 1, 0))
                              : ir_const(F, IT_I64, type_size(base));
                u32 vs = ir_new_slot(F, 8, 8);
                ir_store(F, M_64, ir_slot_addr(F, vs, 0),
                         ir_ins(F, OP_MUL, IT_I64, op->vla ? op->vla : ir_const(F, IT_I64, op->len), esz));
                base = type_array(base, 0, 0);
                base->vslot = vs + 1;
            } else {
                base = type_array(base, op->len, op->complete);
            }
            break;
        default:
            if (base->kind == TY_FUNC || base->kind == TY_ARRAY)
                error_at(tok.loc, "function returning a function or an array");
            base = type_func(base, op->params, op->pnames, op->nparams, op->variadic, op->oldstyle);
            break;
        }
    }
    return base;
}

static Type *type_name(void)
{
    u32 name = 0;
    Type *t = declspec(0);
    t = declarator(t, &name);
    if (name)
        error_at(tok.loc, "unexpected identifier in type name");
    if (tok.kind == KW(KW_ATTRIBUTE)) {
        Attr a;
        memset(&a, 0, sizeof a);
        attributes(&a);
        t = attr_type(t, &a);
    }
    return t;
}

/* ---- initializers ---- */

typedef struct InitReloc {
    u64 off;
    u32 sym;
    i64 addend;
} InitReloc;

typedef struct Init {
    int is_static;
    Buf buf;         /* static: the object's bytes */
    InitReloc *rel;  /* static: addresses to relocate */
    u32 nrel, caprel;
    u32 slot;        /* automatic: the object's slot */
    u64 max;         /* highest offset written, for incomplete arrays */
    Expr pending;    /* an expression parsed ahead of its use */
    int has_pending;
} Init;

static void init_reserve(Init *in, u64 end)
{
    if (end > in->max)
        in->max = end;
    if (in->is_static && end > in->buf.len)
        buf_zero(&in->buf, (size_t)(end - in->buf.len));
}

static void init_scalar(Init *in, Type *ty, u64 off, Field *bf)
{
    Expr e;
    u32 loc = tok.loc;
    int braced = 0;
    if (in->has_pending) {
        e = in->pending;
        in->has_pending = 0;
    } else {
        braced = accept(T_LBRACE);
        assign_expr(&e);
        rvalue(&e);
        if (braced) {
            accept(T_COMMA);
            expect(T_RBRACE);
        }
    }
    if (special_pair(ty, e.ty))
        convert(&e, ty);
    if (type_is_record(ty) != type_is_record(e.ty))
        error_at(loc, "incompatible initializer");
    init_reserve(in, off + type_size(ty));
    if (in->is_static && type_is_record(ty)) {
        /* a constant structure: the bytes of a compound literal */
        Ins *i = &F->ins[e.v];
        ObjSym *os = i->op == OP_ADDR ? &O->syms[i->a] : 0;
        u64 size = type_size(ty), src;
        u32 k, sec;
        if (!os || (os->sec != SEC_DATA && os->sec != SEC_CONST) || os->name)
            error_at(loc, "initializer is not a constant");
        sec = os->sec;
        src = os->value + (u64)(i64)(i32)i->b;
        memcpy(in->buf.p + off, O->sec[sec].p + src, (size_t)size);
        for (k = 0; k < O->nrel[sec]; k++) {
            ObjReloc *r = &O->rel[sec][k];
            if (r->off < src || r->off >= src + size)
                continue;
            if (in->nrel == in->caprel) {
                in->caprel = in->caprel ? in->caprel * 2 : 16;
                in->rel = (InitReloc *)xrealloc(in->rel, in->caprel * sizeof(InitReloc));
            }
            in->rel[in->nrel].off = off + (r->off - src);
            in->rel[in->nrel].sym = r->sym;
            in->rel[in->nrel].addend = r->addend;
            in->nrel++;
        }
        return;
    }
    if (!in->is_static) {
        Expr lv;
        set_expr(&lv, ty, ir_slot_addr(F, in->slot, (i64)off), 1);
        if (bf) {
            lv.bitoff = bf->bitoff;
            lv.bitwidth = bf->bitwidth;
        }
        if (!type_is_record(ty))
            convert(&e, ty);
        store_expr(&lv, &e);
        return;
    }
    convert(&e, ty);
    {
        Ins *i = &F->ins[e.v];
        u64 v = (u64)i->a | ((u64)i->b << 32);
        u8 *p = in->buf.p + off;
        u32 size = (u32)type_size(ty), k;
        for (k = 0; k < in->nrel; k++) /* a later initializer replaces an earlier one */
            if (in->rel[k].off + 8 > off && in->rel[k].off < off + size)
                in->rel[k--] = in->rel[--in->nrel];
        if (type_is_128(ty)) {
            u64 h;
            if (!ir_is_const(F, e.v, &v) || !ir_is_const(F, e.hi, &h))
                error_at(loc, "initializer is not a constant");
            for (k = 0; k < 8; k++) {
                p[k] = (u8)(v >> (8 * k));
                p[8 + k] = (u8)(h >> (8 * k));
            }
            return;
        }
        if (i->op == OP_ADDR) {
            if (size != 8 || bf)
                error_at(loc, "initializer is not a constant");
            if (in->nrel == in->caprel) {
                in->caprel = in->caprel ? in->caprel * 2 : 16;
                in->rel = (InitReloc *)xrealloc(in->rel, in->caprel * sizeof(InitReloc));
            }
            in->rel[in->nrel].off = off;
            in->rel[in->nrel].sym = i->a;
            in->rel[in->nrel].addend = (i32)i->b;
            in->nrel++;
            return;
        }
        if (i->op != OP_CONST && i->op != OP_FCONST)
            error_at(loc, "initializer is not a constant");
        if (bf) {
            u64 mask = (bf->bitwidth >= 64 ? ~(u64)0 : (((u64)1 << bf->bitwidth) - 1)) << bf->bitoff;
            u64 old = 0;
            for (k = 0; k < size; k++)
                old |= (u64)p[k] << (8 * k);
            v = (old & ~mask) | ((v << bf->bitoff) & mask);
        }
        for (k = 0; k < size; k++)
            p[k] = (u8)(v >> (8 * k));
    }
}

static int is_char_array(Type *t)
{
    return t->kind == TY_ARRAY && (t->base->kind == TY_CHAR || t->base->kind == TY_SCHAR || t->base->kind == TY_UCHAR);
}

static void init_string(Init *in, Type *ty, u64 off)
{
    Buf b;
    u64 n;
    memset(&b, 0, sizeof b);
    string_literal(&b);
    n = b.len;
    if (ty->complete) {
        if (n - 1 > ty->len)
            error_at(tok.loc, "string is too long for the array");
        if (n > ty->len)
            n = ty->len; /* the terminator may be dropped */
    }
    init_reserve(in, off + n);
    if (in->is_static)
        memcpy(in->buf.p + off, b.p, (size_t)n);
    else
        emit_mem(OP_MEMCPY, ir_slot_addr(F, in->slot, (i64)off), ir_addr(F, string_sym(b.p, b.len), 0), n, 1);
    buf_free(&b);
}

static void init_list(Init *in, Type *ty, u64 off, int braced);

static void init_elem(Init *in, Type *ty, u64 off, Field *bf)
{
    if ((ty->kind != TY_ARRAY && !type_is_record(ty)) || (VK(ty) == 2 && (in->has_pending || tok.kind != T_LBRACE))) {
        init_scalar(in, ty, off, bf); /* a complex number is a scalar */
        return;
    }
    if (!in->has_pending) {
        if (is_char_array(ty) && tok.kind == T_STR) {
            init_string(in, ty, off);
            return;
        }
        if (is_char_array(ty) && tok.kind == T_LBRACE && peek()->kind == T_STR) {
            next();
            init_string(in, ty, off);
            accept(T_COMMA);
            expect(T_RBRACE);
            return;
        }
        if (accept(T_LBRACE)) {
            init_list(in, ty, off, 1);
            return;
        }
        if (type_is_record(ty) && (!in->is_static || tok.kind == T_LPAREN)) {
            /* either a whole structure value or its first member */
            assign_expr(&in->pending);
            rvalue(&in->pending);
            in->has_pending = 1;
        }
    }
    if (in->has_pending && type_is_record(ty) && type_is_record(in->pending.ty) &&
        in->pending.ty->canon == ty->canon) {
        init_scalar(in, ty, off, 0);
        return;
    }
    init_list(in, ty, off, 0);
}

/* The rest of a designator after its first step: ".member" and "[index]"
   lead further into the object at *off of type *ty. */
static Field *designator_rest(Type **ty, u64 *off)
{
    Field *bf = 0;
    for (;;) {
        Type *t = *ty;
        if (accept(T_DOT)) {
            u64 o = 0;
            Field *f;
            if (tok.kind != T_IDENT || !type_is_record(t))
                error_at(tok.loc, "invalid designator");
            f = type_member(t, tok.v, &o);
            if (!f)
                error_at(tok.loc, "no member named '%s'", intern_str(tok.v));
            next();
            *off += o;
            *ty = f->ty;
            bf = f->bitwidth ? f : 0;
        } else if (accept(T_LBRACK)) {
            i64 i = const_int_expr();
            expect(T_RBRACK);
            if (t->kind != TY_ARRAY || i < 0)
                error_at(tok.loc, "invalid designator");
            *off += (u64)i * type_size(t->base);
            *ty = t->base;
            bf = 0;
        } else {
            return bf;
        }
    }
}

static void init_list(Init *in, Type *ty, u64 off, int braced)
{
    if (ty->kind == TY_ARRAY) {
        u64 i = 0, esize = type_size(ty->base), top = 0;
        for (;;) {
            if (tok.kind == T_RBRACE && !in->has_pending)
                break;
            if (!braced && !in->has_pending && (tok.kind == T_DOT || tok.kind == T_LBRACK))
                break; /* a designator for an enclosing object */
            if (braced && tok.kind == T_LBRACK) {
                /* [index] = value, [first ... last] = value */
                i64 lo, hi;
                Type *t = ty->base;
                u64 o;
                Field *bf;
                next();
                lo = hi = const_int_expr();
                if (accept(T_ELLIPSIS))
                    hi = const_int_expr();
                expect(T_RBRACK);
                if (lo < 0 || hi < lo || (ty->complete && (u64)hi >= ty->len))
                    error_at(tok.loc, "array index in initializer is out of range");
                o = off + (u64)lo * esize;
                bf = designator_rest(&t, &o);
                accept(T_ASSIGN);
                if (hi > lo) {
                    Token *toks;
                    u32 n = skim(&toks);
                    i64 k;
                    for (k = lo; k <= hi; k++) {
                        replay(toks, n);
                        init_elem(in, t, o + (u64)(k - lo) * esize, bf);
                    }
                } else {
                    init_elem(in, t, o, bf);
                }
                i = (u64)hi + 1;
                if (i > top)
                    top = i;
                if (!accept(T_COMMA))
                    break;
                continue;
            }
            if (ty->complete && i >= ty->len) {
                if (braced)
                    error_at(tok.loc, "too many initializers");
                break;
            }
            init_elem(in, ty->base, off + i * esize, 0);
            i++;
            if (i > top)
                top = i;
            if (!braced && ty->complete && i >= ty->len)
                break;
            if (!accept(T_COMMA))
                break;
        }
        if (!ty->complete)
            init_reserve(in, off + top * esize);
    } else {
        Field *f = ty->canon->fields;
        if (!type_complete(ty))
            error_at(tok.loc, "initializer for an incomplete type");
        for (;;) {
            while (f && !f->name && !type_is_record(f->ty))
                f = f->next; /* unnamed bit-fields are padding */
            if (tok.kind == T_RBRACE && !in->has_pending)
                break;
            if (!braced && !in->has_pending && (tok.kind == T_DOT || tok.kind == T_LBRACK))
                break;
            if (braced && !in->has_pending &&
                (tok.kind == T_DOT || (tok.kind == T_IDENT && peek()->kind == T_COLON))) {
                /* .member = value (or the old "member: value") */
                int old = tok.kind == T_IDENT;
                Type *t;
                u64 o = 0;
                Field *g, *bf, *top;
                if (!old)
                    next();
                if (tok.kind != T_IDENT)
                    error_at(tok.loc, "expected a member name");
                g = type_member(ty, tok.v, &o);
                if (!g)
                    error_at(tok.loc, "no member named '%s'", intern_str(tok.v));
                /* the member of this structure it is, or is inside of */
                for (top = ty->canon->fields; top; top = top->next) {
                    u64 dummy = 0;
                    if (top == g || (!top->name && type_is_record(top->ty) && type_member(top->ty, tok.v, &dummy)))
                        break;
                }
                next();
                t = g->ty;
                o += off;
                bf = g->bitwidth ? g : 0;
                if (old) {
                    expect(T_COLON);
                } else {
                    Field *b2 = designator_rest(&t, &o);
                    if (t != g->ty || b2)
                        bf = b2;
                    accept(T_ASSIGN);
                }
                init_elem(in, t, o, bf);
                f = ty->kind == TY_UNION || !top ? 0 : top->next;
                if (!accept(T_COMMA))
                    break;
                continue;
            }
            if (!f) {
                if (braced)
                    error_at(tok.loc, "too many initializers");
                break;
            }
            init_elem(in, f->ty, off + f->off, f->bitwidth ? f : 0);
            f = ty->kind == TY_UNION ? 0 : f->next;
            while (f && !f->name && !type_is_record(f->ty))
                f = f->next;
            if (!braced && !f)
                break;
            if (!accept(T_COMMA))
                break;
        }
    }
    if (braced)
        expect(T_RBRACE);
}

/* Parses "= initializer" for an object of type *typ (completing arrays). */
static void initializer(Init *in, Type **typ)
{
    Type *ty = *typ;
    in->max = 0;
    in->has_pending = 0;
    if (ty->kind == TY_ARRAY || type_is_record(ty)) {
        if (is_char_array(ty) && (tok.kind == T_STR || (tok.kind == T_LBRACE && peek()->kind == T_STR)))
            init_elem(in, ty, 0, 0);
        else if (is_char_array(ty) && tok.kind == T_LPAREN && peek()->kind == T_STR) {
            /* GNU: the string may stand in parentheses */
            next();
            init_elem(in, ty, 0, 0);
            expect(T_RPAREN);
        } else if (accept(T_LBRACE))
            init_list(in, ty, 0, 1);
        else if (type_is_record(ty))
            init_elem(in, ty, 0, 0);
        else
            error_at(tok.loc, "invalid initializer");
        if (ty->kind == TY_ARRAY && !ty->complete) {
            u64 esize = type_size(ty->base);
            *typ = type_array(ty->base, esize ? (in->max + esize - 1) / esize : 0, 1);
            (*typ)->quals = ty->quals;
        }
    } else {
        init_scalar(in, ty, 0, 0);
    }
}

/* ---- objects ---- */

/* Data with internal linkage that holds addresses is kept back until
   something refers to it: tables of functions that only dead code uses
   would otherwise keep those functions, and whatever they call, alive. */
typedef struct HeldData {
    u32 sym, nrel, align;
    int sec;
    u64 size;
    char *bytes;
    InitReloc *rel;
} HeldData;

static HeldData *held;
static u32 nheld, capheld;
static u32 *held_of; /* object symbol -> 1 + its place in held, 0 once emitted */
static u32 nheld_of;

static int held_emit(u32 sym)
{
    HeldData *h;
    u64 off;
    u32 k;
    if (sym >= nheld_of || !held_of[sym])
        return 0;
    h = &held[held_of[sym] - 1];
    held_of[sym] = 0;
    off = obj_reserve(O, h->sec, h->size, h->align);
    buf_put(&O->sec[h->sec], h->bytes, (size_t)h->size);
    for (k = 0; k < h->nrel; k++)
        obj_add_reloc(O, h->sec, (u32)(off + h->rel[k].off), R_ABS64, h->rel[k].sym, h->rel[k].addend);
    O->syms[sym].sec = (u16)h->sec;
    O->syms[sym].value = off;
    O->syms[sym].kind = 2;
    O->syms[sym].size = h->size;
    free(h->bytes);
    free(h->rel);
    return 1;
}

static void define_data(Sym *s, Init *in)
{
    u32 sym = sym_obj(s), k, section = s->section;
    u64 size = type_size(s->ty), off;
    Type *t = s->ty;
    int sec;
    sym_align(sym, s->ty); /* the type may have been completed since the symbol was made */
    if (s->is_tls) /* what every thread gets a copy of */
        section = intern_cstr(in ? ".tdata" : ".tbss");
    while (t->kind == TY_ARRAY)
        t = t->base;
    if (in && in->nrel && s->is_static && !section && !(s->aflags & AT_USED) && O->syms[sym].name &&
        opt_target == TARGET_LINUX) {
        HeldData *h;
        if (nheld == capheld) {
            capheld = capheld ? capheld * 2 : 64;
            held = (HeldData *)xrealloc(held, capheld * sizeof(HeldData));
        }
        if (sym >= nheld_of) {
            u32 n = sym + 256;
            held_of = (u32 *)xrealloc(held_of, n * sizeof(u32));
            memset(held_of + nheld_of, 0, (n - nheld_of) * sizeof(u32));
            nheld_of = n;
        }
        h = &held[nheld++];
        held_of[sym] = nheld;
        if (in->buf.len < size)
            buf_zero(&in->buf, (size_t)(size - in->buf.len));
        if (in->buf.len > size && type_is_record(s->ty))
            size = in->buf.len;
        h->sym = sym;
        h->sec = t->quals & Q_CONST ? SEC_CONST : SEC_DATA;
        h->size = size;
        h->align = type_align(s->ty) < 8 && size >= 8 ? 8 : (u32)type_align(s->ty);
        h->bytes = (char *)xmalloc((size_t)size + 1);
        memcpy(h->bytes, in->buf.p, (size_t)size);
        h->nrel = in->nrel;
        h->rel = (InitReloc *)xmalloc(in->nrel * sizeof(InitReloc));
        memcpy(h->rel, in->rel, in->nrel * sizeof(InitReloc));
        s->defined = 1;
        return;
    }
    if (section) {
        /* a section the program names: often an array put together by the
           linker, so nothing but the type decides the alignment */
        {
            /* ".bss..page_aligned" and its like hold no bytes in the file, as in GCC */
            const char *sn = intern_str(section);
            int bss = (strncmp(sn, ".bss", 4) == 0 && (!sn[4] || sn[4] == '.')) || (s->is_tls && !in);
            sec = obj_section(O, section, s->is_tls ? SF_ALLOC | SF_WRITE | SF_TLS | (bss ? SF_NOBITS : 0)
                                          : bss ? SF_ALLOC | SF_WRITE | SF_NOBITS
                                                : SF_ALLOC | (t->quals & Q_CONST ? 0 : SF_WRITE));
        }
        if (in && in->buf.len > size && type_is_record(s->ty))
            size = in->buf.len;
        off = obj_reserve(O, sec, size, type_align(s->ty));
        if (!(O->secflags[sec] & SF_NOBITS)) {
            size_t n = !in ? 0 : in->buf.len < size ? in->buf.len : (size_t)size;
            if (n)
                buf_put(&O->sec[sec], in->buf.p, n);
            buf_zero(&O->sec[sec], (size_t)size - n);
        }
        for (k = 0; in && k < in->nrel; k++)
            obj_add_reloc(O, sec, (u32)(off + in->rel[k].off), R_ABS64, in->rel[k].sym, in->rel[k].addend);
    } else if (!in) {
        off = obj_reserve(O, SEC_BSS, size, type_align(s->ty) < 8 && size >= 8 ? 8 : type_align(s->ty));
        O->sec[SEC_BSS].len = (size_t)(off + size);
        sec = SEC_BSS;
    } else {
        /* Mach-O keeps addresses out of its constant section; ELF does not mind */
        sec = (t->quals & Q_CONST) && (in->nrel == 0 || opt_target == TARGET_LINUX) ? SEC_CONST : SEC_DATA;
        if (in->buf.len < size)
            buf_zero(&in->buf, (size_t)(size - in->buf.len));
        if (in->buf.len > size && type_is_record(s->ty))
            size = in->buf.len; /* a flexible array member was given elements */
        off = obj_reserve(O, sec, size, type_align(s->ty) < 8 && size >= 8 ? 8 : type_align(s->ty));
        buf_put(&O->sec[sec], in->buf.p, (size_t)size);
        for (k = 0; k < in->nrel; k++)
            obj_add_reloc(O, sec, (u32)(off + in->rel[k].off), R_ABS64, in->rel[k].sym, in->rel[k].addend);
    }
    O->syms[sym].sec = (u16)sec;
    O->syms[sym].value = off;
    O->syms[sym].kind = (u8)(s->is_tls ? 3 : 2);
    O->syms[sym].size = size;
    s->defined = 1;
}

static void static_init(Sym *s)
{
    Init in;
    Func *saved = F;
    memset(&in, 0, sizeof in);
    in.is_static = 1;
    F = const_func;
    initializer(&in, &s->ty);
    F = saved;
    if (!type_complete(s->ty))
        error_at(tok.loc, "variable has an incomplete type");
    define_data(s, &in);
    buf_free(&in.buf);
    free(in.rel);
}

static void static_init_sym(Sym *s)
{
    static_init(s);
}

/* An unnamed automatic object of type t set from the initializer that
   follows; *e becomes that object. */
static void initializer_for(Expr *e, Type *t)
{
    Init in;
    u32 zero = 0, slot = ir_new_slot(F, type_complete(t) ? type_size(t) : 0, type_align(t));
    memset(&in, 0, sizeof in);
    in.slot = slot;
    if (t->kind == TY_ARRAY || type_is_record(t))
        zero = emit_mem(OP_MEMZERO, ir_slot_addr(F, slot, 0), 0, type_size(t), type_align(t));
    initializer(&in, &t);
    F->slots[slot].size = (u32)type_size(t);
    if (zero)
        F->ins[zero].c = (u32)type_size(t);
    set_expr(e, t, ir_slot_addr(F, slot, 0), 1);
}

/* Declares a global (file scope, or block scope extern/function). */
/* C++: whether two function types take the same parameters. */
static int same_params(Type *a, Type *b)
{
    u32 k;
    if (a->nparams != b->nparams || a->variadic != b->variadic)
        return 0;
    for (k = 0; k < a->nparams; k++)
        if (!type_compatible(unqual(a->params[k]), unqual(b->params[k])))
            return 0;
    return 1;
}

static Sym *declare_global(u32 name, Type *ty, int storage, u32 loc)
{
    Sym *s;
    names_reserve(name);
    if (opt_cxx && ty->kind == TY_FUNC) {
        /* functions of one name are told apart by their parameters */
        Sym *last = 0;
        for (s = gsyms[name]; s; last = s, s = s->over) {
            if (s->kind != SK_VAR)
                continue;
            if (s->ty->kind != TY_FUNC)
                error_at(loc, "'%s' redeclared as a different kind of symbol", intern_str(name));
            if (same_params(s->ty, ty)) {
                s->ty = ty;
                return s;
            }
        }
        s = new_sym(name, SK_VAR, ty);
        s->is_static = storage == KW(KW_STATIC) || anon_ns > 0;
        s->c_link = extern_c > 0 || name == id_main || pp_in_system(loc);
        if (!s->c_link)
            s->asmname = mg_func(name, 0, ty);
        if (last)
            last->over = s;
        else
            gsyms[name] = s;
        return s;
    }
    s = gsyms[name];
    if (s) {
        if (!type_compatible(s->ty, ty))
            error_at(loc, "conflicting types for '%s'", intern_str(name));
        /* keep the most informative type */
        if ((ty->kind == TY_FUNC && s->ty->oldstyle && !ty->oldstyle) ||
            (ty->kind == TY_ARRAY && !s->ty->complete && ty->complete))
            s->ty = ty;
        else if (ty->kind == TY_FUNC && !ty->oldstyle)
            s->ty = ty;
    } else {
        s = new_sym(name, SK_VAR, ty);
        s->is_static = storage == KW(KW_STATIC);
        gsyms[name] = s;
    }
    return s;
}

static void func_def(Sym *s, Type *ty, u32 loc);
static int def_extern_inline; /* the definition being read is "extern inline": for inlining only */

/* What must be done when a scope is left: in C++, releasing lock guards (fn is the function to call);
   and giving back the stack that the scope's variable length arrays took (fn is null and the slot
   holds the stack pointer from before the first of them). */
typedef struct Cleanup {
    u32 level;
    u32 slot; /* holds the argument */
    Sym *fn;
} Cleanup;

static Cleanup cleanups[64];
static u32 ncleanup;
static u32 break_level, cont_level; /* the scopes that break and continue leave for */

/* Emits the cleanups of the scopes deeper than `above`, innermost first; they stay registered. */
static void cleanups_run(u32 above)
{
    u32 k;
    for (k = ncleanup; k-- > 0;)
        if (cleanups[k].level > above) {
            u32 arg;
            if (!cleanups[k].fn && (!above || !F->cur))
                continue; /* a return frees the stack by itself */
            arg = ir_load(F, IT_I64, M_64, ir_slot_addr(F, cleanups[k].slot, 0));
            if (!cleanups[k].fn) /* give back the variable length arrays of the scope */
                ir_call(F, IT_VOID, ir_const(F, IT_I64, 2), &arg, 1, 1, IF_ALLOCA);
            else
                ir_call(F, IT_VOID, ir_addr(F, sym_obj(cleanups[k].fn), 0), &arg, 1, 0xffffffffU, 0);
        }
}

static void ret_ins(u32 v)
{
    if (ncleanup)
        cleanups_run(0);
    ir_ret(F, v);
}

static Sym *named_func(const char *name)
{
    u32 id = intern_cstr(name);
    Sym *s;
    names_reserve(id);
    s = gsyms[id];
    if (!s || s->kind != SK_VAR || s->ty->kind != TY_FUNC)
        error_at(tok.loc, "'%s' must be declared before it is needed", name);
    return s;
}

/* The constructors of a class, which are functions named after it; 0 if it has none. */
static Sym *ctor_set(Type *t)
{
    char nm[300];
    u32 id;
    if (!opt_cxx || !type_is_record(t) || !t->canon->tag)
        return 0;
    sprintf(nm, "__ct_%.250s", intern_str(t->canon->tag));
    id = intern_cstr(nm);
    return id < names_cap ? gsyms[id] : 0;
}

/* Calls the constructor of the set that fits the arguments; the result is the object. */
static void ctor_call(Expr *e, Sym *set, Expr *av, u32 n, u32 loc)
{
    static Expr none[1];
    Sym *f = overload_pick(set, 0, av, n, loc);
    set_expr(e, f->ty, ir_addr(F, sym_obj(f), 0), 0);
    call_pre = av ? av : none;
    call_npre = n;
    call_sym = f;
    call(e);
}

/* type name(arguments);   the '(' is the current token, s the variable */
static void paren_init(Sym *s, Type *ty, u32 loc)
{
    Expr av[32], lv, e;
    u32 n;
    Sym *cs = ctor_set(ty);
    next();
    n = parse_args(av, 32);
    set_expr(&lv, unqual(ty), ir_slot_addr(F, s->slot, 0), 1);
    if (type_is_record(ty) && ty->canon->guard) {
        u32 a;
        Sym *lock = named_func("__mcc_guard_lock");
        if (n != 1 || !av[0].lval)
            error_at(loc, "a lock guard is made from one mutex");
        a = av[0].v;
        ir_store(F, M_64, ir_slot_addr(F, s->slot, 0), a);
        ir_call(F, IT_VOID, ir_addr(F, sym_obj(lock), 0), &a, 1, 0xffffffffU, 0);
        if (ncleanup == 64)
            error_at(loc, "too many lock guards");
        cleanups[ncleanup].level = level;
        cleanups[ncleanup].slot = s->slot;
        cleanups[ncleanup++].fn = named_func("__mcc_guard_unlock");
        return;
    }
    if (cs) {
        ctor_call(&e, cs, av, n, loc);
    } else {
        if (n != 1)
            error_at(loc, "no constructor takes these arguments");
        e = av[0];
        rvalue(&e);
    }
    assign(&lv, &e);
}

/* A constructor in a class body: name(parameters) : member(value), ... { }   the '(' is the current
   token. It becomes a function that returns the object, read once the class is complete. */
static void ctor_decl(Type *t)
{
    Token *params, *buf = 0, tk, *out;
    u32 np, n = 0, cap = 0, k, loc = tok.loc;
    char nm[300];
    u32 tag = t->canon->tag, r = intern_cstr("__r");
#define PUT(K, V) do { \
        if (n == cap) { cap = cap ? cap * 2 : 64; buf = (Token *)xrealloc(buf, cap * sizeof(Token)); } \
        memset(&tk, 0, sizeof tk); tk.kind = (u8)(K); tk.v = (V); tk.loc = loc; buf[n++] = tk; \
    } while (0)
    if (!tag)
        error_at(loc, "a class with a constructor needs a name");
    sprintf(nm, "__ct_%.250s", intern_str(tag));
    params = grab_balanced(&np);
    PUT(KW(KW_STATIC), 0);
    PUT(KW(KW_INLINE), 0);
    if (opt_cuda) {
        PUT(KW(KW_ATTRIBUTE), 0); PUT(T_LPAREN, 0); PUT(T_LPAREN, 0);
        PUT(T_IDENT, intern_cstr("host")); PUT(T_COMMA, 0); PUT(T_IDENT, intern_cstr("device"));
        PUT(T_RPAREN, 0); PUT(T_RPAREN, 0);
    }
    PUT(T_IDENT, tag);
    PUT(T_IDENT, intern_cstr(nm));
    for (k = 0; k < np; k++) {
        if (n == cap) { cap = cap ? cap * 2 : 64; buf = (Token *)xrealloc(buf, cap * sizeof(Token)); }
        buf[n++] = params[k];
    }
    PUT(T_LBRACE, 0);
    PUT(T_IDENT, tag); PUT(T_IDENT, r); PUT(T_SEMI, 0);
    if (accept(T_COLON)) {
        for (;;) {
            Token *ev;
            u32 ne;
            if (tok.kind != T_IDENT)
                error_at(tok.loc, "expected a member name");
            PUT(T_IDENT, r); PUT(T_DOT, 0); PUT(T_IDENT, tok.v); PUT(T_ASSIGN, 0);
            next();
            if (tok.kind != T_LPAREN && tok.kind != T_LBRACE)
                error_at(tok.loc, "expected '('");
            ev = grab_balanced(&ne);
            ev[0].kind = T_LPAREN;
            ev[ne - 1].kind = T_RPAREN;
            for (k = 0; k < ne; k++) {
                if (n == cap) { cap = cap ? cap * 2 : 64; buf = (Token *)xrealloc(buf, cap * sizeof(Token)); }
                buf[n++] = ev[k];
            }
            PUT(T_SEMI, 0);
            if (!accept(T_COMMA))
                break;
        }
    }
    if (tok.kind != T_LBRACE || peek()->kind != T_RBRACE)
        error_at(tok.loc, "only constructors that just initialize members are supported");
    next();
    next();
    PUT(KW(KW_RETURN), 0); PUT(T_IDENT, r); PUT(T_SEMI, 0);
    PUT(T_RBRACE, 0);
#undef PUT
    out = (Token *)arena_alloc(&A, n * sizeof(Token));
    memcpy(out, buf, n * sizeof(Token));
    free(buf);
    pend_add(0, 0, 0, 0);
    {
        Pend *p = pend_head;
        while (p->next)
            p = p->next;
        p->raw = out;
        p->nraw = n;
    }
}

/* ---- C++: declarations ---- */

static int explicit_inst;  /* reading "template declaration;": an explicit instantiation */
static int extern_inst;    /* ... after "extern": the instance is another unit's to generate */
static int spec_fn;        /* reading "template <> declaration": an explicit specialization */
static int has_decl_targs; /* the declarator's name had template arguments, now in decl_targs */
static TArgs decl_targs;
static Sym *inst_sym;      /* the function the next declaration defines: an instance of a template */
static Sym **forced;       /* explicitly instantiated: generated whether used or not */
static u32 nforced, capforced;
static void top_item(void);

/* Skips a bracketed group: the current token opens it. */
static void skip_balanced(void)
{
    u32 depth = 0;
    for (;;) {
        int k = tok.kind;
        if (k == T_EOF)
            error_at(tok.loc, "unexpected end of file");
        next();
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE)
            depth++;
        else if ((k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) && --depth == 0)
            return;
    }
}

/* Skips a statement without reading it as code: the branch "if constexpr" does not take. */
static void skip_statement(void)
{
    if (tok.kind == T_IDENT && id_mcc_unroll && tok.v == id_mcc_unroll) {
        /* #pragma unroll [n] before a loop */
        next();
        while (tok.kind != KW(KW_FOR) && tok.kind != KW(KW_WHILE) && tok.kind != KW(KW_DO) && tok.kind != T_EOF) {
            if (tok.kind == T_LPAREN)
                skip_balanced();
            else
                next();
        }
    }
    switch (tok.kind) {
    case T_LBRACE:
        skip_balanced();
        return;
    case KW(KW_IF):
        next();
        if (tok.kind == T_IDENT && tok.v == id_constexpr)
            next();
        skip_balanced();
        skip_statement();
        if (accept(KW(KW_ELSE)))
            skip_statement();
        return;
    case KW(KW_WHILE): case KW(KW_SWITCH): case KW(KW_FOR):
        next();
        skip_balanced();
        skip_statement();
        return;
    case KW(KW_DO):
        next();
        skip_statement();
        expect(KW(KW_WHILE));
        skip_balanced();
        expect(T_SEMI);
        return;
    }
    while (tok.kind != T_SEMI) {
        if (tok.kind == T_EOF)
            error_at(tok.loc, "unexpected end of file");
        if (tok.kind == T_LPAREN || tok.kind == T_LBRACK || tok.kind == T_LBRACE)
            skip_balanced();
        else
            next();
    }
    next();
}

/* template <> struct name<arguments> { ... };   or   template <> a function */
static void template_spec(void)
{
    if (tok.kind == KW(KW_STRUCT) || tok.kind == KW(KW_UNION) || (tok.kind == T_IDENT && tok.v == id_class)) {
        int kind = tok.kind == KW(KW_UNION) ? TY_UNION : TY_STRUCT;
        Sym *s;
        TArgs a;
        TInst *ti;
        Tmpl *t;
        next();
        s = tok.kind == T_IDENT ? lookup(tok.v) : 0;
        if (!s || s->kind != SK_TEMPLATE || !s->is_ctmpl)
            error_at(tok.loc, "expected the name of a class template");
        t = s->tmpl;
        next();
        if (tok.kind != T_LT)
            error_at(tok.loc, "expected '<'");
        parse_targs(&a);
        if (a.n > t->nparams || !targs_defaults(t, &a))
            error_at(tok.loc, "wrong number of arguments for template '%s'", intern_str(t->name));
        ti = inst_find(t, &a, 1);
        if (!ti->ty) {
            Buf b;
            memset(&b, 0, sizeof b);
            mg_s(&b, intern_str(t->name));
            mg_s(&b, "I");
            mg_s(&b, intern_str(ti->key));
            mg_s(&b, "E");
            ti->ty = type_record(kind, intern((const char *)b.p, b.len));
            buf_free(&b);
        }
        ti->spec = 1;
        if (tok.kind == T_LBRACE) {
            rec_resume = ti->ty;
            record_spec();
        }
        expect(T_SEMI);
        return;
    }
    spec_fn = 1;
    declaration(1);
    spec_fn = 0;
}

static void template_decl(void)
{
    TParam params[MAX_TARGS];
    Tmpl *t;
    Token *buf = 0;
    u32 np = 0, n = 0, cap = 0, depth = 0, nhead = 0, k, name = 0, nfp = 0, loc = tok.loc;
    int is_class, has_body = 0, rkind = TY_STRUCT;
    Sym *s, *c;

    next();
    if (tok.kind != T_LT) {
        explicit_inst = 1;
        declaration(1);
        explicit_inst = 0;
        return;
    }
    next();
    if (tok.kind == T_GT) {
        next();
        template_spec();
        return;
    }
    push_scope(); /* a parameter can be used by those after it */
    for (;;) {
        TParam *p = &params[np];
        Sym *ps;
        if (np == MAX_TARGS)
            error_at(tok.loc, "too many template parameters");
        memset(p, 0, sizeof *p);
        if (tok.kind == T_IDENT && !lookup(tok.v) && (tok.v == id_class || tok.v == id_typename)) {
            next();
            p->is_type = 1;
            if (tok.kind == T_IDENT) {
                p->name = tok.v;
                next();
            }
            ps = new_sym(p->name, SK_TYPEDEF, type_tparam(np));
        } else {
            int st;
            Type *bt = declspec(&st);
            p->vty = declarator(bt, &p->name);
            ps = new_sym(p->name, SK_VAR, unqual(p->vty));
            ps->cv = 1;
            ps->val = 1;
        }
        if (p->name)
            bind(ps);
        if (accept(T_ASSIGN)) {
            Token *d = 0;
            u32 nd = 0, capd = 0, ang = 0, par = 0;
            for (;;) {
                int kk = tok.kind;
                if (kk == T_EOF)
                    error_at(tok.loc, "unexpected end of file");
                if (!par && !ang && (kk == T_COMMA || kk == T_GT || kk == T_SHR))
                    break;
                if (kk == T_LPAREN || kk == T_LBRACK)
                    par++;
                else if (kk == T_RPAREN || kk == T_RBRACK)
                    par--;
                else if (!par && kk == T_LT)
                    ang++;
                else if (!par && kk == T_GT)
                    ang--;
                if (nd == capd) {
                    capd = capd ? capd * 2 : 16;
                    d = (Token *)xrealloc(d, capd * sizeof(Token));
                }
                d[nd++] = tok;
                next();
            }
            p->def = with_semi(d, nd);
            p->ndef = nd;
            free(d);
        }
        np++;
        if (!accept(T_COMMA))
            break;
    }
    pop_scope();
    close_angle();

    /* the declaration, kept as tokens */
    is_class = tok.kind == KW(KW_STRUCT) || tok.kind == KW(KW_UNION) || (tok.kind == T_IDENT && tok.v == id_class);
    if (is_class) {
        Token *p2;
        rkind = tok.kind == KW(KW_UNION) ? TY_UNION : TY_STRUCT;
        if (peek()->kind != T_IDENT)
            error_at(loc, "expected a name for the class template");
        name = peek()->v;
        p2 = peek();
        (void)p2;
    }
    for (;;) {
        int kk = tok.kind;
        if (kk == T_EOF)
            error_at(loc, "unexpected end of file in a template");
        if (n == cap) {
            cap = cap ? cap * 2 : 128;
            buf = (Token *)xrealloc(buf, cap * sizeof(Token));
        }
        buf[n++] = tok;
        if (kk == T_LPAREN || kk == T_LBRACK) {
            depth++;
        } else if (kk == T_RPAREN || kk == T_RBRACK) {
            depth--;
        } else if (!depth && kk == T_SEMI) {
            if (!has_body)
                nhead = n - 1;
            break;
        } else if (!depth && kk == T_LBRACE) {
            u32 braces = 1;
            has_body = 1;
            nhead = n - 1;
            while (braces) {
                next();
                if (tok.kind == T_EOF)
                    error_at(loc, "unexpected end of file in a template");
                if (n == cap) {
                    cap *= 2;
                    buf = (Token *)xrealloc(buf, cap * sizeof(Token));
                }
                buf[n++] = tok;
                if (tok.kind == T_LBRACE)
                    braces++;
                else if (tok.kind == T_RBRACE)
                    braces--;
            }
            if (!is_class)
                break;
        }
        next();
    }
    next();

    t = (Tmpl *)arena_alloc(&A, sizeof(Tmpl));
    t->params = (TParam *)arena_alloc(&A, np * sizeof(TParam));
    memcpy(t->params, params, np * sizeof(TParam));
    t->nparams = np;
    t->toks = (Token *)arena_alloc(&A, n * sizeof(Token));
    memcpy(t->toks, buf, n * sizeof(Token));
    t->ntoks = n;
    t->nhead = nhead;
    t->head = with_semi(buf, nhead);
    t->has_body = (u8)has_body;
    t->is_class = (u8)is_class;
    t->rkind = (u8)rkind;
    t->hide = hide;

    if (is_class) {
        if (n > 2 && buf[2].kind == T_LT)
            error_at(loc, "partial specializations of class templates are not supported");
        free(buf);
        t->name = name;
        s = lookup(name);
        if (s && s->kind == SK_TEMPLATE && s->is_ctmpl) {
            if (has_body && !s->tmpl->toks) {
                t->insts = s->tmpl->insts;
                s->tmpl = t;
            }
            return;
        }
        if (!has_body)
            t->toks = 0;
        s = new_sym(name, SK_TEMPLATE, ty_void);
        s->is_ctmpl = 1;
        s->tmpl = t;
        bind(s);
        return;
    }

    /* a function: its name is the identifier before the parameter list */
    for (k = 0; k + 1 < nhead && !name; k++) {
        if (buf[k].kind == KW(KW_ATTRIBUTE) && buf[k + 1].kind == T_LPAREN) {
            u32 d = 0;
            for (k++; k < nhead; k++) {
                if (buf[k].kind == T_LPAREN)
                    d++;
                else if (buf[k].kind == T_RPAREN && --d == 0)
                    break;
            }
            continue;
        }
        if (buf[k].kind == T_IDENT && buf[k + 1].kind == T_LPAREN && buf[k].v != id_decltype) {
            u32 d = 0, j;
            name = buf[k].v;
            for (j = k + 1; j < nhead; j++) {
                if (buf[j].kind == T_LPAREN || buf[j].kind == T_LBRACK || buf[j].kind == T_LT) {
                    d++;
                } else if (buf[j].kind == T_RPAREN || buf[j].kind == T_RBRACK || buf[j].kind == T_GT) {
                    if (--d == 0)
                        break;
                } else if (d == 1 && buf[j].kind == T_COMMA) {
                    nfp++;
                } else if (d == 1 && !nfp && buf[j].kind != KW(KW_VOID)) {
                    nfp = 1;
                }
            }
        }
    }
    free(buf);
    if (!name)
        error_at(loc, "this kind of template is not supported");
    t->name = name;
    t->nfparams = nfp;
    names_reserve(name);
    for (c = gsyms[name]; c; c = c->over) {
        if (c->kind == SK_TEMPLATE && !c->is_ctmpl && c->tmpl->nparams == np && c->tmpl->nfparams == nfp) {
            /* declared before: this may be its definition */
            Tmpl *o = c->tmpl;
            if (has_body && o->has_body)
                continue; /* two definitions: overloads that differ in their parameters' types */
            if (has_body && !o->has_body) {
                TInst *ti;
                o->toks = t->toks;
                o->ntoks = t->ntoks;
                o->nhead = t->nhead;
                o->head = t->head;
                o->params = t->params;
                o->has_body = 1;
                for (ti = o->insts; ti; ti = ti->next)
                    if (ti->fn && !ti->queued && !ti->spec) {
                        ti->queued = 1;
                        pend_add(o, ti, 0, ti->fn);
                    }
            }
            return;
        }
    }
    s = new_sym(name, SK_TEMPLATE, ty_void);
    s->tmpl = t;
    if (gsyms[name]) {
        for (c = gsyms[name]; c->over; c = c->over)
            ;
        c->over = s;
    } else {
        gsyms[name] = s;
        if (!lookup(name))
            bind(s);
    }
}

/* The function that "name<arguments>" in a declaration stands for: an explicit
   specialization being declared, or an explicit instantiation asked for. */
static Sym *spec_or_inst(u32 name, Type *ty, int is_inline, u32 loc)
{
    Sym *c, *fn;
    Tmpl *t = 0;
    TArgs a = decl_targs;
    TInst *ti;
    names_reserve(name);
    for (c = gsyms[name]; c; c = c->over)
        if (c->kind == SK_TEMPLATE && !c->is_ctmpl && c->tmpl->nparams >= a.n &&
            (!t || (c->tmpl->nfparams == ty->nparams && t->nfparams != ty->nparams)))
            t = c->tmpl;
    if (!t)
        error_at(loc, "'%s' is not a function template", intern_str(name));
    if (a.n < t->nparams && !targs_defaults(t, &a))
        error_at(loc, "the template arguments of '%s' must be written out", intern_str(name));
    if (explicit_inst && extern_inst) {
        ti = inst_find(t, &a, 1);
        if (ti->fn)
            return ti->fn; /* in use already: this unit has its own */
        fn = fn_inst(t, &a);
        if (ti->queued && !fn->is_inline && !(fn->aflags & AT_ALWAYS_INLINE)) {
            ti->ext = 1;
            fn->is_static = 0;
        }
        return fn;
    }
    if (explicit_inst) {
        fn = fn_inst(t, &a);
        ti = inst_find(t, &a, 1);
        if (ti->ext) {
            ti->ext = 0;
            pend_add(t, ti, 0, fn);
        }
        fn->is_static = 0;
        if (fn->objsym != NOSYM)
            O->syms[fn->objsym].global = 1;
        if (nforced == capforced) {
            capforced = capforced ? capforced * 2 : 16;
            forced = (Sym **)xrealloc(forced, capforced * sizeof(Sym *));
        }
        forced[nforced++] = fn;
        return fn;
    }
    ti = inst_find(t, &a, 1);
    if (!ti->fn) {
        fn = new_sym(name, SK_VAR, ty);
        fn->asmname = mg_func(name, intern_str(ti->key), ty);
        fn->is_static = (u8)(is_inline != 0);
        ti->fn = fn;
    }
    ti->spec = 1;
    return ti->fn;
}

/* Declarations only C++ has. Returns 1 if the current token began one, which has been read. */
static int cxx_decl(void)
{
    if (tok.kind == KW(KW_EXTERN) && peek()->kind == T_IDENT && peek()->v == id_template && !lookup(id_template)) {
        /* extern template: this instance is another unit's to generate; uses here refer to it */
        next();
        if (peek()->kind == KW(KW_STRUCT) || peek()->kind == KW(KW_UNION) || peek()->kind == T_LT ||
            (peek()->kind == T_IDENT && peek()->v == id_class)) {
            while (tok.kind != T_SEMI && tok.kind != T_EOF)
                next();
            expect(T_SEMI);
            return 1;
        }
        extern_inst = 1;
        template_decl();
        extern_inst = 0;
        return 1;
    }
    if (tok.kind == KW(KW_EXTERN) && peek()->kind == T_STR) {
        next();
        next();
        extern_c++;
        if (accept(T_LBRACE)) {
            while (tok.kind != T_RBRACE) {
                if (tok.kind == T_EOF)
                    error_at(tok.loc, "expected '}'");
                top_item();
            }
            next();
        } else {
            top_item();
        }
        extern_c--;
        return 1;
    }
    if (tok.kind == KW(KW_INLINE) && peek()->kind == T_IDENT && peek()->v == id_namespace)
        next();
    if (tok.kind != T_IDENT || lookup(tok.v))
        return 0;
    if (tok.v == id_namespace) {
        int anon = 1;
        next();
        while (tok.kind == T_IDENT) {
            ns_add(tok.v);
            anon = 0;
            next();
            if (tok.kind == T_SCOPE)
                next();
        }
        expect(T_LBRACE);
        anon_ns += anon;
        while (tok.kind != T_RBRACE) {
            if (tok.kind == T_EOF)
                error_at(tok.loc, "expected '}'");
            top_item();
        }
        next();
        anon_ns -= anon;
        return 1;
    }
    if (tok.v == id_using) {
        cxx_using(0);
        return 1;
    }
    if (tok.v == id_template) {
        template_decl();
        return 1;
    }
    return 0;
}

/* auto &name = object;  the '&' is the current token */
static void auto_ref(u32 loc)
{
    Expr e;
    Sym *s;
    u32 name;
    next();
    if (tok.kind != T_IDENT)
        error_at(loc, "expected identifier");
    name = tok.v;
    next();
    expect(T_ASSIGN);
    if (!level)
        error_at(loc, "a reference at file scope is not supported");
    assign_expr(&e);
    if (!e.lval) {
        rvalue(&e);
        ref_addr(&e, e.ty);
    }
    s = new_sym(name, SK_VAR, e.ty);
    s->local = 1;
    s->byref = 1;
    s->slot = ir_new_slot(F, 8, 8);
    ir_store(F, M_64, ir_slot_addr(F, s->slot, 0), e.v);
    bind(s);
}

/* type &name = object;  the '=' is the current token */
static void local_ref(u32 name, Type *bt, u32 loc)
{
    Expr e;
    Sym *s;
    if (!level)
        error_at(loc, "a reference at file scope is not supported");
    expect(T_ASSIGN);
    ref_bind = 1;
    assign_expr(&e);
    ref_bind = 0;
    ref_addr(&e, bt);
    s = new_sym(name, SK_VAR, bt);
    s->local = 1;
    s->byref = 1;
    s->slot = ir_new_slot(F, 8, 8);
    ir_store(F, M_64, ir_slot_addr(F, s->slot, 0), e.v);
    bind(s);
}

/* const type name = value;  after the '='. A value known here makes the name a constant
   with no storage; otherwise (in a function) it is an ordinary variable. */
static void const_decl(u32 name, Type *ty, int must, u32 loc)
{
    Sym *s = new_sym(name, SK_VAR, unqual(ty)), *old = lookup(name);
    if (old && old->level == level && level > 0)
        error_at(loc, "redeclaration of '%s'", intern_str(name));
    if (must && level == 0) {
        /* at file scope the value may have to be worked out when the program starts */
        Token *it = 0, *raw, tk;
        u32 ni = 0, cap = 0, depth = 0, k, n = 0;
        static u32 seq;
        char nm[40];
        for (;;) {
            int kd = tok.kind;
            if (kd == T_EOF || (!depth && (kd == T_COMMA || kd == T_SEMI)))
                break;
            if (kd == T_LPAREN || kd == T_LBRACK || kd == T_LBRACE)
                depth++;
            else if (kd == T_RPAREN || kd == T_RBRACK || kd == T_RBRACE)
                depth--;
            if (ni == cap) {
                cap = cap ? cap * 2 : 32;
                it = (Token *)xrealloc(it, cap * sizeof(Token));
            }
            it[ni++] = tok;
            next();
        }
        replay(with_semi(it, ni), ni + 1);
        if (cv_parse(s, ty)) {
            expect(T_SEMI);
        } else {
            expect(T_SEMI);
            if (opt_target != TARGET_LINUX)
                error_at(loc, "the initializer of '%s' is not a constant expression", intern_str(name));
            s->is_static = 1;
            s->objsym = obj_add_sym(O, 0, SEC_UNDEF, 0, 0);
            sym_align(s->objsym, s->ty);
            define_data(s, 0);
            raw = (Token *)arena_alloc(&A, (ni + 24) * sizeof(Token));
#define PUT(K, V) do { memset(&tk, 0, sizeof tk); tk.kind = (u8)(K); tk.v = (V); tk.loc = loc; raw[n++] = tk; } while (0)
            sprintf(nm, "__dyninit_%u", seq++);
            PUT(KW(KW_STATIC), 0);
            PUT(KW(KW_ATTRIBUTE), 0); PUT(T_LPAREN, 0); PUT(T_LPAREN, 0);
            PUT(T_IDENT, intern_cstr("constructor"));
            PUT(T_RPAREN, 0); PUT(T_RPAREN, 0);
            PUT(KW(KW_VOID), 0); PUT(T_IDENT, intern_cstr(nm)); PUT(T_LPAREN, 0); PUT(KW(KW_VOID), 0); PUT(T_RPAREN, 0);
            PUT(T_LBRACE, 0);
            PUT(T_IDENT, name); PUT(T_ASSIGN, 0); PUT(T_LPAREN, 0);
            for (k = 0; k < ni; k++)
                raw[n++] = it[k];
            PUT(T_RPAREN, 0); PUT(T_SEMI, 0);
            PUT(T_RBRACE, 0);
#undef PUT
            {
                Pend *pd = pend_add(0, 0, 0, 0);
                pd->raw = raw;
                pd->nraw = n;
            }
        }
        free(it);
    } else if (must) {
        if (!cv_parse(s, ty))
            error_at(loc, "the initializer of '%s' is not a constant expression", intern_str(name));
    } else {
        Expr e;
        u64 iv = 0;
        double fv = 0;
        assign_expr(&e);
        rvalue(&e);
        if (!type_is_scalar(e.ty))
            error_at(loc, "invalid initializer");
        convert(&e, s->ty);
        if (type_is_int(ty) && ir_is_const(F, e.v, &iv)) {
            s->cv = 1;
            s->val = (i64)iv;
        } else if (type_is_float(ty) && ir_is_fconst(F, e.v, &fv)) {
            s->cv = 2;
            s->fval = fv;
        } else {
            s->local = 1;
            s->slot = ir_new_slot(F, type_size(ty), type_align(ty));
            store_val(s->ty, ir_slot_addr(F, s->slot, 0), e.v);
        }
    }
    bind(s);
}

/* Returns 1 if it parsed a function definition. */
static void static_assert(void)
{
    u32 loc = tok.loc;
    i64 v;
    next();
    expect(T_LPAREN);
    v = const_int_expr();
    if (accept(T_COMMA)) {
        Buf b;
        memset(&b, 0, sizeof b);
        if (tok.kind != T_STR)
            error_at(tok.loc, "expected a string");
        string_literal(&b);
        if (!v)
            error_at(loc, "static assertion failed: %s", (const char *)b.p);
        buf_free(&b);
    } else if (!v) {
        error_at(loc, "static assertion failed");
    }
    expect(T_RPAREN);
    expect(T_SEMI);
}

static Sym **aliased; /* symbols that are other names for something defined here */
static u32 naliased;

static void sym_attrs(Sym *s, const Attr *a, int is_inline, int storage)
{
    s->aflags |= a->flags;
    if (a->maxntid)
        s->maxntid = a->maxntid;
    if (a->section)
        s->section = a->section;
    if (a->alias && !s->alias) {
        s->alias = a->alias;
        if ((naliased & (naliased - 1)) == 0)
            aliased = (Sym **)xrealloc(aliased, (naliased ? naliased * 2 : 1) * sizeof(Sym *));
        aliased[naliased++] = s;
    }
    if (a->asmname) {
        /* a later declaration may still name the symbol (the GNU C library's headers:
           extern __typeof (f) f __asm__ ("g"), where the typeof alone has made f's symbol) */
        if (s->asmname != a->asmname && s->objsym != NOSYM && !s->defined && s->ty && s->ty->kind == TY_FUNC)
            s->objsym = NOSYM;
        s->asmname = a->asmname;
    }
    if (is_inline)
        s->is_inline = 1;
    if (storage == KW(KW_EXTERN))
        s->is_extern = 1;
    if (storage == KW(KW_REGISTER))
        s->aflags |= AT_REGISTER;
    if ((s->aflags & AT_ERROR) && !s->local && s->objsym != NOSYM)
        O->syms[s->objsym].trap = 1;
    if ((s->aflags & AT_WEAK) && !s->local && s->objsym != NOSYM)
        O->syms[s->objsym].weak = 1; /* the symbol was made by an earlier use */
}

static int declaration(int file_scope)
{
    int storage, is_inline, is_auto, is_cx = 0, targs_here, tls;
    Sym *mem_here;
    Type *base;
    u32 loc = tok.loc;
    Attr at0;

    if (tok.kind == KW(KW_STATIC_ASSERT)) {
        static_assert();
        return 0;
    }
    if (opt_cxx && cxx_decl())
        return 0;
    base = declspec(&storage);
    at0 = ds_attr;
    is_inline = ds_inline;
    is_auto = ds_auto;
    is_cx = ds_constexpr;
    tls = ds_tls;
    if (tls && opt_target != TARGET_LINUX)
        error_at(loc, "thread-local storage is only supported for Linux targets");
    if (accept(T_SEMI))
        return 0;
    for (;;) {
        u32 name = 0;
        Type *ty;
        Sym *s;
        Attr at = at0;
        u32 vla;
        memset(&dcl_attr, 0, sizeof dcl_attr);
        if (tok.kind == KW(KW_ATTRIBUTE)) { /* GNU: int a, __attribute__((unused)) *b; */
            Attr skip;
            attributes(&skip);
        }
        last_pdef = 0;
        has_decl_targs = 0;
        decl_member = 0;
        if (opt_cxx && is_auto) {
            if (tok.kind == T_AMP || tok.kind == T_ANDAND) {
                auto_ref(loc);
                goto next_d;
            }
            while (tok.kind == T_STAR)
                next(); /* auto *p: the initializer says as much */
        }
        if (is_auto && tok.kind == T_IDENT) {
            name = tok.v;
            next();
            if (opt_cxx && tok.kind == T_ASSIGN && peek()->kind == T_LBRACK) {
                /* a lambda: the variable is only a name for it */
                Expr le;
                next();
                lambda_expr(&le);
                s = new_sym(name, SK_VAR, le.ty);
                s->local = 1;
                s->slot = ir_new_slot(F, 1, 1);
                bind(s);
                goto next_d;
            }
            ty = auto_type();
            if (opt_cxx && base->quals)
                ty = type_qual(ty, base->quals);
            vla_len = 0;
        } else {
            ty = declarator(base, &name);
        }
        targs_here = has_decl_targs;
        has_decl_targs = 0;
        mem_here = decl_member;
        decl_member = 0;
        vla = ty->kind == TY_ARRAY ? vla_len : 0;
        if (ty->kind == TY_ARRAY && ty->vslot && !vla)
            vla = 1; /* an array of arrays whose length is not constant */
        at.flags |= dcl_attr.flags;
        if (dcl_attr.section)
            at.section = dcl_attr.section;
        if (dcl_attr.alias)
            at.alias = dcl_attr.alias;
        decl_suffix(&at);
        if (at.align > at0.align || at.mode != at0.mode || at.vecsize != at0.vecsize)
            ty = attr_type(ty, &at);
        loc = tok.loc;
        if (!name)
            error_at(loc, "expected identifier");
        if (opt_cxx && storage != KW(KW_TYPEDEF) && ty->kind != TY_FUNC && !(at.flags & AT_SHARED)) {
            if (ty->kind == TY_REF) {
                local_ref(name, ty->base, loc);
                goto next_d;
            }
            if (tok.kind == T_ASSIGN && (ty->quals & Q_CONST) && !(ty->quals & Q_VOLATILE) &&
                (type_is_int(ty) || type_is_float(ty)) && storage != KW(KW_EXTERN) && !at.section) {
                next();
                const_decl(name, ty, file_scope || storage == KW(KW_STATIC) || F == const_func, loc);
                goto next_d;
            }
            if ((is_cx || tls) && !file_scope && !storage)
                storage = KW(KW_STATIC); /* a constant table; thread_local in a block means static too */
            if (file_scope && !storage &&
                ((ty->quals & Q_CONST) || (ty->kind == TY_ARRAY && (ty->base->quals & Q_CONST))))
                storage = KW(KW_STATIC); /* constants have internal linkage */
        }
        if (opt_cuda && (at.flags & AT_SHARED) && storage != KW(KW_TYPEDEF) && ty->kind != TY_FUNC) {
            /* CUDA: one object for each block of threads, declared in the PTX module */
            char nm[300];
            sprintf(nm, "%.200s__sh%u", intern_str(name), shared_seq++);
            s = new_sym(name, SK_VAR, ty);
            s->is_static = 1;
            s->defined = 1;
            s->objsym = obj_add_sym(O, intern_cstr(nm), SEC_UNDEF, 0, 0);
            ptx_shared(s->objsym, type_complete(ty) ? type_size(ty) : 0, type_align(ty) < 16 ? 16 : type_align(ty),
                       storage == KW(KW_EXTERN) || !type_complete(ty));
            bind(s);
            if (tok.kind == T_ASSIGN)
                error_at(loc, "a shared variable cannot have an initializer");
        } else if (storage == KW(KW_TYPEDEF)) {
            s = lookup(name);
            if (s && s->level == level && s->kind == SK_TYPEDEF && type_compatible(s->ty, ty)) {
                /* harmless repetition */
            } else {
                s = new_sym(name, SK_TYPEDEF, ty);
                bind(s);
                if ((at.flags & AT_TRANSPARENT) && type_is_record(ty))
                    (ty->canon ? ty->canon : ty)->transparent = 1;
                if (opt_cxx && type_is_record(ty) && ty->canon && !ty->canon->tag && !ty->canon->lname)
                    ty->canon->lname = name; /* typedef struct { } name: the name the type goes by in symbols */
            }
        } else if (ty->kind == TY_FUNC || file_scope || storage == KW(KW_EXTERN)) {
            Sym *cur;
            int bound = 0;
            if (inst_sym) {
                s = inst_sym; /* C++: this declaration is a template being read for one set of arguments */
                inst_sym = 0;
                bound = 1;
            } else if (targs_here) {
                s = spec_or_inst(name, ty, is_inline, loc);
                bound = 1;
            } else if (mem_here) {
                for (s = mem_here; s; s = s->over)
                    if (type_compatible(s->ty, ty))
                        break;
                if (!s)
                    error_at(loc, "the class declares no such '%s'", intern_str(name));
                bound = 1;
            } else {
                s = declare_global(name, ty, storage, loc);
            }
            if (opt_cxx && ty->kind == TY_FUNC) {
                if ((is_inline || is_cx) && s->objsym == NOSYM && !s->defined && !explicit_inst)
                    s->is_static = 1; /* an inline function is each unit's own */
                if (last_pdef) {
                    s->pdef = last_pdef;
                    s->npdef = last_npdef;
                }
                if (is_cx)
                    s->is_cx = 1;
            }
            sym_attrs(s, &at, is_inline, storage);
            if (tls && ty->kind != TY_FUNC)
                s->is_tls = 1;
            cur = lookup(name);
            if (bound) {
                ;
            } else if (level == 0) {
                if (cur && cur != s &&
                    !(opt_cxx && ty->kind == TY_FUNC &&
                      (cur->kind == SK_TEMPLATE || (cur->kind == SK_VAR && cur->ty->kind == TY_FUNC))))
                    error_at(loc, "'%s' redeclared as a different kind of symbol", intern_str(name));
                if (!cur)
                    bind(s);
            } else if (!cur || cur->level != level || cur->real != s) {
                Sym *l = new_sym(name, SK_VAR, ty);
                l->real = s;
                bind(l);
            }
            if (ty->kind == TY_FUNC) {
                if (file_scope && (tok.kind == T_LBRACE || (ty->oldstyle && ty->nparams && is_declstart(&tok)))) {
                    def_extern_inline = is_inline && storage == KW(KW_EXTERN) && !opt_cxx;
                    if (opt_cxx && s->is_cx && !s->body && tok.kind == T_LBRACE) {
                        /* a constexpr function is also kept as tokens, to be worked out where constants are wanted */
                        s->body = grab_balanced(&s->nbody);
                        replay(s->body, s->nbody);
                    }
                    if (mem_here) {
                        push_scope();
                        cs_bind(s->cls);
                        func_def(s, ty, loc);
                        pop_scope();
                        return 1;
                    }
                    func_def(s, ty, loc);
                    return 1;
                }
            } else if (tok.kind == T_ASSIGN) {
                if (s->defined)
                    error_at(loc, "redefinition of '%s'", intern_str(name));
                next();
                s->ty = ty;
                static_init(s);
            } else if (storage != KW(KW_EXTERN) && !s->tentative && !s->defined && !(s->aflags & AT_REGISTER)) {
                s->tentative = 1;
                s->tent_next = tentatives;
                tentatives = s;
            }
        } else if (storage == KW(KW_STATIC)) {
            /* block scope static: an anonymous object */
            s = new_sym(name, SK_VAR, ty);
            s->is_static = 1;
            s->is_tls = (u8)tls;
            s->objsym = obj_add_sym(O, 0, SEC_UNDEF, 0, 0);
            sym_align(s->objsym, ty);
            bind(s);
            if (accept(T_ASSIGN)) {
                static_init(s);
            } else {
                if (!type_complete(ty))
                    error_at(loc, "variable has an incomplete type");
                define_data(s, 0);
            }
        } else {
            Sym *old = lookup(name);
            if (old && old->level == level && level > 0)
                error_at(loc, "redeclaration of '%s'", intern_str(name));
            if (tls)
                error_at(loc, "a thread-local object must be static or extern");
            s = new_sym(name, SK_VAR, ty);
            s->local = 1;
            if (vla) {
                /* the space is taken from the stack when the declaration is reached, and
                   stays until the function returns; the slot holds its address */
                u32 arg, vs;
                if (ty->vslot) {
                    arg = ir_load(F, IT_I64, M_64, ir_slot_addr(F, ty->vslot - 1, 0));
                } else {
                    arg = ir_ins(F, OP_MUL, IT_I64, vla, ir_const(F, IT_I64, type_size(ty->base)));
                    vs = ir_new_slot(F, 8, 8);
                    if (F->cur)
                        ir_store(F, M_64, ir_slot_addr(F, vs, 0), arg);
                    ty->vslot = vs + 1; /* for sizeof */
                }
                s->slot = ir_new_slot(F, 8, 8);
                s->byref = 1;
                if (F->cur && level > 1 && ncleanup < 64) {
                    /* the first of a scope inside the function's own: the space lasts as long as the
                       scope, which matters when that is the body of a loop */
                    u32 k, zero = ir_const(F, IT_I64, 0);
                    for (k = ncleanup; k-- > 0;)
                        if (cleanups[k].level == level && !cleanups[k].fn)
                            break;
                    if (k == 0xffffffffU) {
                        cleanups[ncleanup].level = level;
                        cleanups[ncleanup].slot = ir_new_slot(F, 8, 8);
                        cleanups[ncleanup].fn = 0;
                        ir_store(F, M_64, ir_slot_addr(F, cleanups[ncleanup].slot, 0),
                                 ir_call(F, IT_I64, ir_const(F, IT_I64, 1), &zero, 1, 1, IF_ALLOCA));
                        ncleanup++;
                    }
                }
                if (F->cur)
                    ir_store(F, M_64, ir_slot_addr(F, s->slot, 0),
                             ir_call(F, IT_I64, ir_const(F, IT_I64, 0), &arg, 1, 1, IF_ALLOCA));
                bind(s);
                if (!accept(T_COMMA))
                    break;
                continue;
            }
            s->slot = ir_new_slot(F, type_complete(ty) ? type_size(ty) : 0, type_align(ty));
            sym_attrs(s, &at, 0, storage); /* register ... asm("x0") matters to inline assembly */
            bind(s); /* in scope from here on, its own initializer included */
            if (tok.kind == T_ASSIGN || (opt_cxx && tok.kind == T_LBRACE)) {
                Init in;
                u32 zero = 0;
                if (tok.kind == T_ASSIGN)
                    next();
                memset(&in, 0, sizeof in);
                in.slot = s->slot;
                if (ty->kind == TY_ARRAY || (type_is_record(ty) && tok.kind == T_LBRACE))
                    zero = emit_mem(OP_MEMZERO, ir_slot_addr(F, s->slot, 0), 0, type_size(ty), type_align(ty));
                initializer(&in, &s->ty);
                ty = s->ty;
                F->slots[s->slot].size = (u32)type_size(ty);
                if (zero)
                    F->ins[zero].c = (u32)type_size(ty);
            }
            else if (opt_cxx && tok.kind == T_LPAREN)
                paren_init(s, ty, loc);
            else if (opt_cxx && type_is_record(ty) && strncmp(cur_fn_name, "__ct_", 5) != 0 && ctor_set(ty)) {
                Expr ce, lv;
                ctor_call(&ce, ctor_set(ty), 0, 0, loc); /* the default constructor */
                set_expr(&lv, unqual(ty), ir_slot_addr(F, s->slot, 0), 1);
                assign(&lv, &ce);
            }
            if (!type_complete(ty))
                error_at(loc, "variable has an incomplete type");
        }
    next_d:
        if (!accept(T_COMMA))
            break;
    }
    expect(T_SEMI);
    return 0;
}

/* ---- statements ---- */

static Label *get_label(u32 name)
{
    Label *l;
    for (l = labels; l; l = l->next)
        if (l->name == name)
            return l;
    l = (Label *)arena_alloc(&A, sizeof(Label));
    l->name = name;
    l->blk = ir_new_block(fn_func); /* not F: a static initializer may be naming the label */
    l->loc = tok.loc;
    l->next = labels;
    labels = l;
    return l;
}

static u32 condition(void)
{
    Expr e;
    expect(T_LPAREN);
    expr(&e);
    rvalue(&e);
    expect(T_RPAREN);
    return to_bool(&e);
}

static int case_cmp(const void *a, const void *b)
{
    i64 x = ((const Case *)a)->val, y = ((const Case *)b)->val;
    return x < y ? -1 : x > y;
}

/* Binary search over the sorted cases [lo, hi). */
static void switch_dispatch(u32 v, int it, int uns, Case *c, u32 lo, u32 hi, u32 dflt)
{
    u32 k, nb, mid, lb, rb;
    if (hi - lo <= 3) {
        for (k = lo; k < hi; k++) {
            nb = ir_new_block(F);
            if (c[k].hi == c[k].val)
                ir_br(F, ir_ins(F, OP_EQ, IT_I32, v, ir_const(F, it, (u64)c[k].val)), c[k].blk, nb);
            else
                ir_br(F, ir_ins(F, OP_ULE, IT_I32, ir_ins(F, OP_SUB, it, v, ir_const(F, it, (u64)c[k].val)),
                                ir_const(F, it, (u64)(c[k].hi - c[k].val))), c[k].blk, nb);
            F->cur = nb;
        }
        ir_jmp(F, dflt);
        return;
    }
    mid = lo + (hi - lo) / 2;
    lb = ir_new_block(F);
    rb = ir_new_block(F);
    ir_br(F, ir_ins(F, uns ? OP_ULT : OP_SLT, IT_I32, v, ir_const(F, it, (u64)c[mid].val)), lb, rb);
    F->cur = lb;
    switch_dispatch(v, it, uns, c, lo, mid, dflt);
    F->cur = rb;
    switch_dispatch(v, it, uns, c, mid, hi, dflt);
}

AsmStmt *asm_stmts;
static u32 nasm_stmts, cap_asm_stmts;

/* the number of the general register called s ("x3", "w3", "r3", "lr"), or -1 */
static int asm_reg(const char *s)
{
    int n;
    if (strcmp(s, "lr") == 0)
        return 30;
    if (strcmp(s, "fp") == 0)
        return 29;
    if ((s[0] != 'x' && s[0] != 'w' && s[0] != 'r') || s[1] < '0' || s[1] > '9')
        return -1;
    n = s[1] - '0';
    if (s[2] >= '0' && s[2] <= '9' && !s[3])
        n = n * 10 + (s[2] - '0');
    else if (s[2])
        return -1;
    return n <= 30 ? n : -1;
}

/* x86-64: the number of the register called s ("rax", "%eax", "r10", "xmm3": 32 and up), or -1 */
static int x64_asm_reg(const char *s)
{
    static const char *const names[4][8] = {
        { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi" },
        { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" },
        { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" },
        { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil" }
    };
    int k, j, n;
    if (*s == '%')
        s++;
    for (k = 0; k < 4; k++)
        for (j = 0; j < 8; j++)
            if (strcmp(s, names[k][j]) == 0)
                return j;
    if (s[0] == 'r' && s[1] >= '0' && s[1] <= '9') {
        n = s[1] - '0';
        k = 2;
        if (s[2] >= '0' && s[2] <= '9')
            n = n * 10 + (s[k++] - '0');
        if (s[k] == 'd' || s[k] == 'w' || s[k] == 'b')
            k++;
        return !s[k] && n >= 8 && n <= 15 ? n : -1;
    }
    if (strncmp(s, "xmm", 3) == 0 && s[3] >= '0' && s[3] <= '9') {
        n = s[3] - '0';
        if (s[4] >= '0' && s[4] <= '9' && !s[5])
            n = n * 10 + (s[4] - '0');
        else if (s[4])
            return -1;
        return n <= 15 ? 32 + n : -1;
    }
    return -1;
}

/* x86-64: what the constraint cs asks for: 'r' (o->reg when it names one register), 'm' or 'i' */
static int x64_asm_constraint(const char *cs, AsmOp *o)
{
    const char *q;
    int kind = 'i';
    for (q = cs; *q; q++) {
        const char *one = strchr("acdb..SD", *q);
        if (*q >= '0' && *q <= '9') {
            o->tied = (signed char)(*q - '0');
            return 'r';
        }
        if (*q != '.' && one && *one) {
            if (o->reg < 0)
                o->reg = (signed char)(one - "acdb..SD");
            return 'r';
        }
        if (*q == 'r' || *q == 'q' || *q == 'Q' || *q == 'R' || *q == 'l' || *q == 'p' || *q == 'g')
            return 'r';
        if (*q == 'm' || *q == 'o' || *q == 'V')
            kind = 'm';
    }
    return kind;
}

/* asm [qualifiers] ( "template" [: outputs [: inputs [: clobbers [: labels]]]] )
   The current token follows "asm". Inside a function it becomes a call
   (IF_ASM) whose arguments are the values the template reads, followed by
   one OP_ASMOUT for each register it writes. */
#define ASM_MAXOPS 30
static void asm_statement(int top)
{
    Buf b, c;
    Expr e, outs[ASM_MAXOPS];
    AsmOp ops[ASM_MAXOPS];
    AsmStmt *st;
    u32 args[ASM_MAXOPS], nargs = 0, nops = 0, loc = tok.loc, k, call;
    u64 clobbers = 0;
    int part = 0;

    while (tok.kind == KW(KW_VOLATILE) || tok.kind == KW(KW_INLINE) || tok.kind == KW(KW_GOTO))
        next();
    expect(T_LPAREN);
    memset(&b, 0, sizeof b);
    if (tok.kind != T_STR)
        error_at(tok.loc, "expected the assembler text");
    string_literal(&b);
    for (;;) {
        if (tok.kind == T_SCOPE)
            tok.kind = T_COLON; /* C++ read "::": the other colon is still to come */
        else if (!accept(T_COLON))
            break;
        part++;
        while (tok.kind != T_COLON && tok.kind != T_SCOPE && tok.kind != T_RPAREN) {
            if (part == 3) {
                int r;
                if (tok.kind != T_STR)
                    error_at(tok.loc, "expected the name of a clobbered register");
                memset(&c, 0, sizeof c);
                string_literal(&c);
                r = opt_arch == ARCH_X64 ? x64_asm_reg((char *)c.p) : asm_reg((char *)c.p);
                if (r >= 0)
                    clobbers |= (u64)1 << r;
                buf_free(&c);
            } else if (part > 3) {
                error_at(tok.loc, "asm goto is not supported");
            } else {
                AsmOp *o;
                const char *q;
                char *cs;
                u64 cv;
                if (nops == ASM_MAXOPS)
                    error_at(tok.loc, "too many asm operands");
                o = &ops[nops];
                memset(o, 0, sizeof *o);
                o->reg = o->tied = -1;
                if (accept(T_LBRACK)) {
                    o->name = tok.v;
                    next();
                    expect(T_RBRACK);
                }
                if (tok.kind != T_STR)
                    error_at(tok.loc, "expected an operand constraint");
                memset(&c, 0, sizeof c);
                string_literal(&c);
                cs = (char *)c.p;
                expect(T_LPAREN);
                if (tok.kind == T_IDENT && peek()->kind == T_RPAREN) {
                    Sym *s = lookup(tok.v);
                    if (s && s->kind == SK_VAR && (s->aflags & AT_REGISTER) && s->asmname)
                        o->reg = (signed char)(opt_arch == ARCH_X64 ? x64_asm_reg(intern_str(s->asmname)) : asm_reg(intern_str(s->asmname)));
                }
                expr(&e);
                expect(T_RPAREN);
                o->out = part == 1 ? (strchr(cs, '+') ? 2 : 1) : 0;
                o->kind = 'i';
                if (opt_arch == ARCH_X64 && !cur_fn_dev) {
                    o->kind = (u8)x64_asm_constraint(cs, o);
                    o->cls = (u8)(type_is_scalar(e.ty) && type_complete(e.ty) ? type_size(e.ty) : 8); /* its size */
                } else
                for (q = cs; *q; q++) {
                    if (*q >= '0' && *q <= '9') {
                        o->tied = (signed char)(*q - '0');
                        o->kind = 'r';
                        break;
                    }
                    if (*q == 'r' || *q == 'p' || (cur_fn_dev && (*q == 'l' || *q == 'f' || *q == 'd' || *q == 'h'))) {
                        o->kind = 'r';
                        o->cls = (u8)*q;
                        break;
                    }
                    if (*q == 'Q' || *q == 'm' || *q == 'o' || *q == 'U')
                        o->kind = 'm';
                }
                buf_free(&c);
                if (o->kind == 'm') {
                    if (!e.lval || e.bitwidth)
                        error_at(loc, "asm operand must be in memory");
                    args[nargs++] = e.v;
                } else if (part == 1) {
                    need_lvalue(&e);
                    if (o->kind != 'r' || !type_is_scalar(e.ty) || (type_is_float(e.ty) && !cur_fn_dev) || type_is_128(e.ty))
                        error_at(loc, "unsupported asm output operand");
                    outs[nops] = e;
                    o->ty = (u8)type_ir(e.ty);
                    if (o->out == 2) {
                        rvalue(&e);
                        args[nargs++] = e.v;
                    }
                } else {
                    rvalue(&e);
                    if (!type_is_scalar(e.ty) || (type_is_float(e.ty) && !cur_fn_dev) || type_is_128(e.ty))
                        error_at(loc, "unsupported asm input operand");
                    if (o->kind == 'r') {
                        o->ty = (u8)type_ir(e.ty);
                        args[nargs++] = e.v;
                    } else if (F->cur || top) {
                        char num[320];
                        Ins *vi = &F->ins[e.v];
                        if (ir_is_const(F, e.v, &cv)) {
                            sprintf(num, "%ld", (long)(type_ir(e.ty) == IT_I32 ? (i64)(i32)cv : (i64)cv));
                        } else if (vi->op == OP_ADDR) {
                            const char *sn;
                            if (!O->syms[vi->a].name) {
                                sprintf(num, ".Lmcc.%u", vi->a);
                                obj_bind_name(O, vi->a, intern_cstr(num));
                            }
                            sn = intern_str(O->syms[vi->a].name);
                            if (strlen(sn) > 280)
                                error_at(loc, "symbol name too long for an asm operand");
                            sprintf(num, "%s%+ld", sn, (long)(i32)vi->b);
                        } else {
                            error_at(loc, "asm operand is not a constant");
                        }
                        cs = (char *)xmalloc(strlen(num) + 1);
                        strcpy(cs, num);
                        o->text = cs;
                    }
                }
                nops++;
            }
            if (!accept(T_COMMA))
                break;
        }
    }
    expect(T_RPAREN);
    if (opt_syntax_only || opt_skip_asm || (opt_target != TARGET_LINUX && !cur_fn_dev)) {
        if (!opt_syntax_only && !opt_skip_asm)
            error_at(loc, "inline assembly is only supported for Linux targets");
        buf_free(&b);
        return;
    }
    if (top) {
        const char *file;
        u32 line, col;
        if (nops)
            error_at(loc, "asm outside a function takes no operands");
        loc_decode(loc, &file, &line, &col);
        if (opt_asm_text) {
            buf_put(opt_asm_text, b.p, strlen((char *)b.p));
            buf_u8(opt_asm_text, '\n');
        } else {
            gas_text((char *)b.p, strlen((char *)b.p), SEC_TEXT, file, line, 1);
        }
        buf_free(&b);
        return;
    }
    if (!F->cur) {
        buf_free(&b);
        return;
    }
    if (nasm_stmts == cap_asm_stmts) {
        cap_asm_stmts = cap_asm_stmts ? cap_asm_stmts * 2 : 64;
        asm_stmts = (AsmStmt *)xrealloc(asm_stmts, cap_asm_stmts * sizeof(AsmStmt));
    }
    st = &asm_stmts[nasm_stmts];
    st->tmpl = (char *)b.p;
    st->nops = nops;
    st->ops = (AsmOp *)xmalloc((nops + 1) * sizeof(AsmOp));
    memcpy(st->ops, ops, nops * sizeof(AsmOp));
    st->clobbers = clobbers;
    st->loc = loc;
    st->dev = (u8)(cur_fn_dev != 0);
    call = ir_call(F, IT_VOID, ir_const(F, IT_I64, nasm_stmts++), args, nargs, nargs, IF_ASM);
    for (k = 0; k < nops; k++)
        if (ops[k].kind == 'r' && ops[k].out) {
            Expr val;
            u32 v = ir_raw(F, OP_ASMOUT, ops[k].ty, call, k);
            ir_append(F, F->cur, v);
            set_expr(&val, outs[k].ty, v, 0);
            store_expr(&outs[k], &val);
        }
}

/* ---- #pragma unroll ---- */

/* Whether the n tokens at t are an expression that reading twice does no harm: no calls, nothing assigned. */
static int unroll_pure(const Token *t, u32 n)
{
    u32 k;
    for (k = 0; k < n; k++) {
        int kd = t[k].kind;
        if (kd == T_INC || kd == T_DEC || (kd >= T_ASSIGN && kd <= T_OREQ) || kd == T_LBRACE || kd == T_COMMA)
            return 0;
        if (kd == T_LPAREN && k && (t[k - 1].kind == T_IDENT || t[k - 1].kind == T_RPAREN || t[k - 1].kind == T_RBRACK || t[k - 1].kind == T_GT))
            return 0;
    }
    return 1;
}

/* v as a value of the integer type ty. */
static i64 unroll_fit(Type *ty, i64 v)
{
    u32 bits = (u32)type_size(ty) * 8;
    if (bits >= 64)
        return v;
    if (type_is_unsigned(ty))
        return (i64)((u64)v & (((u64)1 << bits) - 1));
    return (i64)((u64)v << (64 - bits)) >> (64 - bits);
}

/* The value of the n tokens at t (the last a semicolon) if they are a constant, with the counter s as it is. */
static int unroll_const(const Token *t, u32 n, i64 *val)
{
    Expr e;
    u64 c = 0;
    if (n < 2 || !unroll_pure(t, n - 1))
        return 0;
    replay(t, n);
    assign_expr(&e);
    rvalue(&e);
    if (tok.kind != T_SEMI)
        error_at(tok.loc, "expected ';'");
    next();
    if (!type_is_int(e.ty) || !ir_is_const(F, e.v, &c))
        return 0;
    *val = type_is_unsigned(e.ty) ? (i64)c : unroll_fit(e.ty, (i64)c);
    return 1;
}

/* The variable that the first part of a for statement (its n tokens at t, through the semicolon) sets to a
   constant and nothing else, and that constant: the loop's counter. 0 if it is not of that form. */
static Sym *unroll_var(const Token *t, u32 n, i64 *val)
{
    u32 k, depth = 0, eq = 0;
    Sym *s;
    for (k = 0; k + 1 < n; k++) {
        int kd = t[k].kind;
        if (kd == T_LPAREN || kd == T_LBRACK || kd == T_LBRACE)
            depth++;
        else if (kd == T_RPAREN || kd == T_RBRACK || kd == T_RBRACE)
            depth--;
        else if (depth == 0 && kd == T_COMMA)
            return 0;
        else if (depth == 0 && kd == T_ASSIGN && !eq)
            eq = k;
    }
    if (!eq || t[eq - 1].kind != T_IDENT)
        return 0;
    s = lookup(t[eq - 1].v);
    if (!s || s->kind != SK_VAR || !s->local || s->cv || s->byref || s->real || !type_is_int(s->ty) || type_size(s->ty) > 8)
        return 0;
    if (!unroll_const(t + eq + 1, n - eq - 1, val))
        return 0;
    return s;
}

/* The step of the loop, n tokens at t (the last a semicolon), is one that is understood: s++, ++s, s--, --s,
   s += c, s -= c, s *= c, s <<= c, s >>= c or s = c, for a c that is constant given s. With val, *val is the
   counter and becomes what the step makes of it. */
/* A loop body that can be read more than once without asking for it: no labels, no static objects. */
static int unroll_plain(const Token *t, u32 n)
{
    u32 k;
    for (k = 0; k < n; k++) {
        int kd = t[k].kind;
        if (kd == KW(KW_GOTO) || kd == KW(KW_STATIC) || kd == KW(KW_CASE) || kd == KW(KW_DEFAULT))
            return 0;
        if (kd == T_IDENT && k + 1 < n && t[k + 1].kind == T_COLON &&
            (k == 0 || t[k - 1].kind == T_SEMI || t[k - 1].kind == T_LBRACE || t[k - 1].kind == T_RBRACE || t[k - 1].kind == T_RPAREN))
            return 0;
    }
    return 1;
}

static int unroll_quiet; /* a step that is not a constant is no error: the loop was not asked to be unrolled */

static int unroll_step(Sym *s, const Token *t, u32 n, i64 *val)
{
    i64 c = 0, v = val ? *val : 0;
    int op;
    if (n == 3 && ((t[0].kind == T_IDENT && t[0].v == s->name && (t[1].kind == T_INC || t[1].kind == T_DEC)) ||
                   (t[1].kind == T_IDENT && t[1].v == s->name && (t[0].kind == T_INC || t[0].kind == T_DEC)))) {
        op = t[0].kind == T_IDENT ? t[1].kind : t[0].kind;
        if (val)
            *val = op == T_INC ? v + 1 : v - 1;
        return 1;
    }
    if (n < 4 || t[0].kind != T_IDENT || t[0].v != s->name)
        return 0;
    op = t[1].kind;
    if (op != T_ASSIGN && op != T_ADDEQ && op != T_SUBEQ && op != T_MULEQ && op != T_SHLEQ && op != T_SHREQ)
        return 0;
    if (!unroll_pure(t + 2, n - 3))
        return 0;
    if (!val)
        return 1;
    if (!unroll_const(t + 2, n - 2, &c)) {
        if (unroll_quiet)
            return 0;
        error_at(t[0].loc, "#pragma unroll: the step of the loop is not a constant");
    }
    switch (op) {
    case T_ASSIGN: v = c; break;
    case T_ADDEQ: v = (i64)((u64)v + (u64)c); break;
    case T_SUBEQ: v = (i64)((u64)v - (u64)c); break;
    case T_MULEQ: v = (i64)((u64)v * (u64)c); break;
    case T_SHLEQ: v = (i64)((u64)v << (c & 63)); break;
    default: v = type_is_unsigned(s->ty) ? (i64)((u64)v >> (c & 63)) : v >> (c & 63); break;
    }
    *val = v;
    return 1;
}

/* Whether the n tokens at t may change the variable called name. */
static int unroll_writes(u32 name, const Token *t, u32 n)
{
    u32 k;
    for (k = 0; k < n; k++) {
        if (t[k].kind != T_IDENT || t[k].v != name)
            continue;
        if (k && (t[k - 1].kind == T_INC || t[k - 1].kind == T_DEC))
            return 1;
        if (k + 1 < n && (t[k + 1].kind == T_INC || t[k + 1].kind == T_DEC || (t[k + 1].kind >= T_ASSIGN && t[k + 1].kind <= T_OREQ)))
            return 1;
    }
    return 0;
}

/* The tokens that were passed while grabbing, and one more if extra is given. */
static Token *grab_done(u32 *np, const Token *extra)
{
    u32 n = ngrab + (extra ? 1 : 0);
    Token *out = (Token *)arena_alloc(&A, (n ? n : 1) * sizeof(Token));
    grabbing = 0;
    memcpy(out, grabbed, ngrab * sizeof(Token));
    if (extra)
        out[ngrab] = *extra;
    *np = n;
    return out;
}

/* Passes the tokens up to one of kind end outside all brackets, which stays the current one. */
static void grab_to(int end)
{
    u32 depth = 0;
    ngrab = 0;
    grabbing = 1;
    for (;;) {
        int k = tok.kind;
        if (k == T_EOF)
            error_at(tok.loc, "unexpected end of file");
        if (k == end && depth == 0)
            return;
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE)
            depth++;
        else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE)
            depth--;
        next();
    }
}

static void statement(void)
{
    u32 saved_break = break_blk, saved_cont = cont_blk, saved_bl = break_level, saved_cl = cont_level;
    u32 loc = tok.loc;
    int want = sv_pending;
    Expr e;

    sv_pending = 0;
    if (tok.kind == T_IDENT && id_mcc_unroll && tok.v == id_mcc_unroll) {
        /* #pragma unroll [n] */
        i64 n = 0;
        next();
        if (tok.kind != KW(KW_FOR) && tok.kind != KW(KW_WHILE) && tok.kind != KW(KW_DO))
            n = const_int_expr();
        if (tok.kind == KW(KW_FOR))
            unroll_req = n > 0 && n < 65536 ? (u32)n : 65536;
        statement();
        unroll_req = 0;
        return;
    }
    switch (tok.kind) {
    case T_LBRACE:
        next();
        compound(1);
        return;
    case T_SEMI:
        next();
        return;
    case KW(KW_ASM):
        next();
        asm_statement(0);
        expect(T_SEMI);
        return;
    case KW(KW_LABEL):
        next();
        for (;;) {
            Label *l;
            if (tok.kind != T_IDENT)
                error_at(tok.loc, "expected a label name");
            l = (Label *)arena_alloc(&A, sizeof(Label));
            l->name = tok.v;
            l->blk = ir_new_block(F);
            l->loc = tok.loc;
            l->local = level;
            l->defined = 0;
            l->next = labels;
            labels = l;
            next();
            if (!accept(T_COMMA))
                break;
        }
        expect(T_SEMI);
        sv_pending = want;
        return;
    case KW(KW_ATTRIBUTE): {
        Attr sa; /* __attribute__((fallthrough)); */
        attributes(&sa);
        expect(T_SEMI);
        return;
    }
    case KW(KW_IF): {
        u32 c, tb, eb, end;
        next();
        if (opt_cxx && tok.kind == T_IDENT && tok.v == id_constexpr) {
            /* only the branch that is taken is read as code */
            i64 on;
            next();
            expect(T_LPAREN);
            on = const_int_expr();
            expect(T_RPAREN);
            if (on)
                statement();
            else
                skip_statement();
            if (accept(KW(KW_ELSE))) {
                if (on)
                    skip_statement();
                else
                    statement();
            }
            return;
        }
        if (opt_cxx && tok.kind == T_LPAREN && is_declstart(peek())) {
            /* if (type name = value): the variable lives as long as the statement */
            u32 nd, k, depth = 0;
            Token *d = grab_balanced(&nd);
            int is_decl = 0;
            for (k = 0; k < nd; k++) {
                if (d[k].kind == T_LPAREN || d[k].kind == T_LBRACK || d[k].kind == T_LBRACE)
                    depth++;
                else if (d[k].kind == T_RPAREN || d[k].kind == T_RBRACK || d[k].kind == T_RBRACE)
                    depth--;
                else if (d[k].kind == T_ASSIGN && depth == 1)
                    is_decl = 1;
            }
            if (is_decl) {
                Token again[4];
                d[nd - 1].kind = T_SEMI;
                push_scope();
                replay(d + 1, nd - 1);
                declaration(0);
                memset(again, 0, sizeof again);
                again[0].kind = KW(KW_IF);
                again[1].kind = T_LPAREN;
                again[2].kind = T_IDENT;
                again[2].v = scopes[level].syms->name;
                again[3].kind = T_RPAREN;
                again[0].loc = again[1].loc = again[2].loc = again[3].loc = loc;
                replay(again, 4);
                statement();
                pop_scope();
                return;
            }
            replay(d, nd);
        }
        c = condition();
        if (inl.on && inl.cx) {
            u64 cv = 0;
            if (ir_is_const(F, c, &cv)) {
                if (cv)
                    statement();
                else
                    skip_statement();
                if (accept(KW(KW_ELSE))) {
                    if (cv)
                        skip_statement();
                    else
                        statement();
                }
                return;
            }
        }
        tb = ir_new_block(F);
        eb = ir_new_block(F);
        ir_br(F, c, tb, eb);
        F->cur = tb;
        statement();
        if (accept(KW(KW_ELSE))) {
            end = ir_new_block(F);
            ir_jmp(F, end);
            F->cur = eb;
            statement();
            ir_jmp(F, end);
            F->cur = end;
        } else {
            ir_jmp(F, eb);
            F->cur = eb;
        }
        return;
    }
    case KW(KW_WHILE): {
        /* if (cond) do body while (cond); */
        u32 latch = ir_new_block(F), body = ir_new_block(F), end = ir_new_block(F), n, depth = 0;
        Token *ct;
        next();
        if (opt_size) {
            /* one copy of the test: enter at the bottom. The condition is
               still read here, as unreachable code, to record its tokens. */
            ir_jmp(F, latch);
            F->cur = 0;
        }
        rec_start();
        ir_br(F, condition(), body, end);
        for (n = 0; n < nrec; n++) {
            if (rec[n].kind == T_LPAREN)
                depth++;
            else if (rec[n].kind == T_RPAREN && --depth == 0)
                break;
        }
        n++;
        ct = rec_stop(n);
        F->cur = body;
        F->blk[body].lhdr = 1;
        break_blk = end;
        break_level = level;
        cont_blk = latch;
        cont_level = level;
        statement();
        ir_jmp(F, latch);
        F->cur = latch;
        replay(ct, n);
        ir_br(F, condition(), body, end);
        F->cur = end;
        break;
    }
    case KW(KW_DO): {
        u32 body = ir_new_block(F), cond = ir_new_block(F), end = ir_new_block(F);
        next();
        ir_jmp(F, body);
        F->cur = body;
        break_blk = end;
        break_level = level;
        cont_blk = cond;
        cont_level = level;
        statement();
        ir_jmp(F, cond);
        F->cur = cond;
        if (!accept(KW(KW_WHILE)))
            error_at(tok.loc, "expected 'while'");
        ir_br(F, condition(), body, end);
        expect(T_SEMI);
        F->cur = end;
        break;
    }
    case KW(KW_FOR): {
        /* the condition guards the loop and is tested again after each step */
        u32 ur = unroll_req > 1 && !opt_nounroll ? unroll_req : 0;
        int once = unroll_req == 1; /* #pragma unroll 1 */
        /* device code: a short loop that counts to a constant is unrolled unasked, as nvcc does */
        int au = !ur && !unroll_req && cur_fn_dev && !opt_nounroll && !opt_size && !opt_noauto && au_n++ < opt_aumax;
        u32 for_loc = tok.loc;
        u32 body = ur || au ? 0 : ir_new_block(F), step = ur || au ? 0 : ir_new_block(F), end = ur || au ? 0 : ir_new_block(F), n = 0, depth = 0, test = 0;
        Token *ct = 0;
        Sym *uv = 0;
        i64 uval = 0;
        unroll_req = 0;
        next();
        expect(T_LPAREN);
        push_scope();
        if (au && opt_autolog)
            warn_at(for_loc, "loop %u looked at", au_n);
        if (ur || au) {
            ngrab = 0;
            grabbing = 1;
        }
        if (is_declstart(&tok)) {
            declaration(0);
        } else {
            if (tok.kind != T_SEMI)
                expr(&e);
            expect(T_SEMI);
        }
        if (ur || au) {
            u32 in;
            Token *itk = grab_done(&in, 0);
            uv = unroll_var(itk, in, &uval);
        }
        if (uv && tok.kind != T_SEMI) {
            /* #pragma unroll: the body and the step are read once for each time the condition, read before
               each, comes out as a constant that is true; the loop ends where it is a constant that is false.
               Where it is not known (or after ur times) what is left is an ordinary loop. */
            Token *ctk, *stk, *btk, semi;
            u32 cn, sn, bn, copies = 0, c, cnt, fac = 0, j;
            u64 cv = 0;
            int done = 0;
            grab_to(T_SEMI);
            next();
            ctk = grab_done(&cn, 0);
            semi = ctk[cn - 1];
            grab_to(T_RPAREN);
            stk = grab_done(&sn, &semi);
            expect(T_RPAREN);
            ngrab = 0;
            grabbing = 1;
            skip_statement();
            btk = grab_done(&bn, 0);
            if (!unroll_step(uv, stk, sn, 0) || unroll_writes(uv->name, btk, bn) || unroll_writes(uv->name, ctk, cn))
                ur = 0, au = 0; /* not a loop that counts in a way that is understood: an ordinary one */
            if (au) {
                /* how often the loop runs, if reading the condition with the counter known tells */
                i64 v = uval;
                u32 k, keep = F->cur;
                F->cur = ir_new_block(F); /* what reading the condition writes is never run */
                for (k = 0; k <= UNROLL_AUTO_N; k++) {
                    v = unroll_fit(uv->ty, v);
                    uv->cv = 1;
                    uv->val = v;
                    replay(ctk, cn);
                    expr(&e);
                    rvalue(&e);
                    c = to_bool(&e);
                    expect(T_SEMI);
                    if (!ir_is_const(F, c, &cv))
                        break;
                    if (!cv) {
                        if (k && (u64)k * bn <= UNROLL_AUTO_SIZE && unroll_plain(btk, bn) && auto_n < opt_automax) {
                            /* how much code the body is, the loops inside it unrolled: it is read once, into
                               blocks that are never run */
                            u32 n0 = F->nins, s_break = break_blk, s_cont = cont_blk, grown;
                            int s_bl = break_level, s_cl = cont_level;
                            F->cur = ir_new_block(F);
                            break_blk = cont_blk = ir_new_block(F);
                            break_level = cont_level = level;
                            uv->val = unroll_fit(uv->ty, uval);
                            replay(btk, bn);
                            statement();
                            break_blk = s_break;
                            cont_blk = s_cont;
                            break_level = s_bl;
                            cont_level = s_cl;
                            grown = F->nins - n0;
                            if ((u64)grown * k <= UNROLL_AUTO_INS) {
                                auto_n++;
                                ur = k + 1;
                                if (opt_autolog)
                                    warn_at(tok.loc, "loop %u unrolled unasked: %u times %u instructions", auto_n, k, grown);
                            }
                        }
                        break;
                    }
                    unroll_quiet = 1;
                    if (!unroll_step(uv, stk, sn, &v))
                        k = UNROLL_AUTO_N;
                    unroll_quiet = 0;
                }
                uv->cv = 0;
                F->cur = keep;
            }
            if (ur > 1 && ur < 65536 && !au && !opt_unrollpeel) {
                /* #pragma unroll N, a loop that runs a known T > N times: T % N copies, then a loop whose body is N
                   copies (as nvcc reads it), not N copies and then a loop of one */
                i64 v = uval;
                u32 k, keep = F->cur;
                F->cur = ir_new_block(F); /* what reading the condition writes is never run */
                for (k = 0; k <= 4096; k++) {
                    v = unroll_fit(uv->ty, v);
                    uv->cv = 1;
                    uv->val = v;
                    replay(ctk, cn);
                    expr(&e);
                    rvalue(&e);
                    c = to_bool(&e);
                    expect(T_SEMI);
                    if (!ir_is_const(F, c, &cv))
                        break;
                    if (!cv) {
                        if (k > ur) {
                            fac = ur;
                            ur = k % fac;
                        }
                        break;
                    }
                    unroll_quiet = 1;
                    if (!unroll_step(uv, stk, sn, &v))
                        k = 4096;
                    unroll_quiet = 0;
                }
                uv->cv = 0;
                F->cur = keep;
            }
            end = ir_new_block(F);
            for (;;) {
                /* the counter is a constant while the condition and the body are read (and has its value in memory) */
                uval = unroll_fit(uv->ty, uval);
                store_val(uv->ty, ir_slot_addr(F, uv->slot, 0), ir_const(F, type_ir(uv->ty), (u64)uval));
                uv->cv = (u8)(ur != 0);
                uv->val = uval;
                replay(ctk, cn);
                expr(&e);
                rvalue(&e);
                c = to_bool(&e);
                expect(T_SEMI);
                if (!ir_is_const(F, c, &cv) || copies >= ur || ((u64)copies + 1) * bn > 3000000)
                    break;
                if (!cv) {
                    done = 1;
                    break;
                }
                cnt = ir_new_block(F);
                break_blk = end;
                break_level = level;
                cont_blk = cnt;
                cont_level = level;
                replay(btk, bn);
                statement();
                ir_jmp(F, cnt);
                F->cur = cnt;
                unroll_step(uv, stk, sn, &uval);
                copies++;
            }
            uv->cv = 0;
            if (done) {
                ir_jmp(F, end);
            } else {
                body = ir_new_block(F);
                step = ir_new_block(F);
                ir_br(F, c, body, end);
                F->cur = step;
                replay(stk, sn);
                if (tok.kind != T_SEMI)
                    expr(&e);
                expect(T_SEMI);
                replay(ctk, cn);
                expr(&e);
                rvalue(&e);
                ir_br(F, to_bool(&e), body, end);
                expect(T_SEMI);
                F->cur = body;
                F->blk[body].lhdr = 1;
                F->blk[body].nounroll = fac != 0;
                for (j = 0; j < (fac ? fac : 1); j++) {
                    /* (fac copies: the condition holds between them, the loop running a multiple of fac times) */
                    cnt = j + 1 < fac ? ir_new_block(F) : step;
                    break_blk = end;
                    break_level = level;
                    cont_blk = cnt;
                    cont_level = level;
                    replay(btk, bn);
                    statement();
                    ir_jmp(F, cnt);
                    if (cnt != step) {
                        F->cur = cnt;
                        replay(stk, sn);
                        if (tok.kind != T_SEMI)
                            expr(&e);
                        expect(T_SEMI);
                    }
                }
            }
            F->cur = end;
            pop_scope();
            break;
        }
        if (ur || au) {
            body = ir_new_block(F);
            step = ir_new_block(F);
            end = ir_new_block(F);
        }
        if (tok.kind != T_SEMI) {
            if (opt_size) {
                test = ir_new_block(F);
                ir_jmp(F, test);
                F->cur = 0;
            }
            rec_start();
            expr(&e);
            rvalue(&e);
            ir_br(F, to_bool(&e), body, end);
            expect(T_SEMI);
            for (n = 0; n < nrec; n++) {
                int kd = rec[n].kind;
                if (kd == T_LPAREN || kd == T_LBRACK || kd == T_LBRACE)
                    depth++;
                else if (kd == T_RPAREN || kd == T_RBRACK || kd == T_RBRACE)
                    depth--;
                else if (kd == T_SEMI && depth == 0)
                    break;
            }
            n++;
            ct = rec_stop(n);
        } else {
            ir_jmp(F, body);
            expect(T_SEMI);
        }
        F->cur = step;
        if (tok.kind != T_RPAREN)
            expr(&e);
        expect(T_RPAREN);
        if (ct) {
            if (test) {
                ir_jmp(F, test);
                F->cur = test;
            }
            replay(ct, n);
            expr(&e);
            rvalue(&e);
            ir_br(F, to_bool(&e), body, end);
            expect(T_SEMI);
        } else {
            ir_jmp(F, body);
        }
        F->cur = body;
        F->blk[body].lhdr = 1;
        F->blk[body].nounroll = (u8)once;
        break_blk = end;
        break_level = level;
        cont_blk = step;
        cont_level = level;
        statement();
        ir_jmp(F, step);
        F->cur = end;
        pop_scope();
        break;
    }
    case KW(KW_SWITCH): {
        Switch sw;
        u32 dispatch = ir_new_block(F), end = ir_new_block(F), v, n;
        next();
        expect(T_LPAREN);
        expr(&e);
        rvalue(&e);
        expect(T_RPAREN);
        if (!type_is_int(e.ty))
            error_at(loc, "switch quantity is not an integer");
        convert(&e, type_promote(e.ty));
        v = e.v;
        sw.first_case = ncases;
        sw.default_blk = 0;
        sw.ty = e.ty;
        sw.outer = cur_switch;
        cur_switch = &sw;
        ir_jmp(F, dispatch);
        break_blk = end;
        break_level = level;
        statement();
        ir_jmp(F, end);
        cur_switch = sw.outer;
        n = ncases - sw.first_case;
        qsort(cases + sw.first_case, n, sizeof(Case), case_cmp);
        {
            u32 k;
            for (k = 1; k < n; k++)
                if (cases[sw.first_case + k].val <= cases[sw.first_case + k - 1].hi)
                    error_at(loc, "duplicate case value");
        }
        F->cur = dispatch;
        switch_dispatch(v, type_ir(sw.ty), type_is_unsigned(sw.ty), cases + sw.first_case, 0, n,
                        sw.default_blk ? sw.default_blk : end);
        ncases = sw.first_case;
        F->cur = end;
        break;
    }
    case KW(KW_CASE):
    case KW(KW_DEFAULT): {
        u32 blk = ir_new_block(F);
        if (!cur_switch)
            error_at(loc, "case label outside a switch");
        if (tok.kind == KW(KW_CASE)) {
            i64 val, hi;
            next();
            val = hi = const_int_expr();
            if (accept(T_ELLIPSIS)) /* GNU: case lo ... hi */
                hi = const_int_expr();
            if (type_ir(cur_switch->ty) == IT_I32) {
                val = type_is_unsigned(cur_switch->ty) ? (i64)(u32)val : (i64)(i32)val;
                hi = type_is_unsigned(cur_switch->ty) ? (i64)(u32)hi : (i64)(i32)hi;
            }
            if (hi < val)
                error_at(loc, "case range is empty");
            if (ncases == cases_cap) {
                cases_cap = cases_cap ? cases_cap * 2 : 64;
                cases = (Case *)xrealloc(cases, cases_cap * sizeof(Case));
            }
            cases[ncases].val = val;
            cases[ncases].hi = hi;
            cases[ncases].blk = blk;
            ncases++;
        } else {
            next();
            if (cur_switch->default_blk)
                error_at(loc, "multiple default labels");
            cur_switch->default_blk = blk;
        }
        expect(T_COLON);
        ir_jmp(F, blk);
        F->cur = blk;
        statement();
        return;
    }
    case KW(KW_BREAK):
        next();
        if (!break_blk)
            error_at(loc, "break outside a loop or switch");
        if (ncleanup)
            cleanups_run(break_level);
        ir_jmp(F, break_blk);
        expect(T_SEMI);
        return;
    case KW(KW_CONTINUE):
        next();
        if (!cont_blk)
            error_at(loc, "continue outside a loop");
        if (ncleanup)
            cleanups_run(cont_level);
        ir_jmp(F, cont_blk);
        expect(T_SEMI);
        return;
    case KW(KW_RETURN):
        next();
        if (inl.on) {
            inline_return();
            return;
        }
        if (cur_ret->kind == TY_REF) {
            ref_bind = 1;
            expr(&e);
            ref_bind = 0;
            ref_addr(&e, cur_ret->base);
            ret_ins(e.v);
            expect(T_SEMI);
            return;
        }
        if (tok.kind != T_SEMI) {
            if (opt_cxx && tok.kind == T_LBRACE && cur_ret->kind != TY_VOID)
                compound_literal(&e, unqual(cur_ret));
            else
                expr(&e);
            rvalue(&e);
            if (cur_ret->kind != TY_VOID && special_pair(cur_ret, e.ty))
                convert(&e, cur_ret);
            if (cur_ret->kind == TY_VOID) {
                ret_ins(0);
            } else if (type_is_record(cur_ret)) {
                if (!type_is_record(e.ty) || e.ty->canon != cur_ret->canon)
                    error_at(loc, "incompatible return type");
                emit_mem(OP_MEMCPY, ir_load(F, IT_I64, M_64, sret_slot), e.v, type_size(cur_ret), type_align(cur_ret));
                ret_ins(0);
            } else if (type_is_128(cur_ret)) {
                u32 to;
                if (!type_is_scalar(e.ty))
                    error_at(loc, "incompatible return type");
                convert(&e, cur_ret);
                to = ir_load(F, IT_I64, M_64, sret_slot);
                ir_store(F, M_64, to, e.v);
                ir_store(F, M_64, ir_ins(F, OP_ADD, IT_I64, to, ir_const(F, IT_I64, 8)), e.hi);
                ret_ins(0);
            } else {
                if (!type_is_scalar(e.ty))
                    error_at(loc, "incompatible return type");
                convert(&e, cur_ret);
                ret_ins(e.v);
            }
        } else {
            if (cur_ret->kind == TY_VOID || type_is_record(cur_ret) || type_is_128(cur_ret))
                ret_ins(0);
            else if (type_is_float(cur_ret))
                ret_ins(ir_fconst(F, type_ir(cur_ret), 0.0));
            else
                ret_ins(ir_const(F, type_ir(cur_ret), 0));
        }
        expect(T_SEMI);
        return;
    case KW(KW_GOTO): {
        Label *l;
        next();
        if (tok.kind == T_STAR) { /* GNU: goto *p, by way of the function's one dispatch block */
            next();
            expr(&e);
            rvalue(&e);
            if (!cgoto_blk) {
                cgoto_blk = ir_new_block(F);
                cgoto_slot = ir_new_slot(F, 8, 8);
            }
            if (F->cur) {
                ir_store(F, M_64, ir_slot_addr(F, cgoto_slot, 0), e.v);
                ir_jmp(F, cgoto_blk);
            }
            expect(T_SEMI);
            return;
        }
        if (tok.kind != T_IDENT)
            error_at(tok.loc, "expected label name");
        l = get_label(tok.v);
        next();
        ir_jmp(F, l->blk);
        expect(T_SEMI);
        return;
    }
    case T_IDENT:
        if (peek()->kind == T_COLON) {
            Label *l = get_label(tok.v);
            if (l->defined)
                error_at(loc, "duplicate label '%s'", intern_str(tok.v));
            l->defined = 1;
            next();
            next();
            ir_jmp(F, l->blk);
            F->cur = l->blk;
            if (tok.kind == KW(KW_ATTRIBUTE)) {
                Attr la;
                attributes(&la);
            }
            if (tok.kind == T_RBRACE)
                return; /* GNU: a label at the end of a block */
            if (opt_cxx && is_declstart(&tok)) {
                declaration(0);
                return;
            }
            sv_pending = want;
            statement();
            return;
        }
        /* fall through */
    default:
        expr(&e);
        if (want) {
            rvalue(&e);
            sv_val = e;
            sv_has = 1;
        }
        expect(T_SEMI);
        return;
    }
    break_blk = saved_break;
    cont_blk = saved_cont;
    break_level = saved_bl;
    cont_level = saved_cl;
}

static void compound(int new_scope)
{
    if (new_scope)
        push_scope();
    while (tok.kind != T_RBRACE) {
        if (tok.kind == T_EOF)
            error_at(tok.loc, "expected '}'");
        block_item();
    }
    if (ncleanup) {
        cleanups_run(level - 1);
        while (ncleanup && cleanups[ncleanup - 1].level >= level)
            ncleanup--;
    }
    next();
    if (new_scope)
        pop_scope();
}

static void block_item(void)
{
    if (tok.kind == KW(KW_EXTENSION) && is_declstart(peek()))
        next();
    if (opt_cxx && tok.kind == T_IDENT && decl_qualified()) {
        /* Class<args>::member: a type, which starts a declaration, or a function or a constant, which does not */
        static u32 seq;
        char tmp[32];
        Token it;
        u32 loc = tok.loc;
        Type *t = cxx_named_type();
        Sym *al;
        if (cxx_member) {
            al = (Sym *)arena_alloc(&A, sizeof(Sym));
            *al = *cxx_member;
            cxx_member = 0;
        } else {
            al = new_sym(0, SK_TYPEDEF, t);
        }
        sprintf(tmp, "__cxx_b%u", seq++);
        al->name = intern_cstr(tmp);
        al->real = 0;
        bind(al);
        memset(&it, 0, sizeof it);
        it.kind = T_IDENT;
        it.v = al->name;
        it.loc = loc;
        replay(&it, 1);
    }
    if (is_declstart(&tok) && !(tok.kind == T_IDENT && peek()->kind == T_COLON))
        declaration(0);
    else
        statement();
}

/* ---- functions ---- */

/* Linux target: functions wait, as parsed, until they are needed. */
typedef struct LazyFn {
    u32 sec;
    u8 has, done, noemit;
    u8 dev;    /* CUDA: 1 device code only, 2 host and device */
    u8 kernel; /* CUDA: a __global__ function */
    u8 ptx;    /* its PTX has been written */
    u32 maxntid; /* a kernel's launch_bounds */
} LazyFn;

static LazyFn *lazy;
static u32 nlazy;

static void nlazy_reset(void)
{
    if (nlazy)
        memset(lazy, 0, nlazy * sizeof(LazyFn));
    shared_seq = 0;
}

static LazyFn *lazy_fn(u32 sym)
{
    if (sym >= nlazy) {
        u32 n = nlazy ? nlazy : 256;
        while (n <= sym)
            n *= 2;
        lazy = (LazyFn *)xrealloc(lazy, n * sizeof(LazyFn));
        memset(lazy + nlazy, 0, (n - nlazy) * sizeof(LazyFn));
        nlazy = n;
    }
    return &lazy[sym];
}

static void lazy_generate(u32 sym)
{
    u32 sec = lazy[sym].sec;
    lazy[sym].done = 1;
    func_realise(fn_func, sym);
    func_unstash(fn_func, sym);
    arm64_text_sec = (int)sec;
    arm64_gen(fn_func, O);
    arm64_text_sec = SEC_TEXT;
    O->syms[sym].size = O->sec[sec].len - O->syms[sym].value;
}

/* Generates the functions put aside that the code and data refer to. */
static void emit_lazy(void)
{
    u32 *seen = 0, nseen = 0, sec, k;
    for (;;) {
        int more = 0;
        if (nseen < O->nsec) {
            seen = (u32 *)xrealloc(seen, O->nsec * sizeof(u32));
            memset(seen + nseen, 0, (O->nsec - nseen) * sizeof(u32));
            nseen = O->nsec;
        }
        for (sec = 0; sec < nseen; sec++) {
            for (; seen[sec] < O->nrel[sec]; seen[sec]++) {
                u32 sym = O->rel[sec][seen[sec]].sym;
                if (sym < nlazy && lazy[sym].has && !lazy[sym].done && !lazy[sym].noemit && lazy[sym].dev != 1) {
                    lazy_generate(sym);
                    more = 1;
                }
                if (held_emit(O->rel[sec][seen[sec]].sym))
                    more = 1;
            }
        }
        /* and what assembly refers to but has not turned into relocations yet */
        for (sec = 0; (k = gas_pending(sec)) != 0; sec++) {
            if (k - 1 < nlazy && lazy[k - 1].has && !lazy[k - 1].done && !lazy[k - 1].noemit && lazy[k - 1].dev != 1) {
                lazy_generate(k - 1);
                more = 1;
            }
            if (held_emit(k - 1))
                more = 1;
        }
        if (!more && nseen == O->nsec)
            break;
    }
    free(seen);
}

/* CUDA: writes the unit's device code. Kernels come first, then whatever they call. */
static void ptx_one(u32 sym, int entry)
{
    lazy[sym].ptx = 1;
    func_realise(fn_func, sym);
    func_unstash(fn_func, sym);
    opt_finish(fn_func);
    ptx_maxntid = entry ? lazy[sym].maxntid : 0;
    ptx_gen(fn_func, O, entry);
}

static void emit_ptx(void)
{
    u32 k, sym;
    for (sym = 0; sym < nlazy; sym++)
        if (lazy[sym].has && lazy[sym].kernel && !lazy[sym].ptx) {
            ptx_one(sym, 1);
            /* on the host a kernel is only a name: launches go by it, and its address is an opaque handle */
            ir_func_reset(fn_func);
            fn_func->cur = ir_new_block(fn_func);
            fn_func->sym = sym;
            fn_func->name = "(kernel)";
            fn_func->ret_ty = IT_VOID;
            ir_ret(fn_func, 0);
            arm64_gen(fn_func, O);
        }
    while ((k = ptx_next_needed()) != 0) {
        sym = k - 1;
        if (sym < nlazy && lazy[sym].has && lazy[sym].dev)
            ptx_one(sym, 0);
        else
            ptx_extern(sym);
    }
    ptx_finish(O);
}

/* Branch to the label whose number is val, one of lo ... hi. */
static void cgoto_tree(u32 val, const u32 *blks, u32 lo, u32 hi)
{
    u32 mid, left, right;
    if (lo == hi) {
        ir_jmp(F, blks[lo]);
        return;
    }
    mid = lo + (hi - lo + 1) / 2;
    left = ir_new_block(F);
    right = ir_new_block(F);
    ir_br(F, ir_ins(F, OP_ULT, IT_I32, val, ir_const(F, IT_I64, mid)), left, right);
    F->cur = left;
    cgoto_tree(val, blks, lo, mid - 1);
    F->cur = right;
    cgoto_tree(val, blks, mid, hi);
}

static void func_def(Sym *s, Type *ty, u32 loc)
{
    u32 i, v;
    Label *l;

    if (s->defined)
        error_at(loc, "redefinition of '%s'", intern_str(s->name));
    s->defined = 1;
    if (s->aflags & AT_CTOR) {
        /* the program's start calls what this array lists */
        static const char zero[8];
        int sec;
        u64 off;
        if (opt_target != TARGET_LINUX)
            error_at(loc, "constructor functions are not supported for this target");
        sec = obj_section(O, intern_cstr(".init_array"), SF_ALLOC | SF_WRITE);
        off = obj_reserve(O, sec, 8, 8);
        buf_put(&O->sec[sec], zero, 8);
        obj_add_reloc(O, sec, (u32)off, R_ABS64, sym_obj(s), 0);
    }
    F = fn_func;
    ir_func_reset(F);
    F->cur = ir_new_block(F);
    F->name = intern_str(s->name);
    cur_fn_name = F->name;
    F->sym = sym_obj(s);
    if (opt_debug)
        dwarf_func_loc(F->sym, loc, ty->base);
    ir_loc = loc;
    cur_fn_dev = opt_cuda && (s->aflags & (AT_GLOBAL | AT_DEVICE)) != 0;
    cur_fn_kernel = cur_fn_dev && (s->aflags & AT_GLOBAL) != 0;
    opt_dev_parse = cur_fn_dev && ((s->aflags & AT_GLOBAL) || !(s->aflags & AT_HOST)); /* (not what is host code as well) */
    cur_ret = ty->base;
    labels = 0;
    ncleanup = 0;
    nlabel_index = 0;
    cgoto_blk = cgoto_slot = 0;
    break_blk = cont_blk = 0;
    cur_switch = 0;
    push_scope();

    /* K&R parameter declarations */
    while (ty->oldstyle && tok.kind != T_LBRACE) {
        int storage;
        Type *base = declspec(&storage);
        for (;;) {
            u32 name = 0;
            Type *pt = declarator(base, &name);
            for (i = 0; i < ty->nparams; i++)
                if (ty->pnames[i] == name)
                    break;
            if (!name || i == ty->nparams)
                error_at(tok.loc, "declaration for a missing parameter");
            if (pt->kind == TY_ARRAY)
                pt = type_ptr(pt->base);
            else if (pt->kind == TY_FUNC)
                pt = type_ptr(pt);
            ty->params[i] = pt;
            if (!accept(T_COMMA))
                break;
        }
        expect(T_SEMI);
    }

    F->ret_ty = (u8)(type_is_record(cur_ret) || type_is_128(cur_ret) ? IT_VOID : type_ir(cur_ret));
    F->variadic = ty->variadic;
    {
        static const struct Inl none;
        inl = none; /* returns leave this function */
    }
    if (type_is_128(cur_ret)) {
        v = ir_add_param(F, IT_I64);
        F->has_sret = 17; /* two doublewords in registers */
        sret_slot = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0);
        ir_store(F, M_64, sret_slot, v);
    } else if (type_is_record(cur_ret)) {
        v = ir_add_param(F, IT_I64);
        F->has_sret = (u8)(abi_words(cur_ret) ? 1 + type_size(cur_ret) : 1);
        if (opt_arch == ARCH_X64) /* which halves come back in xmm registers */
            F->has_sret |= (u8)((x64_ret_flags(cur_ret) & IF_SSE0 ? 0x40 : 0) | (x64_ret_flags(cur_ret) & IF_SSE1 ? 0x80 : 0));
        sret_slot = ir_slot_addr(F, ir_new_slot(F, 8, 8), 0);
        ir_store(F, M_64, sret_slot, v);
    }
    cur_named_gr = cur_named_fr = 0;
    if (opt_arch == ARCH_X64 && F->has_sret == 1)
        cur_named_gr = 1;
    for (i = 0; i < ty->nparams; i++) {
        Type *pt = ty->params[i];
        Sym *p = new_sym(ty->pnames[i], SK_VAR, pt);
        u32 words = pt->kind == TY_REF ? 0 : abi_words(pt);
        p->local = 1;
        if (pt->kind == TY_REF) {
            /* C++: the address of the argument */
            cur_named_gr++;
            v = ir_add_param(F, IT_I64);
            p->ty = pt->base;
            p->slot = ir_new_slot(F, 8, 8);
            p->byref = 1;
            ir_store(F, M_64, ir_slot_addr(F, p->slot, 0), v);
        } else if (opt_target == TARGET_LINUX && type_is_record(pt) && type_complete(pt) && !type_size(pt)) {
            p->slot = ir_new_slot(F, 1, 1); /* empty: nothing was passed */
        } else if (opt_arch == ARCH_X64 && (type_is_record(pt) || type_is_128(pt))) {
            x64_param(p, pt);
        } else if (words || type_is_128(pt)) {
            /* one or two doublewords, kept in a slot that holds all of them */
            u32 addr;
            if (type_is_128(pt)) {
                words = 2;
                if ((cur_named_gr & 1) && cur_named_gr < 8) {
                    ir_add_param(F, IT_I64);
                    cur_named_gr++;
                }
            }
            if (words == 2 && cur_named_gr == 7) {
                ir_add_param(F, IT_I64);
                cur_named_gr++;
            }
            p->slot = ir_new_slot(F, 8 * words, type_align(pt) > 8 ? type_align(pt) : 8);
            addr = ir_slot_addr(F, p->slot, 0);
            ir_store(F, M_64, addr, ir_add_param(F, IT_I64));
            if (words == 2)
                ir_store(F, M_64, ir_slot_addr(F, p->slot, 8), ir_add_param(F, IT_I64));
            cur_named_gr += words;
        } else if (type_is_record(pt)) {
            cur_named_gr++;
            v = ir_add_param(F, IT_I64);
            p->slot = ir_new_slot(F, 8, 8);
            p->byref = 1;
            ir_store(F, M_64, ir_slot_addr(F, p->slot, 0), v);
        } else if (ty->oldstyle && pt->kind == TY_FLOAT) {
            v = ir_add_param(F, IT_F64);
            p->slot = ir_new_slot(F, 4, 4);
            ir_store(F, M_F32, ir_slot_addr(F, p->slot, 0), ir_ins(F, OP_FTRUNC, IT_F32, v, 0));
        } else {
            v = abi_extend(pt, ir_add_param(F, type_ir(pt)));
            if (cur_fn_kernel && pt->kind == TY_PTR && F->ins[v].op == OP_PARAM)
                F->ins[v].mem = PARAM_GLOBAL;
            p->slot = ir_new_slot(F, type_size(pt), type_align(pt));
            store_val(pt, ir_slot_addr(F, p->slot, 0), v);
            if (type_is_float(pt))
                cur_named_fr++;
            else
                cur_named_gr++;
        }
        if (p->name) {
            binding_params = 1;
            bind(p);
            binding_params = 0;
        }
    }
    expect(T_LBRACE);
    compound(0);
    pop_scope();

    if (F->cur) {
        if (cur_ret->kind == TY_VOID || type_is_record(cur_ret) || type_is_128(cur_ret))
            ir_ret(F, 0);
        else if (type_is_float(cur_ret))
            ir_ret(F, ir_fconst(F, type_ir(cur_ret), 0.0));
        else
            ir_ret(F, ir_const(F, type_ir(cur_ret), 0));
    }
    for (l = labels; l; l = l->next)
        if (!l->defined && !l->local)
            error_at(l->loc, "label '%s' used but not defined", intern_str(l->name));
    if (cgoto_blk) {
        u32 *blks, val;
        if (!nlabel_index)
            error_at(loc, "goto * in a function that takes the address of no label");
        blks = (u32 *)xmalloc((nlabel_index + 1) * sizeof(u32));
        for (l = labels; l; l = l->next)
            if (l->index)
                blks[l->index] = l->blk;
        F->cur = cgoto_blk;
        val = ir_load(F, IT_I64, M_64, ir_slot_addr(F, cgoto_slot, 0));
        cgoto_tree(val, blks, 1, nlabel_index);
        free(blks);
    }

    if (opt_prof_gen)
        prof_instrument(F);
    else if (opt_prof_use)
        prof_apply(F, O->name);
    if (opt_lto && !opt_dump_ir) {
        lto_save(F, O);
        F = const_func;
        return;
    }
    if (opt_syntax_only) {
        F = const_func;
        return;
    }
    if ((opt_target == TARGET_LINUX || cur_fn_dev) && !opt_dump_ir) {
        /* put aside; compiled when something that is kept refers to it */
        u32 sym = F->sym;
        LazyFn *lf = lazy_fn(sym);
        lf->has = 1;
        lf->dev = (u8)(!cur_fn_dev ? 0 : (s->aflags & AT_HOST) && !(s->aflags & AT_GLOBAL) ? 2 : 1);
        lf->kernel = (u8)((s->aflags & AT_GLOBAL) != 0);
        lf->maxntid = s->maxntid;
        lf->sec = s->section ? (u32)obj_section(O, s->section, SF_ALLOC | SF_EXEC) : SEC_TEXT;
        lf->noemit = (u8)def_extern_inline;
        /* (+4: device code, which has no vector operations) */
        func_stash(F, (s->aflags & (AT_NOINLINE | AT_WEAK) ? 2 : s->aflags & AT_ALWAYS_INLINE ? 1 : 0) + (lf->dev ? 4 : 0) + (lf->dev == 1 ? 8 : 0));
        cur_fn_dev = 0;
        opt_dev_parse = 0;
        if (lf->dev == 1) {
            F = const_func; /* its PTX is written at the end of the unit */
            return;
        }
        if (opt_target != TARGET_LINUX || (!s->is_static && !def_extern_inline) || (s->aflags & AT_USED))
            lazy_generate(sym);
        F = const_func;
        return;
    }
    opt_run(F);
    if (opt_dump_ir) {
        ir_dump(F);
    } else if (s->is_static && inline_saved(F->sym)) {
        /* small enough to be inlined wherever it is called: code for it is
           generated at the end, and only if something still refers to it */
        if (ndeferred == cap_deferred) {
            cap_deferred = cap_deferred ? cap_deferred * 2 : 64;
            deferred = (u32 *)xrealloc(deferred, cap_deferred * sizeof(u32));
        }
        deferred[ndeferred++] = F->sym;
    } else {
        arm64_gen(F, O);
    }
    F = const_func;
}

/* Generates the deferred functions that turned out to be needed; the rest
   lose their symbols. */
static void emit_deferred(void)
{
    u32 seen[SEC_COUNT], k, sec, left = ndeferred;
    u8 *state; /* by object symbol: 1 deferred, 2 generated */
    if (!ndeferred)
        return;
    state = (u8 *)xmalloc(O->nsyms);
    memset(state, 0, O->nsyms);
    for (k = 0; k < ndeferred; k++)
        state[deferred[k]] = 1;
    memset(seen, 0, sizeof seen);
    for (;;) {
        int more = 0;
        for (sec = 0; sec < SEC_COUNT; sec++) {
            for (; seen[sec] < O->nrel[sec]; seen[sec]++) {
                u32 sym = O->rel[sec][seen[sec]].sym;
                if (sym < O->nsyms && state[sym] == 1) {
                    state[sym] = 2;
                    inline_restore(fn_func, sym);
                    fn_func->name = "(static)";
                    arm64_gen(fn_func, O);
                    more = 1;
                    left--;
                }
            }
        }
        if (!more)
            break;
    }
    for (k = 0; k < ndeferred && left; k++)
        if (state[deferred[k]] == 1)
            O->syms[deferred[k]].sec = SEC_NONE;
    free(state);
    ndeferred = 0;
}

/* C++: generates the functions that templates and lambdas asked for while the last declaration was read. */
static void cxx_flush(void)
{
    int s_c = extern_c, s_anon = anon_ns;
    u32 k;
    if (level || !pend_head)
        goto force;
    extern_c = anon_ns = 0;
    while (pend_head) {
        Pend *p = pend_head;
        pend_head = p->next;
        if (!pend_head)
            pend_tail = &pend_head;
        if (p->raw) {
            replay(p->raw, p->nraw);
            declaration(1);
        } else if (p->mbody) {
            Hide saved = hide;
            push_scope();
            if (p->t) {
                hide = p->t->hide;
                bind_targs(p->t, &p->ti->args, p->ti->args.n);
                bind(new_sym(p->t->name, SK_TYPEDEF, p->ti->ty));
            }
            cs_bind(p->fn->cls);
            replay(p->mbody, p->nmbody);
            def_extern_inline = 0;
            func_def(p->fn, p->fn->ty, tok.loc);
            pop_scope();
            hide = saved;
        } else if (p->lam) {
            DOp op;
            Type *fty;
            replay(p->lam->params, p->lam->nparams);
            memset(&op, 0, sizeof op);
            param_list(&op);
            fty = type_func(p->fn->ty->base, op.params, op.pnames, op.nparams, 0, 0);
            replay(p->lam->body, p->lam->nbody);
            def_extern_inline = 0;
            func_def(p->fn, fty, tok.loc);
        } else if (!p->ti->spec && !p->ti->ext && !p->fn->defined && !lam_param(p->fn->ty)) {
            Hide saved = hide;
            hide = p->t->hide;
            push_scope();
            bind_targs(p->t, &p->ti->args, p->ti->args.n);
            inst_sym = p->fn;
            replay(p->t->toks, p->t->ntoks);
            declaration(1);
            inst_sym = 0;
            pop_scope();
            hide = saved;
        }
    }
    extern_c = s_c;
    anon_ns = s_anon;
force:
    for (k = 0; k < nforced; k++) {
        u32 sym = sym_obj(forced[k]);
        if (sym < nlazy && lazy[sym].has && !lazy[sym].done && !lazy[sym].noemit && lazy[sym].dev != 1) {
            O->syms[sym].global = 1;
            lazy_generate(sym);
        }
    }
    if (!pend_head && !level)
        nforced = 0;
}

static void top_item(void)
{
    if (accept(T_SEMI))
        return;
    if (tok.kind == KW(KW_ASM)) {
        next();
        asm_statement(1);
        expect(T_SEMI);
        return;
    }
    declaration(1);
    if (opt_cxx)
        cxx_flush();
}

void cc_compile(const char *path, Obj *o, int dump_ir)
{
    Sym *s;
    O = o;
    opt_obj = o;
    opt_dump_ir = dump_ir;
    nasm_stmts = 0; /* the texts and operand lists of the last file are not reclaimed */
    while (nheld) {
        HeldData *h = &held[--nheld];
        if (h->sym < nheld_of && held_of[h->sym]) {
            held_of[h->sym] = 0;
            free(h->bytes);
            free(h->rel);
        }
    }
    if (opt_target == TARGET_LINUX)
        gas_begin(o);
    if (opt_prof_gen)
        prof_begin(o);
    if (opt_debug)
        dwarf_reset(path);
    arena_reset(&A);
    type_init(&A);
    if (names_cap) {
        memset(idents, 0, names_cap * sizeof(Sym *));
        memset(gsyms, 0, names_cap * sizeof(Sym *));
        memset(tags, 0, names_cap * sizeof(Tag *));
    }
    level = 0;
    scopes[0].syms = 0;
    scopes[0].tags = 0;
    tentatives = 0;
    ncases = 0;
    has_ptok = 0;
    nplay = iplay = 0;
    recording = 0;
    rec_id = 0;
    while (nrec_outer)
        free(rec_outer[--nrec_outer].rec);
    ndeferred = 0;
    if (nlazy)
        memset(lazy, 0, nlazy * sizeof(LazyFn));
    nstrlits = 0;
    if (cap_strlits)
        memset(strlits, 0, cap_strlits * sizeof(StrLit));
    if (!fn_func) {
        fn_func = ir_func_new();
        const_func = ir_func_new();
    }
    ir_func_reset(const_func);
    F = const_func;
    inline_reset();
    ptx_reset();
    nlazy_reset();
    id_complex = intern_cstr("_Complex");
    id_alignas = intern_cstr("_Alignas");
    id_alignas2 = intern_cstr(opt_c23 || opt_cxx ? "alignas" : "_Alignas");
    id_float16 = intern_cstr("_Float16");
    id_bf16 = intern_cstr("__bf16");
    id_real = intern_cstr("__real__");
    id_imag = intern_cstr("__imag__");
    vectypes = 0;
    id_va_start = intern_cstr("__builtin_va_start");
    id_nullptr = intern_cstr("nullptr");
    id_true = intern_cstr("true");
    id_false = intern_cstr("false");
    has_pushback = 0;
    id_va_arg = intern_cstr("__builtin_va_arg");
    id_template = intern_cstr("template");
    id_typename = intern_cstr("typename");
    id_class = intern_cstr("class");
    id_constexpr = intern_cstr("constexpr");
    id_namespace = intern_cstr("namespace");
    id_using = intern_cstr("using");
    id_new = intern_cstr("new");
    id_delete = intern_cstr("delete");
    id_decltype = intern_cstr("decltype");
    id_public = intern_cstr("public");
    id_private = intern_cstr("private");
    id_protected = intern_cstr("protected");
    id_static_cast = intern_cstr("static_cast");
    id_reinterpret_cast = intern_cstr("reinterpret_cast");
    id_const_cast = intern_cstr("const_cast");
    id_noexcept = intern_cstr("noexcept");
    id_throw = intern_cstr("throw");
    id_mcc_unroll = intern_cstr("__mcc_unroll");
    id_mutable = intern_cstr("mutable");
    id_explicit = intern_cstr("explicit");
    id_friend = intern_cstr("friend");
    id_virtual = intern_cstr("virtual");
    id_operator = intern_cstr("operator");
    id_is_same = intern_cstr("__is_same");
    id_main = intern_cstr("main");
    cscopes = 0;
    pend_head = 0;
    pend_tail = &pend_head;
    hide.n = 0;
    extern_c = anon_ns = 0;
    nforced = 0;
    inst_sym = 0;
    inst_rec = rec_resume = 0;
    explicit_inst = spec_fn = has_decl_targs = 0;
    call_pre = 0;
    call_sym = 0;
    inl.on = 0;
    if (ns_cap)
        memset(ns_ids, 0, ns_cap);
    id_va_end = intern_cstr("__builtin_va_end");
    if (opt_arch == ARCH_X64) {
        /* an array of one { unsigned gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; }, known here
           only by its offsets: so a va_list is passed on as the address of the caller's */
        Field *fl = (Field *)arena_alloc(&A, sizeof(Field));
        memset(fl, 0, sizeof *fl);
        fl->name = intern_cstr("__x");
        fl->ty = type_array(ty_ulong, 3, 1);
        va_list_type = type_record(TY_STRUCT, intern_cstr("__va_list_tag"));
        type_layout(va_list_type, fl, 0, 0);
        bind(new_sym(intern_cstr("__builtin_va_list"), SK_TYPEDEF, type_array(va_list_type, 1, 1)));
    } else if (opt_target == TARGET_LINUX) {
        /* { void *stack, *gr_top, *vr_top; int gr_offs, vr_offs; }, known here only by its offsets */
        Field *fl = (Field *)arena_alloc(&A, sizeof(Field));
        memset(fl, 0, sizeof *fl);
        fl->name = intern_cstr("__x");
        fl->ty = type_array(ty_ulong, 4, 1);
        va_list_type = type_record(TY_STRUCT, intern_cstr("__va_list"));
        type_layout(va_list_type, fl, 0, 0);
        bind(new_sym(intern_cstr("__builtin_va_list"), SK_TYPEDEF, va_list_type));
    } else {
        bind(new_sym(intern_cstr("__builtin_va_list"), SK_TYPEDEF, type_ptr(ty_char)));
    }
    bind(new_sym(intern_cstr("__int128_t"), SK_TYPEDEF, ty_int128));
    bind(new_sym(intern_cstr("__uint128_t"), SK_TYPEDEF, ty_uint128));

    pp_open(path);
    next();
    while (tok.kind != T_EOF)
        top_item();
    for (s = tentatives; s; s = s->tent_next) {
        if (s->defined)
            continue;
        if (!type_complete(s->ty)) {
            if (s->ty->kind == TY_ARRAY && type_complete(s->ty->base))
                s->ty = type_array(s->ty->base, 1, 1);
            else
                error_at(0, "'%s' has an incomplete type", intern_str(s->name));
        }
        if (s->is_static && s->objsym == NOSYM)
            continue; /* unused */
        define_data(s, 0);
    }
    emit_deferred();
    if (opt_cuda && !opt_syntax_only && !opt_dump_ir)
        emit_ptx();
    if (opt_target == TARGET_LINUX) {
        u32 k;
        /* the targets of aliases are needed whether or not anything else uses them */
        for (k = 0; k < naliased; k++) {
            Sym *to = aliased[k]->alias < names_cap ? gsyms[aliased[k]->alias] : 0;
            u32 t;
            if (!to)
                die("alias of '%s', which is not declared", intern_str(aliased[k]->alias));
            t = sym_obj(to);
            if (t < nlazy && lazy[t].has && !lazy[t].done)
                lazy_generate(t);
            held_emit(t);
        }
        emit_lazy();
        for (k = 0; k < naliased; k++) {
            ObjSym *a, *t;
            u32 as = sym_obj(aliased[k]), ts = sym_obj(gsyms[aliased[k]->alias]);
            a = &O->syms[as];
            t = &O->syms[ts];
            if (t->sec >= 0xfff0)
                die("alias of '%s', which is not defined here", intern_str(aliased[k]->alias));
            a->sec = t->sec;
            a->value = t->value;
            a->kind = t->kind;
            a->size = t->size;
        }
        gas_end();
    }
    naliased = 0;
    if (opt_prof_gen)
        prof_end();
    if (opt_debug && opt_target == TARGET_LINUX && !opt_dump_ir && !opt_syntax_only && !opt_lto)
        dwarf_finish(O);
    ir_loc = 0;
}
