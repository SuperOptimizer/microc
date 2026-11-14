#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
#define CLOCKS_PER_SEC 1000000
typedef long time_t;
typedef long clock_t;
typedef int clockid_t;
#ifndef __timespec_defined
#define __timespec_defined
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_BOOTTIME 7
clock_t clock(void);
time_t time(time_t *t);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime_r(const time_t *t, struct tm *out);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char *strptime(const char *s, const char *fmt, struct tm *tm);
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t id, struct timespec *ts);
int clock_settime(clockid_t id, const struct timespec *ts);
int stime(const time_t *t);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
int clock_getres(clockid_t id, struct timespec *ts);
#endif
