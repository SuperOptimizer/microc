/* <immintrin.h> parity: every intrinsic ufsm uses, over random and special inputs (NaN, -0, infinities, values out
   of range for the conversions, denormals, every _CMP_ predicate, every rounding of cvtps_ph and round_ps). One
   line per intrinsic: a hash of all its results; the expected output is clang -O2 -mavx2 -mfma -mf16c -mbmi2. */
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

#define N 400

typedef union V {
    __m256 f;
    __m256i i;
    __m256d d;
    __m128i q[2];
    float fl[8];
    unsigned u[8];
    unsigned char b[32];
} V;

static V A[N], B[N], C[N];
static unsigned long long rng = 0x9e3779b97f4a7c15ull;
static unsigned hash;
static float table_f[1024];
static int table_i[1024];
static double table_d[1024];

static unsigned long long rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static const float special[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, -1.5f, 3.5f, 1e-40f, -1e-40f, 1e-8f,
                                65504.0f, 65519.0f, 65520.0f, 1e5f, -1e5f, 2147483520.0f, 2147483648.0f, -2147483648.0f,
                                -2147483904.0f, 3e9f, -3e9f, 3.4e38f, 6e-8f, 3e-8f, 1.0f / 3.0f, 255.5f, 0.49999997f};

static void fill(V *v, int k, int kind)
{
    int j;
    for (j = 0; j < 8; j++) {
        unsigned long long r = rnd();
        switch ((kind + j) % 6) {
        case 0: v->u[j] = (unsigned)r; break;
        case 1: v->fl[j] = (float)((int)(r % 2001) - 1000) * 0.25f; break;
        case 2: v->fl[j] = special[r % (sizeof special / sizeof special[0])]; break;
        case 3: v->u[j] = (r & 1 ? 0x7fc00000u : 0xff800000u) | (unsigned)(r >> 8 & 1 ? 0x7f800000u : 0) | (unsigned)(r >> 12 & 0x3ff); break;
        case 4: v->u[j] = (unsigned)(r % 7) * 0x01010101u * (unsigned)(r >> 20 & 0xff); break;
        default: v->fl[j] = (float)(r % 100000) / 7.0f - 5000.0f; break;
        }
    }
    (void)k;
}

static void add(const void *p, unsigned n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned k;
    for (k = 0; k < n; k++)
        hash = (hash ^ b[k]) * 16777619u;
}

static void put(const char *name)
{
    printf("%s %08x\n", name, hash);
    hash = 2166136261u;
}

/* r = expr over a = A[k], b = B[k], c = C[k] (as each type wants) */
#define T(name, rty, expr)                                                                                                   \
    do {                                                                                                                     \
        int k;                                                                                                               \
        for (k = 0; k < N; k++) {                                                                                            \
            __m256 af = A[k].f, bf = B[k].f, cf = C[k].f;                                                                    \
            __m256i ai = A[k].i, bi = B[k].i, ci = C[k].i;                                                                   \
            __m128i aq = A[k].q[0], bq = B[k].q[0];                                                                          \
            rty r = expr;                                                                                                    \
            (void)af, (void)bf, (void)cf, (void)ai, (void)bi, (void)ci, (void)aq, (void)bq;                                  \
            add(&r, sizeof r);                                                                                               \
        }                                                                                                                    \
        put(name);                                                                                                           \
    } while (0)

#define CMP(p) T("cmp_ps " #p, __m256, _mm256_cmp_ps(af, bf, p))
#define CMPA(p) T("cmp_ps aa " #p, __m256, _mm256_cmp_ps(af, af, p))
#define PH(m) T("cvtps_ph " #m, __m128i, _mm256_cvtps_ph(af, m))
#define RND(m) T("round_ps " #m, __m256, _mm256_round_ps(af, m))

