#include <stdio.h>
/* division and remainder by constants: every divisor against edge values */
static const long vals[] = {0, 1, 2, 3, 5, 6, 7, 9, 10, 11, 99, 100, 101, 255, 256, 999, 1000, 65535, 65536, 65537,
    1000000, 16777215, 16777216, 2147483646, 2147483647, 2147483648, 2147483649, 4294967294, 4294967295, 4294967296,
    4294967297, 123456789012, 999999999999999999, 1000000000000000000, 4611686018427387903, 4611686018427387904,
    9223372036854775806, 9223372036854775807, 6148914691236517205, 3074457345618258603, 1234567890123456789};
#define NV (sizeof vals / sizeof vals[0])
static unsigned long h;
static void mix(unsigned long x) { h = (h ^ x) * 1099511628211UL + 7; }
static void s32_3(int x) { mix((unsigned long)(long)(x / 3)); mix((unsigned long)(long)(x % 3)); }
static void u32_3(unsigned x) { mix(x / 3u); mix(x % 3u); }
static void s32_5(int x) { mix((unsigned long)(long)(x / 5)); mix((unsigned long)(long)(x % 5)); }
static void u32_5(unsigned x) { mix(x / 5u); mix(x % 5u); }
static void s32_6(int x) { mix((unsigned long)(long)(x / 6)); mix((unsigned long)(long)(x % 6)); }
static void u32_6(unsigned x) { mix(x / 6u); mix(x % 6u); }
static void s32_7(int x) { mix((unsigned long)(long)(x / 7)); mix((unsigned long)(long)(x % 7)); }
static void u32_7(unsigned x) { mix(x / 7u); mix(x % 7u); }
static void s32_9(int x) { mix((unsigned long)(long)(x / 9)); mix((unsigned long)(long)(x % 9)); }
static void u32_9(unsigned x) { mix(x / 9u); mix(x % 9u); }
static void s32_10(int x) { mix((unsigned long)(long)(x / 10)); mix((unsigned long)(long)(x % 10)); }
static void u32_10(unsigned x) { mix(x / 10u); mix(x % 10u); }
static void s32_11(int x) { mix((unsigned long)(long)(x / 11)); mix((unsigned long)(long)(x % 11)); }
static void u32_11(unsigned x) { mix(x / 11u); mix(x % 11u); }
static void s32_12(int x) { mix((unsigned long)(long)(x / 12)); mix((unsigned long)(long)(x % 12)); }
static void u32_12(unsigned x) { mix(x / 12u); mix(x % 12u); }
static void s32_13(int x) { mix((unsigned long)(long)(x / 13)); mix((unsigned long)(long)(x % 13)); }
static void u32_13(unsigned x) { mix(x / 13u); mix(x % 13u); }
static void s32_14(int x) { mix((unsigned long)(long)(x / 14)); mix((unsigned long)(long)(x % 14)); }
static void u32_14(unsigned x) { mix(x / 14u); mix(x % 14u); }
static void s32_25(int x) { mix((unsigned long)(long)(x / 25)); mix((unsigned long)(long)(x % 25)); }
static void u32_25(unsigned x) { mix(x / 25u); mix(x % 25u); }
static void s32_60(int x) { mix((unsigned long)(long)(x / 60)); mix((unsigned long)(long)(x % 60)); }
static void u32_60(unsigned x) { mix(x / 60u); mix(x % 60u); }
static void s32_100(int x) { mix((unsigned long)(long)(x / 100)); mix((unsigned long)(long)(x % 100)); }
static void u32_100(unsigned x) { mix(x / 100u); mix(x % 100u); }
static void s32_125(int x) { mix((unsigned long)(long)(x / 125)); mix((unsigned long)(long)(x % 125)); }
static void u32_125(unsigned x) { mix(x / 125u); mix(x % 125u); }
static void s32_641(int x) { mix((unsigned long)(long)(x / 641)); mix((unsigned long)(long)(x % 641)); }
static void u32_641(unsigned x) { mix(x / 641u); mix(x % 641u); }
static void s32_1000(int x) { mix((unsigned long)(long)(x / 1000)); mix((unsigned long)(long)(x % 1000)); }
static void u32_1000(unsigned x) { mix(x / 1000u); mix(x % 1000u); }
static void s32_1025(int x) { mix((unsigned long)(long)(x / 1025)); mix((unsigned long)(long)(x % 1025)); }
static void u32_1025(unsigned x) { mix(x / 1025u); mix(x % 1025u); }
static void s32_3600(int x) { mix((unsigned long)(long)(x / 3600)); mix((unsigned long)(long)(x % 3600)); }
static void u32_3600(unsigned x) { mix(x / 3600u); mix(x % 3600u); }
static void s32_65535(int x) { mix((unsigned long)(long)(x / 65535)); mix((unsigned long)(long)(x % 65535)); }
static void u32_65535(unsigned x) { mix(x / 65535u); mix(x % 65535u); }
static void s32_65537(int x) { mix((unsigned long)(long)(x / 65537)); mix((unsigned long)(long)(x % 65537)); }
static void u32_65537(unsigned x) { mix(x / 65537u); mix(x % 65537u); }
static void s32_1000000(int x) { mix((unsigned long)(long)(x / 1000000)); mix((unsigned long)(long)(x % 1000000)); }
static void u32_1000000(unsigned x) { mix(x / 1000000u); mix(x % 1000000u); }
static void s32_16777215(int x) { mix((unsigned long)(long)(x / 16777215)); mix((unsigned long)(long)(x % 16777215)); }
static void u32_16777215(unsigned x) { mix(x / 16777215u); mix(x % 16777215u); }
static void s32_2147483647(int x) { mix((unsigned long)(long)(x / 2147483647)); mix((unsigned long)(long)(x % 2147483647)); }
static void u32_2147483647(unsigned x) { mix(x / 2147483647u); mix(x % 2147483647u); }
static void s32_1073741825(int x) { mix((unsigned long)(long)(x / 1073741825)); mix((unsigned long)(long)(x % 1073741825)); }
static void u32_1073741825(unsigned x) { mix(x / 1073741825u); mix(x % 1073741825u); }
static void s32_1431655765(int x) { mix((unsigned long)(long)(x / 1431655765)); mix((unsigned long)(long)(x % 1431655765)); }
static void u32_1431655765(unsigned x) { mix(x / 1431655765u); mix(x % 1431655765u); }
static void s64_3(long x) { mix((unsigned long)(x / 3L)); mix((unsigned long)(x % 3L)); }
static void u64_3(unsigned long x) { mix(x / 3UL); mix(x % 3UL); }
static void s64_5(long x) { mix((unsigned long)(x / 5L)); mix((unsigned long)(x % 5L)); }
static void u64_5(unsigned long x) { mix(x / 5UL); mix(x % 5UL); }
static void s64_6(long x) { mix((unsigned long)(x / 6L)); mix((unsigned long)(x % 6L)); }
static void u64_6(unsigned long x) { mix(x / 6UL); mix(x % 6UL); }
static void s64_7(long x) { mix((unsigned long)(x / 7L)); mix((unsigned long)(x % 7L)); }
static void u64_7(unsigned long x) { mix(x / 7UL); mix(x % 7UL); }
static void s64_9(long x) { mix((unsigned long)(x / 9L)); mix((unsigned long)(x % 9L)); }
static void u64_9(unsigned long x) { mix(x / 9UL); mix(x % 9UL); }
static void s64_10(long x) { mix((unsigned long)(x / 10L)); mix((unsigned long)(x % 10L)); }
static void u64_10(unsigned long x) { mix(x / 10UL); mix(x % 10UL); }
static void s64_11(long x) { mix((unsigned long)(x / 11L)); mix((unsigned long)(x % 11L)); }
static void u64_11(unsigned long x) { mix(x / 11UL); mix(x % 11UL); }
static void s64_12(long x) { mix((unsigned long)(x / 12L)); mix((unsigned long)(x % 12L)); }
static void u64_12(unsigned long x) { mix(x / 12UL); mix(x % 12UL); }
static void s64_13(long x) { mix((unsigned long)(x / 13L)); mix((unsigned long)(x % 13L)); }
static void u64_13(unsigned long x) { mix(x / 13UL); mix(x % 13UL); }
static void s64_14(long x) { mix((unsigned long)(x / 14L)); mix((unsigned long)(x % 14L)); }
static void u64_14(unsigned long x) { mix(x / 14UL); mix(x % 14UL); }
static void s64_25(long x) { mix((unsigned long)(x / 25L)); mix((unsigned long)(x % 25L)); }
static void u64_25(unsigned long x) { mix(x / 25UL); mix(x % 25UL); }
static void s64_60(long x) { mix((unsigned long)(x / 60L)); mix((unsigned long)(x % 60L)); }
static void u64_60(unsigned long x) { mix(x / 60UL); mix(x % 60UL); }
static void s64_100(long x) { mix((unsigned long)(x / 100L)); mix((unsigned long)(x % 100L)); }
static void u64_100(unsigned long x) { mix(x / 100UL); mix(x % 100UL); }
static void s64_125(long x) { mix((unsigned long)(x / 125L)); mix((unsigned long)(x % 125L)); }
static void u64_125(unsigned long x) { mix(x / 125UL); mix(x % 125UL); }
static void s64_641(long x) { mix((unsigned long)(x / 641L)); mix((unsigned long)(x % 641L)); }
static void u64_641(unsigned long x) { mix(x / 641UL); mix(x % 641UL); }
static void s64_1000(long x) { mix((unsigned long)(x / 1000L)); mix((unsigned long)(x % 1000L)); }
static void u64_1000(unsigned long x) { mix(x / 1000UL); mix(x % 1000UL); }
static void s64_1025(long x) { mix((unsigned long)(x / 1025L)); mix((unsigned long)(x % 1025L)); }
static void u64_1025(unsigned long x) { mix(x / 1025UL); mix(x % 1025UL); }
static void s64_3600(long x) { mix((unsigned long)(x / 3600L)); mix((unsigned long)(x % 3600L)); }
static void u64_3600(unsigned long x) { mix(x / 3600UL); mix(x % 3600UL); }
static void s64_65535(long x) { mix((unsigned long)(x / 65535L)); mix((unsigned long)(x % 65535L)); }
static void u64_65535(unsigned long x) { mix(x / 65535UL); mix(x % 65535UL); }
static void s64_65537(long x) { mix((unsigned long)(x / 65537L)); mix((unsigned long)(x % 65537L)); }
static void u64_65537(unsigned long x) { mix(x / 65537UL); mix(x % 65537UL); }
static void s64_1000000(long x) { mix((unsigned long)(x / 1000000L)); mix((unsigned long)(x % 1000000L)); }
static void u64_1000000(unsigned long x) { mix(x / 1000000UL); mix(x % 1000000UL); }
static void s64_16777215(long x) { mix((unsigned long)(x / 16777215L)); mix((unsigned long)(x % 16777215L)); }
static void u64_16777215(unsigned long x) { mix(x / 16777215UL); mix(x % 16777215UL); }
static void s64_2147483647(long x) { mix((unsigned long)(x / 2147483647L)); mix((unsigned long)(x % 2147483647L)); }
static void u64_2147483647(unsigned long x) { mix(x / 2147483647UL); mix(x % 2147483647UL); }
static void s64_1073741825(long x) { mix((unsigned long)(x / 1073741825L)); mix((unsigned long)(x % 1073741825L)); }
static void u64_1073741825(unsigned long x) { mix(x / 1073741825UL); mix(x % 1073741825UL); }
static void s64_1431655765(long x) { mix((unsigned long)(x / 1431655765L)); mix((unsigned long)(x % 1431655765L)); }
static void u64_1431655765(unsigned long x) { mix(x / 1431655765UL); mix(x % 1431655765UL); }
static void s64_4294967295(long x) { mix((unsigned long)(x / 4294967295L)); mix((unsigned long)(x % 4294967295L)); }
static void u64_4294967295(unsigned long x) { mix(x / 4294967295UL); mix(x % 4294967295UL); }
static void s64_4294967297(long x) { mix((unsigned long)(x / 4294967297L)); mix((unsigned long)(x % 4294967297L)); }
static void u64_4294967297(unsigned long x) { mix(x / 4294967297UL); mix(x % 4294967297UL); }
static void s64_1000000000000(long x) { mix((unsigned long)(x / 1000000000000L)); mix((unsigned long)(x % 1000000000000L)); }
static void u64_1000000000000(unsigned long x) { mix(x / 1000000000000UL); mix(x % 1000000000000UL); }
static void s64_1000000000000000000(long x) { mix((unsigned long)(x / 1000000000000000000L)); mix((unsigned long)(x % 1000000000000000000L)); }
static void u64_1000000000000000000(unsigned long x) { mix(x / 1000000000000000000UL); mix(x % 1000000000000000000UL); }
static void s64_4611686018427387903(long x) { mix((unsigned long)(x / 4611686018427387903L)); mix((unsigned long)(x % 4611686018427387903L)); }
static void u64_4611686018427387903(unsigned long x) { mix(x / 4611686018427387903UL); mix(x % 4611686018427387903UL); }
static void s64_3074457345618258603(long x) { mix((unsigned long)(x / 3074457345618258603L)); mix((unsigned long)(x % 3074457345618258603L)); }
static void u64_3074457345618258603(unsigned long x) { mix(x / 3074457345618258603UL); mix(x % 3074457345618258603UL); }
static void s64_6700417(long x) { mix((unsigned long)(x / 6700417L)); mix((unsigned long)(x % 6700417L)); }
static void u64_6700417(unsigned long x) { mix(x / 6700417UL); mix(x % 6700417UL); }

