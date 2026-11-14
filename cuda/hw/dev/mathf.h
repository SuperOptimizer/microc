/* The device library, single precision: the functions of <math.h> that the device has no instruction for,
   in float arithmetic only (a GPU's doubles are slow) and without tables, each good to about a unit in the
   last place. fma is relied on. The including file says how a function is declared (MCD: all are meant to be inlined into their callers), how the bits of
   a float are read and made (F2U, U2F), and gives fma, rint, sqrt, trunc, fabs, and the lesser and the greater
   of two floats (FMA, RINT, SQRT, TRUNC, FABS, FMIN, FMAX), which are instructions; and 2 to the power of a
   float and the inverse of one, as the device approximates them (EX2 where the result is a normal number, EX2N
   anywhere; RCP). */

#define MCD_INF U2F(0x7f800000U)
#define MCD_NAN U2F(0x7fc00000U)
#define LN2HI 0x1.62e4p-1f      /* log 2 = LN2HI + LN2LO, the first with few bits: a small multiple of it is exact */
#define LN2LO 0x1.7f7d1cp-20f
#define LN2 0x1.62e43p-1f
#define L102HI 0x1.344000p-2f /* log10(2) the same way */
#define L102LO 0x1.3509f8p-18f
#define LOG2E 0x1.715476p+0f
#define INVLN2_L 0x1.4ae0cp-26f /* 1 / log 2 = LOG2E + INVLN2_L */
#define PI_H 0x1.921fb6p+1f
#define PI_L -0x1.777a5cp-24f
#define PIO2_H 0x1.921fb6p+0f
#define PIO2_L -0x1.777a5cp-25f
#define PIO4_H 0x1.921fb6p-1f
#define PIO4_L -0x1.777a5cp-26f

/* The polynomials, each fitted to its function over the interval it is used on (Chebyshev fits). */
/* EXP_P(z) on [-0.3467, 0.3467], to 1.40e-09 */
#define EXP_P(z) (0x1.000000p-1f + (z) * (0x1.555556p-3f + (z) * (0x1.5554e8p-5f + (z) * (0x1.1110e0p-7f + (z) * (0x1.6d434ep-10f + (z) * (0x1.a12516p-13f))))))
/* SIN_P(z) on [0, 0.617], to 2.81e-11 */
#define SIN_P(z) (-0x1.555556p-3f + (z) * (0x1.11110ep-7f + (z) * (-0x1.a013a6p-13f + (z) * (0x1.6dbdc6p-19f))))
/* COS_P(z) on [0, 0.617], to 2.35e-12 */
#define COS_P(z) (0x1.555556p-5f + (z) * (-0x1.6c16c0p-10f + (z) * (0x1.a015c4p-16f + (z) * (-0x1.252422p-22f))))
/* LOG_P(z) on [0, 0.02945], to 1.15e-09 */
#define LOG_P(z) (0x1.555556p-1f + (z) * (0x1.9999ecp-2f + (z) * (0x1.245c38p-2f + (z) * (0x1.ddd172p-3f))))
/* ATAN_P(z) on [0, 0.17158], to 5.36e-10 */
#define ATAN_P(z) (-0x1.555556p-2f + (z) * (0x1.99997cp-3f + (z) * (-0x1.248a16p-3f + (z) * (0x1.c57ba2p-4f + (z) * (-0x1.6143f2p-4f + (z) * (0x1.9d8962p-5f))))))
/* ASIN_P(z) on [0, 0.25], to 2.47e-10 */
#define ASIN_P(z) (0x1.555556p-3f + (z) * (0x1.33331ep-4f + (z) * (0x1.6dc0f0p-5f + (z) * (0x1.efedf8p-6f + (z) * (0x1.82db24p-6f + (z) * (0x1.5a80a0p-7f + (z) * (0x1.fb7ca4p-6f)))))))
/* ERF_P(z) on [0, 1.0], to 3.38e-11 */
#define ERF_P(z) (0x1.20dd76p+0f + (z) * (-0x1.812746p-2f + (z) * (0x1.ce2f08p-4f + (z) * (-0x1.b829c8p-6f + (z) * (0x1.562a60p-8f + (z) * (-0x1.bccf66p-11f + (z) * (0x1.d88750p-14f + (z) * (-0x1.445f48p-17f))))))))
/* ERFC_A(z), z + 0.625 in [0.25, 1.0], to 2.69e-09 */
#define ERFC_A(z) (0x1.f545fcp-2f + (z) * (-0x1.5f684cp-3f + (z) * (-0x1.539aa8p-10f + (z) * (0x1.27545ap-4f + (z) * (-0x1.3f6b7cp-4f + (z) * (0x1.966c34p-5f + (z) * (-0x1.7013f8p-7f + (z) * (-0x1.931bd2p-6f + (z) * (0x1.c1eb20p-5f + (z) * (-0x1.87c352p-5f))))))))))
/* ERFC_B(z), z + 0.174 in [0.09799999999999999, 0.25], to 1.91e-09 */
#define ERFC_B(z) (0x1.1cad5cp-1f + (z) * (-0x1.7138ecp-4f + (z) * (-0x1.bd4178p-3f + (z) * (0x1.ab63a6p-3f + (z) * (0x1.ceaaf4p-4f + (z) * (-0x1.c4fb3ap-2f))))))
/* ERFC_C(z) on [-0.25, 0.25], to 5.80e-11 */
#define ERFC_C(z) (0x1.27c6d2p-2f + (z) * (-0x1.492e42p-1f + (z) * (0x1.edc564p-2f + (z) * (-0x1.b6e67cp-6f + (z) * (-0x1.349b62p-3f + (z) * (0x1.b3f44ap-5f + (z) * (0x1.b84c44p-6f + (z) * (-0x1.250894p-6f + (z) * (-0x1.30f2cep-9f)))))))))

