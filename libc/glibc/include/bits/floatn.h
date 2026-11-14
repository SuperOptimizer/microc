/* mcc with the GNU C library: there is no 128-bit floating type */
#ifndef _BITS_FLOATN_H
#define _BITS_FLOATN_H
#include <features.h>
#define __HAVE_FLOAT128 0
#define __HAVE_DISTINCT_FLOAT128 0
#define __HAVE_FLOAT64X 0
#define __HAVE_FLOAT64X_LONG_DOUBLE 0
#include <bits/floatn-common.h>
#endif
