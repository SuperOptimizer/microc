/* Operating system interface. Everything outside ISO C89 lives here. */
#include "mcc.h"
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifndef __linux__
#include <sys/sysctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

u8 *os_read_file(const char *path, size_t *len)
{
    int in = strcmp(path, "-") == 0; /* standard input */
    FILE *f = in ? stdin : fopen(path, "rb");
    Buf b;
    size_t n;
    if (!f)
        return 0;
    b.p = 0;
    b.len = b.cap = 0;
    for (;;) {
        buf_reserve(&b, 65536);
        n = fread(b.p + b.len, 1, b.cap - b.len, f);
        if (n == 0)
            break;
        b.len += n;
    }
    if (!in)
        fclose(f);
    buf_reserve(&b, 1);
    b.p[b.len] = 0;
    *len = b.len;
    return b.p;
}

void os_write_file(const char *path, const u8 *data, size_t len, int executable)
{
    int fd;
    size_t done = 0;
    if (strcmp(path, "/dev/null") == 0)
        return;
    /* Remove first: the kernel caches code signatures per file, so an
       executable must never be rewritten in place. */
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, executable ? 0755 : 0644);
    if (fd < 0)
        die("cannot create %s", path);
    while (done < len) {
        long n = (long)write(fd, data + done, len - done);
        if (n <= 0)
            die("write error on %s", path);
        done += (size_t)n;
    }
    close(fd);
}

#ifdef __linux__
int os_ncpu(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}
#else
int os_ncpu(void)
{
    int mib[2], n = 1;
    size_t len = sizeof n;
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    if (sysctl(mib, 2, &n, &len, 0, 0) != 0 || n < 1)
        n = 1;
    return n;
}
#endif

int os_spawn(int *fd)
{
    int p[2], id;
    if (pipe(p) != 0)
        die("cannot create a pipe");
    fflush(stdout);
    fflush(stderr);
    id = (int)fork();
    if (id < 0)
        die("cannot start a process");
    if (id == 0) {
        close(p[0]);
        *fd = p[1];
    } else {
        close(p[1]);
        *fd = p[0];
    }
    return id;
}

int os_wait(int id)
{
    int status = 0;
    if (waitpid(id, &status, 0) < 0)
        return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void os_send(int fd, const u8 *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        long n = (long)write(fd, data + done, len - done);
        if (n <= 0)
            die("write error");
        done += (size_t)n;
    }
    close(fd);
}

u8 *os_receive(int fd, size_t *len)
{
    Buf b;
    b.p = 0;
    b.len = b.cap = 0;
    for (;;) {
        long n;
        buf_reserve(&b, 65536);
        n = (long)read(fd, b.p + b.len, b.cap - b.len);
        if (n <= 0)
            break;
        b.len += (size_t)n;
    }
    close(fd);
    *len = b.len;
    return b.p;
}

void os_exit(int status)
{
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}

/* ---- for the build tools ---- */

/* When the file was last changed, in nanoseconds; 0 if there is no such file. */
i64 os_mtime(const char *path, int *is_dir)
{
    struct stat st;
    i64 t;
    if (stat(path, &st) != 0)
        return 0;
    if (is_dir)
        *is_dir = S_ISDIR(st.st_mode);
#ifdef __linux__
    t = (i64)st.st_mtim.tv_sec * 1000000000 + (i64)st.st_mtim.tv_nsec;
#else
    t = (i64)st.st_mtimespec.tv_sec * 1000000000 + (i64)st.st_mtimespec.tv_nsec;
#endif
    return t ? t : 1;
}

int os_chdir(const char *dir) { return chdir(dir); }

char *os_getcwd(void)
{
    char *buf = (char *)xmalloc(4096);
    if (!getcwd(buf, 4096))
        die("cannot find the current directory");
    return buf;
}

/* Starts a program; its standard output goes to out_fd unless that is negative. */
int os_run(char **argv, char **envp, int out_fd, int close_fd)
{
    int id;
    fflush(stdout);
    fflush(stderr);
    id = (int)fork();
    if (id < 0)
        die("cannot start a process");
    if (id == 0) {
        if (out_fd >= 0) {
            dup2(out_fd, 1);
            close(out_fd);
        }
        if (close_fd >= 0)
            close(close_fd);
        execve(argv[0], argv, envp);
        fprintf(stderr, "mcc: cannot run %s\n", argv[0]);
        _exit(127);
    }
    return id;
}

/* Waits for any child (or only looks, if block is 0); returns its id and gives its exit status. */
int os_wait_any(int *code, int block)
{
    int status = 0, id = (int)waitpid(-1, &status, block ? 0 : WNOHANG);
    if (id > 0)
        *code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return id;
}

int os_pipe(int fd[2]) { return pipe(fd); }
long os_read(int fd, void *buf, size_t n) { return (long)read(fd, buf, n); }
long os_write(int fd, const void *buf, size_t n) { return (long)write(fd, buf, n); }
void os_close(int fd) { close(fd); }
int os_getpid(void) { return (int)getpid(); }
int os_exists(const char *path) { return access(path, F_OK) == 0; }
int os_mkfifo(const char *path) { return mkfifo(path, 0600); }
int os_open_fifo(const char *path) { return open(path, O_RDWR | O_NONBLOCK); }
void os_unlink(const char *path) { unlink(path); }
void os_exec(char **argv, char **envp) { execve(argv[0], argv, envp); }

void os_sleep_ms(int ms) { usleep((unsigned)ms * 1000); }

int os_list_dir(const char *dir, void (*fn)(const char *name, void *ctx), void *ctx)
{
    DIR *d = opendir(*dir ? dir : ".");
    struct dirent *e;
    if (!d)
        return -1;
    while ((e = readdir(d)) != 0)
        fn(e->d_name, ctx);
    closedir(d);
    return 0;
}
