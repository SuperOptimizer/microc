#include <stdio.h>
int max(int a, int b) { return a > b ? a : b; }
unsigned umin(unsigned a, unsigned b) { if (b < a) a = b; return a; }
long clampl(long x, long lo, long hi) { if (x < lo) x = lo; if (x > hi) x = hi; return x; }
double dmax(double a, double b) { return a > b ? a : b; }
float fsel(int c, float a, float b) { return c ? a : b; }
int absi(int x) { return x < 0 ? -x : x; }
int b01(int a, int b) { return a < b ? 1 : 0; }
int b10(double a, double b) { return a < b ? 0 : 1; }
int two(int c, int *p, int *q) { int x, y; if (c > 3) { x = *p; y = *q; } else { x = *q; y = *p; } return x * 10 + y; }
char *ps(char *a, char *b, long n) { return n ? a : b; }
int sgn(int x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
int main(void) {
    int i, j, p = 7, q = 9; double nan = 0.0; nan = nan / nan;
    for (i = -3; i <= 3; i++) for (j = -3; j <= 3; j += 2)
        printf("%d %d: %d %u %ld %.1f %.1f %d %d %d %d %s %d\n", i, j, max(i, j), umin((unsigned)i, (unsigned)j), clampl(i * 10, j, 12),
               dmax(i, j), (double)fsel(i, 1.5f, 2.5f), absi(i), b01(i, j), b10(i, j), two(i + j, &p, &q), ps("a", "b", i), sgn(i));
    printf("%.1f %.1f %d %d\n", dmax(nan, 1.0), dmax(1.0, nan), b10(nan, 1.0), b10(1.0, nan));
    return 0;
}
