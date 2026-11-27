/* The math library, part 3: the transcendental functions in double precision that C99 added.
   The method and the kernels are those of mathd.c. */
#define MATHTAB_X
#define MATH_KERNELS
#include "mathimpl.h"
#include "mathtab.h"

COUNT_IS(cbrt_poly, 6);
typedef char erf_tab_count[ERF_TAB_N == 13 ? 1 : -1];

double exp2(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l, kd;
    long e;
    if (ax >= 0x4090e00000000000UL) { /* 1080 */
        if (ax > INF64)
            return x + x;
        return x > 0 ? x * 0x1p1023 : 0.0;
    }
    if (ax < 0x3c00000000000000UL)
        return 1.0 + x;
    kd = (x * 128.0 + ROUNDER) - ROUNDER;
    h = exp_tail((long)kd, (x - kd * 0.0078125) * EXP2_C, &l, &e);
    return __math_scale(h, l, e);
}

double exp10(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l, kd, r;
    long e;
    if (ax >= 0x4074a00000000000UL) { /* 330 */
        if (ax > INF64)
            return x + x;
        return x > 0 ? x * 0x1p1023 : 0.0;
    }
    if (ax < 0x3c00000000000000UL)
        return 1.0 + x;
    kd = (x * EXP10_INVL + ROUNDER) - ROUNDER;
    r = (x - kd * EXP10_A1) - kd * EXP10_A2;
    h = exp_tail((long)kd, r * LN10_H + r * LN10_L, &l, &e);
    return __math_scale(h, l, e);
}
double expm1(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l, s, t;
    long e;
    if (ax >= 0x4043000000000000UL) { /* 38 */
        if (ax > INF64)
            return x + x;
        if (x < 0)
            return -1.0;
        if (x > 710.0)
            return x * 0x1p1023;
    } else if (ax < 0x3fc0000000000000UL) { /* 1/8 */
        if (ax < 0x3c90000000000000UL)
            return x;
        return x + (0.5 * x * x + x * x * x * poly(x, expm1_poly, 12));
    }
    h = exp_dd(x, 0.0, &l, &e);
    if (e > 60)
        return __math_scale(h, l, e);
    s = u2d((u64)(e + 1023) << 52);
    h *= s;
    l *= s;
    TWOSUM(s, t, h, -1.0);
    return s + (t + l);
}

double log2(double x)
{
    double h, l, p, pl, s, e;
    int k;
    if (log_special(x, &l))
        return l;
    h = log_dd(x, &l, &k);
    TWOPROD(p, pl, h, INVLN2_H);
    pl += h * INVLN2_L + l * INVLN2_H;
    TWOSUM(s, e, (double)k, p);
    return s + (e + pl);
}

double log1p(double x)
{
    u64 ix = d2u(x), ax = ix & ~SIGN64;
    double u, c, h, l;
    if (ax < 0x3e10000000000000UL) /* 2^-30: x - x^2/2 */
        return fma(-0.5 * x, x, x);
    if (ix >= 0xbff0000000000000UL) /* x <= -1 or a negative NaN */
        return x == -1.0 ? -D_INF : (x - x) / (x - x);
    if (ax >= INF64)
        return x + x;
    TWOSUM(u, c, 1.0, x);
    h = log_dd(u, &l, 0);
    c /= u;
    return h + (l + (c - 0.5 * c * c)); /* the square counts when u is 1 */
}

/* log(h + l), h + l normalised */
static double log_of_dd(double h, double l)
{
    double lo, r = log_dd(h, &lo, 0);
    return r + (lo + l / h);
}

double asinh(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), r, h, l, s, e, sh, sl, th, tl;
    if (ax >= INF64)
        return x + x;
    if (ax < 0x3e40000000000000UL)
        return x;
    if (a > 0x1p28) {
        h = log_dd(a, &l, 0);
        TWOSUM(s, e, h, LN2_H);
        r = s + (e + (l + LN2_L));
    } else {
        TWOPROD(h, l, a, a);
        TWOSUM(s, e, 1.0, h);
        e += l;
        sh = sqrt(s);
        sl = (fma(-sh, sh, s) + e) / (2.0 * sh);
        TWOSUM(th, tl, sh, a);
        tl += sl;
        FASTSUM(th, tl, th, tl);
        r = log_of_dd(th, tl);
    }
    return x < 0 ? -r : r;
}

