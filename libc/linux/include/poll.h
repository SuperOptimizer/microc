#ifndef _POLL_H
#define _POLL_H
struct pollfd {
    int fd;
    short events, revents;
};
typedef unsigned long nfds_t;
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 0x10
#define POLLNVAL 0x20
#define POLLRDNORM 0x40
#define POLLRDBAND 0x80
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
int poll(struct pollfd *fds, nfds_t n, int timeout);
#endif
