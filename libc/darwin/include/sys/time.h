/* Time of day; the layout is Darwin's. */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
struct timeval {
    long tv_sec;
    int tv_usec;
};
#ifndef __timespec_defined
#define __timespec_defined
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif
struct timezone {
    int tz_minuteswest, tz_dsttime;
};
int gettimeofday(struct timeval *tv, void *tz);
int utimes(const char *path, const struct timeval tv[2]);
#endif
