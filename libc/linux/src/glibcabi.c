/* The GNU C library's side of dl.c: what a shared object loaded into this (static) program finds when it
   asks "libc.so.6" for a name. glibctab.c is the table of names; most are this library's own functions,
   whose arguments and structures agree with the GNU library's on x86-64 Linux (the kernel's layouts, the same
   constants). Here are the ones that do not exist under that name or differ:
     - the GNU library's internal spellings (__xstat, __isoc99_sscanf, _IO_putc, __cxa_atexit, ...);
     - struct sigaction, which the GNU library lays out its own way;
     - strerror_r, which there returns the message;
     - calls this library had no use for itself (posix_spawn, nftw, shm_open, eventfd, timerfd).
   The pthread and semaphore objects of this library are smaller than the GNU ones and all-zero when
   statically initialised, as theirs are, so they live in the room a caller compiled for glibc gives them. */
#ifdef __x86_64__
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

long syscall(long n, ...);

struct glibc_entry {
    const char *name;
    void *addr;
};
extern struct glibc_entry __glibc_table[];
extern const int __glibc_table_n;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct glibc_entry *)a)->name, ((const struct glibc_entry *)b)->name);
}

/* (called with dl.c's lock held: the table is put in the order of strcmp the first time) */
void *__glibc_lookup(const char *name)
{
    static int sorted;
    int lo = 0, hi = __glibc_table_n - 1;
    if (!sorted) {
        qsort(__glibc_table, (size_t)__glibc_table_n, sizeof __glibc_table[0], by_name);
        sorted = 1;
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, __glibc_table[mid].name);
        if (c == 0)
            return __glibc_table[mid].addr;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

int __glibc_IO_getc(FILE *f) { return getc(f); }
int __glibc_IO_putc(int c, FILE *f) { return putc(c, f); }

/* destructors of a loaded object: it is never unloaded, and at exit the process goes as a whole */
int __glibc_cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)fn, (void)arg, (void)dso;
    return 0;
}
void __glibc_cxa_finalize(void *dso) { (void)dso; }

int __glibc_register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso)
{
    (void)dso;
    return pthread_atfork(prepare, parent, child);
}

int __glibc_xstat(int ver, const char *path, struct stat *st)
{
    (void)ver;
    return stat(path, st);
}
int __glibc_lxstat(int ver, const char *path, struct stat *st)
{
    (void)ver;
    return lstat(path, st);
}
int __glibc_fxstat(int ver, int fd, struct stat *st)
{
    (void)ver;
    return fstat(fd, st);
}
int __glibc_xmknod(int ver, const char *path, mode_t mode, dev_t *dev)
{
    (void)ver;
    return mknod(path, mode, *dev);
}

ssize_t __glibc_getdelim(char **line, size_t *cap, int delim, FILE *f) { return getdelim(line, cap, delim, f); }

int __glibc_isoc99_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
int __glibc_isoc99_fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int __glibc_open(const char *path, int flags, mode_t mode) { return open(path, flags, mode); }
int __glibc_openat(int dir, const char *path, int flags, mode_t mode) { return (int)syscall(257, dir, path, flags, mode); }

int __glibc_accept4(int fd, void *addr, void *len, int flags) { return (int)syscall(288, fd, addr, len, flags); }
int __glibc_eventfd(unsigned init, int flags) { return (int)syscall(290, init, flags); }
int __glibc_mlock(const void *p, size_t n) { return (int)syscall(149, p, n); }
int __glibc_timerfd_create(int clock, int flags) { return (int)syscall(283, clock, flags); }
int __glibc_timerfd_settime(int fd, int flags, const void *new_value, void *old_value)
{
    return (int)syscall(286, fd, flags, new_value, old_value);
}

int __glibc_pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int protocol)
{
    (void)a, (void)protocol;
    return 0;
}
int __glibc_pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int shared)
{
    (void)a, (void)shared;
    return 0;
}

/* the GNU one: the message itself, in buf only when it had to be made */
char *__glibc_strerror_r(int err, char *buf, size_t n)
{
    const char *s = strerror(err);
    size_t len = strlen(s);
    if (!n)
        return buf;
    if (len >= n)
        len = n - 1;
    memcpy(buf, s, len);
    buf[len] = 0;
    return buf;
}

/* struct sigaction as the GNU library has it: the handler, a mask of 1024 bits, the flags, the restorer */
struct glibc_sigaction {
    void (*handler)(int);
    unsigned long mask[16];
    int flags;
    void (*restorer)(void);
};

int __glibc_sigaction(int sig, const struct glibc_sigaction *act, struct glibc_sigaction *old)
{
    struct sigaction a, o;
    int r;
    if (act) {
        memset(&a, 0, sizeof a);
        a.sa_handler = act->handler;
        a.sa_flags = (unsigned long)(unsigned)act->flags;
        memcpy(&a.sa_mask, act->mask, sizeof a.sa_mask);
    }
    r = sigaction(sig, act ? &a : 0, old ? &o : 0);
    if (r == 0 && old) {
        memset(old, 0, sizeof *old);
        old->handler = o.sa_handler;
        old->flags = (int)o.sa_flags;
        memcpy(old->mask, &o.sa_mask, sizeof o.sa_mask);
    }
    return r;
}

/* POSIX shared memory: files of /dev/shm */
static int shm_path(const char *name, char *out, size_t cap)
{
    while (*name == '/')
        name++;
    if (!*name || strchr(name, '/') || strlen(name) + 10 > cap) {
        errno = EINVAL;
        return -1;
    }
    strcpy(out, "/dev/shm/");
    strcat(out, name);
    return 0;
}
int __glibc_shm_open(const char *name, int flags, mode_t mode)
{
    char path[300];
    if (shm_path(name, path, sizeof path))
        return -1;
    return open(path, flags | O_NOFOLLOW | O_CLOEXEC, mode);
}
int __glibc_shm_unlink(const char *name)
{
    char path[300];
    if (shm_path(name, path, sizeof path))
        return -1;
    return unlink(path);
}

/* posix_spawn: fork, the file actions, exec. The caller's posix_spawn_file_actions_t (80 bytes there) holds
   the list's address and length in its first words. */
struct spawn_act {
    int kind; /* 1 dup2, 2 open */
    int fd, fd2, flags;
    mode_t mode;
    char *path;
};
struct spawn_acts {
    struct spawn_act *v;
    int n, cap;
};

int __glibc_spawn_init(struct spawn_acts *a)
{
    memset(a, 0, sizeof *a);
    return 0;
}
int __glibc_spawn_destroy(struct spawn_acts *a)
{
    int i;
    for (i = 0; i < a->n; i++)
        free(a->v[i].path);
    free(a->v);
    memset(a, 0, sizeof *a);
    return 0;
}
static struct spawn_act *spawn_add(struct spawn_acts *a)
{
    if (a->n == a->cap) {
        struct spawn_act *v = (struct spawn_act *)realloc(a->v, (size_t)(a->cap + 8) * sizeof *v);
        if (!v)
            return 0;
        a->v = v;
        a->cap += 8;
    }
    memset(&a->v[a->n], 0, sizeof a->v[0]);
    return &a->v[a->n++];
}
int __glibc_spawn_adddup2(struct spawn_acts *a, int fd, int newfd)
{
    struct spawn_act *x = spawn_add(a);
    if (!x)
        return ENOMEM;
    x->kind = 1;
    x->fd = fd;
    x->fd2 = newfd;
    return 0;
}
int __glibc_spawn_addopen(struct spawn_acts *a, int fd, const char *path, int flags, mode_t mode)
{
    struct spawn_act *x = spawn_add(a);
    if (!x)
        return ENOMEM;
    x->kind = 2;
    x->fd = fd;
    x->flags = flags;
    x->mode = mode;
    x->path = strdup(path);
    return 0;
}
int __glibc_posix_spawn(pid_t *pid, const char *path, const struct spawn_acts *a, const void *attr, char *const argv[],
                        char *const envp[])
{
    pid_t p;
    int i;
    (void)attr;
    p = fork();
    if (p < 0)
        return errno;
    if (p == 0) {
        for (i = 0; a && i < a->n; i++) {
            const struct spawn_act *x = &a->v[i];
            if (x->kind == 1) {
                if (dup2(x->fd, x->fd2) < 0)
                    _exit(127);
            } else {
                int fd = open(x->path, x->flags, x->mode);
                if (fd < 0)
                    _exit(127);
                if (fd != x->fd) {
                    dup2(fd, x->fd);
                    close(fd);
                }
            }
        }
        execve(path, argv, envp);
        _exit(127);
    }
    if (pid)
        *pid = p;
    return 0;
}

/* nftw: the tree under a path, each entry handed to fn */
struct glibc_ftw {
    int base, level;
};
typedef int (*nftw_fn)(const char *path, const struct stat *st, int kind, struct glibc_ftw *where);

static int walk(char *path, size_t len, size_t cap, nftw_fn fn, int flags, int level)
{
    struct stat st;
    struct glibc_ftw w;
    const char *slash;
    int kind, r, isdir;
    r = flags & 1 ? lstat(path, &st) : stat(path, &st); /* FTW_PHYS */
    if (r < 0 && !(flags & 1) && lstat(path, &st) == 0)
        kind = 6; /* FTW_SLN */
    else if (r < 0) {
        memset(&st, 0, sizeof st);
        kind = 3; /* FTW_NS */
    } else if (S_ISDIR(st.st_mode))
        kind = flags & 8 ? 5 : 1; /* FTW_DP, FTW_D */
    else if (S_ISLNK(st.st_mode))
        kind = 4; /* FTW_SL */
    else
        kind = 0; /* FTW_F */
    isdir = kind == 1 || kind == 5;
    slash = strrchr(path, '/');
    w.base = slash && slash[1] ? (int)(slash + 1 - path) : 0;
    w.level = level;
    if (!(flags & 8) || !isdir) { /* (FTW_DEPTH: a directory after what it holds) */
        r = fn(path, &st, kind, &w);
        if (r)
            return r;
    }
    if (isdir) {
        DIR *d = opendir(path);
        struct dirent *de;
        if (d) {
            while ((de = readdir(d)) != 0) {
                size_t n = strlen(de->d_name);
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || len + n + 2 > cap)
                    continue;
                path[len] = '/';
                memcpy(path + len + 1, de->d_name, n + 1);
                r = walk(path, len + 1 + n, cap, fn, flags, level + 1);
                path[len] = 0;
                if (r) {
                    closedir(d);
                    return r;
                }
            }
            closedir(d);
        }
        if (flags & 8)
            return fn(path, &st, kind, &w);
    }
    return 0;
}

int __glibc_nftw(const char *path, nftw_fn fn, int nfds, int flags)
{
    char *buf;
    size_t len = strlen(path);
    int r;
    (void)nfds;
    buf = (char *)malloc(4096);
    if (!buf || len >= 4096) {
        free(buf);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    r = walk(buf, len, 4096, fn, flags, 0);
    free(buf);
    return r;
}
#else
extern int __glibcabi_nothing_here;
#endif
