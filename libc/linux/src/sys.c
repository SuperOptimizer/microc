/* System calls of aarch64 Linux, and what is built directly on them. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "thread.h"

char **environ;
char *program_invocation_name = "", *program_invocation_short_name = "";

static long ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define S0(n) ret(__syscall(n, 0, 0, 0, 0, 0, 0))
#define S1(n, a) ret(__syscall(n, (long)(a), 0, 0, 0, 0, 0))
#define S2(n, a, b) ret(__syscall(n, (long)(a), (long)(b), 0, 0, 0, 0))
#define S3(n, a, b, c) ret(__syscall(n, (long)(a), (long)(b), (long)(c), 0, 0, 0))
#define S4(n, a, b, c, d) ret(__syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0))
#define S5(n, a, b, c, d, e) ret(__syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0))
#define S6(n, a, b, c, d, e, f) ret(__syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f)))

/* ---- threads: what every program has ----
   Thread-local objects are found from the thread pointer (TPIDR_EL0): it
   points at 16 bytes of the thread's own, and the thread's copy of the image
   that the PT_TLS program header describes follows, at the image's alignment. */

#ifdef __x86_64__
struct __tls_image __tls = {0, 0, 16, 16, 16};
#else
struct __tls_image __tls = {0, 0, 0, 16, 16};
#endif
int __libc_threaded;
static struct __pthread first_thread;
static __thread struct __pthread *self_td;
static __thread int thread_errno;

int *__errno_location(void) { return &thread_errno; }
struct __pthread *__thread_self(void) { return self_td; }
void __thread_enter(struct __pthread *td) { self_td = td; }

void __tls_fill(char *tp)
{
    memcpy(tp + __tls.bias, __tls.data, __tls.filesz);
    memset(tp + __tls.bias + __tls.filesz, 0, __tls.memsz - __tls.filesz);
}

/* The first thread's copy, before anything uses a thread-local object (errno is one). */
static void tls_start(char **envp)
{
    unsigned long *aux, phdr = 0, phent = 56, phnum = 0, k;
    long m;
    char *tp;
    while (*envp)
        envp++;
    for (aux = (unsigned long *)(envp + 1); aux[0]; aux += 2) {
        if (aux[0] == 3)
            phdr = aux[1];
        else if (aux[0] == 4)
            phent = aux[1];
        else if (aux[0] == 5)
            phnum = aux[1];
    }
    for (k = 0; phdr && k < phnum; k++) {
        const unsigned long *ph = (const unsigned long *)(phdr + k * phent);
        if ((unsigned)ph[0] == 7) { /* PT_TLS */
            __tls.data = (const char *)ph[2];
            __tls.filesz = ph[4];
            __tls.memsz = ph[5];
            __tls.align = ph[6] < 16 ? 16 : ph[6];
            __tls.bias = (16 + __tls.align - 1) & ~(__tls.align - 1);
#ifdef __x86_64__ /* see sys/x86_64/arch_x64.c */
            __tls.memsz = ((ph[5] + (ph[6] ? ph[6] : 1) - 1) & ~((ph[6] ? ph[6] : 1) - 1)) + 16;
#endif
        }
    }
    m = __syscall(222, 0, (long)(__tls.bias + __tls.memsz + __tls.align), 3, 0x22, -1, 0);
    if (m < 0 && m > -4096)
        for (;;)
            __syscall(94, 127, 0, 0, 0, 0, 0);
    tp = (char *)(((unsigned long)m + __tls.align - 1) & ~(__tls.align - 1));
    __tls_fill(tp);
    __set_tp(tp);
    self_td = &first_thread;
    first_thread.tid = (int)__syscall(96, (long)&first_thread.tid, 0, 0, 0, 0, 0); /* set_tid_address */
}

long __futex_wait(volatile int *addr, int val, const void *timeout)
{
    return __syscall(98, (long)addr, FUTEX_WAIT | FUTEX_PRIVATE, val, (long)timeout, 0, 0);
}

long __futex_wake(volatile int *addr, int n)
{
    return __syscall(98, (long)addr, FUTEX_WAKE | FUTEX_PRIVATE, n, 0, 0, 0);
}

/* The lock: 0 free, 1 held, 2 held and somebody may be asleep waiting for it. */
int __trylock(volatile int *l)
{
    return __sync_bool_compare_and_swap(l, 0, 1);
}

