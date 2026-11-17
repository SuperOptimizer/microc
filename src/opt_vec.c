/* Loop vectorisation (-O3).

   Handles the simplest useful shape: a loop of one block that counts up by
   one, reads and writes arrays at the loop index, and computes each element
   independently of the others (plus integer sums over the elements). The
   loop is kept as it is; in front of it goes a copy that does 16 bytes'
   worth of elements per iteration, entered only when there are more than
   that many iterations and the arrays written do not overlap the others.
   The copy always leaves at least one iteration to the original loop, which
   therefore still produces every value the code after the loop may use. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int opt_fast_math; /* floating-point sums may be reordered */
int opt_finite_math;
int opt_no_signed_zeros;
int opt_reciprocal_math; /* -freciprocal-math (in -ffast-math): x / c may be x * (1 / c) */ /* -fno-signed-zeros (in -ffast-math): +0 and -0 may be taken for each other */

static Func *f;

enum { K_NONE, K_IDX, K_ADDR, K_VEC, K_MASK, K_RED, K_SKIP, K_IVPHI, K_IVNEXT, K_LINV,
       K_GIDX,   /* x86-64: a vector of i32 indexes widened (and shifted: shift), base = the vector; for a gather */
       K_GADDR }; /* base (from outside the loop) + a K_GIDX: base = it, ioff = the index vector, shift */
#define MAX_GATH 4
#define MAX_LINV 16
#define MAX_RED 4
#define MAX_IV 6
#define MAX_BASES 12
#define MAX_PAIRS 8
#define MAX_SPLATS 24 /* (x86-64: vec_pressure is the real bound) */
#define MAX_VECS 12 /* few enough that none is ever spilled */

static u8 *kind, *shift;
static u8 *lext; /* narrow lanes (narrow_exts): how each value of the loop stands to its lane, as lane_ext says */
static int ext_lanes; /* x86-64: comparisons, right shifts and byte / short conversions in lanes of one or two bytes
                         where the values are known to be what their lanes hold (MCC_NOVECEXT: lanes of four bytes) */
static int nar; /* this loop is done that way */
static u8 *mmk; /* a select that is a lane-by-lane maximum or minimum: MM_* bits, else 0; a comparison: 1 if also used as a number */
static int mixed;  /* x86-64: elements of different sizes, all in lanes of four bytes (narrower ones widened as they are
                      read and narrowed as they are written) */
#define MM_ON 1
#define MM_MAX 2 /* else the minimum */
#define MM_UNS 4 /* compared without sign */
#define MM_FL 8  /* floating point: maxps / minps */
int opt_x64_avx2;  /* x86-64: AVX2 (256-bit integer lanes): opt_vec uses 32-byte vectors in AVX functions */
int opt_x64_sse41; /* x86-64: SSE4.1 may be used (pmulld, pmaxsd, ...); -msse4.1, -mavx2, -march=native ... */
Obj *opt_obj;
/* Loops that read through an index computed lane by lane: only with -fvec-gather. With AVX2 the gather is vpgatherdd /
   vgatherdps (clamped row 0.544 -> 0.259 ns an element in isolation), but a gather takes no store-to-load forwarding:
   one that reads what was just stored waits for the stores to retire. ufsm's reg.c filt_axis fills a row by a clamped
   index from data the previous pass wrote: test_reg 45.9 -> 53.2 Gcycles with gathers, and no other ufsm test gained
   (suite 675.4 vs 681.9). With SSE4.1 the gather is four extract / load / insert steps, slower than scalar. */
int opt_vec_gather;
static u32 *base, *cnt, *map, *lpos;
static u32 *ioff; /* an index or address (K_IDX, K_ADDR): the value from outside the loop added to the counter before it
                     was widened (bit 31: taken away), as in a[i + k]; 0 if none */
#define IOFF_ZX 0x20000000U /* in ioff: the value is added 32 bits wide and the sum zero-extended (guarded against wrapping) */
static int zext_guard; /* such indexes are taken (MCC_NOVECZX: not) */
static u32 idx_off, walk_off;
static u32 gth[MAX_GATH], ngth; /* the gathering loads */ /* that of the index is_idx and the access walks found */
static u32 L;
static u32 iv;  /* the induction variable that indexes arrays, if any */
static int ext; /* how the body widens it when it is 32 bits wide */
static u32 lanesz; /* bytes per element; 0 = not known yet */
static u32 vw = 16; /* bytes per vector */
static int vty = IT_V128; /* the type of vector values */

/* induction variables: phi, phi + step each time round */
static u32 iv_phi[MAX_IV], iv_next[MAX_IV], iv_init[MAX_IV], niv;
static i64 iv_step[MAX_IV];

static u32 mk(u32 blk, int op, int ty, u32 a, u32 b)
{
    u32 v = ir_raw(f, op, ty, a, b);
    ir_append(f, blk, v);
    return v;
}

/* Constant while the loop runs: computed outside it, or read inside it from
   an address fixed outside it (K_LINV), which the guard in front of the
   vector loop checks is not among the elements written. */
#define INV(v) (f->ins[v].blk != L || kind[v] == K_LINV)

/* such a read is repeated in the preheader for the code built there */
static u32 linv[MAX_LINV], linv_copy[MAX_LINV], nlinv;
static u8 linv_der[MAX_LINV]; /* not a read: arithmetic on such values (m[3] * m[4]), done again in the preheader */

/* a read's address as root + a constant */
static void linv_root(u32 a, u32 *root, i64 *off)
{
    Ins *i = &f->ins[a];
    u64 c;
    *root = a;
    *off = 0;
    if (i->op == OP_ADD && i->ty == IT_I64 && ir_is_const(f, i->b, &c) && (i64)c > -65536 && (i64)c < 65536) {
        *root = i->a;
        *off = (i64)c;
    }
}

static u32 outside(u32 x)
{
    u32 k;
    if (f->ins[x].blk != L)
        return x;
    for (k = 0; linv[k] != x; k++)
        ;
    return linv_copy[k];
}

/* Is v used anywhere but by user? */
static int used_but(u32 v, u32 user)
{
    u32 b, x, k, nops, *ops;
    for (b = 1; b < f->nblk; b++) {
        if (f->blk[b].arg == v && (f->blk[b].term == TERM_BR || f->blk[b].term == TERM_RET))
            return 1;
        for (k = 0; k < 2; k++)
            for (x = k ? f->blk[b].first : f->blk[b].phis; x; x = f->ins[x].next) {
                u32 j;
                if (x == user)
                    continue;
                IR_OPS(f, &f->ins[x], ops, nops);
                for (j = 0; j < nops; j++)
                    if (ops[j] == v)
                        return 1;
            }
    }
    return 0;
}

/* The loop test t < a && t < b (if-converted to an and of the two), a and b fixed before the loop: t < min (a, b), the
   minimum taken in the preheader P. Likewise for <=, and without sign. Exact; the scalar loop gains by it too. */
static void two_bounds(u32 l, u32 P, u32 cond)
{
    u32 x, y, m, op, ty;
    Ins *c = &f->ins[cond], *X, *Y;
    if (c->op != OP_AND || c->blk != l || c->a == c->b)
        return;
    x = c->a;
    y = c->b;
    X = &f->ins[x];
    Y = &f->ins[y];
    op = X->op;
    if (X->blk != l || Y->blk != l || Y->op != op || X->a != Y->a || (op != OP_SLT && op != OP_ULT && op != OP_SLE && op != OP_ULE))
        return;
    ty = f->ins[X->b].ty;
    if (f->ins[X->b].blk == l || f->ins[Y->b].blk == l || f->ins[Y->b].ty != ty || f->ins[X->a].ty != ty || (ty != IT_I32 && ty != IT_I64))
        return;
    /* the smaller bound */
    m = mk(P, op == OP_SLT || op == OP_SLE ? OP_SLT : OP_ULT, IT_I32, f->ins[x].b, f->ins[y].b);
    m = mk(P, OP_SELECT, (int)ty, m, f->ins[x].b);
    f->ins[m].c = f->ins[y].b;
    c = &f->ins[cond];
    c->op = (u8)op;
    c->a = f->ins[x].a;
    c->b = m;
    if (!used_but(x, cond))
        ir_remove(f, x);
    if (!used_but(y, cond))
        ir_remove(f, y);
}

static int iv_index(u32 x)
{
    u32 k;
    for (k = 0; k < niv; k++)
        if (iv_phi[k] == x || iv_next[k] == x)
            return (int)k;
    return -1;
}

/* x counts array elements: the one variable stepping by one that the loop indexes with */
static int index_iv(u32 x, int ty)
{
    int k;
    if (INV(x) || kind[x] != K_IVPHI || f->ins[x].ty != ty)
        return 0;
    k = iv_index(x);
    if (iv_step[k] != 1 || (iv && iv != x))
        return 0;
    iv = x;
    return 1;
}

static int is_idx(u32 x, u32 *sh)
{
    idx_off = !INV(x) && kind[x] == K_IDX ? ioff[x] : 0;
    if (!INV(x) && kind[x] == K_IDX) {
        *sh = shift[x];
        return 1;
    }
    *sh = 0;
    return index_iv(x, IT_I64);
}

static u8 iv_val[MAX_IV]; /* the variable's value is used in the arithmetic, not only to index */

/* x is computed lane by lane: a vector value of the loop, or an induction variable (0, 1, 2, ... per lane) */
static int note_splat(u32 x);

static int is_vec(u32 x)
{
    if (INV(x))
        return 0;
    if (kind[x] == K_VEC)
        return 1;
    if (kind[x] == K_MASK) {
        /* a comparison used as a number: the mask and 1 (mmk marks it) */
        mmk[x] = 1;
        return note_splat(ir_const(f, IT_I32, 1));
    }
    if (kind[x] != K_IVPHI)
        return 0;
    iv_val[iv_index(x)] = 1;
    return 1;
}

static int mem_size(int mem)
{
    static const u8 sizes[] = { 1, 1, 2, 2, 4, 8, 4, 8 };
    return sizes[mem];
}

/* one element size for the whole loop */
static int set_lane(int mem)
{
    if (!lanesz)
        lanesz = (u32)mem_size(mem);
    return lanesz == (u32)mem_size(mem);
}

/* can a value of this type be one lane? (i32 values may be narrower in memory) */
static int fits(int ty)
{
    return ty == IT_I32 ? lanesz <= 4 : ty == IT_F32 ? lanesz == 4 : (ty == IT_I64 || ty == IT_F64) && lanesz == 8;
}

static int lane_kind(int ty)
{
    return ty == IT_F32 ? M_F32 : ty == IT_F64 ? M_F64 : ty == IT_I64 ? M_64 : lanesz == 1 ? M_8U : lanesz == 2 ? M_16U : M_32;
}

/* values from outside the loop, spread over the lanes in the preheader */
static u32 splat_of[MAX_SPLATS + MAX_IV * 2], splat[MAX_SPLATS + MAX_IV * 2], nsplat;
/* divisors fixed for the loop and their reciprocals (made in the preheader) */
static u32 rcp_of[MAX_SPLATS], rcp[MAX_SPLATS], nrcp;

/* Is 1 / x exact (x a constant power of two whose reciprocal is a normal number of its type)? */
static int exact_recip(u32 x)
{
    double d;
    u64 bits;
    int e;
    if (!ir_is_fconst(f, x, &d))
        return 0;
    memcpy(&bits, &d, 8);
    e = (int)(bits >> 52 & 0x7ff) - 1023; /* d = +-2^e when the fraction is zero (and it is no zero, subnormal, inf or NaN) */
    if ((bits & (((u64)1 << 52) - 1)) || (bits >> 52 & 0x7ff) == 0 || (bits >> 52 & 0x7ff) == 0x7ff)
        return 0;
    return f->ins[x].ty == IT_F32 ? -e >= -126 && -e <= 127 && e >= -126 && e <= 127 : -e >= -1022 && -e <= 1023;
}

/* conditions fixed for the loop that selects use, and their masks (made in the preheader) */
static u32 invc[MAX_SPLATS], invm[MAX_SPLATS], ninvc;

static int note_splat(u32 x)
{
    u32 k;
    for (k = 0; k < nsplat && splat_of[k] != x; k++)
        ;
    if (k == nsplat) {
        if (nsplat == (opt_arch == ARCH_X64 ? MAX_SPLATS : 8))
            return 0;
        splat_of[nsplat++] = x;
    }
    return 1;
}

/* operand x of a vector instruction: the copy of a value of the loop, or a spread one */
static u32 vec_operand(u32 x, u32 P)
{
    u32 j;
    if (!INV(x))
        return kind[x] == K_MASK && mmk[x] ? lpos[x] : map[x];
    for (j = 0; j < nsplat && splat_of[j] != x; j++)
        ;
    if (j == nsplat) {
        splat_of[j] = x;
        splat[j] = mk(P, OP_VSPLAT, vty, outside(x), 0);
        f->ins[splat[j]].mem = (u8)lane_kind(f->ins[x].ty);
        nsplat++;
    }
    return splat[j];
}

static int distinct(u32 a, u32 b)
{
    Ins *x = &f->ins[a], *y = &f->ins[b];
    return INV(a) && INV(b) && x->op == y->op && (x->op == OP_ADDR || x->op == OP_SLOT) && x->a != y->a;
}

/* The address a of an access of 1 << *sh bytes moves on by that much each
   time round. *key names the array walked: a value from outside the loop
   that the index is added to, or a pointer that is itself stepped. */
static int walks(u32 a, u32 size, u32 *key)
{
    int k;
    walk_off = 0;
    if (INV(a))
        return 0;
    if (kind[a] == K_ADDR) {
        walk_off = ioff[a];
        *key = base[a];
        return size == (u32)1 << shift[a];
    }
    if (kind[a] != K_IVPHI && kind[a] != K_IVNEXT)
        return 0;
    k = iv_index(a);
    *key = a;
    return f->ins[a].ty == IT_I64 && iv_step[k] == (i64)size;
}

/* The select i (of i32 values) as a maximum or minimum of the two values its condition compares: MM_* bits, or 0. */
static int minmax_of(Ins *i)
{
    Ins *q = &f->ins[i->a];
    int gt, uns;
    if (i->b == i->c || !((q->a == i->b && q->b == i->c) || (q->a == i->c && q->b == i->b)))
        return 0;
    if (i->ty == IT_F32 || i->ty == IT_F64) {
        /* x86-64's maxps (t, e) is the select c ? t : e of c = t > e lane by lane: a NaN on either side, or equal
           values, give e, as the select's false arm does (minps likewise with <). With >= or <= equal values
           give t, which differs only in a zero's sign: -fno-signed-zeros. */
        int strict = q->op == OP_FGT || q->op == OP_FLT;
        if (opt_arch != ARCH_X64 || !(q->op >= OP_FLT && q->op <= OP_FGE) || (!strict && !opt_no_signed_zeros))
            return 0;
        gt = q->op == OP_FGT || q->op == OP_FGE;
        if (q->a != i->b)
            gt = !gt;
        return MM_ON | (gt ? MM_MAX : 0) | MM_FL;
    }
    if (f->ins[q->a].ty != IT_I32)
        return 0;
    switch (q->op) {
    case OP_SGT: case OP_SGE: gt = 1; uns = 0; break;
    case OP_SLT: case OP_SLE: gt = 0; uns = 0; break;
    case OP_UGT: case OP_UGE: gt = 1; uns = 1; break;
    case OP_ULT: case OP_ULE: gt = 0; uns = 1; break;
    default: return 0;
    }
    /* (a > b ? a : b) keeps the larger; (a > b ? b : a) the smaller. Equal values make no difference. */
    if (q->a != i->b)
        gt = !gt;
    return MM_ON | (gt ? MM_MAX : 0) | (uns ? MM_UNS : 0);
}

/* How the value x of a loop over narrow elements stands to its lane: 1 the lane zero-extended, 2 sign-extended,
   0 neither (only its low bits are right, which is all that a store or an addition needs). */
