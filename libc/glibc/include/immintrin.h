/* immintrin.h: the x86 SSE2 / SSE4.1 / AVX / AVX2 / FMA / F16C / BMI2 (and a few AVX-512) intrinsics, written over GNU vector
   types, element by element, with the instructions' exact results: per-128-bit-lane behaviour of the AVX2 packs, shuffles and
   unpacks, sign-bit selection in the blends and gathers, 0x80000000 for float-to-int conversions out of range or of a NaN,
   the second operand of min / max when either is a NaN or both are zero, rounding to nearest-even where the instruction uses
   MXCSR's default mode, the half-float conversion with all four rounding modes.
   The vector types are real GNU vectors, so code that mixes intrinsics with vector operators (a + b, a[i]) compiles. What
   this does not give yet is speed: mcc lowers vector operations element by element. The driver therefore defines __AVX2__ and
   its like only when asked by name (-mavx2 / -mfma / -mf16c / -mbmi2 / -mavx512f / -mavx512bw), not for -march=native: a
   program's scalar path stays the default, and these give its SIMD path, bit for bit, for testing. */
#ifndef __MCC_IMMINTRIN_H
#define __MCC_IMMINTRIN_H

#include <string.h>

typedef float __m128 __attribute__((vector_size(16)));
typedef double __m128d __attribute__((vector_size(16)));
typedef long long __m128i __attribute__((vector_size(16)));
typedef float __m256 __attribute__((vector_size(32)));
typedef double __m256d __attribute__((vector_size(32)));
typedef long long __m256i __attribute__((vector_size(32)));
typedef float __m512 __attribute__((vector_size(64)));
typedef long long __m512i __attribute__((vector_size(64)));
typedef unsigned long long __mmask64;
typedef unsigned int __mmask32;
typedef unsigned short __mmask16;
typedef unsigned char __mmask8;
/* the unaligned flavours are the same types here (loads and stores go through memcpy) */
typedef __m128 __m128_u;
typedef __m128i __m128i_u;
typedef __m256 __m256_u;
typedef __m256i __m256i_u;

/* element views (internal) */
typedef float __mcc_v4sf __attribute__((vector_size(16)));
typedef int __mcc_v4si __attribute__((vector_size(16)));
typedef unsigned int __mcc_v4su __attribute__((vector_size(16)));
typedef short __mcc_v8hi __attribute__((vector_size(16)));
typedef signed char __mcc_v16qi __attribute__((vector_size(16)));
typedef unsigned char __mcc_v16qu __attribute__((vector_size(16)));
typedef long long __mcc_v2di __attribute__((vector_size(16)));
typedef float __mcc_v8sf __attribute__((vector_size(32)));
typedef double __mcc_v4df __attribute__((vector_size(32)));
typedef int __mcc_v8si __attribute__((vector_size(32)));
typedef unsigned int __mcc_v8su __attribute__((vector_size(32)));
typedef short __mcc_v16hi __attribute__((vector_size(32)));
typedef unsigned short __mcc_v16hu __attribute__((vector_size(32)));
typedef signed char __mcc_v32qi __attribute__((vector_size(32)));
typedef unsigned char __mcc_v32qu __attribute__((vector_size(32)));
typedef long long __mcc_v4di __attribute__((vector_size(32)));
typedef signed char __mcc_v64qi __attribute__((vector_size(64)));

#define __MCC_I static inline

/* ---- constants ---- */
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#define _MM_FROUND_TO_NEAREST_INT 0x00
#define _MM_FROUND_TO_NEG_INF 0x01
#define _MM_FROUND_TO_POS_INF 0x02
#define _MM_FROUND_TO_ZERO 0x03
#define _MM_FROUND_CUR_DIRECTION 0x04
#define _MM_FROUND_RAISE_EXC 0x00
#define _MM_FROUND_NO_EXC 0x08
#define _MM_FROUND_NINT (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_NEAREST_INT)
#define _MM_FROUND_FLOOR (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_NEG_INF)
#define _MM_FROUND_CEIL (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_POS_INF)
#define _MM_FROUND_TRUNC (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_ZERO)
#define _MM_FROUND_RINT (_MM_FROUND_RAISE_EXC | _MM_FROUND_CUR_DIRECTION)
#define _MM_FROUND_NEARBYINT (_MM_FROUND_NO_EXC | _MM_FROUND_CUR_DIRECTION)
#define _CMP_EQ_OQ 0x00
#define _CMP_LT_OS 0x01
#define _CMP_LE_OS 0x02
#define _CMP_UNORD_Q 0x03
#define _CMP_NEQ_UQ 0x04
#define _CMP_NLT_US 0x05
#define _CMP_NLE_US 0x06
#define _CMP_ORD_Q 0x07
#define _CMP_EQ_UQ 0x08
#define _CMP_NGE_US 0x09
#define _CMP_NGT_US 0x0a
#define _CMP_FALSE_OQ 0x0b
#define _CMP_NEQ_OQ 0x0c
#define _CMP_GE_OS 0x0d
#define _CMP_GT_OS 0x0e
#define _CMP_TRUE_UQ 0x0f
#define _CMP_EQ_OS 0x10
#define _CMP_LT_OQ 0x11
#define _CMP_LE_OQ 0x12
#define _CMP_UNORD_S 0x13
#define _CMP_NEQ_US 0x14
#define _CMP_NLT_UQ 0x15
#define _CMP_NLE_UQ 0x16
#define _CMP_ORD_S 0x17
#define _CMP_EQ_US 0x18
#define _CMP_NGE_UQ 0x19
#define _CMP_NGT_UQ 0x1a
#define _CMP_FALSE_OS 0x1b
#define _CMP_NEQ_OS 0x1c
#define _CMP_GE_OQ 0x1d
#define _CMP_GT_OQ 0x1e
#define _CMP_TRUE_US 0x1f

