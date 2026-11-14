#ifndef _STDDEF_H
#define _STDDEF_H
typedef unsigned long size_t;
typedef long ptrdiff_t;
#ifdef __x86_64__
typedef int wchar_t;
#else
typedef unsigned int wchar_t;
#endif
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