void __lock(volatile int *l)
{
    int k;
    if (__sync_bool_compare_and_swap(l, 0, 1))
        return;
    /* held: it is usually free again within a few instructions */
    for (k = 0; k < 60; k++)
        if (__atomic_load_n(l, __ATOMIC_RELAXED) == 0 && __sync_bool_compare_and_swap(l, 0, 1))
            return;
    while (__atomic_exchange_n(l, 2, __ATOMIC_SEQ_CST) != 0)
        __futex_wait(l, 2, 0);
}

void __unlock(volatile int *l)
{
    if (__atomic_exchange_n(l, 0, __ATOMIC_SEQ_CST) == 2)
        __futex_wake(l, 1);
}

struct __rlock {
    volatile int lock;
    int count;
    void *owner;
};

void __rlock_take(struct __rlock *l)
{
    void *me = &thread_errno; /* an address no other thread has */
    if (__atomic_load_n(&l->owner, __ATOMIC_RELAXED) == me) {
        l->count++;
        return;
    }
    __lock(&l->lock);
    __atomic_store_n(&l->owner, me, __ATOMIC_RELAXED);
    l->count = 1;
}

void __rlock_drop(struct __rlock *l)
{
    if (--l->count == 0) {
        __atomic_store_n(&l->owner, (void *)0, __ATOMIC_RELAXED);
        __unlock(&l->lock);
    }
}

int main(int argc, char **argv, char **envp);

/* the functions to call before main: the linker gathers their addresses */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

void __libc_start(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    tls_start(environ);
    if (argc > 0 && argv[0]) {
        char *slash = strrchr(argv[0], '/');
        program_invocation_name = argv[0];
        program_invocation_short_name = slash ? slash + 1 : argv[0];
    }
    {
        void (**f)(void);
        for (f = __init_array_start; f < __init_array_end; f++)
            (*f)();
    }
    exit(main(argc, argv, environ));
}

long syscall(long n, ...)
{
    va_list ap;
    long a[6];
    int i;
    va_start(ap, n);
    for (i = 0; i < 6; i++)
        a[i] = va_arg(ap, long);
    va_end(ap);
    return ret(__syscall(n, a[0], a[1], a[2], a[3], a[4], a[5]));
}

void _exit(int status)
{
    for (;;)
        __syscall(94, status, 0, 0, 0, 0, 0);
}
void _Exit(int status) { _exit(status); }

/* ---- files ---- */

ssize_t read(int fd, void *buf, size_t n) { return S3(63, fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return S3(64, fd, buf, n); }
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return S4(67, fd, buf, n, off); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return S4(68, fd, buf, n, off); }
ssize_t readv(int fd, const struct iovec *v, int n) { return S3(65, fd, v, n); }
ssize_t writev(int fd, const struct iovec *v, int n) { return S3(66, fd, v, n); }
ssize_t sendfile(int out, int in, off_t *off, size_t n) { return S4(71, out, in, off, n); }
ssize_t copy_file_range(int in, off_t *offin, int out, off_t *offout, size_t n, unsigned flags)
{
    return S6(285, in, offin, out, offout, n, flags);
}

int openat(int dir, const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)S4(56, dir, path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)S4(56, AT_FDCWD, path, flags, mode);
}