/* LOG1_Q(f) = (log(1 + f) - f) / f^2 on [-0.2929, 0.4143], error 4.97e-09 */
#define LOG1_Q(z) (-0x1.0000000000000p-1f + (z) * (0x1.55554e0000000p-2f + (z) * (-0x1.0000080000000p-2f + (z) * (0x1.99a1300000000p-3f + (z) * (-0x1.5559da0000000p-3f + (z) * (0x1.237a2a0000000p-3f + (z) * (-0x1.fcc31a0000000p-4f + (z) * (0x1.e3d7c20000000p-4f + (z) * (-0x1.d95b800000000p-4f + (z) * (0x1.152d5a0000000p-4f))))))))))
/* ERF_G(t) = log2(erfc(t)) on [1.0, 4.0], error 1.16e-08 */
#define ERF_G(z) (0x1.3ae8040000000p-11f + (z) * (-0x1.a197980000000p+0f + (z) * (-0x1.d20f480000000p-1f + (z) * (-0x1.4820a60000000p-3f + (z) * (0x1.4046fc0000000p-5f + (z) * (-0x1.dee2080000000p-8f + (z) * (0x1.0361140000000p-10f + (z) * (-0x1.78f9420000000p-14f + (z) * (0x1.4198fe0000000p-18f + (z) * (-0x1.d5d90c0000000p-24f))))))))))
/* ATAN1_P(z) = (atan(t) - t) / t^3, z = t^2, on [0, 1.0], error 3.93e-10 */
#define ATAN1_P(z) (-0x1.5555560000000p-2f + (z) * (0x1.99998c0000000p-3f + (z) * (-0x1.2490360000000p-3f + (z) * (0x1.c6d94a0000000p-4f + (z) * (-0x1.72251c0000000p-4f + (z) * (0x1.2fd2180000000p-4f + (z) * (-0x1.d7eaf80000000p-5f + (z) * (0x1.39fa7a0000000p-5f + (z) * (-0x1.3b92d40000000p-6f + (z) * (0x1.96cc740000000p-8f + (z) * (-0x1.eae3fc0000000p-11f)))))))))))
/* TANH_P(z) = (tanh(t) - t) / t^3, z = t^2, on [0, 0.3025], error 3.80e-10 */
#define TANH_P(z) (-0x1.5555560000000p-2f + (z) * (0x1.1111040000000p-3f + (z) * (-0x1.ba142e0000000p-5f + (z) * (0x1.6579360000000p-6f + (z) * (-0x1.1764100000000p-7f + (z) * (0x1.4b239a0000000p-9f))))))

/* y 2^k for any k, with one rounding where the result is subnormal */
MCD float mcd_scalef(float y, int k)
{
    /* up to three steps, each a power that a float holds; a step down leaves 24 bits of room, so that only
       the last can round */
    int a = k > 127 ? 127 : 0, b;
    a = k < -126 ? -102 : a;
    k -= a;
    b = k > 127 ? 127 : 0;
    b = k < -126 ? -102 : b;
    k -= b;
    k = k > 127 ? 127 : k;
    k = k < -126 ? -126 : k;
    return ((y * U2F((unsigned)(127 + a) << 23)) * U2F((unsigned)(127 + b) << 23)) * U2F((unsigned)(127 + k) << 23);
}

MCD float mcd_scalbnf(float x, int n)
{
    n = n > 400 ? 400 : n;
    return mcd_scalef(x, n < -400 ? -400 : n);
}
MCD float mcd_ldexpf(float x, int n) { return mcd_scalbnf(x, n); }

/* (Where a choice is made between two values, both are computed first: the choice is then one instruction
   and not a branch.) */
MCD float mcd_frexpf(float x, int *e)
{
    unsigned ix = F2U(x), ax = ix & 0x7fffffffU, is = F2U(x * 0x1p25f), iy;
    int sub = ax < 0x00800000U, special = ax == 0 || ax >= 0x7f800000U, k = sub ? -25 : 0;
    float m;
    iy = sub ? is : ix;
    k += (int)(iy >> 23 & 255) - 126;
    m = U2F((iy & 0x807fffffU) | 0x3f000000U);
    *e = special ? 0 : k;
    return special ? x : m;
}

