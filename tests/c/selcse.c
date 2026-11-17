/* Selects made twice from the same test and values are one value (a min / max macro whose argument is itself a
   ?: writes it out twice); selects that differ in the test or in one value stay apart. */
#include <stdio.h>

#define IMIN(a, b) ((a) < (b) ? (a) : (b))
#define IMAX(a, b) ((a) > (b) ? (a) : (b))

__attribute__((noinline)) static int clampidx(float az, int n)
{
    return IMIN((int)az, n - 2 < 0 ? 0 : n - 2);
}

__attribute__((noinline)) static void three(const float *q, const int *dim, int *c0)
{
    int d;
    for (d = 0; d < 3; d++)
        c0[d] = IMIN((int)q[d], dim[d] - 2 < 0 ? 0 : dim[d] - 2);
}

__attribute__((noinline)) static int nest(int a, int b, int c)
{
    return IMAX(IMIN(a, b > c ? b : c), IMIN(a, b > c ? c : b)) + IMIN(IMAX(a, b < 0 ? -b : b), IMAX(c, b < 0 ? -b : b));
}

/* the same test, other values; the same values, another test */
__attribute__((noinline)) static int differ(int a, int b, int c)
{
    int x = a < b ? a : b, y = a < b ? b : a, z = a <= b ? a : b, w = a < c ? a : b;
    return x * 1000 + y * 100 + z * 10 + w;
}

__attribute__((noinline)) static double fsel(double a, double b, int c)
{
    double x = c ? a : b, y = c ? a : b, z = c ? b : a;
    return x * y - z;
}

int main(void)
{
    static const float q[3] = {3.7f, -2.5f, 100.25f};
    static const int dim[3] = {1, 8, 50};
    int c0[3], a, b, c;
    unsigned s = 0;
    printf("%d %d %d %d\n", clampidx(5.5f, 4), clampidx(5.5f, 1), clampidx(-3.5f, 9), clampidx(1.5f, 9));
    three(q, dim, c0);
    printf("%d %d %d\n", c0[0], c0[1], c0[2]);
    for (a = -3; a <= 3; a++)
        for (b = -3; b <= 3; b++)
            for (c = -3; c <= 3; c++)
                s = s * 7u + (unsigned)(nest(a, b, c) + differ(a, b, c));
    printf("%u\n", s);
    printf("%g %g\n", fsel(1.5, 2.5, 0), fsel(1.5, 2.5, 1));
    return 0;
}
