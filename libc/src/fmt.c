/* printf formatting engine. Floating point output is exact: the binary
   value is expanded to all of its decimal digits, then rounded. */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

typedef void (*OutFn)(void *ctx, const char *s, size_t n);

typedef struct Out {
    OutFn fn;
    void *ctx;
    int count;
} Out;

static void out(Out *o, const char *s, size_t n)
{
    if (n) {
        o->fn(o->ctx, s, n);
        o->count += (int)n;
    }
}

static void pad(Out *o, int c, int n)
{
    char buf[32];
    memset(buf, c, sizeof buf);
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        out(o, buf, (size_t)k);
        n -= k;
    }
}

#define F_LEFT 1
#define F_PLUS 2
#define F_SPACE 4
#define F_ALT 8
#define F_ZERO 16

/* Emits prefix + zeros + body, padded to width. */
static void field(Out *o, int flags, int width, const char *prefix, int nprefix, int zeros, const char *body, int nbody,
                  int trail_zeros, const char *tail, int ntail)
{
    int len = nprefix + zeros + nbody + trail_zeros + ntail;
    if (!(flags & (F_LEFT | F_ZERO)))
        pad(o, ' ', width - len);
    out(o, prefix, (size_t)nprefix);
    if (!(flags & F_LEFT) && (flags & F_ZERO))
        pad(o, '0', width - len);
    pad(o, '0', zeros);
    out(o, body, (size_t)nbody);
    pad(o, '0', trail_zeros);
    out(o, tail, (size_t)ntail);
    if (flags & F_LEFT)
        pad(o, ' ', width - len);
}

/* ---- floating point ---- */

#define NLIMB 130 /* base 10^9 limbs: enough for 2^53 * 5^1074 */

/* All decimal digits of the integer mant * 2^e2 (e2 >= 0) or mant * 5^-e2
   (e2 < 0). Returns the digit count; digits are written without leading zeros. */
static int big_digits(unsigned long mant, int e2, char *digits)
{
    unsigned int limb[NLIMB];
    int n = 0, i, nd = 0;
    while (mant) {
        limb[n++] = (unsigned int)(mant % 1000000000UL);
        mant /= 1000000000UL;
    }
    while (e2 > 0) {
        int k = e2 > 29 ? 29 : e2;
        unsigned long carry = 0;
        for (i = 0; i < n; i++) {
            unsigned long v = ((unsigned long)limb[i] << k) + carry;
            limb[i] = (unsigned int)(v % 1000000000UL);
            carry = v / 1000000000UL;
        }
        while (carry) {
            limb[n++] = (unsigned int)(carry % 1000000000UL);
            carry /= 1000000000UL;
        }
        e2 -= k;
    }
    while (e2 < 0) {
        int k = -e2 > 13 ? 13 : -e2;
        unsigned long m = 1, carry = 0;
        for (i = 0; i < k; i++)
            m *= 5;
        for (i = 0; i < n; i++) {
            unsigned long v = (unsigned long)limb[i] * m + carry;
            limb[i] = (unsigned int)(v % 1000000000UL);
            carry = v / 1000000000UL;
        }
        while (carry) {
            limb[n++] = (unsigned int)(carry % 1000000000UL);
            carry /= 1000000000UL;
        }
        e2 += k;
    }
    for (i = n - 1; i >= 0; i--) {
        char tmp[9];
        unsigned int v = limb[i];
        int k;
        for (k = 8; k >= 0; k--) {
            tmp[k] = (char)('0' + v % 10);
            v /= 10;
        }
        k = 0;
        if (i == n - 1)
            while (k < 8 && tmp[k] == '0')
                k++;
        for (; k < 9; k++)
            digits[nd++] = tmp[k];
    }
    return nd;
}

/* Rounds the digit string to keep digits (half to even); returns 1 if the
   rounding carried into a new leading digit (the string then starts with 1). */
