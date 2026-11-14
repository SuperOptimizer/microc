/* direct addressing of globals, sign-extending loads, bit tests, loops entered from two places */
#include <stdio.h>
#include <string.h>

static char c1 = 'x';
static short s1 = -2;
static int i1 = -3;
static long l1 = -4;
static double d1 = 1.5;
static float f1 = 2.5f;
static unsigned char bytes[16];
static int tab[8] = { 1, -2, 3, -4, 5, -6, 7, -8 };
static struct { char c; short h; int i; long l; } st = { 1, -2, 3, -4 };
int counter;
long total;
static unsigned char cls[256];

static long widen(int k) { return (long)tab[k & 7] + i1 + s1 + c1; }

/* reads a long at an address the compiler cannot assume is 8-byte aligned */
static long peek(int off)
{
    long v;
    memcpy(&v, bytes + off, sizeof v);
    return v + *(long *)(void *)&tab[2] * 0;
}

static int bits(unsigned long fl, int n)
{
    if (fl & 1)
        n += 1;
    if (!(fl & 0x80))
        n += 10;
    if (fl & 0x100000000UL)
        n += 100;
    if ((fl & 0x8000000000000000UL) == 0)
        n += 1000;
    while (fl & 2) {
        fl >>= 1;
        n++;
    }
    return n;
}

static unsigned scan(const char *p, int skip)
{
    unsigned h = 2166136261u;
    if (skip) {
        p++;
        h ^= 1;
    }
    do {
        h = (h ^ (unsigned char)*p) * 16777619u;
        p++;
    } while (cls[(unsigned char)*p] & 2);
    return h + (unsigned)(cls[(unsigned char)p[-1]] & 1);
}

int main(void)
{
    int k;
    long sum = 0;
    for (k = 0; k < 256; k++)
        cls[k] = (unsigned char)((k >= 'a' && k <= 'z') ? 3 : (k >= '0' && k <= '9') ? 2 : 0);
    for (k = 0; k < 16; k++)
        bytes[k] = (unsigned char)(k * 17 + 1);
    for (k = 0; k < 12; k++) {
        counter += k;
        total += widen(k) * counter;
        st.h = (short)(st.h - k);
        st.l += st.h + st.c;
        st.i ^= tab[k & 7];
        d1 += f1;
        f1 = f1 * 0.5f + 1;
        l1 -= i1;
        sum += peek(k & 7) & 0xffff;
    }
    printf("%d %ld %d %ld %d %g %g %ld %ld\n", counter, total, st.h, st.l, st.i, d1, (double)f1, l1, sum);
    printf("%d %d %d %d\n", bits(0, 0), bits(0x81, 0), bits(0x100000006UL, 0), bits(0x8000000000000003UL, 0));
    printf("%u %u %u\n", scan("hello world", 0), scan("xab12 c", 1), scan("9", 0));
    return 0;
}
