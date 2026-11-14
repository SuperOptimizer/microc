/* A POSIX shell: enough of sh(1) to run build systems. The script text is parsed
   one command line at a time into a small tree, which is then walked. Words are
   kept as written and expanded when the command runs. */
#include "box.h"

enum { T_EOF, T_NL, T_WORD, T_AND, T_OR, T_SEMI, T_DSEMI, T_AMP, T_PIPE, T_LP, T_RP, T_REDIR };
enum { R_IN, R_OUT, R_APP, R_RW, R_DUPIN, R_DUPOUT, R_HERE, R_HERETAB };
enum { N_CMD, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_SUB, N_GROUP, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_FUNC, N_NOT };

typedef struct Redir {
    struct Redir *next;
    int fd, op, quoted;
    char *word, *body;
} Redir;

typedef struct Case {
    struct Case *next;
    char **pats;
    int npats;
    struct Node *body;
} Case;

typedef struct Node {
    int type, line;
    struct Node *a, *b, *c, *next;
    char **words, **assigns;
    int nwords, nassigns;
    char *name;
    Redir *redirs;
    Case *cases;
} Node;

typedef struct Parser {
    const char *p;
    int line, tok, rfd, rop, nhere;
    char *text;
    Redir *here[16];
} Parser;

typedef struct Var {
    struct Var *next;
    char *name, *val;
    int flags;
} Var;
#define V_EXPORT 1
#define V_RDONLY 2

typedef struct Func {
    struct Func *next;
    char *name;
    Node *body;
} Func;

typedef struct Local {
    char *name, *val;
    int flags;
} Local;

#define F_LAST 1 /* the shell exits after this command, so it may exec in place */

static Parser *P;
static Var *vars[256];
static Func *funcs;
static Local *locals;
static int nlocals, caplocals;
static int status, substatus;
static int eflag, xflag, uflag, fflag;
static int noerr, brkn, contn, retp, loopdepth, funcdepth;
static char **posv;
static int posc;
static char *arg0;
static char *trap_exit;
static int shpid, bgpid, curline;
static int optpos;

static int run(Node *n, int flags);
static int run_string(const char *s);
static void sh_exit(int st);
static char *exp_str(const char *word);

static void sh_err(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s: ", arg0 ? arg0 : "sh");
    if (curline)
        fprintf(stderr, "line %d: ", curline);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- scanning quoted text; each returns the position after the construct */

static const char *scan_dollar(const char *p, int indq);

static const char *scan_sq(const char *p)
{
    while (*p && *p != '\'')
        p++;
    return *p ? p + 1 : p;
}

static const char *scan_bq(const char *p)
{
    while (*p && *p != '`') {
        if (*p == '\\' && p[1])
            p++;
        p++;
    }
    return *p ? p + 1 : p;
}

static const char *scan_dq(const char *p)
{
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p += 2;
        else if (*p == '`')
            p = scan_bq(p + 1);
        else if (*p == '$')
            p = scan_dollar(p, 1);
        else
            p++;
    }
    return *p ? p + 1 : p;
}

static const char *scan_paren(const char *p)
{
    int depth = 1;
    while (*p) {
        switch (*p) {
        case '\\':
            p += p[1] ? 2 : 1;
            break;
        case '\'':
            p = scan_sq(p + 1);
            break;
        case '"':
            p = scan_dq(p + 1);
            break;
        case '`':
            p = scan_bq(p + 1);
            break;
        case '$':
            p = scan_dollar(p, 0);
            break;
        case '(':
            depth++;
            p++;
            break;
        case ')':
            p++;
            if (--depth == 0)
                return p;
            break;
        default:
            p++;
        }
    }
    return p;
}

static const char *scan_brace(const char *p, int indq)
{
    while (*p) {
        switch (*p) {
        case '}':
            return p + 1;
        case '\\':
            p += p[1] ? 2 : 1;
            break;
        case '\'':
            p = indq ? p + 1 : scan_sq(p + 1);
            break;
        case '"':
            p = scan_dq(p + 1);
            break;
        case '`':
            p = scan_bq(p + 1);
            break;
        case '$':
            p = scan_dollar(p, indq);
            break;
        default:
            p++;
        }
    }
    return p;
}

static const char *scan_dollar(const char *p, int indq)
{
    if (p[1] == '(')
        return scan_paren(p + 2);
    if (p[1] == '{')
        return scan_brace(p + 2, indq);
    return p + 1;
}

/* the text inside a construct that began at `from` (after the opener) and was
   scanned to `to`; `close` is the closing character */
static char *inner(const char *from, const char *to, int close)
{
    if (to > from && to[-1] == close)
        to--;
    return xstrndup(from, (size_t)(to - from));
}

/* ---- lexer */

static void syntax(const char *msg)
{
    curline = P->line;
    sh_err("syntax error: %s", msg);
    fflush(stderr);
    exit(2);
}

static char *unquote(const char *w, int *quoted)
{
    Buf b;
    memset(&b, 0, sizeof b);
    *quoted = 0;
    while (*w) {
        if (*w == '\\' && w[1]) {
            *quoted = 1;
            bputc(&b, w[1]);
            w += 2;
        } else if (*w == '\'' || *w == '"') {
            int q = *w++;
            *quoted = 1;
            while (*w && *w != q) {
                if (q == '"' && *w == '\\' && (w[1] == '"' || w[1] == '\\' || w[1] == '$' || w[1] == '`'))
                    w++;
                bputc(&b, *w++);
            }
            if (*w)
                w++;
        } else
            bputc(&b, *w++);
    }
    return bstr(&b);
}

static void read_heredocs(void)
{
    int i;
    for (i = 0; i < P->nhere; i++) {
        Redir *r = P->here[i];
        char *delim = unquote(r->word, &r->quoted);
        size_t dn = strlen(delim);
        Buf b;
        const char *p = P->p;
        memset(&b, 0, sizeof b);
        while (*p) {
            const char *e;
            if (r->op == R_HERETAB)
                while (*p == '\t')
                    p++;
            e = strchr(p, '\n');
            if (!e)
                e = p + strlen(p);
            P->line++;
            if ((size_t)(e - p) == dn && !memcmp(p, delim, dn)) {
                p = *e ? e + 1 : e;
                break;
            }
            bput(&b, p, (size_t)(e - p));
            bputc(&b, '\n');
            p = *e ? e + 1 : e;
        }
        P->p = p;
        r->body = bstr(&b);
        r->op = R_HERE;
        free(delim);
    }
    P->nhere = 0;
}

static void lex(void)
{
    const char *p = P->p, *s;
    for (;;) {
        if (*p == ' ' || *p == '\t')
            p++;
        else if (*p == '\\' && p[1] == '\n') {
            p += 2;
            P->line++;
        } else if (*p == '#') {
            while (*p && *p != '\n')
                p++;
        } else
            break;
    }
    P->rfd = -1;
    s = p;
    switch (*p) {
    case 0:
        P->tok = T_EOF;
        break;
    case '\n':
        P->tok = T_NL;
        P->line++;
        P->p = p + 1;
        if (P->nhere)
            read_heredocs();
        return;
    case '&':
        if (p[1] == '&')
            P->tok = T_AND, p += 2;
        else
            P->tok = T_AMP, p++;
        break;
    case '|':
        if (p[1] == '|')
            P->tok = T_OR, p += 2;
        else
            P->tok = T_PIPE, p++;
        break;
    case ';':
        if (p[1] == ';')
            P->tok = T_DSEMI, p += 2;
        else
            P->tok = T_SEMI, p++;
        break;
    case '(':
        P->tok = T_LP, p++;
        break;
    case ')':
        P->tok = T_RP, p++;
        break;
    default:
        while (*p) {
            int c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' || c == ')'
                || c == '<' || c == '>')
                break;
            if (c == '\\') {
                if (p[1] == '\n')
                    P->line++;
                p += p[1] ? 2 : 1;
            } else if (c == '\'')
                p = scan_sq(p + 1);
            else if (c == '"')
                p = scan_dq(p + 1);
            else if (c == '`')
                p = scan_bq(p + 1);
            else if (c == '$')
                p = scan_dollar(p, 0);
            else
                p++;
        }
        if (*p == '<' || *p == '>') {
            const char *d = s;
            while (d < p && isdigit((unsigned char)*d))
                d++;
            if (d == p) {
                if (p > s)
                    P->rfd = atoi(s);
                P->tok = T_REDIR;
                if (*p == '<') {
                    if (p[1] == '<') {
                        if (p[2] == '-')
                            P->rop = R_HERETAB, p += 3;
                        else
                            P->rop = R_HERE, p += 2;
                    } else if (p[1] == '&')
                        P->rop = R_DUPIN, p += 2;
                    else if (p[1] == '>')
                        P->rop = R_RW, p += 2;
                    else
                        P->rop = R_IN, p++;
                } else {
                    if (p[1] == '>')
                        P->rop = R_APP, p += 2;
                    else if (p[1] == '&')
                        P->rop = R_DUPOUT, p += 2;
                    else if (p[1] == '|')
                        P->rop = R_OUT, p += 2;
                    else
                        P->rop = R_OUT, p++;
                }
                break;
            }
        }
        {
            const char *q;
            for (q = s; q < p; q++)
                if (*q == '\n')
                    P->line++;
        }
        P->tok = T_WORD;
        P->text = xstrndup(s, (size_t)(p - s));
        break;
    }
    P->p = p;
}

/* ---- parser */

static Node *parse_list(int oneline);
static Node *parse_command(void);

static Node *mk(int type, Node *a, Node *b)
{
    Node *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = type;
    n->a = a;
    n->b = b;
    n->line = P->line;
    return n;
}

static int isres(const char *w) { return P->tok == T_WORD && !strcmp(P->text, w); }

static void expect(const char *w)
{
    if (!isres(w)) {
        char msg[64];
        sprintf(msg, "expected '%.40s'", w);
        syntax(msg);
    }
    lex();
}