static int lane_ext(u32 x)
{
    Ins *i = &f->ins[x];
    u64 c;
    i64 sc, lo, hi;
    if (INV(x)) {
        if (!ir_is_const(f, x, &c))
            return 0;
        sc = (i64)(i32)(u32)c;
        hi = ((i64)1 << (lanesz * 8)) - 1;
        lo = -((i64)1 << (lanesz * 8 - 1));
        /* a constant that fits both ways goes with either: 3 */
        return (sc >= 0 && sc <= hi ? 1 : 0) | (sc >= lo && sc <= hi / 2 ? 2 : 0);
    }
    if (i->op == OP_LOAD)
        return i->mem == M_8U || i->mem == M_16U ? 1 : i->mem == M_8S || i->mem == M_16S ? 2 : 0;
    if (kind[x] == K_VEC && mmk[x])
        return shift[x]; /* worked out for it below */
    if (nar)
        return kind[x] == K_MASK ? 3 : kind[x] == K_VEC ? lext[x] : 0; /* (a comparison as a number is 0 or 1) */
    return 0;
}

/* The vector operation of the maximum or minimum v (mmk[v] set), for lanes of lanesz bytes; 0 if none is right. */
static int minmax_op(u32 v)
{
    Ins *i = &f->ins[v];
    int m = mmk[v], uns = (m & MM_UNS) != 0;
    shift[v] = 0;
    if (m & MM_FL)
        return m & MM_MAX ? OP_VSMAX : OP_VSMIN; /* maxps / minps: SSE */
    if (lanesz < 4) {
        /* The widened values must compare as their lanes do. Zero-extended ones do so without sign whichever
           way they are compared; sign-extended ones with a sign compared with it, without one compared without. */
        int e = lane_ext(i->b) & lane_ext(i->c);
        if (!e)
            return 0;
        if (e & 1) {
            uns = 1;
            shift[v] = 1;
        } else {
            shift[v] = 2;
        }
    }
    return m & MM_MAX ? (uns ? OP_VUMAX : OP_VSMAX) : (uns ? OP_VUMIN : OP_VSMIN);
}

/* x (an i32 of the loop) is known to be below 2^31: an unsigned conversion of it is the signed one */
static int nonneg31(u32 x)
{
    Ins *i = &f->ins[x];
    u64 c;
    if (INV(x))
        return ir_is_const(f, x, &c) && (u32)c < 0x80000000U;
    return (i->op == OP_LOAD && (i->mem == M_8U || i->mem == M_16U)) || i->op == OP_ZEXT8 || i->op == OP_ZEXT16 ||
           (i->op == OP_AND && ((INV(i->b) && nonneg31(i->b)) || (INV(i->a) && nonneg31(i->a)))) ||
           (i->op == OP_SHR && INV(i->b) && ir_is_const(f, i->b, &c) && (c & 31));
}

/* Has x86-64 an instruction for the vector form of v, in lanes of lanesz bytes? */
static int x64_has(u32 v)
{
    Ins *i = &f->ins[v];
    int op = i->op, sse41 = opt_x64_sse41;
    if (kind[v] == K_VEC && mmk[v]) {
        int o = minmax_op(v);
        if (mmk[v] & MM_FL)
            return 1;
        /* pmaxub pminub pmaxsw pminsw are SSE2; the rest SSE4.1 */
        return (lanesz == 1 && (o == OP_VUMAX || o == OP_VUMIN)) || (lanesz == 2 && (o == OP_VSMAX || o == OP_VSMIN)) || sse41;
    }
    switch (op) {
    case OP_MUL:
        return lanesz == 2 || (lanesz == 4 && sse41);
    case OP_SHL: case OP_SHR:
        return lanesz > 1 || ext_lanes; /* (bytes: the shift of 16-bit lanes, and a mask) */
    case OP_SAR:
        return lanesz == 2 || lanesz == 4 || (nar && lanesz == 1); /* (of a zero-extended byte: a logical shift) */
    case OP_EQ: case OP_NE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
    case OP_ULT: case OP_ULE: case OP_UGT: case OP_UGE:
        return lanesz <= 4;
    case OP_SITOF: case OP_FTOSI:
        return lanesz == 4;
    case OP_UITOF:
        return lanesz == 4 && i->ty == IT_F32 && f->ins[i->a].ty == IT_I32 && nonneg31(i->a);
    case OP_FTOUI:
        /* (unsigned char) f: out of the byte's range the C program is undefined, so the signed conversion will do */
        return lanesz == 4 && cnt[v] == 1 && mmk[v] == 2;
    default:
        return 1;
    }
}

/* ---- narrow lanes that know what they hold ----
   A lane of one or two bytes holds the low bits of the int the C program computes. That is all a sum, a product, a
   left shift or a store needs, but a comparison, a right shift or a conversion to a wider type needs the whole
   value. Where the value is known to be its lane zero-extended (a byte read as unsigned char, x & 15, x >> 1 of
   such, a comparison's 0 or 1) or sign-extended (a signed char read), those work on the lanes too: bytes compare as
   the ints do, the logical shift of a zero-extended lane shifts in the zeros the int has, and (uint8_t) x is the
   lane itself. So `u[i] = (uint8_t) ((u[i] >> 1) ^ (uint8_t) (0u - (u[i] & 1u)))` (volcomp's zigzag) or
   `m[i] = a[i] != 0 && b[i] == t` runs 32 bytes to a vector instead of 8 widened ones.
   ext_of: 1 = the value is its lane zero-extended, 2 = sign-extended, 3 = both, 0 = only the low bits are right. */
static int ext_of(u32 x)
{
    Ins *i = &f->ins[x];
    u64 c;
    u32 bits = lanesz * 8;
    if (ir_is_const(f, x, &c)) {
        i64 sc = (i64)(i32)(u32)c, hi = ((i64)1 << bits) - 1, lo = -((i64)1 << (bits - 1));
        return i->ty != IT_I32 ? 0 : (sc >= 0 && sc <= hi ? 1 : 0) | (sc >= lo && sc <= hi / 2 ? 2 : 0);
    }
    if (i->ty != IT_I32)
        return 0;
    if (INV(x)) {
        if (i->op == OP_LOAD && (u32)mem_size(i->mem) <= lanesz)
            return i->mem == M_8U || i->mem == M_16U ? ((u32)mem_size(i->mem) < lanesz ? 3 : 1) : 2;
        return ir_umax32(f, x) < ((u32)1 << bits) ? 1 : 0;
    }
    if (kind[x] == K_MASK)
        return 3; /* as a number: 0 or 1 */
    return kind[x] == K_VEC ? lext[x] : 0;
}

static int minmax_op(u32 v);
static int x64_has(u32 v);

/* Can every instruction of the loop l work in lanes of lanesz (1 or 2) bytes? Fills lext, puts in shift[] of a
   comparison the comparison of the lanes that decides as the ints' does, and notes the masks byte shifts need. */
static int narrow_exts(u32 l, u32 cond)
{
    u32 v, bits = lanesz * 8;
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v];
        int ea, eb, e;
        u64 c;
        lext[v] = 0;
        if (v == cond || (kind[v] != K_VEC && kind[v] != K_MASK))
            continue;
        if (i->op == OP_LOAD) {
            lext[v] = i->mem == M_8U || i->mem == M_16U ? 1 : 2;
            continue;
        }
        if (i->op == OP_SELECT) {
            if (mmk[v]) {
                if ((mmk[v] & MM_FL) || !minmax_op(v))
                    return 0;
                lext[v] = shift[v];
            } else {
                if (i->ty != IT_I32)
                    return 0;
                lext[v] = (u8)(ext_of(i->b) & ext_of(i->c));
            }
            continue;
        }
        if (i->op == OP_CALL || (kind[v] == K_VEC && i->ty != IT_I32) || (kind[v] == K_MASK && f->ins[i->a].ty != IT_I32))
            return 0;
        ea = ext_of(i->a);
        eb = ir_opcount_tab[i->op] >= 2 ? ext_of(i->b) : 0;
        switch (i->op) {
        case OP_ZEXT8:
            lext[v] = lanesz == 1 ? 1 : 3;
            break;
        case OP_ZEXT16:
            if (lanesz != 2)
                return 0;
            lext[v] = 1;
            break;
        case OP_SEXT8:
            lext[v] = 2;
            break;
        case OP_SEXT16:
            if (lanesz != 2)
                return 0;
            lext[v] = 2;
            break;
        case OP_AND:
            e = ((ea | eb) & 1) | (ea & eb & 2);
            lext[v] = (u8)(ea == 3 || eb == 3 ? 3 : e);
            break;
        case OP_OR: case OP_XOR:
            lext[v] = (u8)(ea & eb);
            break;
        case OP_ADD: case OP_SUB: case OP_NEG:
            break;
        case OP_MUL:
            if (lanesz != 2)
                return 0;
            break;
        case OP_SAR:
            /* (unsigned char) x >> 1 is an int's arithmetic shift of a value that is not negative: the logical one */
            if (!(ea & 1)) {
                if (!ir_is_const(f, i->b, &c) || c >= bits || !(ea & 2) || lanesz != 2)
                    return 0;
                lext[v] = 2;
                break;
            }
            /* fall through */
        case OP_SHL: case OP_SHR:
            if (!ir_is_const(f, i->b, &c) || c >= bits || (i->op != OP_SHL && !(ea & 1)))
                return 0;
            if (lanesz == 1 && !note_splat(ir_const(f, IT_I32, i->op != OP_SHL ? 0xffu >> c : (0xffu << c) & 0xff)))
                return 0;
            if (i->op != OP_SHL)
                lext[v] = (u8)(c ? 3 : 1);
            break;
        case OP_EQ: case OP_NE:
            if (!(ea & eb))
                return 0;
            shift[v] = 0;
            break;
        case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
            /* sign-extended lanes compare with sign as the ints do; zero-extended ones without */
            if (!(ea & eb))
                return 0;
            shift[v] = (u8)(ea & eb & 2 ? 0 : i->op + (OP_ULT - OP_SLT));
            break;
        case OP_ULT: case OP_ULE: case OP_UGT: case OP_UGE:
            /* (without sign the order of sign-extended values is their lanes' too) */
            if (!(ea & eb))
                return 0;
            shift[v] = 0;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

/* x86-64 has 14 vector registers to give out, and a vector value is never spilled: the most vector values
   the loop keeps at one time, from the splats, sums and counters held all through it and the body's own. */
static u32 vec_pressure(u32 extra)
{
    u32 v, p = 0, q, most = 0, *ops, nops, k;
#define VECVAL(x) (!INV(x) && (kind[x] == K_VEC || kind[x] == K_MASK || (kind[x] == K_IVPHI && iv_val[iv_index(x)])))
    for (v = f->blk[L].phis; v; v = f->ins[v].next)
        lpos[v] = 0;
    for (v = f->blk[L].first; v; v = f->ins[v].next)
        lpos[v] = 0;
    /* where each is used last */
    for (v = f->blk[L].first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v];
        p++;
        if (kind[v] == K_SKIP || kind[v] == K_IVNEXT || kind[v] == K_IDX || kind[v] == K_ADDR || kind[v] == K_LINV ||
            kind[v] == K_GIDX || kind[v] == K_GADDR)
            continue;
        IR_OPS(f, i, ops, nops);
        for (k = 0; k < nops; k++)
            if (VECVAL(ops[k]))
                lpos[ops[k]] = p;
    }
    /* how many are alive at each place */
    p = 0;
    for (v = f->blk[L].first; v; v = f->ins[v].next) {
        u32 n = 0, x;
        p++;
        q = 0;
        for (x = f->blk[L].first; x; x = f->ins[x].next) {
            q++;
            if (q > p)
                break;
            if (VECVAL(x) && (lpos[x] >= p || q == p))
                n++;
        }
        for (x = f->blk[L].phis; x; x = f->ins[x].next)
            if (VECVAL(x))
                n++; /* a counter's vector, and its copy for the later vectors of a round */
        if (n > most)
            most = n;
    }
#undef VECVAL
    return most + nsplat + extra;
}

/* A call of sqrtf, fabsf, floorf and the like (or their double forms) that x86-64 does lane by lane: 1 + VM_*, or
   VM_FABS; 0 for any other call. */
#define VM_FABS 0x40
static int vmath_of(u32 v)
{
    static const char *const names[] = { "sqrt", "floor", "ceil", "trunc", "rint", "nearbyint", "fabs", 0 };
    Ins *i = &f->ins[v], *c = &f->ins[i->c];
    const char *nm;
    size_t bl;
    u32 k, a;
    int isf;
    if (opt_arch != ARCH_X64 || !opt_obj || c->op != OP_ADDR || c->b || i->b != 1 || (i->flags & (IF_SRET | IF_ASM | IF_ATOMIC | IF_ALLOCA)))
        return 0;
    if (opt_obj->syms[c->a].sec != SEC_UNDEF && opt_obj->syms[c->a].sec != SEC_NONE)
        return 0; /* the program's own function of that name */
    nm = intern_str(opt_obj->syms[c->a].name);
    bl = strlen(nm);
    isf = bl > 1 && nm[bl - 1] == 'f';
    a = f->xargs[i->a];
    if (f->ins[a].ty != (isf ? IT_F32 : IT_F64) || i->ty != f->ins[a].ty)
        return 0;
    for (k = 0; names[k]; k++)
        if (strlen(names[k]) == bl - (size_t)isf && strncmp(nm, names[k], bl - (size_t)isf) == 0)
            break;
    if (!names[k] || (k == VM_SQRT && opt_math_errno) || (k > VM_SQRT && k <= VM_NEARBYINT && !opt_x64_sse41))
        return 0;
    return names[k + 1] ? 1 + (int)k : VM_FABS;
}

int opt_vec_report; /* say what was and was not vectorised */

/* Can the range of the i32 x, an index of a gather, be bounded from the loop's counters and values from outside it
   (sums, differences, shifts and multiplications by constants, selects)? Then the elements it picks lie in that range,
   whose addresses are checked against what the loop writes. (Like the rest of this file, index arithmetic is taken not
   to wrap around.) */
static int gmono(u32 x, int depth)
{
    Ins *i = &f->ins[x];
    u64 c;
    if (depth > 12)
        return 0;
    if (INV(x))
        return i->ty == IT_I32;
    if (kind[x] == K_IVPHI)
        return i->ty == IT_I32;
    switch (i->op) {
    case OP_ADD: case OP_SUB:
        return gmono(i->a, depth + 1) && gmono(i->b, depth + 1);
    case OP_SHR: case OP_SAR: case OP_SHL:
        return ir_is_const(f, i->b, &c) && gmono(i->a, depth + 1);
    case OP_MUL:
        return INV(i->b) && ir_is_const(f, i->b, &c) && (i32)(u32)c > 0 && gmono(i->a, depth + 1);
    case OP_SELECT:
        return kind[x] == K_VEC && gmono(i->b, depth + 1) && gmono(i->c, depth + 1);
    default:
        return 0;
    }
}

static u32 gmin(u32 P, u32 a, u32 b, int max)
{
    u32 r = mk(P, OP_SELECT, IT_I32, mk(P, max ? OP_SGT : OP_SLT, IT_I32, a, b), a);
    f->ins[r].c = b;
    return r;
}

/* the least and the most x (gmono said yes) can be over the loop's count (i64) iterations, in the preheader P */
static void grange(u32 x, u32 P, u32 count, u32 *lo, u32 *hi)
{
    Ins *i = &f->ins[x];
    u32 alo, ahi, blo, bhi;
    if (INV(x)) {
        *lo = *hi = outside(x);
        return;
    }
    if (kind[x] == K_IVPHI) {
        int k = iv_index(x);
        u32 t = mk(P, OP_TRUNC, IT_I32, mk(P, OP_SUB, IT_I64, count, ir_const(f, IT_I64, 1)), 0), e;
        if (iv_step[k] != 1)
            t = mk(P, OP_MUL, IT_I32, t, ir_const(f, IT_I32, (u64)iv_step[k]));
        e = mk(P, OP_ADD, IT_I32, iv_init[k], t);
        *lo = iv_step[k] > 0 ? iv_init[k] : e;
        *hi = iv_step[k] > 0 ? e : iv_init[k];
        return;
    }
    grange(i->op == OP_SELECT ? i->b : i->a, P, count, &alo, &ahi);
    switch (i->op) {
    case OP_ADD:
        grange(i->b, P, count, &blo, &bhi);
        *lo = mk(P, OP_ADD, IT_I32, alo, blo);
        *hi = mk(P, OP_ADD, IT_I32, ahi, bhi);
        return;
    case OP_SUB:
        grange(i->b, P, count, &blo, &bhi);
        *lo = mk(P, OP_SUB, IT_I32, alo, bhi);
        *hi = mk(P, OP_SUB, IT_I32, ahi, blo);
        return;
    case OP_SELECT:
        grange(i->c, P, count, &blo, &bhi);
        *lo = gmin(P, alo, blo, 0);
        *hi = gmin(P, ahi, bhi, 1);
        return;
    default: /* shifts, multiplications by a positive constant: order kept (a logical shift only of what is not negative) */
        *lo = mk(P, i->op, IT_I32, alo, i->b);
        *hi = mk(P, i->op, IT_I32, ahi, i->b);
        if (i->op == OP_SHR) {
            /* where it may be negative, anything from 0 to all ones shifted */
            u32 neg = mk(P, OP_SLT, IT_I32, alo, ir_const(f, IT_I32, 0)), r;
            r = mk(P, OP_SELECT, IT_I32, neg, ir_const(f, IT_I32, 0));
            f->ins[r].c = *lo;
            *lo = r;
            r = mk(P, OP_SELECT, IT_I32, neg, mk(P, OP_SHR, IT_I32, ir_const(f, IT_I32, 0xffffffffU), i->b));
            f->ins[r].c = *hi;
            *hi = r;
        }
        return;
    }
}

/* ---- 64-bit arithmetic that only its low 32 bits are taken of ----
   `chunk[i] = (uint8_t) (key * 31 + i * 5 + (i >> 7))` with a 64-bit key is computed 64 bits wide in C, each int
   term widened first, and then cut down again. Sums, differences, products, and / or / xor and left shifts give the
   same low 32 bits whether they are worked out 64 or 32 bits wide, so a tree of them that only a truncation to
   32 bits uses is done 32 bits wide, its widened terms taken as they were before widening and its terms from outside
   the loop cut down once in front of it. This is exact, and what lets such a loop have lanes of four bytes (there
   is no 64-bit vector multiplication, and elements of eight bytes do not mix with narrower ones).
   MCC_NOVECNARROW: off. */
static u32 *nuses; /* how often each value is used in the function (while narrow_loops runs) */
static u32 nw_of[8], nw_to[8], nnw;

static int nw_can(u32 x, u32 l, int depth)
{
    Ins *i = &f->ins[x];
    u64 c;
    if (i->ty != IT_I64)
        return 0;
    if (i->blk != l)
        return i->blk != 0 || i->op == OP_CONST; /* fixed for the loop (constants are in no block) */
    if (i->op == OP_ZEXT32 || i->op == OP_SEXT32)
        return f->ins[i->a].ty == IT_I32; /* (whatever else uses the widened value keeps it) */
    if (depth > 16 || nuses[x] != 1 || i->var) /* (-g: a variable's value keeps its width) */
        return 0;
    switch (i->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
        return nw_can(i->a, l, depth + 1) && nw_can(i->b, l, depth + 1);
    case OP_SHL:
        return ir_is_const(f, i->b, &c) && c < 32 && nw_can(i->a, l, depth + 1);
    default:
        return 0;
    }
}

static u32 nw_do(u32 x, u32 l, u32 P)
{
    Ins *i = &f->ins[x];
    u64 c;
    u32 k, a, b;
    if (i->blk != l) {
        if (ir_is_const(f, x, &c))
            return ir_const(f, IT_I32, c & 0xffffffffU);
        for (k = 0; k < nnw; k++)
            if (nw_of[k] == x)
                return nw_to[k];
        a = mk(P, OP_TRUNC, IT_I32, x, 0);
        if (nnw < 8) {
            nw_of[nnw] = x;
            nw_to[nnw++] = a;
        }
        return a;
    }
    if (i->op == OP_ZEXT32 || i->op == OP_SEXT32)
        return i->a;
    a = nw_do(i->a, l, P);
    b = i->op == OP_SHL ? ir_const(f, IT_I32, f->ins[i->b].a) : nw_do(f->ins[x].b, l, P);
    i = &f->ins[x];
    i->a = a;
    i->b = b;
    i->ty = IT_I32;
    return x;
}

static void narrow_loops(void)
{
    u32 b, v, k, nx, *ops, nops, n = f->nins, any = 0;
    nuses = 0;
    for (b = 1; b < f->nblk; b++) {
        Block *bl = &f->blk[b];
        u32 P;
        if (bl->term != TERM_BR || bl->succ[0] != b || bl->succ[1] == b || bl->npreds != 2)
            continue;
        P = bl->preds[bl->preds[0] == b ? 1 : 0];
        if (P == b || f->blk[P].term != TERM_JMP)
            continue;
        nnw = 0;
        for (v = bl->first; v; v = nx) {
            Ins *i = &f->ins[v];
            nx = i->next;
            if (i->op != OP_TRUNC || i->ty != IT_I32 || f->ins[i->a].blk != b || f->ins[i->a].op == OP_PHI ||
                f->ins[i->a].op == OP_ZEXT32 || f->ins[i->a].op == OP_SEXT32)
                continue;
            if (!nuses) {
                /* counted when first needed; a value this pass makes or changes is never asked about again */
                u32 bb, x, j;
                nuses = (u32 *)xmalloc((size_t)n * sizeof(u32));
                memset(nuses, 0, (size_t)n * sizeof(u32));
                for (bb = 1; bb < f->nblk; bb++) {
                    if (f->blk[bb].arg && (f->blk[bb].term == TERM_BR || f->blk[bb].term == TERM_RET) && f->blk[bb].arg < n)
                        nuses[f->blk[bb].arg] += 2; /* (a use that is no instruction: never narrowed) */
                    for (k = 0; k < 2; k++)
                        for (x = k ? f->blk[bb].first : f->blk[bb].phis; x; x = f->ins[x].next) {
                            Ins *xi = &f->ins[x];
                            IR_OPS(f, xi, ops, nops);
                            for (j = 0; j < nops; j++)
                                if (ops[j] < n)
                                    nuses[ops[j]]++;
                            /* (what else may name a value: counted too, which only ever keeps a tree as it is) */
                            if ((xi->op == OP_CALL || xi->op == OP_MEMCPY || xi->op == OP_MEMZERO) && xi->c < n)
                                nuses[xi->c]++;
                        }
                }
            }
            if (i->a >= n || v >= n || !nw_can(i->a, b, 0))
                continue;
            ir_replace(f, v, nw_do(i->a, b, P));
            any = 1;
        }
    }
    if (nuses)
        free(nuses);
    if (any) {
        ir_resolve_all(f);
        opt_dce(f); /* the widenings nothing uses any more */
    }
}

/* ---- a count that is a whole number of vectors: no scalar iterations at all ----
   The vector loop used always to leave the original loop at least one iteration, which then produced whatever the
   code after the loop reads of the loop's values. A loop over 4096 bytes at 64 a round did 63 rounds and then 64
   scalar iterations, a third of all its instructions; one over 16 floats at 16 a round was not entered at all. Where
   the code after the loop reads nothing of the loop but its counters and finished sums (maxima, minima), those have
   values the vector loop's end can state, and the original loop is skipped when nothing is left for it: its exit
   edge goes through a new block where each such value is chosen between the scalar loop's and the vectors'.
   MCC_NOVECEXACT: as before. */
static int vec_exact;
static int vec_bitred; /* x86-64: or / and / xor carried round a loop are reductions */
/* ---- what the vector loop leaves over ----
   The vector loop does two (sums: four) vectors a round, and what is left, up to a round's worth less one, went
   to the original loop one element at a time: 16 bytes of a 100-byte row took as many instructions as the other 84,
   and a loop over 16 ints summed four vectors a round never started. With vec_epi the original loop is vectorised a
   second time, one vector a round, behind the first: a block R joins the way round the first vector loop and the way
   out of it (the counters and sums so far are phis there), and is the preheader the second try_loop builds in.
   MCC_NOVECEPI: off. */
static int vec_epi;
static int epi_pass; /* the second go: one vector a round */
#define MAX_LIVE 8
static u32 live_of[MAX_LIVE], nlive;

/* The loop l's values that the rest of the function uses: 0 if there is one that is neither a counter (its value in
   or after the last iteration) nor a finished reduction (red_next). */
static int exact_liveouts(u32 l, const u32 *red_next, u32 nred)
{
    u32 b, x, k, j, n, m;
    nlive = 0;
    for (b = 1; b < f->nblk; b++) {
        u32 arg = f->blk[b].term == TERM_BR || f->blk[b].term == TERM_RET ? f->blk[b].arg : 0;
        if (b == l || f->blk[b].term == TERM_NONE)
            continue;
        for (k = 0; k < 3; k++)
            for (x = k == 2 ? arg : k ? f->blk[b].first : f->blk[b].phis; x; x = k == 2 ? 0 : f->ins[x].next) {
                n = k == 2 ? 1 : f->ins[x].op == OP_BYVAL ? 1 : ir_opcount(f, x); /* (OP_BYVAL's b is a number) */
                for (j = 0; j < n; j++) {
                    u32 o = k == 2 ? x : *ir_opref(f, x, j);
                    if (f->ins[o].blk != l || f->ins[o].op == OP_CONST || f->ins[o].op == OP_FCONST)
                        continue;
                    for (m = 0; m < nred && red_next[m] != o; m++)
                        ;
                    if (iv_index(o) < 0 && m == nred)
                        return 0;
                    for (m = 0; m < nlive && live_of[m] != o; m++)
                        ;
                    if (m == nlive) {
                        if (nlive == MAX_LIVE)
                            return 0;
                        live_of[nlive++] = o;
                    }
                }
            }
    }
    return 1;
}

static u32 missv; /* the instruction being looked at, for the report */
#define MISS(why) \
    do { \
        if (opt_vec_report && !epi_pass) { \
            fprintf(stderr, "%s: loop not vectorised: %s", f->name, why); \
            if (opt_vec_report > 1 && missv) \
                fprintf(stderr, " [op %d ty %d; operands' ops %d %d; line %u]", f->ins[missv].op, f->ins[missv].ty, \
                        f->ins[f->ins[missv].a].op, ir_opcount_tab[f->ins[missv].op] >= 2 && ir_opcount_tab[f->ins[missv].op] != 255 ? f->ins[f->ins[missv].b].op : -1, \
                        dwarf_line_of(f->ins[missv].loc)); \
            fputc('\n', stderr); \
        } \
        return 0; \
    } while (0)

