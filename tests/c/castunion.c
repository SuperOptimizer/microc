/* GNU: cast to a union */
#include <stdio.h>
typedef union {
    unsigned long whole;
    struct {
        unsigned lo, hi;
    } parts;
} Split;
struct P { int a, b; };
union Q { double d; struct P p; int i; };
#define HI(x) (((Split)(x)).parts.hi)
int main(void)
{
    unsigned long v = 0x1234567800abcdefUL;
    struct P p;
    p.a = 3;
    p.b = 4;
    printf("%x %x\n", HI(v), ((Split)v).parts.lo);
    printf("%d %d\n", ((union Q)p).p.b, ((union Q)7).i);
    return 0;
}