/* ---- helpers ---- */
__MCC_I unsigned __mcc_fbits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
__MCC_I float __mcc_bitsf(unsigned u) { float f; memcpy(&f, &u, 4); return f; }
/* the predicates of vcmpps (the signalling / quiet distinction only concerns exceptions, not results) */
__MCC_I int __mcc_cmpf(float a, float b, int p)
{
    int un = a != a || b != b;
    switch (p & 15) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    case 7: return !un;
    case 8: return un || a == b;
    case 9: return un || a < b;
    case 10: return un || a <= b;
    case 11: return 0;
    case 12: return !un && a != b;
    case 13: return !un && a >= b;
    case 14: return !un && a > b;
    default: return 1;
    }
}
/* cvttps2dq / cvtps2dq: the integer, or 0x80000000 when it does not fit (or a NaN) */
__MCC_I int __mcc_f2i(float x, int nearest)
{
    float r = nearest ? __builtin_rintf(x) : __builtin_truncf(x);
    if (!(r >= -2147483648.0f && r < 2147483648.0f))
        return (int)0x80000000u;
    return (int)r;
}
/* roundps: imm & 4 = MXCSR's mode (round to nearest even, the default), else imm & 3 */
__MCC_I float __mcc_roundf(float x, int imm)
{
    switch (imm & 4 ? 0 : imm & 3) {
    case 1: return __builtin_floorf(x);
    case 2: return __builtin_ceilf(x);
    case 3: return __builtin_truncf(x);
    default: return __builtin_rintf(x);
    }
}
/* vcvtps2ph: float to IEEE half with rounding mode (0 nearest even, 1 down, 2 up, 3 toward zero; imm & 4: MXCSR = nearest) */
__MCC_I unsigned short __mcc_f2h(float f, int imm)
{
    unsigned x = __mcc_fbits(f), sign = (x >> 16) & 0x8000u, mode = imm & 4 ? 0 : imm & 3, res;
    int e = (int)((x >> 23) & 0xff), neg = sign != 0, up, shift;
    unsigned long long m = x & 0x7fffffu, q, rem, half;
    if (e == 255)
        return (unsigned short)(sign | 0x7c00u | (m ? 0x200u | (unsigned)(m >> 13) : 0)); /* inf; NaN made quiet, payload kept */
    if (e == 0 && m == 0)
        return (unsigned short)sign;
    if (e)
        m |= 0x800000u;
    else
        e = 1;
    e -= 127; /* value = m * 2^(e - 23) */
    if (e > 15) {
        /* beyond the largest half: infinity, or the largest finite one when rounding toward zero or away from that side */
        int inf = mode == 0 || (mode == 1 && neg) || (mode == 2 && !neg);
        return (unsigned short)(sign | (inf ? 0x7c00u : 0x7bffu));
    }
    shift = e >= -14 ? 13 : 13 + (-14 - e);
    if (shift > 60)
        shift = 60;
    q = m >> shift;
    rem = m & ((1ull << shift) - 1);
    half = 1ull << (shift - 1);
    if (mode == 0)
        up = rem > half || (rem == half && (q & 1));
    else if (mode == 1)
        up = neg && rem;
    else if (mode == 2)
        up = !neg && rem;
    else
        up = 0;
    if (e >= -14)
        res = (unsigned)(((unsigned)(e + 14) << 10) + q); /* q holds the implicit bit: it adds the 1 of the biased exponent */
    else
        res = (unsigned)q;
    res += (unsigned)up; /* a carry moves into the exponent, up to infinity */
    return (unsigned short)(sign | res);
}
__MCC_I float __mcc_h2f(unsigned short h)
{
    unsigned sign = (unsigned)(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3ffu;
    if (e == 31)
        return __mcc_bitsf(sign | 0x7f800000u | (m << 13));
    if (e == 0) {
        if (!m)
            return __mcc_bitsf(sign);
        e = 1;
        while (!(m & 0x400u)) {
            m <<= 1;
            e--;
        }
        m &= 0x3ffu;
        return __mcc_bitsf(sign | ((e + 112) << 23) | (m << 13));
    }
    return __mcc_bitsf(sign | ((e + 112) << 23) | (m << 13));
}
__MCC_I short __mcc_sat16(int v) { return (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
__MCC_I unsigned char __mcc_satu8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
__MCC_I unsigned short __mcc_satu16(int v) { return (unsigned short)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

/* ---- casts (the same bits) ---- */
__MCC_I __m256i _mm256_castps_si256(__m256 a) { return (__m256i)a; }
__MCC_I __m256 _mm256_castsi256_ps(__m256i a) { return (__m256)a; }
__MCC_I __m256 _mm256_castpd_ps(__m256d a) { return (__m256)a; }
__MCC_I __m256d _mm256_castps_pd(__m256 a) { return (__m256d)a; }
__MCC_I __m128i _mm256_castsi256_si128(__m256i a) { __m128i r; memcpy(&r, &a, 16); return r; }
__MCC_I __m256i _mm256_castsi128_si256(__m128i a) { __m256i r; memset(&r, 0, sizeof r); memcpy(&r, &a, 16); return r; }
__MCC_I __m128 _mm256_castps256_ps128(__m256 a) { __m128 r; memcpy(&r, &a, 16); return r; }
__MCC_I __m128i _mm_castps_si128(__m128 a) { return (__m128i)a; }
__MCC_I __m128 _mm_castsi128_ps(__m128i a) { return (__m128)a; }

/* ---- set, load, store ---- */
__MCC_I __m128i _mm_setzero_si128(void) { __m128i r; memset(&r, 0, sizeof r); return r; }
__MCC_I __m128 _mm_setzero_ps(void) { __m128 r; memset(&r, 0, sizeof r); return r; }
__MCC_I __m256 _mm256_setzero_ps(void) { __m256 r; memset(&r, 0, sizeof r); return r; }
__MCC_I __m256d _mm256_setzero_pd(void) { __m256d r; memset(&r, 0, sizeof r); return r; }
__MCC_I __m256i _mm256_setzero_si256(void) { __m256i r; memset(&r, 0, sizeof r); return r; }
__MCC_I __m512 _mm512_setzero_ps(void) { __m512 r; memset(&r, 0, 64); return r; }
__MCC_I __m256 _mm256_set1_ps(float a) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = a; return (__m256)r; }
__MCC_I __m128 _mm_set1_ps(float a) { __mcc_v4sf r; int i; for (i = 0; i < 4; i++) r[i] = a; return (__m128)r; }
__MCC_I __m256i _mm256_set1_epi32(int a) { __mcc_v8si r; int i; for (i = 0; i < 8; i++) r[i] = a; return (__m256i)r; }
__MCC_I __m256i _mm256_set1_epi16(short a) { __mcc_v16hi r; int i; for (i = 0; i < 16; i++) r[i] = a; return (__m256i)r; }
__MCC_I __m256i _mm256_set1_epi8(char a) { __mcc_v32qi r; int i; for (i = 0; i < 32; i++) r[i] = (signed char)a; return (__m256i)r; }
__MCC_I __m256i _mm256_set1_epi64x(long long a) { __m256i r; int i; for (i = 0; i < 4; i++) r[i] = a; return r; }
__MCC_I __m128i _mm_set1_epi8(char a) { __mcc_v16qi r; int i; for (i = 0; i < 16; i++) r[i] = (signed char)a; return (__m128i)r; }
__MCC_I __m128i _mm_set1_epi32(int a) { __mcc_v4si r; int i; for (i = 0; i < 4; i++) r[i] = a; return (__m128i)r; }
__MCC_I __m256 _mm256_setr_ps(float a, float b, float c, float d, float e, float f, float g, float h)
{
    __mcc_v8sf r;
    r[0] = a; r[1] = b; r[2] = c; r[3] = d; r[4] = e; r[5] = f; r[6] = g; r[7] = h;
    return (__m256)r;
}
__MCC_I __m256 _mm256_set_ps(float h, float g, float f, float e, float d, float c, float b, float a)
{
    return _mm256_setr_ps(a, b, c, d, e, f, g, h);
}
__MCC_I __m256i _mm256_setr_epi32(int a, int b, int c, int d, int e, int f, int g, int h)
{
    __mcc_v8si r;
    r[0] = a; r[1] = b; r[2] = c; r[3] = d; r[4] = e; r[5] = f; r[6] = g; r[7] = h;
    return (__m256i)r;
}
__MCC_I __m256i _mm256_set_epi32(int h, int g, int f, int e, int d, int c, int b, int a)
{
    return _mm256_setr_epi32(a, b, c, d, e, f, g, h);
}
__MCC_I __m128 _mm_loadu_ps(const float *p) { __m128 r; memcpy(&r, p, 16); return r; }
__MCC_I void _mm_storeu_ps(float *p, __m128 a) { memcpy(p, &a, 16); }
__MCC_I __m256 _mm256_loadu_ps(const float *p) { __m256 r; memcpy(&r, p, 32); return r; }
__MCC_I __m256 _mm256_load_ps(const float *p) { return _mm256_loadu_ps(p); }
__MCC_I void _mm256_storeu_ps(float *p, __m256 a) { memcpy(p, &a, 32); }
__MCC_I void _mm256_store_ps(float *p, __m256 a) { memcpy(p, &a, 32); }
__MCC_I __m256i _mm256_loadu_si256(const void *p) { __m256i r; memcpy(&r, p, 32); return r; }
__MCC_I __m256i _mm256_load_si256(const void *p) { return _mm256_loadu_si256(p); }
__MCC_I void _mm256_storeu_si256(void *p, __m256i a) { memcpy(p, &a, 32); }
__MCC_I void _mm256_store_si256(void *p, __m256i a) { memcpy(p, &a, 32); }
__MCC_I __m128i _mm_loadu_si128(const void *p) { __m128i r; memcpy(&r, p, 16); return r; }
__MCC_I __m128i _mm_load_si128(const void *p) { return _mm_loadu_si128(p); }
__MCC_I __m128i _mm_loadl_epi64(const void *p) { __m128i r; memset(&r, 0, sizeof r); memcpy(&r, p, 8); return r; }
__MCC_I void _mm_storeu_si128(void *p, __m128i a) { memcpy(p, &a, 16); }
__MCC_I void _mm_store_si128(void *p, __m128i a) { memcpy(p, &a, 16); }
__MCC_I void _mm_storel_epi64(void *p, __m128i a) { memcpy(p, &a, 8); }
__MCC_I __m512 _mm512_loadu_ps(const void *p) { __m512 r; memcpy(&r, p, 64); return r; }
__MCC_I void _mm512_storeu_ps(void *p, __m512 a) { memcpy(p, &a, 64); }
__MCC_I __m512i _mm512_loadu_si512(const void *p) { __m512i r; memcpy(&r, p, 64); return r; }
__MCC_I void _mm512_storeu_si512(void *p, __m512i a) { memcpy(p, &a, 64); }
__MCC_I __m128i _mm_cvtsi64_si128(long long a) { __m128i r; memset(&r, 0, sizeof r); r[0] = a; return r; }
__MCC_I __m128i _mm_cvtsi32_si128(int a) { __mcc_v4si r; memset(&r, 0, sizeof r); r[0] = a; return (__m128i)r; }
__MCC_I int _mm_cvtsi128_si32(__m128i a) { return ((__mcc_v4si)a)[0]; }
__MCC_I long long _mm_cvtsi128_si64(__m128i a) { return a[0]; }
__MCC_I float _mm256_cvtss_f32(__m256 a) { return a[0]; }

/* ---- float arithmetic ---- */
__MCC_I __m256 _mm256_add_ps(__m256 a, __m256 b) { return a + b; }
__MCC_I __m256 _mm256_sub_ps(__m256 a, __m256 b) { return a - b; }
__MCC_I __m256 _mm256_mul_ps(__m256 a, __m256 b) { return a * b; }
__MCC_I __m256 _mm256_div_ps(__m256 a, __m256 b) { return a / b; }
__MCC_I __m128 _mm_add_ps(__m128 a, __m128 b) { return a + b; }
__MCC_I __m128 _mm_sub_ps(__m128 a, __m128 b) { return a - b; }
__MCC_I __m128 _mm_mul_ps(__m128 a, __m128 b) { return a * b; }
__MCC_I __m256 _mm256_sqrt_ps(__m256 a) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = __builtin_sqrtf(a[i]); return (__m256)r; }
/* vminps / vmaxps: the second operand unless the first is strictly smaller (larger): NaNs and +-0 pairs give b */
__MCC_I __m256 _mm256_min_ps(__m256 a, __m256 b) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; return (__m256)r; }
__MCC_I __m256 _mm256_max_ps(__m256 a, __m256 b) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; return (__m256)r; }
__MCC_I __m128 _mm_min_ps(__m128 a, __m128 b) { __mcc_v4sf r; int i; for (i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; return (__m128)r; }
__MCC_I __m128 _mm_max_ps(__m128 a, __m128 b) { __mcc_v4sf r; int i; for (i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; return (__m128)r; }
/* vfmadd: one rounding (fmaf is the correctly rounded fused operation) */
__MCC_I __m256 _mm256_fmadd_ps(__m256 a, __m256 b, __m256 c) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = __builtin_fmaf(a[i], b[i], c[i]); return (__m256)r; }
__MCC_I __m256 _mm256_fmsub_ps(__m256 a, __m256 b, __m256 c) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = __builtin_fmaf(a[i], b[i], -c[i]); return (__m256)r; }
__MCC_I __m256 _mm256_fnmadd_ps(__m256 a, __m256 b, __m256 c) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = __builtin_fmaf(-a[i], b[i], c[i]); return (__m256)r; }
__MCC_I __m256 _mm256_round_ps(__m256 a, int imm) { __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = __mcc_roundf(a[i], imm); return (__m256)r; }
#define _mm256_floor_ps(a) _mm256_round_ps((a), _MM_FROUND_FLOOR)
#define _mm256_ceil_ps(a) _mm256_round_ps((a), _MM_FROUND_CEIL)

