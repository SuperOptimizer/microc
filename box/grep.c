/* grep and diff */
#include "box.h"

typedef struct Pat {
    regex_t re;
    char *text;
} Pat;

static Pat *pats;
static int npats;
static int g_fixed, g_icase, g_word, g_line;

static void add_patterns(const char *s, int cflags)
{
    for (;;) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        pats = xrealloc(pats, (size_t)(npats + 1) * sizeof *pats);
        pats[npats].text = xstrndup(s, n);
        if (!g_fixed && regcomp(&pats[npats].re, pats[npats].text, cflags))
            die("bad regular expression: %s", pats[npats].text);
        npats++;
        if (!e)
            break;
        s = e + 1;
    }
}

static int isw(int c) { return isalnum(c) || c == '_'; }

/* find a match in the line at or after `from`; returns 1 and sets the span */
static int match_line(const char *line, size_t from, size_t *so, size_t *eo)
{
    int i, found = 0;
    size_t len = strlen(line);
    for (i = 0; i < npats; i++) {
        size_t at = from;
        for (;;) {
            size_t s, e;
            if (g_fixed) {
                const char *h = g_icase ? strcasestr(line + at, pats[i].text) : strstr(line + at, pats[i].text);
                if (!h)
                    break;
                s = (size_t)(h - line);
                e = s + strlen(pats[i].text);
            } else {
                regmatch_t m[1];
                if (regexec(&pats[i].re, line + at, 1, m, at ? REG_NOTBOL : 0))
                    break;
                s = at + (size_t)m[0].rm_so;
                e = at + (size_t)m[0].rm_eo;
            }
            if ((g_line && (s != 0 || e != len))
                || (g_word && ((s > 0 && isw((unsigned char)line[s - 1])) || (e < len && isw((unsigned char)line[e]))))) {
                if (g_line || s >= len)
                    break;
                at = s + 1;
                continue;
            }
            if (!found || s < *so) {
                *so = s;
                *eo = e;
            }
            found = 1;
            break;
        }
    }
    return found;
}

int grep_main(int argc, char **argv)
{
    int i, cflags = 0, invert = 0, quiet = 0, count = 0, list = 0, number = 0, noname = 0, withname = 0, silent = 0,
           only = 0, st = 1, havepat = 0, nfiles, lneg = 0;
    long max = -1;
    Buf b;
    memset(&b, 0, sizeof b);
    pats = 0;
    npats = 0;
    g_fixed = g_icase = g_word = g_line = 0;
    if (!strcmp(argv[0], "egrep"))
        cflags |= REG_EXTENDED;
    if (!strcmp(argv[0], "fgrep"))
        g_fixed = 1;
    /* options first, so that -E and -i apply to every pattern */
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        if (!strcmp(argv[i], "--")) {
            break;
        }
        if (argv[i][1] == '-')
            continue;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'E')
                cflags |= REG_EXTENDED;
            else if (*p == 'F')
                g_fixed = 1;
            else if (*p == 'i' || *p == 'y')
                g_icase = 1, cflags |= REG_ICASE;
            else if (*p == 'e' || *p == 'f' || *p == 'm' || *p == 'A' || *p == 'B' || *p == 'C') {
                if (!p[1])
                    i++;
                break;
            }
        }
    }
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p;
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (argv[i][1] == '-') {
            if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--silent"))
                quiet = 1;
            else if (strncmp(argv[i], "--color", 7) && strncmp(argv[i], "--colour", 8))
                die("bad option %s", argv[i]);
            continue;
        }
        for (p = argv[i] + 1; *p; p++) {
            const char *v;
            switch (*p) {
            case 'E': case 'F': case 'G': case 'i': case 'y': case 'a': case 'I': case 'U': break;
            case 'v': invert = 1; break;
            case 'q': quiet = 1; break;
            case 'c': count = 1; break;
            case 'l': list = 1; break;
            case 'L': list = 1; lneg = 1; break;
            case 'n': number = 1; break;
            case 'h': noname = 1; break;
            case 'H': withname = 1; break;
            case 's': silent = 1; break;
            case 'w': g_word = 1; break;
            case 'x': g_line = 1; break;
            case 'o': only = 1; break;
            case 'e': case 'f': case 'm': case 'A': case 'B': case 'C':
                v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
                if (!v)
                    die("option needs an argument");
                if (*p == 'e') {
                    add_patterns(v, cflags);
                    havepat = 1;
                } else if (*p == 'f') {
                    char *text = readfile(v, 0);
                    size_t n;
                    if (!text)
                        die("%s: %s", v, strerror(errno));
                    n = strlen(text);
                    if (n && text[n - 1] == '\n')
                        text[n - 1] = 0;
                    if (n > 1 || (n == 1 && text[0]))
                        add_patterns(text, cflags);
                    havepat = 1;
                } else if (*p == 'm')
                    max = atol(v);
                p = "x";
                break;
            default:
                die("bad option -%c", *p);
            }
        }
    }
    if (!havepat) {
        if (i >= argc)
            die("missing pattern");
        add_patterns(argv[i++], cflags);
    }
    nfiles = argc - i;
    do {
        const char *name = i < argc ? argv[i] : "(standard input)";
        FILE *f = i < argc && strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        long n = 0, lineno = 0;
        int show = (nfiles > 1 || withname) && !noname;
        if (!f) {
            if (!silent)
                warn("%s: %s", name, strerror(errno));
            st = st == 0 ? 0 : 2;
            continue;
        }
        while (readline(f, &b)) {
            size_t so = 0, eo = 0;
            int m = match_line(b.s, 0, &so, &eo);
            lineno++;
            if (m == invert)
                continue;
            n++;
            if (st)
                st = 0;
            if (quiet || list)
                break;
            if (!count && only && !invert) {
                size_t at = 0;
                while (at <= b.n && match_line(b.s, at, &so, &eo)) {
                    if (eo == so) {
                        at = eo + 1;
                        continue;
                    }
                    if (show)
                        printf("%s:", name);
                    if (number)
                        printf("%ld:", lineno);
                    fwrite(b.s + so, 1, eo - so, stdout);
                    putchar('\n');
                    at = eo;
                }
            } else if (!count) {
                if (show)
                    printf("%s:", name);
                if (number)
                    printf("%ld:", lineno);
                puts(b.s);
            }
            if (max >= 0 && n >= max)
                break;
        }
        if (f != stdin)
            fclose(f);
        if (count) {
            if (show)
                printf("%s:", name);
            printf("%ld\n", n);
        }
        if (list && (n > 0) != lneg)
            puts(name);
        if (quiet && n)
            return 0;
    } while (++i < argc);
    if (lneg)
        return 0;
    return st;
}

