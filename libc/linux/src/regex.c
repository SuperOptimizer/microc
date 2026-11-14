/* POSIX regular expressions, basic and extended, with the usual GNU additions.
   A pattern is compiled to a small program that a backtracking matcher runs; the
   first alternative that leads to a match wins, and repetitions take as much as
   they can. */
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHAR, CHARI, ANY, SET, BOL, EOL, WORDB, NWORDB, WBEG, WEND, SAVE, BACKREF, SPLIT, JMP, MARK, CHECK, MATCH };

typedef struct Inst {
    short op;
    short y;
    int x;
} Inst;

typedef struct Prog {
    Inst *code;
    unsigned char *sets; /* 32 bytes each */
    const char **sub;    /* scratch for the matcher */
    const char **mark;
    int n, nsets, nsub, nmark, flags;
} Prog;

typedef struct C {
    const char *p;
    Inst *code;
    unsigned char *sets;
    int n, cap, nsets, nsub, nmark, flags, err, ere, depth;
} C;

#define MAXCODE 30000

static void emit(C *c, int op, int x, int y)
{
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        Inst *code = c->n < MAXCODE ? realloc(c->code, (size_t)cap * sizeof(Inst)) : 0;
        if (!code) {
            c->err = REG_ESPACE;
            c->n = 0; /* keep going harmlessly; the error is reported at the end */
            if (!c->cap)
                return;
        } else {
            c->code = code;
            c->cap = cap;
        }
    }
    c->code[c->n].op = (short)op;
    c->code[c->n].x = x;
    c->code[c->n].y = (short)y;
    c->n++;
}

static void insert(C *c, int at)
{
    emit(c, JMP, 0, 0);
    if (c->err)
        return;
    memmove(c->code + at + 1, c->code + at, (size_t)(c->n - 1 - at) * sizeof(Inst));
}

/* Length of the operator `ch` at the current position, or 0 if it is not there. */
static int op_at(C *c, int ch)
{
    if (ch == '*')
        return *c->p == '*';
    if (c->ere)
        return *c->p == ch;
    return c->p[0] == '\\' && c->p[1] == ch ? 2 : 0;
}

static void set_add(C *c, unsigned char *set, int ch)
{
    set[ch >> 3] |= (unsigned char)(1 << (ch & 7));
    if (c->flags & REG_ICASE) {
        int o = isupper(ch) ? tolower(ch) : toupper(ch);
        set[o >> 3] |= (unsigned char)(1 << (o & 7));
    }
}

static unsigned char *new_set(C *c)
{
    unsigned char *s = realloc(c->sets, (size_t)(c->nsets + 1) * 32);
    if (!s) {
        c->err = REG_ESPACE;
        return 0;
    }
    c->sets = s;
    s += c->nsets * 32;
    memset(s, 0, 32);
    emit(c, SET, c->nsets++, 0);
    return s;
}

static void class_add(C *c, unsigned char *set, int (*fn)(int), int invert)
{
    int i;
    for (i = 0; i < 256; i++)
        if ((fn(i) != 0) != invert)
            set_add(c, set, i);
}

static int is_word(int ch) { return isalnum(ch) || ch == '_'; }

static void bracket(C *c)
{
    unsigned char *set = new_set(c);
    int neg = 0, first = 1, i;
    if (!set)
        return;
    if (*c->p == '^') {
        neg = 1;
        c->p++;
    }
    for (;; first = 0) {
        int lo, hi;
        if (!*c->p) {
            c->err = REG_EBRACK;
            return;
        }
        if (*c->p == ']' && !first)
            break;
        if (c->p[0] == '[' && c->p[1] == ':') {
            static const char *const names[] = {"alpha", "digit", "alnum", "upper", "lower", "space",
                                                "blank", "punct", "print", "graph", "cntrl", "xdigit"};
            static int (*const fns[])(int) = {isalpha, isdigit, isalnum, isupper, islower, isspace,
                                              isblank, ispunct, isprint, isgraph, iscntrl, isxdigit};
            const char *e = strstr(c->p + 2, ":]");
            int k;
            if (!e) {
                c->err = REG_ECTYPE;
                return;
            }
            for (k = 0; k < 12; k++)
                if (strlen(names[k]) == (size_t)(e - c->p - 2) && memcmp(names[k], c->p + 2, strlen(names[k])) == 0)
                    break;
            if (k == 12) {
                c->err = REG_ECTYPE;
                return;
            }
            if ((c->flags & REG_ICASE) && (k == 3 || k == 4))
                k = 0;
            class_add(c, set, fns[k], 0);
            c->p = e + 2;
            continue;
        }
        if (c->p[0] == '[' && (c->p[1] == '=' || c->p[1] == '.') && c->p[2] && c->p[3] == c->p[1] && c->p[4] == ']') {
            lo = (unsigned char)c->p[2];
            c->p += 5;
        } else {
            lo = (unsigned char)*c->p++;
        }
        hi = lo;
        if (c->p[0] == '-' && c->p[1] && c->p[1] != ']') {
            hi = (unsigned char)c->p[1];
            c->p += 2;
            if (hi < lo) {
                c->err = REG_ERANGE;
                return;
            }
        }
        for (i = lo; i <= hi; i++)
            set_add(c, set, i);
    }
    c->p++;
    if (neg) {
        for (i = 0; i < 32; i++)
            set[i] = (unsigned char)~set[i];
        if (c->flags & REG_NEWLINE)
            set['\n' >> 3] &= (unsigned char)~(1 << ('\n' & 7));
    }
}