/* ---- float bitwise, compare, select ---- */
__MCC_I __m256 _mm256_and_ps(__m256 a, __m256 b) { return (__m256)((__m256i)a & (__m256i)b); }
__MCC_I __m256 _mm256_or_ps(__m256 a, __m256 b) { return (__m256)((__m256i)a | (__m256i)b); }
__MCC_I __m256 _mm256_xor_ps(__m256 a, __m256 b) { return (__m256)((__m256i)a ^ (__m256i)b); }
__MCC_I __m256 _mm256_andnot_ps(__m256 a, __m256 b) { return (__m256)(~(__m256i)a & (__m256i)b); }
__MCC_I __m256 _mm256_cmp_ps(__m256 a, __m256 b, int p)
{
    __mcc_v8si r;
    int i;
    for (i = 0; i < 8; i++)
        r[i] = __mcc_cmpf(a[i], b[i], p) ? -1 : 0;
    return (__m256)r;
}
__MCC_I __m256 _mm256_blendv_ps(__m256 a, __m256 b, __m256 m)
{
    __mcc_v8si mi = (__mcc_v8si)m;
    __mcc_v8sf r;
    int i;
    for (i = 0; i < 8; i++)
        r[i] = mi[i] < 0 ? b[i] : a[i];
    return (__m256)r;
}
__MCC_I int _mm256_movemask_ps(__m256 a)
{
    __mcc_v8si ai = (__mcc_v8si)a;
    int r = 0, i;
    for (i = 0; i < 8; i++)
        r |= (ai[i] < 0) << i;
    return r;
}
__MCC_I int _mm_movemask_ps(__m128 a)
{
    __mcc_v4si ai = (__mcc_v4si)a;
    int r = 0, i;
    for (i = 0; i < 4; i++)
        r |= (ai[i] < 0) << i;
    return r;
}

