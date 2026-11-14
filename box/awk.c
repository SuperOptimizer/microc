/* awk: the pattern scanning language. The program is parsed into a tree and walked. */
#include "box.h"
#include <math.h>

enum { V_UNINIT, V_NUM, V_STR, V_STRNUM };

typedef struct Val {
    int t;
    double n;
    char *s;
} Val;

typedef struct Elem {
    struct Elem *next;
    char *key;
    Val v;
} Elem;

typedef struct Array {
    Elem **tab;
    int size, count;
} Array;

typedef struct Cell {
    Val v;
    Array *arr;
    struct Cell *ref;
} Cell;

enum {
    N_NUM, N_STR, N_REGEX, N_VAR, N_LOCAL, N_FIELD, N_INDEX, N_ASSIGN, N_COND, N_OR, N_AND, N_IN, N_MATCH, N_BIN,
    N_CONCAT, N_NEG, N_NOT, N_PLUS, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_CALL, N_BUILTIN, N_GETLINE, N_GROUP,
    S_EXPR, S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BLOCK, S_NEXT, S_EXIT, S_RETURN, S_BREAK,
    S_CONTINUE, S_DELETE, S_NEXTFILE
};

typedef struct ANode {
    int op, idx;
    struct ANode *a, *b, *c, *d, *next;
    double num;
    char *str;
    regex_t *re;
} ANode;

typedef struct AFunc {
    char *name;
    char **params;
    int nparams;
    ANode *body;
} AFunc;

typedef struct Rule {
    struct Rule *next;
    ANode *pat, *pat2, *action;
    int inrange, kind; /* 0 main, 1 BEGIN, 2 END */
} Rule;

typedef struct Stream {
    char *name;
    FILE *f;
    int pid;
} Stream;

/* tokens */
enum {
    T_EOF = 256, T_NL, T_NUMBER, T_STRING, T_NAME, T_FUNCNAME, T_BUILTIN, T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE,
    T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_NEXTFILE, T_EXIT, T_RETURN, T_DELETE, T_IN, T_GETLINE,
    T_PRINT, T_PRINTF, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_MODEQ, T_POWEQ, T_EQ, T_LE, T_GE, T_NE, T_NOMATCH, T_AND,
    T_OR, T_INCR, T_DECR, T_APPEND
};

enum { B_LENGTH, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG,
       B_SQRT, B_INT, B_RAND, B_SRAND, B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH };

static const char *const bnames[] = { "length", "substr", "index", "split", "sub", "gsub", "match", "sprintf", "sin",
                                      "cos", "atan2", "exp", "log", "sqrt", "int", "rand", "srand", "tolower",
                                      "toupper", "system", "close", "fflush", 0 };

enum { G_NF, G_NR, G_FNR, G_FS, G_OFS, G_ORS, G_RS, G_FILENAME, G_SUBSEP, G_RSTART, G_RLENGTH, G_ENVIRON, G_ARGC,
       G_ARGV, G_CONVFMT, G_OFMT, G_NSPECIAL };

static const char *src, *srcname;
static int aline, tok;
static double toknum;
static char *tokstr;
static int tokbuiltin;

static char **gnames;
static Cell *gcells;
static int nglobals;
static AFunc *afuncs;
static int nafuncs;
static int curfn = -1;
static Rule *rules, **rtail = &rules;
static int nogt, noin;

static Cell *frame;
static Val retval;
static char *rec;       /* $0 */
static char **fld;      /* $1.. */
static int nf, fldcap;
static Stream streams[32];
static int nstreams;
static int exitcode;
static FILE *mainf;
static int argi;
static unsigned long randseed = 1;

enum { X_NORMAL, X_BREAK, X_CONT, X_NEXT, X_EXIT, X_RET };

static void afail(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "awk: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (src)
        fprintf(stderr, " (%s line %d)", srcname, aline);
    fputc('\n', stderr);
    exit(2);
}

/* ---- values */

static Val mknum(double n)
{
    Val v;
    v.t = V_NUM;
    v.n = n;
    v.s = 0;
    return v;
}

static Val mkown(char *s)
{
    Val v;
    v.t = V_STR;
    v.n = 0;
    v.s = s;
    return v;
}

static Val mkstr(const char *s) { return mkown(xstrdup(s)); }

static void vfree(Val *v)
{
    free(v->s);
    v->s = 0;
}

static Val vcopy(const Val *v)
{
    Val r = *v;
    if (v->s)
        r.s = xstrdup(v->s);
    return r;
}

static int looks_numeric(const char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    if (!*s)
        return 0;
    strtod(s, &end);
    if (end == s)
        return 0;
    while (isspace((unsigned char)*end))
        end++;
    return !*end;
}

/* a string from input: numeric if it looks like a number */
static Val mkinput(const char *s)
{
    Val v = mkstr(s);
    if (looks_numeric(s)) {
        v.t = V_STRNUM;
        v.n = strtod(s, 0);
    }
    return v;
}

static double num_of(const Val *v)
{
    if (v->t == V_NUM || v->t == V_STRNUM)
        return v->n;
    if (v->t == V_STR)
        return strtod(v->s, 0);
    return 0;
}

static const char *fmtnum(double n, char *buf, const char *fmt)
{
    if (n == (double)(long)n && n > -1e15 && n < 1e15)
        sprintf(buf, "%ld", (long)n);
    else
        sprintf(buf, fmt, n);
    return buf;
}

static const char *str_of(const Val *v, char *buf)
{
    if (v->t == V_STR || v->t == V_STRNUM)
        return v->s;
    if (v->t == V_UNINIT)
        return "";
    return fmtnum(v->n, buf, gcells[G_CONVFMT].v.s ? gcells[G_CONVFMT].v.s : "%.6g");
}

static int truth(const Val *v)
{
    if (v->t == V_NUM || v->t == V_STRNUM)
        return v->n != 0;
    if (v->t == V_STR)
        return v->s[0] != 0;
    return 0;
}

/* ---- arrays */

static unsigned ahash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

static Array *newarray(void)
{
    Array *a = xmalloc(sizeof *a);
    a->size = 16;
    a->count = 0;
    a->tab = xmalloc((size_t)a->size * sizeof *a->tab);
    memset(a->tab, 0, (size_t)a->size * sizeof *a->tab);
    return a;
}

static Elem *alookup(Array *a, const char *key, int create)
{
    unsigned h = ahash(key);
    Elem *e;
    for (e = a->tab[h % (unsigned)a->size]; e; e = e->next)
        if (!strcmp(e->key, key))
            return e;
    if (!create)
        return 0;
    if (a->count >= a->size * 2) {
        int nsize = a->size * 4, i;
        Elem **nt = xmalloc((size_t)nsize * sizeof *nt);
        memset(nt, 0, (size_t)nsize * sizeof *nt);
        for (i = 0; i < a->size; i++)
            while ((e = a->tab[i]) != 0) {
                unsigned k = ahash(e->key) % (unsigned)nsize;
                a->tab[i] = e->next;
                e->next = nt[k];
                nt[k] = e;
            }
        free(a->tab);
        a->tab = nt;
        a->size = nsize;
    }
    e = xmalloc(sizeof *e);
    e->key = xstrdup(key);
    e->v.t = V_UNINIT;
    e->v.n = 0;
    e->v.s = 0;
    e->next = a->tab[h % (unsigned)a->size];
    a->tab[h % (unsigned)a->size] = e;
    a->count++;
    return e;
}

static void adelete(Array *a, const char *key)
{
    Elem **pp = &a->tab[ahash(key) % (unsigned)a->size], *e;
    for (; (e = *pp) != 0; pp = &e->next)
        if (!strcmp(e->key, key)) {
            *pp = e->next;
            free(e->key);
            vfree(&e->v);
            free(e);
            a->count--;
            return;
        }
}

static void aclear(Array *a)
{
    int i;
    Elem *e;
    for (i = 0; i < a->size; i++)
        while ((e = a->tab[i]) != 0) {
            a->tab[i] = e->next;
            free(e->key);
            vfree(&e->v);
            free(e);
        }
    a->count = 0;
}

/* ---- lexer */

static int global_index(const char *name)
{
    int i;
    for (i = 0; i < nglobals; i++)
        if (!strcmp(gnames[i], name))
            return i;
    gnames = xrealloc(gnames, (size_t)(nglobals + 1) * sizeof *gnames);
    gcells = xrealloc(gcells, (size_t)(nglobals + 1) * sizeof *gcells);
    gnames[nglobals] = xstrdup(name);
    memset(&gcells[nglobals], 0, sizeof *gcells);
    return nglobals++;
}

static int func_index(const char *name, int create)
{
    int i;
    for (i = 0; i < nafuncs; i++)
        if (!strcmp(afuncs[i].name, name))
            return i;
    if (!create)
        return -1;
    afuncs = xrealloc(afuncs, (size_t)(nafuncs + 1) * sizeof *afuncs);
    memset(&afuncs[nafuncs], 0, sizeof *afuncs);
    afuncs[nafuncs].name = xstrdup(name);
    return nafuncs++;
}

