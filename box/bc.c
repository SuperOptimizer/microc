/* bc: integer arithmetic only (scale is accepted and ignored). The program text is
   interpreted directly; loops and function calls re-read their source. */
#include "box.h"

typedef struct BVar {
    struct BVar *next;
    char *name;
    long val;
    const char *body, *params; /* function */
} BVar;

static BVar *bvars;
static const char *bp;
static int bskip;      /* parse without effect */
static int breturn, bbreak;
static long bretval;
static long obase = 10;
static int bassigned;  /* the last expression was an assignment */
static FILE *bin;

static void b_stmt(void);
static long b_expr(void);

static BVar *b_var(const char *name, size_t n)
{
    BVar *v;
    for (v = bvars; v; v = v->next)
        if (strlen(v->name) == n && !memcmp(v->name, name, n))
            return v;
    v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->name = xstrndup(name, n);
    v->next = bvars;
    bvars = v;
    return v;
}

static void b_space(int newlines)
{
    for (;;) {
        if (*bp == ' ' || *bp == '\t' || *bp == '\r' || (newlines && *bp == '\n'))
            bp++;
        else if (*bp == '\\' && bp[1] == '\n')
            bp += 2;
        else if (bp[0] == '/' && bp[1] == '*') {
            const char *e = strstr(bp + 2, "*/");
            bp = e ? e + 2 : bp + strlen(bp);
        } else if (*bp == '#') {
            while (*bp && *bp != '\n')
                bp++;
        } else
            break;
    }
}

static void b_fail(const char *msg)
{
    fflush(stdout);
    fprintf(stderr, "bc: %s near \"%.20s\"\n", msg, bp);
    exit(1);
}

static void b_print(long v)
{
    if (obase == 16)
        printf(v < 0 ? "-%lX" : "%lX", v < 0 ? -v : v);
    else if (obase == 8)
        printf(v < 0 ? "-%lo" : "%lo", v < 0 ? -v : v);
    else if (obase == 2) {
        char buf[72];
        int n = 0;
        unsigned long u = (unsigned long)(v < 0 ? -v : v);
        do
            buf[n++] = (char)('0' + (u & 1));
        while ((u >>= 1) != 0);
        if (v < 0)
            putchar('-');
        while (n)
            putchar(buf[--n]);
    } else
        printf("%ld", v);
}

static int b_word(const char *w)
{
    size_t n = strlen(w);
    b_space(0);
    if (!strncmp(bp, w, n) && !isalnum((unsigned char)bp[n]) && bp[n] != '_') {
        bp += n;
        return 1;
    }
    return 0;
}

static long b_call(BVar *f)
{
    long args[16], saved[32], r;
    BVar *loc[32];
    int n = 0, nloc = 0, i, saveskip = bskip;
    const char *ret, *p;
    b_space(0);
    if (*bp != ')')
        for (;;) {
            if (n == 16)
                b_fail("too many arguments");
            args[n++] = b_expr();
            b_space(0);
            if (*bp != ',')
                break;
            bp++;
        }
    if (*bp != ')')
        b_fail("expected )");
    bp++;
    if (bskip)
        return 0;
    if (!f->body)
        b_fail("undefined function");
    for (p = f->params, i = 0; *p && *p != ')';) {
        const char *s;
        while (*p == ' ' || *p == ',')
            p++;
        s = p;
        while (isalnum((unsigned char)*p) || *p == '_')
            p++;
        if (p == s)
            break;
        loc[nloc] = b_var(s, (size_t)(p - s));
        saved[nloc] = loc[nloc]->val;
        nloc++;
        i++;
    }
    for (i = 0; i < nloc; i++)
        loc[i]->val = i < n ? args[i] : 0;
    ret = bp;
    bp = f->body;
    b_space(1);
    if (b_word("auto")) {
        for (;;) {
            const char *s;
            b_space(0);
            s = bp;
            while (isalnum((unsigned char)*bp) || *bp == '_')
                bp++;
            if (nloc < 32 && bp > s) {
                loc[nloc] = b_var(s, (size_t)(bp - s));
                saved[nloc] = loc[nloc]->val;
                loc[nloc]->val = 0;
                nloc++;
            }
            b_space(0);
            if (*bp != ',')
                break;
            bp++;
        }
    }
    breturn = 0;
    bretval = 0;
    for (;;) {
        b_space(1);
        if (*bp == ';') {
            bp++;
            continue;
        }
        if (!*bp || *bp == '}' || breturn)
            break;
        b_stmt();
    }
    r = bretval;
    breturn = 0;
    bskip = saveskip;
    for (i = nloc - 1; i >= 0; i--)
        loc[i]->val = saved[i];
    bp = ret;
    return r;
}

