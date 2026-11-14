/* mcc's <stddef.h> for use with the GNU C library, whose headers ask for single definitions from it by
   defining __need_NAME first. */
#if !defined(__need_size_t) && !defined(__need_wchar_t) && !defined(__need_NULL) && !defined(__need_wint_t) && \
    !defined(__need_ptrdiff_t)
#define __mcc_stddef_all
#endif
#if (defined(__mcc_stddef_all) || defined(__need_size_t)) && !defined(__mcc_size_t)
#define __mcc_size_t
#define __size_t__
#define _SIZE_T
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif
#undef __need_size_t
#if (defined(__mcc_stddef_all) || defined(__need_ptrdiff_t)) && !defined(__mcc_ptrdiff_t)
#define __mcc_ptrdiff_t
#define _PTRDIFF_T
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif
#undef __need_ptrdiff_t
#if (defined(__mcc_stddef_all) || defined(__need_wchar_t)) && !defined(__mcc_wchar_t) && !defined(__cplusplus)
#define __mcc_wchar_t
#define _WCHAR_T
#define __wchar_t__
typedef __WCHAR_TYPE__ wchar_t;
#endif
#undef __need_wchar_t
#if defined(__need_wint_t) && !defined(_WINT_T)
#define _WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#undef __need_wint_t
#if defined(__mcc_stddef_all) || defined(__need_NULL)
#undef NULL
#ifdef __cplusplus
#define NULL 0L
#else
#define NULL ((void *)0)
#endif
#endif
#undef __need_NULL
#ifdef __mcc_stddef_all
#undef __mcc_stddef_all
#ifndef _STDDEF_H
#define _STDDEF_H
#define offsetof(type, member) __builtin_offsetof(type, member)
typedef struct { long long __max_align_ll; long double __max_align_ld; } max_align_t;
#endif
#endif
