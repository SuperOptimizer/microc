/* ldexp / scalbn in line (x86-64: a multiplication for a constant exponent, vscalef with AVX-512): the library's
   values, also at the ends of the range */
#include <math.h>
#include <stdio.h>

static double h16(unsigned h)
{
    int e = (h >> 10) & 31, m = h & 1023;
    return e == 0 ? ldexp((double)m, -24) : ldexp((double)(m | 1024), e - 25);
}

int main(void)
{
    volatile double x = 1.75, tiny = 4.9406564584124654e-324, big = 1.7976931348623157e308, nz = -0.0;
    volatile int n = 10, m = -1074, p = 2000, q = -2000, z = 0;
    unsigned h;
    double s = 0;
    for (h = 0; h < 0x7c00; h += 37)
        s += h16(h);
    printf("%.9g %.9g %.9g\n", s, h16(1), h16(0x3c00));
    printf("%g %g %g %g\n", ldexp(x, n), ldexp(x, m), ldexp(x, p), ldexp(x, q));
    printf("%g %g %g %g\n", ldexp(tiny, 1074), ldexp(big, -2098), ldexp(x, z), ldexp(nz, n));
    printf("%g %g %g %g\n", ldexp(x, 3), ldexp(x, -1022), ldexp(x, 1023), ldexp(x, -1074));
    printf("%.17g %.17g\n", ldexp(1.0000000000000002, -1023), ldexp(3.0, -1075));
    return 0;
}
