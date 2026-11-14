/* The parts of POSIX and of common practice that sit on top of the system calls. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#ifdef __linux__
#include <mntent.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/wait.h>
#ifdef __linux__
#include <syslog.h>
#endif
#include <time.h>
#include <unistd.h>
#include "../../src/lock.h"
#ifdef __linux__
#include <arpa/inet.h>
#endif
#ifdef __linux__
#include <netdb.h>
#endif
#include <inttypes.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* ---- running programs ---- */

int execv(const char *path, char *const argv[]) { return execve(path, argv, environ); }

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    const char *path = getenv("PATH"), *p, *e;
    char buf[PATH_MAX];
    size_t n = strlen(file);
    int seen_eacces = 0;
    if (strchr(file, '/'))
        return execve(file, argv, envp);
    if (!path)
        path = "/usr/local/bin:/bin:/usr/bin";
    for (p = path;; p = e + 1) {
        size_t l;
        e = strchrnul(p, ':');
        l = (size_t)(e - p);
        if (l + n + 2 <= sizeof buf) {
            memcpy(buf, p, l);
            if (l)
                buf[l++] = '/';
            memcpy(buf + l, file, n + 1);
            execve(buf, argv, envp);
            if (errno == EACCES)
                seen_eacces = 1;
            else if (errno != ENOENT && errno != ENOTDIR)
                return -1;
        }
        if (!*e)
            break;
    }
    if (seen_eacces)
        errno = EACCES;
    return -1;
}

int execvp(const char *file, char *const argv[]) { return execvpe(file, argv, environ); }

#define MAXARGS 256
int execl(const char *path, const char *arg, ...)
{
    char *argv[MAXARGS];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    argv[n++] = (char *)arg;
    while (n < MAXARGS && argv[n - 1])
        argv[n++] = va_arg(ap, char *);
    va_end(ap);
    return execv(path, argv);
}

int execlp(const char *file, const char *arg, ...)
{
    char *argv[MAXARGS];
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    argv[n++] = (char *)arg;
    while (n < MAXARGS && argv[n - 1])
        argv[n++] = va_arg(ap, char *);
    va_end(ap);
    return execvp(file, argv);
}

int execle(const char *path, const char *arg, ...)
{
    char *argv[MAXARGS];
    char **envp;
    int n = 0;
    va_list ap;
    va_start(ap, arg);
    argv[n++] = (char *)arg;
    while (n < MAXARGS && argv[n - 1])
        argv[n++] = va_arg(ap, char *);
    envp = va_arg(ap, char **);
    va_end(ap);
    return execve(path, argv, envp);
}

int system(const char *cmd)
{
    pid_t pid;
    int status;
    if (!cmd)
        return 1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
#ifdef __MCC_POSIX__
        execlp("sh", "sh", "-c", cmd, (char *)0); /* the shell on PATH: the toolchain's own when it leads */
#endif
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}

int daemon(int nochdir, int noclose)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    setsid();
    if (!nochdir)
        chdir("/");
    if (!noclose) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
    }
    return 0;
}

/* ---- environment ---- */

static char **own_env; /* the array, once this library has had to make its own */

static int env_find(const char *name, size_t n)
{
    int i;
    if (!environ)
        return -1;
    for (i = 0; environ[i]; i++)
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=')
            return i;
    return -1;
}

static int env_put(char *s, size_t n, int overwrite)
{
    int i, count = 0;
    char **e;
    LOCK(__env_lock);
    i = env_find(s, n);
    if (i >= 0) {
        if (overwrite)
            environ[i] = s;
        UNLOCK(__env_lock);
        return 0;
    }
    while (environ && environ[count])
        count++;
    e = (char **)malloc(((size_t)count + 2) * sizeof(char *));
    if (!e) {
        UNLOCK(__env_lock);
        return -1;
    }
    if (count)
        memcpy(e, environ, (size_t)count * sizeof(char *));
    e[count] = s;
    e[count + 1] = 0;
    if (environ == own_env)
        free(own_env);
    environ = own_env = e;
    UNLOCK(__env_lock);
    return 0;
}