double acosh(double x)
{
    double h, l, s, e, d, ph, pl, sh, sl, th, tl;
    if (!(x > 1.0))
        return x == 1.0 ? 0.0 : (x - x) / (x - x);
    if (x > 0x1p28) {
        if (x - x != 0)
            return x;
        h = log_dd(x, &l, 0);
        TWOSUM(s, e, h, LN2_H);
        return s + (e + (l + LN2_L));
    }
    d = x - 1.0;
    TWOSUM(ph, pl, x, 1.0);
    TWOPROD(h, l, d, ph);
    l += d * pl;
    sh = sqrt(h);
    sl = (fma(-sh, sh, h) + l) / (2.0 * sh);
    TWOSUM(th, tl, x, sh);
    tl += sl;
    FASTSUM(th, tl, th, tl);
    return log_of_dd(th, tl);
}

double atanh(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double a = u2d(ax), r, nh, nl, dh, dl, q, ql;
    if (ax >= ONE64) {
        if (ax == ONE64)
            return x / 0.0;
        return (x - x) / (x - x);
    }
    if (ax < 0x3e40000000000000UL)
        return x;
    TWOSUM(nh, nl, 1.0, a);
    TWOSUM(dh, dl, 1.0, -a);
    q = nh / dh;
    ql = (fma(-q, dh, nh) + (nl - q * dl)) / dh;
    FASTSUM(q, ql, q, ql);
    r = 0.5 * log_of_dd(q, ql);
    return x < 0 ? -r : r;
}

void sincos(double x, double *sp, double *cp)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h = x, l = 0.0, lo, s, c;
    int n = 0;
    if (ax <= PIO4_BITS) {
        if (ax < TINY_BITS) {
            *sp = x;
            *cp = 1.0;
            return;
        }
    } else if (ax >= INF64) {
        *sp = *cp = x - x;
        return;
    } else {
        n = __math_rem_pio2(x, &h, &l);
    }
    s = sin_k(h, l, &lo);
    c = cos_k(h, l, &lo);
    switch (n) {
    case 0: *sp = s; *cp = c; break;
    case 1: *sp = c; *cp = -s; break;
    case 2: *sp = -s; *cp = -c; break;
    default: *sp = -c; *cp = s; break;
    }
}

/* sin(pi x) and cos(pi x) (C23 sinpi / cospi; CUDA's sincospi): the reduction is exact, a = |x| - 2 floor(|x| / 2) in [0, 2) and
   f = a - n / 2 in [-1/4, 1/4] with n = the nearest multiple of 1/2, so no pi-sized rounding enters; then the kernels on pi f in
   double-double. Integers give sin +-0 (the sign of x) and cos +-1, half-integers cos +0 (IEEE 754 sinPi / cosPi). The device library
   had no double sinpi / cospi until 11-27 (mcuda_device.h's sincospi calls them: ufsm's FFT twiddle table four_tab_k) */
void __mcc_sincospi(double x, double *sp, double *cp)
{
    double a = fabs(x), r, f, h, l, lo, s, c;
    int n;
    if ((d2u(x) & ~SIGN64) >= INF64) {
        *sp = *cp = x - x;
        return;
    }
    r = a - 2.0 * floor(a * 0.5);
    n = (int)floor(2.0 * r + 0.5);
    f = r - 0.5 * n;
    TWOPROD(h, l, f, PI_H);
    l += f * PI_L;
    s = f == 0.0 ? 0.0 : sin_k(h, l, &lo);
    c = cos_k(h, l, &lo);
    switch (n & 3) {
    case 0: break;
    case 1: h = s; s = c; c = f == 0.0 ? 0.0 : -h; break;
    case 2: s = f == 0.0 ? 0.0 : -s; c = -c; break;
    default: h = s; s = -c; c = f == 0.0 ? 0.0 : h; break;
    }
    *sp = d2u(x) >> 63 ? -s : s;
    *cp = c;
}

