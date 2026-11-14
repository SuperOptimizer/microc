/* variable length arrays give their space back when their scope ends */
#include <stdio.h>
#include <string.h>
static unsigned long lo = ~0UL, hi;
static void note(void *p) { unsigned long a = (unsigned long)p; if (a < lo) lo = a; if (a > hi) hi = a; }
static int sum(int n)
{
    int i, t = 0;
    for (i = 0; i < 20000; i++) {
        char a[n], b[n * 2];
        long c[n];
        memset(a, 1, n); memset(b, 2, n * 2); c[n - 1] = i;
        note(a); note(b); note(c);
        if (i % 3 == 0) continue;
        if (i == 19999) break;
        { short d[n + i % 5]; d[0] = (short)i; note(d); t += d[0] & 1; }
        t += a[n - 1] + b[n] + (int)(c[n - 1] & 1);
    }
    i = 0;
    while (i < 20000) { int e[n]; e[0] = i++; note(e); if (e[0] & 1) continue; t++; }
    do { char f[n * 4]; f[n] = 1; note(f); t += f[n]; } while (++i < 40000);
    return t;
}
int main(void)
{
    char top[sum(1000) > 0 ? 8 : 4];
    int n = 300, t;
    char keep[n];
    keep[0] = 5; keep[n - 1] = 7;
    t = sum(1000);
    printf("%d %d %d\n", t, (int)sizeof top, keep[0] + keep[n - 1]);
    printf("%s\n", hi - lo < 100000 ? "bounded" : "grows");
    return 0;
}
