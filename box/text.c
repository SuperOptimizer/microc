/* Text and miscellaneous tools: head tail wc basename dirname sleep env uname whoami
   id hostname getconf seq which nproc date expr tr cut sort uniq od xargs sha1sum
   pwd true false. */
#include "box.h"
#include <sys/utsname.h>

static const char *optarg_of(int argc, char **argv, int *i, int skip)
{
    if (argv[*i][skip])
        return argv[*i] + skip;
    if (*i + 1 >= argc)
        die("option %s needs an argument", argv[*i]);
    return argv[++*i];
}

int true_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

int false_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

int pwd_main(int argc, char **argv)
{
    char buf[4096];
    (void)argc;
    (void)argv;
    if (!getcwd(buf, sizeof buf))
        return 1;
    puts(buf);
    return 0;
}

int head_main(int argc, char **argv)
{
    long n = 10, k;
    int i, bytes = 0, st = 0, c, many;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (argv[i][1] == 'n')
            n = atol(optarg_of(argc, argv, &i, 2));
        else if (argv[i][1] == 'c') {
            bytes = 1;
            n = atol(optarg_of(argc, argv, &i, 2));
        } else if (isdigit((unsigned char)argv[i][1]))
            n = atol(argv[i] + 1);
        else if (argv[i][1] != 'q')
            die("bad option %s", argv[i]);
    }
    many = argc - i > 1;
    do {
        FILE *f = i < argc ? (strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin) : stdin;
        if (!f) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        if (many)
            printf("==> %s <==\n", argv[i]);
        for (k = 0; k < n && (c = getc(f)) != EOF;) {
            putchar(c);
            if (bytes || c == '\n')
                k++;
        }
        if (f != stdin)
            fclose(f);
    } while (++i < argc);
    return st;
}

int tail_main(int argc, char **argv)
{
    long n = 10;
    int i, bytes = 0, from = 0, st = 0;
    for (i = 1; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]; i++) {
        const char *a = argv[i], *v;
        if (a[0] == '+') {
            from = 1;
            n = atol(a + 1);
        } else if (a[1] == 'n' || a[1] == 'c') {
            bytes = a[1] == 'c';
            v = optarg_of(argc, argv, &i, 2);
            if (*v == '+')
                from = 1;
            n = atol(*v == '+' || *v == '-' ? v + 1 : v);
        } else if (isdigit((unsigned char)a[1]))
            n = atol(a + 1);
        else if (a[1] != 'q' && a[1] != 'f')
            die("bad option %s", a);
    }
    do {
        size_t len, pos;
        char *s;
        if (i < argc && strcmp(argv[i], "-")) {
            s = readfile(argv[i], &len);
            if (!s) {
                warn("%s: %s", argv[i], strerror(errno));
                st = 1;
                continue;
            }
        } else
            s = readall(0, &len);
        if (bytes) {
            pos = from ? (size_t)(n > 0 ? n - 1 : 0) : ((size_t)n < len ? len - (size_t)n : 0);
        } else if (from) {
            long k = 1;
            for (pos = 0; pos < len && k < n; pos++)
                if (s[pos] == '\n')
                    k++;
        } else {
            long k = 0;
            pos = len;
            if (pos && s[pos - 1] == '\n')
                pos--;
            for (; pos > 0; pos--)
                if (s[pos - 1] == '\n' && ++k >= n)
                    break;
            if (n == 0)
                pos = len;
        }
        if (pos < len)
            fwrite(s + pos, 1, len - pos, stdout);
        free(s);
    } while (++i < argc);
    return st;
}

int wc_main(int argc, char **argv)
{
    int i, lf = 0, wf = 0, cf = 0, st = 0, nfiles;
    long tl = 0, tw = 0, tc = 0;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'l')
                lf = 1;
            else if (*p == 'w')
                wf = 1;
            else if (*p == 'c' || *p == 'm')
                cf = 1;
        }
    }
    if (!lf && !wf && !cf)
        lf = wf = cf = 1;
    nfiles = argc - i;
    do {
        FILE *f = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        long l = 0, w = 0, ch = 0;
        int c, inword = 0, first = 1;
        if (!f) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        while ((c = getc(f)) != EOF) {
            ch++;
            if (c == '\n')
                l++;
            if (isspace(c))
                inword = 0;
            else if (!inword) {
                inword = 1;
                w++;
            }
        }
        if (f != stdin)
            fclose(f);
        if (lf)
            printf("%ld", l), first = 0;
        if (wf)
            printf("%s%ld", first ? "" : " ", w), first = 0;
        if (cf)
            printf("%s%ld", first ? "" : " ", ch);
        if (i < argc)
            printf(" %s", argv[i]);
        putchar('\n');
        tl += l;
        tw += w;
        tc += ch;
    } while (++i < argc);
    if (nfiles > 1) {
        int first = 1;
        if (lf)
            printf("%ld", tl), first = 0;
        if (wf)
            printf("%s%ld", first ? "" : " ", tw), first = 0;
        if (cf)
            printf("%s%ld", first ? "" : " ", tc);
        puts(" total");
    }
    return st;
}

