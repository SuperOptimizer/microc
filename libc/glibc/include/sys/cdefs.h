/* mcc with the GNU C library: the library's own file, less what it offers only to a full C++ compiler */
#include_next <sys/cdefs.h>
#ifndef _MCC_SYS_CDEFS_H
#define _MCC_SYS_CDEFS_H
#ifdef __cplusplus
/* To C++ the headers are those written for GCC 4.3: strchr and its like are then declared once, as in C,
   not as pairs of overloaded functions (string.h, strings.h, wchar.h, stdlib.h) */
#undef __GNUC_PREREQ
#define __GNUC_PREREQ(maj, min) ((maj) < 4 || ((maj) == 4 && (min) <= 3))
#endif
#endif
