/* sed: the stream editor */
#include "box.h"

enum { A_NONE, A_LINE, A_LAST, A_RE, A_PLUS, A_STEP };

typedef struct Addr {
    int type;
    long n, step;
    regex_t *re;
} Addr;

typedef struct Cmd {
    Addr a1, a2;
    int neg, inrange, c;
    long endline;
    regex_t *re; /* s: null means the last one used */
    char *repl, *text, *label;
    int global, print, nth, jump;
    FILE *wf;
    unsigned char *map;
} Cmd;

static Cmd *cmds;
static int ncmds, capcmds;
static int nflag, ext, inplace;
static const char *sp; /* script text being parsed */
static regex_t *lastre;

static char **files;
static int nfiles, filei;
static FILE *in;
static Buf ps, hs, next, appendq;
static int havenext, lastnl, nextnl;
static long lineno;
static FILE *out;

static void sfail(const char *msg)
{
    fflush(stdout);
    fprintf(stderr, "sed: %s\n", msg);
    exit(1);
}

static Cmd *newcmd(void)
{
    if (ncmds == capcmds) {
        capcmds = capcmds ? capcmds * 2 : 32;
        cmds = xrealloc(cmds, (size_t)capcmds * sizeof *cmds);
    }
    memset(&cmds[ncmds], 0, sizeof *cmds);
    return &cmds[ncmds++];
}

/* read a delimited regex or replacement from the script; keeps escapes except
   that an escaped delimiter becomes the delimiter itself */
static char *delimited(int delim, int isre)
{
    Buf b;
    memset(&b, 0, sizeof b);
    for (;;) {
        int c = *sp;
        if (!c || (c == '\n' && isre))
            sfail("unterminated expression");
        sp++;
        if (c == delim)
            break;
        if (c == '\\') {
            if (*sp == delim) {
                if (isre && strchr(".*[]^$+?(){}|", delim))
                    bputc(&b, '\\');
                bputc(&b, *sp++);
            } else if (*sp == 'n' && isre) {
                bputc(&b, '\n');
                sp++;
            } else if (*sp == 't' && isre) {
                bputc(&b, '\t');
                sp++;
            } else {
                bputc(&b, c);
                if (*sp)
                    bputc(&b, *sp++);
            }
        } else if (c == '[' && isre) {
            /* a bracket expression: the delimiter is literal inside */
            bputc(&b, c);
            if (*sp == '^')
                bputc(&b, *sp++);
            if (*sp == ']')
                bputc(&b, *sp++);
            while (*sp && *sp != ']' && *sp != '\n') {
                if (sp[0] == '[' && (sp[1] == ':' || sp[1] == '.' || sp[1] == '=')) {
                    int k = sp[1];
                    bputc(&b, *sp++);
                    bputc(&b, *sp++);
                    while (*sp && !(sp[0] == k && sp[1] == ']'))
                        bputc(&b, *sp++);
                    if (*sp)
                        bputc(&b, *sp++);
                    if (*sp)
                        bputc(&b, *sp++);
                } else if (sp[0] == '\\' && sp[1] == 'n') {
                    bputc(&b, '\n');
                    sp += 2;
                } else if (sp[0] == '\\' && sp[1] == 't') {
                    bputc(&b, '\t');
                    sp += 2;
                } else if (sp[0] == '\\' && sp[1] == ']') {
                    bputc(&b, '\\');
                    sp++;
                } else
                    bputc(&b, *sp++);
            }
            if (*sp == ']')
                bputc(&b, *sp++);
        } else
            bputc(&b, c);
    }
    return bstr(&b);
}

static regex_t *compile(const char *pat, int icase)
{
    regex_t *re;
    if (!*pat)
        return 0;
    re = xmalloc(sizeof *re);
    if (regcomp(re, pat, (ext ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0))) {
        fflush(stdout);
        fprintf(stderr, "sed: bad regular expression: %s\n", pat);
        exit(1);
    }
    return re;
}