static int lex_escape(void)
{
    int c = *src++, v, n;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        v = c - '0';
        for (n = 0; n < 2 && *src >= '0' && *src <= '7'; n++)
            v = v * 8 + (*src++ - '0');
        return v;
    }
    return c;
}

static void alex(void)
{
    static const struct {
        const char *name;
        int tok;
    } kw[] = { { "BEGIN", T_BEGIN },   { "END", T_END },       { "function", T_FUNCTION }, { "func", T_FUNCTION },
               { "if", T_IF },         { "else", T_ELSE },     { "while", T_WHILE },       { "for", T_FOR },
               { "do", T_DO },         { "break", T_BREAK },   { "continue", T_CONTINUE }, { "next", T_NEXT },
               { "nextfile", T_NEXTFILE }, { "exit", T_EXIT }, { "return", T_RETURN },     { "delete", T_DELETE },
               { "in", T_IN },         { "getline", T_GETLINE }, { "print", T_PRINT },     { "printf", T_PRINTF },
               { 0, 0 } };
    int c, i;
    for (;;) {
        if (*src == ' ' || *src == '\t' || *src == '\r')
            src++;
        else if (*src == '\\' && src[1] == '\n') {
            src += 2;
            aline++;
        } else if (*src == '\\' && src[1] == '\r' && src[2] == '\n') {
            src += 3;
            aline++;
        } else if (*src == '#') {
            while (*src && *src != '\n')
                src++;
        } else
            break;
    }
    c = (unsigned char)*src;
    if (!c) {
        tok = T_EOF;
        return;
    }
    if (c == '\n') {
        src++;
        aline++;
        tok = T_NL;
        return;
    }
    if (isdigit(c) || (c == '.' && isdigit((unsigned char)src[1]))) {
        char *end;
        if (c == '0' && (src[1] == 'x' || src[1] == 'X'))
            toknum = (double)strtoul(src, &end, 16);
        else
            toknum = strtod(src, &end);
        src = end;
        tok = T_NUMBER;
        return;
    }
    if (isalpha(c) || c == '_') {
        const char *s = src;
        while (isalnum((unsigned char)*src) || *src == '_')
            src++;
        tokstr = xstrndup(s, (size_t)(src - s));
        for (i = 0; kw[i].name; i++)
            if (!strcmp(kw[i].name, tokstr)) {
                tok = kw[i].tok;
                return;
            }
        for (i = 0; bnames[i]; i++)
            if (!strcmp(bnames[i], tokstr)) {
                tok = T_BUILTIN;
                tokbuiltin = i;
                return;
            }
        tok = *src == '(' ? T_FUNCNAME : T_NAME;
        return;
    }
    if (c == '"') {
        Buf b;
        memset(&b, 0, sizeof b);
        for (src++; *src && *src != '"';) {
            if (*src == '\n')
                afail("newline in string");
            if (*src == '\\' && src[1]) {
                src++;
                if (*src == '\n') {
                    src++;
                    aline++;
                } else
                    bputc(&b, lex_escape());
            } else
                bputc(&b, *src++);
        }
        if (*src)
            src++;
        tokstr = bstr(&b);
        tok = T_STRING;
        return;
    }
    src++;
    tok = c;
    switch (c) {
    case '+':
        if (*src == '+')
            tok = T_INCR, src++;
        else if (*src == '=')
            tok = T_ADDEQ, src++;
        break;
    case '-':
        if (*src == '-')
            tok = T_DECR, src++;
        else if (*src == '=')
            tok = T_SUBEQ, src++;
        break;
    case '*':
        if (*src == '*') {
            src++;
            tok = '^';
            if (*src == '=')
                tok = T_POWEQ, src++;
        } else if (*src == '=')
            tok = T_MULEQ, src++;
        break;
    case '/':
        if (*src == '=')
            tok = T_DIVEQ, src++;
        break;
    case '%':
        if (*src == '=')
            tok = T_MODEQ, src++;
        break;
    case '^':
        if (*src == '=')
            tok = T_POWEQ, src++;
        break;
    case '=':
        if (*src == '=')
            tok = T_EQ, src++;
        break;
    case '<':
        if (*src == '=')
            tok = T_LE, src++;
        break;
    case '>':
        if (*src == '=')
            tok = T_GE, src++;
        else if (*src == '>')
            tok = T_APPEND, src++;
        break;
    case '!':
        if (*src == '=')
            tok = T_NE, src++;
        else if (*src == '~')
            tok = T_NOMATCH, src++;
        break;
    case '&':
        if (*src == '&')
            tok = T_AND, src++;
        break;
    case '|':
        if (*src == '|')
            tok = T_OR, src++;
        break;
    }
}

/* the current token is / or /=: read a regular expression instead */
static char *lex_regex(int diveq)
{
    Buf b;
    memset(&b, 0, sizeof b);
    if (diveq)
        bputc(&b, '=');
    while (*src && *src != '/') {
        if (*src == '\n')
            afail("newline in regular expression");
        if (*src == '\\' && src[1] == '/') {
            bputc(&b, '/');
            src += 2;
        } else if (*src == '\\' && src[1]) {
            bputc(&b, *src++);
            bputc(&b, *src++);
        } else if (*src == '[') {
            bputc(&b, *src++);
            if (*src == '^')
                bputc(&b, *src++);
            if (*src == ']')
                bputc(&b, *src++);
            while (*src && *src != ']' && *src != '\n') {
                if (src[0] == '[' && src[1] == ':') {
                    while (*src && !(src[0] == ':' && src[1] == ']'))
                        bputc(&b, *src++);
                    if (*src)
                        bputc(&b, *src++);
                    if (*src)
                        bputc(&b, *src++);
                } else if (*src == '\\' && src[1]) {
                    bputc(&b, *src++);
                    bputc(&b, *src++);
                } else
                    bputc(&b, *src++);
            }
        } else
            bputc(&b, *src++);
    }
    if (*src)
        src++;
    alex();
    return bstr(&b);
}

/* awk string escapes inside a regular expression source */
static regex_t *compile_re(const char *s)
{
    regex_t *re = xmalloc(sizeof *re);
    Buf b;
    int inbr = 0;
    memset(&b, 0, sizeof b);
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n': bputc(&b, '\n'); break;
            case 't': bputc(&b, '\t'); break;
            case 'r': bputc(&b, '\r'); break;
            case '/': bputc(&b, '/'); break;
            case '"': bputc(&b, '"'); break;
            default:
                if (inbr && *s != ']' && *s != '\\' && *s != '^' && *s != '-') {
                    bputc(&b, *s);
                } else if (inbr) {
                    /* POSIX brackets have no escapes: put the character where it is literal */
                    bputc(&b, '\\');
                    bputc(&b, *s);
                } else {
                    bputc(&b, '\\');
                    bputc(&b, *s);
                }
            }
            continue;
        }
        if (*s == '[' && !inbr) {
            inbr = 1;
            bputc(&b, *s);
            if (s[1] == '^')
                bputc(&b, *++s);
            if (s[1] == ']')
                bputc(&b, *++s);
            continue;
        }
        if (*s == ']' && inbr)
            inbr = 0;
        bputc(&b, *s);
    }
    if (regcomp(re, bstr(&b), REG_EXTENDED))
        afail("bad regular expression /%s/", b.s);
    free(b.s);
    return re;
}

/* ---- parser */

static ANode *p_expr(void);
static ANode *p_ternary(void);
static ANode *p_unary(void);
static ANode *p_stmt(void);
static ANode *p_block(void);

