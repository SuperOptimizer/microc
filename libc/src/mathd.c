/* The math library, part 2: the transcendental functions of C89 in double precision, and the kernels
   that the others (mathx.c) are made of.

   The aim is an error below one unit in the last place everywhere, and close to half a unit for most
   functions. The means are the classical ones: table-driven argument reduction (Tang), and carrying
   the last steps of a computation as an unevaluated sum of two doubles ("hi + lo", about 100 bits)
   so that only the final rounding counts. fma() and sqrt() are single instructions behind a call.
   The tables and coefficients come from tests/ext/libm/mktab.py. */
#define MATHTAB_CORE
#define MATH_KERNELS
#include "mathimpl.h"
#include "mathtab.h"

/* c[0] + c[1] z + ... + c[n-1] z^(n-1), n >= 2; the even and the odd terms separately, to halve the dependency chain */
double poly(double z, const double *c, int n)
{
    double z2 = z * z, a, b;
    int i = n - 1;
    if (i & 1) {
        b = c[i];
        a = c[i - 1];
        i -= 2;
    } else {
        a = c[i];
        b = c[i - 1];
        i -= 2;
        a = c[i] + z2 * a;
        i--;
    }
    for (; i > 0; i -= 2) {
        b = c[i] + z2 * b;
        a = c[i - 1] + z2 * a;
    }
    return a + z * b;
}

COUNT_IS(sin_poly, 7);
COUNT_IS(cos_poly, 7);

/* x with the sign of y */
static double sign_of(double x, double y) { return u2d((d2u(x) & ~SIGN64) | (d2u(y) & SIGN64)); }

static int clz64(u64 x)
{
    int n = 0;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}

/* (nh + nl) / (dh + dl), rounded once */
double div_dd(double nh, double nl, double dh, double dl)
{
    double q = nh / dh;
    return q + (fma(-q, dh, nh) + (nl - q * dl)) / dh;
}

/* ================= exponentials ================= */

/* h + l, 0 <= h < 2^52, counts units of the smallest subnormal number: the double nearest to that.
   Adding 2^52 rounds to an integer, and the tail decides the ties. */
double round_sub(double h, double l)
{
    double w = 0x1p52 + h, err = (0x1p52 - w) + h;
    w = w + (err + l);
    return (w - 0x1p52) * 0x1p-537 * 0x1p-537;
}

/* (hi + lo) * 2^e rounded once, also when the result is subnormal; |hi| in [1/2, 4), lo its tail */
double __math_scale(double hi, double lo, long e)
{
    double s, h, l;
    if ((u64)(e + 1021) <= 2042)
        return hi * u2d((u64)(e + 1023) << 52);
    if (e > 0) {
        if (e > 1100)
            e = 1100;
        return hi * 0x1p1000 * u2d((u64)(e - 1000 + 1023) << 52);
    }
    if (e < -1080)
        e = -1080;
    if (hi < 0)
        return -__math_scale(-hi, -lo, e);
    s = u2d((u64)(e + 1074 + 1023) << 52);
    h = hi * s;
    l = lo * s;
    if (h >= 0x1p52)
        return h * 0x1p-537 * 0x1p-537;
    return round_sub(h, l);
}

/* (h + l) 2^e for any normal h: normalise and round */
double scale_any(double h, double l, long e)
{
    double t = h + l, s;
    long k = (long)((d2u(t) >> 52) & 0x7ff) - 1023;
    l = (h - t) + l;
    s = u2d((u64)(1023 - k) << 52);
    return __math_scale(t * s, l * s, e + k);
}

/* exp(r) * 2^(k/128) = (hi + lo) * 2^*pe with a relative error of 2^-60; |r| < 0.003 */
double exp_tail(long k, double r, double *lo, long *pe)
{
    const double *t = __math_exp_tab[k & 127];
    double p = r + r * r * (0.5 + r * (0x1.5555555555555p-3 + r * (0x1.5555555555555p-5
                   + r * (0x1.1111111111111p-7 + r * 0x1.6c16c16c16c17p-10))));
    double w = t[0] * p + t[1], h = t[0] + w;
    *lo = (t[0] - h) + w;
    *pe = k >> 7;
    return h;
}

