/* Runs of steps y = (x >> 1) ^ (K & -(x & 1)) taken together (opt.c lfsr_chain): a crc's 8 steps per byte, 64-bit
   steps, 3 and 5 steps, a different K in each step, a middle value read as well (the run ends there), a step written
   with 0 - bit and with the operands the other way round, 31 steps of a 32-bit value. */
#include <stdio.h>

static unsigned crc32(const unsigned char *p, int n)
{
    unsigned c = 0xffffffffu;
    int i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static unsigned long crc64(const unsigned char *p, int n)
{
    unsigned long c = ~0ul;
    int i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (-(c & 1) & 0xc96c5795d7870f42ul) ^ (c >> 1);
    }
    return c;
}

static unsigned three(unsigned x)
{
    x = (x >> 1) ^ (0x82f63b78u & -(x & 1));
    x = (x >> 1) ^ (0x82f63b78u & -(x & 1));
    x = (x >> 1) ^ (0x82f63b78u & -(x & 1));
    return x;
}

static unsigned mixed(unsigned x)
{
    x = (x >> 1) ^ (0x80000001u & -(x & 1));
    x = (x >> 1) ^ (0xa3000000u & -(x & 1));
    x = (x >> 1) ^ (0x00000003u & -(x & 1));
    x = (x >> 1) ^ (0xffffffffu & -(x & 1));
    x = (x >> 1) ^ (0xd0000001u & -(x & 1));
    return x;
}

static unsigned tapped(unsigned x, unsigned *mid)
{
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    *mid = x;
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    x = (x >> 1) ^ (0xedb88320u & -(x & 1));
    return x;
}

static unsigned many(unsigned x)
{
    int k;
    for (k = 0; k < 31; k++)
        x = (x >> 1) ^ (0xb4bcd35cu & -(x & 1));
    return x;
}

int main(void)
{
    unsigned char buf[97];
    unsigned i, m, s = 0, t = 0, u = 0, w = 0, x = 12345;
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)(i * 131 + 7);
    printf("%08x %08x\n", crc32(buf, 97), crc32((const unsigned char *)"123456789", 9));
    printf("%016lx\n", crc64(buf, 97));
    for (i = 0; i < 1000; i++) {
        x = x * 1664525u + 1013904223u;
        s += three(x);
        t ^= mixed(x);
        u += tapped(x, &m) ^ m;
        w ^= many(x) + i;
    }
    printf("%08x %08x %08x %08x\n", s, t, u, w);
    printf("%08x %08x %08x\n", three(1), mixed(0x1f), many(0x80000001u));
    return 0;
}