static int parse_addr(Addr *a)
{
    if (isdigit((unsigned char)*sp)) {
        char *end;
        a->type = A_LINE;
        a->n = strtol(sp, &end, 10);
        sp = end;
        if (*sp == '~') {
            a->type = A_STEP;
            a->step = strtol(sp + 1, &end, 10);
            sp = end;
        }
        return 1;
    }
    if (*sp == '$') {
        sp++;
        a->type = A_LAST;
        return 1;
    }
    if (*sp == '/' || *sp == '\\') {
        int delim, icase = 0;
        char *pat;
        if (*sp == '\\')
            sp++;
        delim = *sp++;
        pat = delimited(delim, 1);
        if (*sp == 'I') {
            icase = 1;
            sp++;
        }
        a->type = A_RE;
        a->re = compile(pat, icase);
        return 1;
    }
    return 0;
}

static char *read_text(void)
{
    Buf b;
    memset(&b, 0, sizeof b);
    while (*sp == ' ' || *sp == '\t')
        sp++;
    if (*sp == '\\') {
        sp++;
        if (*sp == '\n')
            sp++;
    }
    while (*sp && *sp != '\n') {
        if (*sp == '\\' && sp[1]) {
            sp++;
            bputc(&b, *sp++);
        } else
            bputc(&b, *sp++);
    }
    bputc(&b, '\n');
    return bstr(&b);
}

static char *read_word(int stop_semi)
{
    const char *s;
    while (*sp == ' ' || *sp == '\t')
        sp++;
    s = sp;
    while (*sp && *sp != '\n' && !(stop_semi && (*sp == ';' || *sp == '}' || *sp == ' ')))
        sp++;
    return xstrndup(s, (size_t)(sp - s));
}

static FILE *wfile(const char *name)
{
    FILE *f;
    if (!strcmp(name, "/dev/stdout"))
        return stdout;
    if (!strcmp(name, "/dev/stderr"))
        return stderr;
    f = fopen(name, "w");
    if (!f)
        sfail("cannot open file for writing");
    return f;
}

