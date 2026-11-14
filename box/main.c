/* mbox: dispatch on the name the program was run as, or on the first argument. */
#include "box.h"

const char *prog = "mbox";
char *box_self;

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(2);
    }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 16;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
}

void bputc(Buf *b, int c)
{
    char ch = (char)c;
    if (b->n + 1 < b->cap)
        b->s[b->n++] = ch;
    else
        bput(b, &ch, 1);
}

void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

char *bstr(Buf *b)
{
    if (b->n + 1 > b->cap) {
        b->cap = b->n + 16;
        b->s = xrealloc(b->s, b->cap);
    }
    b->s[b->n] = 0;
    return b->s;
}

void warn(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void die(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

int readline(FILE *f, Buf *b)
{
    int c;
    b->n = 0;
    while ((c = getc(f)) != EOF) {
        if (c == '\n') {
            bstr(b);
            return 1;
        }
        bputc(b, c);
    }
    bstr(b);
    return b->n > 0;
}

char *readall(int fd, size_t *len)
{
    Buf b;
    char tmp[16384];
    long n;
    memset(&b, 0, sizeof b);
    while ((n = read(fd, tmp, sizeof tmp)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            bput(&b, tmp, (size_t)n);
    if (len)
        *len = b.n;
    return bstr(&b);
}

char *readfile(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    char *s;
    if (fd < 0)
        return 0;
    s = readall(fd, len);
    close(fd);
    return s;
}

FILE *xfopen(const char *path, const char *mode)
{
    FILE *f;
    if (!strcmp(path, "-"))
        return *mode == 'r' ? stdin : stdout;
    f = fopen(path, mode);
    if (!f)
        die("%s: %s", path, strerror(errno));
    return f;
}

#include "applets.h"

applet_fn find_applet(const char *name)
{
    int i;
    for (i = 0; applets[i].name; i++)
        if (!strcmp(applets[i].name, name))
            return applets[i].fn;
    return 0;
}

const char *applet_name(int i) { return applets[i].name; }

int run_applet(applet_fn fn, int argc, char **argv)
{
    int st;
    prog = argv[0];
    st = fn(argc, argv);
    if (fflush(stdout) != 0 || ferror(stdout))
        st = st ? st : 1;
    return st;
}

char *path_search(const char *name, const char *path)
{
    const char *e;
    struct stat st;
    Buf b;
    if (strchr(name, '/'))
        return xstrdup(name);
    memset(&b, 0, sizeof b);
    for (; path; path = *e ? e + 1 : 0) {
        e = path + strcspn(path, ":");
        b.n = 0;
        if (e == path)
            bputc(&b, '.');
        else
            bput(&b, path, (size_t)(e - path));
        bputc(&b, '/');
        bputs(&b, name);
        if (stat(bstr(&b), &st) == 0 && S_ISREG(st.st_mode) && access(b.s, X_OK) == 0)
            return b.s;
    }
    free(b.s);
    return 0;
}

static void addarg(char ***v, int *n, char *s)
{
    *v = xrealloc(*v, (size_t)(*n + 2) * sizeof(char *));
    (*v)[(*n)++] = s;
    (*v)[*n] = 0;
}

/* Become the program: one of the tools here, a script for one of them, or a file
   found on PATH. Does not return. */
void box_exec(char **argv, char **env)
{
    char *path, head[256];
    const char *pathvar = "/usr/bin:/bin";
    applet_fn fn;
    int fd, argc = 0, i, n;
    long got;
    fflush(stdout);
    while (argv[argc])
        argc++;
    /* /bin/NAME and /usr/bin/NAME mean the tool here, when there is one */
    if (!strncmp(argv[0], "/bin/", 5) && !strchr(argv[0] + 5, '/') && find_applet(argv[0] + 5))
        argv[0] += 5;
    else if (!strncmp(argv[0], "/usr/bin/", 9) && !strchr(argv[0] + 9, '/') && find_applet(argv[0] + 9))
        argv[0] += 9;
    if (!strchr(argv[0], '/') && (fn = find_applet(argv[0])) != 0) {
        environ = env;
        exit(run_applet(fn, argc, argv));
    }
    for (i = 0; env[i]; i++)
        if (!strncmp(env[i], "PATH=", 5))
            pathvar = env[i] + 5;
    path = path_search(argv[0], pathvar);
    if (!path) {
        warn("%s: not found", argv[0]);
        exit(127);
    }
    /* a script whose interpreter is one of ours runs here, not through the system's */
    fd = open(path, O_RDONLY);
    got = fd >= 0 ? read(fd, head, sizeof head - 1) : 0;
    if (fd >= 0)
        close(fd);
    if (got > 2 && head[0] == '#' && head[1] == '!' && box_self) {
        char *interp, *arg, *base, **nv = 0;
        head[got] = 0;
        head[strcspn(head, "\n")] = 0;
        interp = head + 2;
        while (*interp == ' ' || *interp == '\t')
            interp++;
        arg = interp + strcspn(interp, " \t");
        if (*arg) {
            *arg++ = 0;
            while (*arg == ' ' || *arg == '\t')
                arg++;
            for (i = (int)strlen(arg); i > 0 && (arg[i - 1] == ' ' || arg[i - 1] == '\t'); i--)
                arg[i - 1] = 0;
        }
        base = strrchr(interp, '/');
        base = base ? base + 1 : interp;
        if (!strcmp(base, "bash") || !strcmp(base, "dash"))
            base = (char *)"sh";
        if (find_applet(base)) {
            n = 0;
            addarg(&nv, &n, base);
            if (*arg)
                addarg(&nv, &n, arg);
            addarg(&nv, &n, path);
            for (i = 1; i < argc; i++)
                addarg(&nv, &n, argv[i]);
            execve(box_self, nv, env);
        }
    }
    execve(path, argv, env);
    if (errno == ENOEXEC && box_self) {
        char **nv = 0;
        n = 0;
        addarg(&nv, &n, (char *)"sh");
        addarg(&nv, &n, path);
        for (i = 1; i < argc; i++)
            addarg(&nv, &n, argv[i]);
        execve(box_self, nv, env);
    }
    warn("%s: %s", argv[0], strerror(errno));
    exit(errno == ENOENT ? 127 : 126);
}

/* run a command and wait for it */
int box_spawn(char **argv)
{
    int pid, st = 0;
    fflush(stdout);
    pid = fork();
    if (pid < 0)
        return 126;
    if (pid == 0)
        box_exec(argv, environ);
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* where this executable is, for running scripts through it */
static char *self_path(const char *argv0)
{
    char cwd[4096];
    Buf b;
    const char *path, *e;
    memset(&b, 0, sizeof b);
    if (argv0[0] == '/')
        return xstrdup(argv0);
    if (strchr(argv0, '/')) {
        if (!getcwd(cwd, sizeof cwd))
            return 0;
        bputs(&b, cwd);
        bputc(&b, '/');
        bputs(&b, argv0);
        return bstr(&b);
    }
    path = getenv("PATH");
    for (; path; path = *e ? e + 1 : 0) {
        e = path + strcspn(path, ":");
        b.n = 0;
        if (e == path)
            bputc(&b, '.');
        else
            bput(&b, path, (size_t)(e - path));
        bputc(&b, '/');
        bputs(&b, argv0);
        if (access(bstr(&b), X_OK) == 0) {
            if (b.s[0] != '/' && getcwd(cwd, sizeof cwd)) {
                char *rel = xstrdup(b.s);
                b.n = 0;
                bputs(&b, cwd);
                bputc(&b, '/');
                bputs(&b, rel);
                free(rel);
            }
            return bstr(&b);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *name;
    applet_fn fn;
    int i, st;
    if (argc < 1)
        return 2;
    name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    box_self = self_path(argv[0]);
    fn = find_applet(name);
    if (!fn && argc > 1) {
        argv++;
        argc--;
        name = argv[0];
        fn = find_applet(name);
        if (!fn && !strcmp(name, "--list")) {
            for (i = 0; applet_name(i); i++)
                puts(applet_name(i));
            return 0;
        }
    }
    if (!fn) {
        fprintf(stderr, "mbox: %s: no such tool (try mbox --list)\n", name);
        return 127;
    }
    argv[0] = (char *)name; /* tools look at the name they were run by */
    st = run_applet(fn, argc, argv);
    return st;
}