static void alternatives(C *c);

static void literal(C *c, int ch)
{
    if ((c->flags & REG_ICASE) && isalpha(ch))
        emit(c, CHARI, tolower(ch), 0);
    else
        emit(c, CHAR, ch, 0);
}

/* One atom; returns 0 when the branch ends here. `start` is true at the start of a branch. */
static int atom(C *c, int start)
{
    int ch = (unsigned char)*c->p, l;
    unsigned char *set;
    if (!ch || op_at(c, '|') || (op_at(c, ')') && c->depth))
        return 0;
    if ((l = op_at(c, '(')) != 0) {
        int g = ++c->nsub;
        c->p += l;
        c->depth++;
        emit(c, SAVE, 2 * g, 0);
        alternatives(c);
        emit(c, SAVE, 2 * g + 1, 0);
        c->depth--;
        if ((l = op_at(c, ')')) == 0) {
            if (!c->err)
                c->err = REG_EPAREN;
            return 0;
        }
        c->p += l;
        return 1;
    }
    c->p++;
    switch (ch) {
    case '.':
        emit(c, ANY, 0, 0);
        return 1;
    case '[':
        bracket(c);
        return 1;
    case '^':
        if (c->ere || start) {
            emit(c, BOL, 0, 0);
            return 1;
        }
        break;
    case '$':
        if (c->ere || !*c->p || op_at(c, '|') || (op_at(c, ')') && c->depth)) {
            emit(c, EOL, 0, 0);
            return 1;
        }
        break;
    case '\\':
        ch = (unsigned char)*c->p;
        if (!ch) {
            c->err = REG_EESCAPE;
            return 0;
        }
        c->p++;
        switch (ch) {
        case 'w': case 'W': case 's': case 'S':
            set = new_set(c);
            if (set)
                class_add(c, set, ch == 'w' || ch == 'W' ? is_word : isspace, ch == 'W' || ch == 'S');
            return 1;
        case 'b': emit(c, WORDB, 0, 0); return 1;
        case 'B': emit(c, NWORDB, 0, 0); return 1;
        case '<': emit(c, WBEG, 0, 0); return 1;
        case '>': emit(c, WEND, 0, 0); return 1;
        case '`': emit(c, BOL, 1, 0); return 1;
        case '\'': emit(c, EOL, 1, 0); return 1;
        }
        if (ch >= '1' && ch <= '9') {
            if (ch - '0' > c->nsub) {
                c->err = REG_ESUBREG;
                return 0;
            }
            emit(c, BACKREF, ch - '0', 0);
            return 1;
        }
        break;
    }
    literal(c, ch);
    return 1;
}

static void repeat(C *c, int s, int min, int max)
{
    int len = c->n - s, i;
    Inst *a;
    if (c->err)
        return;
    a = malloc((size_t)(len + 1) * sizeof(Inst));
    if (!a) {
        c->err = REG_ESPACE;
        return;
    }
    memcpy(a, c->code + s, (size_t)len * sizeof(Inst));
    c->n = s;
#define COPY() for (i = 0; i < len && !c->err; i++) emit(c, a[i].op, a[i].x, a[i].y)
    while (min-- > 0) {
        COPY();
        if (max > 0)
            max--;
    }
    if (max < 0) {
        int k = c->nmark++;
        emit(c, SPLIT, 1, len + 4);
        emit(c, MARK, k, 0);
        COPY();
        emit(c, CHECK, k, 0);
        emit(c, JMP, -(len + 3), 0);
    } else {
        int r = max, j;
        for (j = 0; j < r && !c->err; j++) {
            if ((r - j) * (len + 1) > 32000) {
                c->err = REG_ESPACE;
                break;
            }
            emit(c, SPLIT, 1, (r - j) * (len + 1));
            COPY();
        }
    }
#undef COPY
    free(a);
}