static int try_loop(u32 l, u32 n0)
{
    u32 pl, pp, P, V, M, cond, t, n, v, k, j, ph;
    u32 red_phi[MAX_RED], red_next[MAX_RED], red_x[MAX_RED], acc[MAX_RED][4], nred = 0, un, u, offv;
    int red_op[MAX_RED];
    u32 keys[MAX_BASES], start[MAX_BASES], nkeys = 0, key, ivvec[MAX_IV];
    u8 stored[MAX_BASES], keysz[MAX_BASES], klsh[MAX_BASES];
    u32 koff[MAX_BASES];
    u32 maxmem = 0, offn[4];
    u32 pa[MAX_PAIRS], pb[MAX_PAIRS], np = 0;
    u8 pka[MAX_PAIRS], pkb[MAX_PAIRS];
    u32 nvec = 0, maxsh = 0, full_only = 0, vf, init64, n64, count, ok, vcnt, ivv, ivv2, sh, offend;
    int uns, le, c, cty, tnext, lsh, wide = 0, exact = 0;
    u32 iv_fin[MAX_IV], red_fin[MAX_RED];
    i64 st;
    Ins *ci;

    L = l;
    missv = 0;
    if (f->blk[l].term != TERM_BR || f->blk[l].succ[0] != l || f->blk[l].succ[1] == l || f->blk[l].npreds != 2)
        return 0;
    pl = f->blk[l].preds[0] == l ? 0 : 1;
    pp = 1 - pl;
    P = f->blk[l].preds[pp];
    if (f->blk[l].preds[pl] != l || P == l || f->blk[P].term != TERM_JMP)
        return 0;
    for (v = f->blk[l].phis; v; v = f->ins[v].next) {
        if (v >= n0)
            return 0;
        cnt[v] = 0;
        kind[v] = K_NONE;
    }
    nlinv = 0;
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        if (v >= n0)
            return 0;
        cnt[v] = 0;
        kind[v] = K_NONE;
        mmk[v] = 0;
        ioff[v] = 0;
        if (f->ins[v].op == OP_LOAD && f->ins[f->ins[v].a].blk != l && nlinv < MAX_LINV) {
            kind[v] = K_LINV;
            linv_der[nlinv] = 0;
            linv[nlinv++] = v;
        }
    }
    cond = f->blk[l].arg;
    if (cond < n0)
        two_bounds(l, P, cond);
    ci = &f->ins[cond];
    missv = cond;
    if (cond >= n0 || (ci->op != OP_SLT && ci->op != OP_ULT && ci->op != OP_NE && ci->op != OP_SGT && ci->op != OP_SLE &&
                       ci->op != OP_ULE) || ci->blk != l)
        MISS("a loop test that is not handled");
    missv = 0;
    t = ci->a;
    n = ci->b;
    if (!INV(n) || INV(t))
        MISS("the loop bound changes inside the loop, or the test is not on a counter");

    /* uses inside the loop */
    for (v = f->blk[l].phis; v; v = f->ins[v].next) {
        u32 x = f->xargs[f->ins[v].a + pl];
        if (!INV(x))
            cnt[x]++;
    }
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v];
        u32 *ops, nops;
        if (i->op == OP_CALL && !vmath_of(v)) {
            if (opt_vec_report && opt_obj && f->ins[i->c].op == OP_ADDR && !(i->flags & (IF_ASM | IF_ATOMIC | IF_ALLOCA)))
                fprintf(stderr, "%s: loop not vectorised: it calls a function (%s)\n", f->name,
                        intern_str(opt_obj->syms[f->ins[f->ins[v].c].a].name));
            else
                MISS("it calls a function");
            return 0;
        }
        IR_OPS(f, i, ops, nops);
        for (k = 0; k < nops; k++)
            if (!INV(ops[k]))
                cnt[ops[k]]++;
    }

    /* every value carried round the loop is an induction variable or a sum */
    niv = 0;
    for (ph = f->blk[l].phis; ph; ph = f->ins[ph].next) {
        u32 r1 = f->xargs[f->ins[ph].a + pl];
        Ins *ri = &f->ins[r1];
        if (INV(r1) || ri->ty != f->ins[ph].ty)
            MISS("a value carried from one iteration to the next");
        if (ri->op == OP_ADD && ri->a == ph && f->ins[ri->b].op == OP_CONST) {
            if (niv == MAX_IV)
                MISS("too many counters");
            iv_phi[niv] = ph;
            iv_next[niv] = r1;
            iv_init[niv] = f->xargs[f->ins[ph].a + pp];
            iv_step[niv] = ri->ty == IT_I32 ? (i64)(i32)f->ins[ri->b].a : (i64)((u64)f->ins[ri->b].a | (u64)f->ins[ri->b].b << 32);
            kind[ph] = K_IVPHI;
            kind[r1] = K_IVNEXT;
            niv++;
            continue;
        }
        if (nred == MAX_RED || cnt[r1] != 1)
            MISS("a value carried from one iteration to the next and used in between");
        red_phi[nred] = ph;
        red_next[nred] = r1;
        if (ri->op == OP_SELECT) {
            /* if (x > m) m = x: a running maximum or minimum */
            Ins *q = &f->ins[ri->a];
            u32 x = ri->b == ph ? ri->c : ri->b;
            int gt, uns_cmp, takes_x_when_true = ri->b != ph;
            /* floating point (x86-64's maxps / minps), with -ffast-math. maxps (x, m) is the select m = x > m ? x : m lane by
               lane, NaNs included: either operand unordered gives the second, m, as the select's false arm does; equal
               ones give m too, and with >= x instead, which differs only in the sign of a zero. A NaN start stays NaN in
               every lane. What differs is the order in which the lanes' results are combined, and that changes only
               which zero comes out: so -fno-signed-zeros suffices when the false arm keeps m. m = m > x ? m : x keeps
               the last NaN seen, an order no lanes can follow: that one needs -ffinite-math-only. */
            int fl = (ri->ty == IT_F32 || ri->ty == IT_F64) && opt_fast_math && opt_arch == ARCH_X64 &&
                     (opt_finite_math || (opt_no_signed_zeros && takes_x_when_true));
            if ((ri->ty != IT_I32 && !fl) || INV(ri->a) || cnt[ri->a] != 1 || cnt[ph] != 2 || (ri->b != ph && ri->c != ph) || x == ph ||
                !((q->a == x && q->b == ph) || (q->a == ph && q->b == x)))
                MISS("a value carried from one iteration to the next that is not a sum, maximum or minimum");
            switch (q->op) {
            case OP_SGT: case OP_SGE: gt = 1; uns_cmp = 0; break;
            case OP_SLT: case OP_SLE: gt = 0; uns_cmp = 0; break;
            case OP_UGT: case OP_UGE: gt = 1; uns_cmp = 1; break;
            case OP_ULT: case OP_ULE: gt = 0; uns_cmp = 1; break;
            case OP_FGT: case OP_FGE: gt = 1; uns_cmp = 0; break;
            case OP_FLT: case OP_FLE: gt = 0; uns_cmp = 0; break;
            default: MISS("an operation that has no vector form");
            }
            if (fl != (q->op >= OP_FEQ && q->op <= OP_FGE))
                MISS("an operation that has no vector form");
            /* is the larger of the two kept? */
            if ((q->a == x) != takes_x_when_true)
                gt = !gt;
            red_op[nred] = gt ? (uns_cmp ? OP_VUMAX : OP_VSMAX) : (uns_cmp ? OP_VUMIN : OP_VSMIN);
            red_x[nred] = x;
            kind[ri->a] = K_SKIP;
        } else {
            /* floating-point sums come out differently when added in another order: only on request.
               x86-64: any |= s[k], all &= m[i], parity ^= b[i] likewise, in any order (MCC_NOVECBITRED: not) */
            int bitred = (ri->op == OP_OR || ri->op == OP_AND || ri->op == OP_XOR) && vec_bitred && (ri->ty == IT_I32 || ri->ty == IT_I64);
            if (!(ri->op == OP_ADD || (ri->op == OP_FADD && opt_fast_math) || bitred) || (ri->a != ph && ri->b != ph) || ri->a == ri->b ||
                cnt[ph] != 1)
                MISS("a value carried from one iteration to the next that is not a sum (floating-point sums need -ffast-math)");
            red_op[nred] = ri->op;
            red_x[nred] = ri->a == ph ? ri->b : ri->a;
        }
        kind[r1] = K_RED;
        nred++;
    }
    c = iv_index(t);
    if (c < 0)
        MISS("the loop test is not on a counter");
    /* sext (i + k) and sext (i - k) that only index: the addition is folded into where the array starts */
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v], *a = &f->ins[i->a];
        /* (zero-extended, i + k is that too when it cannot wrap: the counter runs from a constant that is not
           negative to a constant bound, and bound + k is still a positive int) */
        int zok = 0;
        if (i->op == OP_ZEXT32 && !INV(i->a) && a->ty == IT_I32 && a->op == OP_ADD && f->ins[a->b].op == OP_CONST &&
            f->ins[n].op == OP_CONST && (ci->op == OP_ULT || ci->op == OP_SLT) && f->ins[iv_init[c]].op == OP_CONST &&
            iv_step[c] == 1) {
            u64 kk = f->ins[a->b].a, bb = f->ins[n].a, i0 = f->ins[iv_init[c]].a;
            zok = kk < 0x10000 && bb < 0x7fff0000 && i0 < 0x7fff0000 && !f->ins[a->b].b && !f->ins[n].b && !f->ins[iv_init[c]].b;
        }
        if ((i->op == OP_SEXT32 || zok) && !INV(i->a) && kind[i->a] == K_IVNEXT && a->ty == IT_I32 && a->op == OP_ADD && a->a == iv_phi[iv_index(i->a)]) {
            base[v] = a->a; /* a[i + 1] where i + 1 is the counter's next value */
            ioff[v] = a->b;
            continue;
        }
        if (i->op == OP_ZEXT32 && !zok && zext_guard && !INV(i->a) && a->op == OP_ADD && a->ty == IT_I32 && cnt[i->a] == 1 &&
            a->a != a->b) {
            /* (unsigned) (k + i) widened, k any value fixed for the loop: the sum may wrap round 2^32, and then the
               elements are not next to each other. The guard in front of the vector loop checks that it does not
               within the loop's count (IOFF_ZX; below, where the arrays' starts are made). */
            u32 x = !INV(a->a) && kind[a->a] == K_IVPHI ? a->a : a->b, y = x == a->a ? a->b : a->a;
            if (!INV(x) && kind[x] == K_IVPHI && INV(y) && f->ins[y].ty == IT_I32 && y < IOFF_ZX && iv_step[iv_index(x)] == 1) {
                base[v] = x;
                ioff[v] = y | IOFF_ZX;
                kind[i->a] = K_SKIP;
            }
            continue;
        }
        if ((i->op != OP_SEXT32 && !zok) || INV(i->a) || (a->op != OP_ADD && a->op != OP_SUB) || a->ty != IT_I32 || cnt[i->a] != 1)
            continue;
        if (!INV(a->a) && kind[a->a] == K_IVPHI && INV(a->b) && f->ins[a->b].ty == IT_I32) {
            base[v] = a->a;
            ioff[v] = a->b | (a->op == OP_SUB ? 0x80000000U : 0);
        } else if (a->op == OP_ADD && !INV(a->b) && kind[a->b] == K_IVPHI && INV(a->a) && f->ins[a->a].ty == IT_I32) {
            base[v] = a->b;
            ioff[v] = a->a;
        } else {
            continue;
        }
        kind[i->a] = K_SKIP;
    }

    lanesz = 0;
    mixed = 0;
    nar = 0;
    ngth = 0;
    nsplat = 0;
    ninvc = 0;
    memset(iv_val, 0, sizeof iv_val);
    iv = 0;
    ext = 0;
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v];
        missv = v;
        if (v == cond || kind[v] == K_IVNEXT || kind[v] == K_SKIP || kind[v] == K_LINV)
            continue;
        if (kind[v] == K_RED) {
            for (k = 0; red_next[k] != v; k++)
                ;
            if (!is_vec(red_x[k]))
                MISS("a value that is neither per-element nor constant in the loop");
            continue;
        }
        if (nlinv < MAX_LINV && ((i->op >= OP_ADD && i->op <= OP_MUL) || (i->op >= OP_AND && i->op <= OP_SAR) ||
                                 (i->op >= OP_FADD && i->op <= OP_FDIV)) &&
            (i->ty == IT_I32 || i->ty == IT_I64 || i->ty == IT_F32 || i->ty == IT_F64) && INV(i->a) && INV(i->b)) {
            /* the same every time round, from values read once: worked out in the preheader */
            kind[v] = K_LINV;
            linv_der[nlinv] = 1;
            linv[nlinv++] = v;
            continue;
        }
        switch (i->op) {
        case OP_SEXT32:
        case OP_ZEXT32:
            if (ioff[v] & IOFF_ZX) {
                /* (the guard makes these consecutive however the counter itself is compared and widened) */
                if (!index_iv(base[v], IT_I32))
                    MISS("an index of an unsupported kind");
                kind[v] = K_IDX;
                shift[v] = 0;
                break;
            }
            if (ioff[v]) {
                /* a[i + k]: signed, so i + k does not wrap, and widened it is i widened plus k widened */
                if (!index_iv(base[v], IT_I32) || (ext && ext != i->op))
                    MISS("an index of an unsupported kind");
                ext = i->op;
                kind[v] = K_IDX;
                shift[v] = 0;
                break;
            }
            if (i->op == OP_SEXT32 && opt_arch == ARCH_X64 && (vw == 32 ? opt_vec_gather > 0 : !f->avx && opt_x64_sse41 && opt_vec_gather > 0) && !INV(i->a) && kind[i->a] != K_IVPHI &&
                f->ins[i->a].ty == IT_I32 && is_vec(i->a)) {
                kind[v] = K_GIDX; /* an index computed lane by lane: the load it addresses gathers */
                shift[v] = 0;
                base[v] = i->a;
                break;
            }
            if (!index_iv(i->a, IT_I32) || (ext && ext != i->op))
                MISS("an index of an unsupported kind");
            ext = i->op;
            kind[v] = K_IDX;
            shift[v] = 0;
            break;
        case OP_SHL:
            if (!INV(i->a) && kind[i->a] == K_GIDX && !shift[i->a] && f->ins[i->b].op == OP_CONST && f->ins[i->b].a <= 3) {
                kind[v] = K_GIDX;
                shift[v] = (u8)f->ins[i->b].a;
                base[v] = base[i->a];
                break;
            }
            if (!INV(i->a) && kind[i->a] == K_ADDR && !shift[i->a] && !ioff[i->a] && f->ins[i->b].op == OP_CONST &&
                f->ins[i->b].a <= 3 && f->ins[i->b].a) {
                /* (i + k) << s with i + k 64 bits wide (size_t indexes): k folded into where the array starts */
                kind[v] = K_IDX;
                ioff[v] = base[i->a] | 0x40000000U;
                shift[v] = (u8)f->ins[i->b].a;
                break;
            }
            if (!is_idx(i->a, &sh))
                goto vshift;
            if (f->ins[i->b].op != OP_CONST || f->ins[i->b].b != 0 || f->ins[i->b].a + sh > 3)
                MISS("an index of an unsupported kind");
            kind[v] = K_IDX;
            ioff[v] = idx_off;
            shift[v] = (u8)(sh + f->ins[i->b].a);
            break;
        case OP_LOAD:
        case OP_STORE:
            if (i->op == OP_LOAD && !INV(i->a) && kind[i->a] == K_GADDR) {
                /* a gather: elements of four bytes or less, each lane its own (lanes of four bytes) */
                if ((u32)mem_size(i->mem) != (u32)1 << shift[i->a] || mem_size(i->mem) > 4 || (i->flags & 0x80) || ngth == MAX_GATH ||
                    !gmono(ioff[i->a], 0))
                    MISS("a gather that is not handled");
                if (!set_lane(M_32))
                    mixed = 1;
                if (maxmem < 4)
                    maxmem = 4;
                gth[ngth++] = v;
                kind[v] = K_VEC;
                nvec++;
                break;
            }
            if (!walks(i->a, (u32)mem_size(i->mem), &key))
                MISS("a memory access that does not move by one element each time round");
            if (!set_lane(i->mem)) {
                if (opt_arch != ARCH_X64)
                    MISS("elements of different sizes");
                mixed = 1;
            }
            if ((u32)mem_size(i->mem) > maxmem)
                maxmem = (u32)mem_size(i->mem);
            for (k = 0; k < nkeys && !(keys[k] == key && koff[k] == walk_off); k++)
                ;
            if (k == nkeys) {
                if (nkeys == MAX_BASES)
                    MISS("too many arrays");
                stored[k] = 0;
                koff[k] = walk_off;
                keysz[nkeys] = (u8)mem_size(i->mem);
                keys[nkeys++] = key;
            }
            if (keysz[k] != (u8)mem_size(i->mem))
                MISS("one array read or written as elements of different sizes");
            if (i->op == OP_LOAD) {
                if (i->flags & 0x80)
                    MISS("an operation that has no vector form");
                kind[v] = K_VEC;
                nvec++;
                break;
            }
            stored[k] = 1;
            if (!(INV(i->b) || is_vec(i->b)))
                MISS("a value that is neither per-element nor constant in the loop");
            if (INV(i->b) && !note_splat(i->b))
                MISS("too many values from outside the loop");
            break;
        case OP_ADD:
            if (i->ty == IT_I64 && (INV(i->a) || INV(i->b))) {
                u32 x = INV(i->a) ? i->b : i->a, y = INV(i->a) ? i->a : i->b;
                if (!INV(x) && kind[x] == K_GIDX) {
                    kind[v] = K_GADDR;
                    shift[v] = shift[x];
                    base[v] = y;
                    ioff[v] = base[x];
                    break;
                }
                if (!INV(x) && kind[x] == K_ADDR && !shift[x] && !ioff[x]) {
                    /* p + (i + k), bytes: k folded into where the array starts */
                    kind[v] = K_ADDR;
                    shift[v] = 0;
                    base[v] = y;
                    ioff[v] = base[x] | 0x40000000U;
                    break;
                }
            }
            if (i->ty == IT_I64 && ((INV(i->b) && is_idx(i->a, &sh)) || (INV(i->a) && is_idx(i->b, &sh)))) {
                kind[v] = K_ADDR;
                ioff[v] = idx_off;
                shift[v] = (u8)sh;
                base[v] = INV(i->b) ? i->b : i->a;
                break;
            }
            /* fall through */
        case OP_MUL:
            if (i->op == OP_MUL && i->ty == IT_I64)
                MISS("a 64-bit multiplication, which has no vector form"); /* no such instruction */
            /* fall through */
        case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
            int va = is_vec(i->a), vb = is_vec(i->b);
            if (!(va || INV(i->a)) || !(vb || INV(i->b)) || !(va || vb))
                MISS("a value that is neither per-element nor constant in the loop");
            if ((!va || !vb) && !note_splat(va ? i->b : i->a))
                MISS("too many values from outside the loop");
            kind[v] = K_VEC;
            nvec++;
            break;
        }
        case OP_EQ: case OP_NE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
        case OP_ULT: case OP_ULE: case OP_UGT: case OP_UGE:
        case OP_FEQ: case OP_FNE: case OP_FLT: case OP_FLE: case OP_FGT: case OP_FGE: {
            /* each lane becomes all ones or all zeros, for a select to use */
            int va = is_vec(i->a), vb = is_vec(i->b);
            if (!(va || INV(i->a)) || !(vb || INV(i->b)) || !(va || vb))
                MISS("a value that is neither per-element nor constant in the loop");
            if ((!va || !vb) && !note_splat(va ? i->b : i->a))
                MISS("too many values from outside the loop");
            kind[v] = K_MASK; /* full width only, unless a select below makes a maximum or minimum of it */
            shift[v] = 0;       /* (narrow lanes: the comparison to build instead, narrow_exts) */
            nvec++;
            break;
        }
        case OP_SELECT:
            if (!INV(i->a) && kind[i->a] == K_MASK && cnt[i->a] == 1 && (i->ty == IT_I32 || i->ty == IT_F32 || i->ty == IT_F64) &&
                (mmk[v] = minmax_of(i))) {
                /* c = a > b ? a : b and its like: one instruction, at any element width */
                if (!(INV(i->b) || is_vec(i->b)) || !(INV(i->c) || is_vec(i->c)))
                    MISS("a value that is neither per-element nor constant in the loop");
                if ((INV(i->b) && !note_splat(i->b)) || (INV(i->c) && !note_splat(i->c)))
                    MISS("too many values from outside the loop");
                kind[i->a] = K_SKIP;
                kind[v] = K_VEC; /* the comparison was counted: it is this */
                break;
            }
            mmk[v] = 0;
            if (INV(i->a) && f->ins[i->a].ty == IT_I32 && (is_vec(i->b) || is_vec(i->c)) && (INV(i->b) || is_vec(i->b)) &&
                (INV(i->c) || is_vec(i->c))) {
                /* a choice fixed for the whole loop (c ? x[i] : 1 - x[i]): the condition spread as a mask */
                if ((INV(i->b) && !note_splat(i->b)) || (INV(i->c) && !note_splat(i->c)) || ninvc == MAX_SPLATS)
                    MISS("too many values from outside the loop");
                for (k = 0; k < ninvc && invc[k] != i->a; k++)
                    ;
                if (k == ninvc) {
                    invc[ninvc++] = i->a;
                    nvec++;
                }
                kind[v] = K_VEC;
                nvec++;
                break;
            }
            if (INV(i->a) || kind[i->a] != K_MASK || !(INV(i->b) || is_vec(i->b)) || !(INV(i->c) || is_vec(i->c)))
                MISS("a condition used in an unsupported way");
            if ((INV(i->b) && !note_splat(i->b)) || (INV(i->c) && !note_splat(i->c)))
                MISS("a condition used in an unsupported way");
            kind[v] = K_VEC;
            nvec++;
            break;
        case OP_SITOF: case OP_UITOF: case OP_FTOSI: case OP_FTOUI:
            /* between integers and floating point of the same width */
            if (!is_vec(i->a))
                MISS("a value that is neither per-element nor constant in the loop");
            full_only = 1;
            kind[v] = K_VEC;
            nvec++;
            break;
        case OP_SHR: case OP_SAR:
            full_only = 1;
            /* fall through */
        vshift:
            /* by a constant; bits shifted in from above are wrong in narrow lanes */
            if (!is_vec(i->a) || f->ins[i->b].op != OP_CONST || f->ins[i->b].b != 0)
                MISS("a value that is neither per-element nor constant in the loop");
            if (f->ins[i->b].a > maxsh)
                maxsh = f->ins[i->b].a;
            kind[v] = K_VEC;
            nvec++;
            break;
        case OP_ZEXT8: case OP_ZEXT16: case OP_SEXT8: case OP_SEXT16:
            /* the low byte or half of a 32-bit lane, extended: an and, or two shifts */
            if (i->ty != IT_I32 || !is_vec(i->a) || ((i->op == OP_ZEXT8 || i->op == OP_ZEXT16) &&
                !note_splat(ir_const(f, IT_I32, i->op == OP_ZEXT8 ? 0xff : 0xffff))))
                MISS("a value that is neither per-element nor constant in the loop");
            if (!INV(i->a) && f->ins[i->a].op == OP_FTOUI)
                mmk[i->a] = 2; /* only its low bits matter */
            full_only = 1;
            kind[v] = K_VEC;
            nvec++;
            break;
        case OP_CALL: {
            /* sqrtf (x) and its like: the function done lane by lane */
            u32 a = f->xargs[i->a];
            int vm = vmath_of(v);
            if (!is_vec(a) || (vm == VM_FABS && !note_splat(ir_const(f, i->ty == IT_F32 ? IT_I32 : IT_I64,
                                                                           i->ty == IT_F32 ? 0x7fffffffU : ~(u64)0 >> 1))))
                MISS("a value that is neither per-element nor constant in the loop");
            shift[v] = (u8)vm;
            kind[v] = K_VEC;
            nvec++;
            break;
        }
        case OP_NEG: case OP_FNEG:
            if (!is_vec(i->a))
                MISS("a value that is neither per-element nor constant in the loop");
            kind[v] = K_VEC;
            nvec++;
            break;
        default:
            MISS("an operation that has no vector form");
        }
    }
    for (v = f->blk[l].first; v; v = f->ins[v].next)
        if (kind[v] == K_MASK)
            full_only = 1; /* a lane of all ones or all zeros from values of the full width */
    if (opt_arch == ARCH_X64 && lanesz && lanesz < 4 && !mixed) {
        /* narrow lanes cannot do it: lanes of four bytes, the elements widened, can (they do what C does) */
        int narrow_ok = maxsh < lanesz * 8 && !full_only;
        u32 nsplat0 = nsplat;
        u64 c0, c1;
        if (ext_lanes) {
            nar = 1;
            narrow_ok = narrow_exts(l, cond);
            /* a count known to be too small for two vectors of narrow lanes: lanes of four bytes still do some of it */
            if (narrow_ok && iv_step[c] == 1 && ir_is_const(f, n, &c1) && ir_is_const(f, iv_init[c], &c0) &&
                (i64)(i32)(u32)c1 - (i64)(i32)(u32)c0 <= (i64)(vw / lanesz * 2))
                narrow_ok = 0;
        }
        for (v = f->blk[l].first; v && narrow_ok; v = f->ins[v].next)
            if ((kind[v] == K_VEC && mmk[v] && !minmax_op(v)) || ((kind[v] == K_VEC || kind[v] == K_MASK) && !x64_has(v)) ||
                (kind[v] == K_VEC && (f->ins[v].ty == IT_F32 || f->ins[v].ty == IT_F64)))
                narrow_ok = 0;
        for (k = 0; k < nred && narrow_ok; k++)
            if (!(red_op[k] == OP_ADD && f->ins[red_x[k]].op == OP_LOAD))
                narrow_ok = 0;
        for (k = 0; k < niv; k++)
            if (iv_val[k] && f->ins[iv_phi[k]].ty == IT_I32)
                narrow_ok = 0; /* the counter as a value has a lane of four bytes */
        mixed = !narrow_ok;
        if (nar && narrow_ok) {
            full_only = 0; /* each use was looked at */
            maxsh = 0;
        } else if (nar) {
            nar = 0;
            nsplat = nsplat0;
            for (v = f->blk[l].first; v; v = f->ins[v].next)
                if (kind[v] == K_MASK)
                    shift[v] = 0;
        }
    }
    if (mixed) {
        if (maxmem > 4)
            MISS("elements of different sizes, some wider than four bytes");
        lanesz = 4;
    }
    if (!lanesz || maxsh >= lanesz * 8 || (full_only && lanesz < 4))
        MISS("no array access, or an operation not possible at this element width");
    for (v = f->blk[l].first; v; v = f->ins[v].next) {
        if (kind[v] == K_VEC && mmk[v] && !minmax_op(v))
            MISS("a maximum or minimum of values that are not what their narrow elements hold");
        if (opt_arch == ARCH_X64 && (kind[v] == K_VEC || kind[v] == K_MASK) && !x64_has(v))
            MISS("an operation that x86-64 has no vector instruction for (or only with -msse4.1)");
    }
    if (opt_arch == ARCH_X64 && !opt_x64_sse41)
        for (k = 0; k < nred; k++)
            if ((red_op[k] == OP_VSMAX || red_op[k] == OP_VSMIN || red_op[k] == OP_VUMAX || red_op[k] == OP_VUMIN) &&
                f->ins[red_phi[k]].ty == IT_I32)
                MISS("a maximum or minimum that x86-64 has an instruction for only with -msse4.1");
    for (k = 0, j = 0; k < nkeys; k++)
        j += stored[k];
    if (!j && !nred)
        MISS("nothing is stored or summed");
    /* (x86-64: vec_pressure below counts what is alive at once; AVX functions' vectors can also be spilled) */
    if (opt_arch != ARCH_X64 && nvec + nred * 5 > MAX_VECS)
        MISS("too much going on for the vector registers");
    for (v = f->blk[l].first; v; v = f->ins[v].next)
        if ((kind[v] == K_VEC && !fits(f->ins[v].ty)) || (kind[v] == K_MASK && !fits(f->ins[f->ins[v].a].ty)))
            MISS("elements of different widths");
    for (k = 0; k < nred; k++) {
        Ins *x = &f->ins[red_x[k]];
        if (fits(f->ins[red_phi[k]].ty) && lanesz >= 4)
            continue;
        /* bytes or shorts added up into an int: each vector is summed across on the spot */
        if (lanesz < 4 && red_op[k] == OP_ADD && f->ins[red_phi[k]].ty == IT_I32 && x->op == OP_LOAD && kind[red_x[k]] == K_VEC) {
            red_op[k] = OP_VSUM;
            wide = 1;
            continue;
        }
        MISS("elements of different widths");
    }
    for (k = 0; k < nred; k++)
        if (wide && red_op[k] != OP_VSUM)
            MISS("elements of different widths");
    for (k = 0; k < niv; k++) {
        if (!iv_val[k])
            continue;
        /* a lane each: only when it is as wide as the elements, and not next to sums (registers) */
        if (lanesz != (f->ins[iv_phi[k]].ty == IT_I32 ? 4u : 8u) || nred || iv_step[k] > 1000 || iv_step[k] < -1000 || (nvec += 3) > MAX_VECS)
            MISS("the loop counter used as a value in a way that is not handled");
    }

    /* on x86-64: four sums side by side each, the counters' steps, two for the instructions made of several */
    if (opt_arch == ARCH_X64) {
        u32 extra = 3 + ninvc, nconst;
        for (k = 0; k < nred; k++)
            extra += red_op[k] == OP_VSUM ? 0 : 4;
        for (k = 0; k < niv; k++)
            extra += iv_val[k] ? 2 : 0;
        for (v = f->blk[l].first; v; v = f->ins[v].next)
            extra += kind[v] == K_MASK && mmk[v];
        /* AVX functions have xmm0-12 (xmm13 is a scratch there), but their vectors can be spilled: a spread value from
           outside the loop is then read from memory where it is used, which costs little (those count half, spread
           constants, a broadcast from .rodata, not at all), and a few spills of the body's own values cost less than
           the eight scalar rounds they replace: the bound is loose (phase_blend_row's atan2 polynomial: 20) */
        nconst = 0;
        for (k = 0; k < nsplat; k++)
            nconst += f->ins[splat_of[k]].op == OP_CONST || f->ins[splat_of[k]].op == OP_FCONST;
        if (vec_pressure(extra) - (f->avx ? nconst + (nsplat - nconst) / 2 : 0) > (f->avx ? 24u : 14u))
            MISS("too many vector values at once for the x86-64 vector registers");
    }
    /* How many times does the loop run? Three ways of counting are known.
       A count that comes out too small is harmless: the original loop does
       whatever the vectors leave. */
    st = iv_step[c];
    cty = f->ins[t].ty;
    tnext = t == iv_next[c];
    uns = ci->op == OP_ULT || ci->op == OP_ULE;
    le = ci->op == OP_SLE || ci->op == OP_ULE; /* i <= n: counted as i < n + 1, which 64 bits hold */
    if (f->ins[n].ty != cty || (le && (cty != IT_I32 || st != 1 || !tnext)))
        MISS("a loop test that is not handled");
    if (st == 1 && tnext && ci->op != OP_SGT) {
        /* for (i = a; i < n; i++), or i != n */
        if (ci->op == OP_NE)
            uns = iv == iv_phi[c] && ext == OP_ZEXT32;
        else if (iv == iv_phi[c] && ext && (ext == OP_ZEXT32) != uns)
            MISS("the index may wrap around"); /* the index is not widened the way it is compared */
    } else if (st == -1 && (ci->op == OP_NE || ci->op == OP_SGT) && f->ins[n].op == OP_CONST && !f->ins[n].a && !f->ins[n].b) {
        /* while (n--), do ... while (--n), while (n-- > 0) */
    } else if (cty == IT_I64 && (st == 2 || st == 4 || st == 8) && tnext && (ci->op == OP_ULT || ci->op == OP_NE)) {
        /* for (p = a; p < end; p++), or p != end */
    } else {
        MISS("the index may wrap around");
    }
    /* an index that is not what the loop counts with: only signed ones cannot wrap */
    if (iv && iv != iv_phi[c] && ext == OP_ZEXT32)
        MISS("the index may wrap around");

    exact = vec_exact && exact_liveouts(l, red_next, nred);

    /* ---- build ---- */