double sinpi(double x)
{
    double s, c;
    __mcc_sincospi(x, &s, &c);
    return s;
}

double cospi(double x)
{
    double s, c;
    __mcc_sincospi(x, &s, &c);
    return c;
}

/* ================= roots ================= */

double cbrt(double x)
{
    u64 ix = d2u(x), ax = ix & ~SIGN64;
    int e = 0, q;
    double m, t, t2, t2l, t3, r;
    static const double c2[3] = {1.0, 0x1.428a2f98d728bp+0, 0x1.965fea53d6e3dp+0}; /* 2^(i/3) */
    if (ax == 0 || ax >= INF64)
        return x + x;
    if (ax < 0x0010000000000000UL) {
        ax = d2u(u2d(ax) * 0x1p54);
        e = -54;
    }
    e += (int)(ax >> 52) - 1023 + 3072;
    q = e / 3 - 1024;
    m = u2d((ax & MANT64) | ONE64);
    t = m - 1.5;
    t2 = t * t;
    t = POLY6(t, t2, cbrt_poly) * c2[e % 3];
    m = u2d((ax & MANT64) | ((u64)(1023 + e % 3) << 52));
    /* Halley's step triples the 17 good bits; then one Newton step with the residual computed exactly */
    t3 = t * t * t;
    t = t - t * (t3 - m) / (2.0 * t3 + m);
    TWOPROD(t2, t2l, t, t);
    r = fma(t2, t, -m) + t2l * t;
    t = t - r / (3.0 * t2);
    t *= u2d((u64)(q + 1023) << 52);
    return (ix >> 63) ? -t : t;
}

double hypot(double x, double y)
{
    u64 ax = d2u(x) & ~SIGN64, ay = d2u(y) & ~SIGN64, t;
    double a, b, ah, al, bh, bl, s, e, h, sc = 1.0;
    if (ax == INF64 || ay == INF64)
        return D_INF;
    if (ax > INF64 || ay > INF64)
        return x + y;
    if (ax < ay) {
        t = ax;
        ax = ay;
        ay = t;
    }
    a = u2d(ax);
    b = u2d(ay);
    if (ay == 0 || (ax >> 52) - (ay >> 52) > 60)
        return a + b;
    if (ax > 0x5fd0000000000000UL) { /* 2^510 */
        a *= 0x1p-600;
        b *= 0x1p-600;
        sc = 0x1p600;
    } else if (ay < 0x23d0000000000000UL) { /* 2^-450: the squares must keep their low halves */
        a *= 0x1p600;
        b *= 0x1p600;
        sc = 0x1p-600;
    }
    TWOPROD(ah, al, a, a);
    TWOPROD(bh, bl, b, b);
    TWOSUM(s, e, ah, bh);
    e += al + bl;
    h = sqrt(s);
    e = (fma(-h, h, s) + e) / (2.0 * h);
    s = h + e;
    if (sc < 1.0 && s < 0x1p-422) /* a subnormal result */
        return round_sub(s * 0x1p474, ((h - s) + e) * 0x1p474);
    return s * sc;
}

/* ================= error function ================= */

/* The tables cut [2^k, 2^(k+1)) into 8 pieces and hold, for each, the value at the centre as hi + lo
   and the coefficients of a polynomial in the distance from the centre. */
static double piece(const double *t, int n, double d, double *lo)
{
    double d2 = d * d, p, h;
    const double *c = t + 2;
    if (n == 13)
        p = d * POLY13(d, d2, c);
    else
        p = d * poly(d, c, n);
    h = t[0] + (t[1] + p);
    *lo = (t[0] - h) + (t[1] + p);
    return h;
}
#define PIECE_INDEX(ax, first) (int)(((ax) >> 49) - ((first) >> 49))
#define PIECE_CENTRE(ax) u2d(((ax) & ~0x1ffffffffffffUL) | 0x1000000000000UL)

/* erf(x) for |x| < 1/4 as hi + lo */
static double erf_small(double x, double *lo)
{
    double z = x * x, h, l;
    TWOPROD(h, l, x, TWO_SQRTPI_H);
    l += x * TWO_SQRTPI_L + x * z * poly(z, erf_poly, sizeof erf_poly / sizeof erf_poly[0]);
    *lo = l;
    return h;
}

