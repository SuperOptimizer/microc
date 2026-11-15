// ok
/* what must still compile: a function template's dependent call resolved at instantiation, a declaration ahead of the call, C
   library calls through their headers */
#include <string.h>
template <class T> int twice(T t) { return dbl(t); }
static int dbl(int x) { return x + x; }
int h(int x) { return twice(x) + (int)strlen("ab"); }
__device__ int dv(int);
__global__ void k(int *p) { p[0] = dv(p[1]); }
__device__ int dv(int x) { return x * 3; }
