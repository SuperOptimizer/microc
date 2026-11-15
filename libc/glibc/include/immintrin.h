/* mcc's <immintrin.h>: x86-64 SIMD intrinsics (SSE through AVX2, FMA, F16C, BMI2).
   The vector types are GNU vectors; each intrinsic is a compiler builtin (src/x86i.c lists them) that becomes
   the instruction itself in x86-64 host code. They are there whatever -m options say; the __AVX2__ and
   other macros say what -mavx2, -march=... asked for. AVX-512: the few that ufsm uses
   (512-bit vectors are registers only with -mavx512f or -march=x86-64-v4 / znver4 / native on such a machine). */
#ifndef __MCC_IMMINTRIN_H
#define __MCC_IMMINTRIN_H

typedef float __m512 __attribute__((__vector_size__(64)));
typedef double __m512d __attribute__((__vector_size__(64)));
typedef long long __m512i __attribute__((__vector_size__(64)));
typedef float __m512_u __attribute__((__vector_size__(64), __aligned__(1)));
typedef double __m512d_u __attribute__((__vector_size__(64), __aligned__(1)));
typedef long long __m512i_u __attribute__((__vector_size__(64), __aligned__(1)));
typedef unsigned char __mmask8;
typedef unsigned short __mmask16;
typedef unsigned int __mmask32;
typedef unsigned long long __mmask64;
typedef float __m256 __attribute__((__vector_size__(32)));
typedef double __m256d __attribute__((__vector_size__(32)));
typedef long long __m256i __attribute__((__vector_size__(32)));
typedef float __m128 __attribute__((__vector_size__(16)));
typedef double __m128d __attribute__((__vector_size__(16)));
typedef long long __m128i __attribute__((__vector_size__(16)));
typedef float __m256_u __attribute__((__vector_size__(32), __aligned__(1)));
typedef double __m256d_u __attribute__((__vector_size__(32), __aligned__(1)));
typedef long long __m256i_u __attribute__((__vector_size__(32), __aligned__(1)));
typedef float __m128_u __attribute__((__vector_size__(16), __aligned__(1)));
typedef double __m128d_u __attribute__((__vector_size__(16), __aligned__(1)));
typedef long long __m128i_u __attribute__((__vector_size__(16), __aligned__(1)));

/* GCC's names for the lane types, which some code uses */
typedef float __v8sf __attribute__((__vector_size__(32)));
typedef double __v4df __attribute__((__vector_size__(32)));
typedef long long __v4di __attribute__((__vector_size__(32)));
typedef unsigned long long __v4du __attribute__((__vector_size__(32)));
typedef int __v8si __attribute__((__vector_size__(32)));
typedef unsigned int __v8su __attribute__((__vector_size__(32)));
typedef short __v16hi __attribute__((__vector_size__(32)));
typedef unsigned short __v16hu __attribute__((__vector_size__(32)));
typedef char __v32qi __attribute__((__vector_size__(32)));
typedef signed char __v32qs __attribute__((__vector_size__(32)));
typedef unsigned char __v32qu __attribute__((__vector_size__(32)));
typedef float __v4sf __attribute__((__vector_size__(16)));
typedef double __v2df __attribute__((__vector_size__(16)));
typedef long long __v2di __attribute__((__vector_size__(16)));
typedef unsigned long long __v2du __attribute__((__vector_size__(16)));
typedef int __v4si __attribute__((__vector_size__(16)));
typedef unsigned int __v4su __attribute__((__vector_size__(16)));
typedef short __v8hi __attribute__((__vector_size__(16)));
typedef unsigned short __v8hu __attribute__((__vector_size__(16)));
typedef char __v16qi __attribute__((__vector_size__(16)));
typedef signed char __v16qs __attribute__((__vector_size__(16)));
typedef unsigned char __v16qu __attribute__((__vector_size__(16)));