int basename_main(int argc, char **argv)
{
    char *s, *b;
    size_t n, k;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    if (i >= argc)
        die("missing operand");
    s = xstrdup(argv[i]);
    n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        s[--n] = 0;
    b = strrchr(s, '/');
    b = b && b[1] ? b + 1 : s;
    if (i + 1 < argc) {
        n = strlen(b);
        k = strlen(argv[i + 1]);
        if (k < n && !strcmp(b + n - k, argv[i + 1]))
            b[n - k] = 0;
    }
    puts(b);
    return 0;
}

int dirname_main(int argc, char **argv)
{
    char *s;
    size_t n;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    if (i >= argc)
        die("missing operand");
    s = xstrdup(argv[i]);
    n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        n--;
    while (n > 0 && s[n - 1] != '/')
        n--;
    while (n > 1 && s[n - 1] == '/')
        n--;
    if (n == 0)
        puts(".");
    else {
        s[n] = 0;
        puts(s);
    }
    return 0;
}

int sleep_main(int argc, char **argv)
{
    double t;
    struct timeval tv;
    if (argc < 2)
        die("missing operand");
    t = strtod(argv[1], 0);
    tv.tv_sec = (long)t;
    tv.tv_usec = (int)((t - (double)(long)t) * 1e6);
    select(0, 0, 0, 0, &tv);
    return 0;
}

int env_main(int argc, char **argv)
{
    int i = 1;
    char **e;
    static char *empty[1];
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "-"))
            environ = empty;
        else if (!strcmp(argv[i], "-u") && i + 1 < argc)
            unsetenv(argv[++i]);
        else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else if (strchr(argv[i], '=') && argv[i][0] != '=')
            putenv(argv[i]);
        else
            break;
    }
    if (i < argc)
        box_exec(argv + i, environ);
    for (e = environ; e && *e; e++)
        puts(*e);
    return 0;
}

int uname_main(int argc, char **argv)
{
    struct utsname u;
    int i, mask = 0, first = 1;
    const char *p;
    if (uname(&u) < 0)
        die("uname failed");
    for (i = 1; i < argc; i++)
        for (p = argv[i] + (argv[i][0] == '-'); *p; p++)
            switch (*p) {
            case 's': mask |= 1; break;
            case 'n': mask |= 2; break;
            case 'r': mask |= 4; break;
            case 'v': mask |= 8; break;
            case 'm': mask |= 16; break;
            case 'p': mask |= 32; break;
            case 'o': mask |= 1; break;
            case 'a': mask |= 31; break;
            }
    if (!mask)
        mask = 1;
    for (i = 0; i < 6; i++)
        if (mask >> i & 1) {
            const char *s = i == 0 ? u.sysname : i == 1 ? u.nodename : i == 2 ? u.release : i == 3 ? u.version
                                                                                                  : u.machine;
            if (i == 5)
                s = !strcmp(u.machine, "arm64") ? "arm" : u.machine;
            printf("%s%s", first ? "" : " ", s);
            first = 0;
        }
    putchar('\n');
    return 0;
}

int hostname_main(int argc, char **argv)
{
    struct utsname u;
    (void)argc;
    (void)argv;
    if (uname(&u) < 0)
        return 1;
    puts(u.nodename);
    return 0;
}

static const char *user_name(void)
{
    static char buf[32];
    const char *u = getenv("USER");
    if (!u || !*u)
        u = getenv("LOGNAME");
    if (u && *u)
        return u;
    if (getuid() == 0)
        return "root";
    sprintf(buf, "%d", (int)getuid());
    return buf;
}

int whoami_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    puts(user_name());
    return 0;
}

int id_main(int argc, char **argv)
{
    int i, uflag = 0, gflag = 0, nflag = 0;
    for (i = 1; i < argc; i++) {
        if (strchr(argv[i], 'u'))
            uflag = 1;
        if (strchr(argv[i], 'g'))
            gflag = 1;
        if (strchr(argv[i], 'n'))
            nflag = 1;
    }
    if (uflag && nflag)
        puts(user_name());
    else if (uflag)
        printf("%d\n", (int)getuid());
    else if (gflag)
        printf("%d\n", (int)getgid());
    else
        printf("uid=%d(%s) gid=%d\n", (int)getuid(), user_name(), (int)getgid());
    return 0;
}

static int ncpu(void)
{
    const char *e = getenv("MBOX_NPROC");
    long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int nproc_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("%d\n", ncpu());
    return 0;
}