struct odd { char c[24]; };
struct odd3 { short a, b, c; };
static long diff(struct odd *a, struct odd *b) { return a - b; }
static long diff3(struct odd3 *a, struct odd3 *b) { return a - b; }

static unsigned long rnd(unsigned long *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

int main(void)
{
    static struct odd o[50];
    static struct odd3 p[50];
    unsigned long k, n, seed = 88172645463325252UL;
    long v;
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_3((int)v);
        s32_3((int)-v);
        s32_3((int)(v + 1));
        s32_3((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_3((int)k);
        s32_3((int)(k >> (k & 63)));
    }
    printf("s32_3 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_3((unsigned)v);
        u32_3((unsigned)-v);
        u32_3((unsigned)(v + 1));
        u32_3((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_3((unsigned)k);
        u32_3((unsigned)(k >> (k & 63)));
    }
    printf("u32_3 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_5((int)v);
        s32_5((int)-v);
        s32_5((int)(v + 1));
        s32_5((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_5((int)k);
        s32_5((int)(k >> (k & 63)));
    }
    printf("s32_5 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_5((unsigned)v);
        u32_5((unsigned)-v);
        u32_5((unsigned)(v + 1));
        u32_5((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_5((unsigned)k);
        u32_5((unsigned)(k >> (k & 63)));
    }
    printf("u32_5 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_6((int)v);
        s32_6((int)-v);
        s32_6((int)(v + 1));
        s32_6((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_6((int)k);
        s32_6((int)(k >> (k & 63)));
    }
    printf("s32_6 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_6((unsigned)v);
        u32_6((unsigned)-v);
        u32_6((unsigned)(v + 1));
        u32_6((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_6((unsigned)k);
        u32_6((unsigned)(k >> (k & 63)));
    }
    printf("u32_6 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_7((int)v);
        s32_7((int)-v);
        s32_7((int)(v + 1));
        s32_7((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_7((int)k);
        s32_7((int)(k >> (k & 63)));
    }
    printf("s32_7 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_7((unsigned)v);
        u32_7((unsigned)-v);
        u32_7((unsigned)(v + 1));
        u32_7((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_7((unsigned)k);
        u32_7((unsigned)(k >> (k & 63)));
    }
    printf("u32_7 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_9((int)v);
        s32_9((int)-v);
        s32_9((int)(v + 1));
        s32_9((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_9((int)k);
        s32_9((int)(k >> (k & 63)));
    }
    printf("s32_9 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_9((unsigned)v);
        u32_9((unsigned)-v);
        u32_9((unsigned)(v + 1));
        u32_9((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_9((unsigned)k);
        u32_9((unsigned)(k >> (k & 63)));
    }
    printf("u32_9 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_10((int)v);
        s32_10((int)-v);
        s32_10((int)(v + 1));
        s32_10((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_10((int)k);
        s32_10((int)(k >> (k & 63)));
    }
    printf("s32_10 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_10((unsigned)v);
        u32_10((unsigned)-v);
        u32_10((unsigned)(v + 1));
        u32_10((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_10((unsigned)k);
        u32_10((unsigned)(k >> (k & 63)));
    }
    printf("u32_10 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_11((int)v);
        s32_11((int)-v);
        s32_11((int)(v + 1));
        s32_11((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_11((int)k);
        s32_11((int)(k >> (k & 63)));
    }
    printf("s32_11 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_11((unsigned)v);
        u32_11((unsigned)-v);
        u32_11((unsigned)(v + 1));
        u32_11((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_11((unsigned)k);
        u32_11((unsigned)(k >> (k & 63)));
    }
    printf("u32_11 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_12((int)v);
        s32_12((int)-v);
        s32_12((int)(v + 1));
        s32_12((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_12((int)k);
        s32_12((int)(k >> (k & 63)));
    }
    printf("s32_12 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_12((unsigned)v);
        u32_12((unsigned)-v);
        u32_12((unsigned)(v + 1));
        u32_12((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_12((unsigned)k);
        u32_12((unsigned)(k >> (k & 63)));
    }
    printf("u32_12 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_13((int)v);
        s32_13((int)-v);
        s32_13((int)(v + 1));
        s32_13((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_13((int)k);
        s32_13((int)(k >> (k & 63)));
    }
    printf("s32_13 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_13((unsigned)v);
        u32_13((unsigned)-v);
        u32_13((unsigned)(v + 1));
        u32_13((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_13((unsigned)k);
        u32_13((unsigned)(k >> (k & 63)));
    }
    printf("u32_13 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_14((int)v);
        s32_14((int)-v);
        s32_14((int)(v + 1));
        s32_14((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_14((int)k);
        s32_14((int)(k >> (k & 63)));
    }
    printf("s32_14 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_14((unsigned)v);
        u32_14((unsigned)-v);
        u32_14((unsigned)(v + 1));
        u32_14((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_14((unsigned)k);
        u32_14((unsigned)(k >> (k & 63)));
    }
    printf("u32_14 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_25((int)v);
        s32_25((int)-v);
        s32_25((int)(v + 1));
        s32_25((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_25((int)k);
        s32_25((int)(k >> (k & 63)));
    }
    printf("s32_25 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_25((unsigned)v);
        u32_25((unsigned)-v);
        u32_25((unsigned)(v + 1));
        u32_25((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_25((unsigned)k);
        u32_25((unsigned)(k >> (k & 63)));
    }
    printf("u32_25 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_60((int)v);
        s32_60((int)-v);
        s32_60((int)(v + 1));
        s32_60((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_60((int)k);
        s32_60((int)(k >> (k & 63)));
    }
    printf("s32_60 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_60((unsigned)v);
        u32_60((unsigned)-v);
        u32_60((unsigned)(v + 1));
        u32_60((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_60((unsigned)k);
        u32_60((unsigned)(k >> (k & 63)));
    }
    printf("u32_60 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_100((int)v);
        s32_100((int)-v);
        s32_100((int)(v + 1));
        s32_100((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_100((int)k);
        s32_100((int)(k >> (k & 63)));
    }
    printf("s32_100 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_100((unsigned)v);
        u32_100((unsigned)-v);
        u32_100((unsigned)(v + 1));
        u32_100((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_100((unsigned)k);
        u32_100((unsigned)(k >> (k & 63)));
    }
    printf("u32_100 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_125((int)v);
        s32_125((int)-v);
        s32_125((int)(v + 1));
        s32_125((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_125((int)k);
        s32_125((int)(k >> (k & 63)));
    }
    printf("s32_125 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_125((unsigned)v);
        u32_125((unsigned)-v);
        u32_125((unsigned)(v + 1));
        u32_125((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_125((unsigned)k);
        u32_125((unsigned)(k >> (k & 63)));
    }
    printf("u32_125 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_641((int)v);
        s32_641((int)-v);
        s32_641((int)(v + 1));
        s32_641((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_641((int)k);
        s32_641((int)(k >> (k & 63)));
    }
    printf("s32_641 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_641((unsigned)v);
        u32_641((unsigned)-v);
        u32_641((unsigned)(v + 1));
        u32_641((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_641((unsigned)k);
        u32_641((unsigned)(k >> (k & 63)));
    }
    printf("u32_641 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_1000((int)v);
        s32_1000((int)-v);
        s32_1000((int)(v + 1));
        s32_1000((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_1000((int)k);
        s32_1000((int)(k >> (k & 63)));
    }
    printf("s32_1000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_1000((unsigned)v);
        u32_1000((unsigned)-v);
        u32_1000((unsigned)(v + 1));
        u32_1000((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_1000((unsigned)k);
        u32_1000((unsigned)(k >> (k & 63)));
    }
    printf("u32_1000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_1025((int)v);
        s32_1025((int)-v);
        s32_1025((int)(v + 1));
        s32_1025((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_1025((int)k);
        s32_1025((int)(k >> (k & 63)));
    }
    printf("s32_1025 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_1025((unsigned)v);
        u32_1025((unsigned)-v);
        u32_1025((unsigned)(v + 1));
        u32_1025((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_1025((unsigned)k);
        u32_1025((unsigned)(k >> (k & 63)));
    }
    printf("u32_1025 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_3600((int)v);
        s32_3600((int)-v);
        s32_3600((int)(v + 1));
        s32_3600((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_3600((int)k);
        s32_3600((int)(k >> (k & 63)));
    }
    printf("s32_3600 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_3600((unsigned)v);
        u32_3600((unsigned)-v);
        u32_3600((unsigned)(v + 1));
        u32_3600((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_3600((unsigned)k);
        u32_3600((unsigned)(k >> (k & 63)));
    }
    printf("u32_3600 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_65535((int)v);
        s32_65535((int)-v);
        s32_65535((int)(v + 1));
        s32_65535((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_65535((int)k);
        s32_65535((int)(k >> (k & 63)));
    }
    printf("s32_65535 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_65535((unsigned)v);
        u32_65535((unsigned)-v);
        u32_65535((unsigned)(v + 1));
        u32_65535((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_65535((unsigned)k);
        u32_65535((unsigned)(k >> (k & 63)));
    }
    printf("u32_65535 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_65537((int)v);
        s32_65537((int)-v);
        s32_65537((int)(v + 1));
        s32_65537((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_65537((int)k);
        s32_65537((int)(k >> (k & 63)));
    }
    printf("s32_65537 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_65537((unsigned)v);
        u32_65537((unsigned)-v);
        u32_65537((unsigned)(v + 1));
        u32_65537((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_65537((unsigned)k);
        u32_65537((unsigned)(k >> (k & 63)));
    }
    printf("u32_65537 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_1000000((int)v);
        s32_1000000((int)-v);
        s32_1000000((int)(v + 1));
        s32_1000000((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_1000000((int)k);
        s32_1000000((int)(k >> (k & 63)));
    }
    printf("s32_1000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_1000000((unsigned)v);
        u32_1000000((unsigned)-v);
        u32_1000000((unsigned)(v + 1));
        u32_1000000((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_1000000((unsigned)k);
        u32_1000000((unsigned)(k >> (k & 63)));
    }
    printf("u32_1000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_16777215((int)v);
        s32_16777215((int)-v);
        s32_16777215((int)(v + 1));
        s32_16777215((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_16777215((int)k);
        s32_16777215((int)(k >> (k & 63)));
    }
    printf("s32_16777215 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_16777215((unsigned)v);
        u32_16777215((unsigned)-v);
        u32_16777215((unsigned)(v + 1));
        u32_16777215((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_16777215((unsigned)k);
        u32_16777215((unsigned)(k >> (k & 63)));
    }
    printf("u32_16777215 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_2147483647((int)v);
        s32_2147483647((int)-v);
        s32_2147483647((int)(v + 1));
        s32_2147483647((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_2147483647((int)k);
        s32_2147483647((int)(k >> (k & 63)));
    }
    printf("s32_2147483647 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_2147483647((unsigned)v);
        u32_2147483647((unsigned)-v);
        u32_2147483647((unsigned)(v + 1));
        u32_2147483647((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_2147483647((unsigned)k);
        u32_2147483647((unsigned)(k >> (k & 63)));
    }
    printf("u32_2147483647 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_1073741825((int)v);
        s32_1073741825((int)-v);
        s32_1073741825((int)(v + 1));
        s32_1073741825((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_1073741825((int)k);
        s32_1073741825((int)(k >> (k & 63)));
    }
    printf("s32_1073741825 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_1073741825((unsigned)v);
        u32_1073741825((unsigned)-v);
        u32_1073741825((unsigned)(v + 1));
        u32_1073741825((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_1073741825((unsigned)k);
        u32_1073741825((unsigned)(k >> (k & 63)));
    }
    printf("u32_1073741825 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s32_1431655765((int)v);
        s32_1431655765((int)-v);
        s32_1431655765((int)(v + 1));
        s32_1431655765((int)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s32_1431655765((int)k);
        s32_1431655765((int)(k >> (k & 63)));
    }
    printf("s32_1431655765 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u32_1431655765((unsigned)v);
        u32_1431655765((unsigned)-v);
        u32_1431655765((unsigned)(v + 1));
        u32_1431655765((unsigned)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u32_1431655765((unsigned)k);
        u32_1431655765((unsigned)(k >> (k & 63)));
    }
    printf("u32_1431655765 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_3((long)v);
        s64_3((long)-v);
        s64_3((long)(v + 1));
        s64_3((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_3((long)k);
        s64_3((long)(k >> (k & 63)));
    }
    printf("s64_3 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_3((unsigned long)v);
        u64_3((unsigned long)-v);
        u64_3((unsigned long)(v + 1));
        u64_3((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_3((unsigned long)k);
        u64_3((unsigned long)(k >> (k & 63)));
    }
    printf("u64_3 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_5((long)v);
        s64_5((long)-v);
        s64_5((long)(v + 1));
        s64_5((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_5((long)k);
        s64_5((long)(k >> (k & 63)));
    }
    printf("s64_5 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_5((unsigned long)v);
        u64_5((unsigned long)-v);
        u64_5((unsigned long)(v + 1));
        u64_5((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_5((unsigned long)k);
        u64_5((unsigned long)(k >> (k & 63)));
    }
    printf("u64_5 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_6((long)v);
        s64_6((long)-v);
        s64_6((long)(v + 1));
        s64_6((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_6((long)k);
        s64_6((long)(k >> (k & 63)));
    }
    printf("s64_6 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_6((unsigned long)v);
        u64_6((unsigned long)-v);
        u64_6((unsigned long)(v + 1));
        u64_6((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_6((unsigned long)k);
        u64_6((unsigned long)(k >> (k & 63)));
    }
    printf("u64_6 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_7((long)v);
        s64_7((long)-v);
        s64_7((long)(v + 1));
        s64_7((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_7((long)k);
        s64_7((long)(k >> (k & 63)));
    }
    printf("s64_7 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_7((unsigned long)v);
        u64_7((unsigned long)-v);
        u64_7((unsigned long)(v + 1));
        u64_7((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_7((unsigned long)k);
        u64_7((unsigned long)(k >> (k & 63)));
    }
    printf("u64_7 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_9((long)v);
        s64_9((long)-v);
        s64_9((long)(v + 1));
        s64_9((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_9((long)k);
        s64_9((long)(k >> (k & 63)));
    }
    printf("s64_9 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_9((unsigned long)v);
        u64_9((unsigned long)-v);
        u64_9((unsigned long)(v + 1));
        u64_9((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_9((unsigned long)k);
        u64_9((unsigned long)(k >> (k & 63)));
    }
    printf("u64_9 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_10((long)v);
        s64_10((long)-v);
        s64_10((long)(v + 1));
        s64_10((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_10((long)k);
        s64_10((long)(k >> (k & 63)));
    }
    printf("s64_10 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_10((unsigned long)v);
        u64_10((unsigned long)-v);
        u64_10((unsigned long)(v + 1));
        u64_10((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_10((unsigned long)k);
        u64_10((unsigned long)(k >> (k & 63)));
    }
    printf("u64_10 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_11((long)v);
        s64_11((long)-v);
        s64_11((long)(v + 1));
        s64_11((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_11((long)k);
        s64_11((long)(k >> (k & 63)));
    }
    printf("s64_11 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_11((unsigned long)v);
        u64_11((unsigned long)-v);
        u64_11((unsigned long)(v + 1));
        u64_11((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_11((unsigned long)k);
        u64_11((unsigned long)(k >> (k & 63)));
    }
    printf("u64_11 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_12((long)v);
        s64_12((long)-v);
        s64_12((long)(v + 1));
        s64_12((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_12((long)k);
        s64_12((long)(k >> (k & 63)));
    }
    printf("s64_12 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_12((unsigned long)v);
        u64_12((unsigned long)-v);
        u64_12((unsigned long)(v + 1));
        u64_12((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_12((unsigned long)k);
        u64_12((unsigned long)(k >> (k & 63)));
    }
    printf("u64_12 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_13((long)v);
        s64_13((long)-v);
        s64_13((long)(v + 1));
        s64_13((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_13((long)k);
        s64_13((long)(k >> (k & 63)));
    }
    printf("s64_13 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_13((unsigned long)v);
        u64_13((unsigned long)-v);
        u64_13((unsigned long)(v + 1));
        u64_13((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_13((unsigned long)k);
        u64_13((unsigned long)(k >> (k & 63)));
    }
    printf("u64_13 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_14((long)v);
        s64_14((long)-v);
        s64_14((long)(v + 1));
        s64_14((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_14((long)k);
        s64_14((long)(k >> (k & 63)));
    }
    printf("s64_14 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_14((unsigned long)v);
        u64_14((unsigned long)-v);
        u64_14((unsigned long)(v + 1));
        u64_14((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_14((unsigned long)k);
        u64_14((unsigned long)(k >> (k & 63)));
    }
    printf("u64_14 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_25((long)v);
        s64_25((long)-v);
        s64_25((long)(v + 1));
        s64_25((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_25((long)k);
        s64_25((long)(k >> (k & 63)));
    }
    printf("s64_25 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_25((unsigned long)v);
        u64_25((unsigned long)-v);
        u64_25((unsigned long)(v + 1));
        u64_25((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_25((unsigned long)k);
        u64_25((unsigned long)(k >> (k & 63)));
    }
    printf("u64_25 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_60((long)v);
        s64_60((long)-v);
        s64_60((long)(v + 1));
        s64_60((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_60((long)k);
        s64_60((long)(k >> (k & 63)));
    }
    printf("s64_60 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_60((unsigned long)v);
        u64_60((unsigned long)-v);
        u64_60((unsigned long)(v + 1));
        u64_60((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_60((unsigned long)k);
        u64_60((unsigned long)(k >> (k & 63)));
    }
    printf("u64_60 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_100((long)v);
        s64_100((long)-v);
        s64_100((long)(v + 1));
        s64_100((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_100((long)k);
        s64_100((long)(k >> (k & 63)));
    }
    printf("s64_100 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_100((unsigned long)v);
        u64_100((unsigned long)-v);
        u64_100((unsigned long)(v + 1));
        u64_100((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_100((unsigned long)k);
        u64_100((unsigned long)(k >> (k & 63)));
    }
    printf("u64_100 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_125((long)v);
        s64_125((long)-v);
        s64_125((long)(v + 1));
        s64_125((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_125((long)k);
        s64_125((long)(k >> (k & 63)));
    }
    printf("s64_125 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_125((unsigned long)v);
        u64_125((unsigned long)-v);
        u64_125((unsigned long)(v + 1));
        u64_125((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_125((unsigned long)k);
        u64_125((unsigned long)(k >> (k & 63)));
    }
    printf("u64_125 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_641((long)v);
        s64_641((long)-v);
        s64_641((long)(v + 1));
        s64_641((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_641((long)k);
        s64_641((long)(k >> (k & 63)));
    }
    printf("s64_641 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_641((unsigned long)v);
        u64_641((unsigned long)-v);
        u64_641((unsigned long)(v + 1));
        u64_641((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_641((unsigned long)k);
        u64_641((unsigned long)(k >> (k & 63)));
    }
    printf("u64_641 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1000((long)v);
        s64_1000((long)-v);
        s64_1000((long)(v + 1));
        s64_1000((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1000((long)k);
        s64_1000((long)(k >> (k & 63)));
    }
    printf("s64_1000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1000((unsigned long)v);
        u64_1000((unsigned long)-v);
        u64_1000((unsigned long)(v + 1));
        u64_1000((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1000((unsigned long)k);
        u64_1000((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1025((long)v);
        s64_1025((long)-v);
        s64_1025((long)(v + 1));
        s64_1025((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1025((long)k);
        s64_1025((long)(k >> (k & 63)));
    }
    printf("s64_1025 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1025((unsigned long)v);
        u64_1025((unsigned long)-v);
        u64_1025((unsigned long)(v + 1));
        u64_1025((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1025((unsigned long)k);
        u64_1025((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1025 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_3600((long)v);
        s64_3600((long)-v);
        s64_3600((long)(v + 1));
        s64_3600((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_3600((long)k);
        s64_3600((long)(k >> (k & 63)));
    }
    printf("s64_3600 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_3600((unsigned long)v);
        u64_3600((unsigned long)-v);
        u64_3600((unsigned long)(v + 1));
        u64_3600((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_3600((unsigned long)k);
        u64_3600((unsigned long)(k >> (k & 63)));
    }
    printf("u64_3600 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_65535((long)v);
        s64_65535((long)-v);
        s64_65535((long)(v + 1));
        s64_65535((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_65535((long)k);
        s64_65535((long)(k >> (k & 63)));
    }
    printf("s64_65535 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_65535((unsigned long)v);
        u64_65535((unsigned long)-v);
        u64_65535((unsigned long)(v + 1));
        u64_65535((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_65535((unsigned long)k);
        u64_65535((unsigned long)(k >> (k & 63)));
    }
    printf("u64_65535 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_65537((long)v);
        s64_65537((long)-v);
        s64_65537((long)(v + 1));
        s64_65537((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_65537((long)k);
        s64_65537((long)(k >> (k & 63)));
    }
    printf("s64_65537 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_65537((unsigned long)v);
        u64_65537((unsigned long)-v);
        u64_65537((unsigned long)(v + 1));
        u64_65537((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_65537((unsigned long)k);
        u64_65537((unsigned long)(k >> (k & 63)));
    }
    printf("u64_65537 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1000000((long)v);
        s64_1000000((long)-v);
        s64_1000000((long)(v + 1));
        s64_1000000((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1000000((long)k);
        s64_1000000((long)(k >> (k & 63)));
    }
    printf("s64_1000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1000000((unsigned long)v);
        u64_1000000((unsigned long)-v);
        u64_1000000((unsigned long)(v + 1));
        u64_1000000((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1000000((unsigned long)k);
        u64_1000000((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_16777215((long)v);
        s64_16777215((long)-v);
        s64_16777215((long)(v + 1));
        s64_16777215((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_16777215((long)k);
        s64_16777215((long)(k >> (k & 63)));
    }
    printf("s64_16777215 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_16777215((unsigned long)v);
        u64_16777215((unsigned long)-v);
        u64_16777215((unsigned long)(v + 1));
        u64_16777215((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_16777215((unsigned long)k);
        u64_16777215((unsigned long)(k >> (k & 63)));
    }
    printf("u64_16777215 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_2147483647((long)v);
        s64_2147483647((long)-v);
        s64_2147483647((long)(v + 1));
        s64_2147483647((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_2147483647((long)k);
        s64_2147483647((long)(k >> (k & 63)));
    }
    printf("s64_2147483647 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_2147483647((unsigned long)v);
        u64_2147483647((unsigned long)-v);
        u64_2147483647((unsigned long)(v + 1));
        u64_2147483647((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_2147483647((unsigned long)k);
        u64_2147483647((unsigned long)(k >> (k & 63)));
    }
    printf("u64_2147483647 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1073741825((long)v);
        s64_1073741825((long)-v);
        s64_1073741825((long)(v + 1));
        s64_1073741825((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1073741825((long)k);
        s64_1073741825((long)(k >> (k & 63)));
    }
    printf("s64_1073741825 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1073741825((unsigned long)v);
        u64_1073741825((unsigned long)-v);
        u64_1073741825((unsigned long)(v + 1));
        u64_1073741825((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1073741825((unsigned long)k);
        u64_1073741825((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1073741825 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1431655765((long)v);
        s64_1431655765((long)-v);
        s64_1431655765((long)(v + 1));
        s64_1431655765((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1431655765((long)k);
        s64_1431655765((long)(k >> (k & 63)));
    }
    printf("s64_1431655765 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1431655765((unsigned long)v);
        u64_1431655765((unsigned long)-v);
        u64_1431655765((unsigned long)(v + 1));
        u64_1431655765((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1431655765((unsigned long)k);
        u64_1431655765((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1431655765 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_4294967295((long)v);
        s64_4294967295((long)-v);
        s64_4294967295((long)(v + 1));
        s64_4294967295((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_4294967295((long)k);
        s64_4294967295((long)(k >> (k & 63)));
    }
    printf("s64_4294967295 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_4294967295((unsigned long)v);
        u64_4294967295((unsigned long)-v);
        u64_4294967295((unsigned long)(v + 1));
        u64_4294967295((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_4294967295((unsigned long)k);
        u64_4294967295((unsigned long)(k >> (k & 63)));
    }
    printf("u64_4294967295 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_4294967297((long)v);
        s64_4294967297((long)-v);
        s64_4294967297((long)(v + 1));
        s64_4294967297((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_4294967297((long)k);
        s64_4294967297((long)(k >> (k & 63)));
    }
    printf("s64_4294967297 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_4294967297((unsigned long)v);
        u64_4294967297((unsigned long)-v);
        u64_4294967297((unsigned long)(v + 1));
        u64_4294967297((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_4294967297((unsigned long)k);
        u64_4294967297((unsigned long)(k >> (k & 63)));
    }
    printf("u64_4294967297 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1000000000000((long)v);
        s64_1000000000000((long)-v);
        s64_1000000000000((long)(v + 1));
        s64_1000000000000((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1000000000000((long)k);
        s64_1000000000000((long)(k >> (k & 63)));
    }
    printf("s64_1000000000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1000000000000((unsigned long)v);
        u64_1000000000000((unsigned long)-v);
        u64_1000000000000((unsigned long)(v + 1));
        u64_1000000000000((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1000000000000((unsigned long)k);
        u64_1000000000000((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1000000000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_1000000000000000000((long)v);
        s64_1000000000000000000((long)-v);
        s64_1000000000000000000((long)(v + 1));
        s64_1000000000000000000((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_1000000000000000000((long)k);
        s64_1000000000000000000((long)(k >> (k & 63)));
    }
    printf("s64_1000000000000000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_1000000000000000000((unsigned long)v);
        u64_1000000000000000000((unsigned long)-v);
        u64_1000000000000000000((unsigned long)(v + 1));
        u64_1000000000000000000((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_1000000000000000000((unsigned long)k);
        u64_1000000000000000000((unsigned long)(k >> (k & 63)));
    }
    printf("u64_1000000000000000000 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_4611686018427387903((long)v);
        s64_4611686018427387903((long)-v);
        s64_4611686018427387903((long)(v + 1));
        s64_4611686018427387903((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_4611686018427387903((long)k);
        s64_4611686018427387903((long)(k >> (k & 63)));
    }
    printf("s64_4611686018427387903 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_4611686018427387903((unsigned long)v);
        u64_4611686018427387903((unsigned long)-v);
        u64_4611686018427387903((unsigned long)(v + 1));
        u64_4611686018427387903((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_4611686018427387903((unsigned long)k);
        u64_4611686018427387903((unsigned long)(k >> (k & 63)));
    }
    printf("u64_4611686018427387903 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_3074457345618258603((long)v);
        s64_3074457345618258603((long)-v);
        s64_3074457345618258603((long)(v + 1));
        s64_3074457345618258603((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_3074457345618258603((long)k);
        s64_3074457345618258603((long)(k >> (k & 63)));
    }
    printf("s64_3074457345618258603 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_3074457345618258603((unsigned long)v);
        u64_3074457345618258603((unsigned long)-v);
        u64_3074457345618258603((unsigned long)(v + 1));
        u64_3074457345618258603((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_3074457345618258603((unsigned long)k);
        u64_3074457345618258603((unsigned long)(k >> (k & 63)));
    }
    printf("u64_3074457345618258603 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        s64_6700417((long)v);
        s64_6700417((long)-v);
        s64_6700417((long)(v + 1));
        s64_6700417((long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        s64_6700417((long)k);
        s64_6700417((long)(k >> (k & 63)));
    }
    printf("s64_6700417 %lx\n", h);
    h = 0;
    for (k = 0; k < NV; k++) {
        v = vals[k];
        u64_6700417((unsigned long)v);
        u64_6700417((unsigned long)-v);
        u64_6700417((unsigned long)(v + 1));
        u64_6700417((unsigned long)~v);
    }
    for (n = 0; n < 200; n++) {
        k = rnd(&seed);
        u64_6700417((unsigned long)k);
        u64_6700417((unsigned long)(k >> (k & 63)));
    }
    printf("u64_6700417 %lx\n", h);
    printf("%ld %ld %ld %ld\n", diff(o + 37, o + 3), diff(o + 3, o + 37), diff3(p + 49, p), diff3(p + 1, p + 44));
    return 0;
}
