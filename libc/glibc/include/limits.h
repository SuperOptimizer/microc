/* mcc's <limits.h> for use with the GNU C library: the compiler's numbers, then the library's header */
#ifndef _GCC_LIMITS_H_
#define _GCC_LIMITS_H_
#include_next <limits.h>
#undef CHAR_BIT
#define CHAR_BIT 8
#undef SCHAR_MIN
#define SCHAR_MIN (-128)
#undef SCHAR_MAX
#define SCHAR_MAX 127
#undef UCHAR_MAX
#define UCHAR_MAX 255
#undef CHAR_MIN
#undef CHAR_MAX
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif
#undef SHRT_MIN
#define SHRT_MIN (-32768)
#undef SHRT_MAX
#define SHRT_MAX 32767
#undef USHRT_MAX
#define USHRT_MAX 65535
#undef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#undef INT_MAX
#define INT_MAX 2147483647
#undef UINT_MAX
#define UINT_MAX 4294967295U
#undef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#undef LONG_MAX
#define LONG_MAX 9223372036854775807L
#undef ULONG_MAX
#define ULONG_MAX 18446744073709551615UL
#undef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#undef LLONG_MAX
#define LLONG_MAX 9223372036854775807LL
#undef ULLONG_MAX
#define ULLONG_MAX 18446744073709551615ULL
#undef LONG_LONG_MIN
#define LONG_LONG_MIN LLONG_MIN
#undef LONG_LONG_MAX
#define LONG_LONG_MAX LLONG_MAX
#undef ULONG_LONG_MAX
#define ULONG_LONG_MAX ULLONG_MAX
#undef MB_LEN_MAX
#define MB_LEN_MAX 16
#endif