int getconf_main(int argc, char **argv)
{
    const char *n = argc > 1 ? argv[1] : "";
    if (!strncmp(n, "LFS", 3) || !strncmp(n, "POSIX_V", 7) || !strncmp(n, "XBS5", 4))
        puts("");
    else if (!strcmp(n, "_NPROCESSORS_ONLN") || !strcmp(n, "_NPROCESSORS_CONF") || !strcmp(n, "NPROCESSORS_ONLN"))
        printf("%d\n", ncpu());
    else if (!strcmp(n, "PAGESIZE") || !strcmp(n, "PAGE_SIZE"))
        printf("%d\n", (int)getpagesize());
    else if (!strcmp(n, "LONG_BIT"))
        printf("%d\n", (int)(sizeof(long) * 8));
    else if (!strcmp(n, "ARG_MAX"))
        puts("262144");
    else if (!strcmp(n, "PATH"))
        puts("/usr/bin:/bin");
    else {
        puts("undefined");
        return 1;
    }
    return 0;
}

int seq_main(int argc, char **argv)
{
    long first = 1, step = 1, last, v;
    if (argc < 2)
        die("missing operand");
    last = atol(argv[argc - 1]);
    if (argc > 2)
        first = atol(argv[1]);
    if (argc > 3)
        step = atol(argv[2]);
    if (!step)
        die("zero increment");
    for (v = first; step > 0 ? v <= last : v >= last; v += step)
        printf("%ld\n", v);
    return 0;
}

int which_main(int argc, char **argv)
{
    int i, st = 0;
    const char *path = getenv("PATH");
    for (i = 1; i < argc; i++) {
        char *p;
        if (argv[i][0] == '-')
            continue;
        p = path_search(argv[i], path ? path : "/usr/bin:/bin");
        if (p && access(p, X_OK) == 0)
            puts(p);
        else
            st = 1;
    }
    return st;
}

int date_main(int argc, char **argv)
{
    time_t t = time(0);
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y", *e;
    char buf[512];
    int i, utc = 0;
    struct tm *tm;
    e = getenv("SOURCE_DATE_EPOCH");
    if (e && *e)
        t = (time_t)atol(e);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '+')
            fmt = a + 1;
        else if (!strcmp(a, "-u") || !strcmp(a, "--utc"))
            utc = 1;
        else if (!strcmp(a, "-R"))
            fmt = "%a, %d %b %Y %H:%M:%S +0000";
        else if (!strncmp(a, "-d", 2) || !strncmp(a, "--date=", 7) || !strncmp(a, "-r", 2)) {
            const char *v = a[1] == '-' ? a + 7 : optarg_of(argc, argv, &i, 2);
            struct stat st;
            if (*v == '@')
                t = (time_t)atol(v + 1);
            else if (a[1] == 'r' && stat(v, &st) == 0)
                t = st.st_mtime;
            else if (a[1] == 'r' && isdigit((unsigned char)*v))
                t = (time_t)atol(v);
        }
    }
    tm = utc ? gmtime(&t) : localtime(&t);
    if (!strftime(buf, sizeof buf, fmt, tm))
        buf[0] = 0;
    puts(buf);
    return 0;
}

/* expr */
static char **xv;
static int xi, xn;
static char *x_or(void);

static int x_isnum(const char *s)
{
    if (*s == '-')
        s++;
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return 0;
    return 1;
}

static int x_null(const char *s) { return !*s || (x_isnum(s) && atol(s) == 0); }

static char *x_num(long v)
{
    char buf[32];
    sprintf(buf, "%ld", v);
    return xstrdup(buf);
}

static int x_is(const char *op) { return xi < xn && !strcmp(xv[xi], op); }

static char *x_primary(void)
{
    char *v;
    if (xi >= xn)
        die("syntax error");
    if (x_is("(")) {
        xi++;
        v = x_or();
        if (!x_is(")"))
            die("syntax error");
        xi++;
        return v;
    }
    if (x_is("length") && xi + 1 < xn) {
        xi += 2;
        return x_num((long)strlen(xv[xi - 1]));
    }
    if (x_is("substr") && xi + 3 < xn) {
        const char *s = xv[xi + 1];
        long pos = atol(xv[xi + 2]), len = atol(xv[xi + 3]), n = (long)strlen(s);
        xi += 4;
        if (pos < 1 || pos > n || len <= 0)
            return xstrdup("");
        if (len > n - pos + 1)
            len = n - pos + 1;
        return xstrndup(s + pos - 1, (size_t)len);
    }
    if (x_is("index") && xi + 2 < xn) {
        const char *s = xv[xi + 1];
        size_t k = strcspn(s, xv[xi + 2]);
        xi += 3;
        return x_num(s[k] ? (long)k + 1 : 0);
    }
    return xstrdup(xv[xi++]);
}

