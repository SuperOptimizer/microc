/* The instructions that are more than a line of C: conversions with modifiers, floating point with rounding
   modes and half precision, atomics, the warp-synchronous instructions (shuffles, votes, the tensor-core
   matrix products and loads), and the functions a module may call without defining them. */
#include "ptx.h"
#include <sys/time.h>
#include <math.h>

#define NOSLOT 0x7fffffff

static u32 ftz32(u32 x) { return (x & 0x7f800000U) ? x : x & 0x80000000U; }

/* ---- cvt ---- */

static const McFmt *small_fmt(int t)
{
    switch (t) {
    case T_E4M3X2:
        return &mc_fmt_e4m3;
    case T_E5M2X2:
        return &mc_fmt_e5m2;
    case T_E2M1X2:
        return &mc_fmt_e2m1;
    case T_E2M3X2:
        return &mc_fmt_e2m3;
    case T_E3M2X2:
        return &mc_fmt_e3m2;
    }
    return 0;
}

static int tbits_(int t) { return mc_tbits(t); }
static int is_int(int t) { return t >= T_B8 && t <= T_S64; }
static int is_sint(int t) { return t >= T_S8 && t <= T_S64; }

/* a value into one of the 16-bit formats, with the modifiers of cvt */
static u32 enc_half(const McFmt *f, double v, u32 spec)
{
    int rm = (int)(spec >> 10 & 7), satf = (int)(spec >> 17 & 1), relu = (int)(spec >> 16 & 1), pzo = (int)(spec >> 18 & 1);
    u32 r, sbit = 1U << (f->ebits + f->mbits);
    if (relu && v != v)
        return f->nancode ? f->nancode : f->maxcode;
    r = mc_enc(f, v, rm, satf);
    if (relu && (r & sbit))
        r = 0;
    if (pzo && r == sbit)
        r = 0;
    return r;
}

static double dec_any(int t, u32 bits)
{
    switch (t) {
    case T_F16:
        return (double)mc_h2f_tab[bits & 0xffff];
    case T_BF16:
        return (double)mc_bitsf(bits << 16);
    }
    return 0;
}

/* 0 if the conversion exists; *out gets the result. */
static int cvt_do(u32 spec, u64 a, u64 b, u64 *out)
{
    int dt = (int)(spec & 31), st = (int)(spec >> 5 & 31), rm = (int)(spec >> 10 & 7), irnd = (int)(spec >> 13 & 1);
    int ftz = (int)(spec >> 14 & 1), sat = (int)(spec >> 15 & 1), relu = (int)(spec >> 16 & 1), satf = (int)(spec >> 17 & 1);
    int rbc = (int)(spec >> 19 & 3), two = (int)(spec >> 22 & 1), rb = rbc == 0 ? 8 : rbc == 1 ? 16 : rbc == 2 ? 32 : 64;
    int sb = tbits_(st), db = tbits_(dt), src_int = is_int(st), src_neg = 0;
    double v = 0, v2 = 0;
    u64 iv = 0, r = 0, old;
    const McFmt *sf = small_fmt(st), *df = small_fmt(dt);
    /* the source */
    if (src_int) {
        iv = sb < 64 ? a & ((1UL << sb) - 1) : a;
        if (is_sint(st) && sb < 64 && (iv >> (sb - 1) & 1))
            iv |= ~0UL << sb;
        src_neg = is_sint(st) && (s64)iv < 0;
    } else if (st == T_F32) {
        v = (double)mc_bitsf(ftz ? ftz32((u32)a) : (u32)a);
        v2 = (double)mc_bitsf(ftz ? ftz32((u32)b) : (u32)b);
    } else if (st == T_F64)
        v = mc_bitsd(a);
    else if (st == T_F16 || st == T_BF16)
        v = dec_any(st, (u32)a);
    else if (st == T_F16X2 || st == T_BF16X2) {
        v = dec_any(st == T_F16X2 ? T_F16 : T_BF16, (u32)a >> 16);
        v2 = dec_any(st == T_F16X2 ? T_F16 : T_BF16, (u32)a);
        two = 1;
    } else if (sf) {
        two = 1;
        if (st == T_E2M1X2) {
            v = mc_dec(sf, (u32)a >> 4 & 15);
            v2 = mc_dec(sf, (u32)a & 15);
        } else {
            v = mc_dec(sf, (u32)a >> 8 & 0xff);
            v2 = mc_dec(sf, (u32)a & 0xff);
        }
    } else if (st == T_UE8M0X2) {
        v = mc_dec_ue8m0((u32)a >> 8);
        v2 = mc_dec_ue8m0((u32)a);
        two = 1;
    } else
        return -1;
    /* the destination */
    if (is_int(dt)) {
        int dsg = is_sint(dt);
        if (two)
            return -1;
        if (src_int) {
            if (!sat)
                return -1; /* done without us */
            if (dsg) {
                s64 lo = db == 64 ? (s64)0x8000000000000000UL : -(1L << (db - 1)), hi = db == 64 ? 0x7fffffffffffffffL : (1L << (db - 1)) - 1;
                s64 x = (s64)iv;
                if (!is_sint(st) && iv > (u64)hi)
                    x = hi;
                else if (x < lo)
                    x = lo;
                else if (x > hi)
                    x = hi;
                r = (u64)x;
            } else {
                u64 hi = db == 64 ? ~0UL : (1UL << db) - 1;
                r = src_neg ? 0 : iv > hi ? hi : iv;
            }
        } else if (st == T_F32 || st == T_F64 || st == T_F16 || st == T_BF16) {
            if (!irnd)
                return -1;
            if (v != v) {
                /* The PTX manual has 0. The hardware (seen on sm_120) has 0 only from a float (or a half) to 32
                   bits or fewer, and otherwise the top bit of the type: -128 for .s8 and 128 for .u8 from a double. */
                if (db == 64 || st == T_F64)
                    r = dsg ? (u64)0 - (1UL << (db - 1)) : 1UL << (db - 1);
                else
                    r = 0;
            } else {
                v = mc_rint(v, rm);
                if (dsg) {
                    double lim = db == 64 ? 9223372036854775808.0 : (double)(1L << (db - 1));
                    r = v >= lim ? (db == 64 ? 0x7fffffffffffffffUL : (1UL << (db - 1)) - 1) : v < -lim ? (u64)0 - (db == 64 ? 1UL << 63 : 1UL << (db - 1)) : (u64)(s64)v;
                } else {
                    double lim = db == 64 ? 18446744073709551616.0 : (double)(1UL << db);
                    r = v <= 0 ? 0 : v >= lim ? (db == 64 ? ~0UL : (1UL << db) - 1) : v >= 9223372036854775808.0 ? (u64)(v - 9223372036854775808.0) + (1UL << 63) : (u64)v;
                }
            }
        } else
            return -1;
        if (db < 64)
            r &= (1UL << db) - 1;
        if (dsg && db < rb && (r >> (db - 1) & 1))
            r |= ~0UL << db;
        if (rb < 64)
            r &= (1UL << rb) - 1;
        *out = r;
        return 0;
    }
    if (dt == T_F32 || dt == T_F64) {
        Slot x;
        if (two || sf || st == T_UE8M0X2 || st == T_F16X2 || st == T_BF16X2)
            return -1;
        x.u = 0;
        if (src_int) {
            old = mc_round_begin(rm);
            if (dt == T_F32)
                x.f = is_sint(st) ? (float)(s64)iv : (float)iv;
            else
                x.d = is_sint(st) ? (double)(s64)iv : (double)iv;
            mc_setfpcr(old);
        } else {
            if (irnd)
                v = mc_rint(v, rm);
            if (sat)
                v = v != v ? 0.0 : v < 0 ? 0.0 : v > 1 ? 1.0 : v;
            if (dt == T_F32) {
                old = mc_round_begin(rm);
                x.f = (float)v;
                mc_setfpcr(old);
                if (ftz)
                    x.w = ftz32(x.w);
            } else
                x.d = v;
        }
        *out = dt == T_F32 ? (u64)x.w : x.u;
        return 0;
    }
    if (src_int) { /* integers to the small formats go through a double */
        old = mc_round_begin(rm);
        v = is_sint(st) ? (double)(s64)iv : (double)iv;
        mc_setfpcr(old);
    }
    switch (dt) {
    case T_F16:
        if (two)
            return -1;
        *out = enc_half(&mc_fmt_f16, v, spec);
        return 0;
    case T_BF16:
        if (two)
            return -1;
        *out = enc_half(&mc_fmt_bf16, v, spec);
        return 0;
    case T_TF32:
        if (two || st != T_F32)
            return -1;
        *out = (u64)enc_half(&mc_fmt_tf32, v, spec) << 13;
        return 0;
    case T_F16X2:
    case T_BF16X2:
        if (!two)
            return -1;
        df = dt == T_F16X2 ? &mc_fmt_f16 : &mc_fmt_bf16;
        *out = (u64)enc_half(df, v, spec) << 16 | enc_half(df, v2, spec);
        return 0;
    case T_UE8M0X2:
        if (!two || (rm != MC_RZ && rm != MC_RP))
            return -1;
        *out = (u64)mc_enc_ue8m0(v, rm, satf) << 8 | mc_enc_ue8m0(v2, rm, satf);
        return 0;
    }
    if (df) {
        if (!two || !satf || (rm != MC_RN && rm != MC_RZ) || sf)
            return -1;
        if (dt == T_E2M1X2)
            *out = (u64)enc_half(df, v, spec) << 4 | enc_half(df, v2, spec);
        else
            *out = (u64)enc_half(df, v, spec) << 8 | enc_half(df, v2, spec);
        (void)relu;
        return 0;
    }
    return -1;
}

u64 mc_cvt(u32 spec, u64 a, u64 b, u64 c)
{
    u64 r = 0;
    (void)c;
    cvt_do(spec, a, b, &r);
    return r;
}

int mc_cvt_ok(u32 spec)
{
    u64 r;
    return cvt_do(spec, 0, 0, &r) == 0;
}

/* ---- floating point, the general case ---- */

static double sat01(double v) { return v != v ? 0.0 : v < 0 ? 0.0 : v > 1 ? 1.0 : v; }

static const double LN2 = 0.69314718055994530942;

static double log2_(double x)
{
    int e;
    double m;
    if (!(x > 0) || x != x || x > 1.7e308)
        return log(x) / LN2;
    m = frexp(x, &e);
    return (double)(e - 1) + log(m * 2) / LN2;
}

static double exp2_(double x)
{
    double fl;
    if (x != x)
        return x;
    if (x > 1100)
        return mc_bitsd(0x7ff0000000000000UL);
    if (x < -1100)
        return 0;
    fl = floor(x);
    return ldexp(exp((x - fl) * LN2), (int)fl);
}

static double tanh_(double x)
{
    double e;
    if (x != x)
        return x;
    if (x > 20)
        return 1;
    if (x < -20)
        return -1;
    if (x > -1e-4 && x < 1e-4)
        return x - x * x * x / 3;
    e = exp(2 * x);
    return (e - 1) / (e + 1);
}

static double minmax(int max, double a, double b, u32 flags)
{
    int xs = 0;
    double r;
    if (flags & FF_XORSIGN)
        xs = (int)((mc_dbits(a) ^ mc_dbits(b)) >> 63);
    if (flags & FF_ABS) {
        a = a < 0 ? -a : mc_bitsd(mc_dbits(a) & 0x7fffffffffffffffUL);
        b = b < 0 ? -b : mc_bitsd(mc_dbits(b) & 0x7fffffffffffffffUL);
    }
    if (a != a || b != b) {
        if ((flags & FF_NAN) || (a != a && b != b))
            return mc_bitsd(0x7ff8000000000000UL);
        return a != a ? b : a;
    }
    if (a < b)
        r = max ? b : a;
    else if (b < a)
        r = max ? a : b;
    else /* equal, or zeros of both signs: -0 is the smaller */
        r = mc_bitsd(max ? mc_dbits(a) & mc_dbits(b) : mc_dbits(a) | mc_dbits(b));
    if (flags & FF_XORSIGN)
        r = mc_bitsd((mc_dbits(r) & 0x7fffffffffffffffUL) | (u64)xs << 63);
    return r;
}

/* the operation on doubles; single is set when the result must be rounded as a float operation would */
static double fop(u32 sub, u32 flags, double a, double b, double c, int single)
{
    float fa = (float)a, fb = (float)b, fc = (float)c;
    switch (sub) {
    case FG_ADD:
        return single ? (double)(fa + fb) : a + b;
    case FG_SUB:
        return single ? (double)(fa - fb) : a - b;
    case FG_MUL:
        return single ? (double)(fa * fb) : a * b;
    case FG_DIV:
        return single ? (double)(fa / fb) : a / b;
    case FG_FMA:
        return single ? (double)mc_fmaf(fa, fb, fc) : mc_fma(a, b, c);
    case FG_MIN:
    case FG_MAX:
        a = minmax(sub == FG_MAX, a, b, flags);
        if (flags & FF_3SRC)
            a = minmax(sub == FG_MAX, a, (flags & FF_ABS) && c < 0 ? -c : c, flags & ~(u32)(FF_XORSIGN | FF_ABS));
        return a;
    case FG_ABS:
        return mc_bitsd(mc_dbits(a) & 0x7fffffffffffffffUL);
    case FG_NEG:
        return mc_bitsd(mc_dbits(a) ^ 0x8000000000000000UL);
    case FG_RCP:
        return single ? (double)(1.0f / fa) : 1.0 / a;
    case FG_SQRT:
        return single ? (double)mc_sqrtf(fa) : mc_sqrt(a);
    case FG_RSQRT:
        return 1.0 / mc_sqrt(a);
    case FG_SIN:
        return sin(a);
    case FG_COS:
        return cos(a);
    case FG_LG2:
        return log2_(a);
    case FG_EX2:
        return exp2_(a);
    case FG_TANH:
        return tanh_(a);
    case FG_COPYSIGN:
        return mc_bitsd((mc_dbits(b) & 0x7fffffffffffffffUL) | (mc_dbits(a) & 0x8000000000000000UL));
    }
    return 0;
}

static u32 half_op(u32 sub, u32 flags, const McFmt *f, int t, u32 a, u32 b, u32 c)
{
    double r = fop(sub, flags, dec_any(t, a), dec_any(t, b), dec_any(t, c), 0);
    if (flags & FF_SAT)
        r = sat01(r);
    return mc_enc(f, r, (int)(flags & FF_RMODE), 0);
}

u64 mc_fgen(u32 sub, u32 flags, Slot a, Slot b, Slot c)
{
    int t = (int)(flags >> FF_TYPE_SHIFT & 0xff), rm = (int)(flags & FF_RMODE);
    u64 old = 0;
    double r;
    Slot x;
    x.u = 0;
    switch (t) {
    case T_F32:
        if (flags & FF_FTZ) {
            a.w = ftz32(a.w);
            b.w = ftz32(b.w);
            c.w = ftz32(c.w);
        }
        if (rm != MC_RN)
            old = mc_round_begin(rm);
        r = fop(sub, flags, (double)a.f, (double)b.f, (double)c.f, 1);
        x.f = (float)r;
        if (rm != MC_RN)
            mc_setfpcr(old);
        if (flags & FF_SAT)
            x.f = (float)sat01((double)x.f);
        if (flags & FF_FTZ)
            x.w = ftz32(x.w);
        return x.w;
    case T_F64:
        if (rm != MC_RN)
            old = mc_round_begin(rm);
        x.d = fop(sub, flags, a.d, b.d, c.d, 0);
        if (rm != MC_RN)
            mc_setfpcr(old);
        if (flags & FF_SAT)
            x.d = sat01(x.d);
        return x.u;
    case T_F16:
        return half_op(sub, flags, &mc_fmt_f16, T_F16, a.w, b.w, c.w);
    case T_BF16:
        return half_op(sub, flags, &mc_fmt_bf16, T_BF16, a.w, b.w, c.w);
    case T_F16X2:
        return (u64)half_op(sub, flags, &mc_fmt_f16, T_F16, a.w >> 16, b.w >> 16, c.w >> 16) << 16 |
               half_op(sub, flags, &mc_fmt_f16, T_F16, a.w, b.w, c.w);
    case T_BF16X2:
        return (u64)half_op(sub, flags, &mc_fmt_bf16, T_BF16, a.w >> 16, b.w >> 16, c.w >> 16) << 16 |
               half_op(sub, flags, &mc_fmt_bf16, T_BF16, a.w, b.w, c.w);
    }
    return 0;
}

/* ---- atom, red ---- */

u64 mc_atom(Blk *b, u32 spec, u8 *p, u64 x, u64 y)
{
    u32 op = spec & 15, t = spec >> 4;
    int bits = mc_tbits((int)t), sg = t >= T_S8 && t <= T_S64;
    u64 old, nv;
    Slot o, v, r;
    (void)b;
    mcuda_lock_addr(p);
    old = bits == 64 ? *(u64 *)p : bits == 32 ? *(u32 *)p : *(u16 *)p;
    if (bits < 64)
        x &= (1UL << bits) - 1, y &= (1UL << bits) - 1;
    o.u = old;
    v.u = x;
    r.u = 0;
    nv = old;
    switch (op) {
    case AT_AND:
        nv = old & x;
        break;
    case AT_OR:
        nv = old | x;
        break;
    case AT_XOR:
        nv = old ^ x;
        break;
    case AT_CAS:
        nv = old == x ? y : old;
        break;
    case AT_EXCH:
        nv = x;
        break;
    case AT_ADD:
        if (t == T_F32) {
            r.f = o.f + v.f;
            nv = r.w;
        } else if (t == T_F64) {
            r.d = o.d + v.d;
            nv = r.u;
        } else if (t == T_F16)
            nv = mc_enc(&mc_fmt_f16, (double)mc_h2f_tab[old & 0xffff] + (double)mc_h2f_tab[x & 0xffff], MC_RN, 0);
        else if (t == T_BF16)
            nv = mc_enc(&mc_fmt_bf16, (double)mc_bitsf((u32)old << 16) + (double)mc_bitsf((u32)x << 16), MC_RN, 0);
        else
            nv = old + x;
        break;
    case AT_INC:
        nv = old >= x ? 0 : old + 1;
        break;
    case AT_DEC:
        nv = (old == 0 || old > x) ? x : old - 1;
        break;
    case AT_MIN:
    case AT_MAX: {
        int less;
        if (t == T_F32)
            less = v.f < o.f;
        else if (t == T_F64)
            less = v.d < o.d;
        else if (sg)
            less = bits == 64 ? (s64)x < (s64)old : (s32)x < (s32)old;
        else
            less = x < old;
        if (op == AT_MIN)
            nv = less ? x : old;
        else {
            int more;
            if (t == T_F32)
                more = v.f > o.f;
            else if (t == T_F64)
                more = v.d > o.d;
            else if (sg)
                more = bits == 64 ? (s64)x > (s64)old : (s32)x > (s32)old;
            else
                more = x > old;
            nv = more ? x : old;
        }
        break;
    }
    }
    if (bits == 64)
        *(u64 *)p = nv;
    else if (bits == 32)
        *(u32 *)p = (u32)nv;
    else
        *(u16 *)p = (u16)nv;
    mcuda_unlock_addr(p);
    return old;
}

/* ---- warp-synchronous instructions ---- */

static double mma_dec(int ty, u32 e, int kind4)
{
    switch (ty) {
    case 0:
        return (double)mc_h2f_tab[e & 0xffff];
    case 1:
        return (double)mc_bitsf((e & 0xffff) << 16);
    case 2:
        return (double)mc_bitsf(e & ~0x1fffU);
    case 5:
        return mc_dec(&mc_fmt_e4m3, e & 0xff);
    case 6:
        return mc_dec(&mc_fmt_e5m2, e & 0xff);
    case 7:
        /* (in a byte it is bits 2 to 5; the hardware reads bits 0 to 5 as e2m3, the same if bits 0 and 1 are 0) */
        return kind4 ? mc_dec(&mc_fmt_e2m1, e & 15) : mc_dec(&mc_fmt_e2m3, e & 0x3f);
    case 8:
        return mc_dec(&mc_fmt_e2m3, e & 0x3f);
    case 9:
        return mc_dec(&mc_fmt_e3m2, e & 0x3f);
    }
    return 0;
}

static s32 mma_int(int ty, u32 e)
{
    switch (ty) {
    case 3:
        return (s32)(s8)e;
    case 4:
        return (s32)(e & 0xff);
    case 10:
        return (s32)((e & 15) ^ 8) - 8;
    }
    return (s32)(e & 15);
}

/* D = A B + C for the 16 x 8 x K product a warp holds in fragments. */
/* MCUDA_MMA=0: the sums of mma in plain C, to check mc_mma_rows against */
static int mma_scalar(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MCUDA_MMA");
        v = e && e[0] == '0';
    }
    return v;
}

/* MCUDA_MMA_HW=1: the sums of a floating-point mma as the hardware makes them (seen on sm_120; tests/cuda/
   mmahw.py has the model and tests/cuda/hw/mma.txt the device's words it was found from). The default adds
   in double precision and rounds once, which is closer to the true sum; the hardware cuts, and how it cuts
   is not the same on every generation of it. All the kinds that are not of integers are covered; a sum that
   has an infinity or a NaN among its terms is made as without the switch. */
static int mma_hw_v = -1;

static int mma_hw(void)
{
    if (mma_hw_v < 0) {
        const char *e = getenv("MCUDA_MMA_HW");
        mma_hw_v = e && e[0] == '1';
    }
    return mma_hw_v;
}

void mc_mma_hw(int on) { mma_hw_v = on != 0; }

/* the exponent of a number that is not zero, as the hardware has it: its field's, so not below min */
static int hw_exp(double v, int min)
{
    int e = (int)(mc_dbits(v) >> 52 & 0x7ff) - 1023;
    return e < min ? min : e;
}

static const s16 hw_emin[10] = {-14, -126, -126, 0, 0, -6, -14, 0, 0, -2};

/* c + the sum of a[k] * b[8 k], k < K, the hardware's way: every term cut toward zero to a multiple of
   2^(E - 25), E the largest exponent among them (a product's is the sum of its factors'), an exact sum, and
   that cut to .f32 or (half) rounded to .f16. ea, eb: the exponents, a large negative number for a zero. */
/* S 2^e cut toward zero to .f32; a zero is +0 */
static u32 hw_f32(s64 S, int e)
{
    u64 m = (u64)(S < 0 ? -S : S);
    double d;
    int n;
    if (S == 0)
        return 0;
    for (n = 0; m >> n > 0xffffff; n++)
        ;
    m = m >> n << n;
    d = (double)m * mc_bitsd((u64)(1023 + e) << 52);
    if (d >= mc_bitsd((u64)(1023 + 128) << 52))
        return S < 0 ? 0xff800000U : 0x7f800000U;
    if (d < mc_bitsd((u64)(1023 - 126) << 52)) { /* subnormal: cut at 2^-149 */
        m = (u64)(d * mc_bitsd((u64)(1023 + 149) << 52));
        return m ? (u32)m | (S < 0 ? 0x80000000U : 0) : 0;
    }
    return mc_fbits((float)d) | (S < 0 ? 0x80000000U : 0);
}

/* The same for kind::mxf4 and mxf4nvf4 (e2m1 in four bits, K = 64, each chunk of the K with a scale, here
   sc[] with exponents se[]), which the hardware does another way: the products are added sixteen at a time,
   exactly; each such sum times its scale is cut toward zero to a multiple of 2^(E - 35), and so is c; E is
   the largest of c's exponent and the exponents of the scales of the chunks with a product that is not 0. */
static u32 mma_hw_sum4(const double *a, const double *b, int K, int chunk, const int *se, double c)
{
    int k, g, E = -100000, n;
    s64 S = 0;
    double I[4], z;
    if (c != 0)
        E = hw_exp(c, -126);
    for (g = 0; g < K / 16; g++) {
        I[g] = 0;
        z = 0;
        for (k = g * 16; k < g * 16 + 16; k++) {
            double p = a[k] * b[8 * k];
            I[g] += p;
            z += p < 0 ? -p : p;
        }
        if (z != 0 && se[g * 16 / chunk] > E)
            E = se[g * 16 / chunk];
    }
    if (E < -50000)
        return 0;
    for (g = 0; g < K / 16; g++) {
        n = se[g * 16 / chunk] - E + 35;
        if (n > -900)
            S += (s64)(I[g] * mc_bitsd((u64)(1023 + n) << 52));
    }
    S += (s64)(c * mc_bitsd((u64)(1023 + 35 - E) << 52));
    return hw_f32(S, E - 35);
}

static u32 mma_hw_sum(const double *a, const int *ea, const double *b, const int *eb, int K, double c, int half)
{
    int k, E = -100000, e;
    s64 S = 0;
    u64 m;
    double sc, d;
    if (c != 0)
        E = hw_exp(c, half ? -14 : -126);
    for (k = 0; k < K; k++) {
        e = ea[k] + eb[8 * k];
        if (e > E)
            E = e;
    }
    if (E < -50000)
        return 0;
    sc = mc_bitsd((u64)(1023 + 25 - E) << 52);
    for (k = 0; k < K; k++)
        S += (s64)(a[k] * b[8 * k] * sc);
    S += (s64)(c * sc);
    if (S == 0)
        return 0;
    m = (u64)(S < 0 ? -S : S);
    if (half) {
        u32 r;
        d = (double)m * mc_bitsd((u64)(1023 + E - 25) << 52);
        r = mc_enc(&mc_fmt_f16, d, MC_RN, 0);
        return r & 0x7fff ? r | (S < 0 ? 0x8000U : 0) : 0;
    }
    return hw_f32(S, E - 25);
}

static int do_mma(Blk *b, Thr **grp)
{
    double Af[16 * 64], Bf[64 * 8];
    int Ea[16 * 64], Eb[64 * 8];
    s32 Ai[16 * 64], Bi[64 * 8];
    u32 out[32][4];
    Thr *t0 = grp[0];
    s32 *x;
    int K, ta, tb, acc, sat, bs, na, nb, nc, ebits, epr, lane, r, w, i, j, k, isint, kind4, hw, hwnf = 0;
    u32 emask;
    for (lane = 0; lane < 32; lane++)
        if (!grp[lane]) {
            for (i = 0; i < 32 && !grp[i]; i++)
                ;
            mc_fault(b, i < 32 ? grp[i] : 0, "mma.sync needs all 32 threads of the warp%s", 0, 0);
            return -1;
        }
    x = t0->f->xtra + t0->ip->e;
    K = x[1];
    ta = x[2];
    tb = x[3];
    acc = x[4];
    sat = x[5];
    bs = x[6];
    na = x[7];
    nb = x[8];
    nc = x[9];
    isint = acc == 2;
    ebits = 32 * 32 * na / (16 * K); /* the 16 K elements of A lie in the na registers of 32 threads */
    kind4 = ebits == 4;
    epr = 32 / ebits;
    emask = ebits == 32 ? 0xffffffffU : (1U << ebits) - 1;
    hw = !isint && mma_hw();
    if (!isint && !bs && acc == 0 && ebits == 16 && ta == 0 && tb == 0 && K == 16 && na == 4 && nb == 2 && nc == 4 &&
        !mma_scalar()) {
        /* m16n8k16 of f16 with sums in f32, by far the most frequent: the operands are laid out as matrices,
           and mc_mma_f16 does the rest. (Its sums round as the ones below do, because a product of two f16
           is exact in a double. If one of its results is a NaN it says so, and which NaN that is is left to
           the code below.) */
        u32 A16[128], C32[128];
        u16 B16[128];
        double W[640];
        Slot *Rs[32];
        int same = 1;
        for (lane = 0; lane < 32; lane++) {
            Rs[lane] = grp[lane]->R;
            same &= grp[lane]->ip == t0->ip;
        }
        x += 10;
        if (same)
            mc_mma_get16(Rs, x + 4, A16, B16, C32); /* what the loop below does */
        else
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10 + 4;
            Slot *R = t->R;
            int g = lane >> 2, tt = lane & 3;
            u32 *ap = &A16[g * 8 + tt], *cp = &C32[g * 8 + 2 * tt], reg;
            u16 *bp = &B16[tt * 16 + g];
            ap[0] = R[xs[0]].w;
            ap[64] = R[xs[1]].w;
            ap[4] = R[xs[2]].w;
            ap[68] = R[xs[3]].w;
            reg = R[xs[4]].w;
            bp[0] = (u16)reg;
            bp[8] = (u16)(reg >> 16);
            reg = R[xs[5]].w;
            bp[64] = (u16)reg;
            bp[72] = (u16)(reg >> 16);
            cp[0] = R[xs[6]].w;
            cp[1] = R[xs[7]].w;
            cp[64] = R[xs[8]].w;
            cp[65] = R[xs[9]].w;
        }
        if (!(hw ? mc_mma_f16hw(A16, B16, C32, W) : mc_mma_f16(A16, B16, C32, W))) {
            if (same)
                mc_mma_put(Rs, x, C32);
            else
            for (lane = 0; lane < 32; lane++) {
                Thr *t = grp[lane];
                s32 *xs = t->f->xtra + t->ip->e + 10;
                Slot *R = t->R;
                u32 *cp = &C32[(lane >> 2) * 8 + 2 * (lane & 3)];
                R[xs[0]].u = cp[0];
                R[xs[1]].u = cp[1];
                R[xs[2]].u = cp[64];
                R[xs[3]].u = cp[65];
            }
            return 0;
        }
        x -= 10;
    }
    if (isint && !sat && (ta == 3 || ta == 4) && (tb == 3 || tb == 4) && K == 32 && na == 4 && nb == 2 && nc == 4 &&
        !mma_scalar()) {
        /* m16n8k32 of bytes with sums in s32 that wrap: the same way, by mc_mma_s8 */
        u32 A8[128], C32[128];
        u8 B8[256];
        double W[384];
        Slot *Rs[32];
        int same = 1;
        for (lane = 0; lane < 32; lane++) {
            Rs[lane] = grp[lane]->R;
            same &= grp[lane]->ip == t0->ip;
        }
        x += 10;
        if (same)
            mc_mma_get8(Rs, x + 4, A8, B8, C32);
        else
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10 + 4;
            Slot *R = t->R;
            int g = lane >> 2, tt = lane & 3;
            u32 *ap = &A8[g * 8 + tt], *cp = &C32[g * 8 + 2 * tt], reg;
            u8 *bp = &B8[tt * 32 + g];
            ap[0] = R[xs[0]].w;
            ap[64] = R[xs[1]].w;
            ap[4] = R[xs[2]].w;
            ap[68] = R[xs[3]].w;
            reg = R[xs[4]].w;
            bp[0] = (u8)reg;
            bp[8] = (u8)(reg >> 8);
            bp[16] = (u8)(reg >> 16);
            bp[24] = (u8)(reg >> 24);
            reg = R[xs[5]].w;
            bp[128] = (u8)reg;
            bp[136] = (u8)(reg >> 8);
            bp[144] = (u8)(reg >> 16);
            bp[152] = (u8)(reg >> 24);
            cp[0] = R[xs[6]].w;
            cp[1] = R[xs[7]].w;
            cp[64] = R[xs[8]].w;
            cp[65] = R[xs[9]].w;
        }
        mc_mma_s8(A8, B8, C32, W, (long)((ta == 4) | (tb == 4) << 1));
        if (same)
            mc_mma_put(Rs, x, C32);
        else
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10;
            Slot *R = t->R;
            u32 *cp = &C32[(lane >> 2) * 8 + 2 * (lane & 3)];
            R[xs[0]].u = cp[0];
            R[xs[1]].u = cp[1];
            R[xs[2]].u = cp[64];
            R[xs[3]].u = cp[65];
        }
        return 0;
    }
    if (!isint && ebits == 16 && ta == 0 && tb == 0) {
        /* f16 operands, the usual ones: the same placement as below, without a call for every element */
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10 + nc;
            Slot *R = t->R;
            int g = lane >> 2, tt = lane & 3;
            for (r = 0; r < na; r++) {
                u32 reg = R[xs[r]].w;
                double *ap = &Af[(g + 8 * (r & 1)) * 64 + 2 * tt + 8 * (r >> 1)];
                ap[0] = (double)mc_h2f_tab[reg & 0xffff];
                ap[1] = (double)mc_h2f_tab[reg >> 16];
            }
            xs += na;
            for (r = 0; r < nb; r++) {
                u32 reg = R[xs[r]].w;
                int row = 2 * tt + 8 * r;
                Bf[row * 8 + g] = (double)mc_h2f_tab[reg & 0xffff];
                Bf[(row + 1) * 8 + g] = (double)mc_h2f_tab[reg >> 16];
            }
        }
    } else
    for (lane = 0; lane < 32; lane++) {
        Thr *t = grp[lane];
        s32 *xs = t->f->xtra + t->ip->e + 10 + nc;
        int g = lane >> 2, tt = lane & 3;
        for (r = 0; r < na; r++) {
            u32 reg = t->R[xs[r]].w;
            for (w = 0; w < epr; w++) {
                u32 e = reg >> (ebits * w) & emask;
                int row = g + 8 * (r & 1), col = epr * tt + w + 4 * epr * (r >> 1);
                if (isint)
                    Ai[row * 64 + col] = mma_int(ta, e);
                else
                    Af[row * 64 + col] = mma_dec(ta, e, kind4);
            }
        }
        xs += na;
        for (r = 0; r < nb; r++) {
            u32 reg = t->R[xs[r]].w;
            for (w = 0; w < epr; w++) {
                u32 e = reg >> (ebits * w) & emask;
                int row = epr * tt + w + 4 * epr * r;
                if (isint)
                    Bi[row * 8 + g] = mma_int(tb, e);
                else
                    Bf[row * 8 + g] = mma_dec(tb, e, kind4);
            }
        }
    }
    if (!isint && !bs && acc == 0 && nc == 4 && !mma_scalar() && !hw) {
        /* f32 sums, the usual case: all 128 of them by mc_mma_rows. It adds each product to its sum in one
           step, which rounds as multiplying and then adding does, because the products of these formats are
           exact in a double. */
        double S[16 * 8];
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10 + nc + na + nb;
            Slot *R = t->R;
            double *sp = &S[(lane >> 2) * 8 + 2 * (lane & 3)];
            sp[0] = (double)R[xs[0]].f;
            sp[1] = (double)R[xs[1]].f;
            sp[64] = (double)R[xs[2]].f;
            sp[65] = (double)R[xs[3]].f;
        }
        mc_mma_rows(Af, Bf, S, (long)K);
        /* (which NaN a sum of several becomes is left to the code below) */
        for (i = 0, j = 0; i < 128; i++)
            j |= S[i] != S[i];
        if (!j) {
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 10;
            Slot *R = t->R;
            double *sp = &S[(lane >> 2) * 8 + 2 * (lane & 3)];
            Slot v;
            /* (a zero is +0 on the hardware, whatever the signs) */
            v.f = (float)sp[0];
            R[xs[0]].u = v.w == 0x80000000U ? 0 : v.w;
            v.f = (float)sp[1];
            R[xs[1]].u = v.w == 0x80000000U ? 0 : v.w;
            v.f = (float)sp[64];
            R[xs[2]].u = v.w == 0x80000000U ? 0 : v.w;
            v.f = (float)sp[65];
            R[xs[3]].u = v.w == 0x80000000U ? 0 : v.w;
        }
        return 0;
        }
    }
    if (hw) {
        /* the exponents, and whether all of A and B is finite (1024 is the exponent of what is not) */
        for (i = 0; i < 16; i++)
            for (k = 0; k < K; k++) {
                double v = Af[i * 64 + k];
                r = v != 0 ? hw_exp(v, hw_emin[ta]) : -200000;
                Ea[i * 64 + k] = r;
                hwnf |= r == 1024;
            }
        for (k = 0; k < K * 8; k++) {
            r = Bf[k] != 0 ? hw_exp(Bf[k], hw_emin[tb]) : -200000;
            Eb[k] = r;
            hwnf |= r == 1024;
        }
    }
    /* Row by row; the eight columns of a row advance together, each sum in the order of k, so that the host
       works on eight independent chains of additions. */
    for (i = 0; i < 16; i++) {
        if (isint) {
            s64 sl[8];
            u32 su[8];
            for (j = 0; j < 8; j++) {
                Thr *t = grp[(i & 7) * 4 + (j >> 1)];
                s32 *xs = t->f->xtra + t->ip->e + 10 + nc + na + nb;
                su[j] = t->R[xs[(i >> 3) * 2 + (j & 1)]].w;
                sl[j] = (s32)su[j];
            }
            if (sat) {
                for (k = 0; k < K; k++) {
                    s64 a = Ai[i * 64 + k];
                    const s32 *bp = &Bi[k * 8];
                    for (j = 0; j < 8; j++)
                        sl[j] += a * bp[j];
                }
                for (j = 0; j < 8; j++)
                    su[j] = (u32)(sl[j] > 2147483647L ? 2147483647L : sl[j] < -2147483648L ? -2147483648L : sl[j]);
            } else
                for (k = 0; k < K; k++) {
                    s32 a = Ai[i * 64 + k];
                    const s32 *bp = &Bi[k * 8];
                    for (j = 0; j < 8; j++)
                        su[j] += (u32)(a * bp[j]);
                }
            for (j = 0; j < 8; j++)
                out[(i & 7) * 4 + (j >> 1)][(i >> 3) * 2 + (j & 1)] = su[j];
        } else {
            double sd[8];
            int hwdone = 0;
            for (j = 0; j < 8; j++) {
                Thr *t = grp[(i & 7) * 4 + (j >> 1)];
                s32 *xs = t->f->xtra + t->ip->e + 10 + nc + na + nb;
                if (acc == 1)
                    sd[j] = (double)mc_h2f_tab[t->R[xs[i >> 3]].w >> (16 * (j & 1)) & 0xffff];
                else
                    sd[j] = (double)t->R[xs[(i >> 3) * 2 + (j & 1)]].f;
            }
            if (hw && !bs) {
                /* an infinity or a NaN among the terms: the sum says which of the two comes out */
                for (j = 0; j < 8; j++) {
                    double c = sd[j], s = c;
                    if (hwnf || c - c != 0)
                        for (k = 0; k < K; k++)
                            s += Af[i * 64 + k] * Bf[k * 8 + j];
                    if (s - s != 0)
                        sd[j] = s;
                    else {
                        u32 rr = mma_hw_sum(&Af[i * 64], &Ea[i * 64], &Bf[j], &Eb[j], K, c, acc == 1);
                        out[(i & 7) * 4 + (j >> 1)][(i >> 3) * 2 + (j & 1)] = rr;
                        sd[j] = 0;
                        hwdone |= 1 << j;
                    }
                }
            } else if (bs) {
                /* scale_A[i][c] is byte (byte-id-a + c) of the scale register of the thread-id-a pair of quad i mod 8:
                   its first thread for rows 0..7, its second for rows 8..15; scale_B[c][j] is byte (byte-id-b + c) of
                   thread thread-id-b of quad j */
                s32 *sx = t0->f->xtra + t0->ip->e + 10 + 2 * nc + na + nb;
                int chunk = K / bs, c, se[4][8];
                double cd[8];
                Thr *la = grp[(i & 7) * 4 + 2 * (sx[2] & 1) + (i >> 3)];
                u32 ra = la->R[(la->f->xtra + la->ip->e + 10 + 2 * nc + na + nb)[0]].w;
                for (j = 0; j < 8; j++)
                    cd[j] = sd[j];
                for (c = 0; c < bs; c++) {
                    double ps[8], sa = mc_dec_ue8m0(ra >> (8 * ((sx[1] + c) & 3)));
                    for (j = 0; j < 8; j++)
                        ps[j] = 0;
                    for (k = c * chunk; k < (c + 1) * chunk; k++) {
                        double a = Af[i * 64 + k];
                        const double *bp = &Bf[k * 8];
                        for (j = 0; j < 8; j++)
                            ps[j] += a * bp[j];
                    }
                    for (j = 0; j < 8; j++) {
                        Thr *lb = grp[j * 4 + (sx[5] & 3)];
                        u32 rbv = lb->R[(lb->f->xtra + lb->ip->e + 10 + 2 * nc + na + nb)[3]].w;
                        double sv = sa * mc_dec_ue8m0(rbv >> (8 * ((sx[4] + c) & 3)));
                        sd[j] += ps[j] * sv;
                        se[c][j] = (int)(mc_dbits(sv) >> 52 & 0x7ff) - 1023;
                    }
                }
                if (hw)
                    for (j = 0; j < 8; j++) {
                        u32 rr;
                        if (sd[j] - sd[j] != 0)
                            continue;
                        if (kind4 && acc == 0) {
                            int sej[4];
                            for (c = 0; c < bs; c++)
                                sej[c] = se[c][j];
                            rr = mma_hw_sum4(&Af[i * 64], &Bf[j], K, chunk, sej, cd[j]);
                        } else {
                            /* the others as the kinds without scales, a product's exponent with its scale's */
                            double a2[64];
                            int e2[64];
                            for (k = 0; k < K; k++) {
                                a2[k] = Af[i * 64 + k] * mc_bitsd((u64)(1023 + se[k / chunk][j]) << 52);
                                e2[k] = Ea[i * 64 + k] + se[k / chunk][j];
                            }
                            rr = mma_hw_sum(a2, e2, &Bf[j], &Eb[j], K, cd[j], acc == 1);
                        }
                        out[(i & 7) * 4 + (j >> 1)][(i >> 3) * 2 + (j & 1)] = rr;
                        hwdone |= 1 << j;
                    }
            } else {
                /* the eight sums in variables of their own: the common case, and the one the time goes to */
                double s0 = sd[0], s1 = sd[1], s2 = sd[2], s3 = sd[3], s4 = sd[4], s5 = sd[5], s6 = sd[6], s7 = sd[7];
                const double *ap = &Af[i * 64], *bp = Bf;
                for (k = 0; k < K; k++, bp += 8) {
                    double a = ap[k];
                    s0 += a * bp[0];
                    s1 += a * bp[1];
                    s2 += a * bp[2];
                    s3 += a * bp[3];
                    s4 += a * bp[4];
                    s5 += a * bp[5];
                    s6 += a * bp[6];
                    s7 += a * bp[7];
                }
                sd[0] = s0;
                sd[1] = s1;
                sd[2] = s2;
                sd[3] = s3;
                sd[4] = s4;
                sd[5] = s5;
                sd[6] = s6;
                sd[7] = s7;
            }
            /* What is not a number is 0x7fffffff (0x7fff) and a zero is +0: so the hardware, for the kinds that
               are not block-scaled (those were not looked at). */
            for (j = 0; j < 8; j++) {
                u32 rr;
                if (hwdone >> j & 1)
                    continue;
                if (acc == 1) {
                    rr = mc_enc(&mc_fmt_f16, sd[j], MC_RN, 0);
                    if (!bs)
                        rr = sd[j] != sd[j] ? 0x7fff : rr == 0x8000 ? 0 : rr;
                } else {
                    rr = mc_fbits((float)sd[j]);
                    if (!bs)
                        rr = sd[j] != sd[j] ? 0x7fffffffU : rr == 0x80000000U ? 0 : rr;
                }
                out[(i & 7) * 4 + (j >> 1)][(i >> 3) * 2 + (j & 1)] = rr;
            }
        }
    }
    for (lane = 0; lane < 32; lane++) {
        Thr *t = grp[lane];
        s32 *xs = t->f->xtra + t->ip->e + 10;
        if (acc == 1) {
            t->R[xs[0]].u = out[lane][0] | out[lane][1] << 16;
            t->R[xs[1]].u = out[lane][2] | out[lane][3] << 16;
        } else
            for (r = 0; r < nc; r++)
                t->R[xs[r]].u = out[lane][r];
    }
    return 0;
}

static int do_ldmatrix(Blk *b, Thr **grp)
{
    u64 addr[32];
    u32 out[32][4];
    int lane, m, num, trans, shared, i;
    Thr *t0 = 0;
    for (lane = 0; lane < 32; lane++)
        if (grp[lane] && !t0)
            t0 = grp[lane];
    num = t0->f->xtra[t0->ip->e + 1] & 15;
    trans = t0->f->xtra[t0->ip->e + 1] >> 4 & 1;
    shared = t0->f->xtra[t0->ip->e + 1] >> 8 & 1;
    for (lane = 0; lane < 8 * num; lane++) {
        Thr *t = grp[lane];
        u64 ea;
        if (!t) {
            mc_fault(b, t0, "ldmatrix.sync: a thread that has to give a row address is not there%s (lane %lu)", 0, (u64)lane);
            return -1;
        }
        ea = t->R[t->ip->a].u + (u64)(s64)t->ip->c;
        if (shared)
            ea &= 0xffffffffUL; /* (an address in shared memory has 32 bits on the device: it wraps) */
        if (ea < b->shared_size || shared) {
            /* a row is 16 bytes */
            if (ea + 16 > b->shared_size) {
                mc_fault(b, t, "ldmatrix.sync%s: the row at 0x%lx is not inside the shared memory", 0, ea);
                return -1;
            }
            ea += (u64)b->shared;
        }
        if (ea < 4096) {
            mc_fault(b, t, "ldmatrix.sync%s: bad row address 0x%lx", 0, ea);
            return -1;
        }
        addr[lane] = ea;
    }
    for (lane = 0; lane < 32; lane++)
        for (m = 0; m < num; m++) {
            u32 v = 0;
            for (i = 0; i < 2; i++) {
                int row = trans ? 2 * (lane & 3) + i : lane >> 2, col = trans ? lane >> 2 : 2 * (lane & 3) + i;
                v |= (u32) * (u16 *)(addr[m * 8 + row] + 2 * (u64)col) << (16 * i);
            }
            out[lane][m] = v;
        }
    for (lane = 0; lane < 32; lane++)
        if (grp[lane]) {
            Thr *t = grp[lane];
            s32 *xs = t->f->xtra + t->ip->e + 2;
            for (m = 0; m < num; m++)
                t->R[xs[m]].u = out[lane][m];
        }
    return 0;
}

