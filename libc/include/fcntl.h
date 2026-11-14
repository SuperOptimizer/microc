/* Not ISO C: the small POSIX subset the mcc driver needs. */
#ifndef _FCNTL_H
#define _FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NONBLOCK 4
#define O_APPEND 8
#define O_CREAT 0x200
#define O_TRUNC 0x400
#define O_EXCL 0x800
int open(const char *path, int flags, ...);
#endif