int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }
int close(int fd) { return (int)S1(57, fd); }
off_t lseek(int fd, off_t off, int whence) { return S3(62, fd, off, whence); }
int unlinkat(int dir, const char *path, int flags) { return (int)S3(35, dir, path, flags); }
int unlink(const char *path) { return unlinkat(AT_FDCWD, path, 0); }
int rmdir(const char *path) { return unlinkat(AT_FDCWD, path, AT_REMOVEDIR); }
int link(const char *from, const char *to) { return (int)S5(37, AT_FDCWD, from, AT_FDCWD, to, 0); }
int symlink(const char *target, const char *path) { return (int)S3(36, target, AT_FDCWD, path); }
ssize_t readlink(const char *path, char *buf, size_t n) { return S4(78, AT_FDCWD, path, buf, n); }
int renameat(int fd, const char *from, int td, const char *to) { return (int)S4(38, fd, from, td, to); }
int __rename(const char *from, const char *to) { return renameat(AT_FDCWD, from, AT_FDCWD, to); }
int mkdirat(int dir, const char *path, mode_t mode) { return (int)S3(34, dir, path, mode); }
int mkdir(const char *path, mode_t mode) { return mkdirat(AT_FDCWD, path, mode); }
int mknod(const char *path, mode_t mode, dev_t dev) { return (int)S4(33, AT_FDCWD, path, mode, dev); }
int mkfifo(const char *path, mode_t mode) { return mknod(path, (mode & ~(mode_t)S_IFMT) | S_IFIFO, 0); }
int fchmodat(int dir, const char *path, mode_t mode, int flags)
{
    (void)flags;
    return (int)S3(53, dir, path, mode);
}
int chmod(const char *path, mode_t mode) { return fchmodat(AT_FDCWD, path, mode, 0); }
int fchmod(int fd, mode_t mode) { return (int)S2(52, fd, mode); }
int chown(const char *path, uid_t u, gid_t g) { return (int)S5(54, AT_FDCWD, path, u, g, 0); }
int lchown(const char *path, uid_t u, gid_t g) { return (int)S5(54, AT_FDCWD, path, u, g, AT_SYMLINK_NOFOLLOW); }
int fchown(int fd, uid_t u, gid_t g) { return (int)S3(55, fd, u, g); }
int fstatat(int dir, const char *path, struct stat *st, int flags) { return (int)S4(79, dir, path, st, flags); }
int stat(const char *path, struct stat *st) { return fstatat(AT_FDCWD, path, st, 0); }
int lstat(const char *path, struct stat *st) { return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW); }
int fstat(int fd, struct stat *st) { return (int)S2(80, fd, st); }
int faccessat(int dir, const char *path, int mode, int flags)
{
    (void)flags;
    return (int)S3(48, dir, path, mode);
}
int access(const char *path, int mode) { return faccessat(AT_FDCWD, path, mode, 0); }
int chdir(const char *path) { return (int)S1(49, path); }
int fchdir(int fd) { return (int)S1(50, fd); }
int chroot(const char *path) { return (int)S1(51, path); }
int dup(int fd) { return (int)S1(23, fd); }
int dup3(int from, int to, int flags) { return (int)S3(24, from, to, flags); }
int dup2(int from, int to)
{
    if (from == to)
        return fcntl(from, F_GETFD) < 0 ? -1 : to;
    return dup3(from, to, 0);
}
int pipe2(int fd[2], int flags) { return (int)S2(59, fd, flags); }
int pipe(int fd[2]) { return pipe2(fd, 0); }
int truncate(const char *path, off_t len) { return (int)S2(45, path, len); }
int ftruncate(int fd, off_t len) { return (int)S2(46, fd, len); }
int fsync(int fd) { return (int)S1(82, fd); }
int fdatasync(int fd) { return (int)S1(83, fd); }
void sync(void) { S0(81); }
int flock(int fd, int op) { return (int)S2(32, fd, op); }
mode_t umask(mode_t mask) { return (mode_t)S1(166, mask); }
int statfs(const char *path, struct statfs *s) { return (int)S2(43, path, s); }
int fstatfs(int fd, struct statfs *s) { return (int)S2(44, fd, s); }

static void to_statvfs(const struct statfs *f, struct statvfs *s)
{
    memset(s, 0, sizeof *s);
    s->f_bsize = (unsigned long)f->f_bsize;
    s->f_frsize = (unsigned long)(f->f_frsize ? f->f_frsize : f->f_bsize);
    s->f_blocks = f->f_blocks;
    s->f_bfree = f->f_bfree;
    s->f_bavail = f->f_bavail;
    s->f_files = f->f_files;
    s->f_ffree = s->f_favail = f->f_ffree;
    s->f_fsid = (unsigned long)(unsigned)f->f_fsid.__val[0];
    s->f_flag = (unsigned long)f->f_flags;
    s->f_namemax = (unsigned long)f->f_namelen;
}

int statvfs(const char *path, struct statvfs *s)
{
    struct statfs f;
    if (statfs(path, &f) < 0)
        return -1;
    to_statvfs(&f, s);
    return 0;
}

int fstatvfs(int fd, struct statvfs *s)
{
    struct statfs f;
    if (fstatfs(fd, &f) < 0)
        return -1;
    to_statvfs(&f, s);
    return 0;
}

int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout)
{
    return (int)S4(137, set, info, timeout, 8);
}
int sigwaitinfo(const sigset_t *set, siginfo_t *info) { return sigtimedwait(set, info, 0); }

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
        arg = (long)va_arg(ap, void *);
    else
        arg = va_arg(ap, int);
    va_end(ap);
    return (int)S3(25, fd, cmd, arg);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    return (int)S3(29, fd, req, arg);
}