static char *x_match(void)
{
    char *l = x_primary();
    while (x_is(":")) {
        regex_t re;
        regmatch_t m[2];
        char *pat, *r;
        xi++;
        pat = x_primary();
        if (regcomp(&re, pat, 0))
            die("bad regular expression");
        if (regexec(&re, l, 2, m, 0) == 0 && m[0].rm_so == 0) {
            if (re.re_nsub > 0)
                r = m[1].rm_so >= 0 ? xstrndup(l + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so)) : xstrdup("");
            else
                r = x_num((long)m[0].rm_eo);
        } else
            r = re.re_nsub > 0 ? xstrdup("") : x_num(0);
        regfree(&re);
        l = r;
    }
    return l;
}

static char *x_mul(void)
{
    char *l = x_match();
    while (x_is("*") || x_is("/") || x_is("%")) {
        int op = xv[xi++][0];
        char *r = x_match();
        long a, b;
        if (!x_isnum(l) || !x_isnum(r))
            die("non-numeric argument");
        a = atol(l);
        b = atol(r);
        if (op != '*' && !b)
            die("division by zero");
        l = x_num(op == '*' ? a * b : op == '/' ? a / b : a % b);
    }
    return l;
}

static char *x_add(void)
{
    char *l = x_mul();
    while (x_is("+") || x_is("-")) {
        int op = xv[xi++][0];
        char *r = x_mul();
        if (!x_isnum(l) || !x_isnum(r))
            die("non-numeric argument");
        l = x_num(op == '+' ? atol(l) + atol(r) : atol(l) - atol(r));
    }
    return l;
}

static char *x_cmp(void)
{
    char *l = x_add();
    while (x_is("=") || x_is("==") || x_is("!=") || x_is("<") || x_is("<=") || x_is(">") || x_is(">=")) {
        const char *op = xv[xi++];
        char *r = x_add();
        long c;
        int v;
        if (x_isnum(l) && x_isnum(r)) {
            long a = atol(l), b = atol(r);
            c = a < b ? -1 : a > b;
        } else
            c = strcmp(l, r);
        if (op[0] == '=')
            v = c == 0;
        else if (op[0] == '!')
            v = c != 0;
        else if (op[0] == '<')
            v = op[1] ? c <= 0 : c < 0;
        else
            v = op[1] ? c >= 0 : c > 0;
        l = x_num(v);
    }
    return l;
}

static char *x_and(void)
{
    char *l = x_cmp();
    while (x_is("&")) {
        char *r;
        xi++;
        r = x_cmp();
        if (x_null(l) || x_null(r))
            l = xstrdup("0");
    }
    return l;
}

static char *x_or(void)
{
    char *l = x_and();
    while (x_is("|")) {
        char *r;
        xi++;
        r = x_and();
        if (x_null(l))
            l = x_null(r) ? xstrdup("0") : r;
    }
    return l;
}

int expr_main(int argc, char **argv)
{
    char *v;
    xv = argv + 1;
    xn = argc - 1;
    xi = 0;
    if (xn > 0 && !strcmp(xv[0], "--")) {
        xv++;
        xn--;
    }
    v = x_or();
    if (xi < xn)
        die("syntax error");
    puts(v);
    return x_null(v);
}

/* tr */
static int tr_set(const char *s, unsigned char *out)
{
    int n = 0, c, lo, i;
    static const char *const names[] = { "alpha", "digit", "alnum", "upper", "lower", "space", "blank", "punct",
                                         "xdigit", "cntrl", "print", "graph", 0 };
    while (*s && n < 4096) {
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (e) {
                for (i = 0; names[i]; i++)
                    if (strlen(names[i]) == (size_t)(e - s - 2) && !strncmp(names[i], s + 2, (size_t)(e - s - 2)))
                        break;
                for (c = 0; c < 256 && names[i] && n < 4096; c++) {
                    int in = 0;
                    switch (i) {
                    case 0: in = isalpha(c); break;
                    case 1: in = isdigit(c); break;
                    case 2: in = isalnum(c); break;
                    case 3: in = isupper(c); break;
                    case 4: in = islower(c); break;
                    case 5: in = isspace(c); break;
                    case 6: in = c == ' ' || c == '\t'; break;
                    case 7: in = ispunct(c); break;
                    case 8: in = isxdigit(c); break;
                    case 9: in = iscntrl(c); break;
                    case 10: in = isprint(c); break;
                    case 11: in = isgraph(c); break;
                    }
                    if (in)
                        out[n++] = (unsigned char)c;
                }
                s = e + 2;
                continue;
            }
        }
        c = (unsigned char)*s++;
        if (c == '\\' && *s) {
            c = (unsigned char)*s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'a': c = 7; break;
            case 'b': c = 8; break;
            case 'f': c = 12; break;
            case 'v': c = 11; break;
            default:
                if (c >= '0' && c <= '7') {
                    c -= '0';
                    for (i = 0; i < 2 && *s >= '0' && *s <= '7'; i++)
                        c = c * 8 + (*s++ - '0');
                }
            }
        }
        if (*s == '-' && s[1]) {
            lo = c;
            s++;
            c = (unsigned char)*s++;
            if (c == '\\' && *s) {
                c = (unsigned char)*s++;
                if (c == 'n')
                    c = '\n';
                else if (c == 't')
                    c = '\t';
                else if (c >= '0' && c <= '7') {
                    c -= '0';
                    for (i = 0; i < 2 && *s >= '0' && *s <= '7'; i++)
                        c = c * 8 + (*s++ - '0');
                }
            }
            for (; lo <= c && n < 4096; lo++)
                out[n++] = (unsigned char)lo;
        } else
            out[n++] = (unsigned char)c;
    }
    return n;
}

