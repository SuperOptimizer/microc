/* Floating point and the part of Advanced SIMD that compilers emit for ordinary C. */
#include "vm.h"
#include <math.h>

#define M32 0xffffffffUL
#define RD (insn & 31)
#define RN (insn >> 5 & 31)
#define RM (insn >> 16 & 31)

static double getd(V128 *v, int i)
{
    double d;
    memcpy(&d, &v->d[i], 8);
    return d;
}

static float getf(V128 *v, int i)
{
    float f;
    memcpy(&f, &v->w[i], 4);
    return f;
}

static void putd(V128 *v, int i, double d) { memcpy(&v->d[i], &d, 8); }
static void putf(V128 *v, int i, float f) { memcpy(&v->w[i], &f, 4); }

static double get(Cpu *c, u32 r, int dbl) { return dbl ? getd(&c->v[r], 0) : (double)getf(&c->v[r], 0); }

static void set(Cpu *c, u32 r, int dbl, double d)
{
    c->v[r].d[0] = c->v[r].d[1] = 0;
    if (dbl)
        putd(&c->v[r], 0, d);
    else
        putf(&c->v[r], 0, (float)d);
}

/* Round to an integral value: 0 nearest even, 1 up, 2 down, 3 toward zero, 4 nearest away. */
static double rnd(double d, int mode)
{
    double t, diff;
    if (d != d || d == 0 || d >= 4503599627370496.0 || d <= -4503599627370496.0)
        return d;
    t = (double)(s64)d;
    diff = d < t ? t - d : d - t;
    switch (mode) {
    case 0:
        if (diff > 0.5 || (diff == 0.5 && ((s64)t & 1)))
            t += d < 0 ? -1 : 1;
        break;
    case 1:
        if (t < d)
            t += 1;
        break;
    case 2:
        if (t > d)
            t -= 1;
        break;
    case 4:
        if (diff >= 0.5)
            t += d < 0 ? -1 : 1;
        break;
    }
    if (t == 0 && d < 0) { /* a negative number rounds to minus zero */
        u64 bits = 0x8000000000000000UL;
        memcpy(&t, &bits, 8);
    }
    return t;
}

static u64 toint(double d, int uns, int sf)
{
    if (d != d)
        return 0;
    if (uns) {
        if (d <= 0)
            return 0;
        if (!sf)
            return d >= 4294967296.0 ? M32 : (u64)(s64)d;
        if (d >= 18446744073709551616.0)
            return ~(u64)0;
        if (d >= 9223372036854775808.0)
            return (u64)(s64)(d - 9223372036854775808.0) + 0x8000000000000000UL;
        return (u64)(s64)d;
    }
    if (sf) {
        if (d >= 9223372036854775808.0)
            return 0x7fffffffffffffffUL;
        if (d <= -9223372036854775808.0)
            return 0x8000000000000000UL;
        return (u64)(s64)d;
    }
    if (d >= 2147483648.0)
        return 0x7fffffff;
    if (d <= -2147483648.0)
        return 0x80000000UL;
    return (u64)(s64)d & M32;
}

static u32 cmpflags(double a, double b)
{
    if (a != a || b != b)
        return 3;
    return a == b ? 6 : a < b ? 8 : 2;
}

static int condf(Cpu *c, u32 cc)
{
    u32 f = c->nzcv;
    int n = f >> 3 & 1, z = f >> 2 & 1, cy = f >> 1 & 1, v = f & 1, r;
    switch (cc >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return 1;
    }
    return (cc & 1) ? !r : r;
}

/* fmax/fmin give a NaN if either is one; the "number" forms (nm) give the other operand then. */
static double fmaxmin(double a, double b, int min, int nm)
{
    if (a != a)
        return nm && b == b ? b : a;
    if (b != b)
        return nm ? a : b;
    if (a == 0 && b == 0) {
        u64 ua, ub;
        memcpy(&ua, &a, 8);
        memcpy(&ub, &b, 8);
        return (min ? (ua | ub) : (ua & ub)) >> 63 ? -0.0 : 0.0; /* -0 is the smaller zero */
    }
    return min ? (a < b ? a : b) : (a > b ? a : b);
}

