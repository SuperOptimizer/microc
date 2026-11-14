/* strtod by exact decimal arithmetic: the digits are kept as a decimal
   number and shifted by powers of two until the binary mantissa can be
   read off, so the result is correctly rounded. */
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAXDIG 800

typedef struct Dec {
    int n;          /* number of digits */
    int dp;         /* value = 0.d0 d1 d2 ... * 10^dp */
    int sticky;     /* non-zero digits were dropped */
    unsigned char d[MAXDIG];
} Dec;

static void trim(Dec *a)
{
    while (a->n > 0 && a->d[a->n - 1] == 0)
        a->n--;
    if (a->n == 0)
        a->dp = 0;
}

/* a /= 2^k, k <= 28 */
static void shift_right(Dec *a, int k)
{
    unsigned long acc = 0, mask = (1UL << k) - 1;
    int r = 0, w = 0;
    /* pick up enough leading digits to have a non-zero quotient digit */
    for (; acc >> k == 0; r++) {
        if (r >= a->n) {
            if (acc == 0) {
                a->n = 0;
                return;
            }
            while (acc >> k == 0) {
                acc *= 10;
                r++;
            }
            break;
        }
        acc = acc * 10 + a->d[r];
    }
    a->dp -= r - 1;
    for (; r < a->n; r++) {
        a->d[w++] = (unsigned char)(acc >> k);
        acc = (acc & mask) * 10 + a->d[r];
    }
    while (acc) {
        unsigned digit = (unsigned)(acc >> k);
        acc = (acc & mask) * 10;
        if (w < MAXDIG)
            a->d[w++] = (unsigned char)digit;
        else if (digit)
            a->sticky = 1;
    }
    a->n = w;
    trim(a);
}

/* a *= 2^k, k <= 28 */
static void shift_left(Dec *a, int k)
{
    unsigned char out[MAXDIG + 12];
    unsigned long acc = 0;
    int r, extra = 0, w, total;
    /* multiply from the least significant digit, collecting into out reversed */
    w = 0;
    for (r = a->n - 1; r >= 0; r--) {
        acc += (unsigned long)a->d[r] << k;
        out[w++] = (unsigned char)(acc % 10);
        acc /= 10;
    }
    while (acc) {
        out[w++] = (unsigned char)(acc % 10);
        acc /= 10;
        extra++;
    }
    total = w;
    for (r = 0; r < total; r++) {
        unsigned char digit = out[total - 1 - r];
        if (r < MAXDIG)
            a->d[r] = digit;
        else if (digit)
            a->sticky = 1;
    }
    a->n = total < MAXDIG ? total : MAXDIG;
    a->dp += extra;
    trim(a);
}

/* Round a to an integer (half to even) and return it; a < 2^63. */
static unsigned long round_int(const Dec *a)
{
    unsigned long v = 0;
    int i, half;
    if (a->dp < 0)
        return 0;
    for (i = 0; i < a->dp; i++)
        v = v * 10 + (i < a->n ? a->d[i] : 0);
    if (a->dp >= a->n)
        return v;
    half = a->d[a->dp];
    if (half > 5)
        return v + 1;
    if (half < 5)
        return v;
    for (i = a->dp + 1; i < a->n; i++)
        if (a->d[i])
            return v + 1;
    if (a->sticky)
        return v + 1;
    return v + (v & 1);
}

static double make_double(unsigned long mant, int exp2, int neg)
{
    /* mant is the 53-bit significand (bit 52 set unless subnormal),
       exp2 the unbiased exponent of bit 52 */
    unsigned long bits;
    double d;
    if (mant == 0)
        bits = 0;
    else if (!(mant >> 52))
        bits = mant; /* subnormal */
    else
        bits = ((unsigned long)(exp2 + 1023) << 52) | (mant & 0xfffffffffffffUL);
    if (neg)
        bits |= 1UL << 63;
    memcpy(&d, &bits, 8);
    return d;
}