/* exp(xh + xl), |xh| < 1000 */
double exp_dd(double xh, double xl, double *lo, long *pe)
{
    double kd = (xh * EXP_INVL + ROUNDER) - ROUNDER;
    double r = ((xh - kd * EXP_L1) - kd * EXP_L2) + xl;
    return exp_tail((long)kd, r, lo, pe);
}

double exp(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l;
    long e;
    if (ax >= 0x4087500000000000UL) { /* 746 */
        if (ax > INF64)
            return x + x;
        return x > 0 ? x * 0x1p1023 : 0.0;
    }
    if (ax < 0x3c90000000000000UL)
        return 1.0 + x;
    h = exp_dd(x, 0.0, &l, &e);
    return __math_scale(h, l, e);
}

/* e^a and e^-a for 1/4 <= a < 22 */
static void exp_pm(double a, double *ph, double *pl, double *mh, double *ml)
{
    double kd = (a * EXP_INVL + ROUNDER) - ROUNDER;
    double r = (a - kd * EXP_L1) - kd * EXP_L2, h, l, s;
    long k = (long)kd, e;
    h = exp_tail(k, r, &l, &e);
    s = u2d((u64)(e + 1023) << 52);
    *ph = h * s;
    *pl = l * s;
    h = exp_tail(-k, -r, &l, &e);
    s = u2d((u64)(e + 1023) << 52);
    *mh = h * s;
    *ml = l * s;
}

double sinh(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), r, ph, pl, mh, ml, d;
    long e;
    if (ax >= INF64)
        return x + x;
    if (a < 0.25) {
        if (ax < 0x3e40000000000000UL)
            return x;
        r = x * x;
        return x + x * r * poly(r, sinh_poly, 8);
    }
    if (a >= 22.0) {
        if (a > 711.0)
            return x * 0x1p1023;
        ph = exp_dd(a, 0.0, &pl, &e);
        r = __math_scale(ph, pl, e - 1);
    } else {
        exp_pm(a, &ph, &pl, &mh, &ml);
        d = ph - mh;
        r = 0.5 * (d + (((ph - d) - mh) + (pl - ml)));
    }
    return x < 0 ? -r : r;
}

double cosh(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), ph, pl, mh, ml, d;
    long e;
    if (ax >= INF64)
        return a + a;
    if (a < 0.25) {
        if (ax < 0x3e40000000000000UL)
            return 1.0;
    } else if (a >= 22.0) {
        if (a > 711.0)
            return a * 0x1p1023;
        ph = exp_dd(a, 0.0, &pl, &e);
        return __math_scale(ph, pl, e - 1);
    }
    exp_pm(a, &ph, &pl, &mh, &ml);
    d = ph + mh;
    return 0.5 * (d + (((ph - d) + mh) + (pl + ml)));
}

double tanh(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), r, ph, pl, mh, ml, d, dl, s, sl;
    if (ax > INF64)
        return x + x;
    if (a < 0.25) {
        if (ax < 0x3e40000000000000UL)
            return x;
        r = x * x;
        return x + x * r * poly(r, tanh_poly, sizeof tanh_poly / sizeof tanh_poly[0]);
    }
    if (a >= 22.0)
        return x < 0 ? -1.0 : 1.0;
    exp_pm(a, &ph, &pl, &mh, &ml);
    d = ph - mh;
    dl = ((ph - d) - mh) + (pl - ml);
    s = ph + mh;
    sl = ((ph - s) + mh) + (pl + ml);
    r = div_dd(d, dl, s, sl);
    return x < 0 ? -r : r;
}

/* ================= logarithms ================= */

/* log(x) for finite x > 0 as hi + lo with a relative error near 2^-68.
   x = 2^k z with z in [0.70, 1.41); z = (1 + r) / invc with invc from the table, r computed exactly.
   With kp the result is log(z) and *kp = k. */