/* a + b = *s + *e exactly, *s the rounded sum. */
static void two_sum(double a, double b, double *s, double *e)
{
    double x = a + b, bb = x - a;
    *s = x;
    *e = (a - (x - bb)) + (b - bb);
}

/* a + b rounded to odd: the rounded-down-in-magnitude sum with its last bit set if the sum is not exact.
   Rounding that once more, to fewer bits or after a further exact step, gives the correctly rounded result. */
static double add_odd(double a, double b)
{
    double x, e;
    u64 u;
    two_sum(a, b, &x, &e);
    if (e == 0 || x != x || x - x != 0)
        return x;
    memcpy(&u, &x, 8);
    if (!(u & 1)) {
        /* the nearest sum is even: its odd neighbour on the side of the error */
        if ((e > 0) == (x > 0))
            u++;
        else
            u--;
        memcpy(&x, &u, 8);
    }
    return x;
}

/* a * b = *p + *e exactly (Dekker), for values whose product neither overflows nor is subnormal. */
static void two_prod(double a, double b, double *p, double *e)
{
    double x = a * b, t, ah, al, bh, bl;
    t = a * 134217729.0;
    ah = t - (t - a);
    al = a - ah;
    t = b * 134217729.0;
    bh = t - (t - b);
    bl = b - bh;
    *p = x;
    *e = ((ah * bh - x) + ah * bl + al * bh) + al * bl;
}

/* a * b + c with one rounding (Boldo and Melquiond). */
static double fused(double a, double b, double c, int dbl)
{
    double p, pe, th, tl, x = a * b;
    if (!dbl)
        return (double)(float)add_odd(x, c); /* the product of two floats is exact in a double */
    if (x != x || x - x != 0 || c != c || c - c != 0 || x == 0)
        return x + c;
    if (x > 1e300 || x < -1e300 || (x < 1e-280 && x > -1e-280) || a > 1e150 || a < -1e150 || b > 1e150 || b < -1e150)
        return x + c; /* out of the range of the exact product: rounded twice */
    two_prod(a, b, &p, &pe);
    two_sum(c, p, &th, &tl);
    return th + add_odd(tl, pe);
}

