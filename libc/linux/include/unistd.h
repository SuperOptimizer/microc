#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _SC_ARG_MAX 0
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _POSIX_VERSION 200809L
#define _PC_PATH_MAX 4
#define _PC_NAME_MAX 3
extern char **environ;
extern char *optarg;
extern int optind, opterr, optopt;
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int close(int fd);
int unlink(const char *path);
int unlinkat(int dir, const char *path, int flags);
int rmdir(const char *path);
off_t lseek(int fd, off_t off, int whence);
#define lseek64 lseek
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *buf, size_t n);
void _exit(int status);
pid_t fork(void);
pid_t vfork(void);
int pipe(int fd[2]);
int pipe2(int fd[2], int flags);
int dup(int fd);
int dup2(int from, int to);
int dup3(int from, int to, int flags);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
pid_t getsid(pid_t pid);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t uid);
int setegid(gid_t gid);
int setreuid(uid_t r, uid_t e);
int setregid(gid_t r, gid_t e);
int setresuid(uid_t r, uid_t e, uid_t s);
int setresgid(gid_t r, gid_t e, gid_t s);
int getgroups(int n, gid_t *list);
int setgroups(size_t n, const gid_t *list);
char *getcwd(char *buf, size_t n);
int chdir(const char *path);
int fchdir(int fd);
int chroot(const char *path);
int access(const char *path, int mode);
int faccessat(int dir, const char *path, int mode, int flags);
int link(const char *from, const char *to);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t n);
int chown(const char *path, uid_t uid, gid_t gid);
int fchown(int fd, uid_t uid, gid_t gid);
int lchown(const char *path, uid_t uid, gid_t gid);
int truncate(const char *path, off_t len);
int ftruncate(int fd, off_t len);
#define ftruncate64 ftruncate
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
unsigned sleep(unsigned seconds);
int usleep(useconds_t usec);
unsigned alarm(unsigned seconds);
int pause(void);
long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);
int gethostname(char *name, size_t n);
int sethostname(const char *name, size_t n);
int getdomainname(char *name, size_t n);
int setdomainname(const char *name, size_t n);
int getpagesize(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
int getopt(int argc, char *const argv[], const char *opts);
int nice(int inc);
void *sbrk(long inc);
int brk(void *addr);
long syscall(long n, ...);
char *getlogin(void);
char *getpass(const char *prompt);
int daemon(int nochdir, int noclose);
char *crypt(const char *key, const char *salt);
int vhangup(void);
int getdtablesize(void);
ssize_t copy_file_range(int in, off_t *offin, int out, off_t *offout, size_t n, unsigned flags);
int pivot_root(const char *new_root, const char *put_old);
pid_t gettid(void);
int getentropy(void *buf, size_t n);
#endif
