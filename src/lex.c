/* Source files, locations, diagnostics and the raw (pre-macro) lexer. */
#include "mcc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const tok_names[] = {
    "end of file", "identifier", "number", "character constant", "string literal", "stray character", "", "",
    "[", "]", "(", ")", "{", "}", ".", "->", "++", "--", "&", "*", "+", "-", "~", "!", "/", "%", "<<", ">>",
    "<", ">", "<=", ">=", "==", "!=", "^", "|", "&&", "||", "?", ":", "=", "*=", "/=", "%=", "+=", "-=",
    "<<=", ">>=", "&=", "^=", "|=", ",", "#", "##", ";", "...", "::",
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
    "float", "for", "goto", "if", "int", "long", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "__inline__", "__restrict__", "__typeof__", "__asm__", "__attribute__", "__extension__", "__alignof__", "_Bool",
    "__label__", "_Static_assert", "__int128", "_Noreturn", "_Generic", "__auto_type", "_Atomic", "_Thread_local"
};

int opt_gnu = 1, opt_syntax_only, opt_skip_asm, opt_c23, opt_cxx, opt_cuda;
Buf *opt_asm_text;

/* Other spellings of keywords; those marked 1 are identifiers in strict C89,
   those marked 2 are keywords from C23 on (and in C++). */
static const struct {
    const char *name;
    u8 kw, gnu;
} kw_alias[] = {
    {"inline", KW_INLINE, 1}, {"__inline", KW_INLINE, 0}, {"__restrict", KW_RESTRICT, 0},
    {"typeof", KW_TYPEOF, 1}, {"__typeof", KW_TYPEOF, 0}, {"asm", KW_ASM, 1}, {"__asm", KW_ASM, 0},
    {"__attribute", KW_ATTRIBUTE, 0}, {"__alignof", KW_ALIGNOF, 0}, {"_Alignof", KW_ALIGNOF, 0},
    {"__const", KW_CONST, 0}, {"__const__", KW_CONST, 0}, {"__volatile", KW_VOLATILE, 0},
    {"__volatile__", KW_VOLATILE, 0}, {"__signed", KW_SIGNED, 0}, {"__signed__", KW_SIGNED, 0},
    {"bool", KW_BOOL, 2}, {"static_assert", KW_STATIC_ASSERT, 2}, {"alignof", KW_ALIGNOF, 2},
    {"typeof_unqual", KW_TYPEOF, 2}, {"__thread", KW_THREAD_LOCAL, 0}, {"thread_local", KW_THREAD_LOCAL, 1}
};
#define NALIAS (sizeof kw_alias / sizeof kw_alias[0])

int lex_keyword(u32 name)
{
    if (name <= KW_COUNT)
        return name ? T_KEYWORD + (int)name - 1 : 0;
    if (name <= KW_COUNT + NALIAS) {
        int g = kw_alias[name - KW_COUNT - 1].gnu;
        if (g == 0 || (g == 1 && opt_gnu) || (g == 2 && (opt_c23 || opt_cxx)))
            return T_KEYWORD + kw_alias[name - KW_COUNT - 1].kw;
    }
    return 0;
}

/* ---- source files ---- */

typedef struct LineAdj {
    u32 loc;
    u32 line;         /* line number of the line after the directive */
    u32 at_line;      /* physical line of that same line */
    const char *name; /* NULL: unchanged */
    struct LineAdj *next;
} LineAdj;

static SrcFile *files;
static u32 next_base = 1;
static LineAdj *line_adjs; /* most recent first */

/* Translation phases 1 and 2: trigraphs and line splicing. Removed
   newlines are re-inserted after the logical line so line numbers hold. */
