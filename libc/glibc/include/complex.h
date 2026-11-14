/* mcc with the GNU C library: the library's own file; the imaginary unit is not written as a constant here */
#include_next <complex.h>
#undef _Complex_I
#define _Complex_I (__builtin_complex(0.0f, 1.0f))
