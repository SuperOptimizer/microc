/* Shift-and-mask idioms that become bit-field instructions, two-sided range tests that become one
   comparison, and copies the register allocator joins: checked against plain arithmetic. */
#include <stdio.h>

static unsigned ext(unsigned x, int n) { return n == 0 ? (x >> 5) & 0x3f : n == 1 ? (x >> 31) & 1 : (x >> 1) & 0x7fffffff; }
static unsigned long ext64(unsigned long x) { return ((x >> 40) & 0xffff) + ((x >> 63) & 1) + ((x >> 3) & 0x1fffffffffffffffUL); }
static int sext(int x) { return ((x << 20) >> 24) + ((x << 31) >> 31) + ((x << 4) >> 4); }
static long sext64(long x) { return ((x << 8) >> 60) + ((x << 63) >> 63); }
static unsigned uext(unsigned x) { return ((x << 20) >> 24) + ((x << 7) >> 7) + ((x << 31) >> 31); }
static unsigned ins(unsigned x) { return ((x & 0xff) << 4) + ((x & 1) << 31) + ((x & 0xffff) << 16); }
static unsigned long ins64(unsigned long x) { return ((x & 0xfff) << 52) + ((x & 0x7fffffff) << 1); }
static unsigned chain(unsigned x) { return 0xa8c07bfdu | (((x / 8) & 0x7f) << 15); } /* a field of a field */
static unsigned chain2(unsigned x) { return ((((x << 3) >> 9) & 0xff) << 2) + 0u - ((x >> 4) & 1); }
static unsigned lowbit(unsigned x) { return 0u - (x & 1); }
static long lowbit64(long x) { return -(x & 1); }
static int neg(int x) { return 0 - x; }

static int in_s(int x) { return x >= -5 && x <= 9; }
static int out_s(int x) { return x < 10 || x > 20; }
static int in_s2(int x) { return x > 2147483640 && x <= 2147483647; }
static int in_u(unsigned x) { return x >= 7 && x < 4000000000u; }
static int in_l(long x) { return x >= -0x100000000L && x < 0x100000000L; }
static int out_l(long x) { return x <= -3 || x >= 3; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* variables stepped and copied round a loop */
static long walk(const unsigned char *p, char *q)
{
    long n = 0, m = 0;
    int c, last = 0;
    while ((c = *p++) != 0) {
        if (c == last)
            n++;
        else
            m += n, n = 0;
        last = c;
        *q++ = (char)(c + 1);
    }
    *q = 0;
    return n * 1000 + m + (q - (char *)0 > 0);
}

int main(void)
{
    static const unsigned xs[] = { 0, 1, 2, 0x80000000u, 0xffffffffu, 0x12345678u, 0xdeadbeefu, 0x7fffffffu, 0xfedcba98u };
    static const long ls[] = { 0, 1, -1, 2, -3, 3, 0xffffffffL, 0x100000000L, -0x100000000L, -0x100000001L,
                               0x7fffffffffffffffL, -0x7fffffffffffffffL - 1, 0x123456789abcdef0L };
    static const int is[] = { -2147483647 - 1, -6, -5, 0, 9, 10, 20, 21, 2147483640, 2147483641, 2147483647, 'A', 'Z', '[', '@', 6, 7 };
    char buf[32];
    unsigned k;
    for (k = 0; k < sizeof xs / sizeof xs[0]; k++)
        printf("%u %u %u %d %u %u %u %d %u %u\n", ext(xs[k], 0), ext(xs[k], 1), ext(xs[k], 2), sext((int)xs[k]), uext(xs[k]),
               ins(xs[k]), lowbit(xs[k]), neg((int)xs[k]), chain(xs[k]), chain2(xs[k]));
    for (k = 0; k < sizeof ls / sizeof ls[0]; k++)
        printf("%lu %ld %lu %ld %d %d\n", ext64((unsigned long)ls[k]), sext64(ls[k]), ins64((unsigned long)ls[k]),
               lowbit64(ls[k]), in_l(ls[k]), out_l(ls[k]));
    for (k = 0; k < sizeof is / sizeof is[0]; k++)
        printf("%d%d%d%d %d\n", in_s(is[k]), out_s(is[k]), in_s2(is[k]), in_u((unsigned)is[k]), lower(is[k]));
    printf("%ld %s\n", walk((const unsigned char *)"aabbbcddddde", buf), buf);
    return 0;
}