static int round_digits(char *d, int nd, int keep)
{
    int i, up;
    if (keep >= nd)
        return 0;
    if (keep < 0)
        return 0;
    up = d[keep] > '5';
    if (d[keep] == '5') {
        for (i = keep + 1; i < nd; i++)
            if (d[i] != '0')
                up = 1;
        if (!up)
            up = keep > 0 ? (d[keep - 1] - '0') & 1 : 0;
    }
    if (!up)
        return 0;
    for (i = keep - 1; i >= 0; i--) {
        if (d[i] != '9') {
            d[i]++;
            return 0;
        }
        d[i] = '0';
    }
    /* 999.. rounded up to 1000..: shift right by one */
    for (i = keep; i > 0; i--)
        d[i] = d[i - 1];
    d[0] = '1';
    return 1;
}

static void fmt_float(Out *o, double x, int conv, int flags, int width, int prec)
{
    char digits[1200];
    char prefix[2], body[1400], tail[8];
    unsigned long bits, mant;
    int nprefix = 0, nbody = 0, ntail = 0, trail = 0;
    int e2, nd, dp, upper = conv == 'E' || conv == 'G', lower = conv | 32;
    int i, exp10;

    memcpy(&bits, &x, 8);
    if (bits >> 63)
        prefix[nprefix++] = '-';
    else if (flags & F_PLUS)
        prefix[nprefix++] = '+';
    else if (flags & F_SPACE)
        prefix[nprefix++] = ' ';
    e2 = (int)((bits >> 52) & 0x7ff);
    mant = bits & 0xfffffffffffffUL;
    if (e2 == 0x7ff) {
        const char *s = mant ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        field(o, flags & ~F_ZERO, width, prefix, nprefix, 0, s, 3, 0, "", 0);
        return;
    }
    if (prec < 0)
        prec = 6;
    if (e2 == 0) {
        e2 = -1074;
    } else {
        mant |= 1UL << 52;
        e2 -= 1075;
    }
    /* value = digits * 10^(dp - nd): dp is the position of the decimal point */
    if (mant == 0) {
        digits[0] = '0';
        nd = 1;
        dp = 1;
    } else {
        nd = big_digits(mant, e2, digits);
        dp = e2 < 0 ? nd + e2 : nd;
    }
    if (lower == 'g' && prec == 0)
        prec = 1;

    if (lower == 'e' || lower == 'g') {
        int sig = lower == 'e' ? prec + 1 : prec; /* significant digits wanted */
        int use_e = 1;
        if (round_digits(digits, nd, sig))
            dp++;
        if (nd > sig)
            nd = sig;
        exp10 = mant == 0 ? 0 : dp - 1;
        if (lower == 'g') {
            if (exp10 >= -4 && exp10 < prec) {
                use_e = 0;
                prec = prec - 1 - exp10; /* digits after the point in f style */
            } else {
                prec = prec - 1;
            }
            if (!(flags & F_ALT)) {
                /* strip trailing zeros */
                int last = nd;
                while (last > 1 && digits[last - 1] == '0')
                    last--;
                nd = last;
                if (use_e) {
                    if (prec > nd - 1)
                        prec = nd - 1;
                } else {
                    int frac = nd - (exp10 + 1);
                    if (frac < 0)
                        frac = 0;
                    if (prec > frac)
                        prec = frac;
                }
            }
        }
        if (use_e) {
            int ae = exp10 < 0 ? -exp10 : exp10;
            body[nbody++] = digits[0];
            if (prec > 0 || (flags & F_ALT))
                body[nbody++] = '.';
            for (i = 1; i <= prec; i++) {
                if (i < nd)
                    body[nbody++] = digits[i];
                else
                    trail++;
            }
            tail[ntail++] = upper ? 'E' : 'e';
            tail[ntail++] = exp10 < 0 ? '-' : '+';
            if (ae >= 100)
                tail[ntail++] = (char)('0' + ae / 100);
            tail[ntail++] = (char)('0' + ae / 10 % 10);
            tail[ntail++] = (char)('0' + ae % 10);
            field(o, flags, width, prefix, nprefix, 0, body, nbody, trail, tail, ntail);
            return;
        }
        /* fall into fixed notation with the digits already rounded */
    } else {
        /* 'f': keep dp + prec digits */
        int keep = dp + prec;
        if (keep < 0) {
            nd = 0;
        } else {
            if (keep == 0 && nd > 0) {
                /* rounding decides between 0 and 1 at the last place */
                int up = digits[0] > '5';
                if (digits[0] == '5')
                    for (i = 1; i < nd; i++)
                        if (digits[i] != '0')
                            up = 1;
                if (up) {
                    digits[0] = '1';
                    nd = 1;
                    dp++;
                } else {
                    nd = 0;
                }
            } else {
                if (round_digits(digits, nd, keep)) {
                    dp++;
                    keep++;
                }
                if (nd > keep)
                    nd = keep;
            }
        }
    }
    /* fixed notation: integer part, point, prec fraction digits */
    if (dp <= 0) {
        body[nbody++] = '0';
    } else {
        for (i = 0; i < dp; i++)
            body[nbody++] = i < nd ? digits[i] : '0';
    }
    if (prec > 0 || (flags & F_ALT))
        body[nbody++] = '.';
    for (i = 0; i < prec; i++) {
        int idx = dp + i;
        if (nbody >= (int)sizeof body - 1) {
            trail = prec - i;
            break;
        }
        body[nbody++] = idx >= 0 && idx < nd ? digits[idx] : '0';
    }
    field(o, flags, width, prefix, nprefix, 0, body, nbody, trail, "", 0);
}