static int scalar(Cpu *c, u32 insn)
{
    u32 type = insn >> 22 & 3, rd = RD, rn = RN, rm = RM;
    int dbl = type == 1, sf = (int)(insn >> 31);
    double a, b;
    if (type > 1 && (insn & 0x7f36fc00) != 0x1e260000)
        return 0;
    switch (insn >> 10 & 3) {
    case 1:
        c->nzcv = condf(c, insn >> 12 & 15) ? cmpflags(get(c, rn, dbl), get(c, rm, dbl)) : (insn & 15);
        return 1;
    case 2:
        a = get(c, rn, dbl);
        b = get(c, rm, dbl);
        switch (insn >> 12 & 15) {
        case 0: a = dbl ? a * b : (double)((float)a * (float)b); break;
        case 1: a = dbl ? a / b : (double)((float)a / (float)b); break;
        case 2: a = dbl ? a + b : (double)((float)a + (float)b); break;
        case 3: a = dbl ? a - b : (double)((float)a - (float)b); break;
        case 4: a = fmaxmin(a, b, 0, 0); break;
        case 6: a = fmaxmin(a, b, 0, 1); break;
        case 5: a = fmaxmin(a, b, 1, 0); break;
        case 7: a = fmaxmin(a, b, 1, 1); break;
        case 8: a = dbl ? -(a * b) : (double)-((float)a * (float)b); break;
        default: return 0;
        }
        set(c, rd, dbl, a);
        return 1;
    case 3: {
        V128 t = c->v[condf(c, insn >> 12 & 15) ? rn : rm];
        c->v[rd].d[0] = dbl ? t.d[0] : t.w[0];
        c->v[rd].d[1] = 0;
        return 1;
    }
    }
    if ((insn >> 10 & 63) == 0) { /* to and from the general registers */
        u32 rmode = insn >> 19 & 3, opc = insn >> 16 & 7;
        u64 x = c->x[rn];
        switch (opc) {
        case 2:
            if (dbl)
                set(c, rd, 1, sf ? (double)(s64)x : (double)(s32)x);
            else {
                c->v[rd].d[0] = c->v[rd].d[1] = 0;
                putf(&c->v[rd], 0, sf ? (float)(s64)x : (float)(s32)x);
            }
            return 1;
        case 3:
            if (dbl)
                set(c, rd, 1, sf ? (double)x : (double)(u32)x);
            else {
                c->v[rd].d[0] = c->v[rd].d[1] = 0;
                putf(&c->v[rd], 0, sf ? (float)x : (float)(u32)x);
            }
            return 1;
        case 0:
        case 1: c->x[rd] = toint(rnd(get(c, rn, dbl), (int)rmode), (int)(opc & 1), sf); return 1;
        case 4:
        case 5:
            if (rmode)
                return 0;
            c->x[rd] = toint(rnd(get(c, rn, dbl), 4), (int)(opc & 1), sf);
            return 1;
        case 6: c->x[rd] = rmode ? c->v[rn].d[1] : sf ? c->v[rn].d[0] : c->v[rn].w[0]; return 1;
        default:
            if (rmode)
                c->v[rd].d[1] = x;
            else {
                c->v[rd].d[0] = sf ? x : x & M32;
                c->v[rd].d[1] = 0;
            }
            return 1;
        }
    }
    if ((insn >> 10 & 31) == 16) {
        u32 opc = insn >> 15 & 63;
        a = get(c, rn, dbl);
        switch (opc) {
        case 0:
            c->v[rd].d[0] = dbl ? c->v[rn].d[0] : c->v[rn].w[0];
            c->v[rd].d[1] = 0;
            return 1;
        case 1:
            c->v[rd].d[0] = dbl ? c->v[rn].d[0] & 0x7fffffffffffffffUL : c->v[rn].w[0] & 0x7fffffffUL;
            c->v[rd].d[1] = 0;
            return 1;
        case 2:
            c->v[rd].d[0] = dbl ? c->v[rn].d[0] ^ 0x8000000000000000UL : c->v[rn].w[0] ^ 0x80000000UL;
            c->v[rd].d[1] = 0;
            return 1;
        case 3: set(c, rd, dbl, dbl ? sqrt(a) : (double)(float)sqrt(a)); return 1;
        case 4: set(c, rd, 0, a); return 1;
        case 5: set(c, rd, 1, a); return 1;
        case 8: set(c, rd, dbl, rnd(a, 0)); return 1;
        case 9: set(c, rd, dbl, rnd(a, 1)); return 1;
        case 10: set(c, rd, dbl, rnd(a, 2)); return 1;
        case 11: set(c, rd, dbl, rnd(a, 3)); return 1;
        case 12: set(c, rd, dbl, rnd(a, 4)); return 1;
        case 14:
        case 15: set(c, rd, dbl, rnd(a, 0)); return 1;
        }
        return 0;
    }
    if ((insn >> 10 & 15) == 8) {
        c->nzcv = cmpflags(get(c, rn, dbl), insn & 8 ? 0.0 : get(c, rm, dbl));
        return 1;
    }
    if ((insn >> 10 & 7) == 4) {
        u32 imm8 = insn >> 13 & 0xff, b6 = imm8 >> 6 & 1;
        u64 bits;
        if (dbl)
            bits = (u64)(imm8 >> 7) << 63 | (u64)(b6 ^ 1) << 62 | (b6 ? (u64)0xff << 54 : 0) | (u64)(imm8 & 63) << 48;
        else
            bits = (u64)(imm8 >> 7) << 31 | (u64)(b6 ^ 1) << 30 | (b6 ? (u64)0x1f << 25 : 0) | (u64)(imm8 & 63) << 19;
        c->v[rd].d[0] = bits;
        c->v[rd].d[1] = 0;
        return 1;
    }
    return 0;
}

static u64 lane(V128 *v, int size, int i)
{
    switch (size) {
    case 0: return v->b[i];
    case 1: return v->h[i];
    case 2: return v->w[i];
    }
    return v->d[i];
}