char *getcwd(char *buf, size_t n)
{
    char tmp[4096];
    long r;
    if (!buf) {
        r = S2(17, tmp, sizeof tmp);
        return r < 0 ? 0 : strdup(tmp);
    }
    r = S2(17, buf, n);
    return r < 0 ? 0 : buf;
}

int utimensat(int dir, const char *path, const struct timespec ts[2], int flags)
{
    return (int)S4(88, dir, path, ts, flags);
}
int futimens(int fd, const struct timespec ts[2]) { return utimensat(fd, 0, ts, 0); }

int utimes(const char *path, const struct timeval tv[2])
{
    struct timespec ts[2];
    if (tv) {
        ts[0].tv_sec = tv[0].tv_sec;
        ts[0].tv_nsec = tv[0].tv_usec * 1000;
        ts[1].tv_sec = tv[1].tv_sec;
        ts[1].tv_nsec = tv[1].tv_usec * 1000;
    }
    return utimensat(AT_FDCWD, path, tv ? ts : 0, 0);
}

int utime(const char *path, const struct utimbuf *t)
{
    struct timespec ts[2];
    if (t) {
        ts[0].tv_sec = t->actime;
        ts[1].tv_sec = t->modtime;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
    }
    return utimensat(AT_FDCWD, path, t ? ts : 0, 0);
}

/* ---- directories ---- */

struct __dir {
    int fd;
    long pos, len;
    char buf[4096];
};

DIR *fdopendir(int fd)
{
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d)
        return 0;
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d;
    if (fd < 0)
        return 0;
    d = fdopendir(fd);
    if (!d)
        close(fd);
    return d;
}

struct dirent *readdir(DIR *d)
{
    struct dirent *e;
    if (d->pos >= d->len) {
        long n = S3(61, d->fd, d->buf, sizeof d->buf);
        if (n <= 0)
            return 0;
        d->pos = 0;
        d->len = n;
    }
    e = (struct dirent *)(d->buf + d->pos);
    d->pos += e->d_reclen;
    return e;
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}

void rewinddir(DIR *d)
{
    lseek(d->fd, 0, SEEK_SET);
    d->pos = d->len = 0;
}

int dirfd(DIR *d) { return d->fd; }

/* ---- processes ---- */

pid_t fork(void) { return (pid_t)S5(220, SIGCHLD, 0, 0, 0, 0); }
pid_t vfork(void) { return fork(); }
int execve(const char *path, char *const argv[], char *const envp[]) { return (int)S3(221, path, argv, envp); }
pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru) { return (pid_t)S4(260, pid, status, options, ru); }
pid_t waitpid(pid_t pid, int *status, int options) { return wait4(pid, status, options, 0); }
pid_t wait(int *status) { return wait4(-1, status, 0, 0); }
pid_t wait3(int *status, int options, struct rusage *ru) { return wait4(-1, status, options, ru); }
int kill(int pid, int sig) { return (int)S2(129, pid, sig); }
int killpg(int pgrp, int sig) { return kill(-pgrp, sig); }
pid_t getpid(void) { return (pid_t)S0(172); }
pid_t getppid(void) { return (pid_t)S0(173); }
uid_t getuid(void) { return (uid_t)S0(174); }
uid_t geteuid(void) { return (uid_t)S0(175); }
gid_t getgid(void) { return (gid_t)S0(176); }
gid_t getegid(void) { return (gid_t)S0(177); }
int setuid(uid_t u) { return (int)S1(146, u); }
int setgid(gid_t g) { return (int)S1(144, g); }
int setreuid(uid_t r, uid_t e) { return (int)S2(145, r, e); }
int setregid(gid_t r, gid_t e) { return (int)S2(143, r, e); }
int setresuid(uid_t r, uid_t e, uid_t s) { return (int)S3(147, r, e, s); }
int setresgid(gid_t r, gid_t e, gid_t s) { return (int)S3(149, r, e, s); }
int seteuid(uid_t u) { return setresuid((uid_t)-1, u, (uid_t)-1); }
int setegid(gid_t g) { return setresgid((gid_t)-1, g, (gid_t)-1); }
int getgroups(int n, gid_t *list) { return (int)S2(158, n, list); }
int setgroups(size_t n, const gid_t *list) { return (int)S2(159, n, list); }
int setpgid(pid_t pid, pid_t pgid) { return (int)S2(154, pid, pgid); }
pid_t getpgid(pid_t pid) { return (pid_t)S1(155, pid); }
pid_t getpgrp(void) { return getpgid(0); }
pid_t getsid(pid_t pid) { return (pid_t)S1(156, pid); }
pid_t setsid(void) { return (pid_t)S0(157); }
int __getpid(void) { return getpid(); }
int __kill(int pid, int sig) { return kill(pid, sig); }
int getpriority(int which, unsigned who)
{
    long r = S2(141, which, who);
    return r < 0 ? -1 : (int)(20 - r);
}
int setpriority(int which, unsigned who, int prio) { return (int)S3(140, which, who, prio); }
int nice(int inc)
{
    int p = getpriority(PRIO_PROCESS, 0) + inc;
    return setpriority(PRIO_PROCESS, 0, p) < 0 ? -1 : p;
}
int getrlimit(int what, struct rlimit *r) { return (int)S4(261, 0, what, 0, r); }
int setrlimit(int what, const struct rlimit *r) { return (int)S4(261, 0, what, r, 0); }
int getrusage(int who, struct rusage *ru) { return (int)S2(165, who, ru); }
clock_t times(struct tms *t) { return (clock_t)S1(153, t); }
int prctl(int op, ...)
{
    va_list ap;
    long a[4];
    int i;
    va_start(ap, op);
    for (i = 0; i < 4; i++)
        a[i] = va_arg(ap, long);
    va_end(ap);
    return (int)S5(167, op, a[0], a[1], a[2], a[3]);
}

