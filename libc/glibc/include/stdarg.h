/* mcc's <stdarg.h> for use with the GNU C library (which asks for __gnuc_va_list alone with __need___va_list) */
#ifndef __GNUC_VA_LIST
#define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
#endif
#ifdef __need___va_list
#undef __need___va_list
#else
#ifndef _STDARG_H
#define _STDARG_H
#define _ANSI_STDARG_H_
#ifndef _VA_LIST_DEFINED
#define _VA_LIST_DEFINED
#define _VA_LIST
typedef __gnuc_va_list va_list;
#endif
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)
#define __va_copy(d, s) __builtin_va_copy(d, s)
#endif
#endif