static void parse_script(const char *text)
{
    static int sdepth;
    static int sstack[64];
    sp = text;
    for (;;) {
        Cmd *c;
        while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == ';')
            sp++;
        if (!*sp)
            break;
        if (*sp == '#') {
            while (*sp && *sp != '\n')
                sp++;
            continue;
        }
        c = newcmd();
        if (parse_addr(&c->a1)) {
            while (*sp == ' ' || *sp == '\t')
                sp++;
            if (*sp == ',') {
                sp++;
                while (*sp == ' ' || *sp == '\t')
                    sp++;
                if (*sp == '+') {
                    char *end;
                    c->a2.type = A_PLUS;
                    c->a2.n = strtol(sp + 1, &end, 10);
                    sp = end;
                } else if (!parse_addr(&c->a2))
                    sfail("bad address");
            }
        }
        while (*sp == ' ' || *sp == '\t')
            sp++;
        while (*sp == '!') {
            c->neg = !c->neg;
            sp++;
            while (*sp == ' ' || *sp == '\t')
                sp++;
        }
        c->c = *sp++;
        switch (c->c) {
        case '{':
            if (sdepth == 64)
                sfail("nesting too deep");
            sstack[sdepth++] = ncmds - 1;
            break;
        case '}':
            if (!sdepth)
                sfail("unexpected }");
            cmds[sstack[--sdepth]].jump = ncmds - 1;
            break;
        case 's': {
            int delim = *sp++;
            char *pat;
            int icase = 0;
            if (!delim || delim == '\n' || delim == '\\')
                sfail("bad s command");
            pat = delimited(delim, 1);
            c->repl = delimited(delim, 0);
            for (;;) {
                if (*sp == 'g')
                    c->global = 1, sp++;
                else if (*sp == 'p')
                    c->print = 1, sp++;
                else if (*sp == 'i' || *sp == 'I')
                    icase = 1, sp++;
                else if (*sp == 'm' || *sp == 'M' || *sp == 'e')
                    sp++;
                else if (isdigit((unsigned char)*sp)) {
                    char *end;
                    c->nth = (int)strtol(sp, &end, 10);
                    sp = end;
                } else if (*sp == 'w') {
                    sp++;
                    c->wf = wfile(read_word(0));
                } else
                    break;
            }
            c->re = compile(pat, icase);
            break;
        }
        case 'y': {
            int delim = *sp++, i;
            char *a = delimited(delim, 0), *b = delimited(delim, 0), *p, *q;
            c->map = xmalloc(256);
            for (i = 0; i < 256; i++)
                c->map[i] = (unsigned char)i;
            for (p = a, q = b; *p && *q; p++, q++) {
                int x = (unsigned char)*p, y = (unsigned char)*q;
                if (x == '\\' && p[1]) {
                    x = (unsigned char)*++p;
                    if (x == 'n')
                        x = '\n';
                }
                if (y == '\\' && q[1]) {
                    y = (unsigned char)*++q;
                    if (y == 'n')
                        y = '\n';
                }
                c->map[x] = (unsigned char)y;
            }
            if (*p || *q)
                sfail("y: strings differ in length");
            break;
        }
        case 'a': case 'i': case 'c':
            c->text = read_text();
            break;
        case 'b': case 't': case 'T': case ':':
            c->label = read_word(c->c != ':');
            if (c->c == ':') {
                size_t n = strlen(c->label);
                while (n && (c->label[n - 1] == ' ' || c->label[n - 1] == ';'))
                    c->label[--n] = 0;
            }
            break;
        case 'r':
            c->text = read_word(0);
            break;
        case 'w':
            c->wf = wfile(read_word(0));
            break;
        case 'q': case 'Q':
            while (*sp == ' ')
                sp++;
            if (isdigit((unsigned char)*sp)) {
                char *end;
                c->nth = (int)strtol(sp, &end, 10);
                sp = end;
            }
            break;
        case 'd': case 'D': case 'p': case 'P': case 'n': case 'N': case 'h': case 'H': case 'g': case 'G': case 'x':
        case '=': case 'l':
            break;
        default: {
            char msg[64];
            sprintf(msg, "unknown command: '%c'", c->c);
            sfail(msg);
        }
        }
    }
}

static void resolve(void)
{
    int i, j;
    for (i = 0; i < ncmds; i++) {
        Cmd *c = &cmds[i];
        if (c->c != 'b' && c->c != 't' && c->c != 'T')
            continue;
        c->jump = ncmds;
        if (!*c->label)
            continue;
        for (j = 0; j < ncmds; j++)
            if (cmds[j].c == ':' && !strcmp(cmds[j].label, c->label))
                break;
        if (j == ncmds)
            sfail("undefined label");
        c->jump = j;
    }
}

/* input */

static int fetch(Buf *b, int *nl)
{
    int c;
    b->n = 0;
    for (;;) {
        if (!in) {
            if (filei >= nfiles)
                return 0;
            if (!strcmp(files[filei], "-"))
                in = stdin;
            else if (!(in = fopen(files[filei], "r"))) {
                fflush(stdout);
                fprintf(stderr, "sed: can't read %s: %s\n", files[filei], strerror(errno));
                filei++;
                continue;
            }
            filei++;
        }
        while ((c = getc(in)) != EOF) {
            if (c == '\n') {
                *nl = 1;
                bstr(b);
                return 1;
            }
            bputc(b, c);
        }
        if (in != stdin)
            fclose(in);
        in = 0;
        if (b->n) {
            *nl = 0;
            bstr(b);
            return 1;
        }
    }
}

static int islast(void) { return !havenext; }