int putenv(char *s)
{
    char *eq = strchr(s, '=');
    if (!eq)
        return unsetenv(s);
    return env_put(s, (size_t)(eq - s), 1);
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t n = strlen(name), m = strlen(value);
    char *s;
    if (!n || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && env_find(name, n) >= 0)
        return 0;
    s = (char *)malloc(n + m + 2);
    if (!s)
        return -1;
    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value, m + 1);
    return env_put(s, n, 1);
}

int unsetenv(const char *name)
{
    size_t n = strlen(name);
    int i;
    LOCK(__env_lock);
    while ((i = env_find(name, n)) >= 0)
        for (; environ[i]; i++)
            environ[i] = environ[i + 1];
    UNLOCK(__env_lock);
    return 0;
}

int clearenv(void)
{
    static char *none[1];
    environ = none;
    none[0] = 0;
    return 0;
}

/* ---- options ---- */

char *optarg;
int optind = 1, opterr = 1, optopt;
static int optpos; /* within a group of letters */

static void rotate(char *const argv[], int from, int to)
{
    /* argv[to] moves down to argv[from]; what was between moves up one */
    char **v = (char **)argv, *t = v[to];
    int i;
    for (i = to; i > from; i--)
        v[i] = v[i - 1];
    v[from] = t;
}

static int getopt_inner(int argc, char *const argv[], const char *opts, const struct option *lo, int *idx, int only)
{
    int stop_first = 0, skipped, i, c;
    const char *o, *arg;
    if (optind == 0) {
        optind = 1;
        optpos = 0;
    }
    if (*opts == '+' || *opts == '-') {
        stop_first = 1;
        opts++;
    }
    if (getenv("POSIXLY_CORRECT"))
        stop_first = 1;
    optarg = 0;
    if (!optpos) {
        /* find the next option, moving it before the arguments that are not options */
        i = optind;
        if (!stop_first)
            while (i < argc && (argv[i][0] != '-' || !argv[i][1]))
                i++;
        if (i >= argc || argv[i][0] != '-' || !argv[i][1])
            return -1;
        skipped = i - optind;
        if (strcmp(argv[i], "--") == 0) {
            rotate(argv, optind, i);
            optind++;
            return -1;
        }
        arg = argv[i];
        if (lo && (arg[1] == '-' || (only && arg[1]))) {
            const char *name = arg + (arg[1] == '-' ? 2 : 1), *eq = strchr(name, '=');
            size_t n = eq ? (size_t)(eq - name) : strlen(name);
            int k, match = -1, exact = 0, count = 0;
            for (k = 0; lo[k].name; k++)
                if (strncmp(lo[k].name, name, n) == 0) {
                    if (strlen(lo[k].name) == n) {
                        match = k;
                        exact = 1;
                        break;
                    }
                    match = k;
                    count++;
                }
            if (match >= 0 && (exact || count == 1)) {
                int used = 1;
                rotate(argv, optind, i);
                if (lo[match].has_arg) {
                    if (eq)
                        optarg = (char *)eq + 1;
                    else if (lo[match].has_arg == 1) {
                        if (i + 1 < argc) {
                            rotate(argv, optind + 1, i + 1);
                            optarg = argv[optind + 1];
                            used = 2;
                        } else {
                            optind += 1;
                            optopt = lo[match].val;
                            if (opterr && *opts != ':')
                                fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], lo[match].name);
                            return *opts == ':' ? ':' : '?';
                        }
                    }
                } else if (eq) {
                    optind += 1;
                    if (opterr && *opts != ':')
                        fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], lo[match].name);
                    return '?';
                }
                optind += used;
                if (idx)
                    *idx = match;
                if (lo[match].flag) {
                    *lo[match].flag = lo[match].val;
                    return 0;
                }
                return lo[match].val;
            }
            if (arg[1] == '-') {
                rotate(argv, optind, i);
                optind++;
                optopt = 0;
                if (opterr && *opts != ':')
                    fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
                return '?';
            }
        }
        rotate(argv, optind, i);
        (void)skipped;
        optpos = 1;
    }
    arg = argv[optind];
    c = (unsigned char)arg[optpos++];
    o = c == ':' ? 0 : strchr(opts, c);
    if (!o) {
        optopt = c;
        if (opterr && *opts != ':')
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
        if (!arg[optpos]) {
            optind++;
            optpos = 0;
        }
        return '?';
    }
    if (o[1] == ':') {
        if (arg[optpos]) {
            optarg = (char *)arg + optpos;
            optind++;
        } else if (o[2] == ':') {
            optind++; /* an optional argument must be attached */
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
        } else {
            optind++;
            optpos = 0;
            optopt = c;
            if (opterr && *opts != ':')
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
            return *opts == ':' ? ':' : '?';
        }
        optpos = 0;
    } else if (!arg[optpos]) {
        optind++;
        optpos = 0;
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *opts) { return getopt_inner(argc, argv, opts, 0, 0, 0); }
int getopt_long(int argc, char *const argv[], const char *opts, const struct option *lo, int *idx)
{
    return getopt_inner(argc, argv, opts, lo, idx, 0);
}
int getopt_long_only(int argc, char *const argv[], const char *opts, const struct option *lo, int *idx)
{
    return getopt_inner(argc, argv, opts, lo, idx, 1);
}