/* diff: the longest common subsequence by dynamic programming over the part of
   the files between their common beginning and end */

typedef struct Lines {
    char **v;
    int n;
    int nonl; /* last line has no newline */
} Lines;

static void load(const char *name, Lines *l, int newfile)
{
    size_t len, i, s;
    char *text = strcmp(name, "-") ? readfile(name, &len) : readall(0, &len);
    int cap = 0;
    l->v = 0;
    l->n = 0;
    l->nonl = 0;
    if (!text) {
        if (newfile && errno == ENOENT)
            return;
        fflush(stdout);
        fprintf(stderr, "diff: %s: %s\n", name, strerror(errno));
        exit(2);
    }
    for (i = s = 0; i <= len; i++) {
        if (i == len && s == len)
            break;
        if (i == len || text[i] == '\n') {
            if (i == len)
                l->nonl = 1;
            text[i] = 0;
            if (l->n == cap) {
                cap = cap ? cap * 2 : 256;
                l->v = xrealloc(l->v, (size_t)cap * sizeof *l->v);
            }
            l->v[l->n++] = text + s;
            s = i + 1;
        }
    }
}

static void range(int a, int b, const char *sep)
{
    if (b <= a + 1)
        printf("%d", b > a ? a + 1 : a);
    else
        printf("%d%s%d", a + 1, sep, b);
}

