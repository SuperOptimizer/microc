/* x86-64 only: <immintrin.h> (AVX, AVX2, FMA, F16C, BMI2), GNU vector arithmetic on the same types, and
   vectors as arguments and results (System V: xmm / ymm registers). The expected output is what clang
   -mavx2 -mfma -mf16c -mbmi2 prints for this file. */
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

static float fa[16] = {1.5f, -2.25f, 3.0f, -0.0f, 1e10f, -7.75f, 0.5f, 100.0f, 2.5f, 3.5f, -4.5f, 5.5f, 6.25f, -8.0f, 9.0f, 0.125f};
static float fb[16] = {0.5f, 2.25f, -3.0f, 4.0f, 1e-3f, 7.75f, -0.5f, 99.0f, 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f};
static double da[8] = {1.25, -2.5, 3.75, -4.0, 1e100, -1e-100, 0.5, 6.0};
static int ia[16] = {1, -2, 3, -4, 0x7fffffff, (int)0x80000000, 255, 65536, 7, -7, 1000, -1000, 12345, -54321, 0, 42};
static int ib[16] = {5, 2, -3, -4, 1, 1, 256, -65536, 3, 3, -1, 1000, 1, 2, 0, -42};
static unsigned char ba[64], bb[64];
static unsigned short ha[16];

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

static void pf4(const char *n, __m128 v)
{
    float o[4];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 4; k++)
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
    unsigned o[8];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 8; k++)
        printf(" %08x", o[k]);
    printf("\n");
}

static void pi4(const char *n, __m128i v)
{
    unsigned o[4];
    int k;
    memcpy(o, &v, sizeof o);
    printf("%s", n);
    for (k = 0; k < 4; k++)
        printf(" %08x", o[k]);
    printf("\n");
}

/* not inlined: the vectors travel in registers */
__attribute__((noinline)) static __m256 madd_abi(__m256 a, __m256 b, __m256 c, float s, __m128 lo)
{
    return _mm256_add_ps(_mm256_fmadd_ps(a, b, c), _mm256_set_m128(lo, _mm_set1_ps(s)));
}

__attribute__((noinline)) static __m128i add4_abi(__m128i a, __m128i b)
{
    return _mm_add_epi32(a, b);
}

/* more live vectors than registers: spills of whole ymm registers */
__attribute__((noinline)) static __m256 many(const float *p)
{
    __m256 v[20], s;
    int k;
    for (k = 0; k < 20; k++)
        v[k] = _mm256_loadu_ps(p + (k & 7));
    s = _mm256_setzero_ps();
    s = _mm256_add_ps(s, _mm256_mul_ps(v[0], v[19]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[1], v[18]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[2], v[17]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[3], v[16]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[4], v[15]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[5], v[14]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[6], v[13]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[7], v[12]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[8], v[11]));
    s = _mm256_add_ps(s, _mm256_mul_ps(v[9], v[10]));
    return s;
}

/* the GNU operators on the intrinsic types, as volcomp's transforms use them */
static void gnu_ops(void)
{
    __m256 a = _mm256_loadu_ps(fa), b = _mm256_loadu_ps(fb), c;
    __m256i x = _mm256_loadu_si256((const __m256i *)ia), y = _mm256_loadu_si256((const __m256i *)ib);
    __v8si xi = (__v8si)x, yi = (__v8si)y, zi;
    __v8su xu = (__v8su)x;
    c = a * 0.5f + b;
    pf("gnu a*0.5+b", c);
    c = (a - b) / 4.0f;
    pf("gnu (a-b)/4", c);
    c = -a;
    pf("gnu -a", c);
    zi = xi + yi * 3;
    pi("gnu x+y*3", (__m256i)zi);
    zi = xi << 3;
    pi("gnu x<<3", (__m256i)zi);
    zi = xi >> 2;
    pi("gnu x>>2", (__m256i)zi);
    zi = (__v8si)(xu >> 2);
    pi("gnu xu>>2", (__m256i)zi);
    zi = xi << (yi & 7);
    pi("gnu x<<(y&7)", (__m256i)zi);
    zi = xi > yi;
    pi("gnu x>y", (__m256i)zi);
    zi = xi == yi;
    pi("gnu x==y", (__m256i)zi);
    zi = xi <= yi;
    pi("gnu x<=y", (__m256i)zi);
    zi = (__v8si)(a < b);
    pi("gnu a<b", (__m256i)zi);
    zi = ~xi ^ yi;
    pi("gnu ~x^y", (__m256i)zi);
    zi = -xi;
    pi("gnu -x", (__m256i)zi);
}

