#ifndef _WCHAR_H
#define _WCHAR_H
#include <stddef.h>
typedef unsigned int wint_t;
typedef struct { int __s; } mbstate_t;
#define WEOF 0xffffffffU
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
int wcwidth(wchar_t c);
size_t wcslen(const wchar_t *s);
int mbtowc(wchar_t *out, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
#ifdef __x86_64__
#define WCHAR_MIN (-0x7fffffff - 1)
#define WCHAR_MAX 0x7fffffff
#else
#define WCHAR_MIN 0
#define WCHAR_MAX 0xffffffffU
#endif
#endif