static long b_primary(void)
{
    long v;
    b_space(0);
    if (*bp == '(') {
        bp++;
        v = b_expr();
        b_space(0);
        if (*bp != ')')
            b_fail("expected )");
        bp++;
        bassigned = 0;
        return v;
    }
    if (isdigit((unsigned char)*bp) || (*bp >= 'A' && *bp <= 'F')) {
        v = 0;
        while (isdigit((unsigned char)*bp) || (*bp >= 'A' && *bp <= 'F')) {
            v = v * 10 + (isdigit((unsigned char)*bp) ? *bp - '0' : *bp - 'A' + 10);
            bp++;
        }
        if (*bp == '.') {
            bp++;
            while (isdigit((unsigned char)*bp))
                bp++;
        }
        return v;
    }
    if (isalpha((unsigned char)*bp) || *bp == '_') {
        const char *s = bp;
        BVar *var;
        int op = 0;
        while (isalnum((unsigned char)*bp) || *bp == '_')
            bp++;
        var = b_var(s, (size_t)(bp - s));
        b_space(0);
        if (*bp == '(') {
            bp++;
            if (!strcmp(var->name, "read")) {
                Buf b;
                b_space(0);
                if (*bp == ')')
                    bp++;
                memset(&b, 0, sizeof b);
                if (bskip || !readline(bin ? bin : stdin, &b))
                    return 0;
                return atol(b.s);
            }
            return b_call(var);
        }
        if (*bp == '=' && bp[1] != '=')
            op = '=', bp++;
        else if (*bp && strchr("+-*/%^", *bp) && bp[1] == '=')
            op = *bp, bp += 2;
        else if ((*bp == '+' || *bp == '-') && bp[1] == *bp) {
            v = var->val;
            if (!bskip)
                var->val += *bp == '+' ? 1 : -1;
            bp += 2;
            bassigned = 1;
            return v;
        }
        if (op) {
            long r = b_expr(), l = var->val, k;
            switch (op) {
            case '=': l = r; break;
            case '+': l += r; break;
            case '-': l -= r; break;
            case '*': l *= r; break;
            case '/': l = r ? l / r : 0; break;
            case '%': l = r ? l % r : 0; break;
            default:
                for (k = 1; r > 0; r--)
                    k *= l;
                l = k;
            }
            if (!bskip) {
                var->val = l;
                if (!strcmp(var->name, "obase"))
                    obase = l;
            }
            bassigned = 1;
            return l;
        }
        return var->val;
    }
    b_fail("syntax error");
    return 0;
}

static long b_unary(void)
{
    b_space(0);
    if (*bp == '-') {
        bp++;
        return -b_unary();
    }
    if (*bp == '!') {
        bp++;
        return !b_unary();
    }
    return b_primary();
}

static long b_pow(void)
{
    long l = b_unary(), r, k;
    b_space(0);
    if (*bp == '^' && bp[1] != '=') {
        bp++;
        r = b_pow();
        for (k = 1; r > 0; r--)
            k *= l;
        bassigned = 0;
        return k;
    }
    return l;
}

