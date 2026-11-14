#include <stdio.h>
#define T(op, a, b, type) do { type r; int o = __builtin_##op##_overflow(a, b, &r); printf(#op " " #type " %d %ld\n", o, (long)r); } while (0)
int main(void)
{
    long big = 0x7fffffffffffffffL;
    unsigned long ubig = 0xffffffffffffffffUL;
    volatile long v3 = 3, vm = -1;
    T(add, 1, 2, int);
    T(add, 0x7fffffff, 1, int);
    T(add, 0x7fffffff, 1, long);
    T(add, big, 1L, long);
    T(add, big, 1L, unsigned long);
    T(add, ubig, 1UL, unsigned long);
    T(add, ubig, vm, unsigned long);
    T(add, -1, -1, unsigned int);
    T(add, 200, 100, unsigned char);
    T(add, 100, 27, signed char);
    T(add, 100, 28, signed char);
    T(sub, 1UL, 2UL, unsigned long);
    T(sub, 1UL, 2UL, long);
    T(sub, -big - 1, 1L, long);
    T(sub, 0, v3, unsigned int);
    T(sub, 5U, 3U, int);
    T(mul, big, 2L, long);
    T(mul, 0x100000000L, 0x100000000L, unsigned long);
    T(mul, 0xffffffffUL, 0xffffffffUL, unsigned long);
    T(mul, ubig, v3, unsigned long);
    T(mul, vm, v3, long);
    T(mul, vm, v3, unsigned long);
    T(mul, -big - 1, vm, long);
    T(mul, 65536, 65536, int);
    T(mul, 65536, 65535, unsigned int);
    T(mul, vm, ubig, long);
    T(mul, 3, 4, _Bool);
    T(mul, 1, 1, _Bool);
    return 0;
}
