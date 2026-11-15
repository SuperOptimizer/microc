/* x86-64 only: _mm*_set / setr of constant lanes (read-only data, one load) and of mixed lanes; the expected output is clang's. */
#include <immintrin.h>
#include <stdio.h>
#include <string.h>
__attribute__((noinline)) static __m256i perm(__m256i v) { return _mm256_permutevar8x32_epi32(v, _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7)); }
int main(void) {
    int a[8] = {10, 11, 12, 13, 14, 15, 16, 17}, o[8], k; float f[8]; short s[16]; char c[32]; double d[4]; long long q[4];
    _mm256_storeu_si256((__m256i *)o, perm(_mm256_loadu_si256((const __m256i *)a))); for (k = 0; k < 8; k++) printf("%d ", o[k]);
    _mm256_storeu_ps(f, _mm256_set_ps(1.5f, -2, 3, 4, 5, 6, 7, 0.25f)); for (k = 0; k < 8; k++) printf("%g ", f[k]);
    _mm256_storeu_si256((__m256i *)s, _mm256_setr_epi16(1, -2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, -15, 16)); for (k = 0; k < 16; k++) printf("%d ", s[k]);
    _mm256_storeu_si256((__m256i *)c, _mm256_set_epi8(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,-32)); for (k = 0; k < 32; k++) printf("%d ", c[k]);
    _mm256_storeu_pd(d, _mm256_setr_pd(1.25, -2.5, 1e300, 0)); for (k = 0; k < 4; k++) printf("%g ", d[k]);
    _mm256_storeu_si256((__m256i *)q, _mm256_set_epi64x(-1, 2, 3, 0x123456789ll)); for (k = 0; k < 4; k++) printf("%lld ", q[k]);
    _mm_storeu_ps(f, _mm_setr_ps(1, 2, 3, a[0])); for (k = 0; k < 4; k++) printf("%g ", f[k]);
    printf("\n"); return 0; }