static ANode *node(int op, ANode *a, ANode *b)
{
    ANode *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static void want(int t, const char *what)
{
    if (tok != t)
        afail("syntax error: expected %s", what);
    alex();
}

static void optnl(void)
{
    while (tok == T_NL)
        alex();
}

static ANode *varnode(const char *name)
{
    ANode *n;
    int i;
    if (curfn >= 0)
        for (i = 0; i < afuncs[curfn].nparams; i++)
            if (!strcmp(afuncs[curfn].params[i], name)) {
                n = node(N_LOCAL, 0, 0);
                n->idx = i;
                return n;
            }
    n = node(N_VAR, 0, 0);
    n->idx = global_index(name);
    return n;
}

static int islvalue(ANode *n) { return n->op == N_VAR || n->op == N_LOCAL || n->op == N_FIELD || n->op == N_INDEX; }

static ANode *p_exprlist(int close)
{
    ANode *first = 0, **tail = &first;
    while (tok != close && tok != T_EOF) {
        *tail = p_expr();
        tail = &(*tail)->next;
        if (tok != ',')
            break;
        alex();
        optnl();
    }
    return first;
}

static ANode *p_primary(void)
{
    ANode *n, *e;
    switch (tok) {
    case T_NUMBER:
        n = node(N_NUM, 0, 0);
        n->num = toknum;
        alex();
        return n;
    case T_STRING:
        n = node(N_STR, 0, 0);
        n->str = tokstr;
        alex();
        return n;
    case '/':
    case T_DIVEQ:
        n = node(N_REGEX, 0, 0);
        n->str = lex_regex(tok == T_DIVEQ);
        n->re = compile_re(n->str);
        return n;
    case '$':
        alex();
        if (tok == T_INCR || tok == T_DECR) {
            int op = tok == T_INCR ? N_PREINC : N_PREDEC;
            alex();
            return node(N_FIELD, node(op, p_primary(), 0), 0);
        }
        return node(N_FIELD, p_primary(), 0);
    case '(': {
        int savegt = nogt, savein = noin;
        nogt = noin = 0;
        alex();
        e = p_expr();
        if (tok == ',') {
            alex();
            e->next = p_exprlist(')');
            want(')', ")");
            nogt = savegt;
            noin = savein;
            if (tok != T_IN)
                return node(N_GROUP, e, 0); /* the argument list of print or printf */
            alex();
            if (tok != T_NAME)
                afail("syntax error: expected array name");
            n = node(N_IN, e, varnode(tokstr));
            alex();
            return n;
        }
        want(')', ")");
        nogt = savegt;
        noin = savein;
        return node(N_GROUP, e, 0);
    }
    case '-':
        alex();
        return node(N_NEG, p_unary(), 0);
    case '+':
        alex();
        return node(N_PLUS, p_unary(), 0);
    case '!':
        alex();
        return node(N_NOT, p_unary(), 0);
    case T_INCR:
    case T_DECR: {
        int op = tok == T_INCR ? N_PREINC : N_PREDEC;
        alex();
        e = p_primary();
        if (!islvalue(e))
            afail("syntax error: ++ or -- needs a variable");
        return node(op, e, 0);
    }
    case T_NAME:
        n = varnode(tokstr);
        alex();
        if (tok == '[') {
            alex();
            e = p_exprlist(']');
            want(']', "]");
            n = node(N_INDEX, n, e);
        }
        return n;
    case T_FUNCNAME:
        n = node(N_CALL, 0, 0);
        n->idx = func_index(tokstr, 1);
        alex();
        want('(', "(");
        n->a = p_exprlist(')');
        want(')', ")");
        return n;
    case T_BUILTIN:
        n = node(N_BUILTIN, 0, 0);
        n->idx = tokbuiltin;
        alex();
        if (tok == '(') {
            int savegt = nogt, savein = noin;
            nogt = noin = 0;
            alex();
            n->a = p_exprlist(')');
            want(')', ")");
            nogt = savegt;
            noin = savein;
        } else if (n->idx != B_LENGTH)
            afail("syntax error: expected (");
        return n;
    case T_GETLINE:
        n = node(N_GETLINE, 0, 0);
        alex();
        if (tok == T_NAME || tok == '$')
            n->a = p_primary();
        if (tok == '<') {
            alex();
            n->b = p_primary();
            n->idx = 1;
        }
        return n;
    }
    afail("syntax error");
    return 0;
}

static ANode *p_postfix(void)
{
    ANode *n = p_primary();
    if ((tok == T_INCR || tok == T_DECR) && islvalue(n)) {
        n = node(tok == T_INCR ? N_POSTINC : N_POSTDEC, n, 0);
        alex();
    }
    return n;
}

static ANode *p_pow(void)
{
    ANode *n = p_postfix();
    if (tok == '^') {
        alex();
        n = node(N_BIN, n, tok == '-' || tok == '+' || tok == '!' ? p_unary() : p_pow());
        n->idx = '^';
    }
    return n;
}

static ANode *p_unary(void)
{
    if (tok == '-') {
        alex();
        return node(N_NEG, p_unary(), 0);
    }
    if (tok == '+') {
        alex();
        return node(N_PLUS, p_unary(), 0);
    }
    if (tok == '!') {
        alex();
        return node(N_NOT, p_unary(), 0);
    }
    return p_pow();
}

static ANode *p_mul(void)
{
    ANode *n = p_unary();
    while (tok == '*' || tok == '/' || tok == '%') {
        int op = tok;
        alex();
        n = node(N_BIN, n, p_unary());
        n->idx = op;
    }
    return n;
}

static ANode *p_add(void)
{
    ANode *n = p_mul();
    while (tok == '+' || tok == '-') {
        int op = tok;
        alex();
        n = node(N_BIN, n, p_mul());
        n->idx = op;
    }
    return n;
}

static int starts_operand(void)
{
    switch (tok) {
    case T_NUMBER: case T_STRING: case T_NAME: case T_FUNCNAME: case T_BUILTIN: case '$': case '(': case '-':
    case '+': case '!': case T_INCR: case T_DECR:
        return 1;
    }
    return 0;
}

static ANode *p_concat(void)
{
    ANode *n = p_add();
    while (starts_operand() && tok != '-' && tok != '+' && !(tok == '!'))
        n = node(N_CONCAT, n, p_add());
    return n;
}

static ANode *p_cmp(void)
{
    ANode *n = p_concat();
    for (;;) {
        int op = 0;
        if (tok == '|') {
            /* command | getline */
            const char *save = src;
            int saveline = aline;
            alex();
            if (tok == T_GETLINE) {
                ANode *g = node(N_GETLINE, 0, n);
                g->idx = 2;
                alex();
                if (tok == T_NAME || tok == '$')
                    g->a = p_primary();
                n = g;
                continue;
            }
            src = save;
            aline = saveline;
            tok = '|';
            return n;
        }
        switch (tok) {
        case '<': op = '<'; break;
        case T_LE: op = 'L'; break;
        case T_EQ: op = 'E'; break;
        case T_NE: op = 'N'; break;
        case T_GE: op = 'G'; break;
        case '>':
            if (!nogt)
                op = '>';
            break;
        }
        if (!op)
            return n;
        alex();
        n = node(N_BIN, n, p_concat());
        n->idx = op;
    }
}

static ANode *p_match(void)
{
    ANode *n = p_cmp();
    while (tok == '~' || tok == T_NOMATCH) {
        int neg = tok == T_NOMATCH;
        alex();
        n = node(N_MATCH, n, p_cmp());
        n->idx = neg;
    }
    return n;
}

static ANode *p_in(void)
{
    ANode *n = p_match();
    while (tok == T_IN && !noin) {
        alex();
        if (tok != T_NAME)
            afail("syntax error: expected array name");
        n = node(N_IN, n, varnode(tokstr));
        alex();
    }
    return n;
}

static ANode *p_and(void)
{
    ANode *n = p_in();
    while (tok == T_AND) {
        alex();
        optnl();
        n = node(N_AND, n, p_in());
    }
    return n;
}

static ANode *p_or(void)
{
    ANode *n = p_and();
    while (tok == T_OR) {
        alex();
        optnl();
        n = node(N_OR, n, p_and());
    }
    return n;
}

static ANode *p_ternary(void)
{
    ANode *n = p_or(), *t;
    if (tok == '?') {
        alex();
        optnl();
        t = node(N_COND, n, 0);
        t->b = p_ternary();
        optnl();
        want(':', ":");
        optnl();
        t->c = p_ternary();
        return t;
    }
    return n;
}

static ANode *p_expr(void)
{
    ANode *n = p_ternary(), *a;
    int op = 0;
    if (!islvalue(n))
        return n;
    switch (tok) {
    case '=': op = '='; break;
    case T_ADDEQ: op = '+'; break;
    case T_SUBEQ: op = '-'; break;
    case T_MULEQ: op = '*'; break;
    case T_DIVEQ: op = '/'; break;
    case T_MODEQ: op = '%'; break;
    case T_POWEQ: op = '^'; break;
    }
    if (!op)
        return n;
    alex();
    a = node(N_ASSIGN, n, p_expr());
    a->idx = op;
    return a;
}

static void endstmt(void)
{
    if (tok == ';' || tok == T_NL) {
        alex();
        return;
    }
    if (tok != '}' && tok != T_EOF)
        afail("syntax error at end of statement");
}

static ANode *p_simple(void)
{
    ANode *n;
    if (tok == T_PRINT || tok == T_PRINTF) {
        int savegt = nogt;
        n = node(tok == T_PRINT ? S_PRINT : S_PRINTF, 0, 0);
        alex();
        nogt = 1;
        if (tok != ';' && tok != T_NL && tok != '}' && tok != '>' && tok != T_APPEND && tok != '|' && tok != T_EOF) {
            n->a = p_exprlist(0);
            /* print (a, b) > "file": the parenthesized list is the argument list */
            if (n->a && !n->a->next && n->a->op == N_GROUP && n->a->a->next)
                n->a = n->a->a;
        }
        nogt = savegt;
        if (tok == '>' || tok == T_APPEND || tok == '|') {
            n->idx = tok;
            alex();
            n->b = p_concat();
        }
        return n;
    }
    if (tok == T_DELETE) {
        alex();
        if (tok != T_NAME)
            afail("syntax error: expected array name");
        n = node(S_DELETE, varnode(tokstr), 0);
        alex();
        if (tok == '[') {
            alex();
            n->b = p_exprlist(']');
            want(']', "]");
        }
        return n;
    }
    return node(S_EXPR, p_expr(), 0);
}

static ANode *p_stmt(void)
{
    ANode *n;
    optnl();
    switch (tok) {
    case '{':
        return p_block();
    case ';':
        alex();
        return node(S_BLOCK, 0, 0);
    case T_IF:
        alex();
        want('(', "(");
        n = node(S_IF, p_expr(), 0);
        want(')', ")");
        n->b = p_stmt();
        optnl();
        if (tok == ';') {
            /* "if (x) a; else b": look past the semicolon for the else */
            const char *save = src;
            int saveline = aline;
            alex();
            optnl();
            if (tok != T_ELSE) {
                src = save;
                aline = saveline;
                tok = ';';
            }
        }
        if (tok == T_ELSE) {
            alex();
            n->c = p_stmt();
        }
        return n;
    case T_WHILE:
        alex();
        want('(', "(");
        n = node(S_WHILE, p_expr(), 0);
        want(')', ")");
        if (tok == ';') {
            alex();
            n->b = node(S_BLOCK, 0, 0);
        } else
            n->b = p_stmt();
        return n;
    case T_DO:
        alex();
        n = node(S_DO, 0, 0);
        n->b = p_stmt();
        optnl();
        if (tok == ';')
            alex();
        optnl();
        want(T_WHILE, "while");
        want('(', "(");
        n->a = p_expr();
        want(')', ")");
        endstmt();
        return n;
    case T_FOR:
        alex();
        want('(', "(");
        if (tok == T_NAME) {
            /* for (name in array) */
            const char *save = src;
            int saveline = aline;
            char *name = tokstr;
            alex();
            if (tok == T_IN) {
                alex();
                if (tok == T_NAME) {
                    char *arr = tokstr;
                    alex();
                    if (tok == ')') {
                        alex();
                        n = node(S_FORIN, varnode(name), varnode(arr));
                        n->c = p_stmt();
                        return n;
                    }
                }
                afail("syntax error in for");
            }
            src = save;
            aline = saveline;
            tok = T_NAME;
            tokstr = name;
        }
        n = node(S_FOR, 0, 0);
        if (tok != ';')
            n->a = p_simple();
        want(';', ";");
        optnl();
        if (tok != ';')
            n->b = p_expr();
        want(';', ";");
        optnl();
        if (tok != ')')
            n->c = p_simple();
        want(')', ")");
        if (tok == ';') {
            alex();
            n->d = node(S_BLOCK, 0, 0);
        } else
            n->d = p_stmt();
        return n;
    case T_BREAK:
        alex();
        endstmt();
        return node(S_BREAK, 0, 0);
    case T_CONTINUE:
        alex();
        endstmt();
        return node(S_CONTINUE, 0, 0);
    case T_NEXT:
        alex();
        endstmt();
        return node(S_NEXT, 0, 0);
    case T_NEXTFILE:
        alex();
        endstmt();
        return node(S_NEXTFILE, 0, 0);
    case T_EXIT:
    case T_RETURN:
        n = node(tok == T_EXIT ? S_EXIT : S_RETURN, 0, 0);
        alex();
        if (tok != ';' && tok != T_NL && tok != '}' && tok != T_EOF)
            n->a = p_expr();
        endstmt();
        return n;
    }
    n = p_simple();
    endstmt();
    return n;
}

static ANode *p_block(void)
{
    ANode *n = node(S_BLOCK, 0, 0), **tail = &n->a;
    want('{', "{");
    for (;;) {
        optnl();
        if (tok == '}' || tok == T_EOF)
            break;
        *tail = p_stmt();
        tail = &(*tail)->next;
    }
    want('}', "}");
    return n;
}

static void parse_program(const char *text, const char *name)
{
    src = text;
    srcname = name;
    aline = 1;
    alex();
    for (;;) {
        Rule *r;
        while (tok == T_NL || tok == ';')
            alex();
        if (tok == T_EOF)
            break;
        if (tok == T_FUNCTION) {
            AFunc *f;
            int fi;
            alex();
            if (tok != T_NAME && tok != T_FUNCNAME)
                afail("syntax error: expected function name");
            fi = func_index(tokstr, 1);
            alex();
            want('(', "(");
            f = &afuncs[fi];
            while (tok == T_NAME) {
                f->params = xrealloc(f->params, (size_t)(f->nparams + 1) * sizeof *f->params);
                f->params[f->nparams++] = tokstr;
                alex();
                if (tok == ',') {
                    alex();
                    optnl();
                }
            }
            want(')', ")");
            optnl();
            curfn = fi;
            {
                ANode *body = p_block();
                afuncs[fi].body = body;
            }
            curfn = -1;
            continue;
        }
        r = xmalloc(sizeof *r);
        memset(r, 0, sizeof *r);
        if (tok == T_BEGIN || tok == T_END) {
            r->kind = tok == T_BEGIN ? 1 : 2;
            alex();
            optnl();
            r->action = p_block();
        } else {
            if (tok != '{') {
                r->pat = p_expr();
                if (tok == ',') {
                    alex();
                    optnl();
                    r->pat2 = p_expr();
                }
            }
            if (tok == '{')
                r->action = p_block();
        }
        *rtail = r;
        rtail = &r->next;
    }
    src = 0;
}

/* ---- records and fields */

static Val eval(ANode *n);
static int exec(ANode *n);

static void setvar(int g, Val v)
{
    vfree(&gcells[g].v);
    gcells[g].v = v;
}

static const char *gstr(int g, char *buf) { return str_of(&gcells[g].v, buf); }

static void clear_fields(void)
{
    int i;
    for (i = 0; i < nf; i++)
        free(fld[i]);
    nf = 0;
}

static void add_field(const char *s, size_t n)
{
    if (nf == fldcap) {
        fldcap = fldcap ? fldcap * 2 : 32;
        fld = xrealloc(fld, (size_t)fldcap * sizeof *fld);
    }
    fld[nf++] = xstrndup(s, n);
}

/* split s by the separator, calling add for each piece */
static int split_string(const char *s, const char *fs, regex_t *re, void (*add)(const char *, size_t))
{
    int n = 0;
    if (!re && !strcmp(fs, " ")) {
        for (;;) {
            const char *e;
            while (*s == ' ' || *s == '\t' || *s == '\n')
                s++;
            if (!*s)
                break;
            e = s;
            while (*e && *e != ' ' && *e != '\t' && *e != '\n')
                e++;
            add(s, (size_t)(e - s));
            n++;
            s = e;
        }
        return n;
    }
    if (!*s)
        return 0;
    if (!re && fs[0] && !fs[1] && fs[0] != '\\') {
        for (;;) {
            const char *e = strchr(s, fs[0]);
            add(s, e ? (size_t)(e - s) : strlen(s));
            n++;
            if (!e)
                break;
            s = e + 1;
        }
        return n;
    }
    {
        regex_t *r = re ? re : compile_re(fs);
        regmatch_t m[1];
        for (;;) {
            if (regexec(r, s, 1, m, 0) || m[0].rm_eo == 0) {
                if (*s && m[0].rm_eo == 0 && !regexec(r, s + 1, 1, m, REG_NOTBOL)) {
                    m[0].rm_so++;
                    m[0].rm_eo++;
                } else {
                    add(s, strlen(s));
                    n++;
                    break;
                }
            }
            add(s, (size_t)m[0].rm_so);
            n++;
            s += m[0].rm_eo;
        }
        if (!re) {
            regfree(r);
            free(r);
        }
    }
    return n;
}

static void set_nf(void) { setvar(G_NF, mknum(nf)); }

static void set_record(const char *s)
{
    char buf[64];
    char *copy = xstrdup(s);
    free(rec);
    rec = copy;
    clear_fields();
    split_string(rec, gstr(G_FS, buf), 0, add_field);
    set_nf();
}

static void rebuild_record(void)
{
    Buf b;
    char buf[64];
    const char *ofs = gstr(G_OFS, buf);
    int i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < nf; i++) {
        if (i)
            bputs(&b, ofs);
        bputs(&b, fld[i]);
    }
    free(rec);
    rec = bstr(&b);
}