static void f32(void)
{
    T("add_ps", __m256, _mm256_add_ps(af, bf));
    T("sub_ps", __m256, _mm256_sub_ps(af, bf));
    T("mul_ps", __m256, _mm256_mul_ps(af, bf));
    T("div_ps", __m256, _mm256_div_ps(af, bf));
    T("max_ps", __m256, _mm256_max_ps(af, bf));
    T("min_ps", __m256, _mm256_min_ps(af, bf));
    T("sqrt_ps", __m256, _mm256_sqrt_ps(af));
    T("floor_ps", __m256, _mm256_floor_ps(af));
    T("fmadd_ps", __m256, _mm256_fmadd_ps(af, bf, cf));
    T("and_ps", __m256, _mm256_and_ps(af, bf));
    T("andnot_ps", __m256, _mm256_andnot_ps(af, bf));
    T("or_ps", __m256, _mm256_or_ps(af, bf));
    T("xor_ps", __m256, _mm256_xor_ps(af, bf));
    T("blendv_ps", __m256, _mm256_blendv_ps(af, bf, cf));
    T("unpackhi_ps", __m256, _mm256_unpackhi_ps(af, bf));
    T("unpacklo_ps", __m256, _mm256_unpacklo_ps(af, bf));
    T("shuffle_ps 0x1b", __m256, _mm256_shuffle_ps(af, bf, 0x1b));
    T("shuffle_ps 0xe4", __m256, _mm256_shuffle_ps(af, bf, 0xe4));
    T("shuffle_ps 0x88", __m256, _mm256_shuffle_ps(af, bf, 0x88));
    T("permute2f128_ps 0x21", __m256, _mm256_permute2f128_ps(af, bf, 0x21));
    T("permute2f128_ps 0x30", __m256, _mm256_permute2f128_ps(af, bf, 0x30));
    T("permute2f128_ps 0x08", __m256, _mm256_permute2f128_ps(af, bf, 0x08));
    T("permute2f128_ps 0x83", __m256, _mm256_permute2f128_ps(af, bf, 0x83));
    T("movemask_ps", int, _mm256_movemask_ps(af));
    T("cvtepi32_ps", __m256, _mm256_cvtepi32_ps(ai));
    T("cvtps_epi32", __m256i, _mm256_cvtps_epi32(af));
    T("cvttps_epi32", __m256i, _mm256_cvttps_epi32(af));
    T("castpd_ps", __m256, _mm256_castpd_ps(A[k].d));
    T("castps_si256", __m256i, _mm256_castps_si256(af));
    T("castsi256_ps", __m256, _mm256_castsi256_ps(ai));
    T("castsi256_si128", __m128i, _mm256_castsi256_si128(ai));
    T("set1_ps", __m256, _mm256_set1_ps(A[k].fl[3]));
    T("setr_ps", __m256, _mm256_setr_ps(A[k].fl[0], B[k].fl[1], A[k].fl[2], B[k].fl[3], A[k].fl[4], B[k].fl[5], C[k].fl[6], A[k].fl[7]));
    T("setzero_ps", __m256, _mm256_add_ps(_mm256_setzero_ps(), af));
    T("loadu_ps", __m256, _mm256_loadu_ps(A[k].fl));
    CMP(0); CMP(1); CMP(2); CMP(3); CMP(4); CMP(5); CMP(6); CMP(7);
    CMP(8); CMP(9); CMP(10); CMP(11); CMP(12); CMP(13); CMP(14); CMP(15);
    CMP(16); CMP(17); CMP(18); CMP(19); CMP(20); CMP(21); CMP(22); CMP(23);
    CMP(24); CMP(25); CMP(26); CMP(27); CMP(28); CMP(29); CMP(30); CMP(31);
    CMPA(0); CMPA(3); CMPA(4); CMPA(7); CMPA(13); CMPA(31);
    PH(0); PH(1); PH(2); PH(3); PH(4); PH(8); PH(9); PH(10); PH(11); PH(12);
    RND(0); RND(1); RND(2); RND(3); RND(4); RND(8); RND(9); RND(10); RND(11); RND(12);
}