static size_t clean_source(char *s, size_t len)
{
    size_t i = 0, j = 0, owed = 0;
    if (!memchr(s, '\\', len) && !memchr(s, '?', len))
        return len;
    while (i < len) {
        char c = s[i];
        if (c == '?' && i + 2 < len && s[i + 1] == '?') {
            char r = 0;
            switch (s[i + 2]) {
            case '=': r = '#'; break;
            case '(': r = '['; break;
            case '/': r = '\\'; break;
            case ')': r = ']'; break;
            case '\'': r = '^'; break;
            case '<': r = '{'; break;
            case '!': r = '|'; break;
            case '>': r = '}'; break;
            case '-': r = '~'; break;
            }
            if (r) {
                s[i + 2] = r;
                i += 2;
                continue;
            }
        }
        if (c == '\\') {
            size_t k = i + 1;
            if (k < len && s[k] == '\r')
                k++;
            if (k < len && s[k] == '\n') {
                i = k + 1;
                owed++;
                continue;
            }
        }
        if (c == '\n') {
            s[j++] = c;
            i++;
            while (owed) {
                s[j++] = '\n';
                owed--;
            }
            continue;
        }
        s[j++] = c;
        i++;
    }
    s[j] = 0;
    return j;
}

static SrcFile *src_add(const char *name, char *data, size_t len)
{
    SrcFile *f = (SrcFile *)xmalloc(sizeof(SrcFile));
    char *nm = (char *)xmalloc(strlen(name) + 1);
    strcpy(nm, name);
    f->name = nm;
    f->data = data;
    f->len = clean_source(data, len);
    f->base = next_base;
    f->guard = 0;
    f->once = 0;
    f->lines = 0;
    f->nlines = 0;
    f->next = files;
    files = f;
    next_base += (u32)len + 1;
    return f;
}

SrcFile *src_load(const char *path)
{
    SrcFile *f;
    size_t len;
    u8 *data;
    for (f = files; f; f = f->next)
        if (strcmp(f->name, path) == 0)
            return f;
    data = os_read_file(path, &len);
    if (!data)
        return 0;
    return src_add(path, (char *)data, len);
}

void src_deps(Buf *b)
{
    SrcFile *f, **v;
    size_t n = 0, k;
    for (f = files; f; f = f->next)
        n++;
    v = (SrcFile **)xmalloc((n + 1) * sizeof *v);
    for (f = files, k = n; f; f = f->next)
        v[--k] = f;
    for (k = 0; k < n; k++) {
        if (v[k]->name[0] == '<' || strcmp(v[k]->name, "-") == 0)
            continue;
        buf_put(b, " \\\n ", 4);
        buf_put(b, v[k]->name, strlen(v[k]->name));
    }
    free(v);
}

SrcFile *src_buffer(const char *name, const char *text, size_t len)
{
    char *data = (char *)xmalloc(len + 1);
    memcpy(data, text, len);
    data[len] = 0;
    return src_add(name, data, len);
}

static void loc_physical(u32 loc, SrcFile **fp, u32 *line, u32 *col)
{
    SrcFile *f;
    *fp = 0;
    *line = *col = 0;
    for (f = files; f; f = f->next) {
        if (loc >= f->base && loc <= f->base + f->len) {
            const char *p = f->data, *end = f->data + (loc - f->base), *ls = p;
            u32 n = 1;
            while ((p = (const char *)memchr(p, '\n', (size_t)(end - p))) != 0) {
                n++;
                ls = ++p;
            }
            *fp = f;
            *line = n;
            *col = (u32)(end - ls) + 1;
            return;
        }
    }
}

void src_set_line(u32 loc, u32 line, const char *name)
{
    LineAdj *a = (LineAdj *)xmalloc(sizeof(LineAdj));
    SrcFile *f;
    u32 col;
    loc_physical(loc, &f, &a->at_line, &col);
    a->at_line++;
    a->loc = loc;
    a->line = line;
    a->name = name;
    a->next = line_adjs;
    line_adjs = a;
}

void loc_decode(u32 loc, const char **file, u32 *line, u32 *col)
{
    SrcFile *f;
    LineAdj *a;
    loc_physical(loc, &f, line, col);
    *file = f ? f->name : "<unknown>";
    if (!f)
        return;
    for (a = line_adjs; a; a = a->next) {
        if (a->loc <= loc && a->loc >= f->base) {
            *line = *line - a->at_line + a->line;
            if (a->name)
                *file = a->name;
            return;
        }
    }
}

