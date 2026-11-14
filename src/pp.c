/* Preprocessor: directives, macro expansion, #if evaluation. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARAMS 127
#define MAX_INCLUDE_DEPTH 200

enum { BI_NONE, BI_LINE, BI_FILE, BI_COUNTER, BI_LEVEL, BI_HAS_ATTR, BI_PRAGMA, BI_HAS_INCLUDE };
static SrcFile *find_include(const char *name, size_t namelen, int angled, int next, u32 *where);

typedef struct Macro {
    Token *body;
    u32 nbody;
    int nparams; /* -1: object-like */
    u8 busy;     /* being expanded: not available for replacement */
    u8 builtin;
    u8 simple; /* object-like with no ##: body can be replayed as is */
    u8 variadic; /* the last parameter takes all the remaining arguments */
} Macro;

/* A list of tokens being replayed: a macro expansion or an argument. */
typedef struct Src {
    Token *toks;
    u32 pos, n;
    Macro *macro; /* released when the list is exhausted */
    u32 loc;      /* location reported for every token */
    u8 lead;      /* BOL/SPACE flags for the first token */
    u8 barrier;   /* end of an isolated list: yields T_EOF */
} Src;

typedef struct Cond {
    u32 loc;
    u8 taken; /* some group of this #if chain has been taken */
    u8 has_else;
} Cond;

typedef struct Inc {
    SrcFile *file;
    Lexer lx;
    u32 cond_base;
    u32 guard;
    u32 dir; /* 1 + which include directory it was found in, or 0: #include_next starts after it */
    u8 sys;  /* found in a system directory, or included by such a file */
    u8 guard_state; /* 0 start, 1 inside candidate guard, 2 guard closed, 3 not guarded */
} Inc;

static Macro **macros; /* by name id */
static u32 macros_cap;
static Src *srcs;
static u32 nsrc, srcs_cap;
static Cond *conds;
static u32 nconds, conds_cap;
static Inc *incs;
static u32 nincs, incs_cap;
static Inc *cur;
static Token *ts; /* scratch token stack */
static u32 ts_n, ts_cap;
static Arena exp_arena;  /* expansion token lists; reset when no list is live */
static Arena perm_arena; /* macro bodies, pasted and stringified spellings */
static const char **inc_dirs;
static u32 ninc_dirs;
static const char **sys_dirs, **forced; /* -isystem, searched after the -I directories; -include */
static u32 nsys_dirs, nforced;
static u32 counter, id_va_args, id_include_next, id_warning;
static const char *prefix_old, *prefix_new;
int pp_asm; /* assembly is being preprocessed: quotes need not balance, '#' need not stringify */
static Buf predef;
static u32 pp_unit; /* the number of the translation unit being read, from 1 */
static u32 id_define, id_undef, id_include, id_ifdef, id_ifndef, id_elif, id_endif, id_line, id_error, id_pragma,
    id_defined, id_once;
#define ID_IF (KW_IF + 1)
#define ID_ELSE (KW_ELSE + 1)

static void ts_push(const Token *t)
{
    if (ts_n == ts_cap) {
        ts_cap = ts_cap ? ts_cap * 2 : 1024;
        ts = (Token *)xrealloc(ts, ts_cap * sizeof(Token));
    }
    ts[ts_n++] = *t;
}

static Macro *macro_get(u32 name)
{
    return name < macros_cap ? macros[name] : 0;
}

static void macro_set(u32 name, Macro *m)
{
    if (name >= macros_cap) {
        u32 cap = macros_cap ? macros_cap : 4096;
        while (cap <= name)
            cap *= 2;
        macros = (Macro **)xrealloc(macros, cap * sizeof(Macro *));
        memset(macros + macros_cap, 0, (cap - macros_cap) * sizeof(Macro *));
        macros_cap = cap;
    }
    macros[name] = m;
}