MCD int mcd_ilogbf(float x)
{
    unsigned ax = F2U(x) & 0x7fffffffU, as = F2U(FABS(x) * 0x1p25f), ay;
    int sub = ax < 0x00800000U, e = sub ? -152 : -127;
    ay = sub ? as : ax;
    e += (int)(ay >> 23);
    e = ax == 0x7f800000U ? 2147483647 : e;
    return ax == 0 || ax > 0x7f800000U ? -2147483647 - 1 : e;
}

MCD float mcd_logbf(float x)
{
    unsigned ax = F2U(x) & 0x7fffffffU;
    if (ax == 0)
        return -MCD_INF;
    if (ax >= 0x7f800000U)
        return x * x;
    return (float)mcd_ilogbf(x);
}

MCD float mcd_modff(float x, float *ip)
{
    float t = TRUNC(x);
    *ip = t;
    if (FABS(x) == MCD_INF)
        return U2F(F2U(x) & 0x80000000U);
    return U2F((F2U(x - t) & 0x7fffffffU) | (F2U(x) & 0x80000000U));
}

MCD float mcd_roundf(float x)
{
    float t = TRUNC(x), u = t + U2F(0x3f800000U | (F2U(x) & 0x80000000U));
    return FABS(x - t) >= 0.5f ? u : t; /* (x - t is exact) */
}

MCD long long mcd_llroundf(float x) { return (long long)mcd_roundf(x); }
MCD long mcd_lroundf(float x) { return (long)mcd_roundf(x); }
MCD long long mcd_llrintf(float x) { return (long long)RINT(x); }
MCD long mcd_lrintf(float x) { return (long)RINT(x); }
MCD float mcd_nearbyintf(float x) { return RINT(x); }
MCD float mcd_nanf(const char *s) { (void)s; return MCD_NAN; }
MCD float mcd_fdimf(float x, float y) { return x != x ? x : y != y ? y : x > y ? x - y : 0.0f; }
MCD float mcd_rsqrtf(float x) { return 1.0f / SQRT(x); }

/* ---- exponentials ---- */

/* x = k log 2 + r log 2 with k a whole number of at most 126 in size, and e^x = 2^r 2^k: the first power is
   the device's instruction, the second is made from k's bits. Adding 1.5 2^23 to a float rounds it to a whole
   number, which the sum's low bits then hold; with 127 more, shifting them up gives the bits of 2^k. No
   branches, conversions or choices: beyond the range r grows and the first power overflows or vanishes. */
MCD float mcd_expf(float x)
{
    float j = FMIN(FMAX(x * LOG2E, -126.0f), 126.0f) + 12583039.0f, k = j - 12583039.0f, r;
    r = FMA(x, LOG2E, -k);
    r = FMA(x, INVLN2_L, r);
    return EX2(r) * U2F(F2U(j) << 23);
}

MCD float mcd_exp2f(float x) { return EX2N(x); }

MCD float mcd_exp10f(float x)
{
    float k, r;
    if (x != x)
        return x;
    if (x > 38.54f)
        return MCD_INF;
    if (x < -45.2f)
        return 0.0f;
    k = RINT(x * 0x1.a934fp+1f);
    r = FMA(k, -L102HI, x); /* log10(2) in two parts */
    r = FMA(k, -L102LO, r);
    r = FMA(r, 0x1.26bb1cp+1f, r * -0x1.12aabap-25f); /* times log 10 */
    return mcd_scalef(1.0f + (r + r * r * EXP_P(r)), (int)k);
}

MCD float mcd_expm1f(float x)
{
    float k, r, p, t;
    if (x != x || x == 0.0f)
        return x;
    if (x > 88.72284f)
        return MCD_INF;
    if (x < -17.4f)
        return -1.0f;
    if (FABS(x) < 0.3466f)
        return x + x * x * EXP_P(x);
    k = RINT(x * LOG2E);
    r = FMA(k, -LN2HI, x);
    r = FMA(k, -LN2LO, r);
    p = r + r * r * EXP_P(r);
    if (k > 30.0f)
        return mcd_scalef(1.0f + p, (int)k);
    t = U2F((unsigned)(127 + (int)k) << 23);
    return FMA(t, p, t - 1.0f);
}

/* ---- logarithms ---- */

/* x > 0 and finite: x = 2^k (1 + f) with sqrt(1/2) <= 1 + f < sqrt 2. The bits that hold both: k is
   MCD_LOGK of them and f is MCD_LOGF. */
MCD unsigned mcd_logbits(float x)
{
    unsigned ix = F2U(x), is = F2U(x * 0x1p25f) - (25U << 23);
    ix = ix < 0x00800000U ? is : ix;
    return ix + (0x3f800000U - 0x3f3504f3U);
}
#define MCD_LOGK(b) (((int)(b) >> 23) - 127)
#define MCD_LOGF(b) (U2F(((b) & 0x007fffffU) + 0x3f3504f3U) - 1.0f)