static void skipnl(void)
{
    while (P->tok == T_NL)
        lex();
}

static int list_end(void)
{
    if (P->tok == T_EOF || P->tok == T_RP || P->tok == T_DSEMI)
        return 1;
    if (P->tok != T_WORD)
        return 0;
    return isres("then") || isres("else") || isres("elif") || isres("fi") || isres("do") || isres("done")
           || isres("esac") || isres("}");
}

static void push(char ***v, int *n, char *s)
{
    if ((*n & (*n + 1)) == 0)
        *v = xrealloc(*v, (size_t)(*n + 1) * 2 * sizeof(char *));
    (*v)[(*n)++] = s;
    (*v)[*n] = 0;
}

static void parse_redir(Node *n)
{
    Redir *r = xmalloc(sizeof *r), **pp;
    memset(r, 0, sizeof *r);
    r->fd = P->rfd;
    r->op = P->rop;
    lex();
    if (P->tok != T_WORD)
        syntax("missing redirection target");
    r->word = P->text;
    if (r->op == R_HERE || r->op == R_HERETAB) {
        if (P->nhere == 16)
            syntax("too many here-documents");
        P->here[P->nhere++] = r;
    }
    lex();
    for (pp = &n->redirs; *pp; pp = &(*pp)->next)
        ;
    *pp = r;
}

static int isname(const char *s, size_t n)
{
    size_t i;
    if (!n || isdigit((unsigned char)s[0]))
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_')
            return 0;
    return 1;
}

static int isassign(const char *w)
{
    const char *eq = strchr(w, '=');
    return eq && isname(w, (size_t)(eq - w));
}

static Node *parse_if(void)
{
    Node *n = mk(N_IF, 0, 0);
    n->a = parse_list(0);
    expect("then");
    n->b = parse_list(0);
    if (isres("elif")) {
        lex();
        n->c = parse_if();
        return n;
    }
    if (isres("else")) {
        lex();
        n->c = parse_list(0);
    }
    expect("fi");
    return n;
}

static Node *parse_case(void)
{
    Node *n = mk(N_CASE, 0, 0);
    Case **tail = &n->cases;
    if (P->tok != T_WORD)
        syntax("bad case");
    n->name = P->text;
    lex();
    skipnl();
    expect("in");
    skipnl();
    while (!isres("esac")) {
        Case *c = xmalloc(sizeof *c);
        memset(c, 0, sizeof *c);
        if (P->tok == T_LP)
            lex();
        for (;;) {
            if (P->tok != T_WORD)
                syntax("bad case pattern");
            push(&c->pats, &c->npats, P->text);
            lex();
            if (P->tok != T_PIPE)
                break;
            lex();
        }
        if (P->tok != T_RP)
            syntax("expected ')'");
        lex();
        c->body = parse_list(0);
        *tail = c;
        tail = &c->next;
        if (P->tok == T_DSEMI)
            lex();
        skipnl();
        if (P->tok == T_EOF)
            syntax("unterminated case");
    }
    lex();
    return n;
}

static Node *parse_command(void)
{
    Node *n;
    if (P->tok == T_LP) {
        lex();
        n = mk(N_SUB, 0, 0);
        n->a = parse_list(0);
        if (P->tok != T_RP)
            syntax("expected ')'");
        lex();
    } else if (isres("{")) {
        lex();
        n = mk(N_GROUP, 0, 0);
        n->a = parse_list(0);
        expect("}");
    } else if (isres("if")) {
        lex();
        n = parse_if();
    } else if (isres("while") || isres("until")) {
        n = mk(isres("while") ? N_WHILE : N_UNTIL, 0, 0);
        lex();
        n->a = parse_list(0);
        expect("do");
        n->b = parse_list(0);
        expect("done");
    } else if (isres("for")) {
        lex();
        n = mk(N_FOR, 0, 0);
        if (P->tok != T_WORD)
            syntax("bad for");
        n->name = P->text;
        n->nwords = -1;
        lex();
        skipnl();
        if (isres("in")) {
            lex();
            n->nwords = 0;
            while (P->tok == T_WORD) {
                push(&n->words, &n->nwords, P->text);
                lex();
            }
        }
        if (P->tok == T_SEMI)
            lex();
        skipnl();
        expect("do");
        n->b = parse_list(0);
        expect("done");
    } else if (isres("case")) {
        lex();
        n = parse_case();
    } else {
        n = mk(N_CMD, 0, 0);
        for (;;) {
            if (P->tok == T_WORD) {
                if (!n->nwords && isassign(P->text))
                    push(&n->assigns, &n->nassigns, P->text);
                else
                    push(&n->words, &n->nwords, P->text);
                lex();
                if (P->tok == T_LP && n->nwords == 1 && !n->nassigns && !n->redirs) {
                    Node *f = mk(N_FUNC, 0, 0);
                    lex();
                    if (P->tok != T_RP)
                        syntax("bad function definition");
                    lex();
                    skipnl();
                    f->name = n->words[0];
                    f->a = parse_command();
                    return f;
                }
            } else if (P->tok == T_REDIR)
                parse_redir(n);
            else
                break;
        }
        if (!n->nwords && !n->nassigns && !n->redirs)
            syntax("unexpected token");
        return n;
    }
    while (P->tok == T_REDIR)
        parse_redir(n);
    return n;
}

static Node *parse_pipeline(void)
{
    int neg = 0;
    Node *first, *last, *n;
    if (isres("!")) {
        neg = 1;
        lex();
    }
    first = last = parse_command();
    while (P->tok == T_PIPE) {
        lex();
        skipnl();
        last->next = parse_command();
        last = last->next;
    }
    n = first;
    if (first != last) {
        n = mk(N_PIPE, first, 0);
        n->line = first->line;
    }
    if (neg)
        n = mk(N_NOT, n, 0);
    return n;
}

static Node *parse_andor(void)
{
    Node *n = parse_pipeline();
    while (P->tok == T_AND || P->tok == T_OR) {
        int type = P->tok == T_AND ? N_AND : N_OR;
        lex();
        skipnl();
        n = mk(type, n, parse_pipeline());
    }
    return n;
}

static Node *parse_list(int oneline)
{
    Node *list = 0, *n;
    for (;;) {
        if (P->tok == T_NL) {
            if (oneline && list)
                break;
            lex();
            continue;
        }
        if (list_end()) {
            if (oneline && P->tok != T_EOF)
                syntax("unexpected token");
            break;
        }
        n = parse_andor();
        if (P->tok == T_AMP) {
            n = mk(N_BG, n, 0);
            lex();
        } else if (P->tok == T_SEMI)
            lex();
        else if (P->tok != T_NL && !list_end())
            syntax("unexpected token");
        list = list ? mk(N_SEQ, list, n) : n;
    }
    return list;
}

/* ---- variables */

static unsigned hash(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h & 255;
}

