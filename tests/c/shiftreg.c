/* Shifts by a count in a register, of every width and kind (x86-64 with BMI2, -march=native: shlx / shrx / sarx, which
   take any count register and leave the flags alone; otherwise shl / shr / sar by cl). The checks after a shift read
   the flags of a comparison made after it, and a bit reader keeps its count and its value live across the shifts. */
#include <stdio.h>
typedef unsigned long long u64;
typedef long long i64;
__attribute__((noinline)) u64 f1(u64 a, unsigned n) { return a << n; }
__attribute__((noinline)) u64 f2(u64 a, unsigned n) { return a >> n; }
__attribute__((noinline)) i64 f3(i64 a, unsigned n) { return a >> n; }
__attribute__((noinline)) unsigned f4(unsigned a, unsigned n) { return a << n; }
__attribute__((noinline)) unsigned f5(unsigned a, unsigned n) { return a >> n; }
__attribute__((noinline)) int f6(int a, unsigned n) { return a >> n; }
__attribute__((noinline)) u64 f7(u64 a, unsigned char n) { return (a << n) | (a >> (64 - n)); }
__attribute__((noinline)) unsigned f8(const unsigned *p, unsigned n)
{
    unsigned s = 0, i;
    for (i = 0; i < n; i++)
        s += (p[i] << (i & 31)) ^ (p[i] >> (n & 15));
    return s;
}
/* the count is also an operand of the shift (n << n) and the result goes where the count was */
__attribute__((noinline)) u64 f9(u64 n) { n &= 63; n = n << n; return n; }
__attribute__((noinline)) int f10(u64 a, unsigned n) { return (a >> n) > 5 && n > 3; }
static unsigned get(u64 *acc, unsigned *nb, const unsigned char *b, unsigned *pos, unsigned n)
{
    unsigned v;
    while (*nb < n) {
        *acc |= (u64)b[(*pos)++] << *nb;
        *nb += 8;
    }
    v = (unsigned)(*acc & ((1ull << n) - 1));
    *acc >>= n;
    *nb -= n;
    return v;
}
int main(void)
{
    u64 h = 1469598103934665603ull, x = 0x9e3779b97f4a7c15ull, acc = 0;
    unsigned arr[100], nb = 0, pos = 0, k, s = 0;
    unsigned char buf[4096];
    int i;
    for (i = 0; i < 100; i++)
        arr[i] = (unsigned)(x * (u64)(i + 1) >> 17);
    for (i = 0; i < 4096; i++)
        buf[i] = (unsigned char)(x * (u64)(i + 3) >> 29);
    for (i = 0; i < 100000; i++) {
        unsigned n;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        n = (unsigned)(x >> 58);
        h = (h ^ f1(x, n)) * 1099511628211ull;
        h = (h ^ f2(x, n)) * 1099511628211ull;
        h = (h ^ (u64)f3((i64)x, n)) * 1099511628211ull;
        h = (h ^ f4((unsigned)x, n & 31)) * 1099511628211ull;
        h = (h ^ f5((unsigned)x, n & 31)) * 1099511628211ull;
        h = (h ^ (unsigned)f6((int)x, n & 31)) * 1099511628211ull;
        h = (h ^ f7(x, (unsigned char)(n | 1))) * 1099511628211ull;
        h = (h ^ f9(x)) * 1099511628211ull;
        h += (u64)f10(x, n);
    }
    for (k = 0; k < 2000; k++)
        s = s * 31 + get(&acc, &nb, buf, &pos, 1 + k % 13);
    printf("%016llx %08x %08x\n", h, f8(arr, 100), s);
    return 0;
}