static void set_field(int i, const char *s)
{
    if (i == 0) {
        set_record(s);
        return;
    }
    while (nf < i)
        add_field("", 0);
    free(fld[i - 1]);
    fld[i - 1] = xstrdup(s);
    set_nf();
    rebuild_record();
}

static void nf_changed(void)
{
    int n = (int)num_of(&gcells[G_NF].v);
    if (n < 0)
        n = 0;
    while (nf > n)
        free(fld[--nf]);
    while (nf < n)
        add_field("", 0);
    rebuild_record();
}

/* read a record; 0 at end of file */
static int read_record(FILE *f, Buf *b)
{
    char buf[64];
    const char *rs = gstr(G_RS, buf);
    int c, sep = rs[0] ? (unsigned char)rs[0] : '\n', got = 0;
    b->n = 0;
    if (!rs[0]) {
        /* paragraph mode */
        while ((c = getc(f)) == '\n')
            ;
        if (c == EOF)
            return 0;
        for (; c != EOF; c = getc(f)) {
            if (c == '\n') {
                c = getc(f);
                if (c == '\n' || c == EOF)
                    break;
                bputc(b, '\n');
            }
            bputc(b, c);
        }
        bstr(b);
        return 1;
    }
    while ((c = getc(f)) != EOF) {
        got = 1;
        if (c == sep)
            break;
        bputc(b, c);
    }
    bstr(b);
    return got;
}