static Var *var_find(const char *name, int create)
{
    unsigned h = hash(name);
    Var *v;
    for (v = vars[h]; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    if (!create)
        return 0;
    v = xmalloc(sizeof *v);
    v->name = xstrdup(name);
    v->val = 0;
    v->flags = 0;
    v->next = vars[h];
    vars[h] = v;
    return v;
}

static const char *var_get(const char *name)
{
    Var *v = var_find(name, 0);
    return v ? v->val : 0;
}

static void var_set(const char *name, const char *val, int flags)
{
    Var *v = var_find(name, 1);
    if (v->flags & V_RDONLY) {
        sh_err("%s: is read only", name);
        return;
    }
    if (val) {
        char *old = v->val;
        v->val = xstrdup(val);
        free(old);
    }
    v->flags |= flags;
    if (!strcmp(name, "OPTIND"))
        optpos = 0;
}

static void var_unset(const char *name)
{
    Var *v = var_find(name, 0);
    if (v && !(v->flags & V_RDONLY)) {
        free(v->val);
        v->val = 0;
        v->flags = 0;
    }
}

/* NAME=value, already expanded */
static void var_assign(const char *s, int flags)
{
    const char *eq = strchr(s, '=');
    if (eq) {
        char *name = xstrndup(s, (size_t)(eq - s));
        var_set(name, eq + 1, flags);
        free(name);
    } else
        var_set(s, 0, flags);
}

static char **make_env(void)
{
    char **env = 0;
    int n = 0, i;
    Var *v;
    for (i = 0; i < 256; i++)
        for (v = vars[i]; v; v = v->next)
            if ((v->flags & V_EXPORT) && v->val) {
                char *s = xmalloc(strlen(v->name) + strlen(v->val) + 2);
                sprintf(s, "%s=%s", v->name, v->val);
                push(&env, &n, s);
            }
    if (!env)
        push(&env, &n, 0), n = 0;
    return env;
}

static void local_save(const char *name)
{
    Var *v = var_find(name, 0);
    if (nlocals == caplocals) {
        caplocals = caplocals ? caplocals * 2 : 16;
        locals = xrealloc(locals, (size_t)caplocals * sizeof *locals);
    }
    locals[nlocals].name = xstrdup(name);
    locals[nlocals].val = v && v->val ? xstrdup(v->val) : 0;
    locals[nlocals].flags = v ? v->flags : 0;
    nlocals++;
}

static void local_restore(int mark)
{
    while (nlocals > mark) {
        Local *l = &locals[--nlocals];
        Var *v = var_find(l->name, 1);
        free(v->val);
        v->val = l->val;
        v->flags = l->flags;
        free(l->name);
    }
}

static Func *func_find(const char *name)
{
    Func *f;
    for (f = funcs; f; f = f->next)
        if (!strcmp(f->name, name))
            return f;
    return 0;
}

/* ---- expansion */

typedef struct Exp {
    Buf b, q;     /* the field being built and, per character, whether it was quoted */
    int started;  /* the field exists even if empty */
    int split;
    int lit;      /* in the word of ${x-word}: unquoted blanks separate fields */
    char **f, **fq;
    int nf, nfq;
} Exp;

static void exp_word(Exp *e, const char *s, int mode);

static void ec(Exp *e, int c, int quoted)
{
    bputc(&e->b, c);
    bputc(&e->q, quoted ? 1 : 0);
    e->started = 1;
}

static void fbreak(Exp *e)
{
    if (!e->started)
        return;
    bputc(&e->b, 0);
    bputc(&e->q, 0);
    push(&e->f, &e->nf, e->b.s);
    push(&e->fq, &e->nfq, e->q.s);
    memset(&e->b, 0, sizeof e->b);
    memset(&e->q, 0, sizeof e->q);
    e->started = 0;
}

static const char *ifs(void)
{
    const char *s = var_get("IFS");
    return s ? s : " \t\n";
}

static void emit_val(Exp *e, const char *s, int mode)
{
    const char *sep;
    if (!s)
        return;
    if (mode || !e->split) {
        for (; *s; s++)
            ec(e, *s, mode);
        return;
    }
    sep = ifs();
    for (; *s; s++) {
        if (*sep && strchr(sep, *s)) {
            if (*s == ' ' || *s == '\t' || *s == '\n')
                fbreak(e);
            else {
                e->started = 1;
                fbreak(e);
            }
        } else
            ec(e, *s, 0);
    }
}

static char *cmdsubst(const char *cmd)
{
    int fd[2], pid, st = 0;
    Buf b;
    char tmp[4096];
    long n;
    memset(&b, 0, sizeof b);
    fflush(stdout);
    if (pipe(fd) < 0) {
        sh_err("pipe failed");
        return bstr(&b);
    }
    pid = fork();
    if (pid < 0) {
        sh_err("fork failed");
        return bstr(&b);
    }
    if (pid == 0) {
        close(fd[0]);
        if (fd[1] != 1) {
            dup2(fd[1], 1);
            close(fd[1]);
        }
        trap_exit = 0;
        noerr = 0;
        run_string(cmd);
        sh_exit(status);
    }
    close(fd[1]);
    while ((n = read(fd[0], tmp, sizeof tmp)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            bput(&b, tmp, (size_t)n);
    close(fd[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    substatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    while (b.n && b.s[b.n - 1] == '\n')
        b.n--;
    return bstr(&b);
}

/* arithmetic */
static const char *ap;
static int aerr;
static long a_assign(void);

static void a_space(void)
{
    while (isspace((unsigned char)*ap))
        ap++;
}

static long a_var(const char *name)
{
    const char *v = var_get(name);
    if (!v || !*v)
        return 0;
    return strtol(v, 0, 0);
}

static long a_primary(void)
{
    long v;
    a_space();
    if (*ap == '(') {
        ap++;
        v = a_assign();
        a_space();
        if (*ap == ')')
            ap++;
        else
            aerr = 1;
        return v;
    }
    if (isdigit((unsigned char)*ap)) {
        char *end;
        v = (long)strtoul(ap, &end, 0);
        ap = end;
        return v;
    }
    if (isalpha((unsigned char)*ap) || *ap == '_') {
        const char *s = ap;
        char *name;
        while (isalnum((unsigned char)*ap) || *ap == '_')
            ap++;
        name = xstrndup(s, (size_t)(ap - s));
        v = a_var(name);
        free(name);
        return v;
    }
    aerr = 1;
    return 0;
}

static long a_unary(void)
{
    a_space();
    if (*ap == '-' && ap[1] != '-') {
        ap++;
        return -a_unary();
    }
    if (*ap == '+' && ap[1] != '+') {
        ap++;
        return a_unary();
    }
    if (*ap == '!') {
        ap++;
        return !a_unary();
    }
    if (*ap == '~') {
        ap++;
        return ~a_unary();
    }
    return a_primary();
}

static int a_op(int *len)
{
    static const char *const ops[] = { "||", "&&", "|", "^", "&", "==", "!=", "<=", ">=", "<<", ">>", "<", ">", "+",
                                       "-",  "*",  "/", "%", 0 };
    static const int prec[] = { 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 7, 7, 9, 9, 10, 10, 10 };
    int i;
    a_space();
    for (i = 0; ops[i]; i++) {
        size_t n = strlen(ops[i]);
        if (!strncmp(ap, ops[i], n)) {
            if (n == 1 && ap[1] == '=' && *ap != '<' && *ap != '>')
                return -1; /* an assignment operator */
            if (n == 2 && ap[2] == '=' && (i == 9 || i == 10))
                return -1;
            *len = (int)n;
            return i | prec[i] << 8;
        }
    }
    return -1;
}

static long a_binary(int minprec)
{
    long l = a_unary(), r;
    for (;;) {
        int len = 0, op = a_op(&len), p;
        if (op < 0)
            return l;
        p = op >> 8;
        op &= 255;
        if (p < minprec)
            return l;
        ap += len;
        r = a_binary(p + 1);
        switch (op) {
        case 0: l = l || r; break;
        case 1: l = l && r; break;
        case 2: l |= r; break;
        case 3: l ^= r; break;
        case 4: l &= r; break;
        case 5: l = l == r; break;
        case 6: l = l != r; break;
        case 7: l = l <= r; break;
        case 8: l = l >= r; break;
        case 9: l <<= r; break;
        case 10: l >>= r; break;
        case 11: l = l < r; break;
        case 12: l = l > r; break;
        case 13: l += r; break;
        case 14: l -= r; break;
        case 15: l *= r; break;
        default:
            if (!r) {
                sh_err("division by zero");
                aerr = 1;
                return 0;
            }
            if (op == 16)
                l /= r;
            else
                l %= r;
        }
    }
}

static long a_cond(void)
{
    long c = a_binary(1), a, b;
    a_space();
    if (*ap != '?')
        return c;
    ap++;
    a = a_assign();
    a_space();
    if (*ap == ':')
        ap++;
    else
        aerr = 1;
    b = a_cond();
    return c ? a : b;
}

static long a_assign(void)
{
    const char *save = ap, *s;
    a_space();
    s = ap;
    if (isalpha((unsigned char)*ap) || *ap == '_') {
        const char *e;
        int op = 0;
        while (isalnum((unsigned char)*ap) || *ap == '_')
            ap++;
        e = ap;
        a_space();
        if (*ap == '=' && ap[1] != '=')
            op = '=', ap++;
        else if (*ap && strchr("+-*/%&|^", *ap) && ap[1] == '=')
            op = *ap, ap += 2;
        else if ((*ap == '<' || *ap == '>') && ap[1] == *ap && ap[2] == '=')
            op = *ap, ap += 3;
        if (op) {
            char *name = xstrndup(s, (size_t)(e - s)), num[32];
            long r = a_assign(), l = a_var(name);
            switch (op) {
            case '=': l = r; break;
            case '+': l += r; break;
            case '-': l -= r; break;
            case '*': l *= r; break;
            case '/': l = r ? l / r : 0; break;
            case '%': l = r ? l % r : 0; break;
            case '&': l &= r; break;
            case '|': l |= r; break;
            case '^': l ^= r; break;
            case '<': l <<= r; break;
            default: l >>= r;
            }
            sprintf(num, "%ld", l);
            var_set(name, num, 0);
            free(name);
            return l;
        }
    }
    ap = save;
    return a_cond();
}

static long arith(const char *s)
{
    long v;
    ap = s;
    aerr = 0;
    v = a_assign();
    a_space();
    if (*ap || aerr) {
        sh_err("arithmetic syntax error: \"%s\"", s);
        sh_exit(2);
    }
    return v;
}

static char *joinpos(int sepc)
{
    Buf b;
    int i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < posc; i++) {
        if (i && sepc)
            bputc(&b, sepc);
        bputs(&b, posv[i]);
    }
    return bstr(&b);
}

/* the value of a parameter as a fresh string, or 0 when unset */
static char *getparam(const char *name)
{
    char num[32];
    const char *v;
    if (isdigit((unsigned char)name[0])) {
        int i = atoi(name);
        if (i == 0)
            return xstrdup(arg0);
        return i <= posc ? xstrdup(posv[i - 1]) : 0;
    }
    if (name[1] == 0) {
        switch (name[0]) {
        case '?':
            sprintf(num, "%d", status);
            return xstrdup(num);
        case '#':
            sprintf(num, "%d", posc);
            return xstrdup(num);
        case '$':
            sprintf(num, "%d", shpid);
            return xstrdup(num);
        case '!':
            sprintf(num, "%d", bgpid);
            return bgpid ? xstrdup(num) : 0;
        case '-':
            sprintf(num, "%s%s%s%s", eflag ? "e" : "", fflag ? "f" : "", uflag ? "u" : "", xflag ? "x" : "");
            return xstrdup(num);
        case '@':
        case '*': {
            const char *sep = ifs();
            return posc ? joinpos(name[0] == '*' ? *sep : ' ') : 0;
        }
        }
    }
    if (!strcmp(name, "LINENO")) {
        sprintf(num, "%d", curline);
        return xstrdup(num);
    }
    v = var_get(name);
    return v ? xstrdup(v) : 0;
}

/* the word as a pattern: quoted characters are escaped */
static char *mkpat(const char *s, const char *q, size_t n)
{
    Buf b;
    size_t i;
    memset(&b, 0, sizeof b);
    for (i = 0; i < n; i++) {
        if (q[i] && strchr("*?[]\\!-^", s[i]))
            bputc(&b, '\\');
        bputc(&b, s[i]);
    }
    return bstr(&b);
}

static char *exp_join(Exp *e)
{
    Buf b;
    int i;
    fbreak(e);
    memset(&b, 0, sizeof b);
    for (i = 0; i < e->nf; i++) {
        if (i)
            bputc(&b, ' ');
        bputs(&b, e->f[i]);
    }
    return bstr(&b);
}

static void exp_free(Exp *e)
{
    int i;
    for (i = 0; i < e->nf; i++) {
        free(e->f[i]);
        free(e->fq[i]);
    }
    free(e->f);
    free(e->fq);
    free(e->b.s);
    free(e->q.s);
}

static char *exp_mode(const char *word, int mode)
{
    Exp e;
    char *s;
    memset(&e, 0, sizeof e);
    exp_word(&e, word, mode);
    e.started = 1;
    s = exp_join(&e);
    exp_free(&e);
    return s;
}

static char *exp_str(const char *word) { return exp_mode(word, 0); }

static char *exp_pat(const char *word, int mode)
{
    Exp e;
    char *s;
    memset(&e, 0, sizeof e);
    exp_word(&e, word, mode);
    s = mkpat(e.b.s ? e.b.s : "", e.q.s ? e.q.s : "", e.b.n);
    exp_free(&e);
    return s;
}

/* remove the shortest or longest prefix or suffix matching the pattern */
static char *trim(char *val, const char *pat, int op, int longest)
{
    size_t n = strlen(val), i;
    char save;
    if (op == '#') {
        for (i = 0; i <= n; i++) {
            size_t k = longest ? n - i : i;
            int m;
            save = val[k];
            val[k] = 0;
            m = fnmatch(pat, val, 0);
            val[k] = save;
            if (!m)
                return xstrdup(val + k);
        }
    } else {
        for (i = 0; i <= n; i++) {
            size_t k = longest ? i : n - i;
            if (!fnmatch(pat, val + k, 0))
                return xstrndup(val, k);
        }
    }
    return xstrdup(val);
}

static const char *exp_dollar(Exp *e, const char *p, int mode)
{
    const char *q, *s;
    char *name, *val, *txt;
    char num[32];
    int i;
    if (p[1] == '(') {
        q = scan_paren(p + 2);
        if (p[2] == '(' && q[-1] == ')' && q > p + 4 && q[-2] == ')') {
            /* might be arithmetic: the inner parenthesis must close just before the end */
            const char *in = scan_paren(p + 3);
            if (in == q - 1) {
                char *x;
                txt = xstrndup(p + 3, (size_t)(q - 2 - (p + 3)));
                x = exp_mode(txt, 1);
                sprintf(num, "%ld", arith(x));
                emit_val(e, num, mode);
                free(x);
                free(txt);
                return q;
            }
        }
        txt = inner(p + 2, q, ')');
        val = cmdsubst(txt);
        emit_val(e, val, mode);
        free(val);
        free(txt);
        return q;
    }
    if (p[1] == '{') {
        int colon = 0, op = 0, longest = 0, isnull;
        q = scan_brace(p + 2, mode != 0);
        txt = inner(p + 2, q, '}');
        s = txt;
        if (*s == '#' && s[1] && s[1] != '}' && s[1] != ':' && s[1] != '-' && s[1] != '=' && s[1] != '+'
            && s[1] != '?') {
            val = getparam(s + 1);
            sprintf(num, "%lu", (unsigned long)(val ? strlen(val) : 0));
            emit_val(e, num, mode);
            free(val);
            free(txt);
            return q;
        }
        if (isalpha((unsigned char)*s) || *s == '_') {
            while (isalnum((unsigned char)*s) || *s == '_')
                s++;
        } else if (isdigit((unsigned char)*s)) {
            while (isdigit((unsigned char)*s))
                s++;
        } else if (*s)
            s++;
        name = xstrndup(txt, (size_t)(s - txt));
        if (*s == ':' && s[1] && strchr("-=?+", s[1])) {
            colon = 1;
            s++;
        }
        if (*s) {
            op = *s++;
            if ((op == '#' || op == '%') && *s == op) {
                longest = 1;
                s++;
            }
        }
        if (!op && (name[0] == '@' || name[0] == '*') && !name[1]) {
            exp_dollar(e, name[0] == '@' ? "$@" : "$*", mode);
            free(name);
            free(txt);
            return q;
        }
        val = getparam(name);
        isnull = !val || (colon && !*val);
        switch (op) {
        case 0:
            if (!val && uflag) {
                sh_err("%s: parameter not set", name);
                sh_exit(2);
            }
            emit_val(e, val, mode);
            break;
        case '-':
        case '+':
            if (isnull == (op == '-')) {
                e->lit += mode == 0;
                exp_word(e, s, mode);
                e->lit -= mode == 0;
            } else if (op == '-')
                emit_val(e, val, mode);
            break;
        case '=':
            if (isnull) {
                char *w = exp_mode(s, mode);
                var_set(name, w, 0);
                emit_val(e, w, mode);
                free(w);
            } else
                emit_val(e, val, mode);
            break;
        case '?':
            if (isnull) {
                char *w = exp_mode(s, mode);
                sh_err("%s: %s", name, *w ? w : "parameter not set");
                sh_exit(2);
            }
            emit_val(e, val, mode);
            break;
        case '#':
        case '%':
            if (val) {
                char *pat = exp_pat(s, 0), *r = trim(val, pat, op, longest);
                emit_val(e, r, mode);
                free(r);
                free(pat);
            }
            break;
        default:
            sh_err("bad substitution");
            sh_exit(2);
        }
        free(val);
        free(name);
        free(txt);
        return q;
    }
    if (p[1] == '@' || (p[1] == '*' && !mode)) {
        if (mode) {
            for (i = 0; i < posc; i++) {
                if (i)
                    fbreak(e);
                emit_val(e, posv[i], 1);
                e->started = 1;
            }
        } else {
            for (i = 0; i < posc; i++) {
                if (i && e->split)
                    fbreak(e);
                else if (i)
                    ec(e, ' ', 0);
                emit_val(e, posv[i], 0);
            }
        }
        return p + 2;
    }
    if (isalpha((unsigned char)p[1]) || p[1] == '_') {
        s = p + 1;
        while (isalnum((unsigned char)*s) || *s == '_')
            s++;
    } else if (p[1] && strchr("0123456789?#$!-*", p[1]))
        s = p + 2;
    else {
        ec(e, '$', mode);
        return p + 1;
    }
    name = xstrndup(p + 1, (size_t)(s - p - 1));
    val = getparam(name);
    if (!val && uflag && name[0] != '*') {
        sh_err("%s: parameter not set", name);
        sh_exit(2);
    }
    emit_val(e, val, mode);
    free(val);
    free(name);
    return s;
}

/* mode 0: plain word; 1: inside double quotes; 2: here-document */
static void exp_word(Exp *e, const char *s, int mode)
{
    const char *p = s, *q;
    char *txt, *val;
    while (*p) {
        int c = *p;
        if (c == '\\') {
            if (p[1] == '\n')
                p += 2;
            else if (mode == 0) {
                if (p[1]) {
                    ec(e, p[1], 1);
                    p += 2;
                } else {
                    ec(e, c, 0);
                    p++;
                }
            } else if (p[1] == '$' || p[1] == '`' || p[1] == '\\' || (mode == 1 && p[1] == '"')) {
                ec(e, p[1], 1);
                p += 2;
            } else {
                ec(e, c, 1);
                p++;
            }
        } else if (c == '\'' && mode == 0) {
            q = scan_sq(p + 1);
            for (p++; *p && *p != '\''; p++)
                ec(e, *p, 1);
            e->started = 1;
            p = q;
        } else if (c == '"' && mode == 0) {
            q = scan_dq(p + 1);
            txt = inner(p + 1, q, '"');
            if (strcmp(txt, "$@"))
                e->started = 1;
            exp_word(e, txt, 1);
            free(txt);
            p = q;
        } else if (c == '"' && mode == 1)
            p++;
        else if (c == '`') {
            Buf b;
            memset(&b, 0, sizeof b);
            q = scan_bq(p + 1);
            for (p++; *p && *p != '`'; p++) {
                if (*p == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$' || (mode && p[1] == '"')))
                    p++;
                bputc(&b, *p);
            }
            val = cmdsubst(bstr(&b));
            emit_val(e, val, mode);
            free(val);
            free(b.s);
            p = q;
        } else if (c == '$')
            p = exp_dollar(e, p, mode);
        else if (c == '~' && mode == 0 && p == s && (!p[1] || p[1] == '/')) {
            const char *home = var_get("HOME");
            if (home)
                emit_val(e, home, 1);
            else
                ec(e, c, 0);
            p++;
        } else {
            if (mode == 0 && e->lit && e->split && (c == ' ' || c == '\t' || c == '\n'))
                fbreak(e);
            else
                ec(e, c, mode);
            p++;
        }
    }
}

/* pathname expansion */

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int hasmagic(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n)
            i++;
        else if (s[i] == '*' || s[i] == '?' || s[i] == '[')
            return 1;
    }
    return 0;
}

static void globrec(Buf *path, const char *pat, char ***out, int *nout)
{
    size_t base = path->n, len;
    const char *end;
    char *comp;
    while (*pat == '/') {
        bputc(path, '/');
        pat++;
    }
    base = path->n;
    if (!*pat) {
        struct stat st;
        if (lstat(bstr(path), &st) == 0)
            push(out, nout, xstrdup(path->s));
        return;
    }
    end = pat;
    while (*end && *end != '/') {
        if (*end == '\\' && end[1])
            end++;
        end++;
    }
    len = (size_t)(end - pat);
    if (!hasmagic(pat, len)) {
        size_t i;
        for (i = 0; i < len; i++) {
            if (pat[i] == '\\' && i + 1 < len)
                i++;
            bputc(path, pat[i]);
        }
        globrec(path, end, out, nout);
    } else {
        DIR *d;
        struct dirent *de;
        comp = xstrndup(pat, len);
        d = opendir(base ? bstr(path) : ".");
        if (d) {
            while ((de = readdir(d)) != 0) {
                if (de->d_name[0] == '.' && comp[0] != '.')
                    continue;
                if (fnmatch(comp, de->d_name, 0))
                    continue;
                path->n = base;
                bputs(path, de->d_name);
                if (*end) {
                    struct stat st;
                    if (stat(bstr(path), &st) || !S_ISDIR(st.st_mode))
                        continue;
                }
                globrec(path, end, out, nout);
            }
            closedir(d);
        }
        free(comp);
    }
    path->n = base;
}

static void fields_out(Exp *e, char ***out, int *nout)
{
    int i;
    for (i = 0; i < e->nf; i++) {
        char *s = e->f[i], *q = e->fq[i];
        size_t n = strlen(s), k;
        int magic = 0;
        if (!fflag && e->split)
            for (k = 0; k < n; k++)
                if (!q[k] && (s[k] == '*' || s[k] == '?' || s[k] == '['))
                    magic = 1;
        if (magic) {
            char *pat = mkpat(s, q, n), **m = 0;
            int nm = 0, j;
            Buf path;
            memset(&path, 0, sizeof path);
            globrec(&path, pat, &m, &nm);
            free(path.s);
            free(pat);
            if (nm) {
                qsort(m, (size_t)nm, sizeof *m, cmpstr);
                for (j = 0; j < nm; j++)
                    push(out, nout, m[j]);
                free(m);
                continue;
            }
        }
        push(out, nout, xstrdup(s));
    }
}

/* expand command words into arguments */
static char **exp_words(char **words, int n, int *argc)
{
    char **out = 0;
    int nout = 0, i, decl = 0;
    if (n > 0 && (!strcmp(words[0], "export") || !strcmp(words[0], "readonly") || !strcmp(words[0], "local")))
        decl = 1;
    for (i = 0; i < n; i++) {
        Exp e;
        memset(&e, 0, sizeof e);
        e.split = !(decl && isassign(words[i]));
        exp_word(&e, words[i], 0);
        fbreak(&e);
        fields_out(&e, &out, &nout);
        exp_free(&e);
    }
    if (!out)
        push(&out, &nout, 0), nout = 0;
    *argc = nout;
    return out;
}

static void free_args(char **v)
{
    int i;
    if (!v)
        return;
    for (i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

/* ---- redirections */

typedef struct Saved {
    int fd, copy;
} Saved;
static Saved saved[128];
static int nsaved;

static void save_fd(int fd)
{
    int copy = fcntl(fd, F_DUPFD, 10);
    if (nsaved == 128)
        return;
    if (copy >= 0)
        fcntl(copy, F_SETFD, FD_CLOEXEC);
    saved[nsaved].fd = fd;
    saved[nsaved].copy = copy;
    nsaved++;
}

static void restore_fds(int mark)
{
    while (nsaved > mark) {
        Saved *s = &saved[--nsaved];
        if (s->copy >= 0) {
            dup2(s->copy, s->fd);
            close(s->copy);
        } else
            close(s->fd);
    }
}

static int heredoc_fd(Redir *r)
{
    int fd[2];
    char *text = r->quoted ? r->body : exp_mode(r->body, 2);
    size_t n = strlen(text);
    if (pipe(fd) < 0)
        return -1;
    if (n <= 4096) {
        if (write(fd[1], text, n) < 0)
            n = 0;
    } else {
        fflush(stdout);
        if (fork() == 0) {
            close(fd[0]);
            while (n) {
                long w = write(fd[1], text, n);
                if (w <= 0)
                    break;
                text += w;
                n -= (size_t)w;
            }
            _exit(0);
        }
    }
    close(fd[1]);
    if (!r->quoted && n <= 4096)
        free(text);
    return fd[0];
}

static int redirect(Redir *r, int save)
{
    for (; r; r = r->next) {
        int fd = r->fd, nfd = -1, isdup = 0;
        char *w = r->op == R_HERE ? 0 : exp_str(r->word);
        switch (r->op) {
        case R_IN:
            nfd = open(w, O_RDONLY);
            break;
        case R_OUT:
            nfd = open(w, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            break;
        case R_APP:
            nfd = open(w, O_WRONLY | O_CREAT | O_APPEND, 0666);
            break;
        case R_RW:
            nfd = open(w, O_RDWR | O_CREAT, 0666);
            break;
        case R_HERE:
            nfd = heredoc_fd(r);
            break;
        default:
            isdup = 1;
        }
        if (fd < 0)
            fd = r->op == R_IN || r->op == R_RW || r->op == R_HERE || r->op == R_DUPIN ? 0 : 1;
        if (isdup) {
            if (save)
                save_fd(fd);
            if (!strcmp(w, "-"))
                close(fd);
            else if (!isdigit((unsigned char)*w) || dup2(atoi(w), fd) < 0) {
                sh_err("%s: bad file descriptor", w);
                free(w);
                return -1;
            }
        } else {
            if (nfd < 0) {
                sh_err("cannot open %s: %s", w ? w : "here-document", strerror(errno));
                free(w);
                return -1;
            }
            if (save)
                save_fd(fd);
            if (nfd != fd) {
                dup2(nfd, fd);
                close(nfd);
            }
        }
        free(w);
    }
    return 0;
}

/* ---- running programs */

static int wait_for(int pid)
{
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static char *find_path(const char *name)
{
    const char *path = var_get("PATH");
    return path_search(name, path ? path : "/usr/bin:/bin");
}

/* replace the shell with the program; does not return */
static void exec_program(char **argv)
{
    fflush(stdout);
    prog = arg0 ? arg0 : "sh";
    box_exec(argv, make_env());
}

/* ---- builtins */

static int unwinding(void) { return brkn || contn || retp; }

static int num_arg(char **argv, int def)
{
    return argv[1] ? atoi(argv[1]) : def;
}

static int b_true(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

static int b_false(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

static int b_exit(int argc, char **argv)
{
    (void)argc;
    sh_exit(num_arg(argv, status));
    return 0;
}

static int b_return(int argc, char **argv)
{
    (void)argc;
    retp = 1;
    return num_arg(argv, status);
}

static int b_break(int argc, char **argv)
{
    (void)argc;
    if (loopdepth)
        brkn = num_arg(argv, 1);
    if (brkn > loopdepth)
        brkn = loopdepth;
    return 0;
}

static int b_continue(int argc, char **argv)
{
    (void)argc;
    if (loopdepth)
        contn = num_arg(argv, 1);
    if (contn > loopdepth)
        contn = loopdepth;
    return 0;
}

static int b_cd(int argc, char **argv)
{
    const char *dir;
    char cwd[4096];
    int print = 0;
    while (argc > 1 && argv[1][0] == '-' && argv[1][1]) { /* -L, -P, -- */
        argv++;
        argc--;
    }
    dir = argc > 1 ? argv[1] : var_get("HOME");
    if (dir && !strcmp(dir, "-")) {
        dir = var_get("OLDPWD");
        print = 1;
    }
    if (!dir || chdir(dir) < 0) {
        sh_err("cd: can't cd to %s", dir ? dir : "HOME");
        return 1;
    }
    if (var_get("PWD"))
        var_set("OLDPWD", var_get("PWD"), 0);
    if (getcwd(cwd, sizeof cwd))
        var_set("PWD", cwd, 0);
    if (print)
        puts(cwd);
    return 0;
}

static int b_pwd(int argc, char **argv)
{
    char cwd[4096];
    (void)argc;
    (void)argv;
    if (!getcwd(cwd, sizeof cwd))
        return 1;
    puts(cwd);
    return 0;
}

static void print_quoted(const char *s)
{
    putchar('\'');
    for (; *s; s++) {
        if (*s == '\'')
            fputs("'\\''", stdout);
        else
            putchar(*s);
    }
    putchar('\'');
}

static int declare(int argc, char **argv, int flag)
{
    int i, j;
    Var *v;
    if (argc == 1 || !strcmp(argv[1], "-p")) {
        for (j = 0; j < 256; j++)
            for (v = vars[j]; v; v = v->next)
                if (v->flags & flag) {
                    printf("%s %s", argv[0], v->name);
                    if (v->val) {
                        putchar('=');
                        print_quoted(v->val);
                    }
                    putchar('\n');
                }
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (flag == V_RDONLY) {
            var_assign(argv[i], 0);
            {
                char *name = xstrndup(argv[i], strcspn(argv[i], "="));
                var_find(name, 1)->flags |= V_RDONLY;
                free(name);
            }
        } else
            var_assign(argv[i], flag);
    }
    return 0;
}

static int b_export(int argc, char **argv) { return declare(argc, argv, V_EXPORT); }
static int b_readonly(int argc, char **argv) { return declare(argc, argv, V_RDONLY); }

static int b_local(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        char *name = xstrndup(argv[i], strcspn(argv[i], "="));
        if (funcdepth)
            local_save(name);
        free(name);
        var_assign(argv[i], 0);
    }
    return 0;
}

static int b_unset(int argc, char **argv)
{
    int i, fn = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f"))
            fn = 1;
        else if (!strcmp(argv[i], "-v"))
            fn = 0;
        else if (fn) {
            Func **pp;
            for (pp = &funcs; *pp; pp = &(*pp)->next)
                if (!strcmp((*pp)->name, argv[i])) {
                    *pp = (*pp)->next;
                    break;
                }
        } else
            var_unset(argv[i]);
    }
    return 0;
}

static void set_pos(char **argv, int n)
{
    int i;
    char **v = xmalloc((size_t)(n + 1) * sizeof *v);
    for (i = 0; i < n; i++)
        v[i] = xstrdup(argv[i]);
    v[n] = 0;
    posv = v;
    posc = n;
}

static int set_option(int c, int on)
{
    switch (c) {
    case 'e': eflag = on; break;
    case 'x': xflag = on; break;
    case 'u': uflag = on; break;
    case 'f': fflag = on; break;
    case 'a': case 'b': case 'h': case 'm': case 'C': case 'v': case 'n': break;
    default: return -1;
    }
    return 0;
}

static int b_set(int argc, char **argv)
{
    int i, j;
    Var *v;
    if (argc == 1) {
        for (j = 0; j < 256; j++)
            for (v = vars[j]; v; v = v->next)
                if (v->val) {
                    printf("%s=", v->name);
                    print_quoted(v->val);
                    putchar('\n');
                }
        return 0;
    }
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            set_pos(argv + i, argc - i);
            return 0;
        }
        if ((a[0] != '-' && a[0] != '+') || !a[1])
            break;
        if (a[1] == 'o' && !a[2]) {
            const char *o = argv[i + 1] ? argv[++i] : "";
            int on = a[0] == '-';
            if (!strcmp(o, "errexit"))
                eflag = on;
            else if (!strcmp(o, "xtrace"))
                xflag = on;
            else if (!strcmp(o, "nounset"))
                uflag = on;
            else if (!strcmp(o, "noglob"))
                fflag = on;
            continue;
        }
        for (j = 1; a[j]; j++)
            if (set_option(a[j], a[0] == '-') < 0) {
                sh_err("set: bad option -%c", a[j]);
                return 2;
            }
    }
    if (i < argc)
        set_pos(argv + i, argc - i);
    return 0;
}

static int b_shift(int argc, char **argv)
{
    int n = num_arg(argv, 1);
    (void)argc;
    if (n > posc) {
        sh_err("shift: can't shift that many");
        return 1;
    }
    posv += n;
    posc -= n;
    return 0;
}

static int b_eval(int argc, char **argv)
{
    Buf b;
    int i, st;
    memset(&b, 0, sizeof b);
    if (argc > 1 && !strcmp(argv[1], "--")) {
        argv++;
        argc--;
    }
    for (i = 1; i < argc; i++) {
        if (i > 1)
            bputc(&b, ' ');
        bputs(&b, argv[i]);
    }
    if (argc < 2)
        return 0;
    st = run_string(bstr(&b));
    free(b.s);
    return st;
}

static int b_dot(int argc, char **argv)
{
    char *path, *text;
    int st, savec = posc;
    char **savev = posv;
    if (argc < 2)
        return 0;
    path = find_path(argv[1]);
    if (!path && access(argv[1], R_OK) == 0)
        path = xstrdup(argv[1]);
    text = path ? readfile(path, 0) : 0;
    if (!text) {
        sh_err(".: %s: not found", argv[1]);
        sh_exit(2);
    }
    if (argc > 2)
        set_pos(argv + 2, argc - 2);
    st = run_string(text);
    retp = 0;
    posc = savec;
    posv = savev;
    free(path);
    return st;
}

static int b_read(int argc, char **argv)
{
    int raw = 0, i = 1, eof = 0;
    Buf b;
    char c, *p;
    const char *sep = ifs();
    memset(&b, 0, sizeof b);
    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], 'r'))
            raw = 1;
        i++;
    }
    for (;;) {
        long n = read(0, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            eof = 1;
            break;
        }
        if (c == '\\' && !raw) {
            if (read(0, &c, 1) <= 0)
                break;
            if (c != '\n')
                bputc(&b, c);
            continue;
        }
        if (c == '\n')
            break;
        bputc(&b, c);
    }
    p = bstr(&b);
    if (i >= argc) {
        var_set("REPLY", p, 0);
        free(b.s);
        return eof;
    }
    for (; i < argc; i++) {
        char *e;
        while (*p && strchr(sep, *p) && isspace((unsigned char)*p))
            p++;
        if (i == argc - 1) {
            e = p + strlen(p);
            while (e > p && strchr(sep, e[-1]) && isspace((unsigned char)e[-1]))
                e--;
            *e = 0;
            var_set(argv[i], p, 0);
            break;
        }
        e = p;
        while (*e && !strchr(sep, *e))
            e++;
        if (*e)
            *e++ = 0;
        var_set(argv[i], p, 0);
        p = e;
    }
    free(b.s);
    return eof;
}

static int b_trap(int argc, char **argv)
{
    int i = 1;
    const char *action;
    if (argc < 2)
        return 0;
    if (!strcmp(argv[1], "--"))
        i++;
    if (i >= argc)
        return 0;
    action = argv[i++];
    if (i == argc) { /* trap SIGNAL: reset */
        i--;
        action = "-";
    }
    for (; i < argc; i++)
        if (!strcmp(argv[i], "EXIT") || !strcmp(argv[i], "0")) {
            free(trap_exit);
            trap_exit = strcmp(action, "-") && *action ? xstrdup(action) : 0;
        }
    return 0;
}

static int b_wait(int argc, char **argv)
{
    int st = 0, i;
    if (argc < 2) {
        while (waitpid(-1, &st, 0) > 0 || errno == EINTR)
            ;
        return 0;
    }
    for (i = 1; i < argc; i++)
        st = wait_for(atoi(argv[i]));
    return st;
}

static int b_umask(int argc, char **argv)
{
    if (argc < 2) {
        int m = (int)umask(0);
        umask((mode_t)m);
        printf("%04o\n", m);
    } else
        umask((mode_t)strtol(argv[1], 0, 8));
    return 0;
}

static int b_getopts(int argc, char **argv)
{
    const char *opts, *v;
    char **args, buf[32], opt[2];
    int nargs, ind, c;
    const char *spec;
    if (argc < 3)
        return 2;
    opts = argv[1];
    if (argc > 3) {
        args = argv + 3;
        nargs = argc - 3;
    } else {
        args = posv;
        nargs = posc;
    }
    v = var_get("OPTIND");
    ind = v ? atoi(v) : 1;
    if (ind < 1)
        ind = 1;
    opt[1] = 0;
    if (!optpos) {
        if (ind > nargs || args[ind - 1][0] != '-' || !args[ind - 1][1]) {
            var_set(argv[2], "?", 0);
            return 1;
        }
        if (!strcmp(args[ind - 1], "--")) {
            sprintf(buf, "%d", ind + 1);
            var_set("OPTIND", buf, 0);
            var_set(argv[2], "?", 0);
            return 1;
        }
        optpos = 1;
    }
    c = args[ind - 1][optpos++];
    spec = c == ':' ? 0 : strchr(opts, c);
    opt[0] = (char)c;
    var_unset("OPTARG");
    if (!spec) {
        if (*opts == ':')
            var_set("OPTARG", opt, 0);
        else
            sh_err("illegal option -- %c", c);
        opt[0] = '?';
    } else if (spec[1] == ':') {
        if (args[ind - 1][optpos]) {
            var_set("OPTARG", args[ind - 1] + optpos, 0);
        } else if (ind < nargs) {
            var_set("OPTARG", args[ind], 0);
            ind++;
        } else {
            if (*opts == ':') {
                var_set("OPTARG", opt, 0);
                opt[0] = ':';
            } else {
                sh_err("option requires an argument -- %c", c);
                opt[0] = '?';
            }
        }
        ind++;
        optpos = 0;
    }
    if (optpos && !args[ind - 1][optpos]) {
        ind++;
        optpos = 0;
    }
    c = optpos;
    sprintf(buf, "%d", ind);
    var_set("OPTIND", buf, 0);
    optpos = c;
    var_set(argv[2], opt, 0);
    return 0;
}

static int b_kill(int argc, char **argv)
{
    static const char *const names[] = { "0", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "EMT", "FPE", "KILL",
                                         "BUS", "SEGV", "SYS", "PIPE", "ALRM", "TERM", 0 };
    int i = 1, sig = 15, st = 0, k;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        const char *s = argv[1] + 1;
        if (!strcmp(s, "s") && argc > 2) {
            s = argv[2];
            i++;
        }
        i++;
        if (!strncmp(s, "SIG", 3))
            s += 3;
        if (isdigit((unsigned char)*s))
            sig = atoi(s);
        else {
            for (k = 0; names[k] && strcmp(names[k], s); k++)
                ;
            if (!names[k]) {
                sh_err("kill: unknown signal %s", s);
                return 2;
            }
            sig = k;
        }
    }
    for (; i < argc; i++)
        if (kill(atoi(argv[i]), sig) < 0) {
            sh_err("kill: %s: %s", argv[i], strerror(errno));
            st = 1;
        }
    return st;
}

