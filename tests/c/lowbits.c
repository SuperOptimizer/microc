/* the low n bits of a word, as a bit reader masks them: x & ((1 << n) - 1), and with n == 32 written out
   apart (x86-64 BMI2: bzhi; the select of a constant the other arm comes to anyway goes) */
#include <stdio.h>

typedef unsigned long long u64;
typedef unsigned int u32;

static u32 low(u64 acc, u32 n);
static u32 low32(u32 acc, u32 n);
static u64 keep(u64 acc, u32 n);
static u64 low64(u64 acc, u32 n);
static u32 sel2(u32 x, u32 n);

static u32 (*volatile plow)(u64, u32) = low;
static u32 (*volatile plow32)(u32, u32) = low32;
static u64 (*volatile pkeep)(u64, u32) = keep;
static u64 (*volatile plow64)(u64, u32) = low64;
static u32 (*volatile psel2)(u32, u32) = sel2;

static u32 low(u64 acc, u32 n)
{
    return (u32)(acc & ((n == 32u) ? 0xffffffffull : ((1ull << n) - 1ull)));
}

static u32 low32(u32 acc, u32 n)
{
    return acc & ((1u << n) - 1u);
}

/* not the same value: the select stays */
static u64 keep(u64 acc, u32 n)
{
    return n == 3 ? 7 : acc & 1;
}

static u64 low64(u64 acc, u32 n)
{
    u64 m = (1ull << n) - 1ull;
    return (acc & m) + (m >> 1); /* the mask used twice: no bzhi */
}

static u32 sel2(u32 x, u32 n)
{
    return n != 5 ? (x << n) + n : x * 32 + 5;
}

int main(void)
{
    u32 n;
    u64 a = 0xfedcba9876543210ull, s = 0;
    for (n = 0; n <= 32; n++)
        s = s * 31 + plow(a, n);
    for (n = 0; n < 32; n++)
        s = s * 31 + plow32((u32)(a >> 7), n);
    for (n = 0; n < 64; n++)
        s = s * 31 + plow64(a, n);
    for (n = 0; n < 32; n++)
        s = s * 31 + psel2(0x9e3779b9u, n);
    s += pkeep(a, 3) + pkeep(a, 4) + pkeep(a + 1, 2);
    printf("%lx%08lx\n", (unsigned long)(s >> 32), (unsigned long)(s & 0xffffffffu));
    return 0;
}