/* %a: the value in hexadecimal, [-]0xh.hhhhp+d, as glibc writes it: one leading digit (1, or 0 for zero
   and subnormal numbers), the shortest exact fraction unless a precision asks for rounding (to even). */
static void fmt_hex_float(Out *o, double x, int conv, int flags, int width, int prec)
{
    const char *dig = conv == 'A' ? "0123456789ABCDEF" : "0123456789abcdef";
    char prefix[4], body[24], tail[8];
    unsigned long bits, mant;
    int nprefix = 0, nbody = 0, ntail = 0, trail = 0, e2, lead, nfrac = 13, i;

    memcpy(&bits, &x, 8);
    e2 = (int)((bits >> 52) & 0x7ff);
    mant = bits & 0xfffffffffffffUL;
    if (e2 == 0x7ff) {
        fmt_float(o, x, conv == 'A' ? 'E' : 'e', flags, width, prec);
        return;
    }
    if (bits >> 63)
        prefix[nprefix++] = '-';
    else if (flags & F_PLUS)
        prefix[nprefix++] = '+';
    else if (flags & F_SPACE)
        prefix[nprefix++] = ' ';
    prefix[nprefix++] = '0';
    prefix[nprefix++] = conv == 'A' ? 'X' : 'x';
    lead = e2 != 0;
    e2 = e2 ? e2 - 1023 : mant ? -1022 : 0;
    if (prec >= 0 && prec < 13) {
        int drop = 4 * (13 - prec);
        unsigned long all = ((unsigned long)lead << 52) | mant, half = 1UL << (drop - 1), rest = all & ((1UL << drop) - 1);
        all >>= drop;
        if (rest > half || (rest == half && (all & 1)))
            all++;
        lead = (int)(all >> (4 * prec));
        mant = prec ? (all & ((1UL << (4 * prec)) - 1)) << drop : 0;
        nfrac = prec;
    } else if (prec < 0) {
        while (nfrac > 0 && !((mant >> (4 * (13 - nfrac))) & 15))
            nfrac--;
    } else {
        trail = prec - 13;
    }
    body[nbody++] = dig[lead];
    if (nfrac > 0 || trail > 0 || (flags & F_ALT))
        body[nbody++] = '.';
    for (i = 0; i < nfrac; i++)
        body[nbody++] = dig[(mant >> (4 * (12 - i))) & 15];
    tail[ntail++] = conv == 'A' ? 'P' : 'p';
    tail[ntail++] = e2 < 0 ? '-' : '+';
    if (e2 < 0)
        e2 = -e2;
    if (e2 >= 1000)
        tail[ntail++] = (char)('0' + e2 / 1000);
    if (e2 >= 100)
        tail[ntail++] = (char)('0' + e2 / 100 % 10);
    if (e2 >= 10)
        tail[ntail++] = (char)('0' + e2 / 10 % 10);
    tail[ntail++] = (char)('0' + e2 % 10);
    field(o, flags, width, prefix, nprefix, 0, body, nbody, trail, tail, ntail);
}