typedef struct Builtin {
    const char *name;
    int (*fn)(int, char **);
    int special;
} Builtin;

static int b_command(int argc, char **argv);
static int b_exec(int argc, char **argv);

static const Builtin builtins[] = {
    { ":", b_true, 1 },          { "true", b_true, 0 },     { "false", b_false, 0 },     { "exit", b_exit, 1 },
    { "return", b_return, 1 },   { "break", b_break, 1 },   { "continue", b_continue, 1 }, { "cd", b_cd, 0 },
    { "pwd", b_pwd, 0 },         { "export", b_export, 1 }, { "readonly", b_readonly, 1 }, { "local", b_local, 1 },
    { "unset", b_unset, 1 },     { "set", b_set, 1 },       { "shift", b_shift, 1 },     { "eval", b_eval, 1 },
    { ".", b_dot, 1 },           { "read", b_read, 0 },     { "trap", b_trap, 1 },       { "wait", b_wait, 0 },
    { "umask", b_umask, 0 },     { "getopts", b_getopts, 0 }, { "echo", echo_main, 0 },  { "printf", printf_main, 0 },
    { "test", test_main, 0 },    { "[", test_main, 0 },     { "command", b_command, 0 }, { "type", b_command, 0 },
    { "exec", b_exec, 1 },       { "hash", b_true, 0 },     { "ulimit", b_true, 0 },     { "kill", b_kill, 0 },     { 0, 0, 0 }
};