/* ---- strings ---- */

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    return p ? (char *)memcpy(p, s, n) : 0;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

char *strndup(const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    char *p = (char *)malloc(l + 1);
    if (!p)
        return 0;
    memcpy(p, s, l);
    p[l] = 0;
    return p;
}

char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src++) != 0)
        dst++;
    return dst;
}

char *stpncpy(char *dst, const char *src, size_t n)
{
    size_t l = strnlen(src, n);
    memcpy(dst, src, l);
    memset(dst + l, 0, n - l);
    return dst + l;
}

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c)
            return (void *)p;
    return 0;
}

void *mempcpy(void *dst, const void *src, size_t n) { return (char *)memcpy(dst, src, n) + n; }

void *memccpy(void *dst, const void *src, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        if ((*d++ = *s++) == (unsigned char)c)
            return d;
    return 0;
}

void *memmem(const void *h, size_t hn, const void *n, size_t nn)
{
    const char *p = (const char *)h;
    if (!nn)
        return (void *)h;
    for (; hn >= nn; hn--, p++)
        if (*p == *(const char *)n && memcmp(p, n, nn) == 0)
            return (void *)p;
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; a++, b++, n--) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

char *strcasestr(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    for (; *s; s++)
        if (strncasecmp(s, sub, n) == 0)
            return (char *)s;
    return n ? 0 : (char *)s;
}

char *strtok_r(char *s, const char *sep, char **save)
{
    char *tok;
    if (!s)
        s = *save;
    s += strspn(s, sep);
    if (!*s) {
        *save = s;
        return 0;
    }
    tok = s;
    s += strcspn(s, sep);
    if (*s)
        *s++ = 0;
    *save = s;
    return tok;
}

char *strsep(char **s, const char *sep)
{
    char *tok = *s, *e;
    if (!tok)
        return 0;
    e = tok + strcspn(tok, sep);
    if (*e) {
        *e = 0;
        *s = e + 1;
    } else {
        *s = 0;
    }
    return tok;
}

size_t strlcpy(char *dst, const char *src, size_t n)
{
    size_t l = strlen(src);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return l;
}

int strverscmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++)
        ;
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
        long x = strtol(a, 0, 10), y = strtol(b, 0, 10);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void bzero(void *p, size_t n) { memset(p, 0, n); }
void bcopy(const void *from, void *to, size_t n) { memmove(to, from, n); }
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }
int ffs(int v)
{
    int n = 1;
    if (!v)
        return 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
}

