/* Not ISO C: the small POSIX subset the mcc driver needs. */
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
long read(int fd, void *buf, size_t n);
long write(int fd, const void *buf, size_t n);
int close(int fd);
int unlink(const char *path);
long lseek(int fd, long off, int whence);
int isatty(int fd);
void _exit(int status);
int fork(void);
int pipe(int fd[2]);
int execve(const char *path, char *const argv[], char *const envp[]);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int dup2(int from, int to);
int access(const char *path, int mode);
int getpid(void);
#define F_OK 0
#define X_OK 1
int usleep(unsigned usec);
#endif