/* move the next line into `to` (appending when asked); 0 at the end of input */
static int advance(Buf *to, int append)
{
    if (!havenext)
        return 0;
    if (append)
        bputc(to, '\n');
    else
        to->n = 0;
    bput(to, next.s ? next.s : "", next.n);
    bstr(to);
    lastnl = nextnl;
    lineno++;
    havenext = fetch(&next, &nextnl);
    return 1;
}

static void emit(const char *s, size_t n, int nl)
{
    fwrite(s, 1, n, out);
    if (nl)
        putc('\n', out);
}

static void flush_append(void)
{
    if (appendq.n) {
        fwrite(appendq.s, 1, appendq.n, out);
        appendq.n = 0;
    }
}

static int addr_match(Addr *a)
{
    switch (a->type) {
    case A_LINE: return lineno == a->n;
    case A_STEP: return a->step > 0 ? lineno >= a->n && (lineno - a->n) % a->step == 0 : lineno == a->n;
    case A_LAST: return islast();
    case A_RE: {
        regex_t *re = a->re ? a->re : lastre;
        if (!re)
            sfail("no previous regular expression");
        lastre = re;
        return regexec(re, bstr(&ps), 0, 0, 0) == 0;
    }
    }
    return 0;
}

static int selected(Cmd *c)
{
    int m;
    if (c->a1.type == A_NONE)
        m = 1;
    else if (c->a2.type == A_NONE)
        m = addr_match(&c->a1);
    else if (c->inrange) {
        m = 1;
        switch (c->a2.type) {
        case A_LINE:
            if (lineno >= c->a2.n)
                c->inrange = 0;
            break;
        case A_PLUS:
            if (lineno >= c->endline)
                c->inrange = 0;
            break;
        default:
            if (addr_match(&c->a2))
                c->inrange = 0;
        }
    } else {
        m = addr_match(&c->a1) || (c->a1.type == A_LINE && c->a1.n == 0 && lineno == 1);
        if (m) {
            c->inrange = 1;
            if (c->a2.type == A_LINE && c->a2.n <= lineno)
                c->inrange = 0;
            if (c->a2.type == A_PLUS) {
                c->endline = lineno + c->a2.n;
                if (!c->a2.n)
                    c->inrange = 0;
            }
            if (c->a1.type == A_LINE && c->a1.n == 0 && c->a2.type == A_RE && addr_match(&c->a2))
                c->inrange = 0;
        }
    }
    return m != c->neg;
}

static int substitute(Cmd *c)
{
    regex_t *re = c->re ? c->re : lastre;
    regmatch_t m[10];
    Buf o;
    const char *s;
    size_t at = 0;
    int count = 0, done = 0;
    if (!re)
        sfail("no previous regular expression");
    lastre = re;
    s = bstr(&ps);
    memset(&o, 0, sizeof o);
    while (at <= ps.n && regexec(re, s + at, 10, m, at ? REG_NOTBOL : 0) == 0) {
        size_t so = at + (size_t)m[0].rm_so, eo = at + (size_t)m[0].rm_eo;
        const char *r;
        count++;
        bput(&o, s + at, so - at);
        if (c->nth && count < c->nth) {
            bput(&o, s + so, eo - so);
        } else {
            for (r = c->repl; *r; r++) {
                if (*r == '\\' && r[1]) {
                    r++;
                    if (isdigit((unsigned char)*r)) {
                        int k = *r - '0';
                        if (m[k].rm_so >= 0)
                            bput(&o, s + at + m[k].rm_so, (size_t)(m[k].rm_eo - m[k].rm_so));
                    } else if (*r == 'n')
                        bputc(&o, '\n');
                    else if (*r == 't')
                        bputc(&o, '\t');
                    else
                        bputc(&o, *r);
                } else if (*r == '&')
                    bput(&o, s + so, eo - so);
                else
                    bputc(&o, *r);
            }
            done = 1;
        }
        if (eo == so) {
            if (so < ps.n)
                bputc(&o, s[so]);
            at = so + 1;
        } else
            at = eo;
        if (done && !c->global)
            break;
    }
    if (!done) {
        free(o.s);
        return 0;
    }
    if (at < ps.n)
        bput(&o, s + at, ps.n - at);
    free(ps.s);
    ps = o;
    bstr(&ps);
    return 1;
}