static const Builtin *find_builtin(const char *name)
{
    const Builtin *b;
    for (b = builtins; b->name; b++)
        if (!strcmp(b->name, name))
            return b;
    return 0;
}

static int b_command(int argc, char **argv)
{
    int i = 1, st = 0, type = !strcmp(argv[0], "type");
    if (!type) {
        if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-V")))
            i = 2;
        else
            return 0;
    }
    for (; i < argc; i++) {
        char *path = 0;
        const char *what = 0;
        if (func_find(argv[i]))
            what = "a shell function";
        else if (find_builtin(argv[i]))
            what = "a shell builtin";
        else if (!strchr(argv[i], '/') && find_applet(argv[i]))
            what = "a built-in tool";
        else
            path = find_path(argv[i]);
        if (path && access(path, X_OK) != 0) {
            free(path);
            path = 0;
        }
        if (type) {
            if (what)
                printf("%s is %s\n", argv[i], what);
            else if (path)
                printf("%s is %s\n", argv[i], path);
            else
                sh_err("%s: not found", argv[i]);
        } else if (what || path)
            puts(path ? path : argv[i]);
        if (!what && !path)
            st = 1;
        free(path);
    }
    return st;
}

static int b_exec(int argc, char **argv)
{
    if (argc > 1)
        exec_program(argv + 1);
    return 0;
}