int isblank(int c) { return c == ' ' || c == '\t'; }
int isascii(int c) { return (unsigned)c < 128; }
int toascii(int c) { return c & 127; }

static const char *const errs[] = {
    "Success", "Operation not permitted", "No such file or directory", "No such process", "Interrupted system call",
    "Input/output error", "No such device or address", "Argument list too long", "Exec format error",
    "Bad file descriptor", "No child processes", "Resource temporarily unavailable", "Cannot allocate memory",
    "Permission denied", "Bad address", "Block device required", "Device or resource busy", "File exists",
    "Invalid cross-device link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate ioctl for device", "Text file busy",
    "File too large", "No space left on device", "Illegal seek", "Read-only file system", "Too many links",
    "Broken pipe", "Numerical argument out of domain", "Numerical result out of range",
    "Resource deadlock avoided", "File name too long", "No locks available", "Function not implemented",
    "Directory not empty", "Too many levels of symbolic links"};

char *strerror(int err)
{
    static char buf[32];
    if (err >= 0 && err < (int)(sizeof errs / sizeof errs[0]))
        return (char *)errs[err];
    switch (err) {
    case ENODATA: return "No data available";
    case EOVERFLOW: return "Value too large for defined data type";
    case EILSEQ: return "Invalid or incomplete multibyte or wide character";
    case ENOTSOCK: return "Socket operation on non-socket";
    case EOPNOTSUPP: return "Operation not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol";
    case EADDRINUSE: return "Address already in use";
    case ENETUNREACH: return "Network is unreachable";
    case ECONNRESET: return "Connection reset by peer";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    case ENOMEDIUM: return "No medium found";
    }
    sprintf(buf, "Unknown error %d", err);
    return buf;
}

const char *const sys_siglist[NSIG] = {
    "Signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap", "Aborted",
    "Bus error", "Floating point exception", "Killed", "User defined signal 1", "Segmentation fault",
    "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated", "Stack fault", "Child exited",
    "Continued", "Stopped (signal)", "Stopped", "Stopped (tty input)", "Stopped (tty output)",
    "Urgent I/O condition", "CPU time limit exceeded", "File size limit exceeded", "Virtual timer expired",
    "Profiling timer expired", "Window changed", "I/O possible", "Power failure", "Bad system call"};

char *strsignal(int sig)
{
    static char buf[32];
    if (sig > 0 && sig < 32)
        return (char *)sys_siglist[sig];
    sprintf(buf, "Real-time signal %d", sig);
    return buf;
}

void psignal(int sig, const char *msg)
{
    if (msg && *msg)
        fprintf(stderr, "%s: ", msg);
    fprintf(stderr, "%s\n", strsignal(sig));
}

/* ---- numbers ---- */