/* ---- float shuffles (within each 128-bit lane, as AVX does them) ---- */
__MCC_I __m256 _mm256_shuffle_ps(__m256 a, __m256 b, int imm)
{
    __mcc_v8sf r;
    int l;
    for (l = 0; l < 8; l += 4) {
        r[l + 0] = a[l + (imm & 3)];
        r[l + 1] = a[l + ((imm >> 2) & 3)];
        r[l + 2] = b[l + ((imm >> 4) & 3)];
        r[l + 3] = b[l + ((imm >> 6) & 3)];
    }
    return (__m256)r;
}
__MCC_I __m256 _mm256_unpacklo_ps(__m256 a, __m256 b)
{
    __mcc_v8sf r;
    int l;
    for (l = 0; l < 8; l += 4) {
        r[l + 0] = a[l + 0];
        r[l + 1] = b[l + 0];
        r[l + 2] = a[l + 1];
        r[l + 3] = b[l + 1];
    }
    return (__m256)r;
}
__MCC_I __m256 _mm256_unpackhi_ps(__m256 a, __m256 b)
{
    __mcc_v8sf r;
    int l;
    for (l = 0; l < 8; l += 4) {
        r[l + 0] = a[l + 2];
        r[l + 1] = b[l + 2];
        r[l + 2] = a[l + 3];
        r[l + 3] = b[l + 3];
    }
    return (__m256)r;
}
/* vperm2f128: each half from a.lo, a.hi, b.lo, b.hi (imm bits 1:0 / 5:4), or zero (bit 3 / 7) */
__MCC_I __m256 _mm256_permute2f128_ps(__m256 a, __m256 b, int imm)
{
    __mcc_v8sf r;
    int h, i;
    for (h = 0; h < 2; h++) {
        int s = (imm >> (4 * h)) & 15;
        for (i = 0; i < 4; i++)
            r[4 * h + i] = s & 8 ? 0.0f : s & 2 ? b[4 * (s & 1) + i] : a[4 * (s & 1) + i];
    }
    return (__m256)r;
}
__MCC_I __m256i _mm256_permute2x128_si256(__m256i a, __m256i b, int imm)
{
    return (__m256i)_mm256_permute2f128_ps((__m256)a, (__m256)b, imm);
}

