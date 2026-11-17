/* x86-64 only: more vectors alive at once than ymm0-ymm12 hold. With AVX-512 (-march=native on such a processor,
   -march=x86-64-v4) ymm16-ymm31 take the rest, each instruction on them in its EVEX encoding; the comparisons,
   blends by a mask, horizontal adds, gathers and tests have none and keep their operands below ymm16. Floats,
   doubles and integers of every lane width, 128 and 256 bits mixed, scalars beside them, and vectors alive across
   a call. The values are small whole numbers, so every sum and product is exact. The expected output is what gcc
   -O1 -mavx2 -mfma prints for this file. */
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

static float fa[72];
static double da[40];
static int ia[72];
static unsigned char ba[72];
static short ha[72];

static void pf(const char *n, __m256 v)
{
    float o[8];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 8; k++)
        printf(" %g", o[k]);
    printf("\n");
}

static void pd(const char *n, __m256d v)
{
    double o[4];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 4; k++)
        printf(" %g", o[k]);
    printf("\n");
}

static void pi(const char *n, __m256i v)
{
    int o[8];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 8; k++)
        printf(" %d", o[k]);
    printf("\n");
}

__attribute__((noinline)) static void fvec(int n)
{
    const __m256 two = _mm256_set1_ps(2.0f), sign = _mm256_set1_ps(-0.0f), wrap = _mm256_set1_ps(1024.0f);
    const __m256i idx = _mm256_setr_epi32(7, 5, 3, 1, 6, 4, 2, 0);
    __m256 a0 = _mm256_loadu_ps(fa), a1 = _mm256_loadu_ps(fa + 8), a2 = _mm256_loadu_ps(fa + 16), a3 = _mm256_loadu_ps(fa + 24);
    __m256 a4 = _mm256_loadu_ps(fa + 32), a5 = _mm256_loadu_ps(fa + 40), a6 = _mm256_loadu_ps(fa + 48), a7 = _mm256_loadu_ps(fa + 56);
    __m256 a8 = a0, a9 = a1, a10 = a2, a11 = a3, a12 = a4, a13 = a5, a14 = a6, a15 = a7, a16 = a0, a17 = a1, a18 = a2, a19 = a3;
    __m256 a20 = a4, a21 = a5, a22 = a6, a23 = a7, a24 = a0, a25 = a1, a26 = a2, a27 = a3, a28 = a4, a29 = a5;
    float s = 1.0f, t = 3.0f;
    int i;
    for (i = 0; i < n; i++) {
        __m256 x = _mm256_loadu_ps(fa + 8 * (i & 7) + (i & 3)), m;
        a0 = _mm256_add_ps(a0, x);
        a1 = _mm256_sub_ps(a1, x);
        /* (x less 1024 times the whole part of x / 1024: nothing outgrows a float's 24 bits) */
        a2 = _mm256_fmadd_ps(x, a8, a2);
        a2 = _mm256_fnmadd_ps(_mm256_round_ps(_mm256_div_ps(a2, wrap), _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC), wrap, a2);
        a3 = _mm256_max_ps(a3, x);
        a4 = _mm256_min_ps(a4, x);
        m = _mm256_cmp_ps(x, a1, _CMP_LT_OQ);
        a5 = _mm256_add_ps(a5, _mm256_and_ps(m, two));
        a6 = _mm256_blendv_ps(a6, x, _mm256_cmp_ps(a1, a0, _CMP_GT_OQ));
        a7 = _mm256_xor_ps(a7, sign);
        a8 = _mm256_andnot_ps(sign, a1);
        a9 = _mm256_mul_ps(_mm256_set1_ps(s), x);
        a10 = _mm256_div_ps(_mm256_add_ps(a10, x), two);
        a11 = _mm256_sqrt_ps(_mm256_mul_ps(x, x));
        a12 = _mm256_floor_ps(_mm256_mul_ps(a10, two));
        a13 = _mm256_cvtepi32_ps(_mm256_cvttps_epi32(a10));
        a14 = _mm256_permute_ps(a0, 0x1b);
        a15 = _mm256_shuffle_ps(a1, a3, 0x4e);
        a16 = _mm256_unpacklo_ps(a3, a4);
        a17 = _mm256_unpackhi_ps(a3, a4);
        a18 = _mm256_permutevar8x32_ps(a0, idx);
        a19 = _mm256_permute2f128_ps(a0, a1, 0x21);
        a20 = _mm256_insertf128_ps(a20, _mm256_extractf128_ps(a0, 1), 0);
        a21 = _mm256_broadcastss_ps(_mm256_castps256_ps128(a1));
        a22 = _mm256_add_ps(a22, _mm256_set1_ps((float)i + t));
        a23 = _mm256_fnmadd_ps(x, two, a23);
        a24 = _mm256_fmsub_ps(x, two, a24);
        a25 = _mm256_hadd_ps(a0, a1);
        a26 = _mm256_blend_ps(a0, a1, 0x5a);
        a27 = _mm256_moveldup_ps(a0);
        a28 = _mm256_movehdup_ps(a1);
        a29 = _mm256_i32gather_ps(fa, _mm256_and_si256(_mm256_cvttps_epi32(a11), _mm256_set1_epi32(63)), 4);
        s = s + _mm_cvtss_f32(_mm256_castps256_ps128(a11));
        t = t * 2.0f - _mm256_cvtss_f32(a4) - 3.0f;
        if (s > 512.0f)
            s -= 512.0f;
        if (t > 64.0f || t < -64.0f)
            t = 1.0f;
    }
    pf("a0", a0); pf("a1", a1); pf("a2", a2); pf("a3", a3); pf("a4", a4); pf("a5", a5); pf("a6", a6); pf("a7", a7);
    pf("a8", a8); pf("a9", a9); pf("a10", a10); pf("a11", a11); pf("a12", a12); pf("a13", a13); pf("a14", a14);
    pf("a15", a15); pf("a16", a16); pf("a17", a17); pf("a18", a18); pf("a19", a19); pf("a20", a20); pf("a21", a21);
    pf("a22", a22); pf("a23", a23); pf("a24", a24); pf("a25", a25); pf("a26", a26); pf("a27", a27); pf("a28", a28);
    pf("a29", a29);
    printf("s %g t %g\n", (double)s, (double)t);
}