#define C64(x) ir_const(f, IT_I64, (u64)(x))
    for (k = 0; k < nlinv; k++) {
        if (linv_der[k]) {
            Ins in = f->ins[linv[k]];
            linv_copy[k] = mk(P, in.op, in.ty, outside(in.a), outside(in.b));
            continue;
        }
        linv_copy[k] = mk(P, OP_LOAD, f->ins[linv[k]].ty, f->ins[linv[k]].a, 0);
        f->ins[linv_copy[k]].mem = f->ins[linv[k]].mem;
        f->ins[linv_copy[k]].flags = f->ins[linv[k]].flags;
    }
    n = outside(n);
    /* sums run four vectors side by side, so that the additions do not wait
       for each other; other loops two, to halve what the counting costs */
    un = wide || epi_pass ? 1 : nred ? 4 : 2;
    if (getenv("MCC_VECUN") && (u32)atoi(getenv("MCC_VECUN")) < un && atoi(getenv("MCC_VECUN")) > 0)
        un = (u32)atoi(getenv("MCC_VECUN")); /* (measuring: fewer vectors a round) */
    vf = vw / lanesz * un;
    for (lsh = 0; ((u32)1 << lsh) < lanesz; lsh++)
        ;
    for (k = 0; k < nkeys; k++)
        klsh[k] = (u8)(keysz[k] == 1 ? 0 : keysz[k] == 2 ? 1 : keysz[k] == 4 ? 2 : 3);
    if (st == 1) {
        init64 = cty == IT_I32 ? mk(P, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, iv_init[c], 0) : iv_init[c];
        n64 = cty == IT_I32 ? mk(P, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, n, 0) : n;
        if (le)
            n64 = mk(P, OP_ADD, IT_I64, n64, C64(1));
        count = mk(P, OP_SUB, IT_I64, n64, init64);
        ok = mk(P, uns ? OP_ULT : OP_SLT, IT_I32, init64, n64);
    } else if (st == -1) {
        count = cty == IT_I32 ? mk(P, ci->op == OP_SGT ? OP_SEXT32 : OP_ZEXT32, IT_I64, iv_init[c], 0) : iv_init[c];
        if (!tnext)
            count = mk(P, OP_ADD, IT_I64, count, C64(1));
        ok = 0;
    } else {
        u32 d = mk(P, OP_SUB, IT_I64, n, iv_init[c]);
        int sl = st == 2 ? 1 : st == 4 ? 2 : 3;
        ok = mk(P, OP_ULT, IT_I32, iv_init[c], n);
        if (ci->op == OP_NE)
            ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_EQ, IT_I32, mk(P, OP_AND, IT_I64, d, C64(st - 1)), C64(0)));
        else
            d = mk(P, OP_ADD, IT_I64, d, C64(st - 1));
        count = mk(P, OP_SHR, IT_I64, d, C64(sl));
    }
    v = mk(P, exact ? OP_SGE : OP_SGT, IT_I32, count, C64(vf));
    ok = ok ? mk(P, OP_AND, IT_I32, ok, v) : v;
    /* where each array starts */
    for (k = 0; k < nkeys; k++) {
        int q;
        if (INV(keys[k])) {
            q = iv_index(iv);
            v = koff[k] & IOFF_ZX ? 0 : f->ins[iv].ty == IT_I32 ? mk(P, ext, IT_I64, iv_init[q], 0) : iv_init[q];
            if (koff[k] & IOFF_ZX) {
                /* (unsigned) (k + i): the first index is the sum made 32 bits wide; the rest follow it one by one
                   as long as first + count does not pass 2^32 */
                v = mk(P, OP_ZEXT32, IT_I64, mk(P, OP_ADD, IT_I32, iv_init[q], outside(koff[k] & (IOFF_ZX - 1))), 0);
                ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_ULE, IT_I32, mk(P, OP_ADD, IT_I64, v, count), C64((u64)1 << 32)));
            } else if (koff[k] & 0x40000000U)
                v = mk(P, OP_ADD, IT_I64, v, outside(koff[k] & 0x3fffffffU)); /* already 64 bits wide */
            else if (koff[k])
                v = mk(P, koff[k] >> 31 ? OP_SUB : OP_ADD, IT_I64, v, mk(P, OP_SEXT32, IT_I64, outside(koff[k] & 0x7fffffffU), 0));
            if (klsh[k])
                v = mk(P, OP_SHL, IT_I64, v, C64(klsh[k]));
            start[k] = mk(P, OP_ADD, IT_I64, outside(keys[k]), v);
        } else {
            q = iv_index(keys[k]);
            start[k] = keys[k] == iv_phi[q] ? iv_init[q] : mk(P, OP_ADD, IT_I64, iv_init[q], C64(iv_step[q]));
        }
    }
    /* arrays written must be clear of every other array by a vector's width */
    for (k = 0; k < nkeys; k++) {
        for (j = 0; j < nkeys; j++) {
            if (!stored[k] || j == k || (stored[j] && j < k) || distinct(keys[k], keys[j]))
                continue;
            if (np == MAX_PAIRS)
                MISS("too many arrays to check for overlap"); /* what was added to the preheader is dead code */
            pka[np] = (u8)k;
            pkb[np] = (u8)j;
            pa[np] = start[k];
            pb[np++] = start[j];
        }
    }
    for (k = 0; k < np && mixed; k++) {
        /* arrays of different strides: the whole of each written one clear of the whole of the other */
        u32 ka = pka[k], kb = pkb[k], ea, eb;
        ea = mk(P, OP_ADD, IT_I64, pa[k], klsh[ka] ? mk(P, OP_SHL, IT_I64, count, C64(klsh[ka])) : count);
        eb = mk(P, OP_ADD, IT_I64, pb[k], klsh[kb] ? mk(P, OP_SHL, IT_I64, count, C64(klsh[kb])) : count);
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_OR, IT_I32, mk(P, OP_ULE, IT_I32, ea, pb[k]), mk(P, OP_ULE, IT_I32, eb, pa[k])));
    }
    for (k = 0; k < np && !mixed; k++) {
        u32 d = mk(P, OP_SUB, IT_I64, pa[k], pb[k]);
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_UGE, IT_I32, d, C64(vw)));
        d = mk(P, OP_SUB, IT_I64, pb[k], pa[k]);
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_UGE, IT_I32, d, C64(vw)));
    }
    /* what is read once must not be among the elements written: the reads at constant distances from one address
       (a[0], a[1], m[d][2] ...) checked as the one range they span */
    for (k = 0; k < nlinv; k++) {
        u32 a, aend, r;
        i64 lo, hi, o;
        if (linv_der[k])
            continue;
        linv_root(f->ins[linv[k]].a, &r, &lo);
        hi = lo + mem_size(f->ins[linv[k]].mem);
        for (j = 0; j < k; j++) {
            if (linv_der[j])
                continue;
            linv_root(f->ins[linv[j]].a, &a, &o);
            if (a == r)
                break;
        }
        if (j < k)
            continue; /* (checked with the first of them) */
        for (j = k + 1; j < nlinv; j++) {
            if (linv_der[j])
                continue;
            linv_root(f->ins[linv[j]].a, &a, &o);
            if (a != r)
                continue;
            if (o < lo)
                lo = o;
            if (o + mem_size(f->ins[linv[j]].mem) > hi)
                hi = o + mem_size(f->ins[linv[j]].mem);
        }
        a = lo ? mk(P, OP_ADD, IT_I64, r, C64(lo)) : r;
        aend = mk(P, OP_ADD, IT_I64, r, C64(hi));
        for (j = 0; j < nkeys; j++) {
            u32 send;
            if (!stored[j])
                continue;
            send = mk(P, OP_ADD, IT_I64, start[j], klsh[j] ? mk(P, OP_SHL, IT_I64, count, C64(klsh[j])) : count);
            ok = mk(P, OP_AND, IT_I32, ok,
                    mk(P, OP_OR, IT_I32, mk(P, OP_ULE, IT_I32, aend, start[j]), mk(P, OP_UGE, IT_I32, a, send)));
        }
    }
    /* what a gather may read, from its index's first value to its last, must not be among the elements written */
    for (k = 0; k < ngth; k++) {
        u32 ga = f->ins[gth[k]].a, sh2 = shift[ga], lo, hi, glo, ghi, b0 = outside(base[ga]);
        grange(ioff[ga], P, count, &lo, &hi);
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_SLE, IT_I32, lo, hi));
        glo = mk(P, OP_SEXT32, IT_I64, lo, 0);
        ghi = mk(P, OP_SEXT32, IT_I64, hi, 0);
        if (sh2) {
            glo = mk(P, OP_SHL, IT_I64, glo, C64(sh2));
            ghi = mk(P, OP_SHL, IT_I64, ghi, C64(sh2));
        }
        glo = mk(P, OP_ADD, IT_I64, b0, glo);
        ghi = mk(P, OP_ADD, IT_I64, mk(P, OP_ADD, IT_I64, b0, ghi), C64((u64)1 << sh2));
        for (j = 0; j < nkeys; j++) {
            u32 send;
            if (!stored[j])
                continue;
            send = mk(P, OP_ADD, IT_I64, start[j], klsh[j] ? mk(P, OP_SHL, IT_I64, count, C64(klsh[j])) : count);
            ok = mk(P, OP_AND, IT_I32, ok,
                    mk(P, OP_OR, IT_I32, mk(P, OP_ULE, IT_I32, ghi, start[j]), mk(P, OP_UGE, IT_I32, glo, send)));
        }
    }
    vcnt = exact ? count : mk(P, OP_SUB, IT_I64, count, C64(1)); /* (not exact: at least one iteration is left over) */
    vcnt = mk(P, OP_AND, IT_I64, vcnt, C64(~(u64)(vf - 1)));
    offend = lsh ? mk(P, OP_SHL, IT_I64, vcnt, C64(lsh)) : vcnt;
    V = ir_new_block(f);
    M = ir_new_block(f);
    f->blk[V].freq = f->blk[l].freq;
    f->blk[M].freq = f->blk[P].freq;
    f->blk[P].term = TERM_BR;
    f->blk[P].arg = ok;
    f->blk[P].succ[0] = V;
    f->blk[P].succ[1] = l;
    ir_add_pred(f, V, P);
    ir_add_pred(f, V, V);
    f->blk[V].term = TERM_BR;
    f->blk[V].succ[0] = V;
    f->blk[V].succ[1] = M;
    ir_add_pred(f, M, V);
    f->blk[M].term = TERM_JMP;
    f->blk[M].succ[0] = l;
    ir_add_pred(f, l, M);

    /* the vector loop counts a byte offset, the same for every array */
    ivv = ir_phi(f, V, IT_I64);
    f->xargs[f->ins[ivv].a] = C64(0);
    nsplat = 0;
    nrcp = 0;
    memset(invm, 0, sizeof invm);
    for (k = 0; k < nred; k++) {
        /* sums start from nothing; a maximum or minimum starts from the value so far in every lane */
        int ty = f->ins[red_phi[k]].ty;
        u32 z;
        if (red_op[k] == OP_VSUM) {
            acc[k][0] = ir_phi(f, V, ty); /* the total stays an ordinary number */
            f->xargs[f->ins[acc[k][0]].a] = f->xargs[f->ins[red_phi[k]].a + pp];
            continue;
        }
        z = red_op[k] == OP_ADD || red_op[k] == OP_OR || red_op[k] == OP_XOR ? ir_const(f, ty, 0)
            : red_op[k] == OP_AND ? ir_const(f, ty, ty == IT_I32 ? 0xffffffffU : ~(u64)0)
            : red_op[k] == OP_FADD ? ir_fconst(f, ty, 0.0) : f->xargs[f->ins[red_phi[k]].a + pp];
        z = mk(P, OP_VSPLAT, vty, z, 0);
        f->ins[z].mem = (u8)lane_kind(ty);
        for (u = 0; u < un; u++) {
            acc[k][u] = ir_phi(f, V, vty);
            f->ins[acc[k][u]].mem = (u8)lane_kind(ty);
            f->xargs[f->ins[acc[k][u]].a] = z;
        }
    }
    for (k = 0; k < niv; k++) {
        /* in lanes: init, init + step, ..., all moving on by a vector's worth each time round */
        int ty = f->ins[iv_phi[k]].ty;
        u32 io, ph2, nx;
        if (!iv_val[k])
            continue;
        io = mk(P, OP_VIOTA, vty, (u32)(i32)iv_step[k], 0);
        f->ins[io].mem = (u8)lane_kind(ty);
        io = mk(P, OP_ADD, vty, vec_operand(iv_init[k], P), io);
        f->ins[io].mem = (u8)lane_kind(ty);
        ph2 = ir_phi(f, V, vty);
        f->ins[ph2].mem = (u8)lane_kind(ty);
        f->xargs[f->ins[ph2].a] = io;
        nx = mk(V, OP_ADD, vty, ph2, vec_operand(ir_const(f, ty, (u64)(iv_step[k] * (i64)vf)), P));
        f->ins[nx].mem = (u8)lane_kind(ty);
        f->xargs[f->ins[ph2].a + 1] = nx;
        ivvec[k] = ph2;
    }
    for (u = 0; u < un; u++) {
        offv = u ? mk(V, OP_ADD, IT_I64, ivv, C64(vw * u)) : ivv;
        offn[0] = offn[1] = 0; /* the offsets in arrays of narrower elements, made when first needed */
        for (k = 0; k < niv; k++) {
            int ty = f->ins[iv_phi[k]].ty;
            if (!iv_val[k])
                continue;
            map[iv_phi[k]] = ivvec[k];
            if (u) {
                /* the later vectors of the same round are further along */
                map[iv_phi[k]] = mk(V, OP_ADD, vty, ivvec[k], vec_operand(ir_const(f, ty, (u64)(iv_step[k] * (i64)(vf / un * u))), P));
                f->ins[map[iv_phi[k]]].mem = (u8)lane_kind(ty);
            }
        }
        for (v = f->blk[l].first; v; v = f->ins[v].next) {
            Ins in = f->ins[v];
            u32 nv, x, y;
            int lk = lane_kind(in.ty);
            if (v == cond || kind[v] == K_IVNEXT || kind[v] == K_IDX || kind[v] == K_ADDR || kind[v] == K_SKIP || kind[v] == K_LINV ||
                kind[v] == K_GIDX || kind[v] == K_GADDR)
                continue;
            if (kind[v] == K_RED) {
                for (k = 0; red_next[k] != v; k++)
                    ;
                if (red_op[k] == OP_VSUM) {
                    int xm = f->ins[red_x[k]].mem;
                    nv = mk(V, OP_VSUM, IT_I32, map[red_x[k]], 0);
                    f->ins[nv].mem = (u8)lane_kind(IT_I32);
                    f->ins[nv].flags = (u8)(xm == M_8S || xm == M_16S ? 6 : 5);
                    nv = mk(V, OP_ADD, IT_I32, acc[k][0], nv);
                    f->xargs[f->ins[acc[k][0]].a + 1] = nv;
                    acc[k][0] = nv;
                    continue;
                }
                if (red_op[k] >= OP_VSMAX && red_op[k] <= OP_VUMIN && (f->ins[red_phi[k]].ty == IT_F32 || f->ins[red_phi[k]].ty == IT_F64))
                    nv = mk(V, red_op[k], vty, vec_operand(red_x[k], P), acc[k][u]); /* maxps (x, m): m where unordered */
                else
                    nv = mk(V, red_op[k], vty, acc[k][u], vec_operand(red_x[k], P));
                f->xargs[f->ins[acc[k][u]].a + 1] = nv;
                acc[k][u] = nv;
            } else if (in.op == OP_LOAD && kind[in.a] == K_GADDR) {
                u32 ga = in.a;
                nv = mk(V, OP_VGATHER, vty, outside(base[ga]), vec_operand(ioff[ga], P));
                f->ins[nv].c = shift[ga];
                f->ins[nv].flags = in.mem; /* the elements' kind; the lanes are four bytes */
            } else if (in.op == OP_LOAD || in.op == OP_STORE) {
                u32 off = offv;
                walks(in.a, (u32)mem_size(in.mem), &key);
                for (k = 0; !(keys[k] == key && koff[k] == walk_off); k++)
                    ;
                if (mixed && klsh[k] < 2) {
                    if (!offn[klsh[k]])
                        offn[klsh[k]] = mk(V, OP_SHR, IT_I64, offv, C64(2 - klsh[k]));
                    off = offn[klsh[k]];
                }
                if (in.op == OP_LOAD) {
                    nv = mk(V, OP_VLOAD, vty, start[k], off);
                } else {
                    lk = lane_kind(f->ins[in.b].ty);
                    x = vec_operand(in.b, P);
                    nv = mk(V, OP_VSTORE, IT_VOID, start[k], x);
                    f->ins[nv].c = off;
                }
                if (mixed && klsh[k] < 2)
                    f->ins[nv].flags = (u8)(0x80 | in.mem); /* widened as read, narrowed as written (x64.c) */
            } else if (in.op == OP_SELECT && mmk[v]) {
                x = vec_operand(in.b, P);
                y = vec_operand(in.c, P);
                nv = mk(V, minmax_op(v), vty, x, y);
            } else if (in.op == OP_SELECT) {
                u32 msk = 0;
                x = vec_operand(in.b, P);
                y = vec_operand(in.c, P);
                if (INV(in.a)) {
                    for (k = 0; invc[k] != in.a; k++)
                        ;
                    if (!invm[k]) {
                        /* all ones in every lane where the condition holds, in the preheader */
                        int it = lanesz == 8 ? IT_I64 : IT_I32;
                        u32 c0 = outside(in.a), nz = mk(P, OP_NE, IT_I32, c0, ir_const(f, IT_I32, 0));
                        if (it == IT_I64)
                            nz = mk(P, OP_ZEXT32, IT_I64, nz, 0);
                        invm[k] = mk(P, OP_VSPLAT, vty, mk(P, OP_SUB, it, ir_const(f, it, 0), nz), 0);
                        f->ins[invm[k]].mem = (u8)lane_kind(it);
                    }
                    msk = invm[k];
                } else {
                    msk = map[in.a];
                }
                nv = mk(V, OP_SELECT, vty, msk, x);
                f->ins[nv].c = y;
            } else if (in.op == OP_CALL) {
                x = vec_operand(f->xargs[in.a], P);
                if (shift[v] == VM_FABS) {
                    nv = mk(V, OP_AND, vty, x, vec_operand(ir_const(f, in.ty == IT_F32 ? IT_I32 : IT_I64,
                                                                          in.ty == IT_F32 ? 0x7fffffffU : ~(u64)0 >> 1), P));
                } else {
                    nv = mk(V, OP_VMATH, vty, x, 0);
                    f->ins[nv].b = (u32)shift[v] - 1;
                }
            } else if (nar && (in.op == OP_ZEXT8 || in.op == OP_SEXT8 || in.op == OP_ZEXT16 || in.op == OP_SEXT16) &&
                       lanesz == (in.op == OP_ZEXT8 || in.op == OP_SEXT8 ? 1u : 2u)) {
                map[v] = vec_operand(in.a, P); /* the lane is the byte (the short) already */
                continue;
            } else if (nar && lanesz == 1 && (in.op == OP_SHL || in.op == OP_SHR || in.op == OP_SAR)) {
                /* bytes: shifted as halves of 16-bit lanes, then the bits that came over from the neighbour cleared
                   (an arithmetic shift here is of a zero-extended lane, narrow_exts: the logical one) */
                u32 sc = f->ins[in.b].a;
                x = mk(V, in.op == OP_SHL ? OP_SHL : OP_SHR, vty, vec_operand(in.a, P), in.b);
                f->ins[x].mem = M_16U;
                nv = mk(V, OP_AND, vty, x, vec_operand(ir_const(f, IT_I32, in.op != OP_SHL ? 0xffu >> sc : (0xffu << sc) & 0xff), P));
            } else if (nar && in.op == OP_SAR && (ext_of(in.a) & 1)) {
                nv = mk(V, OP_SHR, vty, vec_operand(in.a, P), in.b);
            } else if (in.op == OP_ZEXT8 || in.op == OP_ZEXT16) {
                nv = mk(V, OP_AND, vty, vec_operand(in.a, P), vec_operand(ir_const(f, IT_I32, in.op == OP_ZEXT8 ? 0xff : 0xffff), P));
            } else if (in.op == OP_SEXT8 || in.op == OP_SEXT16) {
                u32 cn = ir_const(f, IT_I32, lanesz * 8 - (in.op == OP_SEXT8 ? 8 : 16));
                x = mk(V, OP_SHL, vty, vec_operand(in.a, P), cn);
                f->ins[x].mem = (u8)lk;
                nv = mk(V, OP_SAR, vty, x, cn);
            } else if (in.op == OP_SHL || in.op == OP_SHR || in.op == OP_SAR) {
                nv = mk(V, in.op, vty, vec_operand(in.a, P), in.b); /* the count stays a constant */
            } else if (ir_opcount_tab[in.op] == 1) {
                /* x86-64 takes only unsigned values below 2^31 (x64_has): the signed conversion */
                nv = mk(V, opt_arch == ARCH_X64 && in.op == OP_UITOF ? OP_SITOF : opt_arch == ARCH_X64 && in.op == OP_FTOUI ? OP_FTOSI : in.op, vty, vec_operand(in.a, P), 0);
            } else if (in.op == OP_FDIV && INV(in.b) && (opt_reciprocal_math || exact_recip(in.b)) && nsplat < MAX_SPLATS) {
                /* x / c as x * (1 / c), 1 / c worked out once in the preheader: exact when 1 / c is (c a power of
                   two), else -freciprocal-math (in -ffast-math). A vector division takes ten times a multiplication. */
                for (k = 0; k < nrcp && rcp_of[k] != in.b; k++)
                    ;
                if (k == nrcp) {
                    rcp_of[k] = in.b;
                    rcp[k] = mk(P, OP_FDIV, in.ty, ir_fconst(f, in.ty, 1.0), outside(in.b));
                    nrcp++;
                }
                nv = mk(V, OP_FMUL, vty, vec_operand(in.a, P), vec_operand(rcp[k], P));
            } else {
                if (kind[v] == K_MASK)
                    lk = lane_kind(f->ins[in.a].ty);
                x = vec_operand(in.a, P);
                y = vec_operand(in.b, P);
                nv = mk(V, kind[v] == K_MASK && shift[v] ? shift[v] : in.op, vty, x, y);
            }
            f->ins[nv].mem = (u8)lk;
            map[v] = nv;
            if (kind[v] == K_MASK && mmk[v]) {
                /* the comparison as a number: 1 where the mask is all ones */
                lpos[v] = mk(V, OP_AND, vty, nv, vec_operand(ir_const(f, IT_I32, 1), P));
                f->ins[lpos[v]].mem = (u8)lane_kind(IT_I32);
            }
        }
    }
    ivv2 = mk(V, OP_ADD, IT_I64, ivv, C64(vw * un));
    f->xargs[f->ins[ivv].a + 1] = ivv2;
    f->blk[V].arg = mk(V, OP_NE, IT_I32, ivv2, offend);

    /* the original loop carries on from where the vectors stopped */
    for (ph = f->blk[l].phis; ph; ph = f->ins[ph].next) {
        u32 val;
        int q = iv_index(ph);
        if (q >= 0) {
            int ty = f->ins[ph].ty;
            val = ty == IT_I32 ? mk(M, OP_TRUNC, IT_I32, vcnt, 0) : vcnt;
            if (iv_step[q] != 1)
                val = mk(M, OP_MUL, ty, val, ir_const(f, ty, (u64)iv_step[q]));
            val = mk(M, OP_ADD, ty, iv_init[q], val);
            iv_fin[q] = val;
        } else {
            int lk = lane_kind(f->ins[ph].ty), comb;
            for (k = 0; red_phi[k] != ph; k++)
                ;
            comb = red_op[k];
            if (comb == OP_VSUM) {
                ir_phi_add_arg(f, ph, acc[k][0]);
                red_fin[k] = acc[k][0];
                continue;
            }
            val = acc[k][0];
            for (u = 1; u < un; u++) {
                val = mk(M, comb, vty, val, acc[k][u]);
                f->ins[val].mem = (u8)lk;
            }
            val = mk(M, OP_VSUM, f->ins[ph].ty, val, 0);
            f->ins[val].mem = (u8)lk;
            f->ins[val].flags = (u8)(comb == OP_VSMAX ? 1 : comb == OP_VSMIN ? 2 : comb == OP_VUMAX ? 3 : comb == OP_VUMIN ? 4 :
                                     comb == OP_OR ? 7 : comb == OP_AND ? 8 : comb == OP_XOR ? 9 : 0);
            if (comb == OP_ADD || comb == OP_FADD || comb == OP_OR || comb == OP_AND || comb == OP_XOR)
                val = mk(M, comb, f->ins[ph].ty, f->xargs[f->ins[ph].a + pp], val);
            red_fin[k] = val;
        }
        ir_phi_add_arg(f, ph, val);
    }
    if (exact) {
        /* nothing left over: past the original loop, through the block J its exit edge now goes through too, where
           each of its values that is used later is the scalar loop's or what the vectors ended with */
        u32 E = f->blk[l].succ[1], J = ir_new_block(f), b2, x, nops2, jph[MAX_LIVE], fin[MAX_LIVE];
        for (k = 0; k < nlive; k++) {
            u32 o = live_of[k];
            int q = iv_index(o), ty = f->ins[o].ty;
            if (q >= 0) {
                /* the counter after the last iteration, or in it */
                fin[k] = o == iv_next[q] ? iv_fin[q] : mk(M, OP_SUB, ty, iv_fin[q], ir_const(f, ty, (u64)iv_step[q]));
            } else {
                for (j = 0; red_next[j] != o; j++)
                    ;
                fin[k] = red_fin[j];
            }
        }
        f->blk[J].freq = f->blk[E].freq;
        f->blk[M].term = TERM_BR;
        f->blk[M].arg = mk(M, OP_EQ, IT_I32, vcnt, count);
        f->blk[M].succ[0] = J;
        f->blk[M].succ[1] = l;
        f->blk[l].succ[1] = J;
        ir_add_pred(f, J, l);
        ir_add_pred(f, J, M);
        f->blk[J].term = TERM_JMP;
        f->blk[J].succ[0] = E;
        for (k = 0; f->blk[E].preds[k] != l; k++)
            ;
        f->blk[E].preds[k] = J;
        for (k = 0; k < nlive; k++) {
            jph[k] = ir_phi(f, J, f->ins[live_of[k]].ty);
            f->xargs[f->ins[jph[k]].a] = live_of[k];
            f->xargs[f->ins[jph[k]].a + 1] = fin[k];
        }
        for (b2 = 1; b2 < f->nblk && nlive; b2++) {
            Block *bb = &f->blk[b2];
            if (b2 == l || b2 == J || bb->term == TERM_NONE)
                continue;
            if ((bb->term == TERM_BR || bb->term == TERM_RET) && bb->arg)
                for (k = 0; k < nlive; k++)
                    if (bb->arg == live_of[k])
                        bb->arg = jph[k];
            for (j = 0; j < 2; j++)
                for (x = j ? bb->first : bb->phis; x; x = f->ins[x].next) {
                    nops2 = f->ins[x].op == OP_BYVAL ? 1 : ir_opcount(f, x);
                    for (u = 0; u < nops2; u++) {
                        u32 *o = ir_opref(f, x, u);
                        for (k = 0; k < nlive; k++)
                            if (*o == live_of[k])
                                *o = jph[k];
                    }
                }
        }
    }
    if (opt_vec_report)
        fprintf(stderr, "%s: %s vectorised (%u elements at a time)\n", f->name, epi_pass ? "what a vector loop leaves over" : "loop", vf);
    if (vec_epi && !epi_pass && un > 1) {
        u32 R = ir_new_block(f), rp;
        f->blk[R].freq = f->blk[P].freq;
        f->blk[R].term = TERM_JMP;
        f->blk[R].succ[0] = l;
        ir_add_pred(f, R, P);
        ir_add_pred(f, R, M);
        f->blk[P].succ[1] = R;
        f->blk[M].succ[f->blk[M].term == TERM_BR ? 1 : 0] = R;
        /* l's predecessors were P (at pp), itself and M (the third, as are its phis' values from M) */
        for (ph = f->blk[l].phis; ph; ph = f->ins[ph].next) {
            rp = ir_phi(f, R, f->ins[ph].ty);
            f->xargs[f->ins[rp].a] = f->xargs[f->ins[ph].a + pp];
            f->xargs[f->ins[rp].a + 1] = f->xargs[f->ins[ph].a + 2];
            f->xargs[f->ins[ph].a + pp] = rp;
            f->ins[ph].b = 2;
        }
        f->blk[l].preds[pp] = R;
        f->blk[l].npreds = 2;
        epi_pass = 1;
        try_loop(l, n0);
        epi_pass = 0;
    }
    return 1;
}

