/* File control, with Darwin's numbers. */
#ifndef _FCNTL_H
#define _FCNTL_H
#include <sys/types.h>
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_NONBLOCK 4
#define O_NDELAY O_NONBLOCK
#define O_APPEND 8
#define O_SYNC 0x80
#define O_NOFOLLOW 0x100
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_EXCL 0x800
#define O_NOCTTY 0x20000
#define O_DIRECTORY 0x100000
#define O_CLOEXEC 0x1000000
#define O_BINARY 0
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETPATH 50
#define FD_CLOEXEC 1
#define AT_FDCWD (-2)
int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);
#endif
