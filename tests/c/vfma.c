/* -ffp-contract: a loop the vectoriser takes rounds a * b + c (and a * b - c, c - a * b) as the same loop left scalar does,
   once with FMA or twice without, in float and double. The scalar loops read through an index (not vectorised); every
   result must equal its vectorised twin bit for bit (2026-11-15 ufsm sample.c: the vector loop multiplied and added
   apart while the scalar one fused, an fp16 ulp apart in test_gpu_input). Prints the mismatches: 0 */
#include <stdio.h>
#include <string.h>

#define N 203
static float fa[N], fb[N], fc[N], fv[N], fs[N];
static double da[N], db[N], dc[N], dv[N], ds[N];
static int ix[N];

__attribute__((noinline)) static void f1v(float *o, int n, float k) { for (int i = 0; i < n; i++) o[i] = fa[i] * fb[i] + fc[i] * k; }
__attribute__((noinline)) static void f1s(float *o, int n, float k) { for (int i = 0; i < n; i++) o[i] = fa[ix[i]] * fb[ix[i]] + fc[ix[i]] * k; }
__attribute__((noinline)) static void f2v(float *o, int n) { for (int i = 0; i < n; i++) o[i] = fa[i] * fb[i] - fc[i]; }
__attribute__((noinline)) static void f2s(float *o, int n) { for (int i = 0; i < n; i++) o[i] = fa[ix[i]] * fb[ix[i]] - fc[ix[i]]; }
__attribute__((noinline)) static void f3v(float *o, int n) { for (int i = 0; i < n; i++) o[i] = fc[i] - fa[i] * fb[i]; }
__attribute__((noinline)) static void f3s(float *o, int n) { for (int i = 0; i < n; i++) o[i] = fc[ix[i]] - fa[ix[i]] * fb[ix[i]]; }
__attribute__((noinline)) static void f4v(float *o, int n, float p, float q, float r) { for (int i = 0; i < n; i++) o[i] = p * o[i] + q + r * fa[i]; }
__attribute__((noinline)) static void f4s(float *o, int n, float p, float q, float r) { for (int i = 0; i < n; i++) o[ix[i]] = p * o[ix[i]] + q + r * fa[ix[i]]; }
__attribute__((noinline)) static void d1v(double *o, int n) { for (int i = 0; i < n; i++) o[i] = da[i] * db[i] + dc[i]; }
__attribute__((noinline)) static void d1s(double *o, int n) { for (int i = 0; i < n; i++) o[i] = da[ix[i]] * db[ix[i]] + dc[ix[i]]; }
__attribute__((noinline)) static void d2v(double *o, int n) { for (int i = 0; i < n; i++) o[i] = dc[i] - da[i] * db[i]; }
__attribute__((noinline)) static void d2s(double *o, int n) { for (int i = 0; i < n; i++) o[i] = dc[ix[i]] - da[ix[i]] * db[ix[i]]; }

int main(void)
{
    unsigned s = 12345;
    int bad = 0, n;
    for (int i = 0; i < N; i++) {
        s = s * 1664525u + 1013904223u; fa[i] = (float)(s >> 8) * 1.1e-7f - 0.9f; da[i] = (double)(s >> 3) * 3.3e-10 - 0.7;
        s = s * 1664525u + 1013904223u; fb[i] = (float)(s >> 8) * 7.3e-8f + 0.3f; db[i] = (double)(s >> 3) * 1.7e-10 + 0.2;
        s = s * 1664525u + 1013904223u; fc[i] = (float)(s >> 8) * 2.9e-8f - 0.2f; dc[i] = (double)(s >> 3) * 2.1e-10 - 0.1;
        ix[i] = i;
    }
    for (n = 1; n <= N; n += n < 40 ? 1 : 17) {
        f1v(fv, n, 0.37f); f1s(fs, n, 0.37f); bad += memcmp(fv, fs, (size_t)n * 4) != 0;
        f2v(fv, n); f2s(fs, n); bad += memcmp(fv, fs, (size_t)n * 4) != 0;
        f3v(fv, n); f3s(fs, n); bad += memcmp(fv, fs, (size_t)n * 4) != 0;
        memcpy(fv, fb, sizeof fv); memcpy(fs, fb, sizeof fs);
        f4v(fv, n, 1.1f, 0.05f, 0.02f); f4s(fs, n, 1.1f, 0.05f, 0.02f); bad += memcmp(fv, fs, (size_t)n * 4) != 0;
        d1v(dv, n); d1s(ds, n); bad += memcmp(dv, ds, (size_t)n * 8) != 0;
        d2v(dv, n); d2s(ds, n); bad += memcmp(dv, ds, (size_t)n * 8) != 0;
    }
    printf("mismatches %d\n", bad);
    return 0;
}
