#include "abi.h"
int printf(const char *, ...);

void fill(void *p, int size, long seed)
{
    unsigned char *q = p;
    unsigned long x = (unsigned long)seed * 6364136223846793005UL + 1442695040888963407UL;
    int k;
    for (k = 0; k < size; k++) {
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        q[k] = (unsigned char)(x >> 56);
    }
}

/* Padding does not count: the value is copied into zeroed storage field by field by the caller of hash
   (here it is enough that every type is made by fill and compared with another copy made by fill, but
   padding bytes may be lost in registers, so they are masked out by a mask taken from two fills). */
static unsigned char mask[64];
static int masksize;
long hash(const void *p, int size)
{
    const unsigned char *q = p;
    unsigned long h = 0;
    int k;
    for (k = 0; k < size; k++)
        h = h * 131 + (k < masksize ? q[k] & mask[k] : q[k]);
    return (long)(h >> 8 & 0xffffff);
}

static int bad;
static void check(const char *what, const char *type, long got, long want)
{
    if (got != want) {
        printf("MISMATCH %s %s: got %ld, want %ld\n", what, type, got, want);
        bad++;
    }
}

/* which bytes of T are fields: set every field to all ones through a compound of assignments */
#define SETMASK(T, ...) { T m_; int k_; for (k_ = 0; k_ < (int)sizeof m_; k_++) ((unsigned char *)&m_)[k_] = 0; \
        __VA_ARGS__; masksize = (int)sizeof m_; for (k_ = 0; k_ < masksize; k_++) mask[k_] = ((unsigned char *)&m_)[k_]; }
#define FF(f) { int j_; for (j_ = 0; j_ < (int)sizeof m_.f; j_++) ((unsigned char *)&m_.f)[j_] = 0xff; }

#define TEST(T) \
    { \
        T s = make_##T(1), t = make_##T(2), u = make_##T(3), v = make_##T(4), w = make_##T(5), r; \
        long hs, ht, hu, hv, hw; \
        fill(&s, (int)sizeof s, 1); fill(&t, (int)sizeof t, 2); fill(&u, (int)sizeof u, 3); fill(&v, (int)sizeof v, 4); fill(&w, (int)sizeof w, 5); \
        hs = hash(&s, (int)sizeof s); ht = hash(&t, (int)sizeof t); hu = hash(&u, (int)sizeof u); hv = hash(&v, (int)sizeof v); hw = hash(&w, (int)sizeof w); \
        r = make_##T(1); check("make", #T, hash(&r, (int)sizeof r), hs); \
        check("take0", #T, take0_##T(s), hs); \
        check("take5", #T, take5_##T(1, 2, 3, 4, 5, t, 6), 1 + 4 + 9 + 16 + 25 + ht + 42); \
        check("take7d", #T, take7d_##T(1, 2, 3, 4, 5, 6, 7, u, 8), 1 + 4 + 9 + 16 + 25 + 36 + 49 + 72 + hu); \
        check("takemix", #T, takemix_##T(s, 10, 20.0, t, 30, u, 40.0f, v, w), hs + 3 * ht + 5 * hu + 7 * hv + 11 * hw + 100); \
        check("takestk", #T, takestk_##T(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, v, 15, w), 120 + hv + 3 * hw); \
        r = ret_##T(3); check("ret", #T, hash(&r, (int)sizeof r), hu); \
        r = pass_##T(1, v, 2.0); check("pass", #T, hash(&r, (int)sizeof r), hv); \
        check("var", #T, var_##T(3, s, 1, 2.0, t, 3, 4.0, u, 5, 6.0), ((hs + 3) * 3 + ht + 7) * 3 + hu + 11); \
        check("var8", #T, var_##T(8, s, 1, 2.0, t, 3, 4.0, u, 5, 6.0, v, 1, 1.0, w, 1, 1.0, s, 1, 1.0, t, 1, 1.0, u, 1, 1.0), \
              (((((((hs + 3) * 3 + ht + 7) * 3 + hu + 11) * 3 + hv + 2) * 3 + hw + 2) * 3 + hs + 2) * 3 + ht + 2) * 3 + hu + 2); \
    }

int main(void)
{
    int x = 77;
    __int128 big = ((__int128)5 << 64) + 9;
    SETMASK(S1, FF(a)) TEST(S1)
    SETMASK(S2, FF(a) FF(b)) TEST(S2)
    SETMASK(S3, FF(a) FF(b) FF(c)) TEST(S3)
    SETMASK(S4, FF(a)) TEST(S4)
    SETMASK(S5, FF(a) FF(b)) TEST(S5)
    SETMASK(S6, FF(a) FF(b)) TEST(S6)
    SETMASK(S7, FF(a) FF(b) FF(c)) TEST(S7)
    SETMASK(S8, FF(a) FF(b)) TEST(S8)
    SETMASK(S9, FF(a) FF(b)) TEST(S9)
    SETMASK(S10, FF(a) FF(b)) TEST(S10)
    SETMASK(S11, FF(a)) TEST(S11)
    SETMASK(S12, FF(a)) TEST(S12)
    SETMASK(S13, FF(a) FF(b) FF(c)) TEST(S13)
    SETMASK(S14, FF(p) FF(a)) TEST(S14)
    SETMASK(S15, FF(a)) TEST(S15)
    SETMASK(S16, FF(a) FF(b) FF(c)) TEST(S16)
    SETMASK(S17, FF(a) FF(b) FF(c) FF(d)) TEST(S17)
    SETMASK(S18, FF(a) FF(b) FF(c)) TEST(S18)
    SETMASK(S19, FF(a)) TEST(S19)
    SETMASK(S20, m_.a = -1; m_.b = ~0u; m_.c = -1) TEST(S20)
    SETMASK(S21, FF(a) FF(b)) TEST(S21)
    SETMASK(S22, FF(a)) TEST(S22)
    SETMASK(S23, FF(a) FF(b)) TEST(S23)
    SETMASK(S24, FF(a) FF(b)) TEST(S24)
    masksize = 0;
    check("var_mixed", "1", var_mixed("idlpidlidddddddddliiiiq", 1, 2.5, 3L, &x, 4, 5.5, 6L, 7, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10L, 11, 12, 13, 14, big),
          var_mixed("idlpidlidddddddddliiiiq", 1, 2.5, 3L, &x, 4, 5.5, 6L, 7, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10L, 11, 12, 13, 14, big));
    check("var_mixed", "2", var_mixed("ildq", 5, 6L, 1.5, big), ((5 * 3 + 6) * 3 + 3) * 3 + 5);
    check("many", "", many(1, 2, 3, 4, 5, 6, 7, 8, 9.0f, 10.0, 11, 12, 13, 14.0f, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 21, 22.0f),
          1 + 4 + 9 + 16 + 25 + 36 + 49 + 64 + 81 + 100 + 121 + 144 + 169 + 196 + 225 + 256 + 289 + 324 + 361 + 400 + 441 + 484);
    check("add128", "", add128(big, 1, big, 2, 3, 4, big), 45091);
    printf("abi: %d mismatches\n", bad);
    return bad != 0;
}