typedef enum {_MM_PERM_AAAA = 0x00, _MM_PERM_AAAB = 0x01, _MM_PERM_AAAC = 0x02, _MM_PERM_AAAD = 0x03, _MM_PERM_AABA = 0x04, _MM_PERM_AABB = 0x05, _MM_PERM_AABC = 0x06, _MM_PERM_AABD = 0x07, _MM_PERM_AACA = 0x08, _MM_PERM_AACB = 0x09, _MM_PERM_AACC = 0x0A, _MM_PERM_AACD = 0x0B, _MM_PERM_AADA = 0x0C, _MM_PERM_AADB = 0x0D, _MM_PERM_AADC = 0x0E, _MM_PERM_AADD = 0x0F, _MM_PERM_ABAA = 0x10, _MM_PERM_ABAB = 0x11, _MM_PERM_ABAC = 0x12, _MM_PERM_ABAD = 0x13, _MM_PERM_ABBA = 0x14, _MM_PERM_ABBB = 0x15, _MM_PERM_ABBC = 0x16, _MM_PERM_ABBD = 0x17, _MM_PERM_ABCA = 0x18, _MM_PERM_ABCB = 0x19, _MM_PERM_ABCC = 0x1A, _MM_PERM_ABCD = 0x1B, _MM_PERM_ABDA = 0x1C, _MM_PERM_ABDB = 0x1D, _MM_PERM_ABDC = 0x1E, _MM_PERM_ABDD = 0x1F, _MM_PERM_ACAA = 0x20, _MM_PERM_ACAB = 0x21, _MM_PERM_ACAC = 0x22, _MM_PERM_ACAD = 0x23, _MM_PERM_ACBA = 0x24, _MM_PERM_ACBB = 0x25, _MM_PERM_ACBC = 0x26, _MM_PERM_ACBD = 0x27, _MM_PERM_ACCA = 0x28, _MM_PERM_ACCB = 0x29, _MM_PERM_ACCC = 0x2A, _MM_PERM_ACCD = 0x2B, _MM_PERM_ACDA = 0x2C, _MM_PERM_ACDB = 0x2D, _MM_PERM_ACDC = 0x2E, _MM_PERM_ACDD = 0x2F, _MM_PERM_ADAA = 0x30, _MM_PERM_ADAB = 0x31, _MM_PERM_ADAC = 0x32, _MM_PERM_ADAD = 0x33, _MM_PERM_ADBA = 0x34, _MM_PERM_ADBB = 0x35, _MM_PERM_ADBC = 0x36, _MM_PERM_ADBD = 0x37, _MM_PERM_ADCA = 0x38, _MM_PERM_ADCB = 0x39, _MM_PERM_ADCC = 0x3A, _MM_PERM_ADCD = 0x3B, _MM_PERM_ADDA = 0x3C, _MM_PERM_ADDB = 0x3D, _MM_PERM_ADDC = 0x3E, _MM_PERM_ADDD = 0x3F, _MM_PERM_BAAA = 0x40, _MM_PERM_BAAB = 0x41, _MM_PERM_BAAC = 0x42, _MM_PERM_BAAD = 0x43, _MM_PERM_BABA = 0x44, _MM_PERM_BABB = 0x45, _MM_PERM_BABC = 0x46, _MM_PERM_BABD = 0x47, _MM_PERM_BACA = 0x48, _MM_PERM_BACB = 0x49, _MM_PERM_BACC = 0x4A, _MM_PERM_BACD = 0x4B, _MM_PERM_BADA = 0x4C, _MM_PERM_BADB = 0x4D, _MM_PERM_BADC = 0x4E, _MM_PERM_BADD = 0x4F, _MM_PERM_BBAA = 0x50, _MM_PERM_BBAB = 0x51, _MM_PERM_BBAC = 0x52, _MM_PERM_BBAD = 0x53, _MM_PERM_BBBA = 0x54, _MM_PERM_BBBB = 0x55, _MM_PERM_BBBC = 0x56, _MM_PERM_BBBD = 0x57, _MM_PERM_BBCA = 0x58, _MM_PERM_BBCB = 0x59, _MM_PERM_BBCC = 0x5A, _MM_PERM_BBCD = 0x5B, _MM_PERM_BBDA = 0x5C, _MM_PERM_BBDB = 0x5D, _MM_PERM_BBDC = 0x5E, _MM_PERM_BBDD = 0x5F, _MM_PERM_BCAA = 0x60, _MM_PERM_BCAB = 0x61, _MM_PERM_BCAC = 0x62, _MM_PERM_BCAD = 0x63, _MM_PERM_BCBA = 0x64, _MM_PERM_BCBB = 0x65, _MM_PERM_BCBC = 0x66, _MM_PERM_BCBD = 0x67, _MM_PERM_BCCA = 0x68, _MM_PERM_BCCB = 0x69, _MM_PERM_BCCC = 0x6A, _MM_PERM_BCCD = 0x6B, _MM_PERM_BCDA = 0x6C, _MM_PERM_BCDB = 0x6D, _MM_PERM_BCDC = 0x6E, _MM_PERM_BCDD = 0x6F, _MM_PERM_BDAA = 0x70, _MM_PERM_BDAB = 0x71, _MM_PERM_BDAC = 0x72, _MM_PERM_BDAD = 0x73, _MM_PERM_BDBA = 0x74, _MM_PERM_BDBB = 0x75, _MM_PERM_BDBC = 0x76, _MM_PERM_BDBD = 0x77, _MM_PERM_BDCA = 0x78, _MM_PERM_BDCB = 0x79, _MM_PERM_BDCC = 0x7A, _MM_PERM_BDCD = 0x7B, _MM_PERM_BDDA = 0x7C, _MM_PERM_BDDB = 0x7D, _MM_PERM_BDDC = 0x7E, _MM_PERM_BDDD = 0x7F, _MM_PERM_CAAA = 0x80, _MM_PERM_CAAB = 0x81, _MM_PERM_CAAC = 0x82, _MM_PERM_CAAD = 0x83, _MM_PERM_CABA = 0x84, _MM_PERM_CABB = 0x85, _MM_PERM_CABC = 0x86, _MM_PERM_CABD = 0x87, _MM_PERM_CACA = 0x88, _MM_PERM_CACB = 0x89, _MM_PERM_CACC = 0x8A, _MM_PERM_CACD = 0x8B, _MM_PERM_CADA = 0x8C, _MM_PERM_CADB = 0x8D, _MM_PERM_CADC = 0x8E, _MM_PERM_CADD = 0x8F, _MM_PERM_CBAA = 0x90, _MM_PERM_CBAB = 0x91, _MM_PERM_CBAC = 0x92, _MM_PERM_CBAD = 0x93, _MM_PERM_CBBA = 0x94, _MM_PERM_CBBB = 0x95, _MM_PERM_CBBC = 0x96, _MM_PERM_CBBD = 0x97, _MM_PERM_CBCA = 0x98, _MM_PERM_CBCB = 0x99, _MM_PERM_CBCC = 0x9A, _MM_PERM_CBCD = 0x9B, _MM_PERM_CBDA = 0x9C, _MM_PERM_CBDB = 0x9D, _MM_PERM_CBDC = 0x9E, _MM_PERM_CBDD = 0x9F, _MM_PERM_CCAA = 0xA0, _MM_PERM_CCAB = 0xA1, _MM_PERM_CCAC = 0xA2, _MM_PERM_CCAD = 0xA3, _MM_PERM_CCBA = 0xA4, _MM_PERM_CCBB = 0xA5, _MM_PERM_CCBC = 0xA6, _MM_PERM_CCBD = 0xA7, _MM_PERM_CCCA = 0xA8, _MM_PERM_CCCB = 0xA9, _MM_PERM_CCCC = 0xAA, _MM_PERM_CCCD = 0xAB, _MM_PERM_CCDA = 0xAC, _MM_PERM_CCDB = 0xAD, _MM_PERM_CCDC = 0xAE, _MM_PERM_CCDD = 0xAF, _MM_PERM_CDAA = 0xB0, _MM_PERM_CDAB = 0xB1, _MM_PERM_CDAC = 0xB2, _MM_PERM_CDAD = 0xB3, _MM_PERM_CDBA = 0xB4, _MM_PERM_CDBB = 0xB5, _MM_PERM_CDBC = 0xB6, _MM_PERM_CDBD = 0xB7, _MM_PERM_CDCA = 0xB8, _MM_PERM_CDCB = 0xB9, _MM_PERM_CDCC = 0xBA, _MM_PERM_CDCD = 0xBB, _MM_PERM_CDDA = 0xBC, _MM_PERM_CDDB = 0xBD, _MM_PERM_CDDC = 0xBE, _MM_PERM_CDDD = 0xBF, _MM_PERM_DAAA = 0xC0, _MM_PERM_DAAB = 0xC1, _MM_PERM_DAAC = 0xC2, _MM_PERM_DAAD = 0xC3, _MM_PERM_DABA = 0xC4, _MM_PERM_DABB = 0xC5, _MM_PERM_DABC = 0xC6, _MM_PERM_DABD = 0xC7, _MM_PERM_DACA = 0xC8, _MM_PERM_DACB = 0xC9, _MM_PERM_DACC = 0xCA, _MM_PERM_DACD = 0xCB, _MM_PERM_DADA = 0xCC, _MM_PERM_DADB = 0xCD, _MM_PERM_DADC = 0xCE, _MM_PERM_DADD = 0xCF, _MM_PERM_DBAA = 0xD0, _MM_PERM_DBAB = 0xD1, _MM_PERM_DBAC = 0xD2, _MM_PERM_DBAD = 0xD3, _MM_PERM_DBBA = 0xD4, _MM_PERM_DBBB = 0xD5, _MM_PERM_DBBC = 0xD6, _MM_PERM_DBBD = 0xD7, _MM_PERM_DBCA = 0xD8, _MM_PERM_DBCB = 0xD9, _MM_PERM_DBCC = 0xDA, _MM_PERM_DBCD = 0xDB, _MM_PERM_DBDA = 0xDC, _MM_PERM_DBDB = 0xDD, _MM_PERM_DBDC = 0xDE, _MM_PERM_DBDD = 0xDF, _MM_PERM_DCAA = 0xE0, _MM_PERM_DCAB = 0xE1, _MM_PERM_DCAC = 0xE2, _MM_PERM_DCAD = 0xE3, _MM_PERM_DCBA = 0xE4, _MM_PERM_DCBB = 0xE5, _MM_PERM_DCBC = 0xE6, _MM_PERM_DCBD = 0xE7, _MM_PERM_DCCA = 0xE8, _MM_PERM_DCCB = 0xE9, _MM_PERM_DCCC = 0xEA, _MM_PERM_DCCD = 0xEB, _MM_PERM_DCDA = 0xEC, _MM_PERM_DCDB = 0xED, _MM_PERM_DCDC = 0xEE, _MM_PERM_DCDD = 0xEF, _MM_PERM_DDAA = 0xF0, _MM_PERM_DDAB = 0xF1, _MM_PERM_DDAC = 0xF2, _MM_PERM_DDAD = 0xF3, _MM_PERM_DDBA = 0xF4, _MM_PERM_DDBB = 0xF5, _MM_PERM_DDBC = 0xF6, _MM_PERM_DDBD = 0xF7, _MM_PERM_DDCA = 0xF8, _MM_PERM_DDCB = 0xF9, _MM_PERM_DDCC = 0xFA, _MM_PERM_DDCD = 0xFB, _MM_PERM_DDDA = 0xFC, _MM_PERM_DDDB = 0xFD, _MM_PERM_DDDC = 0xFE, _MM_PERM_DDDD = 0xFF} _MM_PERM_ENUM;
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#define _MM_SHUFFLE2(x, y) (((x) << 1) | (y))

#define _MM_FROUND_TO_NEAREST_INT 0x00
#define _MM_FROUND_TO_NEG_INF 0x01
#define _MM_FROUND_TO_POS_INF 0x02
#define _MM_FROUND_TO_ZERO 0x03
#define _MM_FROUND_CUR_DIRECTION 0x04
#define _MM_FROUND_RAISE_EXC 0x00
#define _MM_FROUND_NO_EXC 0x08
#define _MM_FROUND_NINT (_MM_FROUND_TO_NEAREST_INT | _MM_FROUND_RAISE_EXC)
#define _MM_FROUND_FLOOR (_MM_FROUND_TO_NEG_INF | _MM_FROUND_RAISE_EXC)
#define _MM_FROUND_CEIL (_MM_FROUND_TO_POS_INF | _MM_FROUND_RAISE_EXC)
#define _MM_FROUND_TRUNC (_MM_FROUND_TO_ZERO | _MM_FROUND_RAISE_EXC)
#define _MM_FROUND_RINT (_MM_FROUND_CUR_DIRECTION | _MM_FROUND_RAISE_EXC)
#define _MM_FROUND_NEARBYINT (_MM_FROUND_CUR_DIRECTION | _MM_FROUND_NO_EXC)

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

#define _MM_HINT_T0 3
#define _MM_HINT_T1 2
#define _MM_HINT_T2 1
#define _MM_HINT_NTA 0