/* the main input: the files named by ARGV, or standard input */
static int next_main_record(Buf *b)
{
    char buf[64];
    for (;;) {
        if (!mainf) {
            Array *argv = gcells[G_ARGV].arr;
            int argc = (int)num_of(&gcells[G_ARGC].v);
            const char *name = 0;
            static int used;
            for (; argi < argc; argi++) {
                Elem *e;
                char *eq;
                sprintf(buf, "%d", argi);
                e = alookup(argv, buf, 0);
                if (!e || !*(name = str_of(&e->v, buf)))
                    continue;
                eq = strchr(name, '=');
                if (eq && (isalpha((unsigned char)name[0]) || name[0] == '_') && !strchr(name, '/')) {
                    char *var = xstrndup(name, (size_t)(eq - name));
                    setvar(global_index(var), mkinput(eq + 1));
                    free(var);
                    continue;
                }
                break;
            }
            if (argi >= argc) {
                if (used)
                    return 0;
                used = 1;
                mainf = stdin;
                setvar(G_FILENAME, mkstr(""));
            } else {
                used = 1;
                argi++;
                if (!strcmp(name, "-"))
                    mainf = stdin;
                else if (!(mainf = fopen(name, "r"))) {
                    fflush(stdout);
                    fprintf(stderr, "awk: can't open file %s\n", name);
                    exitcode = 2;
                    continue;
                }
                setvar(G_FILENAME, mkstr(name));
            }
            setvar(G_FNR, mknum(0));
        }
        if (read_record(mainf, b))
            break;
        if (mainf != stdin)
            fclose(mainf);
        else if (argi >= (int)num_of(&gcells[G_ARGC].v)) {
            mainf = 0;
            return 0;
        }
        mainf = 0;
    }
    setvar(G_NR, mknum(num_of(&gcells[G_NR].v) + 1));
    setvar(G_FNR, mknum(num_of(&gcells[G_FNR].v) + 1));
    return 1;
}

/* ---- streams for print and getline */

static FILE *open_stream(const char *name, int mode)
{
    int i, fd[2], pid;
    FILE *f = 0;
    for (i = 0; i < nstreams; i++)
        if (!strcmp(streams[i].name, name))
            return streams[i].f;
    if (nstreams == 32)
        afail("too many open files");
    streams[nstreams].pid = 0;
    if (mode == '|' || mode == 'p') {
        static char *cmd[4];
        fflush(stdout);
        if (pipe(fd) < 0)
            afail("pipe failed");
        pid = fork();
        if (pid < 0)
            afail("fork failed");
        if (pid == 0) {
            if (mode == '|') {
                dup2(fd[0], 0);
            } else
                dup2(fd[1], 1);
            close(fd[0]);
            close(fd[1]);
            cmd[0] = (char *)"sh";
            cmd[1] = (char *)"-c";
            cmd[2] = (char *)name;
            cmd[3] = 0;
            box_exec(cmd, environ);
        }
        if (mode == '|') {
            close(fd[0]);
            f = fdopen(fd[1], "w");
        } else {
            close(fd[1]);
            f = fdopen(fd[0], "r");
        }
        streams[nstreams].pid = pid;
    } else if (!strcmp(name, "/dev/stdout") || !strcmp(name, "-"))
        f = mode == '<' ? stdin : stdout;
    else if (!strcmp(name, "/dev/stderr"))
        f = stderr;
    else
        f = fopen(name, mode == '>' ? "w" : mode == '<' ? "r" : "a");
    if (!f)
        return 0;
    streams[nstreams].name = xstrdup(name);
    streams[nstreams].f = f;
    nstreams++;
    return f;
}

static int close_stream(const char *name)
{
    int i, st = -1;
    for (i = 0; i < nstreams; i++)
        if (!strcmp(streams[i].name, name)) {
            if (streams[i].f != stdout && streams[i].f != stderr && streams[i].f != stdin)
                st = fclose(streams[i].f);
            else {
                fflush(streams[i].f);
                st = 0;
            }
            if (streams[i].pid) {
                int ws = 0;
                while (waitpid(streams[i].pid, &ws, 0) < 0 && errno == EINTR)
                    ;
                st = WIFEXITED(ws) ? WEXITSTATUS(ws) : 1;
            }
            free(streams[i].name);
            streams[i] = streams[--nstreams];
            return st;
        }
    return st;
}

/* ---- evaluation */

static Cell *cell_of(ANode *n)
{
    Cell *c = n->op == N_LOCAL ? &frame[n->idx] : &gcells[n->idx];
    return c;
}

static Array *array_of(ANode *n)
{
    Cell *c = cell_of(n);
    while (c->ref)
        c = c->ref;
    if (!c->arr)
        c->arr = newarray();
    return c->arr;
}

static char *subscript(ANode *list)
{
    Buf b;
    char buf[64], sbuf[64];
    memset(&b, 0, sizeof b);
    for (; list; list = list->next) {
        Val v = eval(list);
        bputs(&b, str_of(&v, buf));
        vfree(&v);
        if (list->next)
            bputs(&b, gstr(G_SUBSEP, sbuf));
    }
    return bstr(&b);
}

/* where a variable or array element lives */
static Val *lvalue(ANode *n)
{
    if (n->op == N_INDEX) {
        char *key = subscript(n->b);
        Elem *e = alookup(array_of(n->a), key, 1);
        free(key);
        return &e->v;
    } else {
        Cell *c = cell_of(n);
        if (c->ref) {
            Cell *t = c->ref;
            while (t->ref)
                t = t->ref;
            if (t->arr)
                afail("can't use an array as a scalar");
            c->ref = 0; /* an untyped argument used as a scalar is a local */
        }
        return &c->v;
    }
}

static Val get_field(int i)
{
    if (i == 0)
        return mkinput(rec ? rec : "");
    if (i < 0)
        afail("negative field index");
    if (i > nf) {
        Val v;
        v.t = V_UNINIT;
        v.n = 0;
        v.s = 0;
        return v;
    }
    return mkinput(fld[i - 1]);
}

static void store(ANode *target, Val v)
{
    if (target->op == N_FIELD) {
        Val iv = eval(target->a);
        char buf[64];
        int i = (int)num_of(&iv);
        vfree(&iv);
        set_field(i, str_of(&v, buf));
        vfree(&v);
    } else {
        Val *p = lvalue(target);
        vfree(p);
        *p = v;
        if (p == &gcells[G_NF].v)
            nf_changed();
    }
}

static int compare(Val *a, Val *b)
{
    char b1[64], b2[64];
    int an = a->t == V_NUM || a->t == V_STRNUM || a->t == V_UNINIT, bn = b->t == V_NUM || b->t == V_STRNUM
                                                                         || b->t == V_UNINIT;
    if (an && bn) {
        double x = num_of(a), y = num_of(b);
        return x < y ? -1 : x > y;
    }
    return strcmp(str_of(a, b1), str_of(b, b2));
}

static regex_t *regex_of(ANode *n, int *dynamic)
{
    Val v;
    char buf[64];
    regex_t *re;
    *dynamic = 0;
    if (n->op == N_REGEX)
        return n->re;
    v = eval(n);
    re = compile_re(str_of(&v, buf));
    vfree(&v);
    *dynamic = 1;
    return re;
}

static void regex_done(regex_t *re, int dynamic)
{
    if (dynamic) {
        regfree(re);
        free(re);
    }
}

