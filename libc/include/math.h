#ifndef _MATH_H
#define _MATH_H
extern double __huge_val;
#define HUGE_VAL __huge_val
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double modf(double x, double *ip);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
#endif