static long b_binary(int level)
{
    long l, r;
    if (level == 5)
        return b_pow();
    l = b_binary(level + 1);
    for (;;) {
        int op = 0, len = 1;
        b_space(0);
        switch (level) {
        case 0:
            if (bp[0] == '|' && bp[1] == '|')
                op = '|', len = 2;
            break;
        case 1:
            if (bp[0] == '&' && bp[1] == '&')
                op = '&', len = 2;
            break;
        case 2:
            if ((bp[0] == '=' || bp[0] == '!' || bp[0] == '<' || bp[0] == '>') && bp[1] == '=')
                op = bp[0] == '=' ? 'e' : bp[0] == '!' ? 'n' : bp[0] == '<' ? 'l' : 'g', len = 2;
            else if (bp[0] == '<' || bp[0] == '>')
                op = bp[0];
            break;
        case 3:
            if ((bp[0] == '+' || bp[0] == '-') && bp[1] != '=')
                op = bp[0];
            break;
        default:
            if ((bp[0] == '*' || bp[0] == '/' || bp[0] == '%') && bp[1] != '=')
                op = bp[0];
        }
        if (!op)
            return l;
        bp += len;
        r = b_binary(level + 1);
        bassigned = 0;
        switch (op) {
        case '|': l = l || r; break;
        case '&': l = l && r; break;
        case 'e': l = l == r; break;
        case 'n': l = l != r; break;
        case 'l': l = l <= r; break;
        case 'g': l = l >= r; break;
        case '<': l = l < r; break;
        case '>': l = l > r; break;
        case '+': l += r; break;
        case '-': l -= r; break;
        case '*': l *= r; break;
        default:
            if (!r) {
                if (!bskip)
                    b_fail("divide by zero");
                l = 0;
            } else
                l = op == '/' ? l / r : l % r;
        }
    }
}

static long b_expr(void)
{
    bassigned = 0;
    return b_binary(0);
}

static void b_string(int print)
{
    for (bp++; *bp && *bp != '"'; bp++) {
        int c = *bp;
        if (c == '\\' && print && bp[1]) {
            c = *++bp;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'q': c = '"'; break;
            case 'a': c = 7; break;
            case 'r': c = '\r'; break;
            case 'f': c = '\f'; break;
            case 'b': c = '\b'; break;
            case 'e': c = '\\'; break;
            }
        }
        if (!bskip)
            putchar(c);
    }
    if (*bp)
        bp++;
}

static void b_paren(long *v)
{
    b_space(0);
    if (*bp != '(')
        b_fail("expected (");
    bp++;
    *v = b_expr();
    b_space(0);
    if (*bp != ')')
        b_fail("expected )");
    bp++;
}

static void b_skipped(void)
{
    bskip++;
    b_stmt();
    bskip--;
}

