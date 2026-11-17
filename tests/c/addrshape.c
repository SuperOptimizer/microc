/* Addresses that are sums with constants in several places (an element of an array of records picked by "base +
   small value", then a field, then an index) are put together again before selection. A 32-bit index sum that
   may wrap must wrap as written; sums whose inner values are used elsewhere keep their values. */
#include <stdio.h>
#include <stddef.h>

typedef struct {
    unsigned short ns;
    unsigned char sym, nb;
} E;
typedef struct {
    unsigned short freq[65], cum[64];
    E dt[64];
} M;

static M models[12];
static unsigned char bytes[600];
static long words[80];

__attribute__((noinline)) static unsigned small(const M *m, unsigned x, unsigned i)
{
    return m[(x & 3u) + 5u].dt[i].nb;
}

/* x is any 32-bit value: x + 5 wraps for the last five */
__attribute__((noinline)) static unsigned wraps(const unsigned char *p, unsigned x)
{
    return p[x + 5u];
}

__attribute__((noinline)) static unsigned element(const M *m, unsigned x, unsigned i)
{
    const E *e = &m[(x & 3u) + 2u].dt[i];
    return e->nb * 65536u + e->sym * 256u + e->ns;
}

__attribute__((noinline)) static long stencil(const long *a, size_t i)
{
    return a[i - 1] + 2 * a[i] + a[i + 1] + a[i + 9];
}

__attribute__((noinline)) static void put(M *m, unsigned x, unsigned i, unsigned v)
{
    m[(x & 1u) + 9u].dt[i + 1u].sym = (unsigned char)v;
    m[(x & 1u) + 9u].dt[i + 1u].ns = (unsigned short)(v * 3u);
}

/* the inner sums have other uses */
__attribute__((noinline)) static unsigned long shared(const unsigned char *p, unsigned long i, unsigned long *out)
{
    unsigned long j = i + 7, k = (j + 3) * 5;
    out[0] = j;
    out[1] = k;
    return p[k + 2] + p[j + 1];
}

__attribute__((noinline)) static int negative(const int *a, long i)
{
    return a[(i - 3) * 2 + 1] - a[i - 4];
}

int main(void)
{
    unsigned i, s = 0;
    unsigned long o[2], r;
    static int ints[64];
    for (i = 0; i < 12; i++) {
        unsigned k;
        for (k = 0; k < 64; k++) {
            models[i].dt[k].ns = (unsigned short)(i * 1000u + k);
            models[i].dt[k].sym = (unsigned char)(i * 16u + k);
            models[i].dt[k].nb = (unsigned char)(i + k * 3u);
        }
    }
    for (i = 0; i < 600; i++)
        bytes[i] = (unsigned char)(i * 7u + 1u);
    for (i = 0; i < 80; i++)
        words[i] = (long)i * i - 40;
    for (i = 0; i < 64; i++)
        ints[i] = (int)(i * 5u) - 100;
    for (i = 0; i < 64; i++)
        s = s * 31u + small(models, i * 7u, i) + element(models, i, 63u - i);
    printf("%u %u %u\n", s, small(models, 0xffffffffu, 63), element(models, 0xfffffffeu, 0));
    printf("%u %u %u %u\n", wraps(bytes, 0), wraps(bytes, 500), wraps(bytes + 5, 0xfffffffbu), wraps(bytes + 5, 0xffffffffu));
    printf("%ld %ld\n", stencil(words, 1), stencil(words, 60));
    put(models, 0, 5, 77);
    put(models, 1, 62, 200);
    printf("%u %u %u %u\n", models[9].dt[6].sym, models[9].dt[6].ns, models[10].dt[63].sym, models[10].dt[63].ns);
    r = shared(bytes, 20, o);
    printf("%lu %lu %lu\n", r, o[0], o[1]);
    printf("%d %d\n", negative(ints, 4), negative(ints, 30));
    return 0;
}
