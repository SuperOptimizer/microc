/* System call wrappers. Numbers are the Darwin BSD system call numbers. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

long __syscall(long number, long a, long b, long c, long d, long e, long f);

int errno;
char **__environ;

static long check(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

void _exit(int status)
{
    for (;;)
        __syscall(1, status, 0, 0, 0, 0, 0);
}

long read(int fd, void *buf, size_t n)
{
    return check(__syscall(3, fd, (long)buf, (long)n, 0, 0, 0));
}

long write(int fd, const void *buf, size_t n)
{
    return check(__syscall(4, fd, (long)buf, (long)n, 0, 0, 0));
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
    return (int)check(__syscall(5, (long)path, flags, mode, 0, 0, 0));
}

int close(int fd)
{
    return (int)check(__syscall(6, fd, 0, 0, 0, 0, 0));
}

int unlink(const char *path)
{
    return (int)check(__syscall(10, (long)path, 0, 0, 0, 0, 0));
}

long lseek(int fd, long off, int whence)
{
    return check(__syscall(199, fd, off, whence, 0, 0, 0));
}

int isatty(int fd)
{
    char termios[128];
    return __syscall(54, fd, 0x40487413L, (long)termios, 0, 0, 0) == 0; /* TIOCGETA */
}

int __rename(const char *from, const char *to)
{
    return (int)check(__syscall(128, (long)from, (long)to, 0, 0, 0, 0));
}

/* Anonymous zeroed memory from the kernel; NULL on failure. */
void *__map(size_t n)
{
    long r = __syscall(197, 0, (long)n, 3, 0x1002, -1, 0); /* PROT_READ|WRITE, MAP_PRIVATE|MAP_ANON */
    return r < 0 && r > -4096 ? (void *)0 : (void *)r;
}

void __unmap(void *p, size_t n)
{
    __syscall(73, (long)p, (long)n, 0, 0, 0, 0);
}

int __fork(void);
int __pipe(int fd[2]);

int fork(void)
{
    return (int)check(__fork());
}

int pipe(int fd[2])
{
    return (int)check(__pipe(fd));
}

int waitpid(int pid, int *status, int options)
{
    return (int)check(__syscall(7, pid, (long)status, options, 0, 0, 0));
}

int sysctl(int *name, unsigned namelen, void *old, size_t *oldlen, void *newp, size_t newlen)
{
    return (int)check(__syscall(202, (long)name, (long)namelen, (long)old, (long)oldlen, (long)newp, (long)newlen));
}

int __getpid(void)
{
    return (int)__syscall(20, 0, 0, 0, 0, 0, 0);
}

int __kill(int pid, int sig)
{
    return (int)check(__syscall(37, pid, sig, 1, 0, 0, 0));
}

/* Microseconds since the epoch. */
long __now(void)
{
    long tv[2];
    tv[0] = tv[1] = 0;
    __syscall(116, (long)tv, 0, 0, 0, 0, 0);
    return tv[0] * 1000000L + (long)(int)tv[1];
}

/* Microseconds of processor time used by this process. */
long __cputime(void)
{
    long ru[18];
    if (__syscall(117, 0, (long)ru, 0, 0, 0, 0) < 0)
        return -1;
    return (ru[0] + ru[2]) * 1000000L + (long)(int)ru[1] + (long)(int)ru[3];
}

/* ---- what the build tools need beyond the compiler ---- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)check(__syscall(59, (long)path, (long)argv, (long)envp, 0, 0, 0));
}

int chdir(const char *path)
{
    return (int)check(__syscall(12, (long)path, 0, 0, 0, 0, 0));
}

int dup2(int from, int to)
{
    return (int)check(__syscall(90, from, to, 0, 0, 0, 0));
}

int access(const char *path, int mode)
{
    return (int)check(__syscall(33, (long)path, mode, 0, 0, 0, 0));
}

int getpid(void)
{
    return __getpid();
}

int stat(const char *path, struct stat *st)
{
    return (int)check(__syscall(338, (long)path, (long)st, 0, 0, 0, 0));
}

int mkfifo(const char *path, int mode)
{
    return (int)check(__syscall(132, (long)path, mode, 0, 0, 0, 0));
}

/* The kernel has no call for this; it can name the directory behind a descriptor. */
char *getcwd(char *buf, size_t size)
{
    char path[1024];
    int fd = open(".", O_RDONLY);
    long r;
    if (fd < 0)
        return 0;
    r = check(__syscall(92, fd, 50, (long)path, 0, 0, 0)); /* fcntl F_GETPATH */
    close(fd);
    if (r < 0)
        return 0;
    if (strlen(path) + 1 > size) {
        errno = 34;
        return 0;
    }
    return strcpy(buf, path);
}

struct __dir {
    int fd;
    long pos, len, seek;
    char buf[4096];
};

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | 0x100000); /* O_DIRECTORY */
    DIR *d;
    if (fd < 0)
        return 0;
    d = malloc(sizeof *d);
    if (!d) {
        close(fd);
        return 0;
    }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

struct dirent *readdir(DIR *d)
{
    struct dirent *e;
    if (d->pos >= d->len) {
        d->len = check(__syscall(344, d->fd, (long)d->buf, sizeof d->buf, (long)&d->seek, 0, 0)); /* getdirentries64 */
        d->pos = 0;
        if (d->len <= 0)
            return 0;
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

int usleep(unsigned usec)
{
    struct {
        long sec;
        int usec;
    } tv;
    tv.sec = (long)(usec / 1000000);
    tv.usec = (int)(usec % 1000000);
    return (int)check(__syscall(93, 0, 0, 0, 0, (long)&tv, 0));
}