long long strtoll(const char *s, char **end, int base) { return strtol(s, end, base); }
unsigned long long strtoull(const char *s, char **end, int base) { return strtoul(s, end, base); }
intmax_t strtoimax(const char *s, char **end, int base) { return strtol(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoul(s, end, base); }
long long atoll(const char *s) { return strtol(s, 0, 10); }
long long llabs(long long x) { return x < 0 ? -x : x; }
float strtof(const char *s, char **end) { return (float)strtod(s, end); }
long random(void) { return rand(); }
void srandom(unsigned seed) { srand(seed); }

unsigned short htons(unsigned short v) { return (unsigned short)(v << 8 | v >> 8); }
unsigned short ntohs(unsigned short v) { return htons(v); }
unsigned int htonl(unsigned int v) { return __builtin_bswap32(v); }
unsigned int ntohl(unsigned int v) { return __builtin_bswap32(v); }

/* ---- paths ---- */

char *basename(char *path)
{
    char *e;
    if (!path || !*path)
        return ".";
    e = path + strlen(path) - 1;
    while (e > path && *e == '/')
        *e-- = 0;
    if (e == path && *e == '/')
        return path;
    while (e > path && e[-1] != '/')
        e--;
    return e;
}

char *dirname(char *path)
{
    char *e;
    if (!path || !*path || !strchr(path, '/'))
        return ".";
    e = path + strlen(path) - 1;
    while (e > path && *e == '/')
        e--;
    while (e > path && *e != '/')
        e--;
    while (e > path && *e == '/')
        e--;
    if (e == path && *e != '/')
        return ".";
    e[1] = 0;
    return path;
}

char *realpath(const char *path, char *out)
{
    char work[PATH_MAX], res[PATH_MAX], link[PATH_MAX];
    size_t rl = 0;
    int loops = 0;
    char *p;
    if (!path || !*path) {
        errno = path ? ENOENT : EINVAL;
        return 0;
    }
    if (strlen(path) >= sizeof work) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (path[0] != '/') {
        if (!getcwd(res, sizeof res))
            return 0;
        rl = strlen(res);
        if (rl == 1)
            rl = 0;
    }
    strcpy(work, path);
    p = work;
    while (*p) {
        char *e;
        size_t n;
        struct stat st;
        while (*p == '/')
            p++;
        e = strchrnul(p, '/');
        n = (size_t)(e - p);
        if (n == 0)
            break;
        if (n == 1 && p[0] == '.') {
            p = e;
            continue;
        }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (rl > 0 && res[--rl] != '/')
                ;
            p = e;
            continue;
        }
        if (rl + n + 2 > sizeof res) {
            errno = ENAMETOOLONG;
            return 0;
        }
        res[rl] = '/';
        memcpy(res + rl + 1, p, n);
        res[rl + 1 + n] = 0;
        if (lstat(res, &st) < 0)
            return 0;
        if (S_ISLNK(st.st_mode)) {
            long l = readlink(res, link, sizeof link - 1);
            size_t rest = strlen(e);
            if (l < 0)
                return 0;
            if (++loops > 40) {
                errno = ELOOP;
                return 0;
            }
            if ((size_t)l + rest + 1 > sizeof work) {
                errno = ENAMETOOLONG;
                return 0;
            }
            memmove(link + l, e, rest + 1);
            strcpy(work, link);
            p = work;
            if (work[0] == '/')
                rl = 0;
            continue;
        }
        rl += n + 1;
        p = e;
    }
    if (rl == 0)
        res[rl++] = '/';
    res[rl] = 0;
    if (!out)
        return strdup(res);
    return strcpy(out, res);
}

static void fill_random(char *x)
{
    static unsigned long state;
    struct timespec ts;
    int i;
    clock_gettime(CLOCK_REALTIME, &ts);
    state += (unsigned long)ts.tv_nsec * 65537UL ^ (unsigned long)getpid() ^ (unsigned long)ts.tv_sec << 20;
    for (i = 0; i < 6; i++) {
        state = state * 6364136223846793005UL + 1442695040888963407UL;
        x[i] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[(state >> 33) % 62];
    }
}

int mkstemp(char *tmpl)
{
    size_t n = strlen(tmpl);
    int tries, fd;
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (tries = 0; tries < 100; tries++) {
        fill_random(tmpl + n - 6);
        fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

char *mkdtemp(char *tmpl)
{
    size_t n = strlen(tmpl);
    int tries;
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return 0;
    }
    for (tries = 0; tries < 100; tries++) {
        fill_random(tmpl + n - 6);
        if (mkdir(tmpl, 0700) == 0)
            return tmpl;
        if (errno != EEXIST)
            return 0;
    }
    return 0;
}

char *mktemp(char *tmpl)
{
    size_t n = strlen(tmpl);
    if (n >= 6)
        fill_random(tmpl + n - 6);
    return tmpl;
}

#ifdef __linux__
int ttyname_r(int fd, char *buf, size_t n)
{
    char proc[40];
    long l;
    if (!isatty(fd))
        return ENOTTY;
    sprintf(proc, "/proc/self/fd/%d", fd);
    l = readlink(proc, buf, n - 1);
    if (l < 0)
        return errno;
    buf[l] = 0;
    return 0;
}

char *ttyname(int fd)
{
    static char buf[64];
    return ttyname_r(fd, buf, sizeof buf) ? 0 : buf;
}

#endif

char *getlogin(void) { return getenv("LOGNAME"); }
int getpagesize(void) { return 4096; }
int getdtablesize(void) { return 1024; }

#ifdef __linux__
long sysconf(int name)
{
    struct sysinfo si;
    switch (name) {
    case _SC_ARG_MAX: return 131072;
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return 1024;
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        /* the processors this process may run on */
        unsigned long set[16];
        long n = syscall(SYS_sched_getaffinity, 0L, (long)sizeof set, set, 0L, 0L, 0L), k, count = 0;
        for (k = 0; k < n * 8; k++)
            count += (long)(set[k / 64] >> (k % 64) & 1);
        return count > 0 ? count : 1;
    }
    case _SC_PHYS_PAGES:
        if (sysinfo(&si) == 0)
            return (long)(si.totalram * si.mem_unit / 4096);
        return -1;
    }
    errno = EINVAL;
    return -1;
}