int diff_main(int argc, char **argv)
{
    Lines a, b;
    int i, brief = 0, unified = 0, ctx = 3, newfile = 0, pre, suf, n, m, x, y, differ = 0;
    int *lcs = 0;
    char *ea, *eb; /* edit marks: 1 = line is not common */
    const char *lab[2];
    int nlab = 0;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *p = argv[i];
        if (!strcmp(p, "-q") || !strcmp(p, "--brief"))
            brief = 1;
        else if (!strcmp(p, "-u"))
            unified = 1;
        else if (!strncmp(p, "-U", 2)) {
            unified = 1;
            ctx = atoi(p[2] ? p + 2 : argv[++i]);
        } else if (!strcmp(p, "-N"))
            newfile = 1;
        else if ((!strcmp(p, "-L") || !strcmp(p, "--label")) && i + 1 < argc) {
            if (nlab < 2)
                lab[nlab++] = argv[i + 1];
            i++;
        } else if (!strcmp(p, "-uN") || !strcmp(p, "-Nu") || !strcmp(p, "-Naur") || !strcmp(p, "-ur"))
            unified = newfile = 1;
        else if (strcmp(p, "-a") && strcmp(p, "-p") && strcmp(p, "-b") && strcmp(p, "-w") && strcmp(p, "-s"))
            die("bad option %s", p);
    }
    if (argc - i != 2)
        die("need two files");
    load(argv[i], &a, newfile);
    load(argv[i + 1], &b, newfile);
    for (pre = 0; pre < a.n && pre < b.n && !strcmp(a.v[pre], b.v[pre]); pre++)
        ;
    for (suf = 0; suf < a.n - pre && suf < b.n - pre && !strcmp(a.v[a.n - 1 - suf], b.v[b.n - 1 - suf]); suf++)
        ;
    n = a.n - pre - suf;
    m = b.n - pre - suf;
    if (!n && !m && a.nonl == b.nonl)
        return 0;
    if (brief) {
        printf("Files %s and %s differ\n", argv[i], argv[i + 1]);
        return 1;
    }
    ea = xmalloc((size_t)a.n + 1);
    eb = xmalloc((size_t)b.n + 1);
    memset(ea, 0, (size_t)a.n + 1);
    memset(eb, 0, (size_t)b.n + 1);
    memset(ea + pre, 1, (size_t)n);
    memset(eb + pre, 1, (size_t)m);
    if (a.nonl != b.nonl && !n && !m && a.n && b.n)
        ea[a.n - 1] = eb[b.n - 1] = 1;
    if (n && m && (double)n * (double)m <= 2.0e7) {
        int w = m + 1;
        lcs = xmalloc((size_t)(n + 1) * (size_t)w * sizeof *lcs);
        for (x = n; x >= 0; x--)
            for (y = m; y >= 0; y--) {
                if (x == n || y == m)
                    lcs[x * w + y] = 0;
                else if (!strcmp(a.v[pre + x], b.v[pre + y]))
                    lcs[x * w + y] = lcs[(x + 1) * w + y + 1] + 1;
                else
                    lcs[x * w + y] = lcs[(x + 1) * w + y] > lcs[x * w + y + 1] ? lcs[(x + 1) * w + y] : lcs[x * w + y + 1];
            }
        for (x = y = 0; x < n && y < m;) {
            if (!strcmp(a.v[pre + x], b.v[pre + y])) {
                ea[pre + x++] = 0;
                eb[pre + y++] = 0;
            } else if (lcs[(x + 1) * w + y] >= lcs[x * w + y + 1])
                x++;
            else
                y++;
        }
        free(lcs);
    }
    if (unified) {
        printf("--- %s\n+++ %s\n", nlab > 0 ? lab[0] : argv[i], nlab > 1 ? lab[1] : argv[i + 1]);
    }
    for (x = y = 0; x < a.n || y < b.n;) {
        int x0, y0, x1, y1;
        if (x < a.n && y < b.n && !ea[x] && !eb[y]) {
            x++;
            y++;
            continue;
        }
        differ = 1;
        x0 = x;
        y0 = y;
        if (!unified) {
            while (x < a.n && ea[x])
                x++;
            while (y < b.n && eb[y])
                y++;
            range(x0, x, ",");
            putchar(x == x0 ? 'a' : y == y0 ? 'd' : 'c');
            range(y0, y, ",");
            putchar('\n');
            for (x1 = x0; x1 < x; x1++)
                printf("< %s\n", a.v[x1]);
            if (x > x0 && y > y0)
                puts("---");
            for (y1 = y0; y1 < y; y1++)
                printf("> %s\n", b.v[y1]);
        } else {
            int sx = x0 - ctx < 0 ? 0 : x0 - ctx, sy = y0 - (x0 - sx), gap;
            /* extend the hunk while changes are within 2*ctx lines of each other */
            for (;;) {
                while ((x < a.n && ea[x]) || (y < b.n && eb[y])) {
                    if (x < a.n && ea[x])
                        x++;
                    else
                        y++;
                }
                for (gap = 0; x + gap < a.n && y + gap < b.n && !ea[x + gap] && !eb[y + gap] && gap <= 2 * ctx; gap++)
                    ;
                if (gap <= 2 * ctx && (x + gap < a.n || y + gap < b.n)) {
                    x += gap;
                    y += gap;
                    continue;
                }
                break;
            }
            x1 = x + ctx > a.n ? a.n : x + ctx;
            y1 = y + (x1 - x);
            if (y1 > b.n)
                y1 = b.n;
            printf("@@ -%d", x1 - sx ? sx + 1 : sx);
            if (x1 - sx != 1)
                printf(",%d", x1 - sx);
            printf(" +%d", y1 - sy ? sy + 1 : sy);
            if (y1 - sy != 1)
                printf(",%d", y1 - sy);
            puts(" @@");
            {
                int px = sx, py = sy;
                while (px < x1 || py < y1) {
                    if (px < x1 && ea[px])
                        printf("-%s\n", a.v[px++]);
                    else if (py < y1 && eb[py])
                        printf("+%s\n", b.v[py++]);
                    else {
                        printf(" %s\n", a.v[px]);
                        px++;
                        py++;
                    }
                }
            }
            x = x1;
            y = y1;
        }
    }
    if (a.nonl != b.nonl)
        differ = 1;
    return differ;
}