static void branch(C *c)
{
    int start = 1;
    for (;;) {
        int s = c->n, l;
        if (c->err)
            return;
        if (*c->p == '*' && start) { /* nothing to repeat: an ordinary character */
            c->p++;
            literal(c, '*');
        } else if (!atom(c, start))
            return;
        start = !c->ere && s < c->n && c->code[c->n - 1].op == BOL && c->n - s == 1;
        for (;;) {
            if (*c->p == '*') {
                c->p++;
                repeat(c, s, 0, -1);
            } else if ((l = (c->ere ? *c->p == '+' : c->p[0] == '\\' && c->p[1] == '+' ? 2 : 0)) != 0) {
                c->p += l;
                repeat(c, s, 1, -1);
            } else if ((l = (c->ere ? *c->p == '?' : c->p[0] == '\\' && c->p[1] == '?' ? 2 : 0)) != 0) {
                c->p += l;
                repeat(c, s, 0, 1);
            } else if ((l = op_at(c, '{')) != 0 && (!c->ere || isdigit((unsigned char)c->p[1]))) {
                int min = 0, max;
                c->p += l;
                if (!isdigit((unsigned char)*c->p)) {
                    c->err = REG_BADBR;
                    return;
                }
                while (isdigit((unsigned char)*c->p) && min < 1000)
                    min = min * 10 + (*c->p++ - '0');
                max = min;
                if (*c->p == ',') {
                    c->p++;
                    max = -1;
                    if (isdigit((unsigned char)*c->p)) {
                        max = 0;
                        while (isdigit((unsigned char)*c->p) && max < 1000)
                            max = max * 10 + (*c->p++ - '0');
                    }
                }
                if ((l = op_at(c, '}')) == 0) {
                    c->err = REG_EBRACE;
                    return;
                }
                c->p += l;
                if (min > 255 || max > 255 || (max >= 0 && max < min)) {
                    c->err = REG_BADBR;
                    return;
                }
                repeat(c, s, min, max);
            } else
                break;
            if (c->err)
                return;
        }
    }
}

static void alternatives(C *c)
{
    int s = c->n, l, j;
    branch(c);
    if (c->err || (l = op_at(c, '|')) == 0)
        return;
    c->p += l;
    insert(c, s);
    if (c->err)
        return;
    if (c->n - s + 1 > 32000) {
        c->err = REG_ESPACE;
        return;
    }
    c->code[s].op = SPLIT;
    c->code[s].x = 1;
    c->code[s].y = (short)(c->n - s + 1);
    j = c->n;
    emit(c, JMP, 0, 0);
    alternatives(c);
    if (!c->err)
        c->code[j].x = c->n - j;
}

int regcomp(regex_t *re, const char *pattern, int flags)
{
    C c;
    Prog *g;
    memset(&c, 0, sizeof c);
    c.p = pattern;
    c.flags = flags;
    c.ere = flags & REG_EXTENDED;
    re->__prog = 0;
    re->re_nsub = 0;
    alternatives(&c);
    if (!c.err && *c.p)
        c.err = REG_EPAREN; /* a stray closing parenthesis */
    emit(&c, MATCH, 0, 0);
    g = c.err ? 0 : malloc(sizeof(Prog));
    if (g) {
        g->sub = malloc((size_t)(2 * c.nsub + 2) * sizeof(char *));
        g->mark = malloc((size_t)(c.nmark + 1) * sizeof(char *));
    }
    if (!g || !g->sub || !g->mark) {
        if (g) {
            free(g->sub);
            free(g->mark);
            free(g);
        }
        free(c.code);
        free(c.sets);
        return c.err ? c.err : REG_ESPACE;
    }
    g->code = c.code;
    g->sets = c.sets;
    g->n = c.n;
    g->nsets = c.nsets;
    g->nsub = c.nsub;
    g->nmark = c.nmark;
    g->flags = flags;
    re->__prog = g;
    re->re_nsub = (size_t)c.nsub;
    return 0;
}

void regfree(regex_t *re)
{
    Prog *g = re->__prog;
    if (!g)
        return;
    free(g->code);
    free(g->sets);
    free(g->sub);
    free(g->mark);
    free(g);
    re->__prog = 0;
}

typedef struct M {
    const Prog *g;
    const char *base, *end, *stop; /* stop: where the match ended */
    int notbol, noteol, newline;
} M;

static int word_before(M *m, const char *p) { return p > m->base && is_word((unsigned char)p[-1]); }
static int word_at(M *m, const char *p) { return p < m->end && is_word((unsigned char)*p); }

