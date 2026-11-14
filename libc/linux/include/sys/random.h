#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H
#include <sys/types.h>
#define GRND_NONBLOCK 1
ssize_t getrandom(void *buf, size_t n, unsigned flags);
#endif
