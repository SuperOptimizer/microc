#include <stdio.h>
#include <math.h>
#include <string.h>
/* More floating values alive at once than xmm0-xmm12 hold (x86-64 with AVX-512: xmm16-xmm31 take the rest, every
   instruction on them in its EVEX encoding): arithmetic, comparisons and selects, conversions to and from integers
   of both signs and widths and between float and double, bit casts, negation, absolute value, square root, loads
   and stores at an index, values that live across calls, and loops the vectoriser gives more accumulators than
   there are low registers. All of it is exact in binary floating point, so -ffast-math and fused multiply-adds
   print the same. */

/* x less the multiple of 4096 below it (towards zero) */
static float wrapf(float x)
{
    return x - 4096.0f * (float)(int)(x / 4096.0f);
}

static double wrapd(double x)
{
    return x - 65536.0 * (double)(long)(x / 65536.0);
}

__attribute__((noinline)) float many_f(const float *p, int n, float *frac)
{
    float a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], a4 = p[4], a5 = p[5], a6 = p[6], a7 = p[7], a8 = p[8], a9 = p[9];
    float b0 = p[10], b1 = p[11], b2 = p[12], b3 = p[13], b4 = p[14], b5 = p[15], b6 = p[16], b7 = p[17], b8 = p[18], b9 = p[19];
    int i;
    for (i = 0; i < n; i++) {
        unsigned u;
        a0 = wrapf(a0 * 3.0f + b0);
        a1 = wrapf(a1 * 5.0f - b1);
        a2 = wrapf(a2 > b2 ? a2 - b2 : a2 + b2 + b2);
        a3 = wrapf(b3 - a3 + 7.0f);
        a4 = (float)fabs((double)a4 - 100.0) + (float)(i & 3);
        a5 = wrapf((float)sqrt((double)(a5 * a5)) + b5);
        a6 = (float)((int)a6 / 3 + i * 17);
        a7 = (float)((unsigned)a7 * 7u % 1000u);
        a8 = (float)((double)a8 * 0.5 + (double)b8);
        a9 = a9 < b9 ? b9 + 1.0f : a9 - 2.0f;
        memcpy(&u, &b0, 4);
        u ^= 0x80000000u;
        memcpy(&b0, &u, 4);
        b0 = wrapf(1.0f - b0);
        b1 = wrapf(b1 + a0);
        b2 = wrapf(b2 * 2.0f + 1.0f);
        b3 = wrapf(b3 + a2);
        b4 = b4 > 50.0f ? b4 - 50.0f : b4 + 3.0f;
        b5 = wrapf(b5 + b4);
        b6 = (float)(unsigned long)((unsigned long)b6 * 3ul + 20ul) - 2.0f * (float)(i & 7);
        b6 = wrapf(b6);
        b7 = (float)(long)(b7 + a6) * 0.25f;
        b7 = wrapf((float)(int)b7);
        b8 = wrapf(b8 + b7 + 1.0f);
        b9 = wrapf(b9 + a7);
    }
    *frac = a8;
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a9 + b0 + b1 + b2 + b3 + b4 + b5 + b6 + b7 + b8 + b9;
}

__attribute__((noinline)) double step(double x, double y, int k)
{
    return wrapd(x * 3.0 + y + (double)k);
}

/* eighteen sums alive between two calls, twenty-four values alive across them */
__attribute__((noinline)) double many_d(const double *p, const double *q, int n, double *o)
{
    double d[24], r = 0.0;
    double d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12, d13, d14, d15, d16, d17, d18, d19, d20, d21, d22, d23;
    int i;
    for (i = 0; i < 24; i++)
        d[i] = p[i];
    d0 = d[0]; d1 = d[1]; d2 = d[2]; d3 = d[3]; d4 = d[4]; d5 = d[5]; d6 = d[6]; d7 = d[7];
    d8 = d[8]; d9 = d[9]; d10 = d[10]; d11 = d[11]; d12 = d[12]; d13 = d[13]; d14 = d[14]; d15 = d[15];
    d16 = d[16]; d17 = d[17]; d18 = d[18]; d19 = d[19]; d20 = d[20]; d21 = d[21]; d22 = d[22]; d23 = d[23];
    for (i = 0; i < n; i++) {
        double t0 = q[0] + d0, t1 = q[1] + d1, t2 = q[2] + d2, t3 = q[3] + d3, t4 = q[4] + d4, t5 = q[5] + d5;
        double t6 = q[6] + d6, t7 = q[7] + d7, t8 = q[8] + d8, t9 = q[9] + d9, t10 = q[10] + d10, t11 = q[11] + d11;
        double t12 = q[12] + d12, t13 = q[13] + d13, t14 = q[14] + d14, t15 = q[15] + d15, t16 = q[16] + d16, t17 = q[17] + d17;
        double u = t0 * t17 - t1 * t16 + t2 * t15 - t3 * t14 + t4 * t13 - t5 * t12 + t6 * t11 - t7 * t10 + t8 * t9;
        double v = t17 - t16 + t15 - t14 + t13 - t12 + t11 - t10 + t9 - t8 + t7 - t6 + t5 - t4 + t3 - t2 + t1 - t0;
        double w = step(u, v, i);
        o[i & 15] = w + t0 + t5 + t11 + t17;
        d0 = wrapd(d0 + w); d1 = wrapd(d1 + d0); d2 = wrapd(d2 + d1); d3 = wrapd(d3 - d2); d4 = wrapd(d4 + d3);
        d5 = wrapd(d5 + 2.0 * d4); d6 = wrapd(d6 + d5); d7 = wrapd(d7 - d6); d8 = wrapd(d8 + d7); d9 = wrapd(d9 + d8);
        d10 = d10 > d9 ? d10 - d9 : d10 + 11.0; d11 = wrapd(d11 + d10); d12 = wrapd(d12 + d11); d13 = wrapd(d13 + d12);
        d14 = (double)(float)d14 + 1.0; d15 = (double)((long)d15 >> 1) + d14; d16 = wrapd(d16 + d15);
        d17 = (double)(unsigned)((unsigned)d17 + 99u); d18 = wrapd(d18 + d17); d19 = -d19 + 5.0; d20 = fabs(d19) + d20;
        d20 = wrapd(d20); d21 = sqrt(d21 * d21) + 1.0; d22 = d22 < d21 ? d21 : d22 - 1.0; d23 = wrapd(d23 + d22);
        r += step(d23, d0, i);
        r = wrapd(r);
    }
    return r + d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9 + d10 + d11 + d12 + d13 + d14 + d15 + d16 + d17 + d18 + d19 +
           d20 + d21 + d22 + d23;
}

