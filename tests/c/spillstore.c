#include <stdio.h>
/* a store of a spilled value into a frame array at a spilled index (x64: both scratch registers hold operands, so the
   address is formed first by an lea, which lost the frame base rbp, and an lea with no base dropped its index) */
__attribute__((noinline)) long f(const long *v, int n, long k)
{
    long a[64], b[64];
    long s0 = v[0], s1 = v[1], s2 = v[2], s3 = v[3], s4 = v[4], s5 = v[5], s6 = v[6], s7 = v[7], s8 = v[8], s9 = v[9], s10 = v[10], s11 = v[11], s12 = v[12], s13 = v[13], s14 = v[14], s15 = v[15], s16 = v[16], s17 = v[17], s18 = v[18], s19 = v[19], s20 = v[20], s21 = v[21], s22 = v[22], s23 = v[23];
    long nr = 0, ne = 0;
    for (int i = 0; i < n; i++) {
        a[i] = nr; b[i] = ne;
        nr += v[i] * s1 + s2 + k; ne += v[i + 1] ^ s3;
        s0 += s1 ^ i; s1 += s2 ^ i; s2 += s3 ^ i; s3 += s4 ^ i; s4 += s5 ^ i; s5 += s6 ^ i; s6 += s7 ^ i; s7 += s8 ^ i; s8 += s9 ^ i; s9 += s10 ^ i; s10 += s11 ^ i; s11 += s12 ^ i; s12 += s13 ^ i; s13 += s14 ^ i; s14 += s15 ^ i; s15 += s16 ^ i; s16 += s17 ^ i; s17 += s18 ^ i; s18 += s19 ^ i; s19 += s20 ^ i; s20 += s21 ^ i; s21 += s22 ^ i; s22 += s23 ^ i; s23 += s0 ^ i;
    }
    a[k] = nr; b[k] = ne;
    long t = s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11 + s12 + s13 + s14 + s15 + s16 + s17 + s18 + s19 + s20 + s21 + s22 + s23;
    for (long i = 0; i <= k; i++) t += a[i] * 7 + b[i] + s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11 + s12 + s13 + s14 + s15 + s16 + s17 + s18 + s19 + s20 + s21 + s22 + s23;
    return t;
}
int main(void)
{
    long v[80];
    for (int i = 0; i < 80; i++) v[i] = i * 37 % 101;
    printf("%ld\n", f(v, 40, 40));
    return 0;
}