#define _andn_u32 __builtin_x86_andn_u32
#define _andn_u64 __builtin_x86_andn_u64
#define _bzhi_u32 __builtin_x86_bzhi_u32
#define _bzhi_u64 __builtin_x86_bzhi_u64
#define _cvtsh_ss __builtin_x86_cvtsh_ss
#define _cvtss_sh __builtin_x86_cvtss_sh
#define _mm256_abs_epi16 __builtin_x86_mm256_abs_epi16
#define _mm256_abs_epi32 __builtin_x86_mm256_abs_epi32
#define _mm256_abs_epi8 __builtin_x86_mm256_abs_epi8
#define _mm256_add_epi16 __builtin_x86_mm256_add_epi16
#define _mm256_add_epi32 __builtin_x86_mm256_add_epi32
#define _mm256_add_epi64 __builtin_x86_mm256_add_epi64
#define _mm256_add_epi8 __builtin_x86_mm256_add_epi8
#define _mm256_add_pd __builtin_x86_mm256_add_pd
#define _mm256_add_ps __builtin_x86_mm256_add_ps
#define _mm256_adds_epi16 __builtin_x86_mm256_adds_epi16
#define _mm256_adds_epi8 __builtin_x86_mm256_adds_epi8
#define _mm256_adds_epu16 __builtin_x86_mm256_adds_epu16
#define _mm256_adds_epu8 __builtin_x86_mm256_adds_epu8
#define _mm256_addsub_ps __builtin_x86_mm256_addsub_ps
#define _mm256_alignr_epi8 __builtin_x86_mm256_alignr_epi8
#define _mm256_and_pd __builtin_x86_mm256_and_pd
#define _mm256_and_ps __builtin_x86_mm256_and_ps
#define _mm256_and_si256 __builtin_x86_mm256_and_si256
#define _mm256_andnot_pd __builtin_x86_mm256_andnot_pd
#define _mm256_andnot_ps __builtin_x86_mm256_andnot_ps
#define _mm256_andnot_si256 __builtin_x86_mm256_andnot_si256
#define _mm256_avg_epu16 __builtin_x86_mm256_avg_epu16
#define _mm256_avg_epu8 __builtin_x86_mm256_avg_epu8
#define _mm256_blend_epi16 __builtin_x86_mm256_blend_epi16
#define _mm256_blend_epi32 __builtin_x86_mm256_blend_epi32
#define _mm256_blend_pd __builtin_x86_mm256_blend_pd
#define _mm256_blend_ps __builtin_x86_mm256_blend_ps
#define _mm256_blendv_epi8 __builtin_x86_mm256_blendv_epi8
#define _mm256_blendv_pd __builtin_x86_mm256_blendv_pd
#define _mm256_blendv_ps __builtin_x86_mm256_blendv_ps
#define _mm256_broadcast_sd __builtin_x86_mm256_broadcast_sd
#define _mm256_broadcast_ss __builtin_x86_mm256_broadcast_ss
#define _mm256_broadcastb_epi8 __builtin_x86_mm256_broadcastb_epi8
#define _mm256_broadcastd_epi32 __builtin_x86_mm256_broadcastd_epi32
#define _mm256_broadcastq_epi64 __builtin_x86_mm256_broadcastq_epi64
#define _mm256_broadcastsd_pd __builtin_x86_mm256_broadcastsd_pd
#define _mm256_broadcastsi128_si256 __builtin_x86_mm256_broadcastsi128_si256
#define _mm256_broadcastss_ps __builtin_x86_mm256_broadcastss_ps
#define _mm256_broadcastw_epi16 __builtin_x86_mm256_broadcastw_epi16
#define _mm256_bslli_epi128 __builtin_x86_mm256_bslli_epi128
#define _mm256_bsrli_epi128 __builtin_x86_mm256_bsrli_epi128
#define _mm256_castpd128_pd256 __builtin_x86_mm256_castpd128_pd256
#define _mm256_castpd256_pd128 __builtin_x86_mm256_castpd256_pd128
#define _mm256_castpd_ps __builtin_x86_mm256_castpd_ps
#define _mm256_castpd_si256 __builtin_x86_mm256_castpd_si256
#define _mm256_castps128_ps256 __builtin_x86_mm256_castps128_ps256
#define _mm256_castps256_ps128 __builtin_x86_mm256_castps256_ps128
#define _mm256_castps_pd __builtin_x86_mm256_castps_pd
#define _mm256_castps_si256 __builtin_x86_mm256_castps_si256
#define _mm256_castsi128_si256 __builtin_x86_mm256_castsi128_si256
#define _mm256_castsi256_pd __builtin_x86_mm256_castsi256_pd
#define _mm256_castsi256_ps __builtin_x86_mm256_castsi256_ps
#define _mm256_castsi256_si128 __builtin_x86_mm256_castsi256_si128
#define _mm256_ceil_pd __builtin_x86_mm256_ceil_pd
#define _mm256_ceil_ps __builtin_x86_mm256_ceil_ps
#define _mm256_cmp_pd __builtin_x86_mm256_cmp_pd
#define _mm256_cmp_ps __builtin_x86_mm256_cmp_ps
#define _mm256_cmpeq_epi16 __builtin_x86_mm256_cmpeq_epi16
#define _mm256_cmpeq_epi32 __builtin_x86_mm256_cmpeq_epi32
#define _mm256_cmpeq_epi64 __builtin_x86_mm256_cmpeq_epi64
#define _mm256_cmpeq_epi8 __builtin_x86_mm256_cmpeq_epi8
#define _mm256_cmpgt_epi16 __builtin_x86_mm256_cmpgt_epi16
#define _mm256_cmpgt_epi32 __builtin_x86_mm256_cmpgt_epi32
#define _mm256_cmpgt_epi64 __builtin_x86_mm256_cmpgt_epi64
#define _mm256_cmpgt_epi8 __builtin_x86_mm256_cmpgt_epi8
#define _mm256_cvtepi16_epi32 __builtin_x86_mm256_cvtepi16_epi32
#define _mm256_cvtepi16_epi64 __builtin_x86_mm256_cvtepi16_epi64
#define _mm256_cvtepi32_epi64 __builtin_x86_mm256_cvtepi32_epi64
#define _mm256_cvtepi32_pd __builtin_x86_mm256_cvtepi32_pd
#define _mm256_cvtepi32_ps __builtin_x86_mm256_cvtepi32_ps
#define _mm256_cvtepi8_epi16 __builtin_x86_mm256_cvtepi8_epi16
#define _mm256_cvtepi8_epi32 __builtin_x86_mm256_cvtepi8_epi32
#define _mm256_cvtepi8_epi64 __builtin_x86_mm256_cvtepi8_epi64
#define _mm256_cvtepu16_epi32 __builtin_x86_mm256_cvtepu16_epi32
#define _mm256_cvtepu16_epi64 __builtin_x86_mm256_cvtepu16_epi64
#define _mm256_cvtepu32_epi64 __builtin_x86_mm256_cvtepu32_epi64
#define _mm256_cvtepu8_epi16 __builtin_x86_mm256_cvtepu8_epi16
#define _mm256_cvtepu8_epi32 __builtin_x86_mm256_cvtepu8_epi32
#define _mm256_cvtepu8_epi64 __builtin_x86_mm256_cvtepu8_epi64
#define _mm256_cvtpd_epi32 __builtin_x86_mm256_cvtpd_epi32
#define _mm256_cvtpd_ps __builtin_x86_mm256_cvtpd_ps
#define _mm256_cvtph_ps __builtin_x86_mm256_cvtph_ps
#define _mm256_cvtps_epi32 __builtin_x86_mm256_cvtps_epi32
#define _mm256_cvtps_pd __builtin_x86_mm256_cvtps_pd
#define _mm256_cvtps_ph __builtin_x86_mm256_cvtps_ph
#define _mm256_cvtsd_f64 __builtin_x86_mm256_cvtsd_f64
#define _mm256_cvtsi256_si32 __builtin_x86_mm256_cvtsi256_si32
#define _mm256_cvtss_f32 __builtin_x86_mm256_cvtss_f32
#define _mm256_cvttpd_epi32 __builtin_x86_mm256_cvttpd_epi32
#define _mm256_cvttps_epi32 __builtin_x86_mm256_cvttps_epi32
#define _mm256_div_pd __builtin_x86_mm256_div_pd
#define _mm256_div_ps __builtin_x86_mm256_div_ps
#define _mm256_dp_ps __builtin_x86_mm256_dp_ps
#define _mm256_extract_epi16 __builtin_x86_mm256_extract_epi16
#define _mm256_extract_epi32 __builtin_x86_mm256_extract_epi32
#define _mm256_extract_epi64 __builtin_x86_mm256_extract_epi64
#define _mm256_extract_epi8 __builtin_x86_mm256_extract_epi8
#define _mm256_extractf128_pd __builtin_x86_mm256_extractf128_pd
#define _mm256_extractf128_ps __builtin_x86_mm256_extractf128_ps
#define _mm256_extractf128_si256 __builtin_x86_mm256_extractf128_si256
#define _mm256_extracti128_si256 __builtin_x86_mm256_extracti128_si256
#define _mm256_floor_pd __builtin_x86_mm256_floor_pd
#define _mm256_floor_ps __builtin_x86_mm256_floor_ps
#define _mm256_fmadd_pd __builtin_x86_mm256_fmadd_pd
#define _mm256_fmadd_ps __builtin_x86_mm256_fmadd_ps
#define _mm256_fmsub_pd __builtin_x86_mm256_fmsub_pd
#define _mm256_fmsub_ps __builtin_x86_mm256_fmsub_ps
#define _mm256_fnmadd_pd __builtin_x86_mm256_fnmadd_pd
#define _mm256_fnmadd_ps __builtin_x86_mm256_fnmadd_ps
#define _mm256_fnmsub_pd __builtin_x86_mm256_fnmsub_pd
#define _mm256_fnmsub_ps __builtin_x86_mm256_fnmsub_ps
#define _mm256_hadd_epi16 __builtin_x86_mm256_hadd_epi16
#define _mm256_hadd_epi32 __builtin_x86_mm256_hadd_epi32
#define _mm256_hadd_ps __builtin_x86_mm256_hadd_ps
#define _mm256_i32gather_epi32 __builtin_x86_mm256_i32gather_epi32
#define _mm256_i32gather_epi64 __builtin_x86_mm256_i32gather_epi64
#define _mm256_i32gather_pd __builtin_x86_mm256_i32gather_pd
#define _mm256_i32gather_ps __builtin_x86_mm256_i32gather_ps
#define _mm256_insertf128_pd __builtin_x86_mm256_insertf128_pd
#define _mm256_insertf128_ps __builtin_x86_mm256_insertf128_ps
#define _mm256_insertf128_si256 __builtin_x86_mm256_insertf128_si256
#define _mm256_inserti128_si256 __builtin_x86_mm256_inserti128_si256
#define _mm256_load_pd __builtin_x86_mm256_load_pd
#define _mm256_load_ps __builtin_x86_mm256_load_ps
#define _mm256_load_si256 __builtin_x86_mm256_load_si256
#define _mm256_loadu_pd __builtin_x86_mm256_loadu_pd
#define _mm256_loadu_ps __builtin_x86_mm256_loadu_ps
#define _mm256_loadu_si256 __builtin_x86_mm256_loadu_si256
#define _mm256_madd_epi16 __builtin_x86_mm256_madd_epi16
#define _mm256_maddubs_epi16 __builtin_x86_mm256_maddubs_epi16
#define _mm256_mask_i32gather_epi32 __builtin_x86_mm256_mask_i32gather_epi32
#define _mm256_mask_i32gather_epi64 __builtin_x86_mm256_mask_i32gather_epi64
#define _mm256_mask_i32gather_pd __builtin_x86_mm256_mask_i32gather_pd
#define _mm256_mask_i32gather_ps __builtin_x86_mm256_mask_i32gather_ps
#define _mm256_max_epi16 __builtin_x86_mm256_max_epi16
#define _mm256_max_epi32 __builtin_x86_mm256_max_epi32
#define _mm256_max_epi8 __builtin_x86_mm256_max_epi8
#define _mm256_max_epu16 __builtin_x86_mm256_max_epu16
#define _mm256_max_epu32 __builtin_x86_mm256_max_epu32
#define _mm256_max_epu8 __builtin_x86_mm256_max_epu8
#define _mm256_max_pd __builtin_x86_mm256_max_pd
#define _mm256_max_ps __builtin_x86_mm256_max_ps
#define _mm256_min_epi16 __builtin_x86_mm256_min_epi16
#define _mm256_min_epi32 __builtin_x86_mm256_min_epi32
#define _mm256_min_epi8 __builtin_x86_mm256_min_epi8
#define _mm256_min_epu16 __builtin_x86_mm256_min_epu16
#define _mm256_min_epu32 __builtin_x86_mm256_min_epu32
#define _mm256_min_epu8 __builtin_x86_mm256_min_epu8
#define _mm256_min_pd __builtin_x86_mm256_min_pd
#define _mm256_min_ps __builtin_x86_mm256_min_ps
#define _mm256_movehdup_ps __builtin_x86_mm256_movehdup_ps
#define _mm256_moveldup_ps __builtin_x86_mm256_moveldup_ps
#define _mm256_movemask_epi8 __builtin_x86_mm256_movemask_epi8
#define _mm256_movemask_pd __builtin_x86_mm256_movemask_pd
#define _mm256_movemask_ps __builtin_x86_mm256_movemask_ps
#define _mm256_mul_epi32 __builtin_x86_mm256_mul_epi32
#define _mm256_mul_epu32 __builtin_x86_mm256_mul_epu32
#define _mm256_mul_pd __builtin_x86_mm256_mul_pd
#define _mm256_mul_ps __builtin_x86_mm256_mul_ps
#define _mm256_mulhi_epi16 __builtin_x86_mm256_mulhi_epi16
#define _mm256_mulhi_epu16 __builtin_x86_mm256_mulhi_epu16
#define _mm256_mulhrs_epi16 __builtin_x86_mm256_mulhrs_epi16
#define _mm256_mullo_epi16 __builtin_x86_mm256_mullo_epi16
#define _mm256_mullo_epi32 __builtin_x86_mm256_mullo_epi32
#define _mm256_or_pd __builtin_x86_mm256_or_pd
#define _mm256_or_ps __builtin_x86_mm256_or_ps
#define _mm256_or_si256 __builtin_x86_mm256_or_si256
#define _mm256_packs_epi16 __builtin_x86_mm256_packs_epi16
#define _mm256_packs_epi32 __builtin_x86_mm256_packs_epi32
#define _mm256_packus_epi16 __builtin_x86_mm256_packus_epi16
#define _mm256_packus_epi32 __builtin_x86_mm256_packus_epi32
#define _mm256_permute2f128_pd __builtin_x86_mm256_permute2f128_pd
#define _mm256_permute2f128_ps __builtin_x86_mm256_permute2f128_ps
#define _mm256_permute2f128_si256 __builtin_x86_mm256_permute2f128_si256
#define _mm256_permute2x128_si256 __builtin_x86_mm256_permute2x128_si256
#define _mm256_permute4x64_epi64 __builtin_x86_mm256_permute4x64_epi64
#define _mm256_permute4x64_pd __builtin_x86_mm256_permute4x64_pd
#define _mm256_permute_ps __builtin_x86_mm256_permute_ps
#define _mm256_permutevar8x32_epi32 __builtin_x86_mm256_permutevar8x32_epi32
#define _mm256_permutevar8x32_ps __builtin_x86_mm256_permutevar8x32_ps
#define _mm256_permutevar_ps __builtin_x86_mm256_permutevar_ps
#define _mm256_rcp_ps __builtin_x86_mm256_rcp_ps
#define _mm256_round_pd __builtin_x86_mm256_round_pd
#define _mm256_round_ps __builtin_x86_mm256_round_ps
#define _mm256_rsqrt_ps __builtin_x86_mm256_rsqrt_ps
#define _mm256_sad_epu8 __builtin_x86_mm256_sad_epu8
#define _mm256_set1_epi16 __builtin_x86_mm256_set1_epi16
#define _mm256_set1_epi32 __builtin_x86_mm256_set1_epi32
#define _mm256_set1_epi64x __builtin_x86_mm256_set1_epi64x
#define _mm256_set1_epi8 __builtin_x86_mm256_set1_epi8
#define _mm256_set1_pd __builtin_x86_mm256_set1_pd
#define _mm256_set1_ps __builtin_x86_mm256_set1_ps
#define _mm256_set_epi16 __builtin_x86_mm256_set_epi16
#define _mm256_set_epi32 __builtin_x86_mm256_set_epi32
#define _mm256_set_epi64x __builtin_x86_mm256_set_epi64x
#define _mm256_set_epi8 __builtin_x86_mm256_set_epi8
#define _mm256_set_m128 __builtin_x86_mm256_set_m128
#define _mm256_set_m128i __builtin_x86_mm256_set_m128i
#define _mm256_set_pd __builtin_x86_mm256_set_pd
#define _mm256_set_ps __builtin_x86_mm256_set_ps
#define _mm256_setr_epi16 __builtin_x86_mm256_setr_epi16
#define _mm256_setr_epi32 __builtin_x86_mm256_setr_epi32
#define _mm256_setr_epi64x __builtin_x86_mm256_setr_epi64x
#define _mm256_setr_epi8 __builtin_x86_mm256_setr_epi8
#define _mm256_setr_m128 __builtin_x86_mm256_setr_m128
#define _mm256_setr_m128i __builtin_x86_mm256_setr_m128i
#define _mm256_setr_pd __builtin_x86_mm256_setr_pd
#define _mm256_setr_ps __builtin_x86_mm256_setr_ps
#define _mm256_setzero_pd __builtin_x86_mm256_setzero_pd
#define _mm256_setzero_ps __builtin_x86_mm256_setzero_ps
#define _mm256_setzero_si256 __builtin_x86_mm256_setzero_si256
#define _mm256_shuffle_epi32 __builtin_x86_mm256_shuffle_epi32
#define _mm256_shuffle_epi8 __builtin_x86_mm256_shuffle_epi8
#define _mm256_shuffle_pd __builtin_x86_mm256_shuffle_pd
#define _mm256_shuffle_ps __builtin_x86_mm256_shuffle_ps
#define _mm256_shufflehi_epi16 __builtin_x86_mm256_shufflehi_epi16
#define _mm256_shufflelo_epi16 __builtin_x86_mm256_shufflelo_epi16
#define _mm256_sign_epi16 __builtin_x86_mm256_sign_epi16
#define _mm256_sign_epi32 __builtin_x86_mm256_sign_epi32
#define _mm256_sign_epi8 __builtin_x86_mm256_sign_epi8
#define _mm256_sll_epi16 __builtin_x86_mm256_sll_epi16
#define _mm256_sll_epi32 __builtin_x86_mm256_sll_epi32
#define _mm256_sll_epi64 __builtin_x86_mm256_sll_epi64
#define _mm256_slli_epi16 __builtin_x86_mm256_slli_epi16
#define _mm256_slli_epi32 __builtin_x86_mm256_slli_epi32
#define _mm256_slli_epi64 __builtin_x86_mm256_slli_epi64
#define _mm256_slli_si256 __builtin_x86_mm256_slli_si256
#define _mm256_sllv_epi32 __builtin_x86_mm256_sllv_epi32
#define _mm256_sllv_epi64 __builtin_x86_mm256_sllv_epi64
#define _mm256_sqrt_pd __builtin_x86_mm256_sqrt_pd
#define _mm256_sqrt_ps __builtin_x86_mm256_sqrt_ps
#define _mm256_sra_epi16 __builtin_x86_mm256_sra_epi16
#define _mm256_sra_epi32 __builtin_x86_mm256_sra_epi32
#define _mm256_srai_epi16 __builtin_x86_mm256_srai_epi16
#define _mm256_srai_epi32 __builtin_x86_mm256_srai_epi32
#define _mm256_srav_epi32 __builtin_x86_mm256_srav_epi32
#define _mm256_srl_epi16 __builtin_x86_mm256_srl_epi16
#define _mm256_srl_epi32 __builtin_x86_mm256_srl_epi32
#define _mm256_srl_epi64 __builtin_x86_mm256_srl_epi64
#define _mm256_srli_epi16 __builtin_x86_mm256_srli_epi16
#define _mm256_srli_epi32 __builtin_x86_mm256_srli_epi32
#define _mm256_srli_epi64 __builtin_x86_mm256_srli_epi64
#define _mm256_srli_si256 __builtin_x86_mm256_srli_si256
#define _mm256_srlv_epi32 __builtin_x86_mm256_srlv_epi32
#define _mm256_srlv_epi64 __builtin_x86_mm256_srlv_epi64
#define _mm256_store_pd __builtin_x86_mm256_store_pd
#define _mm256_store_ps __builtin_x86_mm256_store_ps
#define _mm256_store_si256 __builtin_x86_mm256_store_si256
#define _mm256_storeu_pd __builtin_x86_mm256_storeu_pd
#define _mm256_storeu_ps __builtin_x86_mm256_storeu_ps
#define _mm256_storeu_si256 __builtin_x86_mm256_storeu_si256
#define _mm256_sub_epi16 __builtin_x86_mm256_sub_epi16
#define _mm256_sub_epi32 __builtin_x86_mm256_sub_epi32
#define _mm256_sub_epi64 __builtin_x86_mm256_sub_epi64
#define _mm256_sub_epi8 __builtin_x86_mm256_sub_epi8
#define _mm256_sub_pd __builtin_x86_mm256_sub_pd
#define _mm256_sub_ps __builtin_x86_mm256_sub_ps
#define _mm256_subs_epi16 __builtin_x86_mm256_subs_epi16
#define _mm256_subs_epi8 __builtin_x86_mm256_subs_epi8
#define _mm256_subs_epu16 __builtin_x86_mm256_subs_epu16
#define _mm256_subs_epu8 __builtin_x86_mm256_subs_epu8
#define _mm256_testc_pd __builtin_x86_mm256_testc_pd
#define _mm256_testc_ps __builtin_x86_mm256_testc_ps
#define _mm256_testc_si256 __builtin_x86_mm256_testc_si256
#define _mm256_testz_pd __builtin_x86_mm256_testz_pd
#define _mm256_testz_ps __builtin_x86_mm256_testz_ps
#define _mm256_testz_si256 __builtin_x86_mm256_testz_si256
#define _mm256_undefined_ps __builtin_x86_mm256_undefined_ps
#define _mm256_undefined_si256 __builtin_x86_mm256_undefined_si256
#define _mm256_unpackhi_epi16 __builtin_x86_mm256_unpackhi_epi16
#define _mm256_unpackhi_epi32 __builtin_x86_mm256_unpackhi_epi32
#define _mm256_unpackhi_epi64 __builtin_x86_mm256_unpackhi_epi64
#define _mm256_unpackhi_epi8 __builtin_x86_mm256_unpackhi_epi8
#define _mm256_unpackhi_pd __builtin_x86_mm256_unpackhi_pd
#define _mm256_unpackhi_ps __builtin_x86_mm256_unpackhi_ps
#define _mm256_unpacklo_epi16 __builtin_x86_mm256_unpacklo_epi16
#define _mm256_unpacklo_epi32 __builtin_x86_mm256_unpacklo_epi32
#define _mm256_unpacklo_epi64 __builtin_x86_mm256_unpacklo_epi64
#define _mm256_unpacklo_epi8 __builtin_x86_mm256_unpacklo_epi8
#define _mm256_unpacklo_pd __builtin_x86_mm256_unpacklo_pd
#define _mm256_unpacklo_ps __builtin_x86_mm256_unpacklo_ps
#define _mm256_xor_pd __builtin_x86_mm256_xor_pd
#define _mm256_xor_ps __builtin_x86_mm256_xor_ps
#define _mm256_xor_si256 __builtin_x86_mm256_xor_si256
#define _mm256_zeroupper __builtin_x86_mm256_zeroupper
#define _mm256_zextpd128_pd256 __builtin_x86_mm256_zextpd128_pd256
#define _mm256_zextps128_ps256 __builtin_x86_mm256_zextps128_ps256
#define _mm256_zextsi128_si256 __builtin_x86_mm256_zextsi128_si256
#define _mm512_add_epi16 __builtin_x86_mm512_add_epi16
#define _mm512_add_epi32 __builtin_x86_mm512_add_epi32
#define _mm512_add_epi64 __builtin_x86_mm512_add_epi64
#define _mm512_add_epi8 __builtin_x86_mm512_add_epi8
#define _mm512_add_pd __builtin_x86_mm512_add_pd
#define _mm512_add_ps __builtin_x86_mm512_add_ps
#define _mm512_and_epi32 __builtin_x86_mm512_and_epi32
#define _mm512_and_epi64 __builtin_x86_mm512_and_epi64
#define _mm512_and_si512 __builtin_x86_mm512_and_si512
#define _mm512_andnot_si512 __builtin_x86_mm512_andnot_si512
#define _mm512_castpd_ps __builtin_x86_mm512_castpd_ps
#define _mm512_castpd_si512 __builtin_x86_mm512_castpd_si512
#define _mm512_castps_pd __builtin_x86_mm512_castps_pd
#define _mm512_castps_si512 __builtin_x86_mm512_castps_si512
#define _mm512_castsi512_pd __builtin_x86_mm512_castsi512_pd
#define _mm512_castsi512_ps __builtin_x86_mm512_castsi512_ps
#define _mm512_cvtepi32_ps __builtin_x86_mm512_cvtepi32_ps
#define _mm512_cvttps_epi32 __builtin_x86_mm512_cvttps_epi32
#define _mm512_div_pd __builtin_x86_mm512_div_pd
#define _mm512_div_ps __builtin_x86_mm512_div_ps
#define _mm512_fmadd_pd __builtin_x86_mm512_fmadd_pd
#define _mm512_fmadd_ps __builtin_x86_mm512_fmadd_ps
#define _mm512_fmsub_pd __builtin_x86_mm512_fmsub_pd
#define _mm512_fmsub_ps __builtin_x86_mm512_fmsub_ps
#define _mm512_fnmadd_pd __builtin_x86_mm512_fnmadd_pd
#define _mm512_fnmadd_ps __builtin_x86_mm512_fnmadd_ps
#define _mm512_fnmsub_pd __builtin_x86_mm512_fnmsub_pd
#define _mm512_fnmsub_ps __builtin_x86_mm512_fnmsub_ps
#define _mm512_load_epi32 __builtin_x86_mm512_load_epi32
#define _mm512_load_epi64 __builtin_x86_mm512_load_epi64
#define _mm512_load_pd __builtin_x86_mm512_load_pd
#define _mm512_load_ps __builtin_x86_mm512_load_ps
#define _mm512_load_si512 __builtin_x86_mm512_load_si512
#define _mm512_loadu_epi32 __builtin_x86_mm512_loadu_epi32
#define _mm512_loadu_epi64 __builtin_x86_mm512_loadu_epi64
#define _mm512_loadu_pd __builtin_x86_mm512_loadu_pd
#define _mm512_loadu_ps __builtin_x86_mm512_loadu_ps
#define _mm512_loadu_si512 __builtin_x86_mm512_loadu_si512
#define _mm512_mask_add_epi32 __builtin_x86_mm512_mask_add_epi32
#define _mm512_mask_add_epi64 __builtin_x86_mm512_mask_add_epi64
#define _mm512_mask_sub_epi32 __builtin_x86_mm512_mask_sub_epi32
#define _mm512_mask_sub_epi64 __builtin_x86_mm512_mask_sub_epi64
#define _mm512_max_epu8 __builtin_x86_mm512_max_epu8
#define _mm512_max_pd __builtin_x86_mm512_max_pd
#define _mm512_max_ps __builtin_x86_mm512_max_ps
#define _mm512_min_epu8 __builtin_x86_mm512_min_epu8
#define _mm512_min_pd __builtin_x86_mm512_min_pd
#define _mm512_min_ps __builtin_x86_mm512_min_ps
#define _mm512_mul_epi32 __builtin_x86_mm512_mul_epi32
#define _mm512_mul_epu32 __builtin_x86_mm512_mul_epu32
#define _mm512_mul_pd __builtin_x86_mm512_mul_pd
#define _mm512_mul_ps __builtin_x86_mm512_mul_ps
#define _mm512_mullo_epi32 __builtin_x86_mm512_mullo_epi32
#define _mm512_or_epi32 __builtin_x86_mm512_or_epi32
#define _mm512_or_epi64 __builtin_x86_mm512_or_epi64
#define _mm512_or_si512 __builtin_x86_mm512_or_si512
#define _mm512_set1_epi16 __builtin_x86_mm512_set1_epi16
#define _mm512_set1_epi32 __builtin_x86_mm512_set1_epi32
#define _mm512_set1_epi64 __builtin_x86_mm512_set1_epi64
#define _mm512_set1_epi8 __builtin_x86_mm512_set1_epi8
#define _mm512_set1_pd __builtin_x86_mm512_set1_pd
#define _mm512_set1_ps __builtin_x86_mm512_set1_ps
#define _mm512_setzero_epi32 __builtin_x86_mm512_setzero_epi32
#define _mm512_setzero_pd __builtin_x86_mm512_setzero_pd
#define _mm512_setzero_ps __builtin_x86_mm512_setzero_ps
#define _mm512_setzero_si512 __builtin_x86_mm512_setzero_si512
#define _mm512_shuffle_epi32 __builtin_x86_mm512_shuffle_epi32
#define _mm512_slli_epi32 __builtin_x86_mm512_slli_epi32
#define _mm512_slli_epi64 __builtin_x86_mm512_slli_epi64
#define _mm512_sqrt_pd __builtin_x86_mm512_sqrt_pd
#define _mm512_sqrt_ps __builtin_x86_mm512_sqrt_ps
#define _mm512_srai_epi32 __builtin_x86_mm512_srai_epi32
#define _mm512_srli_epi32 __builtin_x86_mm512_srli_epi32
#define _mm512_srli_epi64 __builtin_x86_mm512_srli_epi64
#define _mm512_store_epi32 __builtin_x86_mm512_store_epi32
#define _mm512_store_epi64 __builtin_x86_mm512_store_epi64
#define _mm512_store_pd __builtin_x86_mm512_store_pd
#define _mm512_store_ps __builtin_x86_mm512_store_ps
#define _mm512_store_si512 __builtin_x86_mm512_store_si512
#define _mm512_storeu_epi32 __builtin_x86_mm512_storeu_epi32
#define _mm512_storeu_epi64 __builtin_x86_mm512_storeu_epi64
#define _mm512_storeu_pd __builtin_x86_mm512_storeu_pd
#define _mm512_storeu_ps __builtin_x86_mm512_storeu_ps
#define _mm512_storeu_si512 __builtin_x86_mm512_storeu_si512
#define _mm512_sub_epi16 __builtin_x86_mm512_sub_epi16
#define _mm512_sub_epi32 __builtin_x86_mm512_sub_epi32
#define _mm512_sub_epi64 __builtin_x86_mm512_sub_epi64
#define _mm512_sub_epi8 __builtin_x86_mm512_sub_epi8
#define _mm512_sub_pd __builtin_x86_mm512_sub_pd
#define _mm512_sub_ps __builtin_x86_mm512_sub_ps
#define _mm512_ternarylogic_epi32 __builtin_x86_mm512_ternarylogic_epi32
#define _mm512_ternarylogic_epi64 __builtin_x86_mm512_ternarylogic_epi64
#define _mm512_test_epi16_mask __builtin_x86_mm512_test_epi16_mask
#define _mm512_test_epi32_mask __builtin_x86_mm512_test_epi32_mask
#define _mm512_test_epi64_mask __builtin_x86_mm512_test_epi64_mask
#define _mm512_test_epi8_mask __builtin_x86_mm512_test_epi8_mask
#define _mm512_xor_epi32 __builtin_x86_mm512_xor_epi32
#define _mm512_xor_epi64 __builtin_x86_mm512_xor_epi64
#define _mm512_xor_si512 __builtin_x86_mm512_xor_si512
#define _mm_abs_epi16 __builtin_x86_mm_abs_epi16
#define _mm_abs_epi32 __builtin_x86_mm_abs_epi32
#define _mm_abs_epi8 __builtin_x86_mm_abs_epi8
#define _mm_add_epi16 __builtin_x86_mm_add_epi16
#define _mm_add_epi32 __builtin_x86_mm_add_epi32
#define _mm_add_epi64 __builtin_x86_mm_add_epi64
#define _mm_add_epi8 __builtin_x86_mm_add_epi8
#define _mm_add_pd __builtin_x86_mm_add_pd
#define _mm_add_ps __builtin_x86_mm_add_ps
#define _mm_adds_epi16 __builtin_x86_mm_adds_epi16
#define _mm_adds_epi8 __builtin_x86_mm_adds_epi8
#define _mm_adds_epu16 __builtin_x86_mm_adds_epu16
#define _mm_adds_epu8 __builtin_x86_mm_adds_epu8
#define _mm_addsub_ps __builtin_x86_mm_addsub_ps
#define _mm_alignr_epi8 __builtin_x86_mm_alignr_epi8
#define _mm_and_pd __builtin_x86_mm_and_pd
#define _mm_and_ps __builtin_x86_mm_and_ps
#define _mm_and_si128 __builtin_x86_mm_and_si128
#define _mm_andnot_pd __builtin_x86_mm_andnot_pd
#define _mm_andnot_ps __builtin_x86_mm_andnot_ps
#define _mm_andnot_si128 __builtin_x86_mm_andnot_si128
#define _mm_avg_epu16 __builtin_x86_mm_avg_epu16
#define _mm_avg_epu8 __builtin_x86_mm_avg_epu8
#define _mm_blend_epi16 __builtin_x86_mm_blend_epi16
#define _mm_blend_epi32 __builtin_x86_mm_blend_epi32
#define _mm_blend_pd __builtin_x86_mm_blend_pd
#define _mm_blend_ps __builtin_x86_mm_blend_ps
#define _mm_blendv_epi8 __builtin_x86_mm_blendv_epi8
#define _mm_blendv_pd __builtin_x86_mm_blendv_pd
#define _mm_blendv_ps __builtin_x86_mm_blendv_ps
#define _mm_broadcast_ss __builtin_x86_mm_broadcast_ss
#define _mm_broadcastb_epi8 __builtin_x86_mm_broadcastb_epi8
#define _mm_broadcastd_epi32 __builtin_x86_mm_broadcastd_epi32
#define _mm_broadcastq_epi64 __builtin_x86_mm_broadcastq_epi64
#define _mm_broadcastss_ps __builtin_x86_mm_broadcastss_ps
#define _mm_broadcastw_epi16 __builtin_x86_mm_broadcastw_epi16
#define _mm_bslli_si128 __builtin_x86_mm_bslli_si128
#define _mm_bsrli_si128 __builtin_x86_mm_bsrli_si128
#define _mm_castpd_ps __builtin_x86_mm_castpd_ps
#define _mm_castpd_si128 __builtin_x86_mm_castpd_si128
#define _mm_castps_pd __builtin_x86_mm_castps_pd
#define _mm_castps_si128 __builtin_x86_mm_castps_si128
#define _mm_castsi128_pd __builtin_x86_mm_castsi128_pd
#define _mm_castsi128_ps __builtin_x86_mm_castsi128_ps
#define _mm_ceil_pd __builtin_x86_mm_ceil_pd
#define _mm_ceil_ps __builtin_x86_mm_ceil_ps
#define _mm_cmp_pd __builtin_x86_mm_cmp_pd
#define _mm_cmp_ps __builtin_x86_mm_cmp_ps
#define _mm_cmpeq_epi16 __builtin_x86_mm_cmpeq_epi16
#define _mm_cmpeq_epi32 __builtin_x86_mm_cmpeq_epi32
#define _mm_cmpeq_epi64 __builtin_x86_mm_cmpeq_epi64
#define _mm_cmpeq_epi8 __builtin_x86_mm_cmpeq_epi8
#define _mm_cmpgt_epi16 __builtin_x86_mm_cmpgt_epi16
#define _mm_cmpgt_epi32 __builtin_x86_mm_cmpgt_epi32
#define _mm_cmpgt_epi64 __builtin_x86_mm_cmpgt_epi64
#define _mm_cmpgt_epi8 __builtin_x86_mm_cmpgt_epi8
#define _mm_cvtepi16_epi32 __builtin_x86_mm_cvtepi16_epi32
#define _mm_cvtepi16_epi64 __builtin_x86_mm_cvtepi16_epi64
#define _mm_cvtepi32_epi64 __builtin_x86_mm_cvtepi32_epi64
#define _mm_cvtepi32_ps __builtin_x86_mm_cvtepi32_ps
#define _mm_cvtepi8_epi16 __builtin_x86_mm_cvtepi8_epi16
#define _mm_cvtepi8_epi32 __builtin_x86_mm_cvtepi8_epi32
#define _mm_cvtepi8_epi64 __builtin_x86_mm_cvtepi8_epi64
#define _mm_cvtepu16_epi32 __builtin_x86_mm_cvtepu16_epi32
#define _mm_cvtepu16_epi64 __builtin_x86_mm_cvtepu16_epi64
#define _mm_cvtepu32_epi64 __builtin_x86_mm_cvtepu32_epi64
#define _mm_cvtepu8_epi16 __builtin_x86_mm_cvtepu8_epi16
#define _mm_cvtepu8_epi32 __builtin_x86_mm_cvtepu8_epi32
#define _mm_cvtepu8_epi64 __builtin_x86_mm_cvtepu8_epi64
#define _mm_cvtph_ps __builtin_x86_mm_cvtph_ps
#define _mm_cvtps_epi32 __builtin_x86_mm_cvtps_epi32
#define _mm_cvtps_ph __builtin_x86_mm_cvtps_ph
#define _mm_cvtsd_f64 __builtin_x86_mm_cvtsd_f64
#define _mm_cvtsi128_si32 __builtin_x86_mm_cvtsi128_si32
#define _mm_cvtsi128_si64 __builtin_x86_mm_cvtsi128_si64
#define _mm_cvtsi128_si64x __builtin_x86_mm_cvtsi128_si64x
#define _mm_cvtsi32_si128 __builtin_x86_mm_cvtsi32_si128
#define _mm_cvtsi64_si128 __builtin_x86_mm_cvtsi64_si128
#define _mm_cvtsi64x_si128 __builtin_x86_mm_cvtsi64x_si128
#define _mm_cvtss_f32 __builtin_x86_mm_cvtss_f32
#define _mm_cvttps_epi32 __builtin_x86_mm_cvttps_epi32
#define _mm_div_pd __builtin_x86_mm_div_pd
#define _mm_div_ps __builtin_x86_mm_div_ps
#define _mm_dp_ps __builtin_x86_mm_dp_ps
#define _mm_extract_epi16 __builtin_x86_mm_extract_epi16
#define _mm_extract_epi32 __builtin_x86_mm_extract_epi32
#define _mm_extract_epi64 __builtin_x86_mm_extract_epi64
#define _mm_extract_epi8 __builtin_x86_mm_extract_epi8
#define _mm_extract_ps __builtin_x86_mm_extract_ps
#define _mm_floor_pd __builtin_x86_mm_floor_pd
#define _mm_floor_ps __builtin_x86_mm_floor_ps
#define _mm_fmadd_pd __builtin_x86_mm_fmadd_pd
#define _mm_fmadd_ps __builtin_x86_mm_fmadd_ps
#define _mm_fmsub_pd __builtin_x86_mm_fmsub_pd
#define _mm_fmsub_ps __builtin_x86_mm_fmsub_ps
#define _mm_fnmadd_pd __builtin_x86_mm_fnmadd_pd
#define _mm_fnmadd_ps __builtin_x86_mm_fnmadd_ps
#define _mm_fnmsub_pd __builtin_x86_mm_fnmsub_pd
#define _mm_fnmsub_ps __builtin_x86_mm_fnmsub_ps
#define _mm_hadd_epi16 __builtin_x86_mm_hadd_epi16
#define _mm_hadd_epi32 __builtin_x86_mm_hadd_epi32
#define _mm_hadd_ps __builtin_x86_mm_hadd_ps
#define _mm_i32gather_epi32 __builtin_x86_mm_i32gather_epi32
#define _mm_i32gather_epi64 __builtin_x86_mm_i32gather_epi64
#define _mm_i32gather_pd __builtin_x86_mm_i32gather_pd
#define _mm_i32gather_ps __builtin_x86_mm_i32gather_ps
#define _mm_insert_epi16 __builtin_x86_mm_insert_epi16
#define _mm_insert_epi32 __builtin_x86_mm_insert_epi32
#define _mm_insert_epi64 __builtin_x86_mm_insert_epi64
#define _mm_insert_epi8 __builtin_x86_mm_insert_epi8
#define _mm_lfence __builtin_x86_mm_lfence
#define _mm_load_pd __builtin_x86_mm_load_pd
#define _mm_load_ps __builtin_x86_mm_load_ps
#define _mm_load_si128 __builtin_x86_mm_load_si128
#define _mm_loadl_epi64 __builtin_x86_mm_loadl_epi64
#define _mm_loadu_pd __builtin_x86_mm_loadu_pd
#define _mm_loadu_ps __builtin_x86_mm_loadu_ps
#define _mm_loadu_si128 __builtin_x86_mm_loadu_si128
#define _mm_loadu_si32 __builtin_x86_mm_loadu_si32
#define _mm_loadu_si64 __builtin_x86_mm_loadu_si64
#define _mm_madd_epi16 __builtin_x86_mm_madd_epi16
#define _mm_maddubs_epi16 __builtin_x86_mm_maddubs_epi16
#define _mm_mask_i32gather_epi32 __builtin_x86_mm_mask_i32gather_epi32
#define _mm_mask_i32gather_epi64 __builtin_x86_mm_mask_i32gather_epi64
#define _mm_mask_i32gather_pd __builtin_x86_mm_mask_i32gather_pd
#define _mm_mask_i32gather_ps __builtin_x86_mm_mask_i32gather_ps
#define _mm_max_epi16 __builtin_x86_mm_max_epi16
#define _mm_max_epi32 __builtin_x86_mm_max_epi32
#define _mm_max_epi8 __builtin_x86_mm_max_epi8
#define _mm_max_epu16 __builtin_x86_mm_max_epu16
#define _mm_max_epu32 __builtin_x86_mm_max_epu32
#define _mm_max_epu8 __builtin_x86_mm_max_epu8
#define _mm_max_pd __builtin_x86_mm_max_pd
#define _mm_max_ps __builtin_x86_mm_max_ps
#define _mm_mfence __builtin_x86_mm_mfence
#define _mm_min_epi16 __builtin_x86_mm_min_epi16
#define _mm_min_epi32 __builtin_x86_mm_min_epi32
#define _mm_min_epi8 __builtin_x86_mm_min_epi8
#define _mm_min_epu16 __builtin_x86_mm_min_epu16
#define _mm_min_epu32 __builtin_x86_mm_min_epu32
#define _mm_min_epu8 __builtin_x86_mm_min_epu8
#define _mm_min_pd __builtin_x86_mm_min_pd
#define _mm_min_ps __builtin_x86_mm_min_ps
#define _mm_movehdup_ps __builtin_x86_mm_movehdup_ps
#define _mm_moveldup_ps __builtin_x86_mm_moveldup_ps
#define _mm_movemask_epi8 __builtin_x86_mm_movemask_epi8
#define _mm_movemask_pd __builtin_x86_mm_movemask_pd
#define _mm_movemask_ps __builtin_x86_mm_movemask_ps
#define _mm_mul_epi32 __builtin_x86_mm_mul_epi32
#define _mm_mul_epu32 __builtin_x86_mm_mul_epu32
#define _mm_mul_pd __builtin_x86_mm_mul_pd
#define _mm_mul_ps __builtin_x86_mm_mul_ps
#define _mm_mulhi_epi16 __builtin_x86_mm_mulhi_epi16
#define _mm_mulhi_epu16 __builtin_x86_mm_mulhi_epu16
#define _mm_mulhrs_epi16 __builtin_x86_mm_mulhrs_epi16
#define _mm_mullo_epi16 __builtin_x86_mm_mullo_epi16
#define _mm_mullo_epi32 __builtin_x86_mm_mullo_epi32
#define _mm_or_pd __builtin_x86_mm_or_pd
#define _mm_or_ps __builtin_x86_mm_or_ps
#define _mm_or_si128 __builtin_x86_mm_or_si128
#define _mm_packs_epi16 __builtin_x86_mm_packs_epi16
#define _mm_packs_epi32 __builtin_x86_mm_packs_epi32
#define _mm_packus_epi16 __builtin_x86_mm_packus_epi16
#define _mm_packus_epi32 __builtin_x86_mm_packus_epi32
#define _mm_pause __builtin_x86_mm_pause
#define _mm_permute_ps __builtin_x86_mm_permute_ps
#define _mm_permutevar_ps __builtin_x86_mm_permutevar_ps
#define _mm_prefetch __builtin_x86_mm_prefetch
#define _mm_rcp_ps __builtin_x86_mm_rcp_ps
#define _mm_round_pd __builtin_x86_mm_round_pd
#define _mm_round_ps __builtin_x86_mm_round_ps
#define _mm_rsqrt_ps __builtin_x86_mm_rsqrt_ps
#define _mm_sad_epu8 __builtin_x86_mm_sad_epu8
#define _mm_set1_epi16 __builtin_x86_mm_set1_epi16
#define _mm_set1_epi32 __builtin_x86_mm_set1_epi32
#define _mm_set1_epi64x __builtin_x86_mm_set1_epi64x
#define _mm_set1_epi8 __builtin_x86_mm_set1_epi8
#define _mm_set1_pd __builtin_x86_mm_set1_pd
#define _mm_set1_ps __builtin_x86_mm_set1_ps
#define _mm_set_epi16 __builtin_x86_mm_set_epi16
#define _mm_set_epi32 __builtin_x86_mm_set_epi32
#define _mm_set_epi64x __builtin_x86_mm_set_epi64x
#define _mm_set_epi8 __builtin_x86_mm_set_epi8
#define _mm_set_pd __builtin_x86_mm_set_pd
#define _mm_set_ps __builtin_x86_mm_set_ps
#define _mm_setr_epi16 __builtin_x86_mm_setr_epi16
#define _mm_setr_epi32 __builtin_x86_mm_setr_epi32
#define _mm_setr_epi64x __builtin_x86_mm_setr_epi64x
#define _mm_setr_epi8 __builtin_x86_mm_setr_epi8
#define _mm_setr_pd __builtin_x86_mm_setr_pd
#define _mm_setr_ps __builtin_x86_mm_setr_ps
#define _mm_setzero_pd __builtin_x86_mm_setzero_pd
#define _mm_setzero_ps __builtin_x86_mm_setzero_ps
#define _mm_setzero_si128 __builtin_x86_mm_setzero_si128
#define _mm_sfence __builtin_x86_mm_sfence
#define _mm_shuffle_epi32 __builtin_x86_mm_shuffle_epi32
#define _mm_shuffle_epi8 __builtin_x86_mm_shuffle_epi8
#define _mm_shuffle_pd __builtin_x86_mm_shuffle_pd
#define _mm_shuffle_ps __builtin_x86_mm_shuffle_ps
#define _mm_shufflehi_epi16 __builtin_x86_mm_shufflehi_epi16
#define _mm_shufflelo_epi16 __builtin_x86_mm_shufflelo_epi16
#define _mm_sign_epi16 __builtin_x86_mm_sign_epi16
#define _mm_sign_epi32 __builtin_x86_mm_sign_epi32
#define _mm_sign_epi8 __builtin_x86_mm_sign_epi8
#define _mm_sll_epi16 __builtin_x86_mm_sll_epi16
#define _mm_sll_epi32 __builtin_x86_mm_sll_epi32
#define _mm_sll_epi64 __builtin_x86_mm_sll_epi64
#define _mm_slli_epi16 __builtin_x86_mm_slli_epi16
#define _mm_slli_epi32 __builtin_x86_mm_slli_epi32
#define _mm_slli_epi64 __builtin_x86_mm_slli_epi64
#define _mm_slli_si128 __builtin_x86_mm_slli_si128
#define _mm_sllv_epi32 __builtin_x86_mm_sllv_epi32
#define _mm_sllv_epi64 __builtin_x86_mm_sllv_epi64
#define _mm_sqrt_pd __builtin_x86_mm_sqrt_pd
#define _mm_sqrt_ps __builtin_x86_mm_sqrt_ps
#define _mm_sra_epi16 __builtin_x86_mm_sra_epi16
#define _mm_sra_epi32 __builtin_x86_mm_sra_epi32
#define _mm_srai_epi16 __builtin_x86_mm_srai_epi16
#define _mm_srai_epi32 __builtin_x86_mm_srai_epi32
#define _mm_srav_epi32 __builtin_x86_mm_srav_epi32
#define _mm_srl_epi16 __builtin_x86_mm_srl_epi16
#define _mm_srl_epi32 __builtin_x86_mm_srl_epi32
#define _mm_srl_epi64 __builtin_x86_mm_srl_epi64
#define _mm_srli_epi16 __builtin_x86_mm_srli_epi16
#define _mm_srli_epi32 __builtin_x86_mm_srli_epi32
#define _mm_srli_epi64 __builtin_x86_mm_srli_epi64
#define _mm_srli_si128 __builtin_x86_mm_srli_si128
#define _mm_srlv_epi32 __builtin_x86_mm_srlv_epi32
#define _mm_srlv_epi64 __builtin_x86_mm_srlv_epi64
#define _mm_store_pd __builtin_x86_mm_store_pd
#define _mm_store_ps __builtin_x86_mm_store_ps
#define _mm_store_si128 __builtin_x86_mm_store_si128
#define _mm_storel_epi64 __builtin_x86_mm_storel_epi64
#define _mm_storeu_pd __builtin_x86_mm_storeu_pd
#define _mm_storeu_ps __builtin_x86_mm_storeu_ps
#define _mm_storeu_si128 __builtin_x86_mm_storeu_si128
#define _mm_storeu_si32 __builtin_x86_mm_storeu_si32
#define _mm_storeu_si64 __builtin_x86_mm_storeu_si64
#define _mm_sub_epi16 __builtin_x86_mm_sub_epi16
#define _mm_sub_epi32 __builtin_x86_mm_sub_epi32
#define _mm_sub_epi64 __builtin_x86_mm_sub_epi64
#define _mm_sub_epi8 __builtin_x86_mm_sub_epi8
#define _mm_sub_pd __builtin_x86_mm_sub_pd
#define _mm_sub_ps __builtin_x86_mm_sub_ps
#define _mm_subs_epi16 __builtin_x86_mm_subs_epi16
#define _mm_subs_epi8 __builtin_x86_mm_subs_epi8
#define _mm_subs_epu16 __builtin_x86_mm_subs_epu16
#define _mm_subs_epu8 __builtin_x86_mm_subs_epu8
#define _mm_testc_pd __builtin_x86_mm_testc_pd
#define _mm_testc_ps __builtin_x86_mm_testc_ps
#define _mm_testc_si128 __builtin_x86_mm_testc_si128
#define _mm_testz_pd __builtin_x86_mm_testz_pd
#define _mm_testz_ps __builtin_x86_mm_testz_ps
#define _mm_testz_si128 __builtin_x86_mm_testz_si128
#define _mm_undefined_ps __builtin_x86_mm_undefined_ps
#define _mm_undefined_si128 __builtin_x86_mm_undefined_si128
#define _mm_unpackhi_epi16 __builtin_x86_mm_unpackhi_epi16
#define _mm_unpackhi_epi32 __builtin_x86_mm_unpackhi_epi32
#define _mm_unpackhi_epi64 __builtin_x86_mm_unpackhi_epi64
#define _mm_unpackhi_epi8 __builtin_x86_mm_unpackhi_epi8
#define _mm_unpackhi_pd __builtin_x86_mm_unpackhi_pd
#define _mm_unpackhi_ps __builtin_x86_mm_unpackhi_ps
#define _mm_unpacklo_epi16 __builtin_x86_mm_unpacklo_epi16
#define _mm_unpacklo_epi32 __builtin_x86_mm_unpacklo_epi32
#define _mm_unpacklo_epi64 __builtin_x86_mm_unpacklo_epi64
#define _mm_unpacklo_epi8 __builtin_x86_mm_unpacklo_epi8
#define _mm_unpacklo_pd __builtin_x86_mm_unpacklo_pd
#define _mm_unpacklo_ps __builtin_x86_mm_unpacklo_ps
#define _mm_xor_pd __builtin_x86_mm_xor_pd
#define _mm_xor_ps __builtin_x86_mm_xor_ps
#define _mm_xor_si128 __builtin_x86_mm_xor_si128
#define _pdep_u32 __builtin_x86_pdep_u32
#define _pdep_u64 __builtin_x86_pdep_u64
#define _pext_u32 __builtin_x86_pext_u32
#define _pext_u64 __builtin_x86_pext_u64