static void i256(void)
{
    T("abs_epi16", __m256i, _mm256_abs_epi16(ai));
    T("add_epi8", __m256i, _mm256_add_epi8(ai, bi));
    T("add_epi16", __m256i, _mm256_add_epi16(ai, bi));
    T("add_epi32", __m256i, _mm256_add_epi32(ai, bi));
    T("sub_epi8", __m256i, _mm256_sub_epi8(ai, bi));
    T("sub_epi16", __m256i, _mm256_sub_epi16(ai, bi));
    T("sub_epi32", __m256i, _mm256_sub_epi32(ai, bi));
    T("mullo_epi16", __m256i, _mm256_mullo_epi16(ai, bi));
    T("mullo_epi32", __m256i, _mm256_mullo_epi32(ai, bi));
    T("and_si256", __m256i, _mm256_and_si256(ai, bi));
    T("andnot_si256", __m256i, _mm256_andnot_si256(ai, bi));
    T("or_si256", __m256i, _mm256_or_si256(ai, bi));
    T("xor_si256", __m256i, _mm256_xor_si256(ai, bi));
    T("blendv_epi8", __m256i, _mm256_blendv_epi8(ai, bi, ci));
    T("cmpeq_epi8", __m256i, _mm256_cmpeq_epi8(ai, bi));
    T("cmpeq_epi8 aa", __m256i, _mm256_cmpeq_epi8(ai, _mm256_or_si256(ai, _mm256_and_si256(bi, ci))));
    T("cmpeq_epi16", __m256i, _mm256_cmpeq_epi16(ai, _mm256_and_si256(ai, bi)));
    T("cmpeq_epi32", __m256i, _mm256_cmpeq_epi32(ai, _mm256_and_si256(ai, bi)));
    T("cmpgt_epi16", __m256i, _mm256_cmpgt_epi16(ai, bi));
    T("cmpgt_epi32", __m256i, _mm256_cmpgt_epi32(ai, bi));
    T("max_epu8", __m256i, _mm256_max_epu8(ai, bi));
    T("min_epu8", __m256i, _mm256_min_epu8(ai, bi));
    T("min_epi32", __m256i, _mm256_min_epi32(ai, bi));
    T("movemask_epi8", int, _mm256_movemask_epi8(ai));
    T("packus_epi16", __m256i, _mm256_packus_epi16(ai, bi));
    T("packus_epi32", __m256i, _mm256_packus_epi32(ai, bi));
    T("sign_epi16", __m256i, _mm256_sign_epi16(ai, bi));
    T("slli_epi32 0", __m256i, _mm256_slli_epi32(ai, 0));
    T("slli_epi32 7", __m256i, _mm256_slli_epi32(ai, 7));
    T("slli_epi32 31", __m256i, _mm256_slli_epi32(ai, 31));
    T("slli_epi32 32", __m256i, _mm256_slli_epi32(ai, 32));
    T("srli_epi32 9", __m256i, _mm256_srli_epi32(ai, 9));
    T("srli_epi32 40", __m256i, _mm256_srli_epi32(ai, 40));
    T("srai_epi32 5", __m256i, _mm256_srai_epi32(ai, 5));
    T("srai_epi32 33", __m256i, _mm256_srai_epi32(ai, 33));
    T("srai_epi16 3", __m256i, _mm256_srai_epi16(ai, 3));
    T("srai_epi16 15", __m256i, _mm256_srai_epi16(ai, 15));
    T("srai_epi16 16", __m256i, _mm256_srai_epi16(ai, 16));
    T("srlv_epi32", __m256i, _mm256_srlv_epi32(ai, _mm256_and_si256(bi, _mm256_set1_epi32(63))));
    T("cvtepi8_epi16", __m256i, _mm256_cvtepi8_epi16(aq));
    T("cvtepu8_epi16", __m256i, _mm256_cvtepu8_epi16(aq));
    T("cvtepu8_epi32", __m256i, _mm256_cvtepu8_epi32(aq));
    T("extracti128 0", __m128i, _mm256_extracti128_si256(ai, 0));
    T("extracti128 1", __m128i, _mm256_extracti128_si256(ai, 1));
    T("permute4x64 0x4e", __m256i, _mm256_permute4x64_epi64(ai, 0x4e));
    T("permute4x64 0xd8", __m256i, _mm256_permute4x64_epi64(ai, 0xd8));
    T("permute4x64 0x1b", __m256i, _mm256_permute4x64_epi64(ai, 0x1b));
    T("permutevar8x32", __m256i, _mm256_permutevar8x32_epi32(ai, bi));
    T("set1_epi8", __m256i, _mm256_set1_epi8((char)A[k].b[5]));
    T("set1_epi16", __m256i, _mm256_set1_epi16((short)A[k].u[2]));
    T("set1_epi32", __m256i, _mm256_set1_epi32((int)A[k].u[1]));
    T("setr_epi32", __m256i, _mm256_setr_epi32((int)A[k].u[0], (int)B[k].u[1], 3, -4, (int)A[k].u[4], 0, (int)C[k].u[6], 7));
    T("setzero_si256", __m256i, _mm256_or_si256(_mm256_setzero_si256(), ai));
    T("loadu_si256", __m256i, _mm256_loadu_si256((const __m256i *)(A[k].b)));
    T("testz_si256", int, _mm256_testz_si256(ai, bi));
    T("testz_si256 0", int, _mm256_testz_si256(ai, _mm256_andnot_si256(ai, bi)));
    T("i32gather_epi32", __m256i, _mm256_i32gather_epi32(table_i, _mm256_and_si256(ai, _mm256_set1_epi32(255)), 4));
    T("i32gather_ps", __m256, _mm256_i32gather_ps(table_f, _mm256_and_si256(ai, _mm256_set1_epi32(511)), 4));
    T("i32gather_ps 2", __m256, _mm256_i32gather_ps(table_f, _mm256_and_si256(ai, _mm256_set1_epi32(511)), 2));
    T("i32gather_ps 8", __m256, _mm256_i32gather_ps(table_f, _mm256_and_si256(ai, _mm256_set1_epi32(127)), 8));
    T("mask_i32gather_epi32", __m256i,
      _mm256_mask_i32gather_epi32(bi, table_i, _mm256_and_si256(ai, _mm256_set1_epi32(255)), ci, 4));
    T("mask_i32gather_ps", __m256,
      _mm256_mask_i32gather_ps(bf, table_f, _mm256_and_si256(ai, _mm256_set1_epi32(255)), cf, 4));
    T("i32gather_pd", __m256d, _mm256_i32gather_pd(table_d, _mm256_castsi256_si128(_mm256_and_si256(ai, _mm256_set1_epi32(127))), 8));
    {
        int k;
        for (k = 0; k < N; k++) {
            V o;
            memset(&o, 0x5a, sizeof o);
            _mm256_storeu_ps(o.fl, A[k].f);
            add(&o, sizeof o);
            _mm256_storeu_si256((__m256i *)o.b, B[k].i);
            add(&o, sizeof o);
            _mm_storeu_si128((__m128i *)(o.b + 3), C[k].q[1]);
            add(&o, sizeof o);
        }
        put("storeu");
    }
}