static int quit_status = -1;

static void process(void)
{
    int tflag, pc, i;
    lineno = 0;
    havenext = fetch(&next, &nextnl);
    while (advance(&ps, 0)) {
        tflag = 0;
    restart:
        for (pc = 0; pc < ncmds; pc++) {
            Cmd *c = &cmds[pc];
            if (c->c == '}' || c->c == ':')
                continue;
            if (!selected(c)) {
                if (c->c == '{')
                    pc = c->jump;
                continue;
            }
            switch (c->c) {
            case '{':
                break;
            case 's':
                if (substitute(c)) {
                    tflag = 1;
                    if (c->print)
                        emit(ps.s, ps.n, 1);
                    if (c->wf) {
                        fwrite(ps.s, 1, ps.n, c->wf);
                        putc('\n', c->wf);
                    }
                }
                break;
            case 'y':
                for (i = 0; i < (int)ps.n; i++)
                    ps.s[i] = (char)c->map[(unsigned char)ps.s[i]];
                break;
            case 'd':
                goto nextcycle;
            case 'D': {
                char *nl = memchr(bstr(&ps), '\n', ps.n);
                if (!nl)
                    goto nextcycle;
                memmove(ps.s, nl + 1, ps.n - (size_t)(nl + 1 - ps.s));
                ps.n -= (size_t)(nl + 1 - ps.s);
                bstr(&ps);
                flush_append();
                tflag = 0;
                goto restart;
            }
            case 'p':
                emit(ps.s, ps.n, 1);
                break;
            case 'P': {
                char *nl = memchr(bstr(&ps), '\n', ps.n);
                emit(ps.s, nl ? (size_t)(nl - ps.s) : ps.n, 1);
                break;
            }
            case 'n':
                if (!nflag)
                    emit(ps.s, ps.n, 1);
                flush_append();
                if (!advance(&ps, 0))
                    return;
                break;
            case 'N':
                flush_append();
                if (!advance(&ps, 1)) {
                    if (!nflag)
                        emit(ps.s, ps.n, lastnl);
                    return;
                }
                break;
            case 'a':
                bputs(&appendq, c->text);
                break;
            case 'i':
                fputs(c->text, out);
                break;
            case 'c':
                if (c->a2.type == A_NONE || !c->inrange)
                    fputs(c->text, out);
                goto nextcycle;
            case 'r': {
                size_t len;
                char *t = readfile(c->text, &len);
                if (t) {
                    bput(&appendq, t, len);
                    free(t);
                }
                break;
            }
            case 'w':
                fwrite(ps.s, 1, ps.n, c->wf);
                putc('\n', c->wf);
                break;
            case 'q':
                if (!nflag)
                    emit(ps.s, ps.n, lastnl);
                flush_append();
                quit_status = c->nth;
                return;
            case 'Q':
                quit_status = c->nth;
                return;
            case 'h':
                hs.n = 0;
                bput(&hs, ps.s, ps.n);
                break;
            case 'H':
                bputc(&hs, '\n');
                bput(&hs, ps.s, ps.n);
                break;
            case 'g':
                ps.n = 0;
                bput(&ps, hs.s ? hs.s : "", hs.n);
                bstr(&ps);
                break;
            case 'G':
                bputc(&ps, '\n');
                bput(&ps, hs.s ? hs.s : "", hs.n);
                bstr(&ps);
                break;
            case 'x': {
                Buf t = ps;
                ps = hs;
                hs = t;
                bstr(&ps);
                break;
            }
            case '=':
                fprintf(out, "%ld\n", lineno);
                break;
            case 'l':
                emit(ps.s, ps.n, 0);
                fputs("$\n", out);
                break;
            case 'b':
                pc = c->jump - 1;
                break;
            case 't':
                if (tflag) {
                    tflag = 0;
                    pc = c->jump - 1;
                }
                break;
            case 'T':
                if (!tflag)
                    pc = c->jump - 1;
                tflag = 0;
                break;
            }
        }
        if (!nflag)
            emit(ps.s, ps.n, lastnl || havenext);
    nextcycle:
        flush_append();
    }
}