/* ---- execution */

static void sh_exit(int st)
{
    if (trap_exit) {
        char *t = trap_exit;
        trap_exit = 0;
        status = st;
        retp = brkn = contn = 0;
        run_string(t);
    }
    fflush(stdout);
    exit(st);
}

static void trace(char **assigns, int na, char **argv)
{
    int i;
    const char *ps4 = var_get("PS4");
    fflush(stdout);
    fputs(ps4 ? ps4 : "+ ", stderr);
    for (i = 0; i < na; i++)
        fprintf(stderr, "%s ", assigns[i]);
    for (i = 0; argv[i]; i++)
        fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
    fputc('\n', stderr);
}

static int call_func(Func *f, int argc, char **argv)
{
    char **savev = posv;
    int savec = posc, mark = nlocals, saveloop = loopdepth, st;
    set_pos(argv + 1, argc - 1);
    funcdepth++;
    loopdepth = 0;
    st = run(f->body, 0);
    retp = 0;
    loopdepth = saveloop;
    funcdepth--;
    local_restore(mark);
    posv = savev;
    posc = savec;
    return st;
}

static int run_cmd(Node *n, int flags)
{
    int argc, i, st, mark, pid;
    char **argv, **av;
    Func *f = 0;
    const Builtin *b = 0;
    curline = n->line;
    substatus = 0;
    argv = exp_words(n->words, n->nwords, &argc);
    if (xflag)
        trace(n->assigns, n->nassigns, argv);
    if (!argc) {
        for (i = 0; i < n->nassigns; i++) {
            char *s = exp_str(n->assigns[i]);
            var_assign(s, 0);
            free(s);
        }
        mark = nsaved;
        st = redirect(n->redirs, 1) < 0 ? 1 : substatus;
        restore_fds(mark);
        free_args(argv);
        return st;
    }
    av = argv;
    if (!strcmp(av[0], "command") && argc > 1 && av[1][0] != '-') {
        av++;
        argc--;
    } else
        f = func_find(av[0]);
    if (!f)
        b = find_builtin(av[0]);
    if (f || b) {
        int lmark = nlocals, isexec = b && b->fn == b_exec;
        mark = nsaved;
        if (redirect(n->redirs, !(isexec && argc == 1)) < 0) {
            restore_fds(mark);
            free_args(argv);
            return 1;
        }
        for (i = 0; i < n->nassigns; i++) {
            char *s = exp_str(n->assigns[i]);
            if (!(b && b->special)) {
                char *name = xstrndup(s, strcspn(s, "="));
                local_save(name);
                free(name);
                var_assign(s, V_EXPORT);
            } else
                var_assign(s, isexec ? V_EXPORT : 0);
            free(s);
        }
        st = b ? b->fn(argc, av) : call_func(f, argc, av);
        fflush(stdout);
        if (!(b && b->special))
            local_restore(lmark);
        restore_fds(mark);
        free_args(argv);
        return st;
    }
    fflush(stdout);
    if (!(flags & F_LAST) || trap_exit) {
        pid = fork();
        if (pid < 0) {
            sh_err("fork failed");
            return 126;
        }
        if (pid) {
            free_args(argv);
            return wait_for(pid);
        }
    }
    if (redirect(n->redirs, 0) < 0)
        exit(1);
    for (i = 0; i < n->nassigns; i++) {
        char *s = exp_str(n->assigns[i]);
        var_assign(s, V_EXPORT);
    }
    exec_program(av);
    return 127;
}

