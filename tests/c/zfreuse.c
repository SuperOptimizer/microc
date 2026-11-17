/* a test of a register right after the and / or / xor that wrote it: the flags are used as they stand */
#include <stdio.h>

static int f_or(int a, int b) { int c = a | b; if (c) return c + 1; return -7; }
static int f_and(long a, long b) { long c = a & b; if (c == 0) return 3; return (int)(c >> 1); }
static int f_xor(unsigned a, unsigned b) { unsigned c = a ^ b; return c ? (int)(c & 0xff) : 99; }
static int f_neg(int a, int b) { int c = a | b; if (c < 0) return 1; if (c > 0) return 2; return 0; }
static int f_wide(long a, long b) { long c = a | b; int lo = (int)c; if (lo) return 1; return c ? 2 : 0; }
static int f_cmp(float x, float y, float m)
{
    int inside = 1;
    if (x < 0 || x > m) inside = 0;
    if (y < 0 || y > m) inside = 0;
    return inside;
}
static int f_sel(int a, int b, int x, int y) { return (a ^ b) ? x : y; }

int main(void)
{
    volatile int z = 0;
    printf("%d %d %d\n", f_or(z, 0), f_or(4, 1), f_or(-1, z));
    printf("%d %d %d\n", f_and(12L, 3L), f_and(12L, 6L), f_and(0x300000000L, 0x100000000L));
    printf("%d %d\n", f_xor(5u, 5u), f_xor(0x1234u, 0x1004u));
    printf("%d %d %d\n", f_neg(z, 0), f_neg(-5, 2), f_neg(4, 2));
    printf("%d %d %d\n", f_wide(0x100000000L, z), f_wide(1L, 2L), f_wide(0L, z));
    printf("%d %d %d %d\n", f_cmp(1, 2, 5), f_cmp(-1, 2, 5), f_cmp(1, 6, 5), f_cmp(5, 0, 5));
    printf("%d %d\n", f_sel(3, 3, 10, 20), f_sel(3, 4, 10, 20));
    return 0;
}