/* loc_decode by a table of line starts and a search in it, not by counting lines: a line table asks for every
   instruction's location. */
void loc_line(u32 loc, const char **file, u32 *line, u32 *col)
{
    static SrcFile *last;
    SrcFile *f = last;
    LineAdj *a;
    u32 off, lo, hi;
    if (!f || loc < f->base || loc > f->base + f->len)
        for (f = files; f; f = f->next)
            if (loc >= f->base && loc <= f->base + f->len)
                break;
    *line = *col = 0;
    *file = "<unknown>";
    if (!f)
        return;
    last = f;
    if (!f->lines) {
        u32 n = 1, cap = 256;
        size_t k;
        f->lines = (u32 *)xmalloc(cap * sizeof(u32));
        f->lines[0] = 0;
        for (k = 0; k < f->len; k++)
            if (f->data[k] == '\n') {
                if (n == cap) {
                    cap *= 2;
                    f->lines = (u32 *)xrealloc(f->lines, cap * sizeof(u32));
                }
                f->lines[n++] = (u32)k + 1;
            }
        f->nlines = n;
    }
    off = loc - f->base;
    lo = 0;
    hi = f->nlines;
    while (hi - lo > 1) {
        u32 mid = lo + (hi - lo) / 2;
        if (f->lines[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    *line = lo + 1;
    *col = off - f->lines[lo] + 1;
    *file = f->name;
    for (a = line_adjs; a; a = a->next) {
        if (a->loc <= loc && a->loc >= f->base) {
            *line = *line - a->at_line + a->line;
            if (a->name)
                *file = a->name;
            return;
        }
    }
}

static void vreport(u32 loc, const char *kind, const char *fmt, va_list ap)
{
    const char *file;
    u32 line, col;
    if (loc) {
        loc_decode(loc, &file, &line, &col);
        fprintf(stderr, "%s:%u:%u: ", file, line, col);
    } else {
        fputs("mcc: ", stderr);
    }
    fprintf(stderr, "%s: ", kind);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void error_at(u32 loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(loc, "error", fmt, ap);
    va_end(ap);
    exit(1);
}

void warn_at(u32 loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(loc, "warning", fmt, ap);
    va_end(ap);
}

/* ---- raw lexer ---- */

#define C_START 1 /* may start an identifier */
#define C_IDENT 2 /* may continue an identifier or pp-number */
#define C_DIGIT 4

static u8 cclass[256];

void lex_init(void)
{
    int c, k;
    if (cclass['a'])
        return;
    for (c = 'a'; c <= 'z'; c++)
        cclass[c] = C_START | C_IDENT;
    for (c = 'A'; c <= 'Z'; c++)
        cclass[c] = C_START | C_IDENT;
    cclass['_'] = C_START | C_IDENT;
    for (c = '0'; c <= '9'; c++)
        cclass[c] = C_IDENT | C_DIGIT;
    /* keywords get the first name ids: keyword k is id k + 1 */
    for (k = 0; k < KW_COUNT; k++)
        if (intern_cstr(tok_names[T_KEYWORD + k]) != (u32)k + 1)
            die("internal: keyword ids");
    for (k = 0; k < (int)NALIAS; k++)
        if (intern_cstr(kw_alias[k].name) != (u32)(KW_COUNT + k + 1))
            die("internal: keyword ids");
}

void lex_start(Lexer *lx, SrcFile *f)
{
    lx->p = lx->start = f->data;
    lx->end = f->data + f->len;
    lx->base = f->base;
    lx->bol = 1;
    lx->lenient = 0;
}

void lex_raw(Lexer *lx, Token *t)
{
    const char *p = lx->p;
    const char *s;
    u32 flags = lx->bol ? TF_BOL : 0;
    u32 kind;
    int c;

    for (;;) {
        c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            p++;
            flags |= TF_SPACE;
        } else if (c == '\n') {
            p++;
            flags |= TF_BOL | TF_SPACE;
        } else if (c == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) {
                if (!lx->lenient)
                    error_at(lx->base + (u32)(p - lx->start), "unterminated comment");
                e = lx->end - 2;
            }
            p = e + 2;
            flags |= TF_SPACE;
        } else if (c == '/' && p[1] == '/' && opt_gnu) {
            while (*p && *p != '\n')
                p++;
            flags |= TF_SPACE;
        } else {
            break;
        }
    }
    lx->bol = 0;
    s = p;
    t->flags = (u8)flags;
    t->loc = lx->base + (u32)(p - lx->start);
    t->p = p;

    if (cclass[c & 0xff] & C_START) {
        u32 h = HASH_INIT;
        if (c == 'L' && (p[1] == '\'' || p[1] == '"')) {
            t->flags |= TF_WIDE;
            p++;
            c = *p;
            goto quoted;
        }
        do {
            h = HASH_STEP(h, *p);
            p++;
        } while (cclass[(u8)*p] & C_IDENT);
        t->kind = T_IDENT;
        t->v = intern_hashed(s, (size_t)(p - s), h);
        lx->p = p;
        return;
    }
    if ((cclass[c & 0xff] & C_DIGIT) || (c == '.' && (cclass[(u8)p[1]] & C_DIGIT))) {
        for (;;) {
            if ((*p == 'e' || *p == 'E' || ((*p == 'p' || *p == 'P') && opt_gnu)) && (p[1] == '+' || p[1] == '-'))
                p += 2;
            else if ((cclass[(u8)*p] & C_IDENT) || *p == '.')
                p++;
            else
                break;
        }
        t->kind = T_NUM;
        t->v = (u32)(p - s);
        lx->p = p;
        return;
    }
    if (c == '"' || c == '\'') {
    quoted:
        p++;
        while (*p != c) {
            if (*p == '\\' && p[1] != '\n' && p[1] != 0)
                p++;
            if (*p == '\n' || *p == 0) {
                if (!lx->lenient)
                    error_at(t->loc, "unterminated %s", c == '"' ? "string literal" : "character constant");
                t->kind = T_OTHER;
                t->v = (u32)(p - s);
                lx->p = p;
                return;
            }
            p++;
        }
        p++;
        t->kind = c == '"' ? T_STR : T_CHAR;
        t->v = (u32)(p - s);
        lx->p = p;
        return;
    }

    p++;
    switch (c) {
    case 0:
        if (s >= lx->end) {
            t->kind = T_EOF;
            t->flags |= TF_BOL;
            t->v = 0;
            lx->p = s;
            return;
        }
        kind = T_OTHER;
        break;
    case '[': kind = T_LBRACK; break;
    case ']': kind = T_RBRACK; break;
    case '(': kind = T_LPAREN; break;
    case ')': kind = T_RPAREN; break;
    case '{': kind = T_LBRACE; break;
    case '}': kind = T_RBRACE; break;
    case '~': kind = T_TILDE; break;
    case '?': kind = T_QUESTION; break;
    case ':':
        kind = T_COLON;
        if (opt_cxx && *p == ':') { p++; kind = T_SCOPE; }
        break;
    case ',': kind = T_COMMA; break;
    case ';': kind = T_SEMI; break;
    case '.':
        if (*p == '.' && p[1] == '.') {
            p += 2;
            kind = T_ELLIPSIS;
        } else {
            kind = T_DOT;
        }
        break;
    case '-':
        if (*p == '>') { p++; kind = T_ARROW; }
        else if (*p == '-') { p++; kind = T_DEC; }
        else if (*p == '=') { p++; kind = T_SUBEQ; }
        else kind = T_MINUS;
        break;
    case '+':
        if (*p == '+') { p++; kind = T_INC; }
        else if (*p == '=') { p++; kind = T_ADDEQ; }
        else kind = T_PLUS;
        break;
    case '&':
        if (*p == '&') { p++; kind = T_ANDAND; }
        else if (*p == '=') { p++; kind = T_ANDEQ; }
        else kind = T_AMP;
        break;
    case '|':
        if (*p == '|') { p++; kind = T_OROR; }
        else if (*p == '=') { p++; kind = T_OREQ; }
        else kind = T_PIPE;
        break;
    case '*':
        if (*p == '=') { p++; kind = T_MULEQ; }
        else kind = T_STAR;
        break;
    case '/':
        if (*p == '=') { p++; kind = T_DIVEQ; }
        else kind = T_SLASH;
        break;
    case '%':
        if (*p == '=') { p++; kind = T_MODEQ; }
        else kind = T_PERCENT;
        break;
    case '^':
        if (*p == '=') { p++; kind = T_XOREQ; }
        else kind = T_CARET;
        break;
    case '!':
        if (*p == '=') { p++; kind = T_NE; }
        else kind = T_NOT;
        break;
    case '=':
        if (*p == '=') { p++; kind = T_EQ; }
        else kind = T_ASSIGN;
        break;
    case '<':
        if (*p == '<') {
            p++;
            if (*p == '=') { p++; kind = T_SHLEQ; }
            else kind = T_SHL;
        } else if (*p == '=') { p++; kind = T_LE; }
        else kind = T_LT;
        break;
    case '>':
        if (*p == '>') {
            p++;
            if (*p == '=') { p++; kind = T_SHREQ; }
            else kind = T_SHR;
        } else if (*p == '=') { p++; kind = T_GE; }
        else kind = T_GT;
        break;
    case '#':
        if (*p == '#') { p++; kind = T_HASHHASH; }
        else kind = T_HASH;
        break;
    default:
        kind = T_OTHER;
        break;
    }
    t->kind = (u8)kind;
    t->v = (u32)(p - s);
    lx->p = p;
}

void tok_spell(const Token *t, Buf *b)
{
    switch (t->kind) {
    case T_IDENT:
        buf_put(b, intern_str(t->v), intern_len(t->v));
        break;
    case T_NUM:
    case T_CHAR:
    case T_STR:
    case T_OTHER:
        buf_put(b, t->p, t->v);
        break;
    case T_EOF:
    case T_PARAM:
    case T_PLACEMARKER:
        break;
    default:
        buf_put(b, tok_names[t->kind], strlen(tok_names[t->kind]));
        break;
    }
}

/* ---- literal decoding ---- */

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

int lex_number(const Token *t, u64 *ival, double *fval, int *flags)
{
    const char *p = t->p, *end = t->p + t->v;
    u64 v = 0;
    int isfloat = 0, hex = 0, f = 0;
    const char *q;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && end - p > 2)
        hex = 1;
    if (!hex)
        for (q = p; q < end; q++)
            if (*q == '.' || *q == 'e' || *q == 'E')
                isfloat = 1;
    if (hex)
        for (q = p; q < end; q++)
            if (*q == '.' || *q == 'p' || *q == 'P')
                isfloat = 2;
    if (isfloat == 2) {
        /* hexadecimal floating constant: exact as long as the digits fit in 64 bits */
        double d = 0.0, scale;
        int ex = 0, neg = 0, n = 0, seen_dot = 0, pe = 0;
        u64 m = 0;
        for (q = p + 2; q < end && (hexval(*q) >= 0 || *q == '.'); q++) {
            if (*q == '.') {
                seen_dot = 1;
                continue;
            }
            if (n < 16) {
                m = m * 16 + (u64)hexval(*q);
                n++;
                if (seen_dot)
                    ex -= 4;
            } else if (!seen_dot) {
                ex += 4;
            }
        }
        if (q < end && (*q == 'p' || *q == 'P')) {
            q++;
            if (q < end && (*q == '+' || *q == '-'))
                neg = *q++ == '-';
            while (q < end && *q >= '0' && *q <= '9')
                pe = pe * 10 + (*q++ - '0');
            ex += neg ? -pe : pe;
        } else {
            error_at(t->loc, "hexadecimal floating constant needs an exponent");
        }
        d = (double)m;
        scale = ex < 0 ? 0.5 : 2.0;
        if (ex < 0)
            ex = -ex;
        while (ex-- > 0)
            d *= scale;
        *fval = d;
        if (q < end && (*q == 'f' || *q == 'F')) {
            f |= NUM_FLOAT;
            q++;
        } else if (q < end && (*q == 'l' || *q == 'L')) {
            f |= NUM_LONG;
            q++;
        }
        if (q != end)
            error_at(t->loc, "invalid floating constant");
        *flags = f;
        return 1;
    }
    if (isfloat == 1) {
        char tmp[128];
        char *e;
        size_t n = (size_t)(end - p);
        if (n >= sizeof tmp)
            error_at(t->loc, "floating constant too long");
        memcpy(tmp, p, n);
        tmp[n] = 0;
        *fval = strtod(tmp, &e);
        if (*e == 'f' || *e == 'F') {
            f |= NUM_FLOAT;
            e++;
        } else if (*e == 'l' || *e == 'L') {
            f |= NUM_LONG;
            e++;
        }
        if (*e || e == tmp)
            error_at(t->loc, "invalid floating constant '%s'", tmp);
        *flags = f;
        return 1;
    }
    if (hex) {
        p += 2;
        while (p < end && hexval(*p) >= 0)
            v = v * 16 + (u64)hexval(*p++);
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && end - p > 2) {
        p += 2;
        while (p < end && (*p == '0' || *p == '1'))
            v = v * 2 + (u64)(*p++ - '0');
    } else if (*p == '0') {
        while (p < end && *p >= '0' && *p <= '7')
            v = v * 8 + (u64)(*p++ - '0');
    } else {
        f |= NUM_DECIMAL;
        while (p < end && *p >= '0' && *p <= '9')
            v = v * 10 + (u64)(*p++ - '0');
    }
    for (; p < end; p++) {
        if ((*p == 'u' || *p == 'U') && !(f & NUM_UNSIGNED))
            f |= NUM_UNSIGNED;
        else if ((*p == 'l' || *p == 'L') && !(f & NUM_LONG)) {
            f |= NUM_LONG;
            if (p + 1 < end && p[1] == p[0]) {
                f |= NUM_LLONG;
                p++;
            }
        }
        else
            error_at(t->loc, "invalid integer constant '%.*s'", (int)t->v, t->p);
    }
    *ival = v;
    *flags = f;
    return 0;
}

static int escape(const char **pp, u32 loc)
{
    const char *p = *pp;
    int c = *p++, v, n;
    switch (c) {
    case 'n': v = '\n'; break;
    case 't': v = '\t'; break;
    case 'r': v = '\r'; break;
    case 'a': v = 7; break;
    case 'b': v = 8; break;
    case 'f': v = 12; break;
    case 'v': v = 11; break;
    case 'e': v = 27; break;
    case 'x':
        v = 0;
        if (hexval(*p) < 0)
            error_at(loc, "invalid hexadecimal escape");
        while (hexval(*p) >= 0)
            v = v * 16 + hexval(*p++);
        break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        v = c - '0';
        for (n = 1; n < 3 && *p >= '0' && *p <= '7'; n++)
            v = v * 8 + (*p++ - '0');
        break;
    default:
        v = c; /* \\ \' \" \? and unknown escapes */
        break;
    }
    *pp = p;
    return v;
}

int lex_char(const Token *t)
{
    const char *p = t->p, *end = t->p + t->v - 1;
    int wide = (t->flags & TF_WIDE) != 0;
    int v = 0, n = 0, c;
    p += wide ? 2 : 1;
    if (p >= end)
        error_at(t->loc, "empty character constant");
    while (p < end) {
        if (*p == '\\') {
            p++;
            c = escape(&p, t->loc);
        } else {
            c = (u8)*p++;
        }
        if (wide)
            v = c;
        else
            v = n ? (int)(((u32)v << 8) | (u32)(c & 0xff)) : (int)(signed char)c;
        n++;
    }
    return v;
}

void lex_string(const Token *t, Buf *out)
{
    const char *p = t->p, *end = t->p + t->v - 1;
    int wide = (t->flags & TF_WIDE) != 0;
    int c;
    p += wide ? 2 : 1;
    while (p < end) {
        if (*p == '\\') {
            p++;
            c = escape(&p, t->loc);
        } else {
            c = (u8)*p++;
        }
        if (wide)
            buf_u32(out, (u32)c);
        else
            buf_u8(out, (u32)c);
    }
}