/* ---- search loops: while (i < n && a[i] != x) i++, for (...) if (a[i] > t) break ----
   A loop of two blocks, each ending in a branch out of it: one tests the counter against a bound fixed for the loop,
   the other tests elements read at the counter (plus a constant); nothing is stored, nothing called, and the counter
   is the only value carried round. In front of it goes a loop that tests 16 bytes of elements at a time and moves
   the counter on while every element of the chunk would carry on; at the first chunk where one would leave (or
   when too few are left) the original loop takes over from the chunk's start and finds the exact element. Reading
   ahead of the scalar loop must not fault where it would not: every read starts 16-byte aligned (checked once,
   before the loop: the chunks keep it), so a chunk never crosses into another page, and its first element is one
   the scalar loop reads. */
static u32 sh_, sb_, sp_, snx_, svp_, sblk_; /* the loop's blocks, its counter and next counter, the vector preheader, the block built in */
static u32 slanesz, snl;
static u32 sl_base[8], sl_c[8], sl_k[8], snld; /* the reads: where the array starts, the constant added to the counter, log2 size */
static u32 sv_of[64], sv_val[64], snv;
static int sdry; /* s_vec only checks: nothing is built, a value it takes is 1 */
#define SIN(x) (f->ins[x].blk == sh_ || f->ins[x].blk == sb_)