static void i128(void)
{
    T("mm_and_si128", __m128i, _mm_and_si128(aq, bq));
    T("mm_cmpeq_epi8", __m128i, _mm_cmpeq_epi8(aq, _mm_and_si128(aq, bq)));
    T("mm_cvtsi128_si32", int, _mm_cvtsi128_si32(aq));
    T("mm_cvtsi64_si128", __m128i, _mm_cvtsi64_si128((long long)((unsigned long long)A[k].u[0] << 32 | B[k].u[1])));
    T("mm_loadu_si128", __m128i, _mm_loadu_si128((const __m128i *)(A[k].b + 5)));
    T("mm_max_epu8", __m128i, _mm_max_epu8(aq, bq));
    T("mm_min_epu8", __m128i, _mm_min_epu8(aq, bq));
    T("mm_movemask_epi8", int, _mm_movemask_epi8(aq));
    T("mm_set1_epi8", __m128i, _mm_set1_epi8((char)A[k].b[9]));
    T("mm_setzero_si128", __m128i, _mm_and_si128(_mm_setzero_si128(), aq));
    T("mm_srli_si128 1", __m128i, _mm_srli_si128(aq, 1));
    T("mm_srli_si128 8", __m128i, _mm_srli_si128(aq, 8));
    T("mm_srli_si128 15", __m128i, _mm_srli_si128(aq, 15));
    T("mm_srli_si128 16", __m128i, _mm_srli_si128(aq, 16));
    T("cvtsh_ss", float, _cvtsh_ss((unsigned short)A[k].u[0]));
    T("cvtss_sh 0", unsigned short, _cvtss_sh(A[k].fl[0], 0));
    T("cvtss_sh 3", unsigned short, _cvtss_sh(A[k].fl[1], 3));
    T("pdep_u64", unsigned long long, _pdep_u64((unsigned long long)A[k].u[0] << 32 | A[k].u[1], (unsigned long long)B[k].u[0] << 32 | B[k].u[1]));
}

int main(void)
{
    int k;
    for (k = 0; k < N; k++) {
        fill(&A[k], k, k);
        fill(&B[k], k, k + 1);
        fill(&C[k], k, k + 2);
        if (k % 5 == 0)
            B[k] = A[k]; /* equal operands */
        if (k % 7 == 0)
            C[k].u[k % 8] = A[k].u[k % 8] ^ 0x80000000u;
    }
    for (k = 0; k < 1024; k++) {
        table_f[k] = (float)k * 0.5f - 7.0f;
        table_i[k] = (int)(rnd() >> 7);
        table_d[k] = (double)k * 1.25 - 3.0;
    }
    hash = 2166136261u;
    f32();
    i256();
    i128();
    return 0;
}