/* the logarithm r of x where x is positive and finite, else what the logarithm of x is */
MCD float mcd_logspecial(float x, float r)
{
    unsigned ix = F2U(x);
    float nan = MCD_NAN, minf = -MCD_INF;
    r = ix > 0x7f800000U ? nan : r; /* negative, or not a number */
    r = ix == 0x7f800000U ? x : r;
    return ix << 1 == 0 ? minf : r;
}

/* log(1 + f) - f, for such an f */
#define MCD_LOG1(f) ((f) * (f) * LOG1_Q(f))

MCD float mcd_logf(float x)
{
    unsigned b = mcd_logbits(x);
    float f = MCD_LOGF(b), dk = (float)MCD_LOGK(b);
    return mcd_logspecial(x, FMA(dk, 0x1.62e300p-1f, f + FMA(dk, 0x1.2fefa4p-17f, MCD_LOG1(f))));
}

MCD float mcd_log2f(float x)
{
    unsigned b = mcd_logbits(x);
    float f = MCD_LOGF(b), dk = (float)MCD_LOGK(b), t = f + MCD_LOG1(f), p = t * LOG2E;
    return mcd_logspecial(x, (p + (FMA(t, LOG2E, -p) + t * INVLN2_L)) + dk);
}

MCD float mcd_log10f(float x)
{
    unsigned b = mcd_logbits(x);
    float f = MCD_LOGF(b), dk = (float)MCD_LOGK(b), t = f + MCD_LOG1(f), p = t * 0x1.bcb7b2p-2f; /* 1 / log 10, and what it lacks */
    return mcd_logspecial(x, FMA(dk, L102HI, p + (FMA(t, 0x1.bcb7b2p-2f, -p) + FMA(t, -0x1.5b235ep-27f, dk * L102LO))));
}

MCD float mcd_log1pf(float x)
{
    float u = 1.0f + x, f, c, c2, dk, r, nan = MCD_NAN, minf = -MCD_INF;
    unsigned iu = F2U(u) + (0x3f800000U - 0x3f3504f3U);
    int k = (int)(iu >> 23) - 127;
    c = 1.0f - (u - x); /* what 1 + x lost, one way or the other */
    c2 = x - (u - 1.0f);
    c = k >= 2 ? c : c2;
    c *= RCP(u); /* (small beside the result: an approximate quotient will do) */
    c = k < 25 ? c : 0.0f;
    f = U2F((iu & 0x007fffffU) + 0x3f3504f3U) - 1.0f;
    dk = (float)k;
    r = FMA(dk, 0x1.62e300p-1f, f + (FMA(dk, 0x1.2fefa4p-17f, c) + MCD_LOG1(f)));
    r = x == 0.0f || x == MCD_INF ? x : r;
    r = x < -1.0f ? nan : r;
    r = x == -1.0f ? minf : r;
    return x != x ? x : r;
}

MCD float mcd_powf(float x, float y)
{
    unsigned ix = F2U(x), iy = F2U(y), ax = ix & 0x7fffffffU, ay = iy & 0x7fffffffU;
    int yint = 0, neg = 0; /* yint: y is an odd (1) or even (2) integer */
    unsigned lb;
    float f, t, tl, sh, sl, z, h, l, hh, ll, ph, pl, dk, rh, rl, eh, el, n, r;
    if (ay == 0 || ix == 0x3f800000U)
        return 1.0f;
    if (ax > 0x7f800000U || ay > 0x7f800000U)
        return x + y;
    if (ay >= 0x4b800000U)
        yint = 2;
    else if (ay >= 0x3f800000U && TRUNC(y) == y)
        yint = ((int)FABS(y) & 1) ? 1 : 2;
    if (ay == 0x7f800000U) {
        if (ax == 0x3f800000U)
            return 1.0f;
        return (ax > 0x3f800000U) == !(iy >> 31) ? MCD_INF : 0.0f;
    }
    if (ax == 0x7f800000U || ax == 0) {
        r = (ax == 0) == (iy >> 31 != 0) ? MCD_INF : 0.0f;
        return (ix >> 31) && yint == 1 ? -r : r;
    }
    if (ix >> 31) {
        if (!yint)
            return MCD_NAN;
        neg = yint == 1;
        x = -x;
    }
    /* log2 x as a sum of two floats */
    lb = mcd_logbits(x);
    f = MCD_LOGF(lb);
    dk = (float)MCD_LOGK(lb);
    t = 2.0f + f;
    tl = (2.0f - t) + f;
    sh = f / t;
    sl = (FMA(-sh, t, f) - sh * tl) / t;
    z = sh * sh;
    h = 2.0f * sh;
    l = 2.0f * sl + sh * (z * LOG_P(z));
    hh = h + l;
    ll = (h - hh) + l;
    ph = hh * LOG2E;
    pl = FMA(hh, LOG2E, -ph) + hh * INVLN2_L + ll * LOG2E;
    rh = dk + ph;
    rl = (dk - rh) + ph + pl;
    /* times y, and two to that */
    eh = y * rh;
    el = FMA(y, rh, -eh) + y * rl;
    if (eh > 128.5f)
        return neg ? -MCD_INF : MCD_INF;
    if (eh < -151.0f)
        return neg ? -0.0f : 0.0f;
    n = RINT(eh);
    r = ((eh - n) + el) * LN2;
    r = mcd_scalef(1.0f + (r + r * r * EXP_P(r)), (int)n);
    return neg ? -r : r;
}

