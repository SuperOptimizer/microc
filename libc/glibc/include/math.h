/* mcc with the GNU C library: the library's own file. In C++ (CUDA) the tests of a number's class are
   written out, so that code for a GPU, where the library's functions do not exist, has them too. */
#include_next <math.h>
#if defined __cplusplus && !defined _MCC_MATH_H
#define _MCC_MATH_H
#undef isnan
#undef isfinite
#undef isinf
#define isnan(x) __extension__({ __typeof__(x) __x = (x); __x != __x; })
#define isfinite(x) __extension__({ __typeof__(x) __x = (x); __x - __x == 0; })
#define isinf(x) __extension__({ __typeof__(x) __x = (x); __x == __x && __x - __x != 0; })
#endif
