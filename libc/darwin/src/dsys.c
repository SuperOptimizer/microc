/* The POSIX system calls for programs hosted on Darwin, beyond the few in libc/src/sys.c.
   Numbers are the Darwin BSD system call numbers. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

long __syscall(long number, long a, long b, long c, long d, long e, long f);

static long check(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define CALL1(n, a) check(__syscall(n, (long)(a), 0, 0, 0, 0, 0))
#define CALL2(n, a, b) check(__syscall(n, (long)(a), (long)(b), 0, 0, 0, 0))
#define CALL3(n, a, b, c) check(__syscall(n, (long)(a), (long)(b), (long)(c), 0, 0, 0))

int lstat(const char *path, struct stat *st) { return (int)CALL2(340, path, st); }
int fstat(int fd, struct stat *st) { return (int)CALL2(339, fd, st); }
int mkdir(const char *path, mode_t mode) { return (int)CALL2(136, path, mode); }
int rmdir(const char *path) { return (int)CALL1(137, path); }
ssize_t readlink(const char *path, char *buf, size_t n) { return CALL3(58, path, buf, n); }
int symlink(const char *to, const char *path) { return (int)CALL2(57, to, path); }
int link(const char *from, const char *to) { return (int)CALL2(9, from, to); }
int chmod(const char *path, mode_t mode) { return (int)CALL2(15, path, mode); }
int fchmod(int fd, mode_t mode) { return (int)CALL2(124, fd, mode); }
int chown(const char *path, uid_t u, gid_t g) { return (int)CALL3(16, path, u, g); }
mode_t umask(mode_t mask) { return (mode_t)CALL1(60, mask); }
int dup(int fd) { return (int)CALL1(41, fd); }
pid_t getppid(void) { return (pid_t)CALL1(39, 0); }
uid_t getuid(void) { return (uid_t)CALL1(24, 0); }
uid_t geteuid(void) { return (uid_t)CALL1(25, 0); }
gid_t getgid(void) { return (gid_t)CALL1(47, 0); }
gid_t getegid(void) { return (gid_t)CALL1(43, 0); }
int kill(pid_t pid, int sig) { return (int)CALL3(37, pid, sig, 1); }
int fsync(int fd) { return (int)CALL1(95, fd); }
int ftruncate(int fd, off_t n) { return (int)CALL2(201, fd, n); }
int truncate(const char *path, off_t n) { return (int)CALL2(200, path, n); }
pid_t setsid(void) { return (pid_t)CALL1(147, 0); }
int utimes(const char *path, const struct timeval tv[2]) { return (int)CALL2(138, path, tv); }
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

ssize_t pread(int fd, void *buf, size_t n, off_t off) { return check(__syscall(153, fd, (long)buf, (long)n, off, 0, 0)); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return check(__syscall(154, fd, (long)buf, (long)n, off, 0, 0)); }

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)CALL3(92, fd, cmd, arg);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    long arg;
    va_start(ap, req);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)CALL3(54, fd, req, arg);
}

void *mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off)
{
    long r = __syscall(197, (long)addr, (long)n, prot, flags, fd, off);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int munmap(void *addr, size_t n) { return (int)CALL2(73, addr, n); }
int mprotect(void *addr, size_t n, int prot) { return (int)CALL3(74, addr, n, prot); }

int gettimeofday(struct timeval *tv, void *tz)
{
    struct {
        long sec;
        int usec, pad;
    } t;
    (void)tz;
    t.sec = 0;
    t.usec = 0;
    if (CALL3(116, &t, 0, 0) < 0)
        return -1;
    if (tv) {
        tv->tv_sec = t.sec;
        tv->tv_usec = t.usec;
    }
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    struct {
        long sec;
        int usec, pad;
    } t;
    t.sec = req->tv_sec;
    t.usec = (int)(req->tv_nsec / 1000);
    t.pad = 0;
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    return (int)check(__syscall(93, 0, 0, 0, 0, (long)&t, 0));
}

unsigned sleep(unsigned seconds)
{
    struct timespec t;
    t.tv_sec = seconds;
    t.tv_nsec = 0;
    nanosleep(&t, 0);
    return 0;
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    struct timeval tv;
    (void)id;
    if (gettimeofday(&tv, 0) < 0)
        return -1;
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000;
    return 0;
}

struct tm *gmtime_r(const time_t *t, struct tm *out);

/* Signals: nothing is delivered to handlers; the calls succeed so that programs which only tidy up on a signal still run. */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig;
    (void)act;
    if (old)
        memset(old, 0, sizeof *old);
    return 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    (void)how;
    (void)set;
    if (old)
        *old = 0;
    return 0;
}

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = ~0U; return 0; }
int sigaddset(sigset_t *set, int sig) { *set |= 1U << (sig - 1); return 0; }
int sigdelset(sigset_t *set, int sig) { *set &= ~(1U << (sig - 1)); return 0; }
int sigismember(const sigset_t *set, int sig) { return (*set >> (sig - 1)) & 1; }


long sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX: return 262144;
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return 256;
    case _SC_PAGESIZE: return 16384;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 1;
    }
    errno = EINVAL;
    return -1;
}

#include <sys/ioctl.h>
#include <termios.h>

int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TIOCGETA, t); }
int tcsetattr(int fd, int how, const struct termios *t)
{
    return ioctl(fd, how == TCSADRAIN ? TIOCSETAW : how == TCSAFLUSH ? TIOCSETAF : TIOCSETA, t);
}

#include <sys/sysctl.h>
#include <sys/utsname.h>

static void uname_field(int a, int b, char *out)
{
    int name[2];
    size_t n = 255;
    name[0] = a;
    name[1] = b;
    out[0] = 0;
    if (sysctl(name, 2, out, &n, 0, 0) != 0)
        out[0] = 0;
    out[255] = 0;
}

int uname(struct utsname *u)
{
    uname_field(1, 1, u->sysname);
    uname_field(1, 10, u->nodename);
    uname_field(1, 2, u->release);
    uname_field(1, 4, u->version);
    uname_field(6, 1, u->machine);
    return 0;
}

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout)
{
    return (int)check(__syscall(93, n, (long)r, (long)w, (long)e, (long)timeout, 0));
}

/* ---- sockets ---- */
#include <sys/socket.h>

int socket(int domain, int type, int protocol) { return (int)CALL3(97, domain, type, protocol); }
int connect(int fd, const struct sockaddr *addr, socklen_t len) { return (int)CALL3(98, fd, addr, len); }
int bind(int fd, const struct sockaddr *addr, socklen_t len) { return (int)CALL3(104, fd, addr, len); }
int listen(int fd, int backlog) { return (int)CALL2(106, fd, backlog); }
int accept(int fd, struct sockaddr *addr, socklen_t *len) { return (int)CALL3(30, fd, addr, len); }
int getsockname(int fd, struct sockaddr *addr, socklen_t *len) { return (int)CALL3(32, fd, addr, len); }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len) { return (int)CALL3(31, fd, addr, len); }
int shutdown(int fd, int how) { return (int)CALL2(134, fd, how); }

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    return (int)check(__syscall(105, fd, level, name, (long)val, (long)len, 0));
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    return (int)check(__syscall(118, fd, level, name, (long)val, (long)len, 0));
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len)
{
    return check(__syscall(133, fd, (long)buf, (long)n, flags, (long)addr, (long)len));
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len)
{
    return check(__syscall(29, fd, (long)buf, (long)n, flags, (long)addr, (long)len));
}

ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }
