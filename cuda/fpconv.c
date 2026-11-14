/* Conversions between floating-point formats, bit-exact: IEEE half, bfloat16, tf32 and the 8-, 6- and 4-bit
   formats of the tensor-core instructions. Every source value is exactly a double, so one routine that
   rounds a double into a format does all of them with a single rounding. */
#include "ptx.h"

const McFmt mc_fmt_f16 = {5, 10, 15, 1, 0x7bff, 0x7fff};
const McFmt mc_fmt_bf16 = {8, 7, 127, 1, 0x7f7f, 0x7fff};
const McFmt mc_fmt_tf32 = {8, 10, 127, 1, 0x3fbff, 0x3ffff};
const McFmt mc_fmt_f32 = {8, 23, 127, 1, 0x7f7fffff, 0x7fffffff};
const McFmt mc_fmt_e4m3 = {4, 3, 7, 0, 0x7e, 0x7f};
const McFmt mc_fmt_e5m2 = {5, 2, 15, 1, 0x7b, 0x7f};
const McFmt mc_fmt_e2m1 = {2, 1, 1, 0, 0x7, 0};
const McFmt mc_fmt_e2m3 = {2, 3, 1, 0, 0x1f, 0};
const McFmt mc_fmt_e3m2 = {3, 2, 3, 0, 0x1f, 0};

static u64 dbits(double d)
{
    union {
        double d;
        u64 u;
    } x;
    x.d = d;
    return x.u;
}

static double bitsd(u64 u)
{
    union {
        double d;
        u64 u;
    } x;
    x.u = u;
    return x.d;
}

static float bitsf(u32 u)
{
    union {
        float f;
        u32 u;
    } x;
    x.u = u;
    return x.f;
}

static u32 fbits(float f)
{
    union {
        float f;
        u32 u;
    } x;
    x.f = f;
    return x.u;
}

/* Round v into format f. rmode: MC_RN, MC_RZ, MC_RM, MC_RP, MC_RNA. sat: results beyond the largest finite
   value become that value (.satfinite), and formats without a NaN turn a NaN into it. */
u32 mc_enc(const McFmt *f, double v, int rmode, int sat)
{
    u64 b = dbits(v), m, q, rem, half;
    u32 sign = (u32)(b >> 63), sbit = f->ebits + f->mbits, code;
    s64 e = (s64)(b >> 52 & 0x7ff), te, shift;
    int up;
    m = b & 0xfffffffffffffUL;
    if (e == 0x7ff) {
        if (m)
            return f->nancode ? f->nancode : f->maxcode;
        if (f->ieee && !sat)
            return sign << sbit | (f->maxcode + 1);
        return sign << sbit | f->maxcode;
    }
    if (e == 0) {
        if (m == 0)
            return sign << sbit;
        e = 1;
        while (!(m >> 52)) {
            m <<= 1;
            e--;
        }
    } else
        m |= 1UL << 52;
    te = e - 1023 + f->bias;
    shift = 52 - f->mbits;
    if (te < 1) {
        shift += 1 - te;
        te = 0;
    }
    if (shift >= 64) {
        q = 0;
        rem = 1;
        half = 2;
    } else {
        q = m >> shift;
        rem = m & ((1UL << shift) - 1);
        half = 1UL << (shift - 1);
    }
    up = 0;
    switch (rmode) {
    case MC_RN:
        up = rem > half || (rem == half && (q & 1));
        break;
    case MC_RM:
        up = sign && rem;
        break;
    case MC_RP:
        up = !sign && rem;
        break;
    case MC_RNA:
        up = rem >= half;
        break;
    }
    q += up;
    if (te > (1L << f->ebits))
        code = 0xffffffffU;
    else if (te == 0)
        code = (u32)q;
    else
        code = (u32)(((u64)(te - 1) << f->mbits) + q);
    if (code > f->maxcode) {
        if (f->ieee && !sat) {
            int inf = rmode == MC_RN || rmode == MC_RNA || (rmode == MC_RM && sign) || (rmode == MC_RP && !sign);
            code = inf ? f->maxcode + 1 : f->maxcode;
        } else
            code = f->maxcode;
    }
    return sign << sbit | code;
}

static double pow2(int n) { return bitsd((u64)(n + 1023) << 52); }

