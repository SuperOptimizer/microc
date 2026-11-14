/* find */
#include "box.h"

enum { F_TRUE, F_NAME, F_INAME, F_PATH, F_TYPE, F_NEWER, F_PRUNE, F_PRINT, F_PRINT0, F_PRINTF, F_EXEC, F_AND, F_OR,
       F_NOT, F_EMPTY, F_DELETE, F_QUIT, F_SIZE, F_MTIME, F_PERM, F_FALSE };

typedef struct Expr {
    int op;
    struct Expr *a, *b;
    const char *s;
    char **argv; /* -exec */
    int argc, plus;
    long n;
    int cmp; /* -1 less, 0 equal, 1 more */
    char **batch;
    int nbatch;
} Expr;

static char **fv;
static int fi_, fn_;
static int maxdepth = 1000000, mindepth, depthfirst, follow, hasaction, pruned, quit, fstatus;
static Expr *execs[32];
static int nexecs;

static Expr *f_or(void);

static Expr *f_new(int op, Expr *a, Expr *b)
{
    Expr *e = xmalloc(sizeof *e);
    memset(e, 0, sizeof *e);
    e->op = op;
    e->a = a;
    e->b = b;
    return e;
}

static const char *f_arg(const char *what)
{
    if (fi_ >= fn_)
        die("%s needs an argument", what);
    return fv[fi_++];
}

static void f_num(Expr *e, const char *s)
{
    e->cmp = *s == '+' ? 1 : *s == '-' ? -1 : 0;
    e->n = atol(*s == '+' || *s == '-' ? s + 1 : s);
    e->s = s;
}

static Expr *f_primary(void)
{
    const char *a;
    Expr *e;
    if (fi_ >= fn_)
        die("missing expression");
    a = fv[fi_++];
    if (!strcmp(a, "(")) {
        e = f_or();
        if (fi_ >= fn_ || strcmp(fv[fi_], ")"))
            die("missing )");
        fi_++;
        return e;
    }
    if (!strcmp(a, "!") || !strcmp(a, "-not"))
        return f_new(F_NOT, f_primary(), 0);
    if (!strcmp(a, "-name") || !strcmp(a, "-iname") || !strcmp(a, "-path") || !strcmp(a, "-wholename")) {
        e = f_new(a[1] == 'n' ? F_NAME : a[1] == 'i' ? F_INAME : F_PATH, 0, 0);
        e->s = f_arg(a);
        return e;
    }
    if (!strcmp(a, "-type")) {
        e = f_new(F_TYPE, 0, 0);
        e->s = f_arg(a);
        return e;
    }
    if (!strcmp(a, "-newer")) {
        struct stat st;
        e = f_new(F_NEWER, 0, 0);
        e->s = f_arg(a);
        if (stat(e->s, &st) < 0)
            die("%s: %s", e->s, strerror(errno));
        e->n = (long)st.st_mtime;
        return e;
    }
    if (!strcmp(a, "-size") || !strcmp(a, "-mtime") || !strcmp(a, "-perm")) {
        e = f_new(a[1] == 's' ? F_SIZE : a[1] == 'm' ? F_MTIME : F_PERM, 0, 0);
        f_num(e, f_arg(a));
        if (e->op == F_PERM)
            e->n = strtol(*e->s == '-' || *e->s == '/' || *e->s == '+' ? e->s + 1 : e->s, 0, 8);
        return e;
    }
    if (!strcmp(a, "-maxdepth")) {
        maxdepth = atoi(f_arg(a));
        return f_new(F_TRUE, 0, 0);
    }
    if (!strcmp(a, "-mindepth")) {
        mindepth = atoi(f_arg(a));
        return f_new(F_TRUE, 0, 0);
    }
    if (!strcmp(a, "-depth")) {
        depthfirst = 1;
        return f_new(F_TRUE, 0, 0);
    }
    if (!strcmp(a, "-follow")) {
        follow = 1;
        return f_new(F_TRUE, 0, 0);
    }
    if (!strcmp(a, "-xdev") || !strcmp(a, "-mount") || !strcmp(a, "-noleaf") || !strcmp(a, "-true"))
        return f_new(F_TRUE, 0, 0);
    if (!strcmp(a, "-false"))
        return f_new(F_FALSE, 0, 0);
    if (!strcmp(a, "-prune"))
        return f_new(F_PRUNE, 0, 0);
    if (!strcmp(a, "-empty"))
        return f_new(F_EMPTY, 0, 0);
    if (!strcmp(a, "-quit")) {
        return f_new(F_QUIT, 0, 0);
    }
    if (!strcmp(a, "-delete")) {
        hasaction = 1;
        depthfirst = 1;
        return f_new(F_DELETE, 0, 0);
    }
    if (!strcmp(a, "-print") || !strcmp(a, "-print0")) {
        hasaction = 1;
        return f_new(a[6] ? F_PRINT0 : F_PRINT, 0, 0);
    }
    if (!strcmp(a, "-printf")) {
        hasaction = 1;
        e = f_new(F_PRINTF, 0, 0);
        e->s = f_arg(a);
        return e;
    }
    if (!strcmp(a, "-exec")) {
        hasaction = 1;
        e = f_new(F_EXEC, 0, 0);
        e->argv = fv + fi_;
        while (fi_ < fn_ && strcmp(fv[fi_], ";") && !(!strcmp(fv[fi_], "+") && fi_ > 0 && !strcmp(fv[fi_ - 1], "{}"))) {
            fi_++;
            e->argc++;
        }
        if (fi_ >= fn_)
            die("-exec: missing ; or +");
        if (!strcmp(fv[fi_], "+")) {
            e->plus = 1;
            e->argc--;
            if (nexecs < 32)
                execs[nexecs++] = e;
        }
        fi_++;
        return e;
    }
    die("unknown primary %s", a);
    return 0;
}