/* ---- conversions ---- */
__MCC_I __m256 _mm256_cvtepi32_ps(__m256i a) { __mcc_v8si x = (__mcc_v8si)a; __mcc_v8sf r; int i; for (i = 0; i < 8; i++) r[i] = (float)x[i]; return (__m256)r; }
__MCC_I __m256i _mm256_cvttps_epi32(__m256 a) { __mcc_v8si r; int i; for (i = 0; i < 8; i++) r[i] = __mcc_f2i(a[i], 0); return (__m256i)r; }
__MCC_I __m256i _mm256_cvtps_epi32(__m256 a) { __mcc_v8si r; int i; for (i = 0; i < 8; i++) r[i] = __mcc_f2i(a[i], 1); return (__m256i)r; }
__MCC_I __m128i _mm256_cvtps_ph(__m256 a, int imm)
{
    unsigned short h[8];
    __m128i r;
    int i;
    for (i = 0; i < 8; i++)
        h[i] = __mcc_f2h(a[i], imm);
    memcpy(&r, h, 16);
    return r;
}
__MCC_I __m256 _mm256_cvtph_ps(__m128i a)
{
    unsigned short h[8];
    __mcc_v8sf r;
    int i;
    memcpy(h, &a, 16);
    for (i = 0; i < 8; i++)
        r[i] = __mcc_h2f(h[i]);
    return (__m256)r;
}
__MCC_I unsigned short _cvtss_sh(float a, int imm) { return __mcc_f2h(a, imm); }
__MCC_I float _cvtsh_ss(unsigned short a) { return __mcc_h2f(a); }
__MCC_I __m256i _mm256_cvtepu8_epi32(__m128i a)
{
    unsigned char b[16];
    __mcc_v8si r;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 8; i++)
        r[i] = b[i];
    return (__m256i)r;
}
__MCC_I __m256i _mm256_cvtepu8_epi16(__m128i a)
{
    unsigned char b[16];
    __mcc_v16hi r;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 16; i++)
        r[i] = (short)b[i];
    return (__m256i)r;
}
__MCC_I __m256i _mm256_cvtepi8_epi16(__m128i a)
{
    signed char b[16];
    __mcc_v16hi r;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 16; i++)
        r[i] = (short)b[i];
    return (__m256i)r;
}
__MCC_I __m256i _mm256_cvtepi16_epi32(__m128i a)
{
    short b[8];
    __mcc_v8si r;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 8; i++)
        r[i] = b[i];
    return (__m256i)r;
}

/* ---- integer arithmetic ---- */
__MCC_I __m256i _mm256_add_epi32(__m256i a, __m256i b) { return (__m256i)((__mcc_v8su)a + (__mcc_v8su)b); }
__MCC_I __m256i _mm256_sub_epi32(__m256i a, __m256i b) { return (__m256i)((__mcc_v8su)a - (__mcc_v8su)b); }
__MCC_I __m256i _mm256_add_epi16(__m256i a, __m256i b) { return (__m256i)((__mcc_v16hu)a + (__mcc_v16hu)b); }
__MCC_I __m256i _mm256_sub_epi16(__m256i a, __m256i b) { return (__m256i)((__mcc_v16hu)a - (__mcc_v16hu)b); }
__MCC_I __m256i _mm256_add_epi8(__m256i a, __m256i b) { return (__m256i)((__mcc_v32qu)a + (__mcc_v32qu)b); }
__MCC_I __m256i _mm256_sub_epi8(__m256i a, __m256i b) { return (__m256i)((__mcc_v32qu)a - (__mcc_v32qu)b); }
__MCC_I __m256i _mm256_add_epi64(__m256i a, __m256i b) { return (__m256i)((__mcc_v4di)a + (__mcc_v4di)b); }
__MCC_I __m256i _mm256_mullo_epi32(__m256i a, __m256i b) { return (__m256i)((__mcc_v8su)a * (__mcc_v8su)b); }
__MCC_I __m256i _mm256_mullo_epi16(__m256i a, __m256i b) { return (__m256i)((__mcc_v16hu)a * (__mcc_v16hu)b); }
__MCC_I __m256i _mm256_abs_epi16(__m256i a)
{
    __mcc_v16hi x = (__mcc_v16hi)a;
    __mcc_v16hu r;
    int i;
    for (i = 0; i < 16; i++)
        r[i] = (unsigned short)(x[i] < 0 ? -(int)x[i] : x[i]); /* -32768 stays 0x8000 */
    return (__m256i)r;
}
__MCC_I __m256i _mm256_abs_epi32(__m256i a)
{
    __mcc_v8si x = (__mcc_v8si)a;
    __mcc_v8su r;
    int i;
    for (i = 0; i < 8; i++)
        r[i] = x[i] < 0 ? 0u - (unsigned)x[i] : (unsigned)x[i];
    return (__m256i)r;
}
/* vpsignw: a negated where b < 0, zero where b == 0 */
__MCC_I __m256i _mm256_sign_epi16(__m256i a, __m256i b)
{
    __mcc_v16hi x = (__mcc_v16hi)a, y = (__mcc_v16hi)b;
    __mcc_v16hu r;
    int i;
    for (i = 0; i < 16; i++)
        r[i] = (unsigned short)(y[i] < 0 ? -(int)x[i] : y[i] == 0 ? 0 : x[i]);
    return (__m256i)r;
}
__MCC_I __m256i _mm256_min_epi32(__m256i a, __m256i b) { __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b, r; int i; for (i = 0; i < 8; i++) r[i] = x[i] < y[i] ? x[i] : y[i]; return (__m256i)r; }
__MCC_I __m256i _mm256_max_epi32(__m256i a, __m256i b) { __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b, r; int i; for (i = 0; i < 8; i++) r[i] = x[i] > y[i] ? x[i] : y[i]; return (__m256i)r; }
__MCC_I __m256i _mm256_min_epu8(__m256i a, __m256i b) { __mcc_v32qu x = (__mcc_v32qu)a, y = (__mcc_v32qu)b, r; int i; for (i = 0; i < 32; i++) r[i] = x[i] < y[i] ? x[i] : y[i]; return (__m256i)r; }
__MCC_I __m256i _mm256_max_epu8(__m256i a, __m256i b) { __mcc_v32qu x = (__mcc_v32qu)a, y = (__mcc_v32qu)b, r; int i; for (i = 0; i < 32; i++) r[i] = x[i] > y[i] ? x[i] : y[i]; return (__m256i)r; }
__MCC_I __m128i _mm_min_epu8(__m128i a, __m128i b) { __mcc_v16qu x = (__mcc_v16qu)a, y = (__mcc_v16qu)b, r; int i; for (i = 0; i < 16; i++) r[i] = x[i] < y[i] ? x[i] : y[i]; return (__m128i)r; }
__MCC_I __m128i _mm_max_epu8(__m128i a, __m128i b) { __mcc_v16qu x = (__mcc_v16qu)a, y = (__mcc_v16qu)b, r; int i; for (i = 0; i < 16; i++) r[i] = x[i] > y[i] ? x[i] : y[i]; return (__m128i)r; }