double log_dd(double x, double *lo, int *kp)
{
    u64 ix = d2u(x), tmp, iz;
    int k = 0;
    const double *t;
    double zh, zl, a, b, rh, rl, sh, sl, q, kd, t1, s1, e1, s2, e2, l;
    if (ix < 0x0010000000000000UL) {
        ix = d2u(x * 0x1p52);
        k = -52;
    }
    tmp = ix - LOG_OFF;
    k += (int)((i64)tmp >> 52);
    iz = ix - (tmp & 0xfff0000000000000UL);
    t = __math_log_tab[(tmp >> 45) & 127];
    zh = u2d(iz & ~0x7ffffffUL);
    zl = u2d(iz) - zh;
    a = zh * t[0] - 1.0; /* both products are exact: invc has 21 bits */
    b = zl * t[0];
    TWOSUM(rh, rl, a, b);
    TWOPROD(sh, sl, rh, rh);
    sl += 2.0 * rh * rl;
    q = rh * sh * POLY7(rh, sh, log_poly);
    if (kp) {
        *kp = k;
        kd = 0.0;
    } else {
        kd = (double)k;
    }
    t1 = kd * LN2_H42 + t[1]; /* exact */
    TWOSUM(s1, e1, t1, rh);
    TWOSUM(s2, e2, s1, -0.5 * sh);
    l = (((e1 + e2) + rl) - 0.5 * sl) + q + (kd * LN2_L42 + t[2]);
    s1 = s2 + l;
    *lo = (s2 - s1) + l;
    return s1;
}

/* 0 if x is an ordinary positive number, otherwise the result of a logarithm in *r */
int log_special(double x, double *r)
{
    u64 ix = d2u(x);
    if (ix - 0x0010000000000000UL < 0x7fe0000000000000UL)
        return 0;
    if ((ix << 1) == 0)
        *r = -1.0 / (x * x);
    else if (ix == INF64)
        *r = x;
    else if (ix > INF64)
        *r = (x - x) / (x - x);
    else
        return 0; /* subnormal */
    return 1;
}

double log(double x)
{
    double l;
    if (log_special(x, &l))
        return l;
    return log_dd(x, &l, 0);
}

double log10(double x)
{
    double h, l, p, pl, s, e;
    int k;
    if (log_special(x, &l))
        return l;
    h = log_dd(x, &l, &k);
    TWOPROD(p, pl, h, INVLN10_H);
    pl += h * INVLN10_L + l * INVLN10_H;
    TWOSUM(s, e, (double)k * LOG10_2_H42, p);
    return s + (e + (pl + (double)k * LOG10_2_L42));
}

/* 0 none, 1 odd integer, 2 even integer */
static int int_kind(u64 iy)
{
    int e = (int)((iy >> 52) & 0x7ff);
    if (e < 0x3ff)
        return 0;
    if (e > 0x3ff + 52)
        return 2;
    if (iy & ((1UL << (0x3ff + 52 - e)) - 1))
        return 0;
    return (iy & (1UL << (0x3ff + 52 - e))) ? 1 : 2;
}

double pow(double x, double y)
{
    u64 ix = d2u(x), iy = d2u(y), ax = ix & ~SIGN64, ay = iy & ~SIGN64;
    int kind = 0, neg = 0;
    double lh, ll, ph, pl, h, l, r;
    long e;
    if (ay == 0 || ix == ONE64)
        return 1.0;
    if (ax > INF64 || ay > INF64)
        return x + y;
    if (ay == INF64) {
        if (ax == ONE64)
            return 1.0;
        return (ax < ONE64) == !(iy >> 63) ? 0.0 : D_INF;
    }
    if (ix >> 63)
        kind = int_kind(iy);
    if (ax == 0 || ax == INF64) {
        r = (ax == 0) == !(iy >> 63) ? 0.0 : D_INF;
        if (ax == 0 && (iy >> 63))
            r = 1.0 / u2d(ax); /* division by zero */
        return kind == 1 ? -r : r;
    }
    if (ix >> 63) {
        if (kind == 0)
            return (x - x) / (x - x);
        neg = kind == 1;
    }
    lh = log_dd(u2d(ax), &ll, 0);
    TWOPROD(ph, pl, y, lh);
    if (!(ph > -1000.0 && ph < 1000.0)) {
        r = ph > 0 ? 0x1p1023 * 0x1p1023 : 0x1p-537 * 0x1p-538;
        return neg ? -r : r;
    }
    pl += y * ll;
    h = exp_dd(ph, pl, &l, &e);
    r = __math_scale(h, l, e);
    return neg ? -r : r;
}

/* ================= trigonometric ================= */