int tr_main(int argc, char **argv)
{
    static unsigned char s1[4096], s2[4096];
    int map[256], del[256], sq[256];
    int i = 1, dflag = 0, sflag = 0, cflag = 0, n1, n2 = 0, c, last = -1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'd')
                dflag = 1;
            else if (*p == 's')
                sflag = 1;
            else if (*p == 'c' || *p == 'C')
                cflag = 1;
        }
    }
    if (i >= argc)
        die("missing operand");
    n1 = tr_set(argv[i], s1);
    if (i + 1 < argc)
        n2 = tr_set(argv[i + 1], s2);
    if (cflag) {
        int in[256];
        memset(in, 0, sizeof in);
        for (c = 0; c < n1; c++)
            in[s1[c]] = 1;
        n1 = 0;
        for (c = 0; c < 256; c++)
            if (!in[c])
                s1[n1++] = (unsigned char)c;
    }
    for (c = 0; c < 256; c++) {
        map[c] = c;
        del[c] = sq[c] = 0;
    }
    if (dflag) {
        for (c = 0; c < n1; c++)
            del[s1[c]] = 1;
        for (c = 0; c < n2; c++)
            sq[s2[c]] = 1;
    } else {
        for (c = 0; c < n1; c++) {
            if (n2)
                map[s1[c]] = s2[c < n2 ? c : n2 - 1];
            if (sflag)
                sq[n2 ? map[s1[c]] : s1[c]] = 1;
        }
    }
    while ((c = getchar()) != EOF) {
        if (del[c])
            continue;
        c = map[c];
        if (sflag && sq[c] && c == last)
            continue;
        last = c;
        putchar(c);
    }
    return 0;
}

/* cut */
static int in_list(const char *list, long k)
{
    const char *p = list;
    while (*p) {
        long lo = 1, hi;
        char *end;
        if (*p != '-') {
            lo = strtol(p, &end, 10);
            p = end;
        }
        hi = lo;
        if (*p == '-') {
            p++;
            if (isdigit((unsigned char)*p)) {
                hi = strtol(p, &end, 10);
                p = end;
            } else
                hi = LONG_MAX;
        }
        if (k >= lo && k <= hi)
            return 1;
        if (*p == ',')
            p++;
        else if (*p)
            break;
    }
    return 0;
}

int cut_main(int argc, char **argv)
{
    int i, delim = '\t', mode = 0, only = 0, st = 0;
    const char *list = 0;
    Buf b;
    memset(&b, 0, sizeof b);
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        int o = argv[i][1];
        if (o == 'd')
            delim = (unsigned char)*optarg_of(argc, argv, &i, 2);
        else if (o == 'f' || o == 'c' || o == 'b') {
            mode = o;
            list = optarg_of(argc, argv, &i, 2);
        } else if (o == 's')
            only = 1;
        else
            die("bad option %s", argv[i]);
    }
    if (!list)
        die("missing list");
    do {
        FILE *f = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) {
            warn("%s: %s", argv[i], strerror(errno));
            st = 1;
            continue;
        }
        while (readline(f, &b)) {
            char *s = b.s, *e;
            long k;
            int first = 1;
            if (mode != 'f') {
                for (k = 1; *s; s++, k++)
                    if (in_list(list, k))
                        putchar(*s);
                putchar('\n');
                continue;
            }
            if (!strchr(s, delim)) {
                if (!only)
                    puts(s);
                continue;
            }
            for (k = 1;; k++) {
                e = strchr(s, delim);
                if (in_list(list, k)) {
                    if (!first)
                        putchar(delim);
                    first = 0;
                    fwrite(s, 1, e ? (size_t)(e - s) : strlen(s), stdout);
                }
                if (!e)
                    break;
                s = e + 1;
            }
            putchar('\n');
        }
        if (f != stdin)
            fclose(f);
    } while (++i < argc);
    return st;
}