static Expr *f_and(void)
{
    Expr *e = f_primary();
    while (fi_ < fn_ && strcmp(fv[fi_], "-o") && strcmp(fv[fi_], "-or") && strcmp(fv[fi_], ")")) {
        if (!strcmp(fv[fi_], "-a") || !strcmp(fv[fi_], "-and"))
            fi_++;
        e = f_new(F_AND, e, f_primary());
    }
    return e;
}

static Expr *f_or(void)
{
    Expr *e = f_and();
    while (fi_ < fn_ && (!strcmp(fv[fi_], "-o") || !strcmp(fv[fi_], "-or"))) {
        fi_++;
        e = f_new(F_OR, e, f_and());
    }
    return e;
}

static void run_batch(Expr *e)
{
    char **v;
    int i;
    if (!e->nbatch)
        return;
    v = xmalloc((size_t)(e->argc + e->nbatch + 1) * sizeof *v);
    memcpy(v, e->argv, (size_t)e->argc * sizeof *v);
    memcpy(v + e->argc, e->batch, (size_t)e->nbatch * sizeof *v);
    v[e->argc + e->nbatch] = 0;
    if (box_spawn(v))
        fstatus = 1;
    for (i = 0; i < e->nbatch; i++)
        free(e->batch[i]);
    free(v);
    e->nbatch = 0;
}

