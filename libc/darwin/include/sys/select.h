#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
typedef struct fd_set {
    int fds_bits[32];
} fd_set;
#define FD_SETSIZE 1024
#define FD_ZERO(s) memset((s), 0, sizeof(fd_set))
#define FD_SET(fd, s) ((s)->fds_bits[(fd) / 32] |= 1 << ((fd) % 32))
#define FD_CLR(fd, s) ((s)->fds_bits[(fd) / 32] &= ~(1 << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd) / 32] >> ((fd) % 32)) & 1)
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout);
#endif