double erf(double x)
{
    u64 ax = d2u(x) & ~SIGN64;
    double h, l;
    if (ax < 0x3fd0000000000000UL) {
        if (ax < 0x0c70000000000000UL) { /* 2^-824: x 2/sqrt(pi), rounded once also if it is subnormal */
            if (ax == 0)
                return x;
            TWOPROD(h, l, x * 0x1p200, TWO_SQRTPI_H);
            return scale_any(h, l + x * 0x1p200 * TWO_SQRTPI_L, -200);
        }
        h = erf_small(x, &l);
        return h + l;
    }
    if (ax >= 0x4018000000000000UL) { /* 6 */
        if (ax > INF64)
            return x + x;
        return x < 0 ? -1.0 : 1.0;
    }
    h = piece(erf_tab[PIECE_INDEX(ax, 0x3fd0000000000000UL)], ERF_TAB_N, u2d(ax) - PIECE_CENTRE(ax), &l);
    return x < 0 ? -h : h;
}

double erfc(double x)
{
    u64 ix = d2u(x), ax = ix & ~SIGN64;
    double a = u2d(ax), h, l, s, e, gh, gl, eh, el, zh, zl;
    long k;
    if (ax < 0x3fd0000000000000UL) {
        if (ax < 0x3c70000000000000UL)
            return 1.0 - x;
        h = erf_small(x, &l);
        TWOSUM(s, e, 1.0, -h);
        return s + (e - l);
    }
    if (ax > INF64)
        return x + x;
    if (ix >> 63) { /* 1 + erf(|x|) */
        if (ax >= 0x4018000000000000UL)
            return 2.0;
        h = piece(erf_tab[PIECE_INDEX(ax, 0x3fd0000000000000UL)], ERF_TAB_N, a - PIECE_CENTRE(ax), &l);
        TWOSUM(s, e, 1.0, h);
        return s + (e + l);
    }
    if (ax < 0x3fe0000000000000UL) { /* 1 - erf(x) */
        h = piece(erf_tab[PIECE_INDEX(ax, 0x3fd0000000000000UL)], ERF_TAB_N, a - PIECE_CENTRE(ax), &l);
        TWOSUM(s, e, 1.0, -h);
        return s + (e - l);
    }
    if (ax >= 0x403b500000000000UL) /* 27.3125 */
        return 0x1p-537 * 0x1p-538;
    /* exp(-x^2) times the scaled function exp(x^2) erfc(x), which is smooth */
    gh = piece(erfcx_tab[PIECE_INDEX(ax, 0x3fe0000000000000UL)], ERFCX_TAB_N, a - PIECE_CENTRE(ax), &gl);
    TWOPROD(zh, zl, a, a);
    eh = exp_dd(-zh, -zl, &el, &k);
    TWOPROD(h, l, eh, gh);
    l += eh * gl + el * gh;
    s = h + l;
    l = (h - s) + l;
    return __math_scale(s * 64.0, l * 64.0, k - 6);
}

/* ================= gamma function ================= */

/* log(gamma(y)) for y = yh + yl >= 12 as hi + lo, by Stirling's series */
static double stirling(double yh, double yl, double *lo)
{
    double lh, ll, ph, pl, m = yh - 0.5, w = 1.0 / yh, wl, s, e, s2, e2, s3, e3, th, tl, v;
    lh = log_dd(yh, &ll, 0);
    TWOPROD(ph, pl, m, lh);
    pl += m * ll;
    TWOSUM(s, e, ph, -yh);
    TWOSUM(s2, e2, s, HALF_LN2PI_H);
    /* the series in 1/y: its first term, 1/(12 y), needs more than one double */
    wl = fma(-w, yh, 1.0) * w;
    TWOPROD(th, tl, w, STIRLING_C0_H);
    TWOSUM(s3, e3, s2, th);
    v = w * w;
    /* yl counts through the derivative, which is about log(y) - 1/(2y) */
    e3 += ((e + pl) + e2) + (HALF_LN2PI_L + (tl + (wl * STIRLING_C0_H + w * STIRLING_C0_L))
          + w * v * poly(v, stirling_poly, sizeof stirling_poly / sizeof stirling_poly[0])) + yl * (lh - 0.5 * w);
    s = s3 + e3;
    *lo = (s3 - s) + e3;
    return s;
}

