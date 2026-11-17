/* library calls that write nothing but errno (getenv, strlen, atof ...) leave what is known of locals and globals;
   errno itself, and memory reached through a pointer, is read again */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int N = 40;
static double zcut = -1;
static char buf[32] = "12.5";

static double dist(double y, double x)
{
    return 10 * atan2(y - .5 * N, x - .25 * N) + pow(y + 1, 1.5); /* (C89's math.h: calls both) */
}

static int cut(int z, int y, int x)
{
    const double r = dist(y, x);
    if (r > .47 * N)
        return 0;
    if (zcut < 0)
        zcut = getenv("NOWRITE_NO_SUCH_VARIABLE") ? atof(getenv("NOWRITE_NO_SUCH_VARIABLE")) : 2;
    if (z >= zcut * N)
        return 0;
    return dist(y, x) + atan2(y - .5 * N, x - .25 * N) < 60 ? 1 : 2;
}

static int through(int *e, const char *s)
{
    int a;
    *e = 0;
    a = (int)strtol(s, 0, 10) + atoi(s);
    return a + *e; /* (errno may have been set) */
}

static size_t lens(char *p)
{
    size_t a = strlen(p), b;
    p[2] = 0;
    b = strlen(p);
    return a * 10 + b;
}

int main(void)
{
    int z, y, x, s = 0, e;
    for (z = 0; z < 6; z++)
        for (y = 0; y < 40; y += 3)
            for (x = 0; x < 40; x += 5)
                s += cut(z * 20, y, x);
    printf("%d\n", s);
    errno = 0;
    e = through(&errno, "99999999999999999999999999");
    printf("%d %d\n", e != 0, errno == ERANGE);
    printf("%lu\n", (unsigned long)lens(buf));
    N = (int)atof(buf) + N;
    printf("%d %g\n", N, dist(1, 2));
    return 0;
}