/* x is the counter plus a constant: *c */
static int s_ctr(u32 x, i64 *c)
{
    Ins *i = &f->ins[x];
    u64 k;
    if (x == sp_) {
        *c = 0;
        return 1;
    }
    if (i->op == OP_ADD && i->a == sp_ && ir_is_const(f, i->b, &k)) {
        *c = i->ty == IT_I32 ? (i64)(i32)(u32)k : (i64)k;
        return *c >= -64 && *c <= 64;
    }
    return 0;
}

/* the read at address a: base + ((counter + c) << k), widened if the counter is 32 bits */
static int s_addr(u32 a, u32 sz)
{
    Ins *i = &f->ins[a], *o;
    u32 base, off, k = 0, q;
    i64 c;
    if (i->op != OP_ADD || i->ty != IT_I64)
        return 0;
    base = SIN(i->a) ? i->b : i->a;
    off = base == i->a ? i->b : i->a;
    if (SIN(base) || !SIN(off))
        return 0;
    o = &f->ins[off];
    if (o->op == OP_SHL) {
        u64 kk;
        if (!ir_is_const(f, o->b, &kk) || kk > 3)
            return 0;
        k = (u32)kk;
        off = o->a;
        o = &f->ins[off];
    }
    if ((u32)1 << k != sz)
        return 0;
    q = off;
    if (o->op == OP_SEXT32 || o->op == OP_ZEXT32) {
        if (f->ins[sp_].ty != IT_I32)
            return 0;
        q = o->a;
    } else if (f->ins[sp_].ty != IT_I64) {
        return 0;
    }
    if (!s_ctr(q, &c) || snld == 8)
        return 0;
    sl_base[snld] = base;
    sl_c[snld] = (u32)(i32)c;
    sl_k[snld] = k;
    snld++;
    return 1;
}

static int s_lk(int ty)
{
    return ty == IT_F32 ? M_F32 : ty == IT_F64 ? M_F64 : slanesz == 1 ? M_8U : slanesz == 2 ? M_16U : slanesz == 4 ? M_32 : M_64;
}

/* The vector of x's values for the chunk's lanes (built in sblk_; spread values in svp_), 0 if it has none. */
static u32 s_vec(u32 x, u32 civ)
{
    Ins *i = &f->ins[x];
    u32 k, a, b, v = 0;
    for (k = 0; k < snv; k++)
        if (sv_of[k] == x)
            return sv_val[k];
    if (snv == 64)
        return 0;
    if (!SIN(x)) {
        /* fixed for the loop: in every lane */
        u64 c;
        int ty = i->ty;
        if (ty != IT_I32 && ty != IT_I64 && ty != IT_F32 && ty != IT_F64)
            return 0;
        if (slanesz < 4 && !(ir_is_const(f, x, &c) ? c < 256 : slanesz == 1 && ty == IT_I32 && ir_umax32(f, x) < 256))
            return 0; /* (bytes: only values a byte can be, e.g. b[0] read before the loop) */
        if (sdry)
            v = 1;
        else {
            v = mk(svp_, OP_VSPLAT, IT_V128, x, 0);
            f->ins[v].mem = (u8)s_lk(ty);
        }
    } else if (i->op == OP_LOAD) {
        u32 sz = (u32)mem_size(i->mem), n1 = snld, ea;
        if (i->flags || sz != slanesz || (sz < 4 && i->mem != M_8U && i->mem != M_16U) || !s_addr(i->a, sz))
            return 0;
        if (sdry)
            goto done;
        /* base + ((i + c) << k), i the chunk's first counter */
        ea = f->ins[sp_].ty == IT_I32 ? mk(sblk_, OP_SEXT32, IT_I64, civ, 0) : civ;
        if (sl_c[n1])
            ea = mk(sblk_, OP_ADD, IT_I64, ea, ir_const(f, IT_I64, (u64)(i64)(i32)sl_c[n1]));
        if (sl_k[n1])
            ea = mk(sblk_, OP_SHL, IT_I64, ea, ir_const(f, IT_I64, sl_k[n1]));
        v = mk(sblk_, OP_VLOAD, IT_V128, sl_base[n1], ea);
        f->ins[v].mem = (u8)s_lk(i->ty);
    } else {
        int op = i->op, ty;
        switch (op) {
        case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
        case OP_FADD: case OP_FSUB: case OP_FMUL:
            if (slanesz < 4 && op != OP_AND && op != OP_OR)
                return 0; /* (byte lanes: only the masks' and / or) */
            ty = i->ty;
            break;
        case OP_EQ: case OP_NE:
        case OP_FEQ: case OP_FNE: case OP_FLT: case OP_FLE: case OP_FGT: case OP_FGE:
            ty = f->ins[i->a].ty;
            break;
        case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
            if (slanesz < 4)
                return 0;
            ty = f->ins[i->a].ty;
            break;
        default:
            return 0;
        }
        if ((ty == IT_I32 || ty == IT_F32) ? slanesz > 4 : (ty == IT_I64 || ty == IT_F64) ? slanesz != 8 : 1)
            return 0;
        if (op >= OP_EQ && op <= OP_SGE && slanesz == 8 && ty == IT_I64 && op != OP_EQ && op != OP_NE)
            return 0;
        a = s_vec(i->a, civ);
        b = a ? s_vec(i->b, civ) : 0;
        if (!b)
            return 0;
        if (sdry)
            goto done;
        v = mk(sblk_, op, IT_V128, a, b);
        f->ins[v].mem = (u8)s_lk(ty);
    }
done:
    if (sdry)
        v = 1;
    sv_of[snv] = x;
    sv_val[snv++] = v;
    return v;
}