static int wsync_body(Blk *b, u32 warp, Thr **grp, u32 mask, u32 n)
{
    Thr *t0 = 0;
    u32 kind, lane, j, in[32], out[32], pv[32];
    u64 in64[32];
    (void)warp;
    (void)n;
    for (lane = 0; lane < 32; lane++)
        if (grp[lane]) {
            t0 = grp[lane];
            break;
        }
    if (!t0)
        return 0;
    kind = t0->ip->aux;
    switch (kind) {
    case WS_BARWARP:
        return 0;
    case WS_ACTIVEMASK:
        for (lane = 0; lane < 32; lane++)
            if (grp[lane])
                grp[lane]->R[grp[lane]->ip->d].u = mask;
        return 0;
    case WS_MMA:
        return do_mma(b, grp);
    case WS_LDMATRIX:
        return do_ldmatrix(b, grp);
    case WS_SHFL_UP:
    case WS_SHFL_DOWN:
    case WS_SHFL_BFLY:
    case WS_SHFL_IDX:
        for (lane = 0; lane < 32; lane++)
            in[lane] = grp[lane] ? grp[lane]->R[grp[lane]->ip->a].w : 0;
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 src, maxl, minl;
            u32 bv, cv, seg;
            int ok;
            if (!t)
                continue;
            bv = t->R[t->ip->b].w & 31;
            cv = t->R[t->ip->c].w & 31;
            seg = t->R[t->ip->c].w >> 8 & 31;
            maxl = (s32)((lane & seg) | (cv & ~seg));
            minl = (s32)(lane & seg);
            switch (kind) {
            case WS_SHFL_UP:
                src = (s32)lane - (s32)bv;
                ok = src >= maxl;
                break;
            case WS_SHFL_DOWN:
                src = (s32)(lane + bv);
                ok = src <= maxl;
                break;
            case WS_SHFL_BFLY:
                src = (s32)(lane ^ bv);
                ok = src <= maxl;
                break;
            default:
                src = minl | (s32)(bv & ~seg);
                ok = src <= maxl;
                break;
            }
            if (!ok)
                src = (s32)lane;
            out[lane] = in[src & 31];
            pv[lane] = (u32)ok;
        }
        for (lane = 0; lane < 32; lane++)
            if (grp[lane]) {
                Thr *t = grp[lane];
                s32 pd = t->f->xtra[t->ip->e + 1];
                t->R[t->ip->d].u = out[lane];
                if (pd != NOSLOT)
                    t->R[pd].u = pv[lane];
            }
        return 0;
    case WS_VOTE_ALL:
    case WS_VOTE_ANY:
    case WS_VOTE_EQ:
    case WS_VOTE_BALLOT: {
        u32 ballot = 0, r;
        for (lane = 0; lane < 32; lane++)
            if (grp[lane] && (grp[lane]->R[grp[lane]->ip->a].w & 1))
                ballot |= 1U << lane;
        r = kind == WS_VOTE_BALLOT ? ballot : kind == WS_VOTE_ALL ? ballot == mask : kind == WS_VOTE_ANY ? ballot != 0 : (ballot == 0 || ballot == mask);
        for (lane = 0; lane < 32; lane++)
            if (grp[lane])
                grp[lane]->R[grp[lane]->ip->d].u = r;
        return 0;
    }
    case WS_MATCH_ANY32:
    case WS_MATCH_ANY64:
    case WS_MATCH_ALL32:
    case WS_MATCH_ALL64: {
        int wide = kind == WS_MATCH_ANY64 || kind == WS_MATCH_ALL64, all = 1;
        u64 first = 0;
        int have = 0;
        for (lane = 0; lane < 32; lane++)
            if (grp[lane]) {
                in64[lane] = wide ? grp[lane]->R[grp[lane]->ip->a].u : grp[lane]->R[grp[lane]->ip->a].w;
                if (!have)
                    first = in64[lane], have = 1;
                else if (in64[lane] != first)
                    all = 0;
            }
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            s32 pd;
            u32 r = 0;
            if (!t)
                continue;
            pd = t->f->xtra[t->ip->e + 1];
            if (kind == WS_MATCH_ANY32 || kind == WS_MATCH_ANY64) {
                for (j = 0; j < 32; j++)
                    if (grp[j] && in64[j] == in64[lane])
                        r |= 1U << j;
            } else {
                r = all ? mask : 0;
                if (pd != NOSLOT)
                    out[lane] = (u32)all;
            }
            pv[lane] = r;
        }
        for (lane = 0; lane < 32; lane++)
            if (grp[lane]) {
                Thr *t = grp[lane];
                s32 pd = t->f->xtra[t->ip->e + 1];
                t->R[t->ip->d].u = pv[lane];
                if (pd != NOSLOT && (kind == WS_MATCH_ALL32 || kind == WS_MATCH_ALL64))
                    t->R[pd].u = out[lane];
            }
        return 0;
    }
    case WS_REDUX: {
        s32 *x = t0->f->xtra + t0->ip->e;
        u32 op = (u32)x[1] & 15, fl = (u32)x[1], r = 0;
        int ty = x[2], have = 0;
        double fr = 0;
        for (lane = 0; lane < 32; lane++) {
            Thr *t = grp[lane];
            u32 v;
            if (!t)
                continue;
            v = t->R[t->ip->a].w;
            if (ty == T_F32) {
                double d = (double)mc_bitsf(v);
                if (!have)
                    fr = (fl & 16) ? (d < 0 ? -d : mc_bitsd(mc_dbits(d) & 0x7fffffffffffffffUL)) : d;
                else
                    fr = minmax(op == 2, fr, d, ((fl & 16) ? FF_ABS : 0) | ((fl & 32) ? FF_NAN : 0));
            } else if (!have)
                r = v;
            else
                switch (op) {
                case 0:
                    r += v;
                    break;
                case 1:
                    r = ty == T_S32 ? ((s32)v < (s32)r ? v : r) : (v < r ? v : r);
                    break;
                case 2:
                    r = ty == T_S32 ? ((s32)v > (s32)r ? v : r) : (v > r ? v : r);
                    break;
                case 3:
                    r &= v;
                    break;
                case 4:
                    r |= v;
                    break;
                default:
                    r ^= v;
                    break;
                }
            have = 1;
        }
        if (ty == T_F32)
            r = mc_fbits((float)fr);
        for (lane = 0; lane < 32; lane++)
            if (grp[lane])
                grp[lane]->R[grp[lane]->ip->d].u = r;
        return 0;
    }
    }
    mc_fault(b, t0, "internal error: unknown warp instruction%s %lu", 0, kind);
    return -1;
}

int mc_stats_on;

int mc_wsync(Blk *b, u32 warp, Thr **grp, u32 mask, u32 n)
{
    u64 t0;
    int r;
    if (!mc_stats_on)
        return wsync_body(b, warp, grp, mask, n);
    /* (the processor's counter: asking the system for the time twice would take longer than the operation) */
    t0 = mc_ticks();
    r = wsync_body(b, warp, grp, mask, n);
    b->wsync_n++;
    b->wsync_usec += mc_ticks() - t0;
    return r;
}

/* ---- functions that a module may declare .extern and call ---- */

static const double PI = 3.14159265358979323846;

static double erf_(double x)
{
    double ax = x < 0 ? -x : x, s, term, r;
    int n;
    if (x != x)
        return x;
    if (ax < 2.5) {
        s = term = ax;
        for (n = 1; n < 80; n++) {
            term *= -ax * ax / n;
            s += term / (2 * n + 1);
            if ((term < 0 ? -term : term) < 1e-18 * s)
                break;
        }
        r = s * 1.1283791670955125739;
    } else if (ax > 6)
        r = 1;
    else { /* 1 - erfc by its continued fraction */
        double f = 0;
        for (n = 60; n >= 1; n--)
            f = n * 0.5 / (ax + f);
        r = 1 - exp(-ax * ax) / ((ax + f) * 1.7724538509055160273);
    }
    return x < 0 ? -r : r;
}

/* The gamma function (Lanczos, g = 7): good to some 1e-14 relative, which is more than a float needs. */
static double lanczos(double x)
{
    static const double c[9] = {0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313,
                                -176.61502916214059, 12.507343278686905, -0.13857109526572012,
                                9.9843695780195716e-6, 1.5056327351493116e-7};
    double a = c[0];
    int i;
    for (i = 1; i < 9; i++)
        a += c[i] / (x + (double)i - 1);
    return a;
}

static double tgamma_(double x)
{
    static const double pi = 3.14159265358979323846;
    double t;
    if (x != x)
        return x;
    if (x == mc_rintm(x)) {
        if (x <= 0)
            return x == 0 ? 1 / x : (x - x) / (x - x);
        if (x <= 23) {
            double f = 1;
            int i;
            for (i = 2; i < (int)x; i++)
                f *= i;
            return f;
        }
    }
    if (x < 0.5)
        return pi / (sin(pi * x) * tgamma_(1 - x));
    if (x > 172)
        return 1 / mc_bitsd(0);
    t = x + 6.5;
    return 2.5066282746310002 * pow(t, x - 0.5) * exp(-t) * lanczos(x);
}

static double lgamma_(double x)
{
    static const double pi = 3.14159265358979323846;
    double t, s;
    if (x != x)
        return x;
    if (x - x != 0)
        return 1 / mc_bitsd(0);
    if (x <= 0 && x == mc_rintm(x))
        return 1 / mc_bitsd(0);
    if (x == 1 || x == 2)
        return 0;
    if (x < 0.5) {
        s = sin(pi * x);
        return log(pi / (s < 0 ? -s : s)) - lgamma_(1 - x);
    }
    t = x + 6.5;
    return 0.91893853320467274 + (x - 0.5) * log(t) - t + log(lanczos(x));
}

static double erfc_(double x)
{
    double f = 0;
    int n;
    if (x < 2.5)
        return 1 - erf_(x);
    if (x > 27)
        return 0;
    for (n = 60; n >= 1; n--)
        f = n * 0.5 / (x + f);
    return exp(-x * x) / ((x + f) * 1.7724538509055160273);
}

static double log1p_(double x)
{
    double u = 1 + x;
    if (u == 1)
        return x;
    return log(u) * x / (u - 1);
}

static double expm1_(double x)
{
    double u;
    if (x > -1e-5 && x < 1e-5)
        return x + x * x / 2;
    u = exp(x);
    if (u == 1)
        return x;
    if (u - 1 == -1)
        return -1;
    return (u - 1) * x / log(u);
}

static double cbrt_(double x)
{
    double a = x < 0 ? -x : x, y;
    int i;
    if (a == 0 || a != a || a > 1.7e308)
        return x;
    y = exp(log(a) / 3);
    for (i = 0; i < 2; i++)
        y = y - (y * y * y - a) / (3 * y * y);
    return x < 0 ? -y : y;
}

static double hypot_(double x, double y)
{
    double a = x < 0 ? -x : x, c = y < 0 ? -y : y, t;
    if (a < c) {
        t = a;
        a = c;
        c = t;
    }
    if (a == 0 || a > 1.7e308)
        return a;
    if (c != c)
        return c;
    t = c / a;
    return a * mc_sqrt(1 + t * t);
}

static double sinpi_(double x)
{
    double r = x - 2 * floor(x / 2); /* [0, 2) */
    if (r == 0 || r == 1)
        return x < 0 ? -0.0 : 0.0;
    return r < 0.5 ? sin(PI * r) : r < 1.5 ? sin(PI * (1 - r)) : sin(PI * (r - 2));
}

static double remainder_(double x, double y)
{
    double q;
    if (y == 0 || x != x || y != y)
        return mc_bitsd(0x7ff8000000000000UL);
    q = mc_rintn(x / y);
    return mc_fma(-q, y, x);
}

enum {
    BF_SIN, BF_COS, BF_TAN, BF_ASIN, BF_ACOS, BF_ATAN, BF_SINH, BF_COSH, BF_TANH, BF_EXP, BF_EXP2, BF_EXP10, BF_EXPM1, BF_LOG,
    BF_LOG2, BF_LOG10, BF_LOG1P, BF_SQRT, BF_RSQRT, BF_CBRT, BF_ERF, BF_ERFC, BF_FLOOR, BF_CEIL, BF_TRUNC, BF_ROUND, BF_RINT,
    BF_FABS, BF_SINPI, BF_COSPI, BF_ASINH, BF_ACOSH, BF_ATANH,
    BF_POW, BF_ATAN2, BF_FMOD, BF_REMAINDER, BF_HYPOT, BF_FMIN, BF_FMAX, BF_COPYSIGN, BF_FDIM,
    BF_LDEXP, BF_FREXP, BF_ILOGB, BF_LRINT, BF_LROUND, BF_SINCOS, BF_FMA, BF_MODF,
    BF_NAN, BF_ISNAN, BF_ISINF, BF_ISFINITE, BF_SIGNBIT, BF_PRINTF, BF_VPRINTF, BF_MALLOC, BF_FREE, BF_ASSERT, BF_MEMCPY, BF_MEMSET, BF_MEMMOVE, BF_BZERO, BF_DIVMOD128, BF_TGAMMA, BF_LGAMMA, BF_LOGB
};

