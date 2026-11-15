/* a store into a local array at an index, both index and value spilled (made before a loop that wants every register):
   x64 X_STORE put the address together in a scratch register from the base register alone, and a local is no
   register (mem_ea adds rbp): it stored to -disp(index) and lost the scale (ufsm annot.c, sample.c, 2026-11-15) */
#include <stdio.h>
static volatile int vi[64];
static volatile long vl[64];
/* the index and the value are made before an inner loop that wants every register and stored into a local array at that index after it */
__attribute__((noinline)) static long f(const float *t, float *rc, int P, int n)
{
    long arr[8] = {0}, s = 0;
    for (int e = 0; e < n; e++) {
        const int ix = vi[e & 63] & 7;
        const long val = vl[e & 63];
        float a0 = t[e], a1 = t[e + 1], a2 = t[e + 2], a3 = t[e + 3], a4 = t[e + 4], a5 = t[e + 5];
        long k0 = e, k1 = e * 3, k2 = e ^ 5, k3 = e + 9, k4 = e * 7, k5 = e - 2, k6 = e << 2, k7 = e | 3, k8 = e * 11, k9 = e + 99, k10 = e * 13, k11 = e - 77;
        for (int x = 0; x < P; x++) {
            rc[x] += a0 * t[x] + a1 * (float)(k0 + x) - a2 * (float)(k1 ^ x) + a3 * (float)(k2 & x) + a4 * (float)(k3 | x) - a5 * (float)(k4 - x);
            k0 += k5; k1 ^= k6; k2 += k7; k3 -= k8; k4 += k9; k5 ^= k10; k6 += k11; k7 += k0; k8 ^= k1; k9 += k2; k10 -= k3; k11 += k4;
        }
        arr[ix] = val;
        s += arr[0] + arr[1] * 3 + arr[7] * 5 + k0 + k1 + k2 + k3 + k4 + k5 + k6 + k7 + k8 + k9 + k10 + k11;
    }
    return s;
}
static float t[400], rc[300];
int main(void)
{
    for (int i = 0; i < 64; i++) { vi[i] = i * 5; vl[i] = (long)i * 1000003L; }
    for (int i = 0; i < 400; i++) t[i] = (float)(i % 17) * 0.125f;
    printf("%lx\n", (unsigned long)f(t, rc, 64, 50));
    return 0;
}