static int run_pipe(Node *n)
{
    int count = 0, i, in = -1, fd[2], st = 0, *pids;
    Node *c;
    for (c = n->a; c; c = c->next)
        count++;
    pids = xmalloc((size_t)count * sizeof *pids);
    fflush(stdout);
    for (c = n->a, i = 0; c; c = c->next, i++) {
        fd[0] = fd[1] = -1;
        if (c->next && pipe(fd) < 0) {
            sh_err("pipe failed");
            break;
        }
        pids[i] = fork();
        if (pids[i] < 0) {
            sh_err("fork failed");
            break;
        }
        if (pids[i] == 0) {
            if (in >= 0) {
                dup2(in, 0);
                close(in);
            }
            if (fd[1] >= 0) {
                close(fd[0]);
                dup2(fd[1], 1);
                close(fd[1]);
            }
            trap_exit = 0;
            st = run(c, F_LAST);
            sh_exit(st);
        }
        if (in >= 0)
            close(in);
        if (fd[1] >= 0)
            close(fd[1]);
        in = fd[0];
    }
    count = i;
    for (i = 0; i < count; i++)
        st = wait_for(pids[i]);
    free(pids);
    return st;
}

static int run_loop_body(Node *body, int *st)
{
    *st = run(body, 0);
    if (retp)
        return 1;
    if (brkn) {
        brkn--;
        return 1;
    }
    if (contn && --contn)
        return 1;
    return 0;
}

static int run_compound(Node *n, int flags)
{
    int st = 0, c, i;
    switch (n->type) {
    case N_GROUP:
        return run(n->a, flags);
    case N_IF:
        noerr++;
        c = run(n->a, 0);
        noerr--;
        if (unwinding())
            return c;
        if (!c)
            return run(n->b, flags);
        return n->c ? run(n->c, flags) : 0;
    case N_WHILE:
    case N_UNTIL:
        loopdepth++;
        for (;;) {
            noerr++;
            c = run(n->a, 0);
            noerr--;
            if (unwinding()) {
                if (brkn)
                    brkn--;
                else if (contn)
                    contn--;
                break;
            }
            if ((c == 0) != (n->type == N_WHILE))
                break;
            if (run_loop_body(n->b, &st))
                break;
        }
        loopdepth--;
        return st;
    case N_FOR: {
        char **list;
        int count;
        if (n->nwords < 0) {
            list = posv;
            count = posc;
        } else
            list = exp_words(n->words, n->nwords, &count);
        loopdepth++;
        for (i = 0; i < count; i++) {
            var_set(n->name, list[i], 0);
            if (run_loop_body(n->b, &st))
                break;
        }
        loopdepth--;
        if (n->nwords >= 0)
            free_args(list);
        return st;
    }
    case N_CASE: {
        char *w = exp_str(n->name);
        Case *cs;
        for (cs = n->cases; cs; cs = cs->next)
            for (i = 0; i < cs->npats; i++) {
                char *pat = exp_pat(cs->pats[i], 0);
                int m = fnmatch(pat, w, 0);
                free(pat);
                if (!m) {
                    free(w);
                    return cs->body ? run(cs->body, flags) : 0;
                }
            }
        free(w);
        return 0;
    }
    }
    return 0;
}

static int run(Node *n, int flags)
{
    int st = 0, mark, pid;
    Func *f;
    if (!n)
        return 0;
    switch (n->type) {
    case N_CMD:
        st = run_cmd(n, flags);
        break;
    case N_PIPE:
        st = run_pipe(n);
        break;
    case N_SUB:
        fflush(stdout);
        if (!(flags & F_LAST) || trap_exit) {
            pid = fork();
            if (pid < 0) {
                sh_err("fork failed");
                return 126;
            }
            if (pid) {
                st = wait_for(pid);
                break;
            }
            trap_exit = 0;
        }
        if (redirect(n->redirs, 0) < 0)
            exit(1);
        st = run(n->a, F_LAST);
        sh_exit(st);
        break;
    case N_NOT:
        noerr++;
        st = !run(n->a, 0);
        noerr--;
        status = st;
        return st;
    case N_AND:
    case N_OR:
        noerr++;
        st = run(n->a, 0);
        noerr--;
        if (!unwinding() && (st == 0) == (n->type == N_AND))
            st = run(n->b, flags);
        return st;
    case N_SEQ:
        st = run(n->a, 0);
        if (!unwinding())
            st = run(n->b, flags);
        return st;
    case N_BG:
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd > 0) {
                dup2(fd, 0);
                close(fd);
            }
            trap_exit = 0;
            st = run(n->a, F_LAST);
            sh_exit(st);
        }
        bgpid = pid;
        status = 0;
        return 0;
    case N_FUNC:
        f = func_find(n->name);
        if (!f) {
            f = xmalloc(sizeof *f);
            f->name = n->name;
            f->next = funcs;
            funcs = f;
        }
        f->body = n->a;
        status = 0;
        return 0;
    default:
        mark = nsaved;
        if (redirect(n->redirs, 1) < 0)
            st = 1;
        else
            st = run_compound(n, n->redirs ? 0 : flags);
        restore_fds(mark);
        status = st;
        return st;
    }
    status = st;
    if (st && eflag && !noerr && !unwinding())
        sh_exit(st);
    return st;
}

static int run_string(const char *s)
{
    Parser ps, *save = P;
    Node *n;
    memset(&ps, 0, sizeof ps);
    ps.p = s;
    ps.line = 1;
    P = &ps;
    lex();
    for (;;) {
        n = parse_list(1);
        if (!n)
            break;
        P = save;
        run(n, 0);
        P = &ps;
        if (unwinding())
            break;
    }
    P = save;
    return status;
}

/* ---- echo, printf, test: builtins that are also commands */

