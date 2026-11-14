/* mcc with the GNU C library: long double is the same type as double here. The library's headers have a
   mode for that (made for machines that once had it so): the long double functions become other names
   for the double ones (sinl is sin, strtold is strtod). Its other half, printf and its like under names
   that take such a long double, does not exist in this library: the bits/...-ldbl.h headers beside this
   one are empty, and "%Lf" must not be used. */
#define __NO_LONG_DOUBLE_MATH 1
#define __LONG_DOUBLE_MATH_OPTIONAL 1
#define __LDOUBLE_REDIRECTS_TO_FLOAT128_ABI 0