static void format(Buf *out, const char *fmt, ANode *args)
{
    char spec[64], tmp[512], nbuf[64];
    for (; *fmt; fmt++) {
        size_t n = 0;
        int conv;
        Val v;
        if (*fmt != '%') {
            bputc(out, *fmt);
            continue;
        }
        if (fmt[1] == '%') {
            bputc(out, '%');
            fmt++;
            continue;
        }
        spec[n++] = *fmt++;
        while (*fmt && strchr("-+ #0123456789.*", *fmt) && n < 40) {
            if (*fmt == '*') {
                if (args) {
                    v = eval(args);
                    args = args->next;
                    n += (size_t)sprintf(spec + n, "%d", (int)num_of(&v));
                    vfree(&v);
                }
                fmt++;
            } else
                spec[n++] = *fmt++;
        }
        while (*fmt == 'l' || *fmt == 'h')
            fmt++;
        conv = *fmt;
        if (!conv)
            break;
        if (args) {
            v = eval(args);
            args = args->next;
        } else {
            v.t = V_UNINIT;
            v.n = 0;
            v.s = 0;
        }
        switch (conv) {
        case 'd': case 'i': case 'o': case 'x': case 'X': case 'u':
            spec[n++] = 'l';
            spec[n++] = (char)(conv == 'i' ? 'd' : conv);
            spec[n] = 0;
            sprintf(tmp, spec, (long)num_of(&v));
            bputs(out, tmp);
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
            spec[n++] = (char)conv;
            spec[n] = 0;
            sprintf(tmp, spec, num_of(&v));
            bputs(out, tmp);
            break;
        case 'c':
            if (v.t == V_NUM)
                bputc(out, (int)v.n);
            else {
                const char *s = str_of(&v, nbuf);
                if (*s)
                    bputc(out, *s);
            }
            break;
        case 's': {
            const char *s = str_of(&v, nbuf), *p;
            long width = 0, prec = -1, len, pad;
            int left = 0;
            spec[n] = 0;
            for (p = spec + 1; *p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0'; p++)
                if (*p == '-')
                    left = 1;
            width = strtol(p, (char **)&p, 10);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            if (*p == '.')
                prec = strtol(p + 1, 0, 10);
            len = (long)strlen(s);
            if (prec >= 0 && prec < len)
                len = prec;
            pad = width - len;
            for (; !left && pad > 0; pad--)
                bputc(out, ' ');
            bput(out, s, (size_t)len);
            for (; left && pad > 0; pad--)
                bputc(out, ' ');
            break;
        }
        default:
            spec[n++] = (char)conv;
            bput(out, spec, n);
        }
        vfree(&v);
    }
}

static int getline_into(ANode *n)
{
    Buf b;
    FILE *f = 0;
    int got;
    memset(&b, 0, sizeof b);
    if (n->idx) {
        Val name = eval(n->b);
        char buf[64];
        f = open_stream(str_of(&name, buf), n->idx == 1 ? '<' : 'p');
        vfree(&name);
        if (!f)
            return -1;
        got = read_record(f, &b);
    } else
        got = next_main_record(&b);
    if (!got) {
        free(b.s);
        return 0;
    }
    if (n->idx && !n->a)
        setvar(G_NR, mknum(num_of(&gcells[G_NR].v) + 1));
    if (n->a)
        store(n->a, mkinput(bstr(&b)));
    else
        set_record(bstr(&b));
    free(b.s);
    return 1;
}

static void substitute(Buf *out, const char *repl, const char *match, size_t len)
{
    for (; *repl; repl++) {
        if (*repl == '\\' && (repl[1] == '&' || repl[1] == '\\'))
            bputc(out, *++repl);
        else if (*repl == '&')
            bput(out, match, len);
        else
            bputc(out, *repl);
    }
}

static Array *split_target;
static int split_count;

static void split_add(const char *s, size_t n)
{
    char key[32], *piece = xstrndup(s, n);
    Elem *e;
    sprintf(key, "%d", ++split_count);
    e = alookup(split_target, key, 1);
    vfree(&e->v);
    e->v = mkinput(piece);
    free(piece);
}

static Val builtin(ANode *n)
{
    ANode *a = n->a, *b = a ? a->next : 0, *c = b ? b->next : 0;
    char b1[64], b2[64];
    Val x, y, r;
    const char *s, *t;
    regex_t *re;
    int dyn;
    switch (n->idx) {
    case B_LENGTH:
        if (!a)
            return mknum((double)strlen(rec ? rec : ""));
        if (a->op == N_VAR || a->op == N_LOCAL) {
            Cell *cl = cell_of(a);
            while (cl->ref)
                cl = cl->ref;
            if (cl->arr)
                return mknum(cl->arr->count);
        }
        x = eval(a);
        r = mknum((double)strlen(str_of(&x, b1)));
        vfree(&x);
        return r;
    case B_SUBSTR: {
        double m, len;
        long start, end, slen;
        x = eval(a);
        s = str_of(&x, b1);
        slen = (long)strlen(s);
        y = eval(b);
        m = num_of(&y);
        vfree(&y);
        m = floor(m + 0.5);
        if (c) {
            y = eval(c);
            len = floor(num_of(&y) + 0.5);
            vfree(&y);
        } else
            len = 1e18;
        start = m < 1 ? 1 : (long)m;
        end = m + len > (double)slen + 1 ? slen + 1 : (long)(m + len);
        if (end <= start || start > slen)
            r = mkstr("");
        else
            r = mkown(xstrndup(s + start - 1, (size_t)(end - start)));
        vfree(&x);
        return r;
    }
    case B_INDEX:
        x = eval(a);
        y = eval(b);
        s = str_of(&x, b1);
        t = strstr(s, str_of(&y, b2));
        r = mknum(t ? (double)(t - s) + 1 : 0);
        vfree(&x);
        vfree(&y);
        return r;
    case B_SPLIT:
        x = eval(a);
        split_target = array_of(b->op == N_GROUP ? b->a : b);
        aclear(split_target);
        split_count = 0;
        if (c && c->op == N_REGEX)
            split_string(str_of(&x, b1), "", c->re, split_add);
        else if (c) {
            y = eval(c);
            split_string(str_of(&x, b1), str_of(&y, b2), 0, split_add);
            vfree(&y);
        } else
            split_string(str_of(&x, b1), gstr(G_FS, b2), 0, split_add);
        vfree(&x);
        return mknum(split_count);
    case B_SUB:
    case B_GSUB: {
        Buf out;
        regmatch_t m[1];
        int count = 0;
        size_t at = 0, len;
        ANode *target = c;
        char *text;
        re = regex_of(a, &dyn);
        y = eval(b);
        t = str_of(&y, b2);
        if (target)
            x = eval(target);
        else
            x = get_field(0);
        text = xstrdup(str_of(&x, b1));
        vfree(&x);
        len = strlen(text);
        memset(&out, 0, sizeof out);
        while (at <= len && regexec(re, text + at, 1, m, at ? REG_NOTBOL : 0) == 0) {
            size_t so = at + (size_t)m[0].rm_so, eo = at + (size_t)m[0].rm_eo;
            bput(&out, text + at, so - at);
            substitute(&out, t, text + so, eo - so);
            count++;
            if (eo == so) {
                if (so < len)
                    bputc(&out, text[so]);
                at = so + 1;
            } else
                at = eo;
            if (n->idx == B_SUB)
                break;
        }
        if (count) {
            if (at < len)
                bput(&out, text + at, len - at);
            if (target)
                store(target, mkstr(bstr(&out)));
            else
                set_record(bstr(&out));
        }
        free(out.s);
        free(text);
        vfree(&y);
        regex_done(re, dyn);
        return mknum(count);
    }
    case B_MATCH: {
        regmatch_t m[1];
        x = eval(a);
        re = regex_of(b, &dyn);
        if (regexec(re, str_of(&x, b1), 1, m, 0) == 0) {
            setvar(G_RSTART, mknum((double)m[0].rm_so + 1));
            setvar(G_RLENGTH, mknum((double)(m[0].rm_eo - m[0].rm_so)));
        } else {
            setvar(G_RSTART, mknum(0));
            setvar(G_RLENGTH, mknum(-1));
        }
        regex_done(re, dyn);
        vfree(&x);
        return mknum(num_of(&gcells[G_RSTART].v));
    }
    case B_SPRINTF: {
        Buf out;
        memset(&out, 0, sizeof out);
        if (!a)
            return mkstr("");
        x = eval(a);
        format(&out, str_of(&x, b1), b);
        vfree(&x);
        return mkown(bstr(&out));
    }
    case B_TOLOWER:
    case B_TOUPPER: {
        char *p;
        x = eval(a);
        r = mkstr(str_of(&x, b1));
        vfree(&x);
        for (p = r.s; *p; p++)
            *p = (char)(n->idx == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p));
        return r;
    }
    case B_SYSTEM: {
        static char *cmd[4];
        int st;
        x = eval(a);
        fflush(stdout);
        cmd[0] = (char *)"sh";
        cmd[1] = (char *)"-c";
        cmd[2] = (char *)str_of(&x, b1);
        cmd[3] = 0;
        st = box_spawn(cmd);
        vfree(&x);
        return mknum(st);
    }
    case B_CLOSE:
        x = eval(a);
        r = mknum(close_stream(str_of(&x, b1)));
        vfree(&x);
        return r;
    case B_FFLUSH:
        fflush(stdout);
        return mknum(0);
    case B_RAND:
        randseed = randseed * 1103515245ul + 12345ul;
        return mknum((double)((randseed >> 16) & 0x7fff) / 32768.0);
    case B_SRAND: {
        double old = (double)randseed;
        if (a) {
            x = eval(a);
            randseed = (unsigned long)num_of(&x);
            vfree(&x);
        } else
            randseed = (unsigned long)time(0);
        return mknum(old);
    }
    }
    /* numeric functions of one or two arguments */
    if (a) {
        x = eval(a);
        r = mknum(num_of(&x));
        vfree(&x);
    } else
        r = mknum(0);
    switch (n->idx) {
    case B_INT: r.n = r.n < 0 ? ceil(r.n) : floor(r.n); break;
    case B_SQRT: r.n = sqrt(r.n); break;
    case B_EXP: r.n = exp(r.n); break;
    case B_LOG: r.n = log(r.n); break;
    case B_SIN: r.n = sin(r.n); break;
    case B_COS: r.n = cos(r.n); break;
    case B_ATAN2:
        y = b ? eval(b) : mknum(0);
        r.n = atan2(r.n, num_of(&y));
        vfree(&y);
        break;
    }
    return r;
}