static const char *f_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static void f_printf(const char *fmt, const char *path, const char *start, struct stat *st)
{
    const char *p;
    for (p = fmt; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            putchar(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p == '0' ? 0 : *p);
        } else if (*p == '%' && p[1]) {
            p++;
            switch (*p) {
            case 'p': fputs(path, stdout); break;
            case 'f': fputs(f_base(path), stdout); break;
            case 'P': {
                size_t n = strlen(start);
                const char *r = path + (strlen(path) < n ? 0 : n);
                while (*r == '/')
                    r++;
                fputs(r, stdout);
                break;
            }
            case 'h': {
                const char *s = strrchr(path, '/');
                if (s)
                    fwrite(path, 1, (size_t)(s - path), stdout);
                else
                    putchar('.');
                break;
            }
            case 's': printf("%ld", (long)st->st_size); break;
            case 'm': printf("%o", (unsigned)(st->st_mode & 07777)); break;
            case 'y': putchar(S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : S_ISREG(st->st_mode) ? 'f' : 'o');
                break;
            case 'T': case 'A': case 'C':
                if (p[1] == '@') {
                    p++;
                    printf("%ld.0000000000", (long)st->st_mtime);
                }
                break;
            case '%': putchar('%'); break;
            default: putchar('%'); putchar(*p);
            }
        } else
            putchar(*p);
    }
}