int sed_main(int argc, char **argv)
{
    int i, havescript = 0, st = 0;
    Buf script;
    const char *suffix = 0;
    memset(&script, 0, sizeof script);
    cmds = 0;
    ncmds = capcmds = 0;
    nflag = ext = inplace = 0;
    out = stdout;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i], *p;
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--posix") || !strcmp(a, "--quiet") || !strcmp(a, "--silent")) {
            if (a[2] != 'p')
                nflag = 1;
            continue;
        }
        if (!strncmp(a, "--expression=", 13)) {
            bputs(&script, a + 13);
            bputc(&script, '\n');
            havescript = 1;
            continue;
        }
        if (!strcmp(a, "--regexp-extended")) {
            ext = 1;
            continue;
        }
        for (p = a + 1; *p; p++) {
            if (*p == 'n')
                nflag = 1;
            else if (*p == 'E' || *p == 'r')
                ext = 1;
            else if (*p == 's' || *p == 'u' || *p == 'z')
                ;
            else if (*p == 'i') {
                inplace = 1;
                suffix = p + 1;
                break;
            } else if (*p == 'e' || *p == 'f') {
                const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
                if (!v)
                    sfail("option needs an argument");
                if (*p == 'e')
                    bputs(&script, v);
                else {
                    char *t = readfile(v, 0);
                    if (!t)
                        sfail("cannot read script file");
                    if (t[0] == '#' && t[1] == 'n' && t[2] == '\n')
                        nflag = 1;
                    bputs(&script, t);
                }
                bputc(&script, '\n');
                havescript = 1;
                break;
            } else
                sfail("unknown option");
        }
    }
    if (!havescript) {
        if (i >= argc)
            sfail("missing script");
        bputs(&script, argv[i++]);
    }
    parse_script(bstr(&script));
    resolve();
    if (!inplace) {
        static char *dash[] = { (char *)"-", 0 };
        files = i < argc ? argv + i : dash;
        nfiles = i < argc ? argc - i : 1;
        filei = 0;
        process();
    } else {
        for (; i < argc; i++) {
            char *tmp = xmalloc(strlen(argv[i]) + 16);
            struct stat sb;
            int j;
            sprintf(tmp, "%s.sedXXXXXX", argv[i]);
            if (stat(argv[i], &sb) < 0) {
                fflush(stdout);
                fprintf(stderr, "sed: can't read %s: %s\n", argv[i], strerror(errno));
                st = 1;
                continue;
            }
            j = mkstemp(tmp);
            if (j < 0 || !(out = fdopen(j, "w")))
                sfail("cannot create temporary file");
            files = argv + i;
            nfiles = 1;
            filei = 0;
            for (j = 0; j < ncmds; j++)
                cmds[j].inrange = 0;
            process();
            fclose(out);
            chmod(tmp, sb.st_mode & 07777);
            if (suffix && *suffix) {
                char *bak = xmalloc(strlen(argv[i]) + strlen(suffix) + 1);
                sprintf(bak, "%s%s", argv[i], suffix);
                rename(argv[i], bak);
            }
            if (rename(tmp, argv[i]) < 0)
                sfail("cannot rename temporary file");
            if (quit_status >= 0)
                break;
        }
    }
    fflush(out);
    return quit_status > 0 ? quit_status : st;
}