/* For 0 < x < 12: gamma(x) = gamma(x + n) / (x (x + 1) ... (x + n - 1)) with x + n >= 12.
   Gives the product as ph + pl and x + n as yh + yl. */
static void shift_up(double xh, double xl, double *ph, double *pl, double *yh, double *yl)
{
    double h = 1.0, l = 0.0, fh, fl, t, tl, j = 0.0;
    TWOSUM(fh, fl, xh, xl);
    while (fh < 12.0) {
        TWOPROD(t, tl, h, fh);
        tl += h * fl + l * fh;
        h = t + tl;
        l = (t - h) + tl;
        j += 1.0;
        TWOSUM(fh, fl, xh, j);
        fl += xl;
    }
    *ph = h;
    *pl = l;
    *yh = fh;
    *yl = fl;
}

/* sin(pi a) for a > 0 not an integer as hi + lo; *odd tells whether floor(a) is odd */
static double sinpi_dd(double a, double *lo, int *odd)
{
    double n = floor(a), f = a - n, h, l;
    *odd = n < 0x1p53 && (n * 0.5 != floor(n * 0.5));
    if (f > 0.5)
        f = 1.0 - f;
    if (f <= 0.25) {
        TWOPROD(h, l, f, PI_H);
        l += f * PI_L;
        return sin_k(h, l, lo);
    }
    f = 0.5 - f;
    TWOPROD(h, l, f, PI_H);
    l += f * PI_L;
    return cos_k(h, l, lo);
}

/* gamma(xh + xl) = (hi + lo) 2^e for xh + xl > 0, below 250 */
static double gamma_pos(double xh, double xl, double *lo, long *pe)
{
    double ph, pl, yh, yl, h, l, eh, el, q, ql;
    if (xh >= 12.0) {
        h = stirling(xh, xl, &l);
        return exp_dd(h, l, lo, pe);
    }
    shift_up(xh, xl, &ph, &pl, &yh, &yl);
    h = stirling(yh, yl, &l);
    eh = exp_dd(h, l, &el, pe);
    q = eh / ph;
    ql = (fma(-q, ph, eh) + (el - q * pl)) / ph;
    *lo = ql;
    return q;
}

double tgamma(double x)
{
    u64 ix = d2u(x), ax = ix & ~SIGN64;
    double h, l, a, sh, sl, yh, yl, dh, dl, q, ql;
    long e;
    int odd;
    if (ax >= INF64)
        return ix == (SIGN64 | INF64) ? (x - x) / (x - x) : x + x;
    if (ax < 0x3c80000000000000UL) /* 2^-55 */
        return 1.0 / x;
    if (!(ix >> 63)) {
        if (x > 172.0)
            return x * 0x1p1023;
        h = gamma_pos(x, 0.0, &l, &e);
        return scale_any(h, l, e);
    }
    a = -x;
    if (a == floor(a))
        return (x - x) / (x - x);
    if (a > 186.0)
        return (floor(a) * 0.5 == floor(floor(a) * 0.5)) ? -0.0 : 0.0;
    /* pi / (sin(pi x) gamma(1 - x)) */
    sh = sinpi_dd(a, &sl, &odd);
    TWOSUM(yh, yl, 1.0, a);
    h = gamma_pos(yh, yl, &l, &e);
    TWOPROD(dh, dl, sh, h);
    dl += sh * l + sl * h;
    q = PI_H / dh;
    ql = (fma(-q, dh, PI_H) + (PI_L - q * dl)) / dh;
    h = scale_any(q, ql, -e);
    return odd ? h : -h;
}