#endif

long pathconf(const char *path, int name)
{
    (void)path;
    return name == _PC_PATH_MAX ? PATH_MAX : name == _PC_NAME_MAX ? NAME_MAX : -1;
}
long fpathconf(int fd, int name)
{
    (void)fd;
    return pathconf("", name);
}
int get_nprocs(void) { return 1; }

/* ---- time ---- */

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;
void tzset(void) {}

struct tm *__gmtime_r(const time_t *t, struct tm *out);

struct tm *gmtime_r(const time_t *t, struct tm *out) { return __gmtime_r(t, out); }
struct tm *localtime_r(const time_t *t, struct tm *out) { return __gmtime_r(t, out); }

time_t timegm(struct tm *tm) { return mktime(tm); }

static int two(const char **s, int max_digits, int *out)
{
    int v = 0, n = 0;
    while (**s == ' ')
        (*s)++;
    while (n < max_digits && **s >= '0' && **s <= '9') {
        v = v * 10 + (*(*s)++ - '0');
        n++;
    }
    *out = v;
    return n > 0;
}

static int name_of(const char **s, const char *names, int count)
{
    /* names: three-letter abbreviations run together; a full name may follow the abbreviation */
    int i;
    for (i = 0; i < count; i++)
        if (strncasecmp(*s, names + 3 * i, 3) == 0) {
            *s += 3;
            while (isalpha((unsigned char)**s))
                (*s)++;
            return i;
        }
    return -1;
}