static const struct {
    const char *name;
    int id;
} builtins[] = {{"sin", BF_SIN}, {"cos", BF_COS}, {"tan", BF_TAN}, {"asin", BF_ASIN}, {"acos", BF_ACOS}, {"atan", BF_ATAN},
                {"sinh", BF_SINH}, {"cosh", BF_COSH}, {"tanh", BF_TANH}, {"exp", BF_EXP}, {"exp2", BF_EXP2}, {"exp10", BF_EXP10},
                {"expm1", BF_EXPM1}, {"log", BF_LOG}, {"log2", BF_LOG2}, {"log10", BF_LOG10}, {"log1p", BF_LOG1P}, {"sqrt", BF_SQRT},
                {"rsqrt", BF_RSQRT}, {"cbrt", BF_CBRT}, {"erf", BF_ERF}, {"erfc", BF_ERFC}, {"floor", BF_FLOOR}, {"ceil", BF_CEIL},
                {"trunc", BF_TRUNC}, {"round", BF_ROUND}, {"rint", BF_RINT}, {"nearbyint", BF_RINT}, {"fabs", BF_FABS},
                {"sinpi", BF_SINPI}, {"cospi", BF_COSPI}, {"asinh", BF_ASINH}, {"acosh", BF_ACOSH}, {"atanh", BF_ATANH},
                {"pow", BF_POW}, {"atan2", BF_ATAN2}, {"fmod", BF_FMOD}, {"remainder", BF_REMAINDER}, {"hypot", BF_HYPOT},
                {"fmin", BF_FMIN}, {"fmax", BF_FMAX}, {"copysign", BF_COPYSIGN}, {"fdim", BF_FDIM}, {"ldexp", BF_LDEXP},
                {"scalbn", BF_LDEXP}, {"frexp", BF_FREXP}, {"ilogb", BF_ILOGB}, {"lrint", BF_LRINT}, {"llrint", BF_LRINT},
                {"lround", BF_LROUND}, {"llround", BF_LROUND}, {"tgamma", BF_TGAMMA}, {"lgamma", BF_LGAMMA}, {"logb", BF_LOGB}, {"sincos", BF_SINCOS}, {"fma", BF_FMA}, {"modf", BF_MODF},
                {0, 0}};

/* The number of a function the engine provides, or -1. Math functions go by their C names, with an f at the
   end for float; their arguments and results are passed as the PTX declaration says (.f32 or .f64, .reg or .param).
   A name may carry the prefix __mcuda_, which is how mcc names the library functions a kernel calls. */
int mc_builtin_id(const char *name)
{
    int i;
    size_t n;
    char plain[64];
    if (strncmp(name, "__mcuda_", 8) == 0)
        name += 8;
    /* a C++ name of the simplest kind, _Z4sqrtf or _Z3powdd: the function by its first parameter's type */
    if (name[0] == '_' && name[1] == 'Z' && name[2] >= '1' && name[2] <= '9') {
        size_t len = (size_t)atoi(name + 2);
        const char *q = name + 2;
        while (*q >= '0' && *q <= '9')
            q++;
        if (len < sizeof plain - 2 && strlen(q) > len && (q[len] == 'f' || q[len] == 'd')) {
            int id;
            memcpy(plain, q, len);
            plain[len] = 0;
            id = mc_builtin_id(plain);
            if (id >= 0 && (id >> 1) < BF_VPRINTF)
                return q[len] == 'f' ? id | 1 : id;
        }
    }
    n = strlen(name);
    if (strcmp(name, "printf") == 0)
        return BF_PRINTF << 1;
    if (strcmp(name, "isnan") == 0 || strcmp(name, "__isnan") == 0)
        return BF_ISNAN << 1;
    if (strcmp(name, "isnanf") == 0 || strcmp(name, "__isnanf") == 0)
        return BF_ISNAN << 1 | 1;
    if (strcmp(name, "isinf") == 0 || strcmp(name, "__isinf") == 0)
        return BF_ISINF << 1;
    if (strcmp(name, "isinff") == 0 || strcmp(name, "__isinff") == 0)
        return BF_ISINF << 1 | 1;
    if (strcmp(name, "isfinite") == 0 || strcmp(name, "finite") == 0 || strcmp(name, "__finite") == 0)
        return BF_ISFINITE << 1;
    if (strcmp(name, "isfinitef") == 0 || strcmp(name, "finitef") == 0 || strcmp(name, "__finitef") == 0)
        return BF_ISFINITE << 1 | 1;
    if (strcmp(name, "signbit") == 0 || strcmp(name, "__signbit") == 0)
        return BF_SIGNBIT << 1;
    if (strcmp(name, "signbitf") == 0 || strcmp(name, "__signbitf") == 0)
        return BF_SIGNBIT << 1 | 1;
    if (strcmp(name, "memmove") == 0)
        return BF_MEMMOVE << 1;
    if (strcmp(name, "bzero") == 0)
        return BF_BZERO << 1;
    if (strcmp(name, "__mcc_divmod128") == 0)
        return BF_DIVMOD128 << 1;
    if (strcmp(name, "nan") == 0)
        return BF_NAN << 1;
    if (strcmp(name, "nanf") == 0)
        return BF_NAN << 1 | 1;
    if (strcmp(name, "vprintf") == 0)
        return BF_VPRINTF << 1;
    if (strcmp(name, "malloc") == 0)
        return BF_MALLOC << 1;
    if (strcmp(name, "free") == 0)
        return BF_FREE << 1;
    if (strcmp(name, "__assertfail") == 0 || strcmp(name, "__assert_fail") == 0)
        return BF_ASSERT << 1;
    if (strcmp(name, "memcpy") == 0)
        return BF_MEMCPY << 1;
    if (strcmp(name, "memset") == 0)
        return BF_MEMSET << 1;
    for (i = 0; builtins[i].name; i++) {
        size_t k = strlen(builtins[i].name);
        if (n == k && memcmp(name, builtins[i].name, k) == 0)
            return builtins[i].id << 1;
        if (n == k + 1 && memcmp(name, builtins[i].name, k) == 0 && name[k] == 'f')
            return builtins[i].id << 1 | 1;
    }
    return -1;
}

static double math1(int id, double x)
{
    switch (id) {
    case BF_SIN:
        return sin(x);
    case BF_COS:
        return cos(x);
    case BF_TAN:
        return tan(x);
    case BF_ASIN:
        return asin(x);
    case BF_ACOS:
        return acos(x);
    case BF_ATAN:
        return atan(x);
    case BF_SINH:
        return x > -1e-3 && x < 1e-3 ? x + x * x * x / 6 : sinh(x);
    case BF_COSH:
        return cosh(x);
    case BF_TANH:
        return tanh_(x);
    case BF_EXP:
        return exp(x);
    case BF_EXP2:
        return exp2_(x);
    case BF_EXP10:
        return pow(10.0, x);
    case BF_EXPM1:
        return expm1_(x);
    case BF_LOG:
        return log(x);
    case BF_LOG2:
        return log2_(x);
    case BF_LOG10:
        return log10(x);
    case BF_LOG1P:
        return log1p_(x);
    case BF_SQRT:
        return mc_sqrt(x);
    case BF_RSQRT:
        return 1.0 / mc_sqrt(x);
    case BF_CBRT:
        return cbrt_(x);
    case BF_ERF:
        return erf_(x);
    case BF_ERFC:
        return erfc_(x);
    case BF_TGAMMA:
        return tgamma_(x);
    case BF_LGAMMA:
        return lgamma_(x);
    case BF_LOGB: {
        int e = 0;
        if (x != x)
            return x;
        if (x == 0)
            return -1 / mc_bitsd(0); /* -infinity */
        if (x - x != 0)
            return x < 0 ? -x : x;
        frexp(x, &e);
        return (double)(e - 1);
    }
    case BF_FLOOR:
        return mc_rintm(x);
    case BF_CEIL:
        return mc_rintp(x);
    case BF_TRUNC:
        return mc_rintz(x);
    case BF_ROUND:
        return mc_rinta(x);
    case BF_RINT:
        return mc_rintn(x);
    case BF_FABS:
        return mc_bitsd(mc_dbits(x) & 0x7fffffffffffffffUL);
    case BF_SINPI:
        return sinpi_(x);
    case BF_COSPI:
        return sinpi_(x + 0.5);
    case BF_ASINH:
        return x < 0 ? -log(-x + mc_sqrt(x * x + 1)) : log1p_(x + x * x / (1 + mc_sqrt(x * x + 1)));
    case BF_ACOSH:
        return log(x + mc_sqrt(x * x - 1));
    case BF_ATANH:
        return 0.5 * log1p_(2 * x / (1 - x));
    }
    return 0;
}

static double math2(int id, double x, double y)
{
    switch (id) {
    case BF_POW:
        return pow(x, y);
    case BF_ATAN2:
        return atan2(x, y);
    case BF_FMOD:
        return fmod(x, y);
    case BF_REMAINDER:
        return remainder_(x, y);
    case BF_HYPOT:
        return hypot_(x, y);
    case BF_FMIN:
        return minmax(0, x, y, 0);
    case BF_FMAX:
        return minmax(1, x, y, 0);
    case BF_COPYSIGN:
        return mc_bitsd((mc_dbits(x) & 0x7fffffffffffffffUL) | (mc_dbits(y) & 0x8000000000000000UL));
    case BF_FDIM:
        return x > y ? x - y : x != x || y != y ? x + y : 0.0;
    }
    return 0;
}