static int run(M *m, const Inst *pc, const char *p)
{
    const char *old;
    for (;;) {
        switch (pc->op) {
        case CHAR:
            if (p >= m->end || (unsigned char)*p != pc->x)
                return 0;
            p++;
            break;
        case CHARI:
            if (p >= m->end || tolower((unsigned char)*p) != pc->x)
                return 0;
            p++;
            break;
        case ANY:
            if (p >= m->end || (m->newline && *p == '\n'))
                return 0;
            p++;
            break;
        case SET:
            if (p >= m->end || !(m->g->sets[pc->x * 32 + ((unsigned char)*p >> 3)] & 1 << (*p & 7)))
                return 0;
            p++;
            break;
        case BOL:
            if (!((p == m->base && !m->notbol) || (m->newline && !pc->x && p > m->base && p[-1] == '\n')))
                return 0;
            break;
        case EOL:
            if (!((p == m->end && !m->noteol) || (m->newline && !pc->x && p < m->end && *p == '\n')))
                return 0;
            break;
        case WORDB:
            if (word_before(m, p) == word_at(m, p))
                return 0;
            break;
        case NWORDB:
            if (word_before(m, p) != word_at(m, p))
                return 0;
            break;
        case WBEG:
            if (word_before(m, p) || !word_at(m, p))
                return 0;
            break;
        case WEND:
            if (!word_before(m, p) || word_at(m, p))
                return 0;
            break;
        case SAVE:
            old = m->g->sub[pc->x];
            m->g->sub[pc->x] = p;
            if (run(m, pc + 1, p))
                return 1;
            m->g->sub[pc->x] = old;
            return 0;
        case BACKREF: {
            const char *a = m->g->sub[2 * pc->x], *b = m->g->sub[2 * pc->x + 1];
            size_t n, i;
            if (!a || !b)
                return 0;
            n = (size_t)(b - a);
            if ((size_t)(m->end - p) < n)
                return 0;
            if (m->g->flags & REG_ICASE) {
                for (i = 0; i < n; i++)
                    if (tolower((unsigned char)a[i]) != tolower((unsigned char)p[i]))
                        return 0;
            } else if (memcmp(a, p, n) != 0)
                return 0;
            p += n;
            break;
        }
        case SPLIT:
            if (run(m, pc + pc->x, p))
                return 1;
            pc += pc->y;
            continue;
        case JMP:
            pc += pc->x;
            continue;
        case MARK:
            old = m->g->mark[pc->x];
            m->g->mark[pc->x] = p;
            if (run(m, pc + 1, p))
                return 1;
            m->g->mark[pc->x] = old;
            return 0;
        case CHECK:
            if (m->g->mark[pc->x] == p)
                return 0;
            break;
        case MATCH:
            m->stop = p;
            return 1;
        }
        pc++;
    }
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *out, int flags)
{
    const Prog *g = re->__prog;
    M m;
    const char *p, *from = s;
    const Inst *first = g->code;
    size_t i;
    m.g = g;
    m.end = 0;
    if (flags & REG_STARTEND) {
        from = s + out[0].rm_so;
        m.end = s + out[0].rm_eo;
    }
    if (!m.end)
        m.end = from + strlen(from);
    m.base = from;
    m.notbol = flags & REG_NOTBOL;
    m.noteol = flags & REG_NOTEOL;
    m.newline = g->flags & REG_NEWLINE;
    if (g->flags & REG_NOSUB)
        nmatch = 0;
    for (p = from;; p++) {
        if (first->op == CHAR) {
            p = memchr(p, first->x, (size_t)(m.end - p));
            if (!p)
                break;
        }
        for (i = 0; i < (size_t)(2 * g->nsub + 2); i++)
            g->sub[i] = 0;
        if (run(&m, g->code, p)) {
            for (i = 0; i < nmatch; i++) {
                if (i == 0) {
                    out[0].rm_so = p - s;
                    out[0].rm_eo = m.stop - s;
                } else if (i <= (size_t)g->nsub && g->sub[2 * i] && g->sub[2 * i + 1]) {
                    out[i].rm_so = g->sub[2 * i] - s;
                    out[i].rm_eo = g->sub[2 * i + 1] - s;
                } else {
                    out[i].rm_so = out[i].rm_eo = -1;
                }
            }
            return 0;
        }
        if (p >= m.end || (first->op == BOL && !m.newline))
            break;
    }
    return REG_NOMATCH;
}

size_t regerror(int err, const regex_t *re, char *buf, size_t n)
{
    static const char *const msgs[] = {"Success", "No match", "Invalid regular expression",
        "Invalid collation character", "Invalid character class name", "Trailing backslash",
        "Invalid back reference", "Unmatched [, [^, [:, [., or [=", "Unmatched ( or \\(", "Unmatched \\{",
        "Invalid content of \\{\\}", "Invalid range end", "Memory exhausted", "Invalid preceding regular expression"};
    const char *s = err >= 0 && err <= 13 ? msgs[err] : "Unknown error";
    (void)re;
    if (n) {
        strncpy(buf, s, n - 1);
        buf[n - 1] = 0;
    }
    return strlen(s) + 1;
}