/* ---- circular functions ---- */

/* |x| >= 2^17, finite: the same from the bits of 2 / pi that matter for x */
/* (the two results of a reduction come back in one register: the quadrant above the bits of the remainder) */
#define MCD_QR(q, r) ((unsigned long)(unsigned)(q) << 32 | F2U(r))
#define MCD_Q(v) ((unsigned)((v) >> 32))
#define MCD_R(v) U2F((unsigned)(v))

MCD unsigned long mcd_rempio2f_large(float ax)
{
    unsigned ix = F2U(ax), m = (ix & 0x007fffffU) | 0x00800000U, w0, w1, w2, a0, a1, a2, a3;
    int e = (int)(ix >> 23) - 150, b = e + 30, j = b >> 5, sh = b & 31, q;
    unsigned long long p0, p1, p2, lo, hi, fr;
    float f;
    /* ax = m 2^e; the 96 bits of 2/pi from the one worth 2 after scaling by 2^e */
    a0 = j == 0 ? 0U : j == 1 ? 0xA2F9836EU : j == 2 ? 0x4E441529U : j == 3 ? 0xFC2757D1U : 0xF534DDC0U;
    a1 = j == 0 ? 0xA2F9836EU : j == 1 ? 0x4E441529U : j == 2 ? 0xFC2757D1U : j == 3 ? 0xF534DDC0U : 0xDB629599U;
    a2 = j == 0 ? 0x4E441529U : j == 1 ? 0xFC2757D1U : j == 2 ? 0xF534DDC0U : j == 3 ? 0xDB629599U : 0x3C439041U;
    a3 = j == 0 ? 0xFC2757D1U : j == 1 ? 0xF534DDC0U : j == 2 ? 0xDB629599U : j == 3 ? 0x3C439041U : 0xFE5163ABU;
    if (sh) {
        w0 = a0 << sh | a1 >> (32 - sh);
        w1 = a1 << sh | a2 >> (32 - sh);
        w2 = a2 << sh | a3 >> (32 - sh);
    } else {
        w0 = a0;
        w1 = a1;
        w2 = a2;
    }
    p0 = (unsigned long long)m * w0;
    p1 = (unsigned long long)m * w1;
    p2 = (unsigned long long)m * w2;
    lo = p2 + (p1 << 32);
    hi = p0 + (p1 >> 32) + (lo < p2);
    /* the product's bits 94 and 95 are the quadrant, those below the fraction of one */
    q = (int)(hi >> 30) & 3;
    fr = hi << 34 | lo >> 30;
    if (fr >> 63)
        q = (q + 1) & 3;
    f = (float)(long long)fr * 0x1p-64f;
    return MCD_QR(q, FMA(f, PIO2_H, f * PIO2_L));
}

/* x finite: x = n pi/2 + r with |r| <= pi/4, and n mod 4 */
MCD unsigned long mcd_rempio2f(float x)
{
    float ax = FABS(x), n, t;
    unsigned long v;
    unsigned q;
    if (ax < 0x1p17f) {
        float j = FMA(x, 0x1.45f306p-1f, 0x1.8p23f); /* (the sum is a whole number: its low bits are it) */
        n = j - 0x1.8p23f;
        t = FMA(n, -0x1.921fbp+0f, x);
        t = FMA(n, -0x1.5110b4p-22f, t);
        return MCD_QR(F2U(j) & 3, FMA(n, -0x1.84698ap-48f, t));
    }
    v = mcd_rempio2f_large(ax);
    q = MCD_Q(v);
    t = MCD_R(v);
    q = x < 0.0f ? (4 - q) & 3 : q;
    return MCD_QR(q, x < 0.0f ? -t : t);
}

MCD float mcd_ksinf(float r)
{
    float z = r * r;
    return r + r * z * SIN_P(z);
}

MCD float mcd_kcosf(float r)
{
    float z = r * r, hz = 0.5f * z, w = 1.0f - hz;
    return w + (((1.0f - w) - hz) + z * z * COS_P(z));
}

/* (both the sine and the cosine of the remainder, then a choice by the quadrant: no branches but the one
   for huge arguments) */
