/* Products feeding sums and differences, with more values alive than there are registers. Every value is a
   small integer, so the result is the same whether or not a product and a sum are rounded together
   (-ffp-contract=fast, which the test suite also builds this with). */
#include <stdio.h>

static double many(const double *p, const float *q)
{
    double a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], a4 = p[4], a5 = p[5], a6 = p[6], a7 = p[7], a8 = p[8], a9 = p[9], a10 = p[10], a11 = p[11], a12 = p[12], a13 = p[13], a14 = p[14], a15 = p[15], a16 = p[16], a17 = p[17], a18 = p[18], a19 = p[19], a20 = p[20], a21 = p[21], a22 = p[22], a23 = p[23], a24 = p[24], a25 = p[25], a26 = p[26], a27 = p[27], a28 = p[28], a29 = p[29], a30 = p[30], a31 = p[31], a32 = p[32], a33 = p[33], a34 = p[34], a35 = p[35], a36 = p[36], a37 = p[37], a38 = p[38], a39 = p[39];
    float f0 = q[0], f1 = q[1], f2 = q[2], f3 = q[3], f4 = q[4], f5 = q[5], f6 = q[6], f7 = q[7];
    double r = 0, s, t, u;
    float g;
    s = a0 * a1 + a2;
    t = a2 - a0 * a1;
    u = a1 * a2 - a0;
    r += s * 3.0 + t * u;
    s = a3 * a4 + a5;
    t = a5 - a3 * a4;
    u = a4 * a5 - a3;
    r += s * 3.0 + t * u;
    s = a6 * a7 + a8;
    t = a8 - a6 * a7;
    u = a7 * a8 - a6;
    r += s * 3.0 + t * u;
    s = a9 * a10 + a11;
    t = a11 - a9 * a10;
    u = a10 * a11 - a9;
    r += s * 3.0 + t * u;
    s = a12 * a13 + a14;
    t = a14 - a12 * a13;
    u = a13 * a14 - a12;
    r += s * 3.0 + t * u;
    s = a15 * a16 + a17;
    t = a17 - a15 * a16;
    u = a16 * a17 - a15;
    r += s * 3.0 + t * u;
    s = a18 * a19 + a20;
    t = a20 - a18 * a19;
    u = a19 * a20 - a18;
    r += s * 3.0 + t * u;
    s = a21 * a22 + a23;
    t = a23 - a21 * a22;
    u = a22 * a23 - a21;
    r += s * 3.0 + t * u;
    s = a24 * a25 + a26;
    t = a26 - a24 * a25;
    u = a25 * a26 - a24;
    r += s * 3.0 + t * u;
    s = a27 * a28 + a29;
    t = a29 - a27 * a28;
    u = a28 * a29 - a27;
    r += s * 3.0 + t * u;
    s = a30 * a31 + a32;
    t = a32 - a30 * a31;
    u = a31 * a32 - a30;
    r += s * 3.0 + t * u;
    s = a33 * a34 + a35;
    t = a35 - a33 * a34;
    u = a34 * a35 - a33;
    r += s * 3.0 + t * u;
    s = a36 * a37 + a38;
    t = a38 - a36 * a37;
    u = a37 * a38 - a36;
    r += s * 3.0 + t * u;
    g = f0 * f1 + f2;
    g = g - f3 * f4;
    g = f5 * f6 - g + f7 * g;
    /* everything is still wanted here */
    return r + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 + a37 + a38 + a39 + g + f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7;
}

static double poly(double x, double y) { return x * y + (x - y * 2.0) + (y * x - 7.0) + (5.0 - x * x) + 2.0 * x * y; }

int main(void)
{
    double p[40];
    float q[8];
    int i;
    for (i = 0; i < 40; i++)
        p[i] = (double)((i * 7) % 11 - 4);
    for (i = 0; i < 8; i++)
        q[i] = (float)(i * 3 - 5);
    printf("%.1f %.1f %.1f\n", many(p, q), poly(3.0, -4.0), poly(-12.0, 9.0));
    return 0;
}
