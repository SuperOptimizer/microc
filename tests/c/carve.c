/* arrays at a variable index carved out of a local record (opt_ssa slot_carve): the record cleared or copied whole
   before the array is first indexed (that access crosses the array: slot_carve died on surfcomp.c), two arrays and
   scalars in one record, a record copied out after the loop, a nested array (left in place) */
#include <stdio.h>
#include <string.h>

typedef struct { unsigned bits; int n; unsigned x[2]; unsigned k; unsigned y[3]; } St;

static unsigned __attribute__((noinline)) mix(unsigned a, unsigned b) { return a * 2654435761u ^ (b >> 3); }

static unsigned run(const unsigned char *p, int len)
{
    St s;
    unsigned h = 0;
    int i;
    memset(&s, 0, sizeof s); /* clears x and y with the scalars, before x[] is seen */
    s.bits = 7;
    for (i = 0; i < len; i++) {
        s.k = p[i] & 1;
        s.x[s.k] += p[i];
        s.y[p[i] % 3] ^= mix(s.x[s.k], s.bits);
        s.bits = s.bits * 33 + p[i];
        s.n++;
    }
    h = s.x[0] ^ s.x[1] * 3 ^ s.y[0] ^ s.y[1] * 5 ^ s.y[2] * 7 ^ s.bits ^ (unsigned)s.n;
    return h;
}

static unsigned copied(const unsigned char *p, int len)
{
    St s, t;
    int i;
    unsigned h;
    memset(&t, 0, sizeof t);
    t.y[2] = 99;
    memcpy(&s, &t, sizeof s); /* a copy of the whole record into it first */
    for (i = 0; i < len; i++)
        s.y[i % 3] += p[i] + s.x[i & 1]++;
    memcpy(&t, &s, sizeof t); /* and out of it after */
    h = t.x[0] + t.x[1] * 3 + t.y[0] * 5 + t.y[1] * 7 + t.y[2] * 11 + t.k;
    return h;
}

static int nested(int a, int b)
{
    struct { int pad; int m[3][4]; int tail; } r;
    int i, j, s = 0;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            r.m[i][j] = i * 10 + j;
    r.pad = 5;
    r.tail = 6;
    for (i = 0; i < 3; i++)
        s += r.m[(a + i) % 3][(b + i) % 4];
    return s + r.pad * 100 + r.tail * 1000;
}

int main(void)
{
    unsigned char buf[64];
    int i;
    for (i = 0; i < 64; i++)
        buf[i] = (unsigned char)(i * 37 + 11);
    printf("%u\n", run(buf, 64));
    printf("%u\n", run(buf, 0));
    printf("%u\n", copied(buf, 64));
    printf("%d\n", nested(1, 2));
    return 0;
}
