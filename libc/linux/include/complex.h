#ifndef _COMPLEX_H
#define _COMPLEX_H
#define complex _Complex
#define _Complex_I (__builtin_complex(0.0f, 1.0f))
#define I _Complex_I
#define CMPLX(x, y) __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
double creal(double _Complex);
double cimag(double _Complex);
double cabs(double _Complex);
double carg(double _Complex);
double _Complex conj(double _Complex);
double _Complex cexp(double _Complex);
double _Complex clog(double _Complex);
double _Complex csqrt(double _Complex);
double _Complex cpow(double _Complex, double _Complex);
double _Complex cproj(double _Complex);
float crealf(float _Complex);
float cimagf(float _Complex);
float cabsf(float _Complex);
float cargf(float _Complex);
float _Complex conjf(float _Complex);
float _Complex cexpf(float _Complex);
#endif