/* 64 x 64 -> 128 */
static u64 mulhl(u64 a, u64 b, u64 *lo)
{
    u64 al = a & 0xffffffffUL, ah = a >> 32, bl = b & 0xffffffffUL, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (lh & 0xffffffffUL) + (hl & 0xffffffffUL);
    *lo = (mid << 32) | (ll & 0xffffffffUL);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

/* Payne and Hanek: x = m 2^e with m an integer, and the bits of 2/pi of weight 2^(2-e) and more
   only add multiples of 4 to x 2/pi, so 192 bits from there on give the quadrant and 190 bits of fraction. */
static int rem_pio2_large(double x, double *rh, double *rl)
{
    u64 ix = d2u(x), m = (ix & MANT64) | (1UL << 52);
    int e = (int)(ix >> 52) - 1075, o = e + 62, w = o >> 6, s = o & 63, n, neg = 0, sh = 0, lz;
    const u64 *t = twoopi_bits + w;
    u64 g0 = t[0], g1 = t[1], g2 = t[2], p0, p1, p2, h1, h2, f0, f1, f2, a, b;
    double sc, fh, fl, h, l;
    if (s) {
        g0 = g0 << s | g1 >> (64 - s);
        g1 = g1 << s | g2 >> (64 - s);
        g2 = g2 << s | t[3] >> (64 - s);
    }
    h2 = mulhl(m, g2, &p2);
    h1 = mulhl(m, g1, &p1);
    p1 += h2;
    if (p1 < h2)
        h1++;
    p0 = m * g0 + h1;
    n = (int)(p0 >> 62);
    f0 = p0 << 2 | p1 >> 62;
    f1 = p1 << 2 | p2 >> 62;
    f2 = p2 << 2;
    if (f0 >> 63) { /* fraction above 1/2: the next quadrant, negative remainder */
        n++;
        neg = 1;
        f0 = ~f0 + (f1 == 0 && f2 == 0);
        f1 = ~f1 + (f2 == 0);
        f2 = -f2;
    }
    if (f0 == 0) {
        f0 = f1;
        f1 = f2;
        f2 = 0;
        sh = 64;
    }
    lz = clz64(f0);
    if (lz) {
        f0 = f0 << lz | f1 >> (64 - lz);
        f1 = f1 << lz | f2 >> (64 - lz);
    }
    a = f0 >> 11;
    b = ((f0 & 0x7ff) << 42) | (f1 >> 22);
    sc = u2d((u64)(1023 - 53 - lz - sh) << 52);
    fh = (double)a * sc;
    fl = (double)b * sc * 0x1p-53;
    TWOPROD(h, l, fh, PIO2_H);
    l += fh * PIO2_L + fl * PIO2_H;
    if (neg) {
        h = -h;
        l = -l;
    }
    *rh = h + l;
    *rl = (h - *rh) + l;
    return n;
}

/* x = n pi/2 + (rh + rl), |rh + rl| <= pi/4 (about); returns n mod 4. For finite |x| >= 1/2. */
int __math_rem_pio2(double x, double *rh, double *rl)
{
    u64 ix = d2u(x);
    double ax = u2d(ix & ~SIGN64), h, l;
    int n;
    if (ax < 0x1p20) {
        double kd = (ax * INVPIO2 + ROUNDER) - ROUNDER;
        double a = ax - kd * PIO2_1; /* exact */
        double s, e;
        TWOSUM(s, e, a, -(kd * PIO2_2));
        l = e - kd * PIO2_3;
        h = s + l;
        l = (s - h) + l;
        n = (int)kd;
        if (h < 0x1p-30 && h > -0x1p-30) /* too much cancellation for three parts of pi/2 */
            n = rem_pio2_large(ax, &h, &l);
    } else {
        n = rem_pio2_large(ax, &h, &l);
    }
    if (ix >> 63) {
        h = -h;
        l = -l;
        n = -n;
    }
    *rh = h;
    *rl = l;
    return n & 3;
}

/* sin(h + l) and cos(h + l) for |h| <= pi/4 (and a little more), as a sum of two.
   Above 1/4 the correction to h in h + h^3 P(h^2) is too large to come out of plain arithmetic precisely enough;
   there sin(a + d) = sin(a) cos(d) + cos(a) sin(d) with a = i/64 from a table and |d| <= 1/128. */
double sin_k(double h, double l, double *lo)
{
    double z = h * h, z2, c, r, d, sd, cd;
    const double *t;
    int i, neg = 0;
    if (z < 0.0625) {
        z2 = z * z;
        c = z * h * POLY7(z, z2, sin_poly) + l * (1.0 - 0.5 * z);
        r = h + c;
        *lo = (h - r) + c;
        return r;
    }
    if (h < 0) {
        h = -h;
        l = -l;
        neg = 1;
    }
    i = (int)(h * 64.0 + 0.5);
    t = sin_tab[i - 16];
    d = h - (double)i * 0.015625; /* exact */
    z = d * d;
    sd = (d + d * z * (-0x1.5555555555555p-3 + z * (0x1.1111111111111p-7 + z * -0x1.a01a01a01a01ap-13))) + l; /* sin(d + l) */
    cd = z * (-0.5 + z * (0x1.5555555555555p-5 + z * -0x1.6c16c16c16c17p-10)); /* cos(d) - 1 */
    c = t[2] * sd + (t[1] + (t[3] * sd + t[0] * cd));
    r = t[0] + c;
    c = (t[0] - r) + c;
    *lo = neg ? -c : c;
    return neg ? -r : r;
}

double cos_k(double h, double l, double *lo)
{
    double zh, zl, z2, hz, w, c, r;
    TWOPROD(zh, zl, h, h);
    hz = 0.5 * zh;
    w = 1.0 - hz;
    z2 = zh * zh;
    c = (((1.0 - w) - hz) - 0.5 * zl) + (z2 * POLY7(zh, z2, cos_poly) - h * l);
    r = w + c;
    *lo = (w - r) + c;
    return r;
}

double sin(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l, lo;
    if (ax <= PIO4_BITS)
        return ax < TINY_BITS ? x : sin_k(x, 0.0, &lo);
    if (ax >= INF64)
        return x - x;
    switch (__math_rem_pio2(x, &h, &l)) {
    case 0: return sin_k(h, l, &lo);
    case 1: return cos_k(h, l, &lo);
    case 2: return -sin_k(h, l, &lo);
    default: return -cos_k(h, l, &lo);
    }
}

double cos(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l, lo;
    if (ax <= PIO4_BITS)
        return ax < TINY_BITS ? 1.0 : cos_k(x, 0.0, &lo);
    if (ax >= INF64)
        return x - x;
    switch (__math_rem_pio2(x, &h, &l)) {
    case 0: return cos_k(h, l, &lo);
    case 1: return -sin_k(h, l, &lo);
    case 2: return -cos_k(h, l, &lo);
    default: return sin_k(h, l, &lo);
    }
}

double tan(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h = x, l = 0.0, sh, sl, ch, cl;
    int n = 0;
    if (ax <= PIO4_BITS) {
        if (ax < TINY_BITS)
            return x;
    } else if (ax >= INF64) {
        return x - x;
    } else {
        n = __math_rem_pio2(x, &h, &l);
    }
    sh = sin_k(h, l, &sl);
    ch = cos_k(h, l, &cl);
    return (n & 1) ? -div_dd(ch, cl, sh, sl) : div_dd(sh, sl, ch, cl);
}

/* atan(th + tl) for 0 <= t <= 1 as hi + lo: atan(t) = atan(c) + atan((t - c) / (1 + t c)), c = i/64 */
static double atan_dd(double th, double tl, double *lo)
{
    int i = (int)(th * 64.0 + 0.5);
    double c, nh, p, pe, dh, dl, q, ql, z, s, h, l;
    if (i == 0) {
        q = th;
        ql = tl;
    } else {
        c = (double)i * 0.015625;
        nh = th - c; /* exact */
        TWOPROD(p, pe, th, c);
        dh = 1.0 + p;
        dl = ((1.0 - dh) + p) + (pe + tl * c);
        q = nh / dh;
        ql = (fma(-q, dh, nh) + (tl - q * dl)) / dh;
    }
    z = q * q;
    s = q * z * (-0x1.5555555555555p-2 + z * (0x1.999999999999ap-3 + z * (-0x1.2492492492492p-3 + z * 0x1.c71c71c71c71cp-4)));
    if (i == 0) {
        h = q;
        l = ql + s;
    } else {
        const double *t = __math_atan_tab[i];
        h = t[0] + q;
        l = ((t[0] - h) + q) + (t[1] + ql + s);
    }
    s = h + l;
    *lo = (h - s) + l;
    return s;
}

double atan(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), h, l, t, s;
    if (ax < TINY_BITS)
        return x;
    if (ax > INF64)
        return x + x;
    if (a <= 1.0) {
        h = atan_dd(a, 0.0, &l);
    } else if (ax >= 0x4350000000000000UL) { /* 2^54 */
        h = PIO2_H;
    } else {
        t = 1.0 / a;
        h = atan_dd(t, fma(-t, a, 1.0) * t, &l);
        s = PIO2_H - h;
        h = s + (((PIO2_H - s) - h) + (PIO2_L - l));
    }
    return x < 0 ? -h : h;
}

