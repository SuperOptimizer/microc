#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

long __now(void);
long __cputime(void);

void __assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
    fflush(0);
    abort();
}

/* ---- locale: only the "C" locale exists ---- */

char *setlocale(int category, const char *locale)
{
    (void)category;
    if (!locale || !*locale || strcmp(locale, "C") == 0)
        return "C";
    return 0;
}

struct lconv *localeconv(void)
{
    static struct lconv lc = { ".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127 };
    return &lc;
}

/* ---- time: UTC only ---- */

time_t time(time_t *t)
{
    time_t now = __now() / 1000000L;
    if (t)
        *t = now;
    return now;
}

clock_t clock(void)
{
    return __cputime();
}

double difftime(time_t a, time_t b)
{
    return (double)(a - b);
}

static int leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* The calendar fields of a time (UTC), into a place of the caller's. */
struct tm *__gmtime_r(const time_t *t, struct tm *out)
{
    long days = *t / 86400, rem = *t % 86400, y = 1970;
    int m;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);
    while (days < 0) {
        y--;
        days += leap(y) ? 366 : 365;
    }
    while (days >= (leap(y) ? 366 : 365)) {
        days -= leap(y) ? 366 : 365;
        y++;
    }
    out->tm_year = (int)(y - 1900);
    out->tm_yday = (int)days;
    for (m = 0; m < 12; m++) {
        int n = mdays[m] + (m == 1 && leap(y));
        if (days < n)
            break;
        days -= n;
    }
    out->tm_mon = m;
    out->tm_mday = (int)days + 1;
    out->tm_isdst = 0;
    return out;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return __gmtime_r(t, &tm);
}

struct tm *localtime(const time_t *t)
{
    return gmtime(t);
}

time_t mktime(struct tm *tm)
{
    long y = tm->tm_year + 1900L, days = 0, k;
    int m;
    time_t t;
    y += tm->tm_mon / 12;
    m = tm->tm_mon % 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    for (k = 1970; k < y; k++)
        days += leap(k) ? 366 : 365;
    for (k = y; k < 1970; k++)
        days -= leap(k) ? 366 : 365;
    for (k = 0; k < m; k++)
        days += mdays[k] + (k == 1 && leap(y));
    days += tm->tm_mday - 1;
    t = days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    __gmtime_r(&t, tm);
    return t;
}

static const char *const wday_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mon_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };

char *asctime(const struct tm *tm)
{
    static char buf[32];
    sprintf(buf, "%.3s %.3s %2d %02d:%02d:%02d %d\n", wday_names[tm->tm_wday], mon_names[tm->tm_mon], tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t)
{
    return asctime(localtime(t));
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    size_t n = 0;
    char tmp[64];
    for (; *fmt; fmt++) {
        const char *piece = tmp;
        size_t len;
        if (*fmt != '%') {
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            fmt++;
            switch (*fmt) {
            case 'a': sprintf(tmp, "%.3s", wday_names[tm->tm_wday]); break;
            case 'A': piece = wday_names[tm->tm_wday]; break;
            case 'b': sprintf(tmp, "%.3s", mon_names[tm->tm_mon]); break;
            case 'B': piece = mon_names[tm->tm_mon]; break;
            case 'c': sprintf(tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", wday_names[tm->tm_wday], mon_names[tm->tm_mon],
                              tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case 'd': sprintf(tmp, "%02d", tm->tm_mday); break;
            case 'H': sprintf(tmp, "%02d", tm->tm_hour); break;
            case 'I': sprintf(tmp, "%02d", (tm->tm_hour + 11) % 12 + 1); break;
            case 'j': sprintf(tmp, "%03d", tm->tm_yday + 1); break;
            case 'm': sprintf(tmp, "%02d", tm->tm_mon + 1); break;
            case 'M': sprintf(tmp, "%02d", tm->tm_min); break;
            case 'p': piece = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'S': sprintf(tmp, "%02d", tm->tm_sec); break;
            case 'U': sprintf(tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
            case 'w': sprintf(tmp, "%d", tm->tm_wday); break;
            case 'W': sprintf(tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
            case 'x': sprintf(tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
            case 'X': sprintf(tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'y': sprintf(tmp, "%02d", tm->tm_year % 100); break;
            case 'Y': sprintf(tmp, "%d", tm->tm_year + 1900); break;
            case 'Z': piece = "UTC"; break;
            case 'z': piece = "+0000"; break;
            case 'e': sprintf(tmp, "%2d", tm->tm_mday); break;
            case 'h': sprintf(tmp, "%.3s", mon_names[tm->tm_mon]); break;
            case 'D': sprintf(tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
            case 'F': sprintf(tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'T': sprintf(tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': sprintf(tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'C': sprintf(tmp, "%02d", (tm->tm_year + 1900) / 100); break;
            case 'u': sprintf(tmp, "%d", tm->tm_wday ? tm->tm_wday : 7); break;
            case 'k': sprintf(tmp, "%2d", tm->tm_hour); break;
            case 'l': sprintf(tmp, "%2d", (tm->tm_hour + 11) % 12 + 1); break;
            case 's': {
                struct tm copy = *tm;
                sprintf(tmp, "%ld", (long)mktime(&copy));
                break;
            }
            case 'n': piece = "\n"; break;
            case 't': piece = "\t"; break;
            case '%': piece = "%"; break;
            default: return 0;
            }
        }
        len = strlen(piece);
        if (n + len >= max)
            return 0;
        memcpy(s + n, piece, len);
        n += len;
    }
    if (max)
        s[n] = 0;
    return n;
}