__attribute__((noinline)) static void ivec(int n)
{
    const __m256i one = _mm256_set1_epi32(1), m255 = _mm256_set1_epi32(255);
    const __m256i sh = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i bidx = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                          13, 14, 15);
    __m256i b0 = _mm256_loadu_si256((const __m256i *)ia), b1 = _mm256_loadu_si256((const __m256i *)(ia + 8));
    __m256i b2 = _mm256_loadu_si256((const __m256i *)(ia + 16)), b3 = _mm256_loadu_si256((const __m256i *)(ia + 24));
    __m256i b4 = b0, b5 = b1, b6 = b2, b7 = b3, b8 = b0, b9 = b1, b10 = b2, b11 = b3, b12 = b0, b13 = b1, b14 = b2, b15 = b3;
    __m256i b16 = b0, b17 = b1, b18 = b2, b19 = b3, b20 = b0, b21 = b1, b22 = b2, b23 = b3, b24 = b0, b25 = b1, b26 = b2, b27 = b3;
    __m256i b28 = b0, b29 = b1, b30 = b2, b31 = b3;
    long long q = 5;
    int i, msk = 0;
    for (i = 0; i < n; i++) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(ia + 8 * (i & 7) + (i & 3)));
        __m256i y = _mm256_loadu_si256((const __m256i *)(ba + 4 * (i & 7)));
        __m256i z = _mm256_loadu_si256((const __m256i *)(ha + 2 * (i & 7)));
        b0 = _mm256_add_epi32(b0, x);
        b1 = _mm256_sub_epi32(b1, x);
        b2 = _mm256_mullo_epi32(b2, _mm256_or_si256(x, one));
        b3 = _mm256_and_si256(_mm256_add_epi32(b3, x), m255);
        b4 = _mm256_xor_si256(b4, x);
        b5 = _mm256_andnot_si256(x, b0);
        b6 = _mm256_slli_epi32(x, 3);
        b7 = _mm256_srli_epi32(x, 5);
        b8 = _mm256_srai_epi32(x, 2);
        b9 = _mm256_sllv_epi32(x, sh);
        b10 = _mm256_srlv_epi32(x, sh);
        b11 = _mm256_srav_epi32(x, sh);
        b12 = _mm256_max_epi32(b12, x);
        b13 = _mm256_min_epi32(b13, x);
        b14 = _mm256_abs_epi32(b1);
        b15 = _mm256_shuffle_epi8(y, bidx);
        b16 = _mm256_unpacklo_epi16(z, y);
        b17 = _mm256_packs_epi32(x, b1);
        b18 = _mm256_cvtepu8_epi32(_mm256_castsi256_si128(y));
        b19 = _mm256_permutevar8x32_epi32(x, sh);
        b20 = _mm256_permute4x64_epi64(b0, 0x1b);
        b21 = _mm256_alignr_epi8(x, b0, 4);
        b22 = _mm256_add_epi16(b22, z);
        b23 = _mm256_adds_epu8(b23, y);
        b24 = _mm256_blendv_epi8(b24, x, _mm256_cmpgt_epi32(x, b0));
        b25 = _mm256_sub_epi32(b25, _mm256_cmpeq_epi32(_mm256_and_si256(x, one), one));
        b26 = _mm256_add_epi64(b26, _mm256_set1_epi64x(q));
        b27 = _mm256_inserti128_si256(b27, _mm256_extracti128_si256(b0, 1), 0);
        b28 = _mm256_mullo_epi16(z, _mm256_set1_epi16((short)(i + 3)));
        b29 = _mm256_madd_epi16(z, z);
        b30 = _mm256_i32gather_epi32(ia, _mm256_and_si256(x, _mm256_set1_epi32(63)), 4);
        b31 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(z));
        msk ^= _mm256_movemask_epi8(_mm256_cmpgt_epi8(y, b23)) + _mm256_testz_si256(b3, one);
        q = q * 3 + _mm256_extract_epi32(b0, 5) + _mm_cvtsi128_si32(_mm256_castsi256_si128(b3));
    }
    pi("b0", b0); pi("b1", b1); pi("b2", b2); pi("b3", b3); pi("b4", b4); pi("b5", b5); pi("b6", b6); pi("b7", b7);
    pi("b8", b8); pi("b9", b9); pi("b10", b10); pi("b11", b11); pi("b12", b12); pi("b13", b13); pi("b14", b14);
    pi("b15", b15); pi("b16", b16); pi("b17", b17); pi("b18", b18); pi("b19", b19); pi("b20", b20); pi("b21", b21);
    pi("b22", b22); pi("b23", b23); pi("b24", b24); pi("b25", b25); pi("b26", b26); pi("b27", b27); pi("b28", b28);
    pi("b29", b29); pi("b30", b30); pi("b31", b31);
    printf("msk %d q %lld\n", msk, q);
}