MCD float mcd_sinf(float x)
{
    unsigned long v = mcd_rempio2f(x);
    unsigned n = MCD_Q(v);
    float r = MCD_R(v), s = mcd_ksinf(r), c = mcd_kcosf(r), y = n & 1 ? c : s;
    y = U2F(F2U(y) ^ (n & 2) << 30);
    return FABS(x) < MCD_INF ? y : x - x;
}

MCD float mcd_cosf(float x)
{
    unsigned long v = mcd_rempio2f(x);
    unsigned n = MCD_Q(v);
    float r = MCD_R(v), s = mcd_ksinf(r), c = mcd_kcosf(r), y = n & 1 ? s : c;
    y = U2F(F2U(y) ^ ((n + 1) & 2) << 30);
    return FABS(x) < MCD_INF ? y : x - x;
}

MCD void mcd_sincosf(float x, float *sp, float *cp)
{
    unsigned long v = mcd_rempio2f(x);
    unsigned n = MCD_Q(v);
    float r = MCD_R(v), s = mcd_ksinf(r), c = mcd_kcosf(r), y = n & 1 ? c : s, z = n & 1 ? s : c;
    y = U2F(F2U(y) ^ (n & 2) << 30);
    z = U2F(F2U(z) ^ ((n + 1) & 2) << 30);
    *sp = FABS(x) < MCD_INF ? y : x - x;
    *cp = FABS(x) < MCD_INF ? z : x - x;
}

MCD float mcd_tanf(float x)
{
    unsigned long v = mcd_rempio2f(x);
    float r = MCD_R(v), s = mcd_ksinf(r), c = mcd_kcosf(r), y = MCD_Q(v) & 1 ? -c / s : s / c;
    return FABS(x) < MCD_INF ? y : x - x;
}

/* x = n / 2 + r exactly, |r| <= 1/4; pi r in two parts; n mod 4. x is finite and below 2^23 in size. */
MCD int mcd_rempif(float x, float *th, float *tl)
{
    float n = RINT(x + x), r = x - 0.5f * n, h = r * PI_H;
    *th = h;
    *tl = FMA(r, PI_H, -h) + r * PI_L;
    return (int)n & 3;
}

MCD void mcd_sincospif(float x, float *sp, float *cp)
{
    float ax = FABS(x), h, l, s, c;
    int n;
    if (!(ax < 0x1p23f)) { /* an integer, or not a number */
        if (!(ax < MCD_INF)) {
            *sp = *cp = x - x;
            return;
        }
        *sp = U2F(F2U(x) & 0x80000000U);
        *cp = ax < 0x1p24f && ((int)ax & 1) ? -1.0f : 1.0f;
        return;
    }
    n = mcd_rempif(x, &h, &l);
    s = mcd_ksinf(h) + l * (1.0f - 0.5f * h * h);
    c = mcd_kcosf(h) - h * l;
    *sp = n == 0 ? s : n == 1 ? c : n == 2 ? 0.0f - s : 0.0f - c;
    *cp = n == 0 ? c : n == 1 ? 0.0f - s : n == 2 ? 0.0f - c : s;
    if (n == 0 && s == 0.0f)
        *sp = U2F(F2U(x) & 0x80000000U); /* (the sign of a zero) */
}

MCD float mcd_sinpif(float x)
{
    float s, c;
    mcd_sincospif(x, &s, &c);
    return s;
}

MCD float mcd_cospif(float x)
{
    float s, c;
    mcd_sincospif(x, &s, &c);
    return c;
}

/* atan of t in [0, 1] */
/* the arc tangent of t in [0, 1] */
MCD float mcd_katanf(float t)
{
    float z = t * t;
    return FMA(t * z, ATAN1_P(z), t);
}

MCD float mcd_atanf(float x)
{
    float ax = FABS(x), inv = 1.0f / ax, t = ax > 1.0f ? inv : ax, r = mcd_katanf(t), r2 = (PIO2_L - r) + PIO2_H;
    r = ax > 1.0f ? r2 : r;
    return U2F(F2U(r) | (F2U(x) & 0x80000000U));
}

MCD float mcd_atan2f(float y, float x)
{
    float ax = FABS(x), ay = FABS(y), mx = FMAX(ax, ay), mn = FMIN(ax, ay), t = mn / mx, r, r2, nan = x + y;
    t = mx == 0.0f ? 0.0f : t;
    t = mn == MCD_INF ? 1.0f : t;
    r = mcd_katanf(t);
    r2 = (PIO2_L - r) + PIO2_H;
    r = ay > ax ? r2 : r;
    r2 = (PI_L - r) + PI_H;
    r = F2U(x) >> 31 ? r2 : r;
    r = U2F(F2U(r) | (F2U(y) & 0x80000000U));
    return x != x || y != y ? nan : r;
}