static Val call(ANode *n)
{
    AFunc *f = &afuncs[n->idx];
    Cell *nf_ = xmalloc((size_t)(f->nparams + 1) * sizeof *nf_), *save = frame;
    ANode *a;
    Val r;
    int i;
    if (!f->body)
        afail("calling undefined function %s", f->name);
    memset(nf_, 0, (size_t)(f->nparams + 1) * sizeof *nf_);
    for (a = n->a, i = 0; a; a = a->next, i++) {
        if (i >= f->nparams) {
            Val v = eval(a);
            vfree(&v);
            continue;
        }
        if (a->op == N_VAR || a->op == N_LOCAL) {
            Cell *c = cell_of(a);
            while (c->ref)
                c = c->ref;
            if (c->arr || c->v.t == V_UNINIT) {
                nf_[i].ref = c;
                continue;
            }
        }
        nf_[i].v = eval(a);
    }
    frame = nf_;
    retval.t = V_UNINIT;
    retval.n = 0;
    retval.s = 0;
    exec(f->body);
    frame = save;
    for (i = 0; i < f->nparams; i++) {
        vfree(&nf_[i].v);
        if (nf_[i].arr && !nf_[i].ref) {
            aclear(nf_[i].arr);
            free(nf_[i].arr->tab);
            free(nf_[i].arr);
        }
    }
    free(nf_);
    r = retval;
    retval.s = 0;
    retval.t = V_UNINIT;
    return r;
}


static Val eval(ANode *n)
{
    Val x, y, r;
    char b1[64], b2[64];
    double d;
    switch (n->op) {
    case N_NUM:
        return mknum(n->num);
    case N_STR:
        return mkstr(n->str);
    case N_GROUP:
        return eval(n->a);
    case N_REGEX:
        return mknum(regexec(n->re, rec ? rec : "", 0, 0, 0) == 0);
    case N_VAR:
    case N_LOCAL: {
        Cell *c = cell_of(n);
        while (c->ref)
            c = c->ref;
        return vcopy(&c->v);
    }
    case N_INDEX:
        return vcopy(lvalue(n));
    case N_FIELD:
        x = eval(n->a);
        r = get_field((int)num_of(&x));
        vfree(&x);
        return r;
    case N_ASSIGN:
        y = eval(n->b);
        if (n->idx != '=') {
            double l, rr = num_of(&y);
            x = eval(n->a);
            l = num_of(&x);
            vfree(&x);
            vfree(&y);
            switch (n->idx) {
            case '+': l += rr; break;
            case '-': l -= rr; break;
            case '*': l *= rr; break;
            case '/':
                if (rr == 0)
                    afail("division by zero");
                l /= rr;
                break;
            case '%':
                if (rr == 0)
                    afail("division by zero");
                l = fmod(l, rr);
                break;
            default: l = pow(l, rr);
            }
            y = mknum(l);
        } else if (y.t == V_UNINIT)
            y = mkstr("");
        store(n->a, vcopy(&y));
        return y;
    case N_COND:
        x = eval(n->a);
        d = truth(&x);
        vfree(&x);
        return eval(d ? n->b : n->c);
    case N_OR:
    case N_AND:
        x = eval(n->a);
        d = truth(&x);
        vfree(&x);
        if ((d != 0) == (n->op == N_OR))
            return mknum(d != 0);
        x = eval(n->b);
        d = truth(&x);
        vfree(&x);
        return mknum(d != 0);
    case N_NOT:
        x = eval(n->a);
        d = !truth(&x);
        vfree(&x);
        return mknum(d);
    case N_NEG:
    case N_PLUS:
        x = eval(n->a);
        d = num_of(&x);
        vfree(&x);
        return mknum(n->op == N_NEG ? -d : d);
    case N_IN: {
        Cell *c = cell_of(n->b);
        char *key = subscript(n->a->op == N_GROUP ? n->a->a : n->a);
        while (c->ref)
            c = c->ref;
        d = c->arr && alookup(c->arr, key, 0);
        free(key);
        return mknum(d);
    }
    case N_MATCH: {
        int dyn;
        regex_t *re;
        x = eval(n->a);
        re = regex_of(n->b, &dyn);
        d = regexec(re, str_of(&x, b1), 0, 0, 0) == 0;
        regex_done(re, dyn);
        vfree(&x);
        return mknum(n->idx ? !d : d);
    }
    case N_CONCAT: {
        const char *s, *t;
        char *o;
        size_t ls;
        x = eval(n->a);
        y = eval(n->b);
        s = str_of(&x, b1);
        t = str_of(&y, b2);
        ls = strlen(s);
        o = xmalloc(ls + strlen(t) + 1);
        strcpy(o, s);
        strcpy(o + ls, t);
        vfree(&x);
        vfree(&y);
        return mkown(o);
    }
    case N_BIN: {
        double l, rr;
        x = eval(n->a);
        y = eval(n->b);
        if (strchr("<>LGEN", n->idx)) {
            int c = compare(&x, &y);
            vfree(&x);
            vfree(&y);
            switch (n->idx) {
            case '<': return mknum(c < 0);
            case '>': return mknum(c > 0);
            case 'L': return mknum(c <= 0);
            case 'G': return mknum(c >= 0);
            case 'E': return mknum(c == 0);
            default: return mknum(c != 0);
            }
        }
        l = num_of(&x);
        rr = num_of(&y);
        vfree(&x);
        vfree(&y);
        switch (n->idx) {
        case '+': return mknum(l + rr);
        case '-': return mknum(l - rr);
        case '*': return mknum(l * rr);
        case '/':
            if (rr == 0)
                afail("division by zero");
            return mknum(l / rr);
        case '%':
            if (rr == 0)
                afail("division by zero");
            return mknum(fmod(l, rr));
        default: return mknum(pow(l, rr));
        }
    }
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC: {
        double delta = n->op == N_PREINC || n->op == N_POSTINC ? 1 : -1;
        x = eval(n->a);
        d = num_of(&x);
        vfree(&x);
        store(n->a, mknum(d + delta));
        return mknum(n->op == N_PREINC || n->op == N_PREDEC ? d + delta : d);
    }
    case N_CALL:
        return call(n);
    case N_BUILTIN:
        return builtin(n);
    case N_GETLINE:
        return mknum(getline_into(n));
    }
    afail("internal error: bad expression");
    return mknum(0);
}

static void print_value(FILE *f, Val *v)
{
    char buf[64];
    if (v->t == V_NUM)
        fputs(fmtnum(v->n, buf, gcells[G_OFMT].v.s ? gcells[G_OFMT].v.s : "%.6g"), f);
    else
        fputs(str_of(v, buf), f);
}

