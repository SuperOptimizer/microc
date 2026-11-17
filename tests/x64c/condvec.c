/* x86-64 only: vectors chosen by ?: (each arm's value copied into one temporary, not chosen by its address):
   an arm that is a computed value, a load, an array element, nested choices, a choice inside a short loop and
   __m128 / __m256i / __m256d beside __m256. Small whole numbers: every result is exact. */
#include <immintrin.h>
#include <stdio.h>

static float fa[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static int ia[8] = {1, 2, 3, 4, 5, 6, 7, 8};

static void pf(const char *n, __m256 v)
{
    float o[8];
    int k;
    _mm256_storeu_ps(o, v);
    printf("%s", n);
    for (k = 0; k < 8; k++)
        printf(" %g", o[k]);
    printf("\n");
}

__attribute__((noinline)) static __m256 pick(int c, __m256 a, __m256 b, const float *p)
{
    const __m256 x = c ? _mm256_mul_ps(a, b) : _mm256_loadu_ps(p);
    return _mm256_add_ps(x, a);
}

__attribute__((noinline)) static __m256 nest(int c, int d, __m256 a, const float *p)
{
    return c ? d ? _mm256_add_ps(a, a) : _mm256_loadu_ps(p + 8) : d ? a : _mm256_sub_ps(a, _mm256_loadu_ps(p));
}

__attribute__((noinline)) static __m256 corners(const __m256 *F, const __m256 *G)
{
    __m256 s = _mm256_setzero_ps();
    int pr;
    for (pr = 0; pr < 4; pr++) {
        const int bz = pr >> 1, by = pr & 1;
        s = _mm256_add_ps(s, _mm256_mul_ps(bz ? F[0] : G[0], by ? F[1] : G[1]));
    }
    return s;
}

__attribute__((noinline)) static int others(int c, const int *p, double z)
{
    const __m256i i = c ? _mm256_loadu_si256((const __m256i *)p) : _mm256_set1_epi32(3);
    const __m256d d = c ? _mm256_set1_pd(z) : _mm256_set1_pd(z + 1);
    const __m128 h = c ? _mm_set1_ps(2) : _mm_set1_ps(5);
    int o[8];
    double e[4];
    float g[4];
    _mm256_storeu_si256((__m256i *)o, _mm256_add_epi32(i, i));
    _mm256_storeu_pd(e, d);
    _mm_storeu_ps(g, h);
    return o[0] + o[7] + (int)e[2] + (int)g[3];
}

int main(void)
{
    const __m256 a = _mm256_loadu_ps(fa), b = _mm256_loadu_ps(fa + 8);
    __m256 F[2], G[2];
    int c;
    F[0] = a;
    F[1] = b;
    G[0] = _mm256_set1_ps(1);
    G[1] = _mm256_set1_ps(2);
    for (c = 0; c < 2; c++) {
        pf("pick", pick(c, a, b, fa + 4));
        pf("nest0", nest(c, 0, a, fa));
        pf("nest1", nest(c, 1, a, fa));
        printf("others %d\n", others(c, ia, 7.0));
    }
    pf("corners", corners(F, G));
    return 0;
}