char *strptime(const char *s, const char *fmt, struct tm *tm)
{
    int v;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s))
                s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s++ != *fmt)
                return 0;
            continue;
        }
        fmt++;
        if (*fmt == 'E' || *fmt == 'O')
            fmt++;
        switch (*fmt) {
        case '%':
            if (*s++ != '%')
                return 0;
            break;
        case 'a': case 'A':
            if ((v = name_of(&s, "SunMonTueWedThuFriSat", 7)) < 0)
                return 0;
            tm->tm_wday = v;
            break;
        case 'b': case 'B': case 'h':
            if ((v = name_of(&s, "JanFebMarAprMayJunJulAugSepOctNovDec", 12)) < 0)
                return 0;
            tm->tm_mon = v;
            break;
        case 'd': case 'e':
            if (!two(&s, 2, &tm->tm_mday))
                return 0;
            break;
        case 'H': case 'k':
            if (!two(&s, 2, &tm->tm_hour))
                return 0;
            break;
        case 'I': case 'l':
            if (!two(&s, 2, &tm->tm_hour))
                return 0;
            break;
        case 'j':
            if (!two(&s, 3, &v))
                return 0;
            tm->tm_yday = v - 1;
            break;
        case 'm':
            if (!two(&s, 2, &v))
                return 0;
            tm->tm_mon = v - 1;
            break;
        case 'M':
            if (!two(&s, 2, &tm->tm_min))
                return 0;
            break;
        case 'S':
            if (!two(&s, 2, &tm->tm_sec))
                return 0;
            break;
        case 'y':
            if (!two(&s, 2, &v))
                return 0;
            tm->tm_year = v < 69 ? v + 100 : v;
            break;
        case 'Y':
            if (!two(&s, 4, &v))
                return 0;
            tm->tm_year = v - 1900;
            break;
        case 'p':
            if (strncasecmp(s, "pm", 2) == 0) {
                if (tm->tm_hour < 12)
                    tm->tm_hour += 12;
            } else if (strncasecmp(s, "am", 2) == 0) {
                if (tm->tm_hour == 12)
                    tm->tm_hour = 0;
            } else
                return 0;
            s += 2;
            break;
        case 's': {
            char *e;
            time_t t = strtol(s, &e, 10);
            if (e == s)
                return 0;
            gmtime_r(&t, tm);
            s = e;
            break;
        }
        case 'T':
            s = strptime(s, "%H:%M:%S", tm);
            if (!s)
                return 0;
            break;
        case 'R':
            s = strptime(s, "%H:%M", tm);
            if (!s)
                return 0;
            break;
        case 'D':
            s = strptime(s, "%m/%d/%y", tm);
            if (!s)
                return 0;
            break;
        case 'F':
            s = strptime(s, "%Y-%m-%d", tm);
            if (!s)
                return 0;
            break;
        case 'n': case 't':
            while (isspace((unsigned char)*s))
                s++;
            break;
        case 'Z':
            while (isalpha((unsigned char)*s))
                s++;
            break;
        case 'z':
            if (*s == '+' || *s == '-')
                s++;
            while (isdigit((unsigned char)*s) || *s == ':')
                s++;
            break;
        default:
            return 0;
        }
    }
    return (char *)s;
}

#ifdef __linux__
/* ---- mounted file systems ---- */

FILE *setmntent(const char *path, const char *mode) { return fopen(path, mode); }
int endmntent(FILE *f)
{
    if (f)
        fclose(f);
    return 1;
}

static char *unescape(char *s)
{
    char *r = s, *w = s;
    for (; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return s;
}

struct mntent *getmntent_r(FILE *f, struct mntent *m, char *buf, int n)
{
    static char none[] = "";
    while (fgets(buf, n, f)) {
        char *save, *t, *p = buf + strspn(buf, " \t");
        if (*p == '#' || *p == '\n' || !*p)
            continue;
        m->mnt_fsname = strtok_r(p, " \t\n", &save);
        m->mnt_dir = strtok_r(0, " \t\n", &save);
        m->mnt_type = strtok_r(0, " \t\n", &save);
        m->mnt_opts = strtok_r(0, " \t\n", &save);
        if (!m->mnt_fsname || !m->mnt_dir)
            continue;
        unescape(m->mnt_fsname);
        unescape(m->mnt_dir);
        if (!m->mnt_type)
            m->mnt_type = none;
        if (!m->mnt_opts)
            m->mnt_opts = none;
        t = m->mnt_opts != none ? strtok_r(0, " \t\n", &save) : 0;
        m->mnt_freq = t ? atoi(t) : 0;
        t = t ? strtok_r(0, " \t\n", &save) : 0;
        m->mnt_passno = t ? atoi(t) : 0;
        return m;
    }
    return 0;
}

struct mntent *getmntent(FILE *f)
{
    static struct mntent m;
    static char buf[1024];
    return getmntent_r(f, &m, buf, sizeof buf);
}

int addmntent(FILE *f, const struct mntent *m)
{
    return fprintf(f, "%s %s %s %s %d %d\n", m->mnt_fsname, m->mnt_dir, m->mnt_type, m->mnt_opts, m->mnt_freq,
                   m->mnt_passno) < 0;
}

char *hasmntopt(const struct mntent *m, const char *opt)
{
    size_t n = strlen(opt);
    char *p = m->mnt_opts;
    while (p && *p) {
        if (strncmp(p, opt, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == '='))
            return p;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return 0;
}

#endif

/* ---- shell patterns ---- */

static int in_class(const char **pp, int c, int flags)
{
    const char *p = *pp;
    int neg = 0, hit = 0, first = 1;
    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    if (flags & FNM_CASEFOLD)
        c = tolower(c);
    for (; *p && (first || *p != ']'); first = 0) {
        int lo, hi;
        if (p[0] == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e) {
                size_t n = (size_t)(e - p - 2);
                const char *name = p + 2;
                int m = 0;
#define CLASS(nm, fn) if (n == sizeof nm - 1 && memcmp(name, nm, n) == 0) m = fn(c) != 0
                CLASS("alpha", isalpha); CLASS("digit", isdigit); CLASS("alnum", isalnum); CLASS("upper", isupper);
                CLASS("lower", islower); CLASS("space", isspace); CLASS("blank", isblank); CLASS("punct", ispunct);
                CLASS("print", isprint); CLASS("graph", isgraph); CLASS("cntrl", iscntrl); CLASS("xdigit", isxdigit);
#undef CLASS
                hit |= m;
                p = e + 2;
                continue;
            }
        }
        if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1])
            p++;
        lo = (unsigned char)*p++;
        hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            p++;
            if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1])
                p++;
            hi = (unsigned char)*p++;
        }
        if (flags & FNM_CASEFOLD) {
            if (c >= tolower(lo) && c <= tolower(hi))
                hit = 1;
        } else if (c >= lo && c <= hi)
            hit = 1;
    }
    if (*p != ']')
        return -1; /* not a class after all */
    *pp = p + 1;
    return hit != neg;
}

