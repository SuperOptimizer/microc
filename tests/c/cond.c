/* conditions joined by && and ||: conditional-compare chains, truth values */
#include <stdio.h>

static int range(int x) { return x >= 23 && x <= 38; }
static int one_of(int a) { return a == 1 || a == 3 || a == 9; }
static int urange(unsigned x, unsigned lo, unsigned hi) { return x >= lo && x < hi; }
static int mixed(int a, long b, unsigned c) { return (a < 0 || b > 100000000000L) && c != 7; }
static int nested(int a, int b, int c, int d) { return (a < b && c < d) || (a == d && b != c); }
static int not_both(int a, int b) { return !(a > 5 && b > 5); }
static int big(long a) { return a == 1000000 || a == -1000000 || a == 31 || a == 32; }
static double pick(int a, int b, double x, double y) { return a < b && a != 0 ? x : y; }
static int fl(double x, int n) { return x > 1.5 && n < 4; }

int count_if(const int *v, int n)
{
    int i, c = 0;
    for (i = 0; i < n; i++)
        if (range(v[i]) || one_of(v[i]))
            c++;
    return c;
}

int main(void)
{
    static const int xs[] = { -2147483647 - 1, -1, 0, 1, 2, 3, 5, 6, 7, 9, 22, 23, 24, 31, 32, 38, 39, 2147483647 };
    static const long ls[] = { -1000000, 0, 31, 32, 33, 1000000, 100000000000L, 100000000001L };
    int n = (int)(sizeof xs / sizeof xs[0]), i, j;
    unsigned h = 0;
    for (i = 0; i < n; i++)
        printf("%d%d%d ", range(xs[i]), one_of(xs[i]), urange((unsigned)xs[i], 3, 32));
    printf("\n");
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            h = h * 31 + (unsigned)nested(xs[i], xs[j], xs[(i + j) % n], xs[(i * j) % n]);
            h = h * 7 + (unsigned)not_both(xs[i], xs[j]);
            h = h * 3 + (unsigned)mixed(xs[i], ls[j % 8], (unsigned)xs[j]);
            if (xs[i] < xs[j] && xs[j] != 9 && (xs[i] > 0 || xs[j] == 39))
                h += 5;
            if (!(xs[i] == xs[j] || xs[i] + 1 == xs[j]))
                h ^= 1;
        }
    printf("%u\n", h);
    for (i = 0; i < 8; i++)
        printf("%d", big(ls[i]));
    printf(" %d %g %g %g %d%d%d\n", count_if(xs, n), pick(1, 2, 0.5, 1.5), pick(0, 2, 0.5, 1.5), pick(3, 2, 0.5, 1.5),
           fl(2.0, 3), fl(1.0, 3), fl(2.0, 4));
    return 0;
}
