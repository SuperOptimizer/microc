/* Jump threading: a block whose branch its predecessors decide. A comparison of 64-bit values is decided in 64
   bits (constants that differ only above bit 31), and a predecessor that brings a value of known range (a byte
   loaded unsigned) decides a test against a constant outside that range, as one that brings a constant does;
   a test inside the range stays. */
#include <stdio.h>

typedef struct {
    const unsigned char *buf;
    unsigned long n, pos;
    unsigned acc, nbits;
} Rd;

static inline int get(Rd *r, const unsigned char *tab)
{
    if (r->pos >= r->n)
        return -1;
    r->acc += r->buf[r->pos];
    return tab[r->buf[r->pos++]];
}

static inline int get16(Rd *r, const unsigned short *tab)
{
    if (r->pos >= r->n)
        return -1;
    return tab[r->buf[r->pos++]];
}

/* the decoder's shape: -1 or a byte, tested with < 0 and then with a bound */
__attribute__((noinline)) static long decode(const unsigned char *in, unsigned long n, const unsigned char *tab, unsigned char *out)
{
    Rd r;
    unsigned long i;
    r.buf = in;
    r.n = n;
    r.pos = 0;
    r.acc = 0;
    r.nbits = 0;
    for (i = 0; i < 64; i++) {
        int t = get(&r, tab);
        if (t < 0 || (unsigned)t > 19u)
            return -(long)i - 1;
        out[i] = (unsigned char)(t < 16 ? t : t - 12);
    }
    return (long)r.acc;
}

/* tests inside the value's range: not decided */
__attribute__((noinline)) static int inside(const unsigned char *in, unsigned long n, const unsigned char *tab)
{
    Rd r;
    int s = 0, i;
    r.buf = in;
    r.n = n;
    r.pos = 0;
    r.acc = 0;
    r.nbits = 0;
    for (i = 0; i < 8; i++) {
        int t = get(&r, tab);
        if (t < 100)
            s += 1;
        if (t <= 254)
            s += 10;
        if (t > 0)
            s += 100;
        if (t >= 255)
            s += 1000;
        if (t == 255)
            s += 10000;
        if (t != 7)
            s += 100000;
    }
    return s;
}

/* tests at and beyond the range's ends, signed and unsigned */
__attribute__((noinline)) static int outside(const unsigned char *in, unsigned long n, const unsigned short *tab)
{
    Rd r;
    int s = 0, i;
    r.buf = in;
    r.n = n;
    r.pos = 0;
    r.acc = 0;
    r.nbits = 0;
    for (i = 0; i < 6; i++) {
        int t = get16(&r, tab);
        if (t <= -1)
            s += 1;
        if (t > -1)
            s += 2;
        if (t >= 0)
            s += 4;
        if (t <= 65535)
            s += 8;
        if (t > 65535)
            s += 16;
        if (t == 65536)
            s += 32;
        if (t != -1)
            s += 64;
        if ((unsigned)t < 65536u)
            s += 128;
        if ((unsigned)t >= 65536u)
            s += 256;
        if ((unsigned)t > 65535u)
            s += 512;
        if ((unsigned)t <= 65535u)
            s += 1024;
        if ((unsigned)t < 0u + (unsigned)(i & 0))
            s += 2048;
        s *= 3;
        s &= 0xfffff;
    }
    return s;
}

/* 64-bit values that differ from the tested constant only above bit 31 */
__attribute__((noinline)) static int wide1(int c)
{
    unsigned long x;
    if (c)
        x = 0x100000000UL;
    else
        x = 0;
    if (x != 0)
        return 5;
    return 7;
}

__attribute__((noinline)) static int wide2(int c)
{
    long x;
    if (c)
        x = 0x100000001L;
    else
        x = 2;
    if (x < 2)
        return 5;
    return 7;
}

__attribute__((noinline)) static int wide3(int c)
{
    unsigned long x;
    if (c)
        x = 0x100000001UL;
    else
        x = 3;
    if (x > 2)
        return 5;
    return 7;
}

__attribute__((noinline)) static int wide4(int c)
{
    long x;
    if (c == 1)
        x = -0x100000000L;
    else if (c == 2)
        x = 0x7fffffffL;
    else
        x = 0x80000000L;
    if (x < 0)
        return 1;
    if (x == 0x7fffffffL)
        return 2;
    return 3;
}

int main(void)
{
    unsigned char in[80], tab[256], out[64];
    unsigned short tab16[256];
    unsigned i;
    unsigned long h = 0;
    for (i = 0; i < 256; i++) {
        tab[i] = (unsigned char)(i % 20);
        tab16[i] = (unsigned short)(i * 257);
    }
    for (i = 0; i < 80; i++)
        in[i] = (unsigned char)(i * 37 + 11);
    printf("%ld %ld", decode(in, 80, tab, out), decode(in, 40, tab, out));
    for (i = 0; i < 64; i++)
        h = h * 31 + out[i];
    printf(" %lu\n", h);
    tab[in[5]] = 20;
    printf("%ld\n", decode(in, 80, tab, out));
    tab[in[0]] = 255;
    tab[in[1]] = 254;
    tab[in[2]] = 0;
    tab[in[3]] = 7;
    tab[in[4]] = 100;
    tab[in[5]] = 99;
    printf("%d %d %d\n", inside(in, 80, tab), inside(in, 6, tab), inside(in, 0, tab));
    tab16[in[0]] = 65535;
    tab16[in[1]] = 0;
    printf("%d %d %d\n", outside(in, 80, tab16), outside(in, 3, tab16), outside(in, 0, tab16));
    printf("%d %d %d %d %d %d %d %d %d\n", wide1(0), wide1(1), wide2(0), wide2(1), wide3(0), wide3(1), wide4(1), wide4(2), wide4(3));
    return 0;
}