/* ---- integer bitwise ---- */
__MCC_I __m256i _mm256_and_si256(__m256i a, __m256i b) { return a & b; }
__MCC_I __m256i _mm256_or_si256(__m256i a, __m256i b) { return a | b; }
__MCC_I __m256i _mm256_xor_si256(__m256i a, __m256i b) { return a ^ b; }
__MCC_I __m256i _mm256_andnot_si256(__m256i a, __m256i b) { return ~a & b; }
__MCC_I __m128i _mm_and_si128(__m128i a, __m128i b) { return a & b; }
__MCC_I __m128i _mm_or_si128(__m128i a, __m128i b) { return a | b; }
__MCC_I __m128i _mm_xor_si128(__m128i a, __m128i b) { return a ^ b; }
__MCC_I __m128i _mm_andnot_si128(__m128i a, __m128i b) { return ~a & b; }
__MCC_I int _mm256_testz_si256(__m256i a, __m256i b)
{
    __m256i x = a & b;
    return !(x[0] | x[1] | x[2] | x[3]);
}

/* ---- shifts: a count beyond the width gives 0 (logical) or the sign (arithmetic) ---- */
__MCC_I __m256i _mm256_slli_epi32(__m256i a, int n) { __mcc_v8su x = (__mcc_v8su)a, r; int i; for (i = 0; i < 8; i++) r[i] = (unsigned)n > 31 ? 0 : x[i] << n; return (__m256i)r; }
__MCC_I __m256i _mm256_srli_epi32(__m256i a, int n) { __mcc_v8su x = (__mcc_v8su)a, r; int i; for (i = 0; i < 8; i++) r[i] = (unsigned)n > 31 ? 0 : x[i] >> n; return (__m256i)r; }
__MCC_I __m256i _mm256_srai_epi32(__m256i a, int n) { __mcc_v8si x = (__mcc_v8si)a, r; int i; for (i = 0; i < 8; i++) r[i] = x[i] >> ((unsigned)n > 31 ? 31 : n); return (__m256i)r; }
__MCC_I __m256i _mm256_slli_epi16(__m256i a, int n) { __mcc_v16hu x = (__mcc_v16hu)a, r; int i; for (i = 0; i < 16; i++) r[i] = (unsigned short)((unsigned)n > 15 ? 0 : x[i] << n); return (__m256i)r; }
__MCC_I __m256i _mm256_srli_epi16(__m256i a, int n) { __mcc_v16hu x = (__mcc_v16hu)a, r; int i; for (i = 0; i < 16; i++) r[i] = (unsigned short)((unsigned)n > 15 ? 0 : x[i] >> n); return (__m256i)r; }
__MCC_I __m256i _mm256_srai_epi16(__m256i a, int n) { __mcc_v16hi x = (__mcc_v16hi)a, r; int i; for (i = 0; i < 16; i++) r[i] = (short)(x[i] >> ((unsigned)n > 15 ? 15 : n)); return (__m256i)r; }
__MCC_I __m256i _mm256_slli_epi64(__m256i a, int n) { __m256i r; int i; for (i = 0; i < 4; i++) r[i] = (unsigned)n > 63 ? 0 : (long long)((unsigned long long)a[i] << n); return r; }
__MCC_I __m256i _mm256_srli_epi64(__m256i a, int n) { __m256i r; int i; for (i = 0; i < 4; i++) r[i] = (unsigned)n > 63 ? 0 : (long long)((unsigned long long)a[i] >> n); return r; }
__MCC_I __m256i _mm256_srlv_epi32(__m256i a, __m256i c) { __mcc_v8su x = (__mcc_v8su)a, n = (__mcc_v8su)c, r; int i; for (i = 0; i < 8; i++) r[i] = n[i] > 31 ? 0 : x[i] >> n[i]; return (__m256i)r; }
__MCC_I __m256i _mm256_sllv_epi32(__m256i a, __m256i c) { __mcc_v8su x = (__mcc_v8su)a, n = (__mcc_v8su)c, r; int i; for (i = 0; i < 8; i++) r[i] = n[i] > 31 ? 0 : x[i] << n[i]; return (__m256i)r; }
__MCC_I __m256i _mm256_srav_epi32(__m256i a, __m256i c) { __mcc_v8si x = (__mcc_v8si)a, r; __mcc_v8su n = (__mcc_v8su)c; int i; for (i = 0; i < 8; i++) r[i] = x[i] >> (n[i] > 31 ? 31 : n[i]); return (__m256i)r; }
/* psrldq: the whole 128-bit value, by bytes */
__MCC_I __m128i _mm_srli_si128(__m128i a, int n)
{
    unsigned char b[16], r[16];
    __m128i v;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 16; i++)
        r[i] = (unsigned)n > 15 || i + n > 15 ? 0 : b[i + n];
    memcpy(&v, r, 16);
    return v;
}
__MCC_I __m128i _mm_slli_si128(__m128i a, int n)
{
    unsigned char b[16], r[16];
    __m128i v;
    int i;
    memcpy(b, &a, 16);
    for (i = 0; i < 16; i++)
        r[i] = (unsigned)n > 15 || i < n ? 0 : b[i - n];
    memcpy(&v, r, 16);
    return v;
}