double mc_dec(const McFmt *f, u32 code)
{
    u32 sbit = f->ebits + f->mbits, sign = code >> sbit & 1;
    u32 te = code >> f->mbits & ((1U << f->ebits) - 1), frac = code & ((1U << f->mbits) - 1);
    double v;
    if (f->ieee && te == (1U << f->ebits) - 1) {
        if (frac)
            return bitsd(0x7ff8000000000000UL);
        v = bitsd(0x7ff0000000000000UL);
    } else if (!f->ieee && f->nancode && (code & ((1U << sbit) - 1)) == f->nancode)
        return bitsd(0x7ff8000000000000UL);
    else if (te == 0)
        v = (double)frac * pow2(1 - f->bias - f->mbits);
    else
        v = (double)(frac + (1U << f->mbits)) * pow2((int)te - f->bias - f->mbits);
    return sign ? -v : v;
}

u32 mc_f32_to_f16(u32 x) { return mc_enc(&mc_fmt_f16, (double)bitsf(x), MC_RN, 0); }
u32 mc_f32_to_bf16(u32 x) { return mc_enc(&mc_fmt_bf16, (double)bitsf(x), MC_RN, 0); }

u32 mc_f16_to_f32(u32 h)
{
    u32 s = (h & 0x8000) << 16, e = h >> 10 & 31, m = h & 0x3ff;
    if (e == 31)
        return s | 0x7f800000 | m << 13;
    if (e == 0) {
        if (m == 0)
            return s;
        e = 1;
        while (!(m & 0x400)) {
            m <<= 1;
            e--;
        }
        m &= 0x3ff;
    }
    return s | (e + 112) << 23 | m << 13;
}

u32 mc_bf16_to_f32(u32 h) { return h << 16; }

/* ue8m0: 2^(code - 127), no sign, no zero; 0xff is NaN. Only .rz and .rp exist. */
u32 mc_enc_ue8m0(double v, int rmode, int sat)
{
    u64 b = dbits(v), m = b & 0xfffffffffffffUL;
    s64 e = (s64)(b >> 52 & 0x7ff);
    (void)sat;
    if (e == 0x7ff)
        return m ? 0xff : 0xfe;
    if (b >> 63 || (e == 0 && m == 0))
        return 0; /* negative values and zero: the smallest scale */
    if (e == 0) {
        e = 1;
        while (!(m >> 52)) {
            m <<= 1;
            e--;
        }
        m &= 0xfffffffffffffUL;
    }
    e = e - 1023 + 127;
    if (m && rmode == MC_RP)
        e++;
    if (e < 0)
        return 0;
    if (e > 0xfe)
        return 0xfe;
    return (u32)e;
}

double mc_dec_ue8m0(u32 code)
{
    code &= 0xff;
    if (code == 0xff)
        return bitsd(0x7ff8000000000000UL);
    return pow2((int)code - 127);
}

float mc_h2f_tab[65536];
static int h2f_ready;

void mc_fp_init(void)
{
    u32 i;
    if (h2f_ready)
        return;
    for (i = 0; i < 65536; i++)
        mc_h2f_tab[i] = bitsf(mc_f16_to_f32(i));
    h2f_ready = 1;
}

/* Round to an integral value in the given mode. */
double mc_rint(double d, int rmode)
{
    switch (rmode) {
    case MC_RZ:
        return mc_rintz(d);
    case MC_RM:
        return mc_rintm(d);
    case MC_RP:
        return mc_rintp(d);
    case MC_RNA:
        return mc_rinta(d);
    }
    return mc_rintn(d);
}

/* The rounding mode of the host for what follows; returns the previous control word for mc_setfpcr. */
u64 mc_round_begin(int rmode)
{
    u64 old = mc_getfpcr(), rm = 0;
    switch (rmode) {
    case MC_RP:
        rm = 1;
        break;
    case MC_RM:
        rm = 2;
        break;
    case MC_RZ:
        rm = 3;
        break;
    }
    mc_setfpcr((old & ~(3UL << 22)) | rm << 22);
    return old;
}

u32 mc_fbits(float f) { return fbits(f); }
float mc_bitsf(u32 u) { return bitsf(u); }
u64 mc_dbits(double d) { return dbits(d); }
double mc_bitsd(u64 u) { return bitsd(u); }