static void setlane(V128 *v, int size, int i, u64 x)
{
    switch (size) {
    case 0: v->b[i] = (u8)x; break;
    case 1: v->h[i] = (u16)x; break;
    case 2: v->w[i] = (u32)x; break;
    default: v->d[i] = x;
    }
}

static s64 sx(u64 v, int size) { return size == 3 ? (s64)v : (s64)(v << (64 - (8 << size))) >> (64 - (8 << size)); }

static int simd(Cpu *c, u32 insn)
{
    u32 rd = RD, rn = RN, rm = RM, u = insn >> 29 & 1;
    int q = (int)(insn >> 30 & 1), size = (int)(insn >> 22 & 3), n, i;
    V128 a = c->v[rn], b = c->v[rm], r;
    u64 ones;
    r.d[0] = r.d[1] = 0;
    if ((insn & 0x9f200400) == 0x0e200400) { /* three registers of the same type */
        u32 op = insn >> 11 & 31;
        if (op == 3) {
            for (i = 0; i < 2; i++) {
                u64 x = a.d[i], y = b.d[i], d = c->v[rd].d[i];
                switch ((int)u << 2 | size) {
                case 0: d = x & y; break;
                case 1: d = x & ~y; break;
                case 2: d = x | y; break;
                case 3: d = x | ~y; break;
                case 4: d = x ^ y; break;
                case 5: d = (x & d) | (y & ~d); break;
                case 6: d = (d & ~y) | (x & y); break;
                default: d = (d & y) | (x & ~y);
                }
                r.d[i] = d;
            }
        } else if (op >= 0x18) {
            int dbl = size & 1, hi = size >> 1;
            n = q ? (dbl ? 2 : 4) : (dbl ? 1 : 2);
            if (dbl && !q)
                return 0;
            for (i = 0; i < n; i++) {
                double x = dbl ? getd(&a, i) : (double)getf(&a, i), y = dbl ? getd(&b, i) : (double)getf(&b, i), z;
                int cmp = -1;
                if (op == 0x1a && !u)
                    z = hi ? x - y : x + y;
                else if (op == 0x1a && !hi) { /* faddp */
                    V128 *s = i < n / 2 ? &a : &b;
                    int k = 2 * (i % (n / 2));
                    z = dbl ? getd(s, k) + getd(s, k + 1) : (double)(getf(s, k) + getf(s, k + 1));
                } else if (op == 0x1b && u)
                    z = x * y;
                else if (op == 0x1f && u)
                    z = x / y;
                else if (op == 0x1c) {
                    cmp = !u ? x == y : hi ? x > y : x >= y;
                    z = 0;
                } else
                    return 0;
                if (cmp >= 0)
                    setlane(&r, dbl ? 3 : 2, i, cmp ? ~(u64)0 : 0);
                else if (dbl)
                    putd(&r, i, z);
                else
                    putf(&r, i, (float)z);
            }
        } else {
            n = (q ? 16 : 8) >> size;
            ones = size == 3 ? ~(u64)0 : ((u64)1 << (8 << size)) - 1;
            for (i = 0; i < n; i++) {
                u64 x = lane(&a, size, i), y = lane(&b, size, i), z;
                s64 sxv = sx(x, size), syv = sx(y, size);
                switch (op) {
                case 0x10: z = u ? x - y : x + y; break;
                case 0x13: z = x * y; break;
                case 0x06: z = (u ? x > y : sxv > syv) ? ones : 0; break;
                case 0x07: z = (u ? x >= y : sxv >= syv) ? ones : 0; break;
                case 0x11: z = (u ? x == y : (x & y) != 0) ? ones : 0; break;
                case 0x0c: z = (u ? x > y : sxv > syv) ? x : y; break;
                case 0x0d: z = (u ? x < y : sxv < syv) ? x : y; break;
                case 0x17: {
                    V128 *s = i < n / 2 ? &a : &b;
                    int k = 2 * (i % (n / 2));
                    z = lane(s, size, k) + lane(s, size, k + 1);
                    break;
                }
                default: return 0;
                }
                setlane(&r, size, i, z);
            }
        }
        if (!q)
            r.d[1] = 0;
        c->v[rd] = r;
        return 1;
    }
    if ((insn & 0x9f3e0c00) == 0x0e200800) { /* two registers */
        u32 op = insn >> 12 & 31;
        int dbl = size & 1;
        if (op == 0x1d || op == 0x1b || op == 0x0f) {
            n = q ? (dbl ? 2 : 4) : (dbl ? 1 : 2);
            if ((op == 0x1d) == (size >> 1))
                return 0;
            for (i = 0; i < n; i++) {
                u64 x = lane(&a, dbl ? 3 : 2, i);
                double f = dbl ? getd(&a, i) : (double)getf(&a, i);
                if (op == 0x1d) {
                    if (dbl)
                        putd(&r, i, u ? (double)x : (double)(s64)x);
                    else
                        putf(&r, i, u ? (float)(u32)x : (float)(s32)x);
                } else if (op == 0x1b)
                    setlane(&r, dbl ? 3 : 2, i, toint(rnd(f, 3), (int)u, dbl));
                else
                    setlane(&r, dbl ? 3 : 2, i, u ? x ^ (u64)1 << (dbl ? 63 : 31) : x & ~((u64)1 << (dbl ? 63 : 31)));
            }
        } else {
            n = (q ? 16 : 8) >> size;
            for (i = 0; i < n; i++) {
                u64 x = lane(&a, size, i), z;
                if (op == 0x0b)
                    z = u || sx(x, size) < 0 ? 0 - x : x;
                else if (op == 0x05 && u)
                    z = ~x;
                else if (op == 0x05) {
                    for (z = 0; x; x >>= 1)
                        z += x & 1;
                } else
                    return 0;
                setlane(&r, size, i, z);
            }
        }
        c->v[rd] = r;
        return 1;
    }
    if ((insn & 0x9f3e0c00) == 0x0e300800) { /* across the lanes */
        u32 op = insn >> 12 & 31;
        u64 acc;
        n = (q ? 16 : 8) >> size;
        acc = lane(&a, size, 0);
        if (op == 0x03 && !u)
            acc = (u64)sx(acc, size);
        for (i = 1; i < n; i++) {
            u64 x = lane(&a, size, i);
            s64 sv = sx(x, size), sa = sx(acc, size);
            switch (op) {
            case 0x1b: acc += x; break;
            case 0x03: acc += u ? x : (u64)sv; break;
            case 0x0a: acc = (u ? x > acc : sv > sa) ? x : acc; break;
            case 0x1a: acc = (u ? x < acc : sv < sa) ? x : acc; break;
            default: return 0;
            }
        }
        setlane(&r, op == 0x03 ? size + 1 : size, 0, acc);
        c->v[rd] = r;
        return 1;
    }
    if ((insn & 0xdf3e0c00) == 0x5e300800) { /* scalar pairwise */
        u32 op = insn >> 12 & 31;
        if (op == 0x1b && !u && size == 3)
            r.d[0] = a.d[0] + a.d[1];
        else if (op == 0x0d && u && !(size & 2)) {
            if (size & 1)
                putd(&r, 0, getd(&a, 0) + getd(&a, 1));
            else
                putf(&r, 0, getf(&a, 0) + getf(&a, 1));
        } else
            return 0;
        c->v[rd] = r;
        return 1;
    }
    if ((insn & 0x9fe08400) == 0x0e000400) { /* copies */
        u32 imm5 = insn >> 16 & 31, imm4 = insn >> 11 & 15;
        int idx;
        if (!(imm5 & 15))
            return 0;
        for (size = 0; !(imm5 >> size & 1); size++)
            ;
        idx = (int)(imm5 >> (size + 1));
        n = (q ? 16 : 8) >> size;
        if (u) {
            r = c->v[rd];
            setlane(&r, size, idx, lane(&a, size, (int)(imm4 >> size)));
        } else if (imm4 == 0 || imm4 == 1) {
            u64 x = imm4 ? c->x[rn] : lane(&a, size, idx);
            for (i = 0; i < n; i++)
                setlane(&r, size, i, x);
        } else if (imm4 == 3) {
            r = c->v[rd];
            setlane(&r, size, idx, c->x[rn]);
        } else if (imm4 == 5 || imm4 == 7) {
            u64 x = lane(&a, size, idx);
            if (imm4 == 5)
                x = (u64)sx(x, size);
            c->x[rd] = q ? x : x & M32;
            return 1;
        } else
            return 0;
        c->v[rd] = r;
        return 1;
    }
    if ((insn & 0x9f800400) == 0x0f000400) {
        u32 immh = insn >> 19 & 15, op = insn >> 11 & 31, immhb = insn >> 16 & 0x7f;
        if (immh) { /* shifts by a constant */
            int sh;
            for (size = 3; !(immh >> size & 1); size--)
                ;
            n = (q ? 16 : 8) >> size;
            sh = op == 0x0a ? (int)immhb - (8 << size) : (16 << size) - (int)immhb;
            for (i = 0; i < n; i++) {
                u64 x = lane(&a, size, i);
                if (op == 0x0a && !u)
                    x <<= sh;
                else if (op == 0 && u)
                    x = sh > 63 ? 0 : x >> sh;
                else if (op == 0)
                    x = (u64)(sx(x, size) >> (sh > 63 ? 63 : sh));
                else
                    return 0;
                setlane(&r, size, i, x);
            }
        } else { /* constants */
            u32 cmode = insn >> 12 & 15, imm8 = (insn >> 16 & 7) << 5 | (insn >> 5 & 31);
            u64 v = 0;
            if (insn >> 11 & 1)
                return 0;
            if (!(cmode & 8)) {
                if (cmode & 1)
                    return 0;
                v = (u64)imm8 << (8 * (cmode >> 1));
                v |= v << 32;
            } else if (!(cmode & 4)) {
                if (cmode & 1)
                    return 0;
                v = (u64)imm8 << (8 * (cmode >> 1 & 1));
                v |= v << 16;
                v |= v << 32;
            } else if (cmode < 14) {
                v = cmode & 1 ? (u64)imm8 << 16 | 0xffff : (u64)imm8 << 8 | 0xff;
                v |= v << 32;
            } else if (cmode == 14 && !u) {
                for (i = 0; i < 8; i++)
                    v |= (u64)imm8 << (8 * i);
            } else if (cmode == 14) {
                for (i = 0; i < 8; i++)
                    if (imm8 >> i & 1)
                        v |= (u64)0xff << (8 * i);
            } else {
                u32 b6 = imm8 >> 6 & 1;
                if (u)
                    v = (u64)(imm8 >> 7) << 63 | (u64)(b6 ^ 1) << 62 | (b6 ? (u64)0xff << 54 : 0) | (u64)(imm8 & 63) << 48;
                else {
                    v = (u64)(imm8 >> 7) << 31 | (u64)(b6 ^ 1) << 30 | (b6 ? (u64)0x1f << 25 : 0) | (u64)(imm8 & 63) << 19;
                    v |= v << 32;
                }
            }
            if (u && cmode < 14)
                v = ~v;
            r.d[0] = v;
            r.d[1] = q ? v : 0;
        }
        c->v[rd] = r;
        return 1;
    }
    return 0;
}

int fp_exec(Cpu *c, u32 insn)
{
    if ((insn & 0x5f200000) == 0x1e200000)
        return scalar(c, insn);
    if ((insn & 0x5f000000) == 0x1f000000) {
        int dbl = (int)(insn >> 22 & 3) == 1;
        double a = get(c, insn >> 10 & 31, dbl), x = get(c, RN, dbl);
        if ((insn >> 22 & 3) > 1)
            return 0;
        /* fmadd a + xy, fmsub a - xy, fnmadd -a - xy, fnmsub -a + xy: one rounding */
        if (insn >> 21 & 1)
            a = -a;
        if ((insn >> 21 & 1) != (insn >> 15 & 1))
            x = -x;
        set(c, RD, dbl, fused(x, get(c, RM, dbl), a, dbl));
        return 1;
    }
    return simd(c, insn);
}
