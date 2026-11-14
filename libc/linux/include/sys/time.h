#ifndef _SYS_TIME_H
#define _SYS_TIME_H
typedef long __time_t_for_tv;
struct timeval {
    long tv_sec;
    long tv_usec;
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
struct itimerval {
    struct timeval it_interval, it_value;
};
#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2
int gettimeofday(struct timeval *tv, void *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int getitimer(int which, struct itimerval *v);
int setitimer(int which, const struct itimerval *v, struct itimerval *old);
int utimes(const char *path, const struct timeval tv[2]);
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, op) ((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec op (b)->tv_usec : (a)->tv_sec op (b)->tv_sec)
#endif
