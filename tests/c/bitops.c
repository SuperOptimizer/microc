/* rotates, and-not/or-not/xor-not, and right-shifted operands */
#include <stdio.h>

static unsigned rotr(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }

unsigned f1(unsigned x) { return (x >> 7) | (x << 25); }
unsigned f2(unsigned x, unsigned y) { return y ^ ((x >> 11) | (x << 21)) ^ ((x << 7) | (x >> 25)); }
unsigned long f3(unsigned long x) { return (x << 13) | (x >> 51); }
unsigned f4(unsigned a, unsigned b) { return a & ~b; }
unsigned f5(unsigned a, unsigned b) { return ~b | a; }
unsigned long f6(unsigned long a, unsigned long b) { return a ^ ~b; }
unsigned f7(unsigned a, unsigned b) { return a + (b >> 3); }
int f8(int a, int b) { return a - (b >> 5); }
unsigned f9(unsigned a, unsigned b) { return (b >> 9) & a; }
long f10(long a, long b) { return a | (b >> 60); }
unsigned f11(unsigned x) { unsigned r = (x >> 3) | (x << 29); return r + (r >> 1); }
unsigned f12(unsigned x, unsigned y) { return ((x >> 5) | (y << 27)) ^ y; } /* not a rotate */
unsigned f13(unsigned x) { return (x >> 4) | (x << 27); }                   /* not a rotate */

int main(void)
{
    unsigned a = 0x12345678u, b = 0x9abcdef1u;
    unsigned long la = 0x0123456789abcdefUL, lb = 0xfedcba9876543210UL;
    int i;
    unsigned h = 0;
    printf("%08x %08x %016lx\n", f1(a), f2(a, b), f3(la));
    printf("%08x %08x %016lx\n", f4(a, b), f5(a, b), f6(la, lb));
    printf("%08x %d %08x %016lx\n", f7(a, b), f8(-1000, -77777), f9(a, b), (unsigned long)f10((long)la, (long)lb));
    printf("%08x %08x %08x\n", f11(b), f12(a, b), f13(b));
    for (i = 1; i < 32; i++)
        h = rotr(h ^ a, i) + (b & ~h);
    printf("%08x\n", h);
    return 0;
}