static char *perm_copy(const char *s, size_t n)
{
    char *p = (char *)arena_alloc_raw(&perm_arena, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void push_src(Token *toks, u32 n, Macro *m, u32 loc, u32 lead, int barrier)
{
    Src *s;
    if (nsrc == srcs_cap) {
        srcs_cap = srcs_cap ? srcs_cap * 2 : 64;
        srcs = (Src *)xrealloc(srcs, srcs_cap * sizeof(Src));
    }
    if (nsrc && srcs[nsrc - 1].loc && !barrier)
        loc = srcs[nsrc - 1].loc;
    s = &srcs[nsrc++];
    s->toks = toks;
    s->pos = 0;
    s->n = n;
    s->macro = m;
    s->loc = loc;
    s->lead = (u8)lead;
    s->barrier = (u8)barrier;
    if (m)
        m->busy = 1;
}

static void pop_src(void)
{
    Src *s = &srcs[--nsrc];
    if (s->macro)
        s->macro->busy = 0;
    if (nsrc == 0)
        arena_reset(&exp_arena);
}

static void src_take(Src *s, Token *t)
{
    *t = s->toks[s->pos++];
    if (s->loc)
        t->loc = s->loc;
    if (s->pos == 1)
        t->flags = (u8)((t->flags & ~(TF_BOL | TF_SPACE)) | s->lead);
    else
        t->flags &= ~TF_BOL;
}

static void directive(u32 loc);
static u32 id_unroll, id_mcc_unroll;
static int pragma_mark; /* the directive just read was #pragma unroll */

/* Next token with no macro expansion. Directives between the arguments of
   a macro call are obeyed. */
static void raw_next(Token *t)
{
    for (;;) {
        if (nsrc) {
            Src *s = &srcs[nsrc - 1];
            if (s->pos < s->n) {
                src_take(s, t);
                return;
            }
            if (s->barrier) {
                t->kind = T_EOF;
                return;
            }
            pop_src();
            continue;
        }
        lex_raw(&cur->lx, t);
        if (t->kind == T_HASH && (t->flags & TF_BOL)) {
            directive(t->loc);
            continue;
        }
        return;
    }
}

/* Is the next token a '('? If so consume it. */
static int next_is_lparen(void)
{
    for (;;) {
        if (nsrc) {
            Src *s = &srcs[nsrc - 1];
            if (s->pos < s->n) {
                if (s->toks[s->pos].kind != T_LPAREN)
                    return 0;
                s->pos++;
                return 1;
            }
            if (s->barrier)
                return 0;
            pop_src();
            continue;
        } else {
            Lexer save = cur->lx;
            Token t;
            lex_raw(&cur->lx, &t);
            if (t.kind == T_LPAREN)
                return 1;
            cur->lx = save;
            return 0;
        }
    }
}

static void make_token(Token *t, int kind, const char *s, size_t n, u32 loc)
{
    t->kind = (u8)kind;
    t->flags = 0;
    t->loc = loc;
    t->v = (u32)n;
    t->p = s;
}

static void stringify(Token *out, const Token *toks, u32 n, u32 loc)
{
    Buf b;
    u32 i;
    memset(&b, 0, sizeof b);
    buf_u8(&b, '"');
    for (i = 0; i < n; i++) {
        const Token *t = &toks[i];
        if (i && (t->flags & TF_SPACE))
            buf_u8(&b, ' ');
        if (t->kind == T_STR || t->kind == T_CHAR) {
            u32 k;
            for (k = 0; k < t->v; k++) {
                if (t->p[k] == '"' || t->p[k] == '\\')
                    buf_u8(&b, '\\');
                buf_u8(&b, (u8)t->p[k]);
            }
        } else {
            tok_spell(t, &b);
        }
    }
    buf_u8(&b, '"');
    make_token(out, T_STR, perm_copy((const char *)b.p, b.len), b.len, loc);
    buf_free(&b);
}

/* lhs ## rhs, result in *lhs. */
static void paste(Token *lhs, const Token *rhs)
{
    Buf b;
    Lexer lx;
    Token t, extra;
    char *s;
    if (rhs->kind == T_PLACEMARKER)
        return;
    if (lhs->kind == T_PLACEMARKER) {
        u8 flags = lhs->flags;
        *lhs = *rhs;
        lhs->flags = flags;
        return;
    }
    memset(&b, 0, sizeof b);
    tok_spell(lhs, &b);
    tok_spell(rhs, &b);
    s = perm_copy((const char *)b.p, b.len);
    lx.p = lx.start = s;
    lx.end = s + b.len;
    lx.base = 0;
    lx.bol = 0;
    lx.lenient = 1;
    lex_raw(&lx, &t);
    lex_raw(&lx, &extra);
    if ((extra.kind != T_EOF || t.kind == T_EOF) && pp_asm) {
        /* not C: the text is all that matters */
        t.kind = T_OTHER;
        t.p = s;
        t.v = (u32)b.len;
        t.flags = 0;
    } else if (extra.kind != T_EOF || t.kind == T_EOF)
        error_at(lhs->loc, "pasting \"%s\" does not give a valid preprocessing token", s);
    t.loc = lhs->loc;
    t.flags = (u8)((lhs->flags & (TF_BOL | TF_SPACE)) | (t.flags & TF_WIDE));
    *lhs = t;
    buf_free(&b);
}

static void pp_expanded(Token *t);

/* Expand macro m invoked by token *name. Returns 0 if it is a function-like
   macro not followed by '(' (then the name is an ordinary identifier). */
static int expand(Macro *m, const Token *name)
{
    u32 mark = ts_n, lead = name->flags & (TF_BOL | TF_SPACE);
    u32 astart[MAX_PARAMS + 1], an[MAX_PARAMS + 1], estart[MAX_PARAMS + 1], en[MAX_PARAMS + 1];
    u8 need[MAX_PARAMS + 1];
    Token t, *list;
    u32 i, nargs = 0, rstart, n;
    int pending = 0, raw = 0;

    if (m->builtin) {
        const char *file;
        u32 line, col;
        char tmp[32];
        if (m->builtin == BI_PRAGMA) { /* _Pragma("..."): accepted and ignored */
            int depth = 0;
            if (!next_is_lparen())
                return 0;
            for (;;) {
                raw_next(&t);
                if (t.kind == T_EOF)
                    error_at(name->loc, "unterminated _Pragma");
                if (t.kind == T_RPAREN && depth-- == 0)
                    break;
                if (t.kind == T_LPAREN)
                    depth++;
            }
            push_src((Token *)arena_alloc_raw(&exp_arena, sizeof(Token)), 0, 0, name->loc, lead, 0);
            return 1;
        }
        if (m->builtin == BI_HAS_INCLUDE) {
            /* __has_include("file") or __has_include(<file>) */
            Buf nb;
            int angled = 0, depth = 0, first = 1;
            if (!next_is_lparen())
                return 0;
            memset(&nb, 0, sizeof nb);
            for (;;) {
                raw_next(&t);
                if (t.kind == T_EOF)
                    error_at(name->loc, "unterminated __has_include");
                if (t.kind == T_RPAREN && depth-- == 0)
                    break;
                if (t.kind == T_LPAREN)
                    depth++;
                if (first && t.kind == T_STR && t.v >= 2) {
                    buf_put(&nb, t.p + 1, t.v - 2);
                } else if (first && t.kind == T_LT) {
                    angled = 1;
                } else if (angled && t.kind != T_GT) {
                    tok_spell(&t, &nb);
                }
                first = 0;
            }
            list = (Token *)arena_alloc_raw(&exp_arena, sizeof(Token));
            make_token(list, T_NUM, find_include((const char *)nb.p, nb.len, angled, 0, 0) ? "1" : "0", 1, name->loc);
            buf_free(&nb);
            push_src(list, 1, 0, name->loc, lead, 0);
            return 1;
        }
        if (m->builtin == BI_HAS_ATTR) {
            /* __has_attribute(name): 1 for the attributes mcc acts on or can leave alone */
            static const char *const known[] = {"packed", "weak", "noreturn", "always_inline", "noinline", "used",
                                                "gnu_inline", "naked", "error", "aligned", "section", "alias",
                                                "mode", "unused", "const", "pure", "format", "cold", "malloc",
                                                "deprecated", "warn_unused_result", 0};
            int yes = 0, depth = 0, k;
            if (!next_is_lparen())
                return 0;
            for (;;) {
                raw_next(&t);
                if (t.kind == T_EOF)
                    error_at(name->loc, "unterminated __has_attribute");
                if (t.kind == T_RPAREN && depth-- == 0)
                    break;
                if (t.kind == T_LPAREN)
                    depth++;
                if (t.kind == T_IDENT || t.kind >= T_KEYWORD) {
                    Buf sb;
                    const char *sp;
                    size_t sl;
                    memset(&sb, 0, sizeof sb);
                    tok_spell(&t, &sb);
                    sp = (const char *)sb.p;
                    sl = sb.len;
                    if (sl > 4 && sp[0] == '_' && sp[1] == '_' && sp[sl - 1] == '_' && sp[sl - 2] == '_') {
                        sp += 2;
                        sl -= 4;
                    }
                    for (k = 0; known[k]; k++)
                        if (strlen(known[k]) == sl && memcmp(known[k], sp, sl) == 0)
                            yes = 1;
                    buf_free(&sb);
                }
            }
            list = (Token *)arena_alloc_raw(&exp_arena, sizeof(Token));
            make_token(list, T_NUM, yes ? "1" : "0", 1, name->loc);
            push_src(list, 1, 0, name->loc, lead, 0);
            return 1;
        }
        loc_decode(name->loc, &file, &line, &col);
        list = (Token *)arena_alloc_raw(&exp_arena, sizeof(Token));
        if (m->builtin != BI_FILE) {
            sprintf(tmp, "%u", m->builtin == BI_LINE ? line : m->builtin == BI_COUNTER ? counter++ : nincs - 1);
            make_token(list, T_NUM, perm_copy(tmp, strlen(tmp)), strlen(tmp), name->loc);
        } else {
            Token ft;
            Buf b;
            memset(&b, 0, sizeof b);
            buf_u8(&b, '"');
            if (prefix_old && strncmp(file, prefix_old, strlen(prefix_old)) == 0) {
                buf_put(&b, prefix_new, strlen(prefix_new));
                file += strlen(prefix_old);
            }
            buf_put(&b, file, strlen(file));
            buf_u8(&b, '"');
            make_token(&ft, T_STR, perm_copy((const char *)b.p, b.len), b.len, name->loc);
            buf_free(&b);
            *list = ft;
        }
        push_src(list, 1, 0, name->loc, lead, 0);
        return 1;
    }
    if (m->nparams < 0 && m->simple) {
        push_src(m->body, m->nbody, m, name->loc, lead, 0);
        return 1;
    }
    if (m->nparams >= 0) {
        int depth = 0;
        if (!next_is_lparen())
            return 0;
        astart[0] = ts_n;
        for (;;) {
            raw_next(&t);
            if (t.kind == T_EOF)
                error_at(name->loc, "unterminated call to macro '%s'", intern_str(name->v));
            if (t.kind == T_LPAREN) {
                depth++;
            } else if (t.kind == T_RPAREN) {
                if (depth == 0)
                    break;
                depth--;
            } else if (t.kind == T_COMMA && depth == 0 && !(m->variadic && (int)nargs == m->nparams - 1)) {
                an[nargs] = ts_n - astart[nargs];
                if (++nargs > MAX_PARAMS)
                    error_at(name->loc, "too many macro arguments");
                astart[nargs] = ts_n;
                continue;
            }
            ts_push(&t);
        }
        an[nargs] = ts_n - astart[nargs];
        nargs++;
        if (m->nparams == 0 && nargs == 1 && an[0] == 0)
            nargs = 0;
        if (m->variadic && (int)nargs == m->nparams - 1) {
            /* nothing for the variable part */
            astart[nargs] = ts_n;
            an[nargs++] = 0;
        }
        if ((int)nargs != m->nparams)
            error_at(name->loc, "macro '%s' takes %d arguments, %u given", intern_str(name->v), m->nparams, nargs);

        /* Fully expand the arguments that are substituted plainly. */
        memset(need, 0, sizeof need);
        for (i = 0; i < m->nbody; i++) {
            Token *b = &m->body[i];
            if (b->kind != T_PARAM || (b->flags & TF_STRINGIFY))
                continue;
            if ((i > 0 && m->body[i - 1].kind == T_HASHHASH) || (i + 1 < m->nbody && m->body[i + 1].kind == T_HASHHASH))
                continue;
            need[b->v] = 1;
        }
        for (i = 0; i < nargs; i++) {
            if (!need[i])
                continue;
            /* The argument tokens are copied: the scratch stack may move. */
            list = (Token *)arena_alloc_raw(&exp_arena, (an[i] + 1) * sizeof(Token));
            memcpy(list, ts + astart[i], an[i] * sizeof(Token));
            push_src(list, an[i], 0, 0, 0, 1);
            estart[i] = ts_n;
            for (;;) {
                pp_expanded(&t);
                if (t.kind == T_EOF)
                    break;
                ts_push(&t);
            }
            nsrc--; /* the barrier */
            en[i] = ts_n - estart[i];
        }
    }

    /* Substitute. */
    rstart = ts_n;
    for (i = 0; i < m->nbody; i++) {
        Token *b = &m->body[i];
        const Token *first;
        Token tmp;
        u32 from = 0, count = 0, k;
        if (b->kind == T_HASHHASH) {
            /* ", ## args": the comma goes when there are no arguments, and
               nothing is pasted when there are */
            if (m->variadic && i > 0 && m->body[i - 1].kind == T_COMMA && i + 1 < m->nbody &&
                m->body[i + 1].kind == T_PARAM && (int)m->body[i + 1].v == m->nparams - 1 && ts_n > rstart) {
                if (an[m->nparams - 1] == 0) {
                    ts_n--;
                    i++;
                } else {
                    raw = 1;
                }
                continue;
            }
            pending = 1;
            continue;
        }
        if (b->kind == T_PARAM) {
            if (b->flags & TF_STRINGIFY) {
                stringify(&tmp, ts + astart[b->v], an[b->v], name->loc);
                tmp.flags = b->flags & TF_SPACE;
                first = &tmp;
            } else if (raw) {
                raw = 0;
                from = astart[b->v];
                count = an[b->v];
                tmp = ts[from];
                first = &tmp;
            } else if (pending || (i + 1 < m->nbody && m->body[i + 1].kind == T_HASHHASH)) {
                from = astart[b->v];
                count = an[b->v];
                if (count == 0) {
                    make_token(&tmp, T_PLACEMARKER, "", 0, name->loc);
                    first = &tmp;
                } else {
                    tmp = ts[from];
                    first = &tmp;
                }
            } else {
                from = estart[b->v];
                count = en[b->v];
                if (count == 0)
                    continue;
                tmp = ts[from];
                first = &tmp;
            }
            if (b->kind == T_PARAM && !(b->flags & TF_STRINGIFY))
                tmp.flags = (u8)((tmp.flags & ~(TF_SPACE | TF_BOL)) | (b->flags & TF_SPACE));
        } else {
            first = b;
        }
        if (pending) {
            paste(&ts[ts_n - 1], first);
            pending = 0;
        } else {
            ts_push(first);
        }
        for (k = 1; k < count; k++) {
            tmp = ts[from + k];
            ts_push(&tmp);
        }
    }
    n = 0;
    for (i = rstart; i < ts_n; i++)
        if (ts[i].kind != T_PLACEMARKER)
            ts[rstart + n++] = ts[i];
    list = (Token *)arena_alloc_raw(&exp_arena, (n + 1) * sizeof(Token));
    memcpy(list, ts + rstart, n * sizeof(Token));
    ts_n = mark;
    push_src(list, n, m, name->loc, lead, 0);
    return 1;
}

/* ---- directives ---- */

/* Next token of the current directive line; 0 at end of line. */
static int dir_tok(Token *t)
{
    Lexer save = cur->lx;
    lex_raw(&cur->lx, t);
    if (t->kind == T_EOF || (t->flags & TF_BOL)) {
        cur->lx = save;
        return 0;
    }
    return 1;
}

static void skip_line(void)
{
    Token t;
    while (dir_tok(&t))
        ;
}

static void do_define(u32 loc)
{
    Token t, name;
    Macro *m;
    u32 params[MAX_PARAMS], mark = ts_n, i, n;
    int nparams = -1, k, have, variadic = 0;

    if (!dir_tok(&name) || name.kind != T_IDENT)
        error_at(loc, "macro name missing");
    if (name.v == id_defined)
        error_at(name.loc, "'defined' cannot be used as a macro name");
    have = dir_tok(&t);
    if (have && t.kind == T_LPAREN && !(t.flags & TF_SPACE)) {
        nparams = 0;
        for (;;) {
            if (!dir_tok(&t))
                error_at(loc, "missing ')' in macro parameter list");
            if (t.kind == T_RPAREN && nparams == 0)
                break;
            if (nparams == MAX_PARAMS)
                error_at(t.loc, "too many macro parameters");
            if (t.kind == T_ELLIPSIS) {
                params[nparams++] = id_va_args;
                variadic = 1;
            } else {
                if (t.kind != T_IDENT)
                    error_at(t.loc, "expected parameter name");
                params[nparams++] = t.v;
            }
            if (!dir_tok(&t))
                error_at(loc, "expected ',' or ')' in macro parameter list");
            if (t.kind == T_ELLIPSIS && !variadic) {
                variadic = 1; /* "name..." */
                if (!dir_tok(&t))
                    error_at(loc, "expected ')' in macro parameter list");
            }
            if ((t.kind != T_COMMA || variadic) && t.kind != T_RPAREN)
                error_at(loc, "expected ',' or ')' in macro parameter list");
            if (t.kind == T_RPAREN)
                break;
        }
        have = dir_tok(&t);
    }
    m = (Macro *)arena_alloc(&perm_arena, sizeof(Macro));
    m->nparams = nparams;
    m->variadic = (u8)variadic;
    m->simple = nparams < 0;
    while (have) {
        if (t.kind == T_IDENT && nparams > 0) {
            for (k = 0; k < nparams; k++) {
                if (params[k] == t.v) {
                    t.kind = T_PARAM;
                    t.v = (u32)k;
                    break;
                }
            }
        }
        if (t.kind == T_HASHHASH)
            m->simple = 0;
        ts_push(&t);
        have = dir_tok(&t);
    }
    /* "# param" becomes a stringified parameter */
    n = 0;
    for (i = mark; i < ts_n; i++) {
        if (nparams >= 0 && ts[i].kind == T_HASH) {
            if (i + 1 >= ts_n || ts[i + 1].kind != T_PARAM) {
                if (pp_asm) {
                    ts[mark + n++] = ts[i]; /* an immediate operand */
                    continue;
                }
                error_at(ts[i].loc, "'#' is not followed by a macro parameter");
            }
            ts[i + 1].flags = (u8)((ts[i].flags & TF_SPACE) | TF_STRINGIFY);
            continue;
        }
        ts[mark + n++] = ts[i];
    }
    if (n && (ts[mark].kind == T_HASHHASH || ts[mark + n - 1].kind == T_HASHHASH))
        error_at(loc, "'##' cannot appear at either end of a macro expansion");
    if (n)
        ts[mark].flags &= ~TF_SPACE;
    m->nbody = n;
    m->body = (Token *)arena_alloc_raw(&perm_arena, (n + 1) * sizeof(Token));
    memcpy(m->body, ts + mark, n * sizeof(Token));
    ts_n = mark;
    macro_set(name.v, m);
}

typedef struct Val {
    i64 v;
    int uns;
} Val;

static Token *ev_p, *ev_end;
static u32 ev_loc;

static void ev_cond(Val *r, int eval);

static void ev_primary(Val *r, int eval)
{
    Token *t = ev_p;
    r->v = 0;
    r->uns = 0;
    if (t >= ev_end)
        error_at(ev_loc, "expression expected in #if");
    ev_p++;
    switch (t->kind) {
    case T_NUM: {
        u64 iv;
        double fv;
        int flags;
        if (lex_number(t, &iv, &fv, &flags))
            error_at(t->loc, "floating constant in #if");
        r->v = (i64)iv;
        r->uns = (flags & NUM_UNSIGNED) || (i64)iv < 0;
        break;
    }
    case T_CHAR:
        r->v = lex_char(t);
        break;
    case T_IDENT:
        break; /* remaining identifiers are 0 */
    case T_LPAREN:
        ev_cond(r, eval);
        if (ev_p >= ev_end || ev_p->kind != T_RPAREN)
            error_at(ev_loc, "missing ')' in #if");
        ev_p++;
        break;
    case T_MINUS:
        ev_primary(r, eval);
        r->v = (i64)(0 - (u64)r->v);
        break;
    case T_PLUS:
        ev_primary(r, eval);
        break;
    case T_TILDE:
        ev_primary(r, eval);
        r->v = ~r->v;
        break;
    case T_NOT:
        ev_primary(r, eval);
        r->v = !r->v;
        r->uns = 0;
        break;
    default:
        error_at(t->loc, "invalid token in #if");
    }
}

static int ev_prec(int kind)
{
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

static void ev_binary(Val *l, int minprec, int eval)
{
    ev_primary(l, eval);
    for (;;) {
        int op, prec, uns;
        Val r;
        u64 a, b;
        if (ev_p >= ev_end)
            return;
        op = ev_p->kind;
        prec = ev_prec(op);
        if (prec == 0 || prec < minprec)
            return;
        ev_p++;
        if (op == T_ANDAND) {
            ev_binary(&r, prec + 1, eval && l->v);
            l->v = l->v && r.v;
            l->uns = 0;
            continue;
        }
        if (op == T_OROR) {
            ev_binary(&r, prec + 1, eval && !l->v);
            l->v = l->v || r.v;
            l->uns = 0;
            continue;
        }
        ev_binary(&r, prec + 1, eval);
        uns = l->uns || r.uns;
        a = (u64)l->v;
        b = (u64)r.v;
        switch (op) {
        case T_STAR: l->v = (i64)(a * b); break;
        case T_SLASH:
        case T_PERCENT:
            if (b == 0) {
                if (eval)
                    error_at(ev_loc, "division by zero in #if");
                l->v = 0;
            } else if (uns) {
                l->v = (i64)(op == T_SLASH ? a / b : a % b);
            } else if (r.v == -1) {
                l->v = op == T_SLASH ? (i64)(0 - a) : 0;
            } else {
                l->v = op == T_SLASH ? l->v / r.v : l->v % r.v;
            }
            break;
        case T_PLUS: l->v = (i64)(a + b); break;
        case T_MINUS: l->v = (i64)(a - b); break;
        case T_SHL: l->v = b < 64 ? (i64)(a << b) : 0; break;
        case T_SHR: l->v = b >= 64 ? 0 : uns ? (i64)(a >> b) : l->v >> b; break;
        case T_LT: l->v = uns ? a < b : l->v < r.v; break;
        case T_GT: l->v = uns ? a > b : l->v > r.v; break;
        case T_LE: l->v = uns ? a <= b : l->v <= r.v; break;
        case T_GE: l->v = uns ? a >= b : l->v >= r.v; break;
        case T_EQ: l->v = a == b; break;
        case T_NE: l->v = a != b; break;
        case T_AMP: l->v = (i64)(a & b); break;
        case T_CARET: l->v = (i64)(a ^ b); break;
        case T_PIPE: l->v = (i64)(a | b); break;
        }
        l->uns = (op >= T_LT && op <= T_NE) ? 0 : uns;
    }
}

static void ev_cond(Val *r, int eval)
{
    ev_binary(r, 1, eval);
    if (ev_p < ev_end && ev_p->kind == T_QUESTION) {
        Val a, b;
        ev_p++;
        ev_cond(&a, eval && r->v);
        if (ev_p >= ev_end || ev_p->kind != T_COLON)
            error_at(ev_loc, "missing ':' in #if");
        ev_p++;
        ev_cond(&b, eval && !r->v);
        r->v = r->v ? a.v : b.v;
        r->uns = a.uns || b.uns;
    }
}

/* Collect the rest of the directive line and macro-expand it onto the
   scratch stack; returns the index of the first resulting token. */
static u32 expand_line(int handle_defined)
{
    u32 mark = ts_n, n, start;
    Token t, *list;
    while (dir_tok(&t)) {
        if (handle_defined && t.kind == T_IDENT && t.v == id_defined) {
            Token id;
            int paren = 0;
            if (!dir_tok(&id))
                error_at(t.loc, "missing macro name after 'defined'");
            if (id.kind == T_LPAREN) {
                paren = 1;
                if (!dir_tok(&id))
                    error_at(t.loc, "missing macro name after 'defined'");
            }
            if (id.kind != T_IDENT)
                error_at(id.loc, "missing macro name after 'defined'");
            make_token(&t, T_NUM, macro_get(id.v) ? "1" : "0", 1, t.loc);
            if (paren && (!dir_tok(&id) || id.kind != T_RPAREN))
                error_at(t.loc, "missing ')' after 'defined'");
        }
        ts_push(&t);
    }
    n = ts_n - mark;
    list = (Token *)xmalloc((n + 1) * sizeof(Token));
    memcpy(list, ts + mark, n * sizeof(Token));
    ts_n = mark;
    {
        /* isolate from any outer state: directives only occur at file level */
        u32 saved = nsrc;
        push_src(list, n, 0, 0, 0, 1);
        start = ts_n;
        for (;;) {
            pp_expanded(&t);
            if (t.kind == T_EOF)
                break;
            ts_push(&t);
        }
        nsrc = saved;
    }
    free(list);
    return start;
}

static int eval_if(u32 loc)
{
    u32 start = expand_line(1);
    Val r;
    ev_p = ts + start;
    ev_end = ts + ts_n;
    ev_loc = loc;
    ev_cond(&r, 1);
    if (ev_p < ev_end)
        error_at(ev_p->loc, "unexpected token in #if");
    ts_n = start;
    return r.v != 0;
}

static void push_cond(u32 loc, int taken)
{
    if (nconds == conds_cap) {
        conds_cap = conds_cap ? conds_cap * 2 : 32;
        conds = (Cond *)xrealloc(conds, conds_cap * sizeof(Cond));
    }
    conds[nconds].loc = loc;
    conds[nconds].taken = (u8)taken;
    conds[nconds].has_else = 0;
    nconds++;
}

static void pop_cond(u32 loc)
{
    if (nconds <= cur->cond_base)
        error_at(loc, "#endif without #if");
    nconds--;
    if (cur->guard_state == 1 && nconds == cur->cond_base)
        cur->guard_state = 2;
}

/* Skip a group that is not compiled. Stops after the directive that ends it. */
static void skip_group(void)
{
    int depth = 0;
    Token t, d;
    cur->lx.lenient = 1;
    for (;;) {
        lex_raw(&cur->lx, &t);
        if (t.kind == T_EOF)
            error_at(conds[nconds - 1].loc, "unterminated conditional");
        if (t.kind != T_HASH || !(t.flags & TF_BOL))
            continue;
        if (!dir_tok(&d) || d.kind != T_IDENT)
            continue;
        if (d.v == ID_IF || d.v == id_ifdef || d.v == id_ifndef) {
            depth++;
        } else if (d.v == id_endif) {
            if (depth == 0) {
                cur->lx.lenient = pp_asm;
                skip_line();
                pop_cond(d.loc);
                return;
            }
            depth--;
        } else if (depth == 0 && (d.v == ID_ELSE || d.v == id_elif)) {
            Cond *c = &conds[nconds - 1];
            if (c->has_else)
                error_at(d.loc, "#%s after #else", intern_str(d.v));
            if (cur->guard_state == 1 && nconds == cur->cond_base + 1)
                cur->guard_state = 3;
            if (d.v == ID_ELSE) {
                c->has_else = 1;
                if (!c->taken) {
                    c->taken = 1;
                    cur->lx.lenient = pp_asm;
                    skip_line();
                    return;
                }
            } else if (!c->taken) {
                cur->lx.lenient = pp_asm;
                if (eval_if(d.loc)) {
                    conds[nconds - 1].taken = 1;
                    return;
                }
                cur->lx.lenient = 1;
            }
        }
    }
}

static void push_inc(SrcFile *f, u32 loc)
{
    if (nincs >= MAX_INCLUDE_DEPTH)
        error_at(loc, "#include nested too deeply");
    if (nincs == incs_cap) {
        incs_cap = incs_cap ? incs_cap * 2 : 16;
        incs = (Inc *)xrealloc(incs, incs_cap * sizeof(Inc));
    }
    cur = &incs[nincs++];
    cur->file = f;
    lex_start(&cur->lx, f);
    cur->cond_base = nconds;
    cur->guard = 0;
    cur->guard_state = 0;
    cur->dir = 0;
    cur->sys = 0;
    cur->lx.lenient = pp_asm;
}

/* Takes "./" and "name/../" out of a path, so that one file reached two ways is known to be one file
   (#pragma once depends on it). */
static void path_tidy(char *p)
{
    char *in = p, *out = p;
    while (*in) {
        int at_start = out == p || out[-1] == '/';
        if (at_start && in[0] == '.' && in[1] == '/') {
            in += 2;
            continue;
        }
        if (at_start && in[0] == '.' && in[1] == '.' && in[2] == '/' && out > p + 1) {
            /* drop the component before, unless it is ".." itself */
            char *q = out - 1;
            while (q > p && q[-1] != '/')
                q--;
            if (!(out - q == 3 && q[0] == '.' && q[1] == '.') && !(out - q == 1)) {
                out = q;
                in += 3;
                continue;
            }
        }
        *out++ = *in++;
    }
    *out = 0;
}

static SrcFile *try_include(const char *dir, size_t dirlen, const char *name, size_t namelen)
{
    char *path = (char *)xmalloc(dirlen + namelen + 2);
    SrcFile *f;
    size_t n = 0;
    if (dirlen) {
        memcpy(path, dir, dirlen);
        n = dirlen;
        if (path[n - 1] != '/')
            path[n++] = '/';
    }
    memcpy(path + n, name, namelen);
    path[n + namelen] = 0;
    path_tidy(path);
    f = src_load(path);
    free(path);
    return f;
}

static SrcFile *find_include(const char *name, size_t namelen, int angled, int next, u32 *where)
{
    SrcFile *f = 0;
    u32 i, found = 0;
    if (namelen && name[0] == '/') {
        f = try_include("", 0, name, namelen);
    } else if (namelen) {
        if (!angled && !next) {
            const char *self = cur->file->name, *slash = strrchr(self, '/');
            f = try_include(self, slash ? (size_t)(slash - self + 1) : 0, name, namelen);
        }
        for (i = next ? cur->dir : 0; !f && i < ninc_dirs + nsys_dirs; i++) {
            const char *d = i < ninc_dirs ? inc_dirs[i] : sys_dirs[i - ninc_dirs];
            f = try_include(d, strlen(d), name, namelen);
            found = i + 1;
        }
        if (!f)
            found = 0;
    }
    if (where)
        *where = found;
    return f;
}

static void do_include(u32 loc, int next)
{
    const char *p = cur->lx.p, *name = 0;
    size_t namelen = 0;
    int angled = 0;
    SrcFile *f = 0;
    Buf b;
    u32 i, found = 0;

    memset(&b, 0, sizeof b);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '<' || *p == '"') {
        char close = *p == '<' ? '>' : '"';
        const char *e = p + 1;
        while (*e && *e != '\n' && *e != close)
            e++;
        if (*e != close)
            error_at(loc, "malformed #include");
        angled = *p == '<';
        name = p + 1;
        namelen = (size_t)(e - name);
        cur->lx.p = e + 1;
        skip_line();
    } else {
        u32 start = expand_line(0), n = ts_n - start;
        Token *t = ts + start;
        if (n == 1 && t->kind == T_STR && t->v >= 2) {
            buf_put(&b, t->p + 1, t->v - 2);
        } else if (n >= 3 && t->kind == T_LT && t[n - 1].kind == T_GT) {
            angled = 1;
            for (i = 1; i + 1 < n; i++) {
                if (i > 1 && (t[i].flags & TF_SPACE))
                    buf_u8(&b, ' ');
                tok_spell(&t[i], &b);
            }
        } else {
            error_at(loc, "malformed #include");
        }
        ts_n = start;
        name = (const char *)b.p;
        namelen = b.len;
    }
    if (namelen == 0)
        error_at(loc, "empty file name in #include");
    f = find_include(name, namelen, angled, next, &found);
    if (!f)
        error_at(loc, "cannot find include file '%.*s'", (int)namelen, name);
    buf_free(&b);
    if (f->once == pp_unit || (f->guard && macro_get(f->guard)))
        return;
    {
        int sys = found > ninc_dirs || (!found && cur && cur->sys);
        push_inc(f, loc);
        cur->dir = found;
        cur->sys = (u8)sys;
    }
}

static const char *c_dir; /* the C library's headers: their declarations are C even in C++ */

void pp_c_dir(const char *dir)
{
    c_dir = dir;
}

/* Whether a location is in one of the C library's headers. */
int pp_in_system(u32 loc)
{
    static const char *last;
    static int last_sys;
    const char *file = 0;
    u32 line, col;
    loc_decode(loc, &file, &line, &col);
    if (!file)
        return 0;
    if (file == last)
        return last_sys;
    last = file;
    last_sys = 0;
    if (c_dir) {
        size_t n = strlen(c_dir);
        if (n && !strncmp(file, c_dir, n) && (file[n] == '/' || c_dir[n - 1] == '/'))
            last_sys = 1;
    }
    return last_sys;
}

static void directive(u32 loc)
{
    Token d, t;
    int in_guard = cur->guard_state;
    if (!dir_tok(&d))
        return; /* null directive */
    if (cur->guard_state != 1)
        cur->guard_state = 3;
    if (d.kind != T_IDENT) {
        if (pp_asm || d.kind == T_NUM) {
            skip_line(); /* a comment in assembly, or "# 12 file" from another preprocessor */
            return;
        }
        error_at(d.loc, "invalid preprocessing directive");
    }
    if (d.v == id_define) {
        do_define(d.loc);
    } else if (d.v == id_include || d.v == id_include_next) {
        do_include(d.loc, d.v == id_include_next);
    } else if (d.v == id_ifdef || d.v == id_ifndef) {
        int def;
        if (!dir_tok(&t) || t.kind != T_IDENT)
            error_at(d.loc, "macro name missing");
        skip_line();
        def = macro_get(t.v) != 0;
        if (in_guard == 0 && d.v == id_ifndef) {
            cur->guard = t.v;
            cur->guard_state = 1;
        }
        push_cond(d.loc, d.v == id_ifdef ? def : !def);
        if (!conds[nconds - 1].taken)
            skip_group();
    } else if (d.v == ID_IF) {
        int v = eval_if(d.loc);
        push_cond(d.loc, v);
        if (!v)
            skip_group();
    } else if (d.v == id_elif || d.v == ID_ELSE) {
        if (nconds <= cur->cond_base)
            error_at(d.loc, "#%s without #if", intern_str(d.v));
        if (conds[nconds - 1].has_else)
            error_at(d.loc, "#%s after #else", intern_str(d.v));
        if (d.v == ID_ELSE)
            conds[nconds - 1].has_else = 1;
        if (cur->guard_state == 1 && nconds == cur->cond_base + 1)
            cur->guard_state = 3;
        skip_line();
        skip_group(); /* the group before this one was taken */
    } else if (d.v == id_endif) {
        skip_line();
        pop_cond(d.loc);
    } else if (d.v == id_undef) {
        if (!dir_tok(&t) || t.kind != T_IDENT)
            error_at(d.loc, "macro name missing");
        if (macro_get(t.v))
            macro_set(t.v, 0);
        skip_line();
    } else if (d.v == id_line) {
        u32 start = expand_line(0), n = ts_n - start;
        u64 iv = 0;
        double fv;
        int flags;
        const char *name = 0;
        if (n < 1 || ts[start].kind != T_NUM || lex_number(&ts[start], &iv, &fv, &flags))
            error_at(d.loc, "#line requires a line number");
        if (n >= 2 && ts[start + 1].kind == T_STR)
            name = perm_copy(ts[start + 1].p + 1, ts[start + 1].v - 2);
        ts_n = start;
        src_set_line(d.loc, (u32)iv, name);
    } else if (d.v == id_error) {
        const char *s = cur->lx.p, *e = s;
        while (*e && *e != '\n')
            e++;
        error_at(d.loc, "#error%.*s", (int)(e - s), s);
    } else if (d.v == id_warning) {
        const char *s = cur->lx.p, *e = s;
        while (*e && *e != '\n')
            e++;
        fprintf(stderr, "warning: #warning%.*s\n", (int)(e - s), s);
        skip_line();
    } else if (d.v == id_pragma) {
        if (dir_tok(&t) && t.kind == T_IDENT && t.v == id_once)
            cur->file->once = pp_unit;
        else if (opt_cuda && t.kind == T_IDENT && t.v == id_unroll) {
            /* "#pragma unroll [n]" reaches the parser as the identifier __mcc_unroll, then the rest of the line */
            pragma_mark = 1;
            return;
        }
        skip_line();
    } else if (pp_asm) {
        skip_line(); /* a comment */
    } else {
        error_at(d.loc, "invalid preprocessing directive #%s", intern_str(d.v));
    }
    (void)loc;
}

/* ---- main entry ---- */

static void pp_expanded(Token *t)
{
    for (;;) {
        if (nsrc) {
            Src *s = &srcs[nsrc - 1];
            if (s->pos == s->n) {
                if (s->barrier) {
                    t->kind = T_EOF;
                    return;
                }
                pop_src();
                continue;
            }
            src_take(s, t);
        } else {
            lex_raw(&cur->lx, t);
            if (t->kind == T_EOF) {
                if (nconds > cur->cond_base)
                    error_at(conds[nconds - 1].loc, "unterminated conditional");
                if (cur->guard_state == 2)
                    cur->file->guard = cur->guard;
                if (nincs == 1)
                    return;
                nincs--;
                cur = &incs[nincs - 1];
                continue;
            }
            if (t->kind == T_HASH && (t->flags & TF_BOL)) {
                u32 hl = t->loc;
                directive(hl);
                if (pragma_mark) {
                    pragma_mark = 0;
                    memset(t, 0, sizeof *t);
                    t->kind = T_IDENT;
                    t->v = id_mcc_unroll;
                    t->flags = TF_NOEXPAND;
                    t->loc = hl;
                    return;
                }
                continue;
            }
            if (cur->guard_state != 1)
                cur->guard_state = 3;
        }
        if (t->kind == T_IDENT && !(t->flags & TF_NOEXPAND)) {
            Macro *m = macro_get(t->v);
            if (m) {
                if (m->busy)
                    t->flags |= TF_NOEXPAND;
                else if (expand(m, t))
                    continue;
            }
        }
        return;
    }
}

void pp_next(Token *t)
{
    pp_expanded(t);
}

/* -fmacro-prefix-map=old=new: how __FILE__ spells the start of a path */
void pp_prefix_map(const char *map)
{
    const char *eq = strchr(map, '=');
    if (!eq)
        return;
    prefix_old = perm_copy(map, (size_t)(eq - map));
    prefix_new = eq + 1;
}

void pp_system_dir(const char *dir)
{
    sys_dirs = (const char **)xrealloc((void *)sys_dirs, (nsys_dirs + 1) * sizeof(char *));
    sys_dirs[nsys_dirs++] = dir;
}

void pp_force_include(const char *path)
{
    forced = (const char **)xrealloc((void *)forced, (nforced + 1) * sizeof(char *));
    forced[nforced++] = path;
}

void pp_include_dir(const char *dir)
{
    inc_dirs = (const char **)xrealloc((void *)inc_dirs, (ninc_dirs + 1) * sizeof(char *));
    inc_dirs[ninc_dirs++] = dir;
}

void pp_define(const char *def)
{
    const char *eq = strchr(def, '=');
    buf_put(&predef, "#define ", 8);
    if (eq) {
        buf_put(&predef, def, (size_t)(eq - def));
        buf_u8(&predef, ' ');
        buf_put(&predef, eq + 1, strlen(eq + 1));
    } else {
        buf_put(&predef, def, strlen(def));
        buf_put(&predef, " 1", 2);
    }
    buf_u8(&predef, '\n');
}

void pp_undef(const char *name)
{
    buf_put(&predef, "#undef ", 7);
    buf_put(&predef, name, strlen(name));
    buf_u8(&predef, '\n');
}

static void builtin(const char *name, int kind)
{
    Macro *m = (Macro *)arena_alloc(&perm_arena, sizeof(Macro));
    m->nparams = -1;
    m->builtin = (u8)kind;
    macro_set(intern_cstr(name), m);
}

void pp_init(void)
{
    static const char base[] = "#define __STDC__ 1\n"
                               "#define __mcc__ 1\n"
                               "#define __LP64__ 1\n"
                               "#define __DATE__ \"Jan  1 1970\"\n"
                               "#define __TIME__ \"00:00:00\"\n";
    lex_init();
    id_define = intern_cstr("define");
    id_undef = intern_cstr("undef");
    id_include = intern_cstr("include");
    id_ifdef = intern_cstr("ifdef");
    id_ifndef = intern_cstr("ifndef");
    id_elif = intern_cstr("elif");
    id_endif = intern_cstr("endif");
    id_line = intern_cstr("line");
    id_error = intern_cstr("error");
    id_pragma = intern_cstr("pragma");
    id_defined = intern_cstr("defined");
    id_once = intern_cstr("once");
    id_unroll = intern_cstr("unroll");
    id_mcc_unroll = intern_cstr("__mcc_unroll");
    pp_unit++; /* #pragma once holds for one translation unit, and the files are kept across units */
    id_va_args = intern_cstr("__VA_ARGS__");
    id_include_next = intern_cstr("include_next");
    id_warning = intern_cstr("warning");
    counter = 0;
    if (macros)
        memset(macros, 0, macros_cap * sizeof(Macro *));
    nsrc = nconds = nincs = ts_n = 0;
    predef.len = 0;
    buf_put(&predef, base, sizeof base - 1);
}

void pp_open(const char *path)
{
    SrcFile *f = src_load(path);
    u32 k;
    if (!f)
        die("cannot read %s", path);
    builtin("__LINE__", BI_LINE);
    builtin("__FILE__", BI_FILE);
    builtin("__COUNTER__", BI_COUNTER);
    builtin("__INCLUDE_LEVEL__", BI_LEVEL);
    if (opt_gnu) {
        builtin("__has_attribute", BI_HAS_ATTR);
        builtin("__has_include", BI_HAS_INCLUDE);
        builtin("_Pragma", BI_PRAGMA);
    }
    push_inc(f, 0);
    for (k = nforced; k-- > 0;) {
        SrcFile *g = src_load(forced[k]);
        u32 d;
        for (d = 0; !g && d < ninc_dirs + nsys_dirs; d++) {
            const char *dir = d < ninc_dirs ? inc_dirs[d] : sys_dirs[d - ninc_dirs];
            g = try_include(dir, strlen(dir), forced[k], strlen(forced[k]));
        }
        if (!g)
            die("cannot read %s", forced[k]);
        push_inc(g, 0);
    }
    push_inc(src_buffer("<predefined>", (const char *)predef.p, predef.len), 0);
}

/* The preprocessed text of the open file, for the assembler. */
void pp_text(Buf *b)
{
    Token t;
    int first = 1;
    for (;;) {
        pp_next(&t);
        if (t.kind == T_EOF)
            break;
        if (!first) {
            if (t.flags & TF_BOL)
                buf_u8(b, '\n');
            else if (t.flags & TF_SPACE)
                buf_u8(b, ' ');
        }
        first = 0;
        tok_spell(&t, b);
    }
    buf_u8(b, '\n');
    buf_u8(b, 0);
    b->len--;
}

void pp_dump(void)
{
    Token t;
    Buf b;
    int first = 1;
    memset(&b, 0, sizeof b);
    for (;;) {
        pp_next(&t);
        if (t.kind == T_EOF)
            break;
        if (!first) {
            if (t.flags & TF_BOL)
                buf_u8(&b, '\n');
            else if (t.flags & TF_SPACE)
                buf_u8(&b, ' ');
        }
        first = 0;
        tok_spell(&t, &b);
        if (b.len > 60000) {
            fwrite(b.p, 1, b.len, stdout);
            b.len = 0;
        }
    }
    buf_u8(&b, '\n');
    fwrite(b.p, 1, b.len, stdout);
    buf_free(&b);
}