static float va[256], vb[256];
static int ia[256], ib[256];
static unsigned char ca[256];
static short ha[256];

/* reductions: one accumulator each, a vector register where the loop is vectorised */
__attribute__((noinline)) void many_v(float *o)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0, s8 = 0, s9 = 0;
    float s10 = 0, s11 = 0, s12 = 0, s13 = 0, s14 = 0, s15 = 0, s16 = 0, s17 = 0;
    int i;
    for (i = 0; i < 256; i++) {
        float x = va[i], y = vb[i];
        s0 += x; s1 += y; s2 += x * y; s3 += x - y; s4 += x + y * 2.0f; s5 += x * x; s6 += y * y; s7 += x * 3.0f;
        s8 += y - 1.0f; s9 += (x + 1.0f) * y; s10 += x > y ? x : y; s11 += x < y ? x : y; s12 += x * 0.5f;
        s13 += y * 0.25f; s14 += x - 2.0f * y; s15 += (float)ia[i]; s16 += x * 4.0f - y; s17 += 1.0f;
    }
    o[0] = s0; o[1] = s1; o[2] = s2; o[3] = s3; o[4] = s4; o[5] = s5; o[6] = s6; o[7] = s7; o[8] = s8;
    o[9] = s9; o[10] = s10; o[11] = s11; o[12] = s12; o[13] = s13; o[14] = s14; o[15] = s15; o[16] = s16; o[17] = s17;
}

__attribute__((noinline)) void many_i(int *o)
{
    int s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0, s8 = 0, s9 = 0;
    int s10 = 0, s11 = 0, s12 = 0, s13 = 0, s14 = 0, s15 = 0, s16 = 0, s17 = 0, s18 = 0, s19 = 0;
    int i;
    for (i = 0; i < 256; i++) {
        int x = ia[i], y = ib[i];
        s0 += x; s1 += y; s2 += x * y; s3 += x - y; s4 += x & y; s5 += x | y; s6 += x ^ y; s7 += (int)((unsigned)x << 3);
        s8 += y >> 2; s9 += (int)((unsigned)x >> 20); s10 += x > y ? x : y; s11 += x < y ? x : y; s12 += ca[i];
        s13 += ha[i]; s14 += x * 7 - y; s15 += (int)va[i]; s16 += ~x; s17 += -y; s18 += ca[i] * ha[i]; s19 += x + i;
    }
    o[0] = s0; o[1] = s1; o[2] = s2; o[3] = s3; o[4] = s4; o[5] = s5; o[6] = s6; o[7] = s7; o[8] = s8; o[9] = s9;
    o[10] = s10; o[11] = s11; o[12] = s12; o[13] = s13; o[14] = s14; o[15] = s15; o[16] = s16; o[17] = s17;
    o[18] = s18; o[19] = s19;
}

int main(void)
{
    float p[20], frac, fo[18];
    double dp[24], dq[18], o[16], r;
    int i, io[20];
    for (i = 0; i < 20; i++)
        p[i] = (float)(i * 37 % 101 + 1);
    printf("%.1f", (double)many_f(p, 45, &frac));
    printf(" %.9f\n", (double)frac);
    for (i = 0; i < 24; i++)
        dp[i] = (double)(i * 53 % 211 + 2);
    for (i = 0; i < 18; i++)
        dq[i] = (double)(i * 7 % 13 - 6);
    for (i = 0; i < 16; i++)
        o[i] = 0.0;
    r = many_d(dp, dq, 37, o);
    printf("%.1f", r);
    for (i = 0; i < 16; i++)
        printf(" %.1f", o[i]);
    printf("\n");
    for (i = 0; i < 256; i++) {
        va[i] = (float)(i * 29 % 61 - 30);
        vb[i] = (float)(i * 31 % 47 - 20);
        ia[i] = i * 1237 % 2003 - 1000;
        ib[i] = i * 911 % 1009 - 500;
        ca[i] = (unsigned char)(i * 7);
        ha[i] = (short)(i * 113 % 499 - 250);
    }
    many_v(fo);
    for (i = 0; i < 18; i++)
        printf("%.2f ", (double)fo[i]);
    printf("\n");
    many_i(io);
    for (i = 0; i < 20; i++)
        printf("%d ", io[i]);
    printf("\n");
    return 0;
}