/* the SSE comparisons by name */
#define _mm_cmpeq_ps(a, b) _mm_cmp_ps(a, b, _CMP_EQ_OQ)
#define _mm_cmplt_ps(a, b) _mm_cmp_ps(a, b, _CMP_LT_OS)
#define _mm_cmple_ps(a, b) _mm_cmp_ps(a, b, _CMP_LE_OS)
#define _mm_cmpgt_ps(a, b) _mm_cmp_ps(a, b, _CMP_GT_OS)
#define _mm_cmpge_ps(a, b) _mm_cmp_ps(a, b, _CMP_GE_OS)
#define _mm_cmpneq_ps(a, b) _mm_cmp_ps(a, b, _CMP_NEQ_UQ)
#define _mm_cmpeq_pd(a, b) _mm_cmp_pd(a, b, _CMP_EQ_OQ)
#define _mm_cmplt_pd(a, b) _mm_cmp_pd(a, b, _CMP_LT_OS)
#define _mm_cmple_pd(a, b) _mm_cmp_pd(a, b, _CMP_LE_OS)
#define _mm_cmpgt_pd(a, b) _mm_cmp_pd(a, b, _CMP_GT_OS)
#define _mm_cmpge_pd(a, b) _mm_cmp_pd(a, b, _CMP_GE_OS)
#define _mm_cmpneq_pd(a, b) _mm_cmp_pd(a, b, _CMP_NEQ_UQ)
#define _mm_cmplt_epi8(a, b) _mm_cmpgt_epi8(b, a)
#define _mm_cmplt_epi16(a, b) _mm_cmpgt_epi16(b, a)
#define _mm_cmplt_epi32(a, b) _mm_cmpgt_epi32(b, a)

#endif