static void floats(void)
{
    __m256 a = _mm256_loadu_ps(fa), b = _mm256_loadu_ps(fb), c = _mm256_loadu_ps(fa + 8);
    __m256 m;
    float o[8];
    pf("add_ps", _mm256_add_ps(a, b));
    pf("sub_ps", _mm256_sub_ps(a, b));
    pf("mul_ps", _mm256_mul_ps(a, b));
    pf("div_ps", _mm256_div_ps(a, b));
    pf("min_ps", _mm256_min_ps(a, b));
    pf("max_ps", _mm256_max_ps(a, b));
    pf("sqrt_ps", _mm256_sqrt_ps(_mm256_max_ps(a, _mm256_setzero_ps())));
    pf("and_ps", _mm256_and_ps(a, b));
    pf("andnot_ps", _mm256_andnot_ps(a, b));
    pf("or_ps", _mm256_or_ps(a, _mm256_set1_ps(-0.0f)));
    pf("xor_ps", _mm256_xor_ps(a, _mm256_set1_ps(-0.0f)));
    pf("unpacklo_ps", _mm256_unpacklo_ps(a, b));
    pf("unpackhi_ps", _mm256_unpackhi_ps(a, b));
    pf("shuffle_ps 44", _mm256_shuffle_ps(a, b, 0x44));
    pf("shuffle_ps EE", _mm256_shuffle_ps(a, b, 0xEE));
    pf("shuffle_ps 1B", _mm256_shuffle_ps(a, b, _MM_SHUFFLE(0, 1, 2, 3)));
    pf("permute2f128 20", _mm256_permute2f128_ps(a, b, 0x20));
    pf("permute2f128 31", _mm256_permute2f128_ps(a, b, 0x31));
    m = _mm256_cmp_ps(a, b, _CMP_LT_OQ);
    pi("cmp_ps lt", _mm256_castps_si256(m));
    pi("cmp_ps ge", _mm256_castps_si256(_mm256_cmp_ps(a, b, _CMP_GE_OQ)));
    pi("cmp_ps neq", _mm256_castps_si256(_mm256_cmp_ps(a, a, _CMP_NEQ_UQ)));
    printf("movemask_ps %x\n", _mm256_movemask_ps(m));
    pf("blendv_ps", _mm256_blendv_ps(a, b, m));
    pf("blend_ps", _mm256_blend_ps(a, b, 0xa5));
    pf("fmadd_ps", _mm256_fmadd_ps(a, b, c));
    pf("fmsub_ps", _mm256_fmsub_ps(a, b, c));
    pf("fnmadd_ps", _mm256_fnmadd_ps(a, b, c));
    pf("round_ps near", _mm256_round_ps(c, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    pf("floor_ps", _mm256_floor_ps(c));
    pf("ceil_ps", _mm256_ceil_ps(c));
    pi("cvttps_epi32", _mm256_cvttps_epi32(c));
    pi("cvtps_epi32", _mm256_cvtps_epi32(c));
    pf("cvtepi32_ps", _mm256_cvtepi32_ps(_mm256_loadu_si256((const __m256i *)ia)));
    pf("setr_ps", _mm256_setr_ps(1, 2, 3, 4, 5, 6, 7, 8));
    pf("set_ps", _mm256_set_ps(1, 2, 3, 4, 5, 6, 7, 8));
    pf("castpd_ps", _mm256_castpd_ps(_mm256_set1_pd(1.0)));
    pf4("castps256_ps128", _mm256_castps256_ps128(a));
    pf4("extractf128_ps", _mm256_extractf128_ps(a, 1));
    pf("insertf128_ps", _mm256_insertf128_ps(a, _mm_set1_ps(9.0f), 0));
    pf("broadcastss", _mm256_broadcastss_ps(_mm_set1_ps(-3.5f)));
    pf("permutevar8x32_ps", _mm256_permutevar8x32_ps(a, _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0)));
    pf("permute_ps", _mm256_permute_ps(a, 0x1b));
    printf("cvtss_f32 %g %g\n", _mm256_cvtss_f32(a), _mm_cvtss_f32(_mm256_extractf128_ps(b, 1)));
    _mm256_storeu_ps(o, _mm256_add_ps(a, c));
    printf("storeu_ps %g %g\n", o[0], o[7]);
    pf("abi", madd_abi(a, b, c, 2.0f, _mm_set1_ps(0.25f)));
    pf("spills", many(fa));
    pf4("mm add_ps", _mm_add_ps(_mm_loadu_ps(fa), _mm_loadu_ps(fb)));
    pf4("mm shuffle_ps", _mm_shuffle_ps(_mm_loadu_ps(fa), _mm_loadu_ps(fb), 0x4e));
}

static void doubles(void)
{
    __m256d a = _mm256_loadu_pd(da), b = _mm256_loadu_pd(da + 4);
    int idx[4] = {3, 0, 7, 2};
    pd("add_pd", _mm256_add_pd(a, b));
    pd("mul_pd", _mm256_mul_pd(a, b));
    pd("fmadd_pd", _mm256_fmadd_pd(a, b, a));
    pd("i32gather_pd", _mm256_i32gather_pd(da, _mm_loadu_si128((const __m128i *)idx), 8));
    pd("permute4x64_pd", _mm256_permute4x64_pd(a, 0x1b));
    pd("cvtps_pd", _mm256_cvtps_pd(_mm_loadu_ps(fa)));
    pf4("cvtpd_ps", _mm256_cvtpd_ps(a));
}

static void ints(void)
{
    __m256i x = _mm256_loadu_si256((const __m256i *)ia), y = _mm256_loadu_si256((const __m256i *)ib);
    __m256i bx = _mm256_loadu_si256((const __m256i *)ba), by = _mm256_loadu_si256((const __m256i *)bb), m;
    __m128i lo = _mm256_castsi256_si128(bx);
    pi("add_epi32", _mm256_add_epi32(x, y));
    pi("sub_epi32", _mm256_sub_epi32(x, y));
    pi("add_epi16", _mm256_add_epi16(x, y));
    pi("sub_epi16", _mm256_sub_epi16(x, y));
    pi("add_epi8", _mm256_add_epi8(bx, by));
    pi("sub_epi8", _mm256_sub_epi8(bx, by));
    pi("mullo_epi32", _mm256_mullo_epi32(x, y));
    pi("mullo_epi16", _mm256_mullo_epi16(x, y));
    pi("and", _mm256_and_si256(x, y));
    pi("andnot", _mm256_andnot_si256(x, y));
    pi("or", _mm256_or_si256(x, y));
    pi("xor", _mm256_xor_si256(x, y));
    pi("cmpeq_epi32", _mm256_cmpeq_epi32(x, y));
    pi("cmpgt_epi32", _mm256_cmpgt_epi32(x, y));
    pi("cmpeq_epi16", _mm256_cmpeq_epi16(x, y));
    pi("cmpgt_epi16", _mm256_cmpgt_epi16(x, y));
    pi("cmpeq_epi8", _mm256_cmpeq_epi8(bx, by));
    pi("min_epu8", _mm256_min_epu8(bx, by));
    pi("max_epu8", _mm256_max_epu8(bx, by));
    pi("min_epi32", _mm256_min_epi32(x, y));
    pi("abs_epi16", _mm256_abs_epi16(x));
    pi("sign_epi16", _mm256_sign_epi16(x, y));
    pi("packus_epi32", _mm256_packus_epi32(x, y));
    pi("packus_epi16", _mm256_packus_epi16(x, y));
    pi("srli_epi32 5", _mm256_srli_epi32(x, 5));
    pi("slli_epi32 7", _mm256_slli_epi32(x, 7));
    pi("srai_epi32 3", _mm256_srai_epi32(x, 3));
    pi("srai_epi16 3", _mm256_srai_epi16(x, 3));
    pi("srlv_epi32", _mm256_srlv_epi32(x, _mm256_and_si256(y, _mm256_set1_epi32(31))));
    pi("cvtepu8_epi32", _mm256_cvtepu8_epi32(lo));
    pi("cvtepu8_epi16", _mm256_cvtepu8_epi16(lo));
    pi("cvtepi8_epi16", _mm256_cvtepi8_epi16(lo));
    pi4("extracti128 1", _mm256_extracti128_si256(x, 1));
    pi("inserti128", _mm256_inserti128_si256(x, lo, 1));
    pi("permutevar8x32", _mm256_permutevar8x32_epi32(x, _mm256_setr_epi32(1, 1, 0, 7, 6, 2, 3, 5)));
    pi("permute4x64", _mm256_permute4x64_epi64(x, 0xd8));
    pi("shuffle_epi8", _mm256_shuffle_epi8(bx, by));
    pi("unpacklo_epi8", _mm256_unpacklo_epi8(bx, by));
    m = _mm256_cmpgt_epi8(bx, by);
    pi("blendv_epi8", _mm256_blendv_epi8(bx, by, m));
    printf("movemask_epi8 %08x\n", (unsigned)_mm256_movemask_epi8(m));
    printf("testz %d %d\n", _mm256_testz_si256(x, y), _mm256_testz_si256(x, _mm256_setzero_si256()));
    pi("set1_epi32", _mm256_set1_epi32(-123456));
    pi("set1_epi16", _mm256_set1_epi16(-2));
    pi("set1_epi8", _mm256_set1_epi8((char)0xa7));
    pi("setr_epi32", _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8));
    pi("i32gather_epi32", _mm256_i32gather_epi32(ia, _mm256_setr_epi32(15, 0, 3, 3, 9, 1, 2, 14), 4));
    pi("mask_i32gather_epi32", _mm256_mask_i32gather_epi32(_mm256_set1_epi32(-1), ia, _mm256_setr_epi32(15, 0, 3, 3, 9, 1, 2, 14),
                                                            _mm256_setr_epi32(-1, 0, -1, 0, -1, 0, -1, 0), 4));
    pf("i32gather_ps", _mm256_i32gather_ps(fa, _mm256_setr_epi32(1, 3, 5, 7, 9, 11, 13, 15), 4));
    pf("mask_i32gather_ps", _mm256_mask_i32gather_ps(_mm256_set1_ps(7.0f), fa, _mm256_setr_epi32(1, 3, 5, 7, 9, 11, 13, 15),
                                                      _mm256_castsi256_ps(_mm256_setr_epi32(0, -1, 0, -1, 0, -1, 0, -1)), 4));
    pi4("mm cmpeq_epi8", _mm_cmpeq_epi8(lo, _mm_set1_epi8(3)));
    pi4("mm min_epu8", _mm_min_epu8(lo, _mm_set1_epi8(100)));
    pi4("mm max_epu8", _mm_max_epu8(lo, _mm_set1_epi8(100)));
    pi4("mm and", _mm_and_si128(lo, _mm_set1_epi8(0x0f)));
    pi4("mm srli_si128 3", _mm_srli_si128(lo, 3));
    printf("mm movemask_epi8 %04x\n", (unsigned)_mm_movemask_epi8(lo));
    printf("cvtsi128_si32 %08x\n", (unsigned)_mm_cvtsi128_si32(_mm_srli_si128(lo, 4)));
    pi4("cvtsi64_si128", _mm_cvtsi64_si128(0x1122334455667788LL));
    pi4("abi", add4_abi(lo, _mm_set1_epi32(1)));
    printf("extract_epi32 %d %d\n", _mm256_extract_epi32(x, 6), _mm256_extract_epi32(x, 1));
    printf("extract_epi16 %d\n", _mm256_extract_epi16(x, 13));
    pi4("loadl_epi64", _mm_loadl_epi64((const __m128i *)ia));
    pi("zero", _mm256_setzero_si256());
    pi("castps_si256", _mm256_castps_si256(_mm256_set1_ps(1.0f)));
}