/* ---- the engine ---- */

int __vformat(OutFn fn, void *ctx, const char *fmt, va_list ap)
{
    Out o;
    o.fn = fn;
    o.ctx = ctx;
    o.count = 0;
    for (;;) {
        const char *start = fmt;
        int flags = 0, width = 0, prec = -1, length = 0, c;
        while (*fmt && *fmt != '%')
            fmt++;
        out(&o, start, (size_t)(fmt - start));
        if (!*fmt)
            break;
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-')
                flags |= F_LEFT;
            else if (*fmt == '+')
                flags |= F_PLUS;
            else if (*fmt == ' ')
                flags |= F_SPACE;
            else if (*fmt == '#')
                flags |= F_ALT;
            else if (*fmt == '0')
                flags |= F_ZERO;
            else if (*fmt != '\'') /* thousands grouping: the C locale has none */
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') {
            if (*fmt != 'h')
                length = 'l';
            else if (!length)
                length = 'h';
            else if (length == 'h')
                length = 'H'; /* hh: a char */
            fmt++;
        }
        c = *fmt;
        if (!c)
            break;
        fmt++;
        switch (c) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            char buf[32], prefix[3];
            unsigned long v;
            int n = 0, nprefix = 0, zeros = 0, base = 10;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (c == 'p') {
                v = (unsigned long)va_arg(ap, void *);
                base = 16;
                prefix[nprefix++] = '0';
                prefix[nprefix++] = 'x';
            } else if (c == 'd' || c == 'i') {
                long sv = length == 'l' ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (length == 'h')
                    sv = (short)sv;
                else if (length == 'H')
                    sv = (signed char)sv;
                if (sv < 0) {
                    prefix[nprefix++] = '-';
                    v = 0 - (unsigned long)sv;
                } else {
                    if (flags & F_PLUS)
                        prefix[nprefix++] = '+';
                    else if (flags & F_SPACE)
                        prefix[nprefix++] = ' ';
                    v = (unsigned long)sv;
                }
            } else {
                v = length == 'l' ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned);
                if (length == 'h')
                    v = (unsigned short)v;
                else if (length == 'H')
                    v = (unsigned char)v;
                base = c == 'o' ? 8 : c == 'u' ? 10 : 16;
                if ((flags & F_ALT) && v && base == 16) {
                    prefix[nprefix++] = '0';
                    prefix[nprefix++] = (char)c;
                }
            }
            while (v) {
                buf[sizeof buf - 1 - n++] = dig[v % (unsigned long)base];
                v /= (unsigned long)base;
            }
            if (prec < 0)
                prec = 1;
            else
                flags &= ~F_ZERO;
            if (n < prec)
                zeros = prec - n;
            if (base == 8 && (flags & F_ALT) && zeros == 0)
                zeros = 1;
            field(&o, flags, width, prefix, nprefix, zeros, buf + sizeof buf - n, n, 0, "", 0);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            field(&o, flags & ~F_ZERO, width, "", 0, 0, &ch, 1, 0, "", 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;
            if (!s)
                s = "(null)";
            while ((prec < 0 || n < prec) && s[n])
                n++;
            field(&o, flags & ~F_ZERO, width, "", 0, 0, s, n, 0, "", 0);
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
            fmt_float(&o, va_arg(ap, double), c, flags, width, prec);
            break;
        case 'F':
            fmt_float(&o, va_arg(ap, double), 'f', flags, width, prec);
            break;
        case 'a':
        case 'A':
            fmt_hex_float(&o, va_arg(ap, double), c, flags, width, prec);
            break;
        case 'n':
            if (length == 'l')
                *va_arg(ap, long *) = o.count;
            else if (length == 'h')
                *va_arg(ap, short *) = (short)o.count;
            else if (length == 'H')
                *va_arg(ap, signed char *) = (signed char)o.count;
            else
                *va_arg(ap, int *) = o.count;
            break;
        default: {
            char ch = (char)c;
            out(&o, &ch, 1);
            break;
        }
        }
    }
    return o.count;
}