double atan2(double y, double x)
{
    u64 ix = d2u(x), iy = d2u(y), ax = ix & ~SIGN64, ay = iy & ~SIGN64;
    double a = u2d(ax), b = u2d(ay), t, tl, h, l, s, e, r;
    if (ax > INF64 || ay > INF64)
        return x + y;
    if (ay == 0)
        return (ix >> 63) ? sign_of(PI_H, y) : y;
    if (ax == 0)
        return sign_of(PIO2_H, y);
    if (ax == INF64 || ay == INF64) {
        if (ax != ay)
            r = ay == INF64 ? PIO2_H : (ix >> 63) ? PI_H : 0.0;
        else
            r = (ix >> 63) ? 3.0 * PIO4_H : PIO4_H;
        return sign_of(r, y);
    }
    if (b <= a) {
        t = b / a;
        if (t < 0x1p-1000) {
            r = (ix >> 63) ? PI_H : t;
            return sign_of(r, y);
        }
        if (ay < 0x0370000000000000UL) { /* keep the remainder of the division away from the subnormals */
            a *= 0x1p200;
            b *= 0x1p200;
        } else if (ax > 0x7c80000000000000UL) {
            a *= 0x1p-200;
            b *= 0x1p-200;
        }
        tl = fma(-t, a, b) / a;
        h = atan_dd(t, tl, &l);
        if (ix >> 63) { /* pi - atan */
            TWOSUM(s, e, PI_H, -h);
            h = s + (e + (PI_L - l));
        }
    } else {
        t = a / b;
        if (t < 0x1p-1000)
            return sign_of(PIO2_H, y);
        if (ax < 0x0370000000000000UL) {
            a *= 0x1p200;
            b *= 0x1p200;
        } else if (ay > 0x7c80000000000000UL) {
            a *= 0x1p-200;
            b *= 0x1p-200;
        }
        tl = fma(-t, b, a) / b;
        h = atan_dd(t, tl, &l);
        if (ix >> 63) { /* pi/2 + atan */
            TWOSUM(s, e, PIO2_H, h);
            h = s + (e + (PIO2_L + l));
        } else {
            TWOSUM(s, e, PIO2_H, -h);
            h = s + (e + (PIO2_L - l));
        }
    }
    return sign_of(h, y);
}