MCD float mcd_asinf(float x)
{
    float ax = FABS(x), z, s, c, r;
    if (!(ax <= 1.0f))
        return MCD_NAN;
    if (ax < 0.5f) {
        z = x * x;
        return x + x * z * ASIN_P(z);
    }
    if (ax == 1.0f)
        return x * PIO2_H;
    z = (1.0f - ax) * 0.5f;
    s = SQRT(z);
    c = FMA(-s, s, z) / (s + s); /* sqrt z = s + c */
    r = (PIO2_H - 2.0f * s) - (2.0f * (c + s * z * ASIN_P(z)) - PIO2_L);
    return U2F(F2U(r) | (F2U(x) & 0x80000000U));
}

MCD float mcd_acosf(float x)
{
    float ax = FABS(x), z, s, c;
    if (!(ax <= 1.0f))
        return MCD_NAN;
    if (ax < 0.5f) {
        z = x * x;
        return PIO2_H - (x - (PIO2_L - x * z * ASIN_P(z)));
    }
    if (ax == 1.0f)
        return x > 0.0f ? 0.0f : PI_H;
    z = (1.0f - ax) * 0.5f;
    s = SQRT(z);
    c = FMA(-s, s, z) / (s + s);
    if (x > 0.0f)
        return 2.0f * s + 2.0f * (c + s * z * ASIN_P(z));
    return (PI_H - 2.0f * s) - (2.0f * (c + s * z * ASIN_P(z)) - PI_L);
}

/* ---- hyperbolic functions ---- */

MCD float mcd_sinhf(float x)
{
    float ax = FABS(x), t, h = x < 0.0f ? -0.5f : 0.5f;
    if (x != x || ax < 0x1p-12f)
        return x;
    if (ax < 88.72f) {
        t = mcd_expm1f(ax);
        return h * (t + t / (t + 1.0f));
    }
    t = mcd_expf(0.5f * ax);
    return (h * t) * t;
}

MCD float mcd_coshf(float x)
{
    float ax = FABS(x), t;
    if (x != x)
        return x;
    if (ax < 0.6931f) {
        t = mcd_expm1f(ax);
        return 1.0f + t * t / (2.0f * (1.0f + t));
    }
    if (ax < 88.72f) {
        t = mcd_expf(ax);
        return 0.5f * (t + 1.0f / t);
    }
    t = mcd_expf(0.5f * ax);
    return (0.5f * t) * t;
}

MCD float mcd_tanhf(float x)
{
    float ax = FABS(x), z = x * x, small = FMA(ax * z, TANH_P(z), ax), e = mcd_expf(2.0f * FMIN(ax, 10.0f)),
          big = FMA(-2.0f, RCP(e + 1.0f), 1.0f), r = ax > 0.55f ? big : small;
    r = U2F(F2U(r) | (F2U(x) & 0x80000000U));
    return x != x ? x : r;
}

MCD float mcd_asinhf(float x)
{
    float ax = FABS(x), r;
    if (x != x || ax == MCD_INF || ax < 0x1p-12f)
        return x;
    if (ax >= 0x1p12f)
        r = mcd_logf(ax) + 0x1.62e43p-1f;
    else if (ax >= 2.0f)
        r = mcd_logf(2.0f * ax + 1.0f / (SQRT(ax * ax + 1.0f) + ax));
    else
        r = mcd_log1pf(ax + ax * ax / (SQRT(ax * ax + 1.0f) + 1.0f));
    return U2F(F2U(r) | (F2U(x) & 0x80000000U));
}

MCD float mcd_acoshf(float x)
{
    float t;
    if (!(x >= 1.0f))
        return MCD_NAN;
    if (x < 2.0f) {
        t = x - 1.0f;
        return mcd_log1pf(t + SQRT(2.0f * t + t * t));
    }
    if (x < 0x1p12f)
        return mcd_logf(2.0f * x - 1.0f / (x + SQRT(x * x - 1.0f)));
    return mcd_logf(x) + 0x1.62e43p-1f;
}

MCD float mcd_atanhf(float x)
{
    float ax = FABS(x), r;
    if (!(ax <= 1.0f))
        return MCD_NAN;
    if (ax == 1.0f)
        return x * MCD_INF;
    if (ax < 0x1p-12f)
        return x;
    if (ax < 0.5f)
        r = 0.5f * mcd_log1pf(2.0f * ax + 2.0f * ax * ax / (1.0f - ax));
    else
        r = 0.5f * mcd_log1pf(2.0f * (ax / (1.0f - ax)));
    return U2F(F2U(r) | (F2U(x) & 0x80000000U));
}

/* ---- the error function ---- */

/* exp(-x x) for x >= 0, with what the square lost */
MCD float mcd_expmsq(float x)
{
    float q = x * x, e = FMA(x, x, -q);
    return mcd_expf(-q) * (1.0f - e);
}

MCD float mcd_erff(float x)
{
    float ax = FABS(x), t = FMIN(ax, 4.0f), big, small;
    small = x * ERF_P(x * x);
    big = 1.0f - EX2(ERF_G(t)); /* 1 - erfc */
    big = U2F(F2U(big) | (F2U(x) & 0x80000000U));
    big = ax < 1.0f ? small : big;
    return x != x ? x : big;
}