/* ---- system ---- */

int uname(struct utsname *u) { return (int)S1(160, u); }
int sethostname(const char *name, size_t n) { return (int)S2(161, name, n); }
int setdomainname(const char *name, size_t n) { return (int)S2(162, name, n); }
int gethostname(char *name, size_t n)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    strncpy(name, u.nodename, n);
    if (n)
        name[n - 1] = 0;
    return 0;
}
int getdomainname(char *name, size_t n)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    strncpy(name, u.domainname, n);
    if (n)
        name[n - 1] = 0;
    return 0;
}
int mount(const char *src, const char *dir, const char *type, unsigned long flags, const void *data)
{
    return (int)S5(40, src, dir, type, flags, data);
}
int umount2(const char *dir, int flags) { return (int)S2(39, dir, flags); }
int umount(const char *dir) { return umount2(dir, 0); }
int reboot(int how) { return (int)S3(142, 0xfee1dead, 672274793, how); }
int sysinfo(struct sysinfo *info) { return (int)S1(179, info); }
int klogctl(int type, char *buf, int len) { return (int)S3(116, type, buf, len); }
int vhangup(void) { return (int)S0(58); }
ssize_t getrandom(void *buf, size_t n, unsigned flags) { return S3(278, buf, n, flags); }
int sched_yield(void) { return (int)S0(124); }

/* ---- memory ---- */

void *mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off)
{
    return (void *)S6(222, addr, n, prot, flags, fd, off);
}
int munmap(void *addr, size_t n) { return (int)S2(215, addr, n); }
int mprotect(void *addr, size_t n, int prot) { return (int)S3(226, addr, n, prot); }
int msync(void *addr, size_t n, int flags) { return (int)S3(227, addr, n, flags); }
int madvise(void *addr, size_t n, int advice) { return (int)S3(233, addr, n, advice); }
void *mremap(void *addr, size_t oldn, size_t newn, int flags, ...)
{
    return (void *)S4(216, addr, oldn, newn, flags);
}

/* Anonymous zeroed memory from the kernel; NULL on failure. */
void *__map(size_t n)
{
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}

void __unmap(void *p, size_t n) { munmap(p, n); }

/* ---- time ---- */

int clock_gettime(clockid_t id, struct timespec *ts) { return (int)S2(113, id, ts); }
int clock_settime(clockid_t id, const struct timespec *ts) { return (int)S2(112, id, ts); }
int nanosleep(const struct timespec *req, struct timespec *rem) { return (int)S2(101, req, rem); }
int getitimer(int which, struct itimerval *v) { return (int)S2(102, which, v); }
int setitimer(int which, const struct itimerval *v, struct itimerval *old) { return (int)S3(103, which, v, old); }

int gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;
    (void)tz;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    struct timespec ts;
    (void)tz;
    if (!tv)
        return 0;
    ts.tv_sec = tv->tv_sec;
    ts.tv_nsec = tv->tv_usec * 1000;
    return clock_settime(CLOCK_REALTIME, &ts);
}

int stime(const time_t *t)
{
    struct timespec ts;
    ts.tv_sec = *t;
    ts.tv_nsec = 0;
    return clock_settime(CLOCK_REALTIME, &ts);
}

/* microseconds since 1970, and of processor time used */
long __now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

long __cputime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

unsigned sleep(unsigned seconds)
{
    struct timespec ts, rem;
    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    if (nanosleep(&ts, &rem) < 0)
        return (unsigned)rem.tv_sec;
    return 0;
}

int usleep(useconds_t usec)
{
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (long)(usec % 1000000) * 1000;
    return nanosleep(&ts, 0);
}

unsigned alarm(unsigned seconds)
{
    struct itimerval v, old;
    memset(&v, 0, sizeof v);
    v.it_value.tv_sec = seconds;
    setitimer(ITIMER_REAL, &v, &old);
    return (unsigned)(old.it_value.tv_sec + (old.it_value.tv_usec ? 1 : 0));
}

/* ---- waiting for input ---- */

int poll(struct pollfd *fds, nfds_t n, int timeout)
{
    struct timespec ts;
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (long)(timeout % 1000) * 1000000;
    return (int)S5(73, fds, n, timeout < 0 ? 0 : &ts, 0, 0);
}

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    struct timespec ts;
    if (tv) {
        ts.tv_sec = tv->tv_sec;
        ts.tv_nsec = tv->tv_usec * 1000;
    }
    return (int)S6(72, n, r, w, e, tv ? &ts : 0, 0);
}

int pause(void) { return (int)S5(73, 0, 0, 0, 0, 0); }

/* ---- signals ---- */

#ifdef __x86_64__
void __restore_rt(void);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct sigaction a;
    if (act) {
        a = *act;
        a.sa_flags |= 0x04000000; /* SA_RESTORER */
        a.sa_restorer = __restore_rt;
        act = &a;
    }
    return (int)S4(134, sig, act, old, 8);
}
#else
int sigaction(int sig, const struct sigaction *act, struct sigaction *old) { return (int)S4(134, sig, act, old, 8); }
#endif
int sigprocmask(int how, const sigset_t *set, sigset_t *old) { return (int)S4(135, how, set, old, 8); }
int sigsuspend(const sigset_t *mask) { return (int)S2(133, mask, 8); }
int sigpending(sigset_t *set) { return (int)S2(136, set, 8); }
int sigemptyset(sigset_t *set) { set->__bits[0] = 0; return 0; }
int sigfillset(sigset_t *set) { set->__bits[0] = ~0UL; return 0; }
int sigaddset(sigset_t *set, int sig) { set->__bits[0] |= 1UL << (sig - 1); return 0; }
int sigdelset(sigset_t *set, int sig) { set->__bits[0] &= ~(1UL << (sig - 1)); return 0; }
int sigismember(const sigset_t *set, int sig) { return (int)(set->__bits[0] >> (sig - 1) & 1); }

void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    if (sigaction(sig, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int siginterrupt(int sig, int flag)
{
    struct sigaction sa;
    sigaction(sig, 0, &sa);
    if (flag)
        sa.sa_flags &= ~(unsigned long)SA_RESTART;
    else
        sa.sa_flags |= SA_RESTART;
    return sigaction(sig, &sa, 0);
}

void longjmp(long *env, int val);

void siglongjmp(long *env, int val)
{
    if (env[24])
        sigprocmask(SIG_SETMASK, (sigset_t *)&env[25], 0);
    longjmp(env, val);
}

/* ---- terminals ---- */

int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TCGETS, t); }
int tcsetattr(int fd, int how, const struct termios *t) { return ioctl(fd, TCSETS + how, t); }
int tcflush(int fd, int which) { return ioctl(fd, TCFLSH, (void *)(long)which); }
int tcdrain(int fd) { return ioctl(fd, TCSBRK, (void *)1L); }
int tcflow(int fd, int action) { return ioctl(fd, TCXONC, (void *)(long)action); }
int tcsendbreak(int fd, int dur) { (void)dur; return ioctl(fd, TCSBRK, (void *)0); }
pid_t tcgetpgrp(int fd)
{
    int pgrp;
    return ioctl(fd, TIOCGPGRP, &pgrp) < 0 ? -1 : pgrp;
}
int tcsetpgrp(int fd, pid_t pgrp)
{
    int p = pgrp;
    return ioctl(fd, TIOCSPGRP, &p);
}
pid_t tcgetsid(int fd)
{
    int sid;
    return ioctl(fd, TIOCGSID, &sid) < 0 ? -1 : sid;
}
speed_t cfgetospeed(const struct termios *t) { return t->c_cflag & CBAUD; }
speed_t cfgetispeed(const struct termios *t) { return t->c_cflag & CBAUD; }
int cfsetospeed(struct termios *t, speed_t s)
{
    t->c_cflag = (t->c_cflag & ~(tcflag_t)CBAUD) | (s & CBAUD);
    return 0;
}
int cfsetispeed(struct termios *t, speed_t s) { return s ? cfsetospeed(t, s) : 0; }
int cfsetspeed(struct termios *t, speed_t s) { return cfsetospeed(t, s); }
void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