/* Does the tree of d (in the loop) read and compute only what s_vec takes? A dry run: nothing is built. */
static int s_ok(u32 x, int depth)
{
    Ins *i = &f->ins[x];
    if (!SIN(x))
        return 1;
    if (depth > 24 || x == sp_ || x == snx_)
        return 0;
    if (i->op == OP_LOAD) {
        u32 sz = (u32)mem_size(i->mem), n1 = snld;
        if (!slanesz)
            slanesz = sz;
        if (sz != slanesz || !s_addr(i->a, sz))
            return 0;
        snld = n1; /* (counted again when built) */
        return 1;
    }
    if (ir_opcount_tab[i->op] != 2)
        return 0;
    return s_ok(i->a, depth + 1) && s_ok(i->b, depth + 1);
}

static int try_search(u32 h, u32 n0)
{
    Block *hb = &f->blk[h];
    u32 b, P, k, v, init, cc, dc, n, ph, VP, Vh, Vb, Hin, civ, nxt, ok, al, m, sum, lanes32, h2, b2, A2;
    int hk, bk, cont_c, cont_d, uns, ty;
    i64 coff;
    if (opt_arch != ARCH_X64 || hb->term != TERM_BR || hb->npreds != 2 || !hb->phis || f->ins[hb->phis].next)
        return 0;
    for (hk = 0; hk < 2; hk++) {
        b = hb->succ[hk];
        if (b != h && f->blk[b].term == TERM_BR && f->blk[b].npreds == 1 && (f->blk[b].succ[0] == h) != (f->blk[b].succ[1] == h) &&
            !f->blk[b].phis)
            break;
    }
    if (hk == 2)
        return 0;
    bk = f->blk[b].succ[0] == h ? 0 : 1;
    if (hb->succ[1 - hk] == h || hb->succ[1 - hk] == b || f->blk[b].succ[1 - bk] == b)
        return 0;
    k = hb->preds[0] == b ? 1 : 0;
    if (hb->preds[1 - k] != b)
        return 0;
    P = hb->preds[k];
    if (P == h || P == b || f->blk[P].term != TERM_JMP)
        return 0;
    sh_ = h;
    sb_ = b;
    ph = sp_ = hb->phis;
    ty = f->ins[ph].ty;
    init = f->xargs[f->ins[ph].a + k];
    snx_ = f->xargs[f->ins[ph].a + (1 - k)];
    {
        i64 one;
        if ((ty != IT_I32 && ty != IT_I64) || !SIN(snx_) || !s_ctr(snx_, &one) || one != 1 || ph >= n0)
            return 0;
    }
    /* nothing stored, called, or made in another way than s_vec takes; every instruction older than this pass */
    for (v = hb->first; v; v = f->ins[v].next)
        if (v >= n0 || f->ins[v].op == OP_STORE || f->ins[v].op == OP_CALL || f->ins[v].op == OP_XI || f->ins[v].op >= OP_VLOAD)
            return 0;
    for (v = f->blk[b].first; v; v = f->ins[v].next)
        if (v >= n0 || f->ins[v].op == OP_STORE || f->ins[v].op == OP_CALL || f->ins[v].op == OP_XI || f->ins[v].op >= OP_VLOAD)
            return 0;
    /* which test is on the counter: q < n (or n > q), carrying on when it holds; the other tests the elements */
    for (k = 0; k < 2; k++) {
        u32 c = k ? f->blk[b].arg : hb->arg, q, bnd;
        Ins *ci = &f->ins[c];
        int op = ci->op, cont = k ? bk == 0 : hk == 0;
        if (!SIN(c) || ir_opcount_tab[op] != 2)
            continue;
        q = ci->a;
        bnd = ci->b;
        if (SIN(bnd) && !SIN(q)) {
            u32 t = q;
            q = bnd;
            bnd = t;
            op = op == OP_SGT ? OP_SLT : op == OP_UGT ? OP_ULT : op == OP_SLT ? OP_SGT : op == OP_ULT ? OP_UGT : op == OP_SGE ? OP_SLE :
                 op == OP_UGE ? OP_ULE : op == OP_SLE ? OP_SGE : op == OP_ULE ? OP_UGE : op;
        }
        if (!cont) /* leaving when q >= n is carrying on while q < n */
            op = op == OP_SGE ? OP_SLT : op == OP_UGE ? OP_ULT : 0;
        if ((op != OP_SLT && op != OP_ULT) || SIN(bnd) || f->ins[bnd].ty != ty || !s_ctr(q, &coff))
            continue;
        cc = c;
        n = bnd;
        uns = op == OP_ULT;
        break;
    }
    if (k == 2)
        return 0;
    dc = k ? hb->arg : f->blk[b].arg;
    cont_d = k ? hk == 0 : bk == 0;
    cont_c = 1;
    (void)cont_c;
    (void)cc;
    slanesz = 0;
    snld = 0;
    if (!SIN(dc) || !s_ok(dc, 0) || !slanesz || slanesz == 2)
        return 0;
    /* s_ok only finds the lane size: s_vec, dry, must take the whole test before anything is built (it refuses what
       s_ok lets through, e.g. a byte compared with a value fixed for the loop but not a constant; volcomp's
       b[i] != b[0] then summed a register nothing set: z3w.c, test_fiberraster 2026-11-15) */
    sdry = 1;
    snv = 0;
    snld = 0;
    k = s_vec(dc, sp_);
    sdry = 0;
    if (!k)
        return 0;

    /* the loop's values are used after it only by the phis of the blocks it leaves to (the copy would not dominate
       another use) */
    {
        u32 xh = hb->succ[1 - hk], xb = f->blk[b].succ[1 - bk], q, no, bb;
        for (bb = 1; bb < f->nblk; bb++) {
            Block *bl = &f->blk[bb];
            if (bb == h || bb == b)
                continue;
            if ((bl->term == TERM_BR || bl->term == TERM_RET) && bl->arg && SIN(bl->arg))
                return 0;
            for (v = bl->first; v; v = f->ins[v].next) {
                no = ir_opcount(f, v);
                for (q = 0; q < no; q++)
                    if (SIN(*ir_opref(f, v, q)))
                        return 0;
            }
            for (v = bl->phis; v; v = f->ins[v].next) {
                for (q = 0; q < bl->npreds; q++)
                    if (SIN(f->xargs[f->ins[v].a + q]) && !((bb == xh && bl->preds[q] == h) || (bb == xb && bl->preds[q] == b)))
                        return 0;
            }
        }
    }

    /* ---- build: P -> h2 / b2 (a copy of the loop, until the reads are 16-byte aligned) -> A2 -> VP -> (Vh <-> Vb)
       -> Hin -> h (the loop, which finishes) ---- */
    {
        u32 blk2[2], src[2], bb, x, nv;
        h2 = ir_new_block(f);
        b2 = ir_new_block(f);
        A2 = ir_new_block(f);
        VP = ir_new_block(f);
        Vh = ir_new_block(f);
        Vb = ir_new_block(f);
        Hin = ir_new_block(f);
        hb = &f->blk[h];
        f->blk[h2].freq = f->blk[b2].freq = f->blk[A2].freq = f->blk[VP].freq = f->blk[Hin].freq = f->blk[P].freq;
        f->blk[Vh].freq = f->blk[Vb].freq = hb->freq;
        /* the copy: every instruction of h and b, operands from the loop mapped to their copies */
        src[0] = h;
        src[1] = b;
        blk2[0] = h2;
        blk2[1] = b2;
        ir_add_pred(f, h2, P);
        ir_add_pred(f, h2, A2);
        map[ph] = ir_phi(f, h2, ty);
        for (bb = 0; bb < 2; bb++)
            for (v = f->blk[src[bb]].first; v; v = f->ins[v].next) {
                Ins in = f->ins[v];
                x = ir_raw(f, in.op, in.ty, in.a, in.b);
                f->ins[x].c = in.c;
                f->ins[x].mem = in.mem;
                f->ins[x].flags = in.flags;
                f->ins[x].loc = in.loc;
                ir_append(f, blk2[bb], x);
                map[v] = x;
            }
        for (bb = 0; bb < 2; bb++)
            for (x = f->blk[blk2[bb]].first; x; x = f->ins[x].next) {
                u32 no = ir_opcount(f, x), q;
                for (q = 0; q < no; q++) {
                    u32 *r = ir_opref(f, x, q);
                    if (SIN(*r))
                        *r = map[*r];
                }
            }
        /* h2 branches as h does, b2 as b, but b2 carries on to A2 */
        f->blk[h2].term = TERM_BR;
        f->blk[h2].arg = map[hb->arg];
        f->blk[h2].succ[hk] = b2;
        f->blk[h2].succ[1 - hk] = hb->succ[1 - hk];
        f->blk[b2].term = TERM_BR;
        f->blk[b2].arg = map[f->blk[b].arg];
        f->blk[b2].succ[bk] = A2;
        f->blk[b2].succ[1 - bk] = f->blk[b].succ[1 - bk];
        ir_add_pred(f, b2, h2);
        ir_add_pred(f, A2, b2);
        for (bb = 0; bb < 2; bb++) {
            /* the exits gain the copy as a predecessor: their phis take the copied values */
            u32 from = src[bb], to = blk2[bb], X = f->blk[to].succ[bb ? 1 - bk : 1 - hk], pi;
            for (pi = 0; f->blk[X].preds[pi] != from; pi++)
                ;
            ir_add_pred(f, X, to);
            for (x = f->blk[X].phis; x; x = f->ins[x].next) {
                u32 val = f->xargs[f->ins[x].a + pi];
                ir_phi_add_arg(f, x, SIN(val) ? map[val] : val);
            }
        }
        f->blk[P].succ[0] = h2;
        nv = map[snx_];
        f->xargs[f->ins[map[ph]].a] = init;
        f->xargs[f->ins[map[ph]].a + 1] = nv;
        /* A2: on to the vectors once every read of the next round starts 16-byte aligned */
        f->blk[A2].term = TERM_BR;
        f->blk[A2].succ[0] = VP;
        f->blk[A2].succ[1] = h2;
        ir_add_pred(f, VP, A2);
        f->blk[VP].term = TERM_JMP;
        f->blk[VP].succ[0] = Vh;
        ir_add_pred(f, Vh, VP);
        ir_add_pred(f, Vh, Vb);
        f->blk[Vh].term = TERM_BR;
        f->blk[Vh].succ[0] = Vb;
        f->blk[Vh].succ[1] = Hin;
        ir_add_pred(f, Vb, Vh);
        f->blk[Vb].term = TERM_BR;
        f->blk[Vb].succ[0] = Vh;
        f->blk[Vb].succ[1] = Hin;
        ir_add_pred(f, Hin, Vh);
        ir_add_pred(f, Hin, Vb);
        f->blk[Hin].term = TERM_JMP;
        f->blk[Hin].succ[0] = h;
        hb = &f->blk[h];
        for (k = 0; hb->preds[k] != P; k++)
            ;
        hb->preds[k] = Hin;
        init = nv;
    }

    civ = ir_phi(f, Vh, ty);
    /* the vector of the element test, in Vb */
    svp_ = VP;
    sblk_ = Vb;
    snv = 0;
    snld = 0;
    m = s_vec(dc, civ);
    al = 0;
    for (k = 0; k < snld; k++) {
        u32 ea = ty == IT_I32 ? mk(A2, OP_SEXT32, IT_I64, init, 0) : init, t;
        if (sl_c[k])
            ea = mk(A2, OP_ADD, IT_I64, ea, ir_const(f, IT_I64, (u64)(i64)(i32)sl_c[k]));
        if (sl_k[k])
            ea = mk(A2, OP_SHL, IT_I64, ea, ir_const(f, IT_I64, sl_k[k]));
        t = mk(A2, OP_AND, IT_I64, mk(A2, OP_ADD, IT_I64, sl_base[k], ea), ir_const(f, IT_I64, 15));
        al = al ? mk(A2, OP_OR, IT_I64, al, t) : t;
    }
    f->blk[A2].arg = mk(A2, OP_EQ, IT_I32, al, ir_const(f, IT_I64, 0));
    /* Vh: room for a whole chunk: the last lane's counter test holds (in 64 bits: no wrap) */
    snl = 16 / slanesz;
    {
        u32 i64v = ty == IT_I32 ? mk(Vh, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, civ, 0) : civ;
        u32 n64 = ty == IT_I32 ? mk(Vh, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, n, 0) : n;
        u32 last = mk(Vh, OP_ADD, IT_I64, i64v, ir_const(f, IT_I64, (u64)((i64)snl - 1 + coff)));
        ok = mk(Vh, uns && ty == IT_I64 ? OP_ULT : OP_SLT, IT_I32, last, n64);
        if (coff < 0 || (i64)snl - 1 + coff < 0) /* (a counter test on i - k: that the first lane's holds too) */
            ok = mk(Vh, OP_AND, IT_I32, ok, mk(Vh, uns && ty == IT_I64 ? OP_ULT : OP_SLT, IT_I32,
                                               mk(Vh, OP_ADD, IT_I64, i64v, ir_const(f, IT_I64, (u64)coff)), n64));
        if (uns && ty == IT_I64)
            ok = mk(Vh, OP_AND, IT_I32, ok, mk(Vh, OP_UGE, IT_I32, last, i64v)); /* (no wrap past 2^64) */
    }
    f->blk[Vh].arg = ok;
    /* Vb: every lane carries on: the mask's lanes, read as 32-bit numbers, sum to -4 (all ones) or 0 */
    lanes32 = 4;
    sum = mk(Vb, OP_VSUM, IT_I32, m, 0);
    f->ins[sum].mem = M_32;
    f->blk[Vb].arg = mk(Vb, OP_EQ, IT_I32, sum, ir_const(f, IT_I32, cont_d ? (u64)(i64)-(i64)lanes32 : 0));
    nxt = mk(Vb, OP_ADD, ty, civ, ir_const(f, ty, snl));
    f->xargs[f->ins[civ].a] = init;
    f->xargs[f->ins[civ].a + 1] = nxt;
    for (k = 0; f->blk[h].preds[k] != Hin; k++)
        ;
    f->xargs[f->ins[ph].a + k] = civ;
    if (opt_vec_report)
        fprintf(stderr, "%s: search loop vectorised (%u elements at a time)\n", f->name, snl);
    return 1;
}

int opt_vectorise(Func *fn)
{
    u32 b, nblk = fn->nblk, n0 = fn->nins;
    int done = 0;
    f = fn;
    /* x86-64 with AVX2: 32-byte vectors (the instructions are VEX's, x64.c's sel_avx) */
    vw = opt_arch == ARCH_X64 && f->avx && opt_x64_avx2 ? 32 : 16;
    vty = vw == 32 ? IT_V256 : IT_V128;
    ir_resolve_all(f);
    zext_guard = opt_arch == ARCH_X64 && !getenv("MCC_NOVECZX");
    if (opt_arch == ARCH_X64 && !getenv("MCC_NOVECNARROW")) {
        narrow_loops();
        n0 = fn->nins;
    }
    kind = (u8 *)xmalloc((size_t)n0 * 4);
    shift = kind + n0;
    mmk = shift + n0;
    lext = mmk + n0;
    ext_lanes = opt_arch == ARCH_X64 && !getenv("MCC_NOVECEXT");
    vec_exact = opt_arch == ARCH_X64 && !getenv("MCC_NOVECEXACT");
    vec_bitred = opt_arch == ARCH_X64 && !getenv("MCC_NOVECBITRED");
    vec_epi = opt_arch == ARCH_X64 && !getenv("MCC_NOVECEPI");
    epi_pass = 0;
    base = (u32 *)xmalloc((size_t)n0 * 5 * sizeof(u32));
    cnt = base + n0;
    map = cnt + n0;
    lpos = map + n0;
    ioff = lpos + n0;
    for (b = 1; b < nblk; b++)
        if (f->blk[b].term == TERM_BR)
            done += try_loop(b, n0) || try_search(b, n0);
    free(kind);
    free(base);
    return done;
}