__attribute__((noinline)) static __m256d dstep(__m256d a, __m256d b, double k)
{
    return _mm256_fmadd_pd(a, _mm256_set1_pd(k), b);
}

/* doubles, 128 bits beside 256, and eighteen vectors alive between two calls (the others across them) */
__attribute__((noinline)) static void dvec(int n)
{
    __m256d c0 = _mm256_loadu_pd(da), c1 = _mm256_loadu_pd(da + 4), c2 = _mm256_loadu_pd(da + 8), c3 = _mm256_loadu_pd(da + 12);
    __m256d c4 = _mm256_loadu_pd(da + 16), c5 = _mm256_loadu_pd(da + 20), acc = _mm256_setzero_pd();
    __m128 h = _mm_loadu_ps(fa + 3);
    __m128d g = _mm_loadu_pd(da + 9);
    int i;
    for (i = 0; i < n; i++) {
        __m256d x = _mm256_loadu_pd(da + 4 * (i & 7));
        __m256d t0 = _mm256_add_pd(c0, x), t1 = _mm256_sub_pd(c1, x), t2 = _mm256_mul_pd(c2, x), t3 = _mm256_max_pd(c3, x);
        __m256d t4 = _mm256_min_pd(c4, x), t5 = _mm256_unpacklo_pd(c5, x), t6 = _mm256_unpackhi_pd(c0, x);
        __m256d t7 = _mm256_shuffle_pd(c1, x, 5), t8 = _mm256_permute4x64_pd(x, 0x1b), t9 = _mm256_cvtps_pd(h);
        __m256d t10 = _mm256_cvtepi32_pd(_mm256_cvttpd_epi32(x)), t11 = _mm256_sqrt_pd(_mm256_mul_pd(x, x));
        __m256d t12 = _mm256_floor_pd(_mm256_mul_pd(x, _mm256_set1_pd(0.5))), t13 = _mm256_broadcastsd_pd(g);
        __m256d t14 = _mm256_insertf128_pd(x, g, 1), t15 = _mm256_andnot_pd(_mm256_set1_pd(-0.0), t1);
        __m256d t16 = _mm256_xor_pd(t0, _mm256_set1_pd(-0.0)), t17 = _mm256_fnmadd_pd(x, _mm256_set1_pd(2.0), c5);
        __m256d u = _mm256_add_pd(_mm256_add_pd(_mm256_add_pd(t0, t1), _mm256_add_pd(t2, t3)),
                                  _mm256_add_pd(_mm256_add_pd(t4, t5), _mm256_add_pd(t6, t7)));
        __m256d v = _mm256_add_pd(_mm256_add_pd(_mm256_add_pd(t8, t9), _mm256_add_pd(t10, t11)),
                                  _mm256_add_pd(_mm256_add_pd(t12, t13), _mm256_add_pd(t14, t15)));
        __m256d w = dstep(_mm256_add_pd(u, t16), _mm256_add_pd(v, t17), (double)(i & 3));
        acc = _mm256_add_pd(acc, _mm256_add_pd(w, _mm256_add_pd(t0, t17)));
        c0 = _mm256_sub_pd(c0, t12);
        c1 = _mm256_add_pd(c1, t10);
        c2 = _mm256_min_pd(_mm256_add_pd(c2, t13), _mm256_set1_pd(64.0));
        c3 = _mm256_add_pd(c3, t8);
        c4 = _mm256_sub_pd(c4, t11);
        c5 = dstep(c5, t9, 1.0);
        h = _mm_add_ps(h, _mm256_cvtpd_ps(_mm256_cvtepi32_pd(_mm_set1_epi32(i))));
        g = _mm_add_pd(g, _mm256_castpd256_pd128(t12));
    }
    pd("acc", acc); pd("c0", c0); pd("c1", c1); pd("c2", c2); pd("c3", c3); pd("c4", c4); pd("c5", c5);
    pd("g", _mm256_castpd128_pd256(_mm_add_pd(g, _mm_setzero_pd())));
    pf("h", _mm256_insertf128_ps(_mm256_setzero_ps(), h, 0));
}

int main(void)
{
    int i;
    for (i = 0; i < 72; i++) {
        fa[i] = (float)(i * 29 % 61 - 30);
        ia[i] = i * 1237 % 2003 - 1000;
        ba[i] = (unsigned char)(i * 37 + 11);
        ha[i] = (short)(i * 113 % 499 - 250);
    }
    for (i = 0; i < 40; i++)
        da[i] = (double)(i * 17 % 23 - 11);
    fvec(21);
    ivec(19);
    dvec(13);
    return 0;
}