static int escape(const char **pp, int *stop)
{
    const char *p = *pp;
    int c = *p++, v, n;
    switch (c) {
    case 'a': c = 7; break;
    case 'b': c = 8; break;
    case 'f': c = 12; break;
    case 'n': c = 10; break;
    case 'r': c = 13; break;
    case 't': c = 9; break;
    case 'v': c = 11; break;
    case 'e': c = 27; break;
    case '\\': break;
    case 'c':
        if (stop) {
            *stop = 1;
            break;
        }
        p--;
        c = '\\';
        break;
    case 'x':
        v = 0;
        for (n = 0; n < 2 && isxdigit((unsigned char)*p); n++, p++)
            v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
        if (!n) {
            p--;
            c = '\\';
        } else
            c = v;
        break;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        v = 0;
        p--;
        if (stop && *p == '0')
            p++; /* echo and %b write \0NNN */
        for (n = 0; n < 3 && *p >= '0' && *p <= '7'; n++, p++)
            v = v * 8 + (*p - '0');
        c = v & 255;
        break;
    default:
        p--;
        c = '\\';
    }
    *pp = p;
    return c;
}

int echo_main(int argc, char **argv)
{
    int i = 1, nl = 1, stop = 0;
    if (argc > 1 && !strcmp(argv[1], "-n")) {
        nl = 0;
        i++;
    }
    for (; i < argc && !stop; i++) {
        const char *p = argv[i];
        while (*p && !stop) {
            if (*p == '\\' && p[1]) {
                int c;
                p++;
                c = escape(&p, &stop);
                if (!stop)
                    putchar(c);
            } else
                putchar(*p++);
        }
        if (i + 1 < argc && !stop)
            putchar(' ');
    }
    if (nl && !stop)
        putchar('\n');
    return 0;
}

int printf_main(int argc, char **argv)
{
    const char *fmt, *p;
    int ai = 2, used, st = 0, stop = 0;
    if (argc > 1 && !strcmp(argv[1], "--")) {
        argv++;
        argc--;
    }
    if (argc < 2)
        return 1;
    fmt = argv[1];
    do {
        used = 0;
        for (p = fmt; *p && !stop;) {
            if (*p == '\\') {
                p++;
                putchar(escape(&p, 0));
            } else if (*p != '%')
                putchar(*p++);
            else if (p[1] == '%') {
                putchar('%');
                p += 2;
            } else {
                char spec[64], *end;
                const char *arg, *s = p;
                size_t n;
                int conv;
                p++;
                while (*p && strchr("-+ #0", *p))
                    p++;
                while (isdigit((unsigned char)*p) || *p == '.')
                    p++;
                conv = *p;
                if (!conv)
                    break;
                p++;
                n = (size_t)(p - s);
                if (n > sizeof spec - 4)
                    n = sizeof spec - 4;
                memcpy(spec, s, n);
                spec[n] = 0;
                arg = ai < argc ? argv[ai] : 0;
                if (ai < argc) {
                    ai++;
                    used = 1;
                }
                switch (conv) {
                case 's':
                    printf(spec, arg ? arg : "");
                    break;
                case 'b': {
                    const char *q = arg ? arg : "";
                    while (*q && !stop) {
                        if (*q == '\\' && q[1]) {
                            int c;
                            q++;
                            c = escape(&q, &stop);
                            if (!stop)
                                putchar(c);
                        } else
                            putchar(*q++);
                    }
                    break;
                }
                case 'c':
                    if (arg && *arg)
                        putchar(*arg);
                    break;
                case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
                    long v = 0;
                    if (arg && (*arg == '"' || *arg == '\''))
                        v = (unsigned char)arg[1];
                    else if (arg && *arg) {
                        v = strtol(arg, &end, 0);
                        if (*end) {
                            fflush(stdout);
                            fprintf(stderr, "printf: %s: not a number\n", arg);
                            st = 1;
                        }
                    }
                    spec[n - 1] = 'l';
                    spec[n] = (char)(conv == 'i' ? 'd' : conv);
                    spec[n + 1] = 0;
                    printf(spec, v);
                    break;
                }
                case 'e': case 'f': case 'g': case 'E': case 'G':
                    printf(spec, arg ? strtod(arg, 0) : 0.0);
                    break;
                default:
                    fputs(spec, stdout);
                }
            }
        }
    } while (used && ai < argc && !stop);
    return st;
}

static char **tv;
static int tn, ti, terr;
static int t_or(void);

static int t_unary(int op, const char *a)
{
    struct stat st;
    switch (op) {
    case 'n': return *a != 0;
    case 'z': return *a == 0;
    case 't': return isatty(atoi(a));
    case 'r': return access(a, R_OK) == 0;
    case 'w': return access(a, W_OK) == 0;
    case 'x': return access(a, X_OK) == 0;
    case 'L': case 'h': return lstat(a, &st) == 0 && S_ISLNK(st.st_mode);
    }
    if (stat(a, &st) < 0)
        return 0;
    switch (op) {
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 's': return st.st_size > 0;
    case 'p': return S_ISFIFO(st.st_mode);
    case 'b': return S_ISBLK(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'S': return S_ISSOCK(st.st_mode);
    case 'u': return (st.st_mode & S_ISUID) != 0;
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'k': return (st.st_mode & 01000) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'G': return st.st_gid == getegid();
    }
    terr = 1;
    return 0;
}

static int t_binop(const char *op)
{
    static const char *const ops[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef",
                                       "<", ">",  0 };
    int i;
    for (i = 0; ops[i]; i++)
        if (!strcmp(op, ops[i]))
            return i;
    return -1;
}

static int t_binary(const char *a, int op, const char *b)
{
    long x, y;
    struct stat s1, s2;
    int r1, r2;
    switch (op) {
    case 0: case 1: return !strcmp(a, b);
    case 2: return strcmp(a, b) != 0;
    case 12: return strcmp(a, b) < 0;
    case 13: return strcmp(a, b) > 0;
    case 9: case 10: case 11:
        r1 = stat(a, &s1);
        r2 = stat(b, &s2);
        if (op == 11)
            return !r1 && !r2 && s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino;
        if (r1 || r2)
            return op == 9 ? !r1 && r2 : r1 && !r2;
        if (s1.st_mtime != s2.st_mtime)
            return op == 9 ? s1.st_mtime > s2.st_mtime : s1.st_mtime < s2.st_mtime;
        return 0;
    }
    x = strtol(a, 0, 10);
    y = strtol(b, 0, 10);
    switch (op) {
    case 3: return x == y;
    case 4: return x != y;
    case 5: return x < y;
    case 6: return x <= y;
    case 7: return x > y;
    default: return x >= y;
    }
}

static int t_primary(void)
{
    const char *a;
    int op, r;
    if (ti >= tn) {
        terr = 1;
        return 0;
    }
    a = tv[ti];
    if (ti + 2 < tn + 0 && (op = t_binop(tv[ti + 1])) >= 0) {
        ti += 3;
        return t_binary(a, op, tv[ti - 1]);
    }
    if (!strcmp(a, "!") && ti + 1 < tn) {
        ti++;
        return !t_primary();
    }
    if (!strcmp(a, "(") && ti + 1 < tn) {
        ti++;
        r = t_or();
        if (ti < tn && !strcmp(tv[ti], ")"))
            ti++;
        else
            terr = 1;
        return r;
    }
    if (a[0] == '-' && a[1] && !a[2] && ti + 1 < tn) {
        ti += 2;
        return t_unary(a[1], tv[ti - 1]);
    }
    ti++;
    return *a != 0;
}

static int t_and(void)
{
    int r = t_primary();
    while (ti < tn && !strcmp(tv[ti], "-a")) {
        ti++;
        r = t_primary() && r;
    }
    return r;
}

static int t_or(void)
{
    int r = t_and();
    while (ti < tn && !strcmp(tv[ti], "-o")) {
        ti++;
        r = t_and() || r;
    }
    return r;
}

int test_main(int argc, char **argv)
{
    int r;
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]")) {
            fflush(stdout);
            fprintf(stderr, "[: missing ]\n");
            return 2;
        }
        argc--;
    }
    tv = argv + 1;
    tn = argc - 1;
    ti = 0;
    terr = 0;
    if (tn == 0)
        return 1;
    r = t_or();
    if (terr || ti < tn) {
        fflush(stdout);
        fprintf(stderr, "test: bad expression\n");
        return 2;
    }
    return !r;
}

/* ---- entry */

int sh_main(int argc, char **argv)
{
    int i, st;
    char **e, cwd[4096], num[32];
    const char *cmd = 0;
    char *text;
    shpid = (int)getpid();
    arg0 = argv[0];
    for (e = environ; e && *e; e++)
        if (isassign(*e))
            var_assign(*e, V_EXPORT);
    var_set("IFS", " \t\n", 0);
    var_set("OPTIND", "1", 0);
    sprintf(num, "%d", (int)getppid());
    var_set("PPID", num, 0);
    if (getcwd(cwd, sizeof cwd))
        var_set("PWD", cwd, V_EXPORT);
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        int j;
        if (a[0] != '-' && a[0] != '+')
            break;
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "-"))
            continue;
        if (!strcmp(a, "-o") || !strcmp(a, "+o")) {
            i++;
            continue;
        }
        for (j = 1; a[j]; j++) {
            if (a[j] == 'c') {
                if (i + 1 < argc)
                    cmd = argv[++i];
                else
                    die("-c needs a command");
                if (a[j + 1])
                    die("-c must come last");
                break;
            }
            if (a[j] == 's' || a[j] == 'i' || a[j] == 'l')
                continue;
            if (set_option(a[j], a[0] == '-') < 0)
                die("bad option -%c", a[j]);
        }
    }
    if (cmd) {
        if (i < argc)
            arg0 = argv[i++];
        set_pos(argv + i, argc - i);
        text = xstrdup(cmd);
    } else if (i < argc) {
        arg0 = argv[i];
        text = readfile(argv[i], 0);
        if (!text) {
            fprintf(stderr, "sh: can't open %s\n", argv[i]);
            return 127;
        }
        i++;
        set_pos(argv + i, argc - i);
    } else {
        set_pos(argv + i, 0);
        text = readall(0, 0);
    }
    st = run_string(text);
    retp = 0;
    sh_exit(st);
    return st;
}
