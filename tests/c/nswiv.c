/* an int counter the program itself adds to does not wrap: it is widened once for its use as an index (x86-64);
   counters added to as unsigned, or through a cast, wrap and must keep doing so */
#include <stdio.h>

static float line[64], kk[16];

static float dot(const float *k, const float *l, int i, int r)
{
    float acc = 0;
    int t;
    for (t = 0; t <= 2 * r; t++)
        acc += k[t] * l[i + t];
    return acc;
}

static long down(const int *a, int n, int lo)
{
    long s = 0;
    int i;
    for (i = n; i != lo; i -= 3)
        s += a[i];
    return s;
}

static unsigned long uwrap(const unsigned char *p, unsigned start, unsigned n)
{
    unsigned long s = 0;
    unsigned i, k;
    for (i = start, k = 0; k < n; k++, i += 0x40000000u) /* wraps round 2^32 */
        s = s * 3 + p[(unsigned long)i >> 28] + (unsigned long)i;
    return s;
}

static long castwrap(int start, int n)
{
    long s = 0;
    int i = start, k;
    for (k = 0; k < n; k++) {
        s += (long)i; /* the sign extension of a value that does wrap, by unsigned arithmetic */
        i = (int)((unsigned)i + 0x30000000u);
    }
    return s;
}

static long mixed(const int *a, int n)
{
    long s = 0;
    int i;
    unsigned u = 0xfffffff0u;
    for (i = 0; i <= n; i++, u += 5)
        s += a[i] + (long)(int)u + (long)u;
    return s;
}

int main(void)
{
    static int a[40];
    static unsigned char p[16];
    int k;
    volatile int r = 3, n = 30, lo = 0;
    for (k = 0; k < 64; k++)
        line[k] = (float)(k % 7) - 2;
    for (k = 0; k < 16; k++)
        kk[k] = (float)k * 0.25f, p[k] = (unsigned char)(k * 5 + 1);
    for (k = 0; k < 40; k++)
        a[k] = k * k - 9;
    printf("%g %g\n", dot(kk, line, 5, r), dot(kk, line, 0, 0));
    printf("%ld %ld\n", down(a, n, lo), down(a, 6, 3));
    printf("%lu\n", uwrap(p, 0x90000000u, 9));
    printf("%ld %ld\n", castwrap(0x60000000, 12), castwrap(-5, 3));
    printf("%ld\n", mixed(a, 12));
    return 0;
}