#define ASIN_N (int)(sizeof asin_poly / sizeof asin_poly[0])

double asin(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), z, s, sl, w, hi, lo, r;
    if (ax >= ONE64) {
        if (ax == ONE64)
            return x * PIO2_H;
        return (x - x) / (x - x);
    }
    if (a <= 0.5) {
        if (ax < TINY_BITS)
            return x;
        z = x * x;
        return fma(x, z * poly(z, asin_poly, ASIN_N), x);
    }
    /* pi/2 - 2 asin(sqrt((1 - |x|) / 2)) */
    z = (1.0 - a) * 0.5;
    s = sqrt(z);
    sl = fma(-s, s, z) / (2.0 * s);
    w = fma(s, z * poly(z, asin_poly, ASIN_N), sl);
    TWOSUM(hi, lo, PIO2_H, -2.0 * s);
    r = hi + (lo + (PIO2_L - 2.0 * w));
    return x < 0 ? -r : r;
}

double acos(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), z, s, sl, w, hi, lo;
    if (ax >= ONE64) {
        if (ax == ONE64)
            return x > 0 ? 0.0 : PI_H;
        return (x - x) / (x - x);
    }
    if (a <= 0.5) {
        z = x * x;
        w = x * z * poly(z, asin_poly, ASIN_N);
        TWOSUM(hi, lo, PIO2_H, -x);
        return hi + (lo + (PIO2_L - w));
    }
    z = (1.0 - a) * 0.5;
    s = sqrt(z);
    sl = fma(-s, s, z) / (2.0 * s);
    w = fma(s, z * poly(z, asin_poly, ASIN_N), sl);
    if (x > 0)
        return 2.0 * (s + w);
    TWOSUM(hi, lo, PI_H, -2.0 * s);
    return hi + (lo + (PI_L - 2.0 * w));
}