/* sort */
static int s_rev, s_num, s_fold, s_key1, s_key2, s_sep, s_ver;

static const char *s_field(const char *s, int k, size_t *len)
{
    const char *e;
    int i;
    if (!k) {
        *len = strlen(s);
        return s;
    }
    for (i = 1;; i++) {
        if (s_sep) {
            e = strchr(s, s_sep);
            if (!e)
                e = s + strlen(s);
        } else {
            e = s;
            while (*e == ' ' || *e == '\t')
                e++;
            while (*e && *e != ' ' && *e != '\t')
                e++;
        }
        if (i == k)
            break;
        if (!*e) {
            *len = 0;
            return e;
        }
        s = s_sep ? e + 1 : e;
    }
    if (!s_sep)
        while (*s == ' ' || *s == '\t')
            s++;
    if (s_key2 == k)
        *len = (size_t)(e - s);
    else
        *len = strlen(s);
    return s;
}

static int vercmp(const char *a, const char *b)
{
    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
            if (x != y)
                return x < y ? -1 : 1;
            a = ea;
            b = eb;
        } else {
            if (*a != *b)
                return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    return 0;
}

static int s_cmp(const void *pa, const void *pb)
{
    const char *a = *(char *const *)pa, *b = *(char *const *)pb, *fa, *fb;
    size_t la, lb, n;
    int r = 0;
    fa = s_field(a, s_key1, &la);
    fb = s_field(b, s_key1, &lb);
    if (s_num) {
        double x = strtod(fa, 0), y = strtod(fb, 0);
        r = x < y ? -1 : x > y;
    } else if (s_ver)
        r = vercmp(fa, fb);
    else if (s_fold) {
        n = la < lb ? la : lb;
        r = strncasecmp(fa, fb, n);
        if (!r)
            r = la < lb ? -1 : la > lb;
    } else {
        n = la < lb ? la : lb;
        r = memcmp(fa, fb, n);
        if (!r)
            r = la < lb ? -1 : la > lb;
    }
    if (!r)
        r = strcmp(a, b);
    return s_rev ? -r : r;
}

int sort_main(int argc, char **argv)
{
    int i, uniq = 0, n = 0, cap = 0, j;
    const char *out = 0;
    char **lines = 0;
    Buf b;
    s_rev = s_num = s_fold = s_key1 = s_key2 = s_sep = s_ver = 0;
    memset(&b, 0, sizeof b);
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'r')
                s_rev = 1;
            else if (*p == 'n' || *p == 'g')
                s_num = 1;
            else if (*p == 'u')
                uniq = 1;
            else if (*p == 'f')
                s_fold = 1;
            else if (*p == 'V')
                s_ver = 1;
            else if (*p == 't') {
                s_sep = (unsigned char)*optarg_of(argc, argv, &i, (int)(p - argv[i]) + 1);
                break;
            } else if (*p == 'k') {
                const char *k = optarg_of(argc, argv, &i, (int)(p - argv[i]) + 1), *c;
                s_key1 = atoi(k);
                c = strchr(k, ',');
                s_key2 = c ? atoi(c + 1) : 0;
                if (strchr(k, 'n'))
                    s_num = 1;
                if (strchr(k, 'r'))
                    s_rev = 1;
                break;
            } else if (*p == 'o') {
                out = optarg_of(argc, argv, &i, (int)(p - argv[i]) + 1);
                break;
            } else if (*p != 'b' && *p != 's' && *p != 'd')
                die("bad option -%c", *p);
        }
    }
    do {
        FILE *f = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f)
            die("%s: %s", argv[i], strerror(errno));
        while (readline(f, &b)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                lines = xrealloc(lines, (size_t)cap * sizeof *lines);
            }
            lines[n++] = xstrndup(b.s, b.n);
        }
        if (f != stdin)
            fclose(f);
    } while (++i < argc);
    qsort(lines, (size_t)n, sizeof *lines, s_cmp);
    if (out && !freopen(out, "w", stdout))
        die("%s: %s", out, strerror(errno));
    for (j = 0; j < n; j++) {
        if (uniq && j) {
            size_t la, lb;
            const char *fa = s_field(lines[j - 1], s_key1, &la), *fb = s_field(lines[j], s_key1, &lb);
            if (s_key1 || s_num ? (s_num ? strtod(fa, 0) == strtod(fb, 0) : la == lb && !memcmp(fa, fb, la))
                                : !strcmp(lines[j - 1], lines[j]))
                continue;
        }
        puts(lines[j]);
    }
    return 0;
}

