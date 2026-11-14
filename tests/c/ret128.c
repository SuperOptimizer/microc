/* 128-bit integers as arguments and results: both halves must arrive, in registers and on the stack,
   through pointers to functions, and from functions the compiler does and does not expand in place. */
#include <stdio.h>

typedef unsigned __int128 u128;
typedef __int128 s128;

static void show(const char *what, u128 v)
{
    printf("%s %016lx%016lx\n", what, (unsigned long)(v >> 64), (unsigned long)v);
}

__attribute__((noinline)) static u128 make(unsigned long hi, unsigned long lo) { return (u128)hi << 64 | lo; }
__attribute__((noinline)) static u128 same(u128 a) { return a; }
__attribute__((noinline)) static s128 neg(s128 a) { return -a; }
__attribute__((noinline)) static u128 mul(unsigned long a, unsigned long b) { return (u128)a * b; }
__attribute__((noinline)) static u128 odd(int x, u128 a, int y, u128 b) { return a + b + (u128)(x + y); }
__attribute__((noinline)) static u128 far(long a, long b, long c, long d, long e, long f, long g, u128 h, u128 i)
{
    return h - i + (u128)(a + b + c + d + e + f + g);
}
static u128 small(u128 a, u128 b) { return a ^ b; }
__attribute__((noinline)) static s128 pick(int c, s128 a, long b)
{
    if (c)
        return a;
    return b;
}
static u128 fact(int n) { return n < 2 ? (u128)1 : (u128)n * fact(n - 1); }

int main(void)
{
    u128 a = make(0x0123456789abcdefUL, 0xfedcba9876543210UL), b;
    u128 (*fp)(u128) = same;
    show("make", a);
    show("same", same(a));
    show("pointer", fp(a + 1));
    show("neg", (u128)neg((s128)a));
    show("mul", mul(0xffffffffffffffffUL, 0xfffffffffffffff1UL));
    show("odd", odd(1, a, 2, make(1, 0xffffffffffffffffUL)));
    show("far", far(1, 2, 3, 4, 5, 6, 7, a, make(0, 1)));
    b = small(a, make(0xffffffffffffffffUL, 0));
    show("small", b);
    show("pick1", (u128)pick(1, -(s128)5, 9));
    show("pick0", (u128)pick(0, -(s128)5, -9));
    show("fact", fact(30));
    show("nested", same(mul(3, 5) + same(make(7, 0))));
    return 0;
}