static void b_stmt(void)
{
    long v;
    if ((breturn || bbreak) && !bskip) {
        b_skipped();
        return;
    }
    b_space(1);
    if (*bp == '{') {
        bp++;
        for (;;) {
            b_space(1);
            if (*bp == ';') {
                bp++;
                continue;
            }
            if (!*bp || *bp == '}')
                break;
            b_stmt();
        }
        if (*bp)
            bp++;
        return;
    }
    if (*bp == '"') {
        b_string(0);
        return;
    }
    if (b_word("if")) {
        const char *save;
        b_paren(&v);
        if (v && !bskip && !breturn && !bbreak)
            b_stmt();
        else
            b_skipped();
        save = bp;
        b_space(1);
        if (b_word("else")) {
            if (!v && !bskip && !breturn && !bbreak)
                b_stmt();
            else
                b_skipped();
        } else
            bp = save;
        return;
    }
    if (b_word("while")) {
        const char *cond = bp;
        for (;;) {
            bp = cond;
            if (bskip || breturn || bbreak) {
                bskip++;
                b_paren(&v);
                bskip--;
                v = 0;
            } else
                b_paren(&v);
            if (!v) {
                b_skipped();
                if (!bskip)
                    bbreak = 0;
                return;
            }
            b_stmt();
        }
    }
    if (b_word("for")) {
        const char *cond, *step, *body;
        b_space(0);
        if (*bp != '(')
            b_fail("expected (");
        bp++;
        b_space(0);
        if (*bp != ';')
            b_expr();
        b_space(0);
        if (*bp != ';')
            b_fail("expected ;");
        cond = ++bp;
        for (;;) {
            bp = cond;
            b_space(0);
            if (bskip || breturn || bbreak) {
                bskip++;
                if (*bp != ';')
                    b_expr();
                bskip--;
                v = 0;
            } else
                v = *bp == ';' ? 1 : b_expr();
            b_space(0);
            if (*bp != ';')
                b_fail("expected ;");
            step = ++bp;
            bskip++;
            b_space(0);
            if (*bp != ')')
                b_expr();
            bskip--;
            b_space(0);
            if (*bp != ')')
                b_fail("expected )");
            body = ++bp;
            if (!v) {
                b_skipped();
                if (!bskip)
                    bbreak = 0;
                return;
            }
            b_stmt();
            if (!breturn && !bbreak) {
                bp = step;
                b_space(0);
                if (*bp != ')')
                    b_expr();
            }
            (void)body;
        }
    }
    if (b_word("return")) {
        b_space(0);
        v = 0;
        if (*bp && *bp != ';' && *bp != '\n' && *bp != '}')
            v = b_expr();
        if (!bskip && !breturn) {
            bretval = v;
            breturn = 1;
        }
        return;
    }
    if (b_word("break")) {
        if (!bskip)
            bbreak = 1;
        return;
    }
    if (b_word("halt") || b_word("quit")) {
        if (!bskip && !breturn && !bbreak) {
            fflush(stdout);
            exit(0);
        }
        return;
    }
    if (b_word("print")) {
        for (;;) {
            b_space(0);
            if (*bp == '"') {
                if (breturn || bbreak) {
                    bskip++;
                    b_string(1);
                    bskip--;
                } else
                    b_string(1);
            } else {
                v = b_expr();
                if (!bskip && !breturn && !bbreak)
                    b_print(v);
            }
            b_space(0);
            if (*bp != ',')
                break;
            bp++;
        }
        return;
    }
    if (b_word("define")) {
        const char *s;
        BVar *f;
        int depth = 0;
        b_space(0);
        s = bp;
        while (isalnum((unsigned char)*bp) || *bp == '_')
            bp++;
        f = b_var(s, (size_t)(bp - s));
        b_space(0);
        if (*bp != '(')
            b_fail("expected (");
        f->params = ++bp;
        while (*bp && *bp != ')')
            bp++;
        if (*bp)
            bp++;
        b_space(1);
        if (*bp != '{')
            b_fail("expected {");
        f->body = bp + 1;
        /* skip the body */
        for (; *bp; bp++) {
            if (*bp == '"') {
                for (bp++; *bp && *bp != '"'; bp++)
                    ;
            } else if (bp[0] == '/' && bp[1] == '*') {
                const char *e = strstr(bp + 2, "*/");
                bp = e ? e + 1 : bp + strlen(bp) - 1;
            } else if (*bp == '{')
                depth++;
            else if (*bp == '}' && --depth == 0) {
                bp++;
                break;
            }
        }
        return;
    }
    v = b_expr();
    if (!bskip && !breturn && !bbreak && !bassigned) {
        b_print(v);
        putchar('\n');
    }
}

static void b_run(const char *text)
{
    bp = text;
    for (;;) {
        b_space(1);
        if (*bp == ';') {
            bp++;
            continue;
        }
        if (!*bp)
            break;
        b_stmt();
        breturn = bbreak = 0;
    }
}

int bc_main(int argc, char **argv)
{
    int i, nfiles = 0;
    bin = stdin;
    for (i = 1; i < argc; i++) {
        char *text;
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        text = readfile(argv[i], 0);
        if (!text)
            die("%s: %s", argv[i], strerror(errno));
        nfiles++;
        b_run(text);
    }
    b_run(readall(0, 0));
    (void)nfiles;
    return 0;
}