int uniq_main(int argc, char **argv)
{
    int i, count = 0, dup = 0, uniq = 0, have = 0;
    long n = 0;
    Buf b;
    char *prev = 0;
    FILE *f = stdin;
    memset(&b, 0, sizeof b);
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strchr(argv[i], 'c'))
            count = 1;
        if (strchr(argv[i], 'd'))
            dup = 1;
        if (strchr(argv[i], 'u'))
            uniq = 1;
    }
    if (i < argc && strcmp(argv[i], "-"))
        f = xfopen(argv[i], "r");
    if (i + 1 < argc && !freopen(argv[i + 1], "w", stdout))
        die("%s: %s", argv[i + 1], strerror(errno));
    for (;;) {
        int got = readline(f, &b);
        if (got && have && !strcmp(prev, b.s)) {
            n++;
            continue;
        }
        if (have && !(dup && n < 2) && !(uniq && n > 1)) {
            if (count)
                printf("%7ld %s\n", n, prev);
            else
                puts(prev);
        }
        if (!got)
            break;
        free(prev);
        prev = xstrndup(b.s, b.n);
        have = 1;
        n = 1;
    }
    return 0;
}

int od_main(int argc, char **argv)
{
    int i, fmt = 'o', width = 2, addr = 'o', c, k;
    long off = 0;
    unsigned char line[16];
    FILE *f = stdin;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (a[1] == 'A')
            addr = *optarg_of(argc, argv, &i, 2);
        else if (a[1] == 't') {
            const char *t = optarg_of(argc, argv, &i, 2);
            fmt = t[0];
            width = t[1] ? t[1] - '0' : (fmt == 'c' ? 1 : 2);
        } else {
            const char *p;
            for (p = a + 1; *p; p++) {
                if (*p == 'b')
                    fmt = 'o', width = 1;
                else if (*p == 'c')
                    fmt = 'c', width = 1;
                else if (*p == 'x')
                    fmt = 'x', width = 2;
                else if (*p == 'd')
                    fmt = 'u', width = 2;
            }
        }
    }
    if (i < argc && strcmp(argv[i], "-"))
        f = xfopen(argv[i], "r");
    if (width != 1 && width != 2 && width != 4)
        width = 1;
    for (;;) {
        int n = 0;
        while (n < 16 && (c = getc(f)) != EOF)
            line[n++] = (unsigned char)c;
        if (!n)
            break;
        if (addr != 'n')
            printf(addr == 'x' ? "%07lx" : addr == 'd' ? "%07ld" : "%07lo", off);
        for (k = 0; k < n; k += width) {
            unsigned long v = 0;
            int j;
            for (j = width - 1; j >= 0; j--)
                v = v << 8 | (k + j < n ? line[k + j] : 0);
            if (fmt == 'c') {
                if (v == '\n')
                    fputs("  \\n", stdout);
                else if (v == '\t')
                    fputs("  \\t", stdout);
                else if (v == 0)
                    fputs("  \\0", stdout);
                else if (isprint((int)v))
                    printf("   %c", (int)v);
                else
                    printf(" %03lo", v);
            } else if (fmt == 'x')
                printf(width == 1 ? " %02lx" : width == 2 ? " %04lx" : " %08lx", v);
            else if (fmt == 'u' || fmt == 'd')
                printf(width == 1 ? " %3lu" : width == 2 ? " %5lu" : " %10lu", v);
            else
                printf(width == 1 ? " %03lo" : width == 2 ? " %06lo" : " %011lo", v);
        }
        putchar('\n');
        off += n;
    }
    if (addr != 'n')
        printf(addr == 'x' ? "%07lx\n" : addr == 'd' ? "%07ld\n" : "%07lo\n", off);
    return 0;
}

