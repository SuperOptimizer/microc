/* doubles computed in a loop of four-byte lanes (2026-11-17): ufsm's emb_at accumulates v[k] += (float) (w * s[k]), an int8
   to a double, a product, back to a float. With AVX the doubles are vectors twice as wide (vcvtdq2pd / vcvtps2pd / vmulpd /
   vcvtpd2ps). Every count from 0 to 70 against the scalar loop, bit for bit, in AVX2 functions (and without AVX the same). */
#include <stdio.h>
#include <string.h>

typedef signed char s8;
#define N 80

__attribute__((target("avx2"), noinline)) static void acc8(float *v, const s8 *s, double w, int n)
{
    for (int k = 0; k < n; k++)
        v[k] += (float)(w * s[k]);
}
__attribute__((target("avx2"), noinline)) static void ext_f(float *v, const float *a, double w, int n)
{
    for (int k = 0; k < n; k++)
        v[k] = (float)((double)a[k] * w + 1.0);
}
__attribute__((target("avx2"), noinline)) static void ext_i(float *v, const int *a, double w, double c, int n)
{
    for (int k = 0; k < n; k++)
        v[k] = (float)(a[k] / w - c);
}
__attribute__((noinline)) static void acc8_s(float *v, const s8 *s, double w, int n)
{
    for (volatile int k = 0; k < n; k++)
        v[k] += (float)(w * s[k]);
}
__attribute__((noinline)) static void ext_f_s(float *v, const float *a, double w, int n)
{
    for (volatile int k = 0; k < n; k++)
        v[k] = (float)((double)a[k] * w + 1.0);
}
__attribute__((noinline)) static void ext_i_s(float *v, const int *a, double w, double c, int n)
{
    for (volatile int k = 0; k < n; k++)
        v[k] = (float)(a[k] / w - c);
}

int main(void)
{
    static s8 s[N];
    static float a[N], v1[N], v2[N];
    static int ia[N];
    unsigned x = 12345;
    int bad = 0, n, k, avx = __builtin_cpu_supports("avx2");
    for (n = 0; n <= 70; n++) {
        double w = 0.123456789 * (n + 1);
        for (k = 0; k < N; k++) {
            x = x * 1103515245u + 12345u;
            s[k] = (s8)(x >> 16);
            a[k] = (float)((int)(x >> 8) % 777) * 0.013f;
            ia[k] = (int)(x >> 3) - 0x08000000;
            v1[k] = v2[k] = (float)(x % 1000) * 0.37f;
        }
        if (avx) acc8(v1 + 1, s + 2, w, n); else acc8_s(v1 + 1, s + 2, w, n);
        acc8_s(v2 + 1, s + 2, w, n);
        bad += memcmp(v1, v2, sizeof v1) != 0;
        if (avx) ext_f(v1, a, w, n); else ext_f_s(v1, a, w, n);
        ext_f_s(v2, a, w, n);
        bad += memcmp(v1, v2, sizeof v1) != 0;
        if (avx) ext_i(v1, ia, w, 3.5, n); else ext_i_s(v1, ia, w, 3.5, n);
        ext_i_s(v2, ia, w, 3.5, n);
        bad += memcmp(v1, v2, sizeof v1) != 0;
    }
    printf("vecdbl: %d bad\n", bad);
    return 0;
}