/* log(gamma(xh + xl)) for xh + xl >= 1/2 or so, as hi + lo (absolute error near 2^-62) */
static double lgamma_pos(double xh, double xl, double *lo)
{
    double ph, pl, yh, yl, h, l, lh, ll, s, e;
    if (xh >= 12.0)
        return stirling(xh, xl, lo);
    shift_up(xh, xl, &ph, &pl, &yh, &yl);
    h = stirling(yh, yl, &l);
    lh = log_dd(ph, &ll, 0);
    ll += pl / ph;
    TWOSUM(s, e, h, -lh);
    e += l - ll;
    h = s + e;
    *lo = (s - h) + e;
    return h;
}

static double lgamma_near(double t, double c1h, double c1l, double c2h, double c2l, double p)
{
    double h1, l1, zh, zl, h2, l2, s, e;
    TWOPROD(h1, l1, t, c1h);
    TWOPROD(zh, zl, t, t);
    TWOPROD(h2, l2, zh, c2h);
    TWOSUM(s, e, h1, h2);
    return s + (e + (l1 + t * c1l) + (l2 + zh * c2l + zl * c2h) + t * zh * p);
}

int signgam;

double lgamma_r(double x, int *sign)
{
    u64 ix = d2u(x), ax = ix & ~SIGN64;
    double h, l, a, t, sh, sl, lh, ll, s, e;
    int odd;
    *sign = 1;
    if (ax >= INF64)
        return x * x;
    if (ax < 0x3c30000000000000UL) { /* 2^-60, zero too */
        if (ix >> 63)
            *sign = -1;
        return -log(u2d(ax));
    }
    if (!(ix >> 63)) {
        if (x >= 0x1p50) { /* x (log(x) - 1) - log(x)/2 + log(2 pi)/2; x - 1/2 is no longer exact */
            lh = log_dd(x, &ll, 0);
            TWOSUM(s, e, lh, -1.0);
            TWOPROD(h, l, x, s);
            if (h - h != 0)
                return h;
            return h + (l + x * (e + ll) + (HALF_LN2PI_H - 0.5 * lh));
        }
        /* around the zeros at 1 and 2: c1 t + c2 t^2 + t^3 P(t), the first two terms exactly */
        t = x - 1.0;
        if (t >= -0.125 && t <= 0.125)
            return lgamma_near(t, -EULER_H, -EULER_L, LGAM1_A2_H, LGAM1_A2_L,
                               poly(t, lgam1_poly, sizeof lgam1_poly / sizeof lgam1_poly[0]));
        t = x - 2.0;
        if (t >= -0.125 && t <= 0.125)
            return lgamma_near(t, ONE_M_EULER_H, ONE_M_EULER_L, LGAM2_A2_H, LGAM2_A2_L,
                               poly(t, lgam2_poly, sizeof lgam2_poly / sizeof lgam2_poly[0]));
        return lgamma_pos(x, 0.0, &l);
    }
    a = -x;
    if (a == floor(a))
        return 1.0 / 0.0;
    /* log(pi) - log|sin(pi x)| - log(gamma(1 - x)) */
    sh = sinpi_dd(a, &sl, &odd);
    if (!odd)
        *sign = -1;
    lh = log_dd(sh, &ll, 0);
    ll += sl / sh;
    TWOSUM(s, e, LNPI_H, -lh);
    e += LNPI_L - ll;
    TWOSUM(h, l, 1.0, a);
    h = lgamma_pos(h, l, &l);
    TWOSUM(s, t, s, -h);
    return s + (t + (e - l));
}

double lgamma(double x) { return lgamma_r(x, &signgam); }

/* ---- long double is double ---- */
#define L1(f) double f##l(double x) { return f(x); }
#define L2(f) double f##l(double x, double y) { return f(x, y); }
L1(exp) L1(exp2) L1(exp10) L1(expm1) L1(log) L1(log2) L1(log10) L1(log1p)
L1(sin) L1(cos) L1(tan) L1(asin) L1(acos) L1(atan) L1(sinh) L1(cosh) L1(tanh) L1(asinh) L1(acosh) L1(atanh)
L1(cbrt) L1(erf) L1(erfc) L1(tgamma) L1(lgamma) L2(pow) L2(atan2) L2(hypot)
void sincosl(double x, double *s, double *c) { sincos(x, s, c); }
double lgammal_r(double x, int *sign) { return lgamma_r(x, sign); }