static int exec(ANode *n)
{
    int r;
    Val v;
    char buf[64];
    for (; n; n = n->next) {
        switch (n->op) {
        case S_EXPR:
            v = eval(n->a);
            vfree(&v);
            break;
        case S_BLOCK:
            r = exec(n->a);
            if (r != X_NORMAL)
                return r;
            break;
        case S_PRINT:
        case S_PRINTF: {
            FILE *f = stdout;
            ANode *a;
            if (n->b) {
                v = eval(n->b);
                f = open_stream(str_of(&v, buf), n->idx == '>' ? '>' : n->idx == '|' ? '|' : 'a');
                if (!f)
                    afail("can't redirect to %s", str_of(&v, buf));
                vfree(&v);
            }
            if (n->op == S_PRINTF) {
                Buf out;
                memset(&out, 0, sizeof out);
                if (n->a) {
                    v = eval(n->a);
                    format(&out, str_of(&v, buf), n->a->next);
                    vfree(&v);
                    fwrite(out.s, 1, out.n, f);
                    free(out.s);
                }
            } else {
                char obuf[64];
                if (!n->a)
                    fputs(rec ? rec : "", f);
                for (a = n->a; a; a = a->next) {
                    v = eval(a);
                    print_value(f, &v);
                    vfree(&v);
                    if (a->next)
                        fputs(gstr(G_OFS, obuf), f);
                }
                fputs(gstr(G_ORS, obuf), f);
            }
            if (f == stderr || n->idx == '|')
                fflush(f);
            break;
        }
        case S_IF:
            v = eval(n->a);
            r = truth(&v);
            vfree(&v);
            r = r ? exec(n->b) : n->c ? exec(n->c) : X_NORMAL;
            if (r != X_NORMAL)
                return r;
            break;
        case S_WHILE:
            for (;;) {
                v = eval(n->a);
                r = truth(&v);
                vfree(&v);
                if (!r)
                    break;
                r = exec(n->b);
                if (r == X_BREAK)
                    break;
                if (r != X_NORMAL && r != X_CONT)
                    return r;
            }
            break;
        case S_DO:
            for (;;) {
                r = exec(n->b);
                if (r == X_BREAK)
                    break;
                if (r != X_NORMAL && r != X_CONT)
                    return r;
                v = eval(n->a);
                r = truth(&v);
                vfree(&v);
                if (!r)
                    break;
            }
            break;
        case S_FOR:
            if (n->a) {
                ANode *init = n->a, *save = init->next;
                init->next = 0;
                exec(init);
                init->next = save;
            }
            for (;;) {
                if (n->b) {
                    v = eval(n->b);
                    r = truth(&v);
                    vfree(&v);
                    if (!r)
                        break;
                }
                r = exec(n->d);
                if (r == X_BREAK)
                    break;
                if (r != X_NORMAL && r != X_CONT)
                    return r;
                if (n->c)
                    exec(n->c);
            }
            break;
        case S_FORIN: {
            Array *a = array_of(n->b);
            char **keys = xmalloc((size_t)(a->count + 1) * sizeof *keys);
            int count = 0, i, stop = X_NORMAL;
            Elem *e;
            for (i = 0; i < a->size; i++)
                for (e = a->tab[i]; e; e = e->next)
                    keys[count++] = xstrdup(e->key);
            for (i = 0; i < count; i++) {
                if (stop == X_NORMAL && alookup(a, keys[i], 0)) {
                    store(n->a, mkinput(keys[i]));
                    r = exec(n->c);
                    if (r != X_NORMAL && r != X_CONT)
                        stop = r;
                }
                free(keys[i]);
            }
            free(keys);
            if (stop != X_NORMAL && stop != X_BREAK)
                return stop;
            break;
        }
        case S_BREAK:
            return X_BREAK;
        case S_CONTINUE:
            return X_CONT;
        case S_NEXT:
            return X_NEXT;
        case S_NEXTFILE:
            if (mainf && mainf != stdin)
                fclose(mainf);
            mainf = 0;
            return X_NEXT;
        case S_EXIT:
            if (n->a) {
                v = eval(n->a);
                exitcode = (int)num_of(&v);
                vfree(&v);
            }
            return X_EXIT;
        case S_RETURN:
            if (n->a)
                retval = eval(n->a);
            return X_RET;
        case S_DELETE: {
            Array *a = array_of(n->a);
            if (n->b) {
                char *key = subscript(n->b);
                adelete(a, key);
                free(key);
            } else
                aclear(a);
            break;
        }
        default:
            afail("internal error: bad statement");
        }
    }
    return X_NORMAL;
}

static int run_rules(int kind)
{
    Rule *r;
    for (r = rules; r; r = r->next) {
        int m = 1, x;
        Val v;
        if (r->kind != kind)
            continue;
        if (r->pat2) {
            if (!r->inrange) {
                v = eval(r->pat);
                m = truth(&v);
                vfree(&v);
                if (m)
                    r->inrange = 1;
            }
            if (r->inrange) {
                v = eval(r->pat2);
                if (truth(&v))
                    r->inrange = 0;
                vfree(&v);
                m = 1;
            }
        } else if (r->pat) {
            v = eval(r->pat);
            m = truth(&v);
            vfree(&v);
        }
        if (!m)
            continue;
        if (!r->action) {
            char obuf[64];
            fputs(rec ? rec : "", stdout);
            fputs(gstr(G_ORS, obuf), stdout);
            continue;
        }
        x = exec(r->action);
        if (x == X_NEXT)
            return X_NEXT;
        if (x == X_EXIT)
            return X_EXIT;
    }
    return X_NORMAL;
}

int awk_main(int argc, char **argv)
{
    static const char *const special[] = { "NF", "NR", "FNR", "FS", "OFS", "ORS", "RS", "FILENAME", "SUBSEP", "RSTART",
                                           "RLENGTH", "ENVIRON", "ARGC", "ARGV", "CONVFMT", "OFMT" };
    int i, n = 0, hasmain = 0, hasend = 0;
    const char *fs = 0;
    char *prog_text = 0, key[32], **e;
    Buf progbuf, rb;
    Rule *r;
    char **assigns = 0;
    int nassigns = 0;
    memset(&progbuf, 0, sizeof progbuf);
    memset(&rb, 0, sizeof rb);
    for (i = 0; i < G_NSPECIAL; i++)
        global_index(special[i]);
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (a[1] == 'F')
            fs = a[2] ? a + 2 : argv[++i];
        else if (a[1] == 'v') {
            assigns = xrealloc(assigns, (size_t)(nassigns + 1) * sizeof *assigns);
            assigns[nassigns++] = (char *)(a[2] ? a + 2 : argv[++i]);
        } else if (a[1] == 'f') {
            const char *name = a[2] ? a + 2 : argv[++i];
            char *t = name ? readfile(name, 0) : 0;
            if (!t)
                die("can't open file %s", name ? name : "");
            bputs(&progbuf, t);
            bputc(&progbuf, '\n');
            prog_text = progbuf.s;
        } else
            die("unknown option %s", a);
    }
    if (!prog_text) {
        if (i >= argc)
            die("usage: awk [-F fs] [-v var=value] [-f file | 'program'] [file ...]");
        prog_text = argv[i++];
    } else
        prog_text = bstr(&progbuf);
    setvar(G_FS, mkstr(" "));
    setvar(G_OFS, mkstr(" "));
    setvar(G_ORS, mkstr("\n"));
    setvar(G_RS, mkstr("\n"));
    setvar(G_SUBSEP, mkstr("\034"));
    setvar(G_CONVFMT, mkstr("%.6g"));
    setvar(G_OFMT, mkstr("%.6g"));
    setvar(G_NF, mknum(0));
    setvar(G_NR, mknum(0));
    setvar(G_FNR, mknum(0));
    parse_program(prog_text, "program");
    if (fs) {
        if (!strcmp(fs, "t"))
            fs = "\t";
        else if (!strcmp(fs, "\\t"))
            fs = "\t";
        setvar(G_FS, mkstr(fs));
    }
    gcells[G_ENVIRON].arr = newarray();
    for (e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (eq) {
            char *name = xstrndup(*e, (size_t)(eq - *e));
            alookup(gcells[G_ENVIRON].arr, name, 1)->v = mkinput(eq + 1);
            free(name);
        }
    }
    gcells[G_ARGV].arr = newarray();
    alookup(gcells[G_ARGV].arr, "0", 1)->v = mkstr("awk");
    for (n = 1; i < argc; i++, n++) {
        sprintf(key, "%d", n);
        alookup(gcells[G_ARGV].arr, key, 1)->v = mkinput(argv[i]);
    }
    setvar(G_ARGC, mknum(n));
    argi = 1;
    for (i = 0; i < nassigns; i++) {
        char *eq = strchr(assigns[i], '='), *name;
        Buf vb;
        const char *p;
        if (!eq)
            die("bad -v assignment %s", assigns[i]);
        name = xstrndup(assigns[i], (size_t)(eq - assigns[i]));
        memset(&vb, 0, sizeof vb);
        for (p = eq + 1; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                bputc(&vb, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
            } else
                bputc(&vb, *p);
        }
        setvar(global_index(name), mkinput(bstr(&vb)));
        free(name);
    }
    for (r = rules; r; r = r->next) {
        if (r->kind == 0)
            hasmain = 1;
        if (r->kind == 2)
            hasend = 1;
    }
    rec = xstrdup("");
    if (run_rules(1) != X_EXIT && (hasmain || hasend))
        while (next_main_record(&rb)) {
            set_record(bstr(&rb));
            if (run_rules(0) == X_EXIT)
                break;
        }
    run_rules(2);
    fflush(stdout);
    while (nstreams)
        close_stream(streams[0].name);
    return exitcode;
}