static void halves(void)
{
    __m256 a = _mm256_loadu_ps(fa), back;
    __m128i h = _mm256_cvtps_ph(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    unsigned short s[8];
    int k;
    _mm_storeu_si128((__m128i *)s, h);
    printf("cvtps_ph");
    for (k = 0; k < 8; k++)
        printf(" %04x", s[k]);
    printf("\n");
    back = _mm256_cvtph_ps(h);
    pf("cvtph_ps", back);
    pf4("mm cvtph_ps", _mm_cvtph_ps(_mm_loadu_si128((const __m128i *)ha)));
    printf("cvtsh_ss %g %g\n", _cvtsh_ss(0x3c00), _cvtsh_ss(ha[3]));
    printf("cvtss_sh %04x %04x\n", (unsigned)_cvtss_sh(1.0f, 0), (unsigned)_cvtss_sh(-65504.0f, 0));
}

static void bits(void)
{
    unsigned long long m = 0xf0f0f0f00ff00ff0ULL;
    printf("pdep %016llx\n", (unsigned long long)_pdep_u64(0x123456789abcdefULL, m));
    printf("pext %016llx\n", (unsigned long long)_pext_u64(0x123456789abcdefULL, m));
    printf("pdep32 %08x\n", _pdep_u32(0x1234u, 0xff00ff00u));
    printf("bzhi %016llx\n", (unsigned long long)_bzhi_u64(~0ULL, 37));
}

/* a loop carrying vectors: phis of ymm registers */
static void loop(void)
{
    __m256 s = _mm256_setzero_ps(), t = _mm256_set1_ps(1.0f);
    __m256i c = _mm256_setzero_si256();
    int k;
    for (k = 0; k < 10; k++) {
        s = _mm256_add_ps(s, _mm256_mul_ps(t, _mm256_loadu_ps(fa + (k & 7))));
        t = _mm256_add_ps(t, _mm256_set1_ps(0.5f));
        c = _mm256_add_epi32(c, _mm256_set1_epi32(k));
        if (k == 4)
            s = _mm256_sub_ps(s, t);
    }
    pf("loop", s);
    pi("loop c", c);
}

int main(void)
{
    int k;
    for (k = 0; k < 64; k++) {
        ba[k] = (unsigned char)(k * 37 + 11);
        bb[k] = (unsigned char)(k * 91 + 200);
    }
    for (k = 0; k < 16; k++)
        ha[k] = (unsigned short)(0x3c00 + k * 0x123);
    floats();
    doubles();
    ints();
    halves();
    bits();
    gnu_ops();
    loop();
    k = __builtin_cpu_supports("avx2");
    printf("cpu_supports %s\n", k == 0 || k == 1 ? "ok" : "bad");
    return 0;
}
