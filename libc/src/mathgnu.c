/* The math library: old and GNU names, apart from the rest so that a program may use them for itself. */
#include "mathimpl.h"

float lgammaf_r(float, int *);
int (isnan)(double x) { return x != x; }
int (isinf)(double x) { return (d2u(x) & ~SIGN64) == INF64; }
int isnanf(float x) { return x != x; }
int isinff(float x) { return (f2u(x) & 0x7fffffff) == 0x7f800000; }
int finite(double x) { return (d2u(x) & ~SIGN64) < INF64; }
int finitef(float x) { return (f2u(x) & 0x7fffffff) < 0x7f800000; }
double drem(double x, double y) { return remainder(x, y); }
float dremf(float x, float y) { return remainderf(x, y); }
double dreml(double x, double y) { return remainder(x, y); }
double gamma(double x) { return lgamma_r(x, &signgam); }
float gammaf(float x) { return lgammaf_r(x, &signgam); }
double gammal(double x) { return lgamma_r(x, &signgam); }
double pow10(double x) { return exp10(x); }
float pow10f(float x) { return exp10f(x); }
double pow10l(double x) { return exp10(x); }
