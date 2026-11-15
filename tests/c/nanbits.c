/* NAN and __builtin_nan are the positive quiet NaN, as gcc and glibc make them (a NaN's sign shows in printf and in its bits) */
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    float a = __builtin_nanf(""), b = NAN, e = -NAN;
    double c = __builtin_nan(""), d = NAN;
    unsigned x, y, w;
    unsigned long z, v;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    memcpy(&w, &e, 4);
    memcpy(&z, &c, 8);
    memcpy(&v, &d, 8);
    printf("%x %x %x %lx %lx %g %g %g\n", x, y, w, z, v, (double)a, c, (double)e);
    return 0;
}