/* ---- integer compares and selects ---- */
__MCC_I __m256i _mm256_cmpeq_epi8(__m256i a, __m256i b) { __mcc_v32qi x = (__mcc_v32qi)a, y = (__mcc_v32qi)b, r; int i; for (i = 0; i < 32; i++) r[i] = x[i] == y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m256i _mm256_cmpeq_epi16(__m256i a, __m256i b) { __mcc_v16hi x = (__mcc_v16hi)a, y = (__mcc_v16hi)b, r; int i; for (i = 0; i < 16; i++) r[i] = x[i] == y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m256i _mm256_cmpeq_epi32(__m256i a, __m256i b) { __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b, r; int i; for (i = 0; i < 8; i++) r[i] = x[i] == y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m256i _mm256_cmpgt_epi16(__m256i a, __m256i b) { __mcc_v16hi x = (__mcc_v16hi)a, y = (__mcc_v16hi)b, r; int i; for (i = 0; i < 16; i++) r[i] = x[i] > y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m256i _mm256_cmpgt_epi32(__m256i a, __m256i b) { __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b, r; int i; for (i = 0; i < 8; i++) r[i] = x[i] > y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m256i _mm256_cmpgt_epi8(__m256i a, __m256i b) { __mcc_v32qi x = (__mcc_v32qi)a, y = (__mcc_v32qi)b, r; int i; for (i = 0; i < 32; i++) r[i] = x[i] > y[i] ? -1 : 0; return (__m256i)r; }
__MCC_I __m128i _mm_cmpeq_epi8(__m128i a, __m128i b) { __mcc_v16qi x = (__mcc_v16qi)a, y = (__mcc_v16qi)b, r; int i; for (i = 0; i < 16; i++) r[i] = x[i] == y[i] ? -1 : 0; return (__m128i)r; }
__MCC_I __m128i _mm_cmpeq_epi32(__m128i a, __m128i b) { __mcc_v4si x = (__mcc_v4si)a, y = (__mcc_v4si)b, r; int i; for (i = 0; i < 4; i++) r[i] = x[i] == y[i] ? -1 : 0; return (__m128i)r; }
/* vpblendvb: by the sign bit of each byte of the mask */
__MCC_I __m256i _mm256_blendv_epi8(__m256i a, __m256i b, __m256i m)
{
    __mcc_v32qi x = (__mcc_v32qi)a, y = (__mcc_v32qi)b, k = (__mcc_v32qi)m, r;
    int i;
    for (i = 0; i < 32; i++)
        r[i] = k[i] < 0 ? y[i] : x[i];
    return (__m256i)r;
}
__MCC_I int _mm256_movemask_epi8(__m256i a)
{
    __mcc_v32qi x = (__mcc_v32qi)a;
    unsigned r = 0;
    int i;
    for (i = 0; i < 32; i++)
        r |= (unsigned)(x[i] < 0) << i;
    return (int)r;
}
__MCC_I int _mm_movemask_epi8(__m128i a)
{
    __mcc_v16qi x = (__mcc_v16qi)a;
    int r = 0, i;
    for (i = 0; i < 16; i++)
        r |= (x[i] < 0) << i;
    return r;
}

/* ---- packs (per 128-bit lane: a's half, then b's) ---- */
__MCC_I __m256i _mm256_packus_epi16(__m256i a, __m256i b)
{
    __mcc_v16hi x = (__mcc_v16hi)a, y = (__mcc_v16hi)b;
    __mcc_v32qu r;
    int l, i;
    for (l = 0; l < 2; l++)
        for (i = 0; i < 8; i++) {
            r[16 * l + i] = __mcc_satu8(x[8 * l + i]);
            r[16 * l + 8 + i] = __mcc_satu8(y[8 * l + i]);
        }
    return (__m256i)r;
}
__MCC_I __m256i _mm256_packus_epi32(__m256i a, __m256i b)
{
    __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b;
    __mcc_v16hu r;
    int l, i;
    for (l = 0; l < 2; l++)
        for (i = 0; i < 4; i++) {
            r[8 * l + i] = __mcc_satu16(x[4 * l + i]);
            r[8 * l + 4 + i] = __mcc_satu16(y[4 * l + i]);
        }
    return (__m256i)r;
}
__MCC_I __m256i _mm256_packs_epi32(__m256i a, __m256i b)
{
    __mcc_v8si x = (__mcc_v8si)a, y = (__mcc_v8si)b;
    __mcc_v16hi r;
    int l, i;
    for (l = 0; l < 2; l++)
        for (i = 0; i < 4; i++) {
            r[8 * l + i] = __mcc_sat16(x[4 * l + i]);
            r[8 * l + 4 + i] = __mcc_sat16(y[4 * l + i]);
        }
    return (__m256i)r;
}

/* ---- lane moves ---- */
__MCC_I __m128i _mm256_extracti128_si256(__m256i a, int imm) { __m128i r; memcpy(&r, (char *)&a + 16 * (imm & 1), 16); return r; }
__MCC_I __m128 _mm256_extractf128_ps(__m256 a, int imm) { __m128 r; memcpy(&r, (char *)&a + 16 * (imm & 1), 16); return r; }
__MCC_I __m256i _mm256_inserti128_si256(__m256i a, __m128i b, int imm) { memcpy((char *)&a + 16 * (imm & 1), &b, 16); return a; }
/* vpermd: any 32-bit element by index (low 3 bits) */
__MCC_I __m256i _mm256_permutevar8x32_epi32(__m256i a, __m256i idx)
{
    __mcc_v8si x = (__mcc_v8si)a, k = (__mcc_v8si)idx, r;
    int i;
    for (i = 0; i < 8; i++)
        r[i] = x[k[i] & 7];
    return (__m256i)r;
}
__MCC_I __m256 _mm256_permutevar8x32_ps(__m256 a, __m256i idx)
{
    __mcc_v8si k = (__mcc_v8si)idx;
    __mcc_v8sf r;
    int i;
    for (i = 0; i < 8; i++)
        r[i] = a[k[i] & 7];
    return (__m256)r;
}
/* vpermq: 64-bit elements, two bits of imm each */
__MCC_I __m256i _mm256_permute4x64_epi64(__m256i a, int imm)
{
    __m256i r;
    int i;
    for (i = 0; i < 4; i++)
        r[i] = a[(imm >> (2 * i)) & 3];
    return r;
}

/* ---- gathers: element i from base + index[i] * scale; masked: only where the mask element's sign bit is set ---- */
__MCC_I __m256 _mm256_mask_i32gather_ps(__m256 src, const float *base, __m256i idx, __m256 mask, int scale)
{
    __mcc_v8si k = (__mcc_v8si)idx, m = (__mcc_v8si)mask;
    __mcc_v8sf r = (__mcc_v8sf)src;
    int i;
    for (i = 0; i < 8; i++)
        if (m[i] < 0)
            { float t_; memcpy(&t_, (const char *)base + (long long)k[i] * scale, 4); r[i] = t_; }
    return (__m256)r;
}
__MCC_I __m256 _mm256_i32gather_ps(const float *base, __m256i idx, int scale)
{
    __mcc_v8si k = (__mcc_v8si)idx;
    __mcc_v8sf r;
    int i;
    for (i = 0; i < 8; i++)
        { float t_; memcpy(&t_, (const char *)base + (long long)k[i] * scale, 4); r[i] = t_; }
    return (__m256)r;
}
__MCC_I __m256i _mm256_mask_i32gather_epi32(__m256i src, const int *base, __m256i idx, __m256i mask, int scale)
{
    __mcc_v8si k = (__mcc_v8si)idx, m = (__mcc_v8si)mask, r = (__mcc_v8si)src;
    int i;
    for (i = 0; i < 8; i++)
        if (m[i] < 0)
            { int t_; memcpy(&t_, (const char *)base + (long long)k[i] * scale, 4); r[i] = t_; }
    return (__m256i)r;
}
__MCC_I __m256i _mm256_i32gather_epi32(const int *base, __m256i idx, int scale)
{
    __mcc_v8si k = (__mcc_v8si)idx, r;
    int i;
    for (i = 0; i < 8; i++)
        { int t_; memcpy(&t_, (const char *)base + (long long)k[i] * scale, 4); r[i] = t_; }
    return (__m256i)r;
}
__MCC_I __m256d _mm256_i32gather_pd(const double *base, __m128i idx, int scale)
{
    __mcc_v4si k = (__mcc_v4si)idx;
    __mcc_v4df r;
    int i;
    for (i = 0; i < 4; i++)
        { double t_; memcpy(&t_, (const char *)base + (long long)k[i] * scale, 8); r[i] = t_; }
    return (__m256d)r;
}

/* ---- AVX-512 (the few ufsm reaches under __AVX512F__ / __AVX512BW__) ---- */
/* vptestmb: bit i set where byte i of a & b is not zero */
__MCC_I __mmask64 _mm512_test_epi8_mask(__m512i a, __m512i b)
{
    unsigned char x[64], y[64];
    __mmask64 r = 0;
    int i;
    memcpy(x, &a, 64);
    memcpy(y, &b, 64);
    for (i = 0; i < 64; i++)
        r |= (__mmask64)((x[i] & y[i]) != 0) << i;
    return r;
}

/* ---- BMI2 ---- */
__MCC_I unsigned long long _pdep_u64(unsigned long long src, unsigned long long mask)
{
    unsigned long long r = 0, bit;
    for (bit = 1; mask; bit <<= 1) {
        if (src & bit)
            r |= mask & (0 - mask); /* the lowest set bit of the mask still unfilled */
        mask &= mask - 1;
    }
    return r;
}
__MCC_I unsigned long long _pext_u64(unsigned long long src, unsigned long long mask)
{
    unsigned long long r = 0, bit = 1;
    for (; mask; mask &= mask - 1, bit <<= 1)
        if (src & mask & (0 - mask))
            r |= bit;
    return r;
}
__MCC_I unsigned _pdep_u32(unsigned src, unsigned mask) { return (unsigned)_pdep_u64(src, mask); }
__MCC_I unsigned _pext_u32(unsigned src, unsigned mask) { return (unsigned)_pext_u64(src, mask); }
__MCC_I unsigned long long _bzhi_u64(unsigned long long a, unsigned n) { n &= 255; return n >= 64 ? a : a & ((1ull << n) - 1); }
__MCC_I unsigned long long _tzcnt_u64(unsigned long long a) { return a ? (unsigned long long)__builtin_ctzll(a) : 64; }
__MCC_I unsigned long long _lzcnt_u64(unsigned long long a) { return a ? (unsigned long long)__builtin_clzll(a) : 64; }

/* ---- the rest of SSE that comes with this header ---- */
__MCC_I void _mm_prefetch(const void *p, int hint) { (void)hint; __builtin_prefetch(p); }
#define _MM_HINT_T0 3
#define _MM_HINT_T1 2
#define _MM_HINT_T2 1
#define _MM_HINT_NTA 0
__MCC_I void _mm_pause(void) { }
__MCC_I void _mm_sfence(void) { __sync_synchronize(); }
__MCC_I void _mm_lfence(void) { __sync_synchronize(); }
__MCC_I void _mm_mfence(void) { __sync_synchronize(); }

#undef __MCC_I
#endif