static double dec_to_double(Dec *a, int neg)
{
    int exp2 = 0;
    unsigned long mant;
    trim(a);
    if (a->n == 0)
        return make_double(0, 0, neg);
    if (a->dp > 310)
        goto overflow;
    if (a->dp < -330)
        return make_double(0, 0, neg);
    /* scale into [0.5, 1) */
    while (a->dp > 0) {
        int k = a->dp > 8 ? 28 : a->dp > 4 ? 13 : a->dp > 1 ? 3 : 1;
        shift_right(a, k);
        exp2 += k;
    }
    while (a->dp < 0 || (a->dp == 0 && a->d[0] < 5)) {
        int k = a->dp < -8 ? 28 : a->dp < -4 ? 13 : a->dp < -1 ? 3 : 1;
        shift_left(a, k);
        exp2 -= k;
    }
    /* value = a * 2^exp2 with a in [0.5, 1): as 1.xxx * 2^(exp2-1) */
    exp2--;
    if (exp2 > 1023)
        goto overflow;
    if (exp2 < -1022) {
        /* subnormal: fewer mantissa bits */
        int lose = -1022 - exp2;
        if (lose > 60)
            return make_double(0, 0, neg);
        while (lose > 0) {
            int k = lose > 28 ? 28 : lose;
            shift_right(a, k);
            lose -= k;
        }
        exp2 = -1022;
    }
    shift_left(a, 28);
    shift_left(a, 25); /* times 2^53 */
    mant = round_int(a);
    if (mant >> 53) {
        mant >>= 1;
        exp2++;
        if (exp2 > 1023)
            goto overflow;
    }
    if (exp2 == -1022 && !(mant >> 52))
        return make_double(mant, 0, neg);
    return make_double(mant, exp2, neg);
overflow:
    errno = ERANGE;
    {
        unsigned long bits = 0x7ffUL << 52;
        double d;
        if (neg)
            bits |= 1UL << 63;
        memcpy(&d, &bits, 8);
        return d;
    }
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int hex_digit(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10 : -1;
}

/* What strtod takes besides decimal numbers: "inf", "infinity", "nan", "nan(chars)" and hexadecimal
   0xh.hhhp[+-]d, all without regard to case. p is past the sign. Returns 0 if there is none of them here. */
static int special(const char *p, int neg, double *out, char **end)
{
    static const char *const names[] = { "infinity", "inf", "nan" };
    double d;
    int i, k;
    for (i = 0; i < 3; i++) {
        for (k = 0; names[i][k] && lower(p[k]) == names[i][k]; k++)
            ;
        if (names[i][k])
            continue;
        p += k;
        if (i < 2) {
            d = 1e300 * 1e300;
        } else {
            d = 1e300 * 1e300;
            d = d - d;
            if (*p == '(') {
                for (k = 1; p[k] == '_' || (p[k] >= '0' && p[k] <= '9') || (lower(p[k]) >= 'a' && lower(p[k]) <= 'z'); k++)
                    ;
                if (p[k] == ')')
                    p += k + 1;
            }
        }
        *out = neg ? -d : d;
        *end = (char *)p;
        return 1;
    }
    if (p[0] == '0' && lower(p[1]) == 'x' && (hex_digit(p[2]) >= 0 || (p[2] == '.' && hex_digit(p[3]) >= 0))) {
        unsigned long m = 0;
        long e = 0;
        int sticky = 0, seen_point = 0, drop, top;
        for (p += 2;; p++) {
            int h = hex_digit(*p);
            if (*p == '.' && !seen_point) {
                seen_point = 1;
                continue;
            }
            if (h < 0)
                break;
            if (m >> 60) { /* full: later digits only say whether anything is left */
                sticky |= h != 0;
                if (!seen_point)
                    e += 4;
            } else {
                m = m << 4 | (unsigned long)h;
                if (seen_point)
                    e -= 4;
            }
        }
        if (lower(*p) == 'p') {
            const char *q = p + 1;
            int eneg = 0;
            long x = 0;
            if (*q == '-' || *q == '+')
                eneg = *q++ == '-';
            if (*q >= '0' && *q <= '9') {
                for (; *q >= '0' && *q <= '9'; q++)
                    if (x < 100000)
                        x = x * 10 + (*q - '0');
                e += eneg ? -x : x;
                p = q;
            }
        }
        *end = (char *)p;
        if (m == 0) {
            *out = neg ? -0.0 : 0.0;
            return 1;
        }
        for (top = 63; !(m >> top); top--)
            ;
        m <<= 63 - top;
        e += top; /* the value is m / 2^63 * 2^e, with 1 <= m / 2^63 < 2 */
        if (e > 1023) {
            errno = ERANGE;
            d = 1e300 * 1e300;
        } else {
            drop = e >= -1022 ? 11 : 11 + (int)(-1022 - e < 60 ? -1022 - e : 60);
            if (drop >= 64) {
                sticky = 1;
                m = 0;
            } else {
                unsigned long rest = m & ((1UL << drop) - 1), half = 1UL << (drop - 1);
                m >>= drop;
                if (rest > half || (rest == half && (sticky || (m & 1))))
                    m++;
            }
            d = ldexp((double)m, (int)e - 63 + drop); /* exact: m is on the grid of the result */
            if (d == 0.0 || e < -1022)
                errno = ERANGE;
        }
        *out = neg ? -d : d;
        return 1;
    }
    return 0;
}

double strtod(const char *s, char **end)
{
    static const double pow10[] = { 1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    const char *p = s, *digits_start;
    Dec a;
    int neg = 0, seen_point = 0, any = 0, exp10 = 0;
    unsigned long small = 0;
    int nsmall = 0, exact = 1;

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    {
        double v;
        char *e;
        if (special(p, neg, &v, &e)) {
            if (end)
                *end = e;
            return v;
        }
    }
    a.n = 0;
    a.dp = 0;
    a.sticky = 0;
    digits_start = p;
    for (;; p++) {
        if (*p == '.' && !seen_point) {
            seen_point = 1;
            continue;
        }
        if (*p < '0' || *p > '9')
            break;
        any = 1;
        if (a.n == 0 && *p == '0') {
            if (seen_point)
                a.dp--; /* leading zero after the point */
            continue;
        }
        if (a.n < MAXDIG)
            a.d[a.n++] = (unsigned char)(*p - '0');
        else if (*p != '0')
            a.sticky = 1;
        if (!seen_point)
            a.dp++;
        if (nsmall < 19) {
            small = small * 10 + (unsigned long)(*p - '0');
            nsmall++;
        } else {
            exact = 0;
        }
    }
    if (!any) {
        if (end)
            *end = (char *)s;
        return 0.0;
    }
    (void)digits_start;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-') {
            eneg = 1;
            q++;
        } else if (*q == '+') {
            q++;
        }
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') {
                if (e < 100000)
                    e = e * 10 + (*q - '0');
                q++;
            }
            exp10 = eneg ? -e : e;
            p = q;
        }
    }
    if (end)
        *end = (char *)p;
    a.dp += exp10;
    /* fast path: small integer mantissa and a power of ten that are both exact */
    if (exact && a.sticky == 0 && small < (1UL << 53)) {
        int e = a.dp - nsmall;
        double d = (double)small;
        if (small == 0)
            return neg ? -0.0 : 0.0;
        if (e >= 0 && e <= 22)
            return neg ? -(d * pow10[e]) : d * pow10[e];
        if (e < 0 && e >= -22)
            return neg ? -(d / pow10[-e]) : d / pow10[-e];
    }
    return dec_to_double(&a, neg);
}