int xargs_main(int argc, char **argv)
{
    int i, maxargs = 0, zero = 0, noempty = 0, base, n, c, st = 0, any = 0;
    const char *repl = 0;
    char **cmd = 0;
    size_t size = 0;
    Buf b;
    static char *echo[] = { (char *)"echo", 0 };
    memset(&b, 0, sizeof b);
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (a[1] == 'n')
            maxargs = atoi(optarg_of(argc, argv, &i, 2));
        else if (a[1] == '0')
            zero = 1;
        else if (a[1] == 'r')
            noempty = 1;
        else if (a[1] == 'I')
            repl = optarg_of(argc, argv, &i, 2);
        else if (a[1] == 's' || a[1] == 'P' || a[1] == 'L')
            optarg_of(argc, argv, &i, 2);
        else
            die("bad option %s", a);
    }
    if (i >= argc) {
        argv = echo;
        argc = 1;
        i = 0;
    }
    base = argc - i;
    cmd = xmalloc((size_t)(base + 2) * sizeof *cmd);
    memcpy(cmd, argv + i, (size_t)base * sizeof *cmd);
    n = base;
    for (;;) {
        int got = 0, quote = 0;
        b.n = 0;
        if (repl || zero) {
            while ((c = getchar()) != EOF && c != (zero ? 0 : '\n')) {
                bputc(&b, c);
                got = 1;
            }
            if (c != EOF)
                got = got || zero || repl;
            if (repl && !b.n && c != EOF)
                continue;
        } else {
            while ((c = getchar()) != EOF && isspace(c))
                ;
            for (; c != EOF; c = getchar()) {
                if (quote) {
                    if (c == quote)
                        quote = 0;
                    else
                        bputc(&b, c);
                } else if (c == '"' || c == '\'')
                    quote = c;
                else if (c == '\\') {
                    c = getchar();
                    if (c != EOF)
                        bputc(&b, c);
                } else if (isspace(c))
                    break;
                else
                    bputc(&b, c);
                got = 1;
            }
        }
        if (got && repl) {
            char **v = xmalloc((size_t)(base + 1) * sizeof *v);
            int k;
            for (k = 0; k < base; k++) {
                const char *s = argv[i + k], *h;
                Buf o;
                memset(&o, 0, sizeof o);
                while ((h = strstr(s, repl)) != 0) {
                    bput(&o, s, (size_t)(h - s));
                    bputs(&o, bstr(&b));
                    s = h + strlen(repl);
                }
                bputs(&o, s);
                v[k] = bstr(&o);
            }
            v[base] = 0;
            if (box_spawn(v))
                st = 123;
            any = 1;
            continue;
        }
        if (got) {
            cmd = xrealloc(cmd, (size_t)(n + 2) * sizeof *cmd);
            cmd[n++] = xstrndup(bstr(&b), b.n);
            size += b.n + 1;
        }
        if ((!got && (n > base || (!any && !noempty))) || (got && ((maxargs && n - base >= maxargs) || size > 100000))) {
            cmd[n] = 0;
            if (box_spawn(cmd))
                st = 123;
            any = 1;
            n = base;
            size = 0;
        }
        if (!got)
            break;
    }
    return st;
}

/* SHA-1 */
typedef struct Sha1 {
    unsigned int h[5];
    unsigned char buf[64];
    unsigned long len;
} Sha1;

#define ROL(x, n) ((((x) << (n)) | (((x) & 0xffffffffu) >> (32 - (n)))) & 0xffffffffu)

static void sha1_block(Sha1 *s, const unsigned char *p)
{
    unsigned int w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (unsigned)p[i * 4] << 24 | (unsigned)p[i * 4 + 1] << 16 | (unsigned)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (; i < 80; i++) {
        t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = ROL(t, 1);
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        t = (ROL(a, 5) + (f & 0xffffffffu) + e + k + w[i]) & 0xffffffffu;
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    s->h[0] = (s->h[0] + a) & 0xffffffffu;
    s->h[1] = (s->h[1] + b) & 0xffffffffu;
    s->h[2] = (s->h[2] + c) & 0xffffffffu;
    s->h[3] = (s->h[3] + d) & 0xffffffffu;
    s->h[4] = (s->h[4] + e) & 0xffffffffu;
}

static int sha1_file(const char *name)
{
    Sha1 s;
    FILE *f = strcmp(name, "-") ? fopen(name, "r") : stdin;
    size_t n, fill = 0;
    unsigned long bits;
    int i;
    if (!f) {
        warn("%s: %s", name, strerror(errno));
        return 1;
    }
    s.h[0] = 0x67452301u;
    s.h[1] = 0xefcdab89u;
    s.h[2] = 0x98badcfeu;
    s.h[3] = 0x10325476u;
    s.h[4] = 0xc3d2e1f0u;
    s.len = 0;
    while ((n = fread(s.buf + fill, 1, 64 - fill, f)) > 0) {
        s.len += n;
        fill += n;
        if (fill == 64) {
            sha1_block(&s, s.buf);
            fill = 0;
        }
    }
    if (f != stdin)
        fclose(f);
    bits = s.len * 8;
    s.buf[fill++] = 0x80;
    if (fill > 56) {
        memset(s.buf + fill, 0, 64 - fill);
        sha1_block(&s, s.buf);
        fill = 0;
    }
    memset(s.buf + fill, 0, 64 - fill);
    for (i = 0; i < 8; i++)
        s.buf[63 - i] = (unsigned char)(bits >> (8 * i));
    sha1_block(&s, s.buf);
    for (i = 0; i < 5; i++)
        printf("%08x", s.h[i]);
    printf("  %s\n", name);
    return 0;
}

int sha1sum_main(int argc, char **argv)
{
    int i = 1, st = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1])
        i++;
    if (i >= argc)
        return sha1_file("-");
    for (; i < argc; i++)
        st |= sha1_file(argv[i]);
    return st;
}