static u64 arg_bits(Thr *t, s32 *it)
{
    u64 v = 0;
    u32 n = (u32)it[2] > 8 ? 8 : (u32)it[2];
    memcpy(&v, it[0] ? (u8 *)t->R + (u64)t->f->nreg * 8 + (u32)it[1] : (u8 *)&t->R[it[1]], n);
    return v;
}

static void put_ret(Thr *t, s32 *it, u64 v)
{
    if (it[0])
        memcpy((u8 *)t->R + (u64)t->f->nreg * 8 + (u32)it[1], &v, (u32)it[2] > 8 ? 8 : (u32)it[2]);
    else
        t->R[it[1]].u = v;
}

static double argf(u64 v, int single) { return single ? (double)mc_bitsf((u32)v) : mc_bitsd(v); }
static u64 retf(double d, int single) { return single ? (u64)mc_fbits((float)d) : mc_dbits(d); }

/* printf from a kernel: the arguments lie in a buffer, each on its natural alignment: 32-bit integers (and
   whatever is narrower, promoted), 64-bit ones for l / ll / z, doubles, pointers. */
static void do_vprintf(const char *fmt, const u8 *buf, int slots)
{
    u64 off = 0;
    char spec[40], out[400];
    if (!fmt)
        return;
    while (*fmt) {
        int n = 0, wide = 0;
        if (*fmt != '%') {
            fputc(*fmt++, stdout);
            continue;
        }
        spec[n++] = *fmt++;
        while (*fmt && strchr("-+ #0123456789.", *fmt) && n < 30)
            spec[n++] = *fmt++;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            if (*fmt != 'h')
                wide = 1;
            fmt++;
        }
        if (!*fmt)
            break;
        switch (*fmt) {
        case '%':
            fputc('%', stdout);
            break;
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'c':
            if (wide) {
                u64 v;
                off = (off + 7) & ~7UL;
                memcpy(&v, buf + off, 8);
                off += 8;
                spec[n++] = 'l';
                spec[n++] = *fmt;
                spec[n] = 0;
                sprintf(out, spec, v);
            } else {
                u32 v;
                off = (off + (slots ? 7 : 3)) & (slots ? ~7UL : ~3UL);
                memcpy(&v, buf + off, 4);
                off += slots ? 8 : 4;
                spec[n++] = *fmt;
                spec[n] = 0;
                if (*fmt == 'd' || *fmt == 'i')
                    sprintf(out, spec, (int)v);
                else
                    sprintf(out, spec, v);
            }
            fputs(out, stdout);
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double v;
            off = (off + 7) & ~7UL;
            memcpy(&v, buf + off, 8);
            off += 8;
            spec[n++] = *fmt;
            spec[n] = 0;
            sprintf(out, spec, v);
            fputs(out, stdout);
            break;
        }
        case 's':
        case 'p': {
            u64 v;
            off = (off + 7) & ~7UL;
            memcpy(&v, buf + off, 8);
            off += 8;
            if (*fmt == 's')
                fputs(v ? (const char *)v : "(null)", stdout);
            else {
                sprintf(out, "0x%lx", v);
                fputs(out, stdout);
            }
            break;
        }
        default:
            fputc(*fmt, stdout);
            break;
        }
        fmt++;
    }
}

int mc_builtin_call(Blk *b, Thr *t, int id, Ins *ip)
{
    s32 *x = t->f->xtra + ip->a, *rets = x + 4, *args;
    int nr = x[2], na = x[3], single = id & 1, fn = id >> 1;
    u64 a[5], r = 0;
    int i;
    args = rets + 3 * nr;
    for (i = 0; i < 5; i++)
        a[i] = i < na ? arg_bits(t, args + 3 * i) : 0;
    switch (fn) {
    case BF_VPRINTF:
        mcuda_lock_addr(stdout);
        do_vprintf((const char *)a[0], (const u8 *)a[1], 0);
        mcuda_unlock_addr(stdout);
        r = 0;
        break;
    case BF_MALLOC:
        r = (u64)malloc(a[0]);
        break;
    case BF_FREE:
        free((void *)a[0]);
        break;
    case BF_MEMCPY:
        memcpy((void *)a[0], (void *)a[1], a[2]);
        r = a[0];
        break;
    case BF_MEMSET:
        memset((void *)a[0], (int)a[1], a[2]);
        r = a[0];
        break;
    case BF_MEMMOVE:
        memmove((void *)a[0], (void *)a[1], a[2]);
        r = a[0];
        break;
    case BF_BZERO:
        memset((void *)a[0], 0, a[1]);
        break;
    case BF_NAN:
        r = single ? 0x7fc00000UL : 0x7ff8000000000000UL;
        break;
    case BF_ISNAN: {
        double v = argf(a[0], single);
        r = v != v;
        break;
    }
    case BF_ISINF: {
        double v = argf(a[0], single);
        r = v == v && v - v != 0;
        break;
    }
    case BF_ISFINITE: {
        double v = argf(a[0], single);
        r = v - v == 0;
        break;
    }
    case BF_SIGNBIT:
        r = single ? a[0] >> 31 & 1 : a[0] >> 63;
        break;
    case BF_DIVMOD128: { /* mcc's 128-bit division: (q[4], a low, a high, b low, b high, signed): quotient, remainder */
        u64 *q = (u64 *)a[0], al = a[1], ah = a[2], bl = a[3], bh = a[4], ql = 0, qh = 0, rl = 0, rh = 0;
        int sg = na > 5 && (u32)arg_bits(t, args + 15) != 0, neg_a = 0, neg_b = 0, k;
        if (sg && (s64)ah < 0) {
            neg_a = 1;
            al = ~al + 1;
            ah = ~ah + (al == 0);
        }
        if (sg && (s64)bh < 0) {
            neg_b = 1;
            bl = ~bl + 1;
            bh = ~bh + (bl == 0);
        }
        if (bl | bh)
            for (k = 127; k >= 0; k--) {
                u64 bit = k >= 64 ? ah >> (k - 64) & 1 : al >> k & 1;
                rh = rh << 1 | rl >> 63;
                rl = rl << 1 | bit;
                if (rh > bh || (rh == bh && rl >= bl)) {
                    rh = rh - bh - (rl < bl);
                    rl -= bl;
                    if (k >= 64)
                        qh |= 1UL << (k - 64);
                    else
                        ql |= 1UL << k;
                }
            }
        if (neg_a != neg_b) {
            ql = ~ql + 1;
            qh = ~qh + (ql == 0);
        }
        if (neg_a) {
            rl = ~rl + 1;
            rh = ~rh + (rl == 0);
        }
        q[0] = ql;
        q[1] = qh;
        q[2] = rl;
        q[3] = rh;
        break;
    }
    case BF_PRINTF: { /* as mcc passes them: every argument a parameter of its own, in order */
        u64 v[32];
        memset(v, 0, sizeof v);
        for (i = 1; i < na && i <= 32; i++)
            v[i - 1] = arg_bits(t, args + 3 * i);
        if (a[0]) {
            mcuda_lock_addr(stdout);
            do_vprintf((const char *)a[0], (const u8 *)v, 1);
            mcuda_unlock_addr(stdout);
        }
        r = 0;
        break;
    }
    case BF_ASSERT: {
        char msg[260];
        sprintf(msg, "assertion failed: %.80s, file %.60s, line %u, function %.60s", a[0] ? (const char *)a[0] : "?",
                a[1] ? (const char *)a[1] : "?", (u32)a[2], a[3] ? (const char *)a[3] : "?");
        mc_fault(b, t, "%s", msg, 0);
        return -1;
    }
    case BF_LDEXP:
        r = retf(ldexp(argf(a[0], single), (int)(s32)a[1]), single);
        break;
    case BF_FREXP: {
        int e = 0;
        r = retf(frexp(argf(a[0], single), &e), single);
        if (a[1])
            *(int *)a[1] = e;
        break;
    }
    case BF_ILOGB: {
        int e = 0;
        double v = argf(a[0], single);
        if (v == 0)
            r = 0x80000000U;
        else if (v != v || v > 1.7e308 || v < -1.7e308)
            r = 0x7fffffffU;
        else {
            frexp(v, &e);
            r = (u32)(e - 1);
        }
        break;
    }
    case BF_LRINT:
        r = (u64)(s64)mc_rintn(argf(a[0], single));
        break;
    case BF_LROUND:
        r = (u64)(s64)mc_rinta(argf(a[0], single));
        break;
    case BF_SINCOS: {
        double v = argf(a[0], single), s = sin(v), c = cos(v);
        if (single) {
            *(float *)a[1] = (float)s;
            *(float *)a[2] = (float)c;
        } else {
            *(double *)a[1] = s;
            *(double *)a[2] = c;
        }
        break;
    }
    case BF_FMA:
        if (single)
            r = mc_fbits(mc_fmaf(mc_bitsf((u32)a[0]), mc_bitsf((u32)a[1]), mc_bitsf((u32)a[2])));
        else
            r = mc_dbits(mc_fma(mc_bitsd(a[0]), mc_bitsd(a[1]), mc_bitsd(a[2])));
        break;
    case BF_MODF: {
        double v = argf(a[0], single), ip2 = mc_rintz(v);
        if (single)
            *(float *)a[1] = (float)ip2;
        else
            *(double *)a[1] = ip2;
        r = retf(v - ip2, single);
        break;
    }
    default:
        if (fn >= BF_POW && fn <= BF_FDIM)
            r = retf(math2(fn, argf(a[0], single), argf(a[1], single)), single);
        else
            r = retf(math1(fn, argf(a[0], single)), single);
        break;
    }
    if (nr)
        put_ret(t, rets, r);
    return 0;
}