MCD float mcd_erfcf(float x)
{
    float ax = FABS(x), u, w, r;
    if (x != x)
        return x;
    if (ax < 0.5f)
        return 1.0f - x * ERF_P(x * x);
    if (ax < 1.0f)
        r = ERFC_C(ax - 0.75f);
    else if (ax < 10.1f) {
        u = 1.0f / ax;
        w = ax < 4.0f ? u - 0.625f : u - 0.174f;
        r = mcd_expmsq(ax) * u * (ax < 4.0f ? ERFC_A(w) : ERFC_B(w));
    } else
        r = 0.0f;
    return x < 0.0f ? 2.0f - r : r;
}

/* ---- roots, remainders ---- */

MCD float mcd_cbrtf(float x)
{
    unsigned ix = F2U(x), ax = ix & 0x7fffffffU;
    float a, t, r, tt, d, sc = 1.0f;
    if (ax == 0 || ax >= 0x7f800000U)
        return x;
    a = U2F(ax);
    if (ax < 0x10000000U) { /* away from the ends, where a square or a cube would leave the range */
        a *= 0x1p72f;
        sc = 0x1p-24f;
    } else if (ax > 0x6f000000U) {
        a *= 0x1p-72f;
        sc = 0x1p24f;
    }
    t = U2F(F2U(a) / 3 + 709958130U); /* to five bits */
    r = t * t * t;
    t = t * ((r + (a + a)) / ((r + r) + a)); /* to fifteen */
    r = t * t * t;
    t = t * ((r + (a + a)) / ((r + r) + a));
    tt = t * t;
    d = FMA(tt, t, -a) + FMA(t, t, -tt) * t; /* t^3 - a */
    t -= d / (3.0f * tt);
    return U2F(F2U(t * sc) | (ix & 0x80000000U));
}

MCD float mcd_hypotf(float x, float y)
{
    float a = FABS(x), b = FABS(y), t;
    if (a == MCD_INF || b == MCD_INF)
        return MCD_INF;
    if (x != x || y != y)
        return x + y;
    if (a < b) {
        t = a;
        a = b;
        b = t;
    }
    if (b == 0.0f)
        return a;
    if (a > 0x1p60f) {
        a *= 0x1p-90f;
        b *= 0x1p-90f;
        return SQRT(FMA(a, a, b * b)) * 0x1p90f;
    }
    if (a < 0x1p-60f) {
        a *= 0x1p90f;
        b *= 0x1p90f;
        return SQRT(FMA(a, a, b * b)) * 0x1p-90f;
    }
    return SQRT(FMA(a, a, b * b));
}

/* x - n y, n the integer part of x / y: exact, by long division on the bits */
MCD float mcd_fmodf(float x, float y)
{
    float ax = FABS(x), ay = FABS(y), r, ys, q;
    if (!(ay > 0.0f) || !(ax < MCD_INF))
        return (x * y) / (x * y);
    if (ax < ay)
        return x;
    /* r = r - q ys, ys = ay 2^k such that the quotient is below 2^24: it is right or one too large (the
       division rounds), the product and the difference are exact in the fused operation, and r loses 22
       bits of its distance to ay each time; nearly always once, k = 0 */
    r = ax;
    do {
        ys = ay;
        if (r >= ay * 0x1p22f)
            ys = mcd_scalef(ay, mcd_ilogbf(r) - mcd_ilogbf(ay) - 22);
        q = TRUNC(r / ys);
        r = FMA(-q, ys, r);
        if (r < 0.0f)
            r += ys;
    } while (r >= ay);
    return U2F(F2U(r) | (F2U(x) & 0x80000000U));
}

/* x - n y, n the integer nearest x / y (the even one of two) */
MCD float mcd_remainderf(float x, float y)
{
    float ax = FABS(x), ay = FABS(y), r;
    int odd = 0;
    if (x != x || y != y || ax == MCD_INF || ay == 0.0f)
        return (x * y) / (x * y);
    if (ay == MCD_INF)
        return x;
    /* r = |x| mod |y|, and whether the quotient is odd; every step is exact */
    if (ay > 0x1p126f) { /* (twice it may not exist) */
        r = ax;
        if (ax >= ay) {
            r = mcd_fmodf(0.5f * ax, ay);
            if (r >= 0.5f * ay) {
                r -= 0.5f * ay;
                odd = 1;
            }
            r += r;
        }
    } else {
        r = mcd_fmodf(ax, ay + ay);
        if (r >= ay) {
            r -= ay;
            odd = 1;
        }
    }
    if (ay < 0x1p-100f ? (r + r > ay || (r + r == ay && odd)) : (r > 0.5f * ay || (r == 0.5f * ay && odd)))
        r -= ay;
    return F2U(x) >> 31 ? -r : r;
}