int isatty(int fd)
{
    struct termios t;
    return tcgetattr(fd, &t) == 0;
}

/* ---- sockets ---- */

int socket(int domain, int type, int protocol) { return (int)S3(198, domain, type, protocol); }
int socketpair(int domain, int type, int protocol, int fd[2]) { return (int)S4(199, domain, type, protocol, fd); }
int bind(int fd, const struct sockaddr *a, socklen_t n) { return (int)S3(200, fd, a, n); }
int listen(int fd, int backlog) { return (int)S2(201, fd, backlog); }
int accept(int fd, struct sockaddr *a, socklen_t *n) { return (int)S3(202, fd, a, n); }
int connect(int fd, const struct sockaddr *a, socklen_t n) { return (int)S3(203, fd, a, n); }
int getsockname(int fd, struct sockaddr *a, socklen_t *n) { return (int)S3(204, fd, a, n); }
int getpeername(int fd, struct sockaddr *a, socklen_t *n) { return (int)S3(205, fd, a, n); }
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *a, socklen_t alen)
{
    return S6(206, fd, buf, n, flags, a, alen);
}
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *a, socklen_t *alen)
{
    return S6(207, fd, buf, n, flags, a, alen);
}
ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }
int setsockopt(int fd, int level, int name, const void *val, socklen_t len) { return (int)S5(208, fd, level, name, val, len); }
int getsockopt(int fd, int level, int name, void *val, socklen_t *len) { return (int)S5(209, fd, level, name, val, len); }
int shutdown(int fd, int how) { return (int)S2(210, fd, how); }
ssize_t sendmsg(int fd, const struct msghdr *m, int flags) { return S3(211, fd, m, flags); }
ssize_t recvmsg(int fd, struct msghdr *m, int flags) { return S3(212, fd, m, flags); }

#include <sched.h>
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
    long r = S3(123, pid, size, set);
    if (r < 0)
        return -1;
    if ((size_t)r < size)
        memset((char *)set + r, 0, size - (size_t)r);
    return 0;
}
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set) { return (int)S3(122, pid, size, set); }
int pivot_root(const char *new_root, const char *put_old) { return (int)S2(41, new_root, put_old); }
int swapon(const char *path, int flags) { return (int)S2(224, path, flags); }
int swapoff(const char *path) { return (int)S1(225, path); }
struct timex;
int adjtimex(struct timex *t) { return (int)S1(171, t); }

/* ---- pseudo terminals (devpts) ---- */

int posix_openpt(int flags) { return open("/dev/ptmx", flags); }
int grantpt(int fd) { (void)fd; return 0; }
int unlockpt(int fd)
{
    int unlock = 0;
    return ioctl(fd, TIOCSPTLCK, &unlock);
}
int ptsname_r(int fd, char *buf, size_t n)
{
    unsigned num;
    char name[24], *p = name + sizeof name;
    if (ioctl(fd, TIOCGPTN, &num) < 0)
        return errno;
    *--p = 0;
    do
        *--p = (char)('0' + num % 10);
    while ((num /= 10) != 0);
    memcpy(p -= 9, "/dev/pts/", 9);
    if (strlen(p) >= n)
        return errno = ERANGE;
    strcpy(buf, p);
    return 0;
}
char *ptsname(int fd)
{
    static char buf[24];
    return ptsname_r(fd, buf, sizeof buf) ? 0 : buf;
}