static int fnm(const char *p, const char *s, const char *start, int flags)
{
    for (;;) {
        int c = (unsigned char)*p++;
        switch (c) {
        case 0:
            if ((flags & FNM_LEADING_DIR) && *s == '/')
                return 0;
            return *s ? FNM_NOMATCH : 0;
        case '?':
            if (!*s || ((flags & FNM_PATHNAME) && *s == '/'))
                return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *s == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')))
                return FNM_NOMATCH;
            s++;
            break;
        case '*':
            while (*p == '*')
                p++;
            if ((flags & FNM_PERIOD) && *s == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')))
                return FNM_NOMATCH;
            if (!*p) {
                if (flags & FNM_PATHNAME)
                    return (flags & FNM_LEADING_DIR) || !strchr(s, '/') ? 0 : FNM_NOMATCH;
                return 0;
            }
            for (;; s++) {
                if (fnm(p, s, start, flags) == 0)
                    return 0;
                if (!*s || ((flags & FNM_PATHNAME) && *s == '/'))
                    return FNM_NOMATCH;
            }
        case '[': {
            const char *q = p;
            int r;
            if (!*s || ((flags & FNM_PATHNAME) && *s == '/'))
                return FNM_NOMATCH;
            r = in_class(&q, (unsigned char)*s, flags);
            if (r < 0)
                goto literal;
            if (!r)
                return FNM_NOMATCH;
            p = q;
            s++;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && *p)
                c = (unsigned char)*p++;
            /* fall through */
        default:
        literal:
            if (flags & FNM_CASEFOLD) {
                if (tolower(c) != tolower((unsigned char)*s))
                    return FNM_NOMATCH;
            } else if (c != (unsigned char)*s)
                return FNM_NOMATCH;
            s++;
        }
    }
}

int fnmatch(const char *pattern, const char *s, int flags) { return fnm(pattern, s, s, flags); }

#ifdef __linux__
/* ---- the system log: messages go to the standard error stream ---- */

static const char *log_ident = "";
void openlog(const char *ident, int option, int facility)
{
    (void)option;
    (void)facility;
    log_ident = ident ? ident : "";
}
void closelog(void) {}
void vsyslog(int priority, const char *fmt, va_list ap)
{
    (void)priority;
    if (*log_ident)
        fprintf(stderr, "%s: ", log_ident);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void syslog(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
}

#endif