static int f_eval(Expr *e, const char *path, const char *start, struct stat *st)
{
    switch (e->op) {
    case F_TRUE: return 1;
    case F_FALSE: return 0;
    case F_AND: return f_eval(e->a, path, start, st) && f_eval(e->b, path, start, st);
    case F_OR: return f_eval(e->a, path, start, st) || f_eval(e->b, path, start, st);
    case F_NOT: return !f_eval(e->a, path, start, st);
    case F_NAME: return fnmatch(e->s, f_base(path), 0) == 0;
    case F_INAME: {
        char *a = xstrdup(e->s), *b = xstrdup(f_base(path)), *p;
        int r;
        for (p = a; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        for (p = b; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        r = fnmatch(a, b, 0) == 0;
        free(a);
        free(b);
        return r;
    }
    case F_PATH: return fnmatch(e->s, path, 0) == 0;
    case F_TYPE:
        switch (e->s[0]) {
        case 'f': return S_ISREG(st->st_mode);
        case 'd': return S_ISDIR(st->st_mode);
        case 'l': return S_ISLNK(st->st_mode);
        case 'p': return S_ISFIFO(st->st_mode);
        case 'c': return S_ISCHR(st->st_mode);
        case 'b': return S_ISBLK(st->st_mode);
        case 's': return S_ISSOCK(st->st_mode);
        }
        return 0;
    case F_NEWER: return (long)st->st_mtime > e->n;
    case F_SIZE: {
        long unit = strchr(e->s, 'c') ? 1 : strchr(e->s, 'k') ? 1024 : strchr(e->s, 'M') ? 1024 * 1024 : 512;
        long sz = ((long)st->st_size + unit - 1) / unit;
        return e->cmp > 0 ? sz > e->n : e->cmp < 0 ? sz < e->n : sz == e->n;
    }
    case F_MTIME: {
        long days = ((long)time(0) - (long)st->st_mtime) / 86400;
        return e->cmp > 0 ? days > e->n : e->cmp < 0 ? days < e->n : days == e->n;
    }
    case F_PERM:
        if (*e->s == '-')
            return ((long)st->st_mode & e->n) == e->n;
        if (*e->s == '/' || *e->s == '+')
            return ((long)st->st_mode & e->n) != 0;
        return ((long)st->st_mode & 07777) == e->n;
    case F_EMPTY:
        if (S_ISREG(st->st_mode))
            return st->st_size == 0;
        if (S_ISDIR(st->st_mode)) {
            DIR *d = opendir(path);
            struct dirent *de;
            int empty = 1;
            while (d && (de = readdir(d)) != 0)
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    empty = 0;
            if (d)
                closedir(d);
            return empty;
        }
        return 0;
    case F_PRUNE:
        pruned = 1;
        return 1;
    case F_QUIT:
        quit = 1;
        return 1;
    case F_PRINT:
        puts(path);
        return 1;
    case F_PRINT0:
        fputs(path, stdout);
        putchar(0);
        return 1;
    case F_PRINTF:
        f_printf(e->s, path, start, st);
        return 1;
    case F_DELETE:
        if ((S_ISDIR(st->st_mode) ? rmdir(path) : unlink(path)) < 0) {
            warn("%s: %s", path, strerror(errno));
            fstatus = 1;
            return 0;
        }
        return 1;
    case F_EXEC: {
        char **v;
        int i, r;
        if (e->plus) {
            e->batch = xrealloc(e->batch, (size_t)(e->nbatch + 1) * sizeof *e->batch);
            e->batch[e->nbatch++] = xstrdup(path);
            if (e->nbatch >= 2000)
                run_batch(e);
            return 1;
        }
        v = xmalloc((size_t)(e->argc + 1) * sizeof *v);
        for (i = 0; i < e->argc; i++) {
            const char *s = e->argv[i], *h = strstr(s, "{}");
            if (h) {
                Buf b;
                memset(&b, 0, sizeof b);
                for (; h; s = h + 2, h = strstr(s, "{}")) {
                    bput(&b, s, (size_t)(h - s));
                    bputs(&b, path);
                }
                bputs(&b, s);
                v[i] = bstr(&b);
            } else
                v[i] = e->argv[i];
        }
        v[e->argc] = 0;
        r = box_spawn(v);
        free(v);
        return r == 0;
    }
    }
    return 0;
}

static int cmpn(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void f_walk(Expr *e, const char *path, const char *start, int depth)
{
    struct stat st;
    int isdir;
    if (quit)
        return;
    if ((follow ? stat(path, &st) : lstat(path, &st)) < 0 && lstat(path, &st) < 0) {
        warn("%s: %s", path, strerror(errno));
        fstatus = 1;
        return;
    }
    isdir = S_ISDIR(st.st_mode);
    pruned = 0;
    if (!depthfirst && depth >= mindepth)
        f_eval(e, path, start, &st);
    if (isdir && !pruned && depth < maxdepth && !quit) {
        DIR *d = opendir(path);
        struct dirent *de;
        char **names = 0;
        int n = 0, i;
        size_t plen = strlen(path);
        if (!d) {
            warn("%s: %s", path, strerror(errno));
            fstatus = 1;
        } else {
            while ((de = readdir(d)) != 0) {
                char *p;
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                p = xmalloc(plen + strlen(de->d_name) + 2);
                strcpy(p, path);
                if (plen && path[plen - 1] != '/')
                    strcat(p, "/");
                strcat(p, de->d_name);
                names = xrealloc(names, (size_t)(n + 1) * sizeof *names);
                names[n++] = p;
            }
            closedir(d);
            qsort(names, (size_t)n, sizeof *names, cmpn);
            for (i = 0; i < n; i++) {
                f_walk(e, names[i], start, depth + 1);
                free(names[i]);
            }
            free(names);
        }
    }
    if (depthfirst && depth >= mindepth)
        f_eval(e, path, start, &st);
}

int find_main(int argc, char **argv)
{
    int i, first, j;
    Expr *e;
    static char *dot[] = { (char *)".", 0 };
    maxdepth = 1000000;
    mindepth = depthfirst = follow = hasaction = quit = fstatus = nexecs = 0;
    for (i = 1; i < argc && (!strcmp(argv[i], "-L") || !strcmp(argv[i], "-H") || !strcmp(argv[i], "-P")); i++)
        if (argv[i][1] == 'L')
            follow = 1;
    first = i;
    while (i < argc && argv[i][0] != '-' && strcmp(argv[i], "!") && strcmp(argv[i], "("))
        i++;
    fv = argv;
    fi_ = i;
    fn_ = argc;
    e = fi_ < fn_ ? f_or() : f_new(F_TRUE, 0, 0);
    if (fi_ < fn_)
        die("unexpected %s", fv[fi_]);
    if (!hasaction)
        e = f_new(F_AND, e, f_new(F_PRINT, 0, 0));
    if (first == i)
        f_walk(e, dot[0], dot[0], 0);
    for (j = first; j < i; j++)
        f_walk(e, argv[j], argv[j], 0);
    for (j = 0; j < nexecs; j++)
        run_batch(execs[j]);
    return fstatus;
}
