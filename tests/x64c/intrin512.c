/* x86-64 only: AVX-512 intrinsics and GNU operators on 64-byte vectors (zmm, EVEX), in functions with
   target("avx512f,avx512bw") chosen at run time; the expected output is clang's (-O2 -ffp-contract=off). A machine
   without AVX-512BW prints that it skipped, which is not the expected output. */
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

static unsigned h = 2166136261u;

static void add(const void *p, unsigned n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned k;
    for (k = 0; k < n; k++)
        h = (h ^ b[k]) * 16777619u;
}

#define T512 __attribute__((target("avx512f,avx512bw")))

T512 __attribute__((noinline)) static __m512 madd(__m512 a, __m512 b, __m512 c)
{
    return a * b + c * 0.5f - a;
}

T512 __attribute__((noinline)) static unsigned long long nz(const void *w)
{
    __m512i v = _mm512_loadu_si512(w);
    return (unsigned long long)_mm512_test_epi8_mask(v, v);
}

T512 static void run(const float *a, const unsigned char *bytes)
{
    float o[16];
    __m512 in[16], r[16];
    int k;
    for (k = 0; k < 16; k++)
        in[k] = _mm512_loadu_ps(a + (k % 4) * 16);
    /* more live vectors than registers: spills of zmm */
    for (k = 0; k < 16; k++)
        r[k] = madd(in[k], in[(k + 3) % 16], in[(k + 7) % 16]) + in[k] * in[15 - k] - in[(k + 1) % 16];
    for (k = 0; k < 16; k++) {
        _mm512_storeu_ps(o, r[k]);
        add(o, 64);
    }
    {
        __m512 z = _mm512_setzero_ps(), s = _mm512_set1_ps(2.5f), t = _mm512_fmadd_ps(in[2], s, z);
        t = _mm512_max_ps(t, _mm512_sub_ps(z, t));
        _mm512_storeu_ps(o, _mm512_sqrt_ps(t));
        add(o, 64);
        _mm512_storeu_ps(o, _mm512_div_ps(in[5], _mm512_min_ps(in[6], s)));
        add(o, 64);
    }
    {
        __m512i x = _mm512_loadu_si512(bytes), y = _mm512_add_epi32(x, _mm512_set1_epi32(7));
        y = _mm512_xor_si512(y, _mm512_add_epi64(x, x));
        y = _mm512_sub_epi8(y, _mm512_min_epu8(x, _mm512_set1_epi8(100)));
        _mm512_storeu_si512(o, _mm512_mullo_epi32(y, x));
        add(o, 64);
        _mm512_storeu_ps(o, _mm512_cvtepi32_ps(y));
        add(o, 64);
    }
    for (k = 0; k < 64; k++) {
        unsigned long long m = nz(bytes + k);
        add(&m, 8);
    }
    {
        __m512 n = -in[3];
        _mm512_storeu_ps(o, n);
        add(o, 64);
    }
}

int main(void)
{
    float a[64];
    unsigned char bytes[128];
    int k;
    for (k = 0; k < 64; k++)
        a[k] = (float)k * 0.37f - 5;
    for (k = 0; k < 128; k++)
        bytes[k] = (unsigned char)((k * 37) % 5 == 0 ? 0 : k);
    if (!__builtin_cpu_supports("avx512bw")) {
        printf("no AVX-512BW here: skipped\n");
        return 0;
    }
    run(a, bytes);
    printf("%08x\n", h);
    return 0;
}
