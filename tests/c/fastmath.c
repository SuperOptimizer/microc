#include <stdio.h>
static float fa[1000], fb[1000]; static double da[1000];
static float dot(const float *a, const float *b, int n) { int i; float s = 0; for (i = 0; i < n; i++) s += a[i] * b[i]; return s; }
static double dsum(const double *p, const double *e) { double s = 1.0; while (p != e) s += *p++; return s; }
int main(void) { int i; for (i = 0; i < 1000; i++) { fa[i] = (float)(i % 16); fb[i] = (float)(i % 8); da[i] = i * 0.5; }
  printf("%g %g %g %g\n", (double)dot(fa, fb, 1000), dsum(da, da + 1000), (double)dot(fa, fb, 7), dsum(da, da + 37)); return 0; }
