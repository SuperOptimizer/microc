/* Half-precision floats: the compiler keeps a _Float16 as a float of the
   same value and calls these to move between that and the 16 stored bits. */

static unsigned f2h_bits(unsigned x)
{
    unsigned sign = (x >> 16) & 0x8000u, m = x & 0x7fffffu, v, rem, half;
    int e = (int)((x >> 23) & 0xff);
    if (e == 255)
        return sign | 0x7c00u | (m ? 0x200u | (m >> 13) : 0);
    e = e - 127 + 15;
    if (e >= 31)
        return sign | 0x7c00u;
    if (e <= 0) {
        int sh;
        if (e < -10)
            return sign;
        m |= 0x800000u;
        sh = 14 - e;
        v = m >> sh;
        rem = m & ((1u << sh) - 1);
        half = 1u << (sh - 1);
        if (rem > half || (rem == half && (v & 1)))
            v++;
        return sign | v;
    }
    v = ((unsigned)e << 10) | (m >> 13);
    rem = m & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (v & 1)))
        v++;
    return sign | v;
}

static unsigned h2f_bits(unsigned h)
{
    unsigned sign = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3ffu;
    if (e == 31)
        return sign | 0x7f800000u | (m << 13);
    if (e == 0) {
        if (!m)
            return sign;
        e = 1;
        while (!(m & 0x400u)) {
            m <<= 1;
            e--;
        }
        m &= 0x3ffu;
        return sign | ((e + 127 - 15) << 23) | (m << 13);
    }
    return sign | ((e + 127 - 15) << 23) | (m << 13);
}

unsigned __mcc_f2h(float f)
{
    union { float f; unsigned u; } c;
    c.f = f;
    return f2h_bits(c.u);
}

float __mcc_h2f(unsigned h)
{
    union { float f; unsigned u; } c;
    c.u = h2f_bits(h & 0xffffu);
    return c.f;
}

float __mcc_fround16(float f)
{
    return __mcc_h2f(__mcc_f2h(f));
}

/* double to half in one rounding: the float in between is rounded to odd */
float __mcc_dround16(double d)
{
    union { float f; unsigned u; } c;
    c.f = (float)d;
    if (d == d && (double)c.f != d && !(c.u & 1)) {
        double a = d < 0 ? -d : d, b = c.f < 0 ? -(double)c.f : (double)c.f;
        if (a > b)
            c.u++;
        else
            c.u--;
    }
    return __mcc_h2f(f2h_bits(c.u));
}

/* bfloat16: the top 16 bits of a float, rounded to nearest even. */
unsigned __mcc_f2bf(float f)
{
    union { float f; unsigned u; } c;
    c.f = f;
    if ((c.u & 0x7fffffffu) > 0x7f800000u)
        return (c.u >> 16) | 0x40u; /* a NaN stays one */
    return (c.u + 0x7fffu + ((c.u >> 16) & 1)) >> 16;
}

float __mcc_bf2f(unsigned h)
{
    union { float f; unsigned u; } c;
    c.u = (h & 0xffffu) << 16;
    return c.f;
}

float __mcc_froundbf(float f)
{
    return __mcc_bf2f(__mcc_f2bf(f));
}
