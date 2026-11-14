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

static Func *f;

enum { K_NONE, K_IDX, K_ADDR, K_VEC, K_MASK, K_RED, K_SKIP, K_IVPHI, K_IVNEXT, K_LINV,
       K_GIDX,   /* x86-64: a vector of i32 indexes widened (and shifted: shift), base = the vector; for a gather */
       K_GADDR }; /* base (from outside the loop) + a K_GIDX: base = it, ioff = the index vector, shift */
#define MAX_GATH 4
#define MAX_LINV 4
#define MAX_RED 4
#define MAX_IV 6
#define MAX_BASES 8
#define MAX_PAIRS 6
#define MAX_SPLATS 8
#define MAX_VECS 12 /* few enough that none is ever spilled */

static u8 *kind, *shift;
static u8 *mmk; /* a select that is a lane-by-lane maximum or minimum: MM_* bits, else 0; a comparison: 1 if also used as a number */
static int mixed;  /* x86-64: elements of different sizes, all in lanes of four bytes (narrower ones widened as they are
                      read and narrowed as they are written) */
#define MM_ON 1
#define MM_MAX 2 /* else the minimum */
#define MM_UNS 4 /* compared without sign */
int opt_x64_sse41; /* x86-64: SSE4.1 may be used (pmulld, pmaxsd, ...); -msse4.1, -mavx2, -march=native ... */
Obj *opt_obj;
/* -fvec-gather: loops that read through an index computed lane by lane. Off by default: with SSE4.1 the gather is four
   extract / load / insert steps, measured slower than the scalar loop (clamped row 1.02 vs 1.37 ns, halving 1.20 vs 0.87 ns
   an element); it waits for AVX2's vpgatherdd. */
int opt_vec_gather;
static u32 *base, *cnt, *map, *lpos;
static u32 *ioff; /* an index or address (K_IDX, K_ADDR): the value from outside the loop added to the counter before it
                     was widened (bit 31: taken away), as in a[i + k]; 0 if none */
static u32 idx_off, walk_off;
static u32 gth[MAX_GATH], ngth; /* the gathering loads */ /* that of the index is_idx and the access walks found */
static u32 L;
static u32 iv;  /* the induction variable that indexes arrays, if any */
static int ext; /* how the body widens it when it is 32 bits wide */
static u32 lanesz; /* bytes per element; 0 = not known yet */

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

static u32 outside(u32 x)
{
    u32 k;
    if (f->ins[x].blk != L)
        return x;
    for (k = 0; linv[k] != x; k++)
        ;
    return linv_copy[k];
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

static int note_splat(u32 x)
{
    u32 k;
    for (k = 0; k < nsplat && splat_of[k] != x; k++)
        ;
    if (k == nsplat) {
        if (nsplat == MAX_SPLATS)
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
        splat[j] = mk(P, OP_VSPLAT, IT_V128, outside(x), 0);
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
    if (i->b == i->c || f->ins[q->a].ty != IT_I32 || !((q->a == i->b && q->b == i->c) || (q->a == i->c && q->b == i->b)))
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
    return 0;
}

/* The vector operation of the maximum or minimum v (mmk[v] set), for lanes of lanesz bytes; 0 if none is right. */
static int minmax_op(u32 v)
{
    Ins *i = &f->ins[v];
    int m = mmk[v], uns = (m & MM_UNS) != 0;
    shift[v] = 0;
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
        /* pmaxub pminub pmaxsw pminsw are SSE2; the rest SSE4.1 */
        return (lanesz == 1 && (o == OP_VUMAX || o == OP_VUMIN)) || (lanesz == 2 && (o == OP_VSMAX || o == OP_VSMIN)) || sse41;
    }
    switch (op) {
    case OP_MUL:
        return lanesz == 2 || (lanesz == 4 && sse41);
    case OP_SHL: case OP_SHR:
        return lanesz > 1;
    case OP_SAR:
        return lanesz == 2 || lanesz == 4;
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

static u32 missv; /* the instruction being looked at, for the report */
#define MISS(why) \
    do { \
        if (opt_vec_report) { \
            fprintf(stderr, "%s: loop not vectorised: %s", f->name, why); \
            if (opt_vec_report > 1 && missv) \
                fprintf(stderr, " [op %d ty %d; operands' ops %d %d]", f->ins[missv].op, f->ins[missv].ty, \
                        f->ins[f->ins[missv].a].op, ir_opcount_tab[f->ins[missv].op] >= 2 && ir_opcount_tab[f->ins[missv].op] != 255 ? f->ins[f->ins[missv].b].op : -1); \
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
    int uns, c, cty, tnext, lsh, wide = 0;
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
            linv[nlinv++] = v;
        }
    }
    cond = f->blk[l].arg;
    ci = &f->ins[cond];
    if (cond >= n0 || (ci->op != OP_SLT && ci->op != OP_ULT && ci->op != OP_NE && ci->op != OP_SGT) || ci->blk != l)
        MISS("a loop test that is not handled");
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
            /* floating point: x86-64's maxps / minps differ from the select only with NaNs and signed zeros, which
               -ffast-math lets go */
            int fl = (ri->ty == IT_F32 || ri->ty == IT_F64) && opt_fast_math && opt_arch == ARCH_X64;
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
            /* floating-point sums come out differently when added in another order: only on request */
            if (!(ri->op == OP_ADD || (ri->op == OP_FADD && opt_fast_math)) || (ri->a != ph && ri->b != ph) || ri->a == ri->b ||
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
        if (i->op == OP_SEXT32 && !INV(i->a) && kind[i->a] == K_IVNEXT && a->ty == IT_I32 && a->op == OP_ADD && a->a == iv_phi[iv_index(i->a)]) {
            base[v] = a->a; /* a[i + 1] where i + 1 is the counter's next value */
            ioff[v] = a->b;
            continue;
        }
        if (i->op != OP_SEXT32 || INV(i->a) || (a->op != OP_ADD && a->op != OP_SUB) || a->ty != IT_I32 || cnt[i->a] != 1)
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
    ngth = 0;
    nsplat = 0;
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
        switch (i->op) {
        case OP_SEXT32:
        case OP_ZEXT32:
            if (ioff[v]) {
                /* a[i + k]: signed, so i + k does not wrap, and widened it is i widened plus k widened */
                if (!index_iv(base[v], IT_I32) || (ext && ext != i->op))
                    MISS("an index of an unsupported kind");
                ext = i->op;
                kind[v] = K_IDX;
                shift[v] = 0;
                break;
            }
            if (i->op == OP_SEXT32 && opt_arch == ARCH_X64 && opt_x64_sse41 && opt_vec_gather && !INV(i->a) && kind[i->a] != K_IVPHI &&
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
            nvec++;
            break;
        }
        case OP_SELECT:
            if (!INV(i->a) && kind[i->a] == K_MASK && cnt[i->a] == 1 && i->ty == IT_I32 && (mmk[v] = minmax_of(i))) {
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
        for (v = f->blk[l].first; v && narrow_ok; v = f->ins[v].next)
            if ((kind[v] == K_VEC && mmk[v] && !minmax_op(v)) || ((kind[v] == K_VEC || kind[v] == K_MASK) && !x64_has(v)) ||
                (kind[v] == K_VEC && (f->ins[v].ty == IT_F32 || f->ins[v].ty == IT_F64)))
                narrow_ok = 0;
        for (k = 0; k < nred && narrow_ok; k++)
            if (!(red_op[k] == OP_ADD && f->ins[red_x[k]].op == OP_LOAD))
                narrow_ok = 0;
        mixed = !narrow_ok;
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
    if (nvec + nred * 5 > MAX_VECS)
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
        u32 extra = 3;
        for (k = 0; k < nred; k++)
            extra += red_op[k] == OP_VSUM ? 0 : 4;
        for (k = 0; k < niv; k++)
            extra += iv_val[k] ? 2 : 0;
        for (v = f->blk[l].first; v; v = f->ins[v].next)
            extra += kind[v] == K_MASK && mmk[v];
        if (vec_pressure(extra) > 14)
            MISS("too many vector values at once for the x86-64 vector registers");
    }
    /* How many times does the loop run? Three ways of counting are known.
       A count that comes out too small is harmless: the original loop does
       whatever the vectors leave. */
    st = iv_step[c];
    cty = f->ins[t].ty;
    tnext = t == iv_next[c];
    uns = ci->op == OP_ULT;
    if (f->ins[n].ty != cty)
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

    /* ---- build ---- */
#define C64(x) ir_const(f, IT_I64, (u64)(x))
    for (k = 0; k < nlinv; k++) {
        linv_copy[k] = mk(P, OP_LOAD, f->ins[linv[k]].ty, f->ins[linv[k]].a, 0);
        f->ins[linv_copy[k]].mem = f->ins[linv[k]].mem;
        f->ins[linv_copy[k]].flags = f->ins[linv[k]].flags;
    }
    n = outside(n);
    /* sums run four vectors side by side, so that the additions do not wait
       for each other; other loops two, to halve what the counting costs */
    un = wide ? 1 : nred ? 4 : 2;
    vf = 16 / lanesz * un;
    for (lsh = 0; ((u32)1 << lsh) < lanesz; lsh++)
        ;
    for (k = 0; k < nkeys; k++)
        klsh[k] = (u8)(keysz[k] == 1 ? 0 : keysz[k] == 2 ? 1 : keysz[k] == 4 ? 2 : 3);
    if (st == 1) {
        init64 = cty == IT_I32 ? mk(P, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, iv_init[c], 0) : iv_init[c];
        n64 = cty == IT_I32 ? mk(P, uns ? OP_ZEXT32 : OP_SEXT32, IT_I64, n, 0) : n;
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
    v = mk(P, OP_SGT, IT_I32, count, C64(vf));
    ok = ok ? mk(P, OP_AND, IT_I32, ok, v) : v;
    /* where each array starts */
    for (k = 0; k < nkeys; k++) {
        int q;
        if (INV(keys[k])) {
            q = iv_index(iv);
            v = f->ins[iv].ty == IT_I32 ? mk(P, ext, IT_I64, iv_init[q], 0) : iv_init[q];
            if (koff[k] & 0x40000000U)
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
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_UGE, IT_I32, d, C64(16)));
        d = mk(P, OP_SUB, IT_I64, pb[k], pa[k]);
        ok = mk(P, OP_AND, IT_I32, ok, mk(P, OP_UGE, IT_I32, d, C64(16)));
    }
    /* what is read once must not be among the elements written */
    for (k = 0; k < nlinv; k++) {
        u32 a = f->ins[linv[k]].a, aend = mk(P, OP_ADD, IT_I64, a, C64(mem_size(f->ins[linv[k]].mem)));
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
    vcnt = mk(P, OP_SUB, IT_I64, count, C64(1));
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
    for (k = 0; k < nred; k++) {
        /* sums start from nothing; a maximum or minimum starts from the value so far in every lane */
        int ty = f->ins[red_phi[k]].ty;
        u32 z;
        if (red_op[k] == OP_VSUM) {
            acc[k][0] = ir_phi(f, V, ty); /* the total stays an ordinary number */
            f->xargs[f->ins[acc[k][0]].a] = f->xargs[f->ins[red_phi[k]].a + pp];
            continue;
        }
        z = red_op[k] == OP_ADD ? ir_const(f, ty, 0) : red_op[k] == OP_FADD ? ir_fconst(f, ty, 0.0) : f->xargs[f->ins[red_phi[k]].a + pp];
        z = mk(P, OP_VSPLAT, IT_V128, z, 0);
        f->ins[z].mem = (u8)lane_kind(ty);
        for (u = 0; u < un; u++) {
            acc[k][u] = ir_phi(f, V, IT_V128);
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
        io = mk(P, OP_VIOTA, IT_V128, (u32)(i32)iv_step[k], 0);
        f->ins[io].mem = (u8)lane_kind(ty);
        io = mk(P, OP_ADD, IT_V128, vec_operand(iv_init[k], P), io);
        f->ins[io].mem = (u8)lane_kind(ty);
        ph2 = ir_phi(f, V, IT_V128);
        f->ins[ph2].mem = (u8)lane_kind(ty);
        f->xargs[f->ins[ph2].a] = io;
        nx = mk(V, OP_ADD, IT_V128, ph2, vec_operand(ir_const(f, ty, (u64)(iv_step[k] * (i64)vf)), P));
        f->ins[nx].mem = (u8)lane_kind(ty);
        f->xargs[f->ins[ph2].a + 1] = nx;
        ivvec[k] = ph2;
    }
    for (u = 0; u < un; u++) {
        offv = u ? mk(V, OP_ADD, IT_I64, ivv, C64(16 * u)) : ivv;
        offn[0] = offn[1] = 0; /* the offsets in arrays of narrower elements, made when first needed */
        for (k = 0; k < niv; k++) {
            int ty = f->ins[iv_phi[k]].ty;
            if (!iv_val[k])
                continue;
            map[iv_phi[k]] = ivvec[k];
            if (u) {
                /* the later vectors of the same round are further along */
                map[iv_phi[k]] = mk(V, OP_ADD, IT_V128, ivvec[k], vec_operand(ir_const(f, ty, (u64)(iv_step[k] * (i64)(vf / un * u))), P));
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
                nv = mk(V, red_op[k], IT_V128, acc[k][u], vec_operand(red_x[k], P));
                f->xargs[f->ins[acc[k][u]].a + 1] = nv;
                acc[k][u] = nv;
            } else if (in.op == OP_LOAD && kind[in.a] == K_GADDR) {
                u32 ga = in.a;
                nv = mk(V, OP_VGATHER, IT_V128, outside(base[ga]), vec_operand(ioff[ga], P));
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
                    nv = mk(V, OP_VLOAD, IT_V128, start[k], off);
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
                nv = mk(V, minmax_op(v), IT_V128, x, y);
            } else if (in.op == OP_SELECT) {
                x = vec_operand(in.b, P);
                y = vec_operand(in.c, P);
                nv = mk(V, OP_SELECT, IT_V128, map[in.a], x);
                f->ins[nv].c = y;
            } else if (in.op == OP_CALL) {
                x = vec_operand(f->xargs[in.a], P);
                if (shift[v] == VM_FABS) {
                    nv = mk(V, OP_AND, IT_V128, x, vec_operand(ir_const(f, in.ty == IT_F32 ? IT_I32 : IT_I64,
                                                                          in.ty == IT_F32 ? 0x7fffffffU : ~(u64)0 >> 1), P));
                } else {
                    nv = mk(V, OP_VMATH, IT_V128, x, 0);
                    f->ins[nv].b = (u32)shift[v] - 1;
                }
            } else if (in.op == OP_ZEXT8 || in.op == OP_ZEXT16) {
                nv = mk(V, OP_AND, IT_V128, vec_operand(in.a, P), vec_operand(ir_const(f, IT_I32, in.op == OP_ZEXT8 ? 0xff : 0xffff), P));
            } else if (in.op == OP_SEXT8 || in.op == OP_SEXT16) {
                u32 cn = ir_const(f, IT_I32, in.op == OP_SEXT8 ? 24 : 16);
                x = mk(V, OP_SHL, IT_V128, vec_operand(in.a, P), cn);
                f->ins[x].mem = (u8)lk;
                nv = mk(V, OP_SAR, IT_V128, x, cn);
            } else if (in.op == OP_SHL || in.op == OP_SHR || in.op == OP_SAR) {
                nv = mk(V, in.op, IT_V128, vec_operand(in.a, P), in.b); /* the count stays a constant */
            } else if (ir_opcount_tab[in.op] == 1) {
                /* x86-64 takes only unsigned values below 2^31 (x64_has): the signed conversion */
                nv = mk(V, opt_arch == ARCH_X64 && in.op == OP_UITOF ? OP_SITOF : opt_arch == ARCH_X64 && in.op == OP_FTOUI ? OP_FTOSI : in.op, IT_V128, vec_operand(in.a, P), 0);
            } else {
                if (kind[v] == K_MASK)
                    lk = lane_kind(f->ins[in.a].ty);
                x = vec_operand(in.a, P);
                y = vec_operand(in.b, P);
                nv = mk(V, in.op, IT_V128, x, y);
            }
            f->ins[nv].mem = (u8)lk;
            map[v] = nv;
            if (kind[v] == K_MASK && mmk[v]) {
                /* the comparison as a number: 1 where the mask is all ones */
                lpos[v] = mk(V, OP_AND, IT_V128, nv, vec_operand(ir_const(f, IT_I32, 1), P));
                f->ins[lpos[v]].mem = (u8)lane_kind(IT_I32);
            }
        }
    }
    ivv2 = mk(V, OP_ADD, IT_I64, ivv, C64(16 * un));
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
        } else {
            int lk = lane_kind(f->ins[ph].ty), comb;
            for (k = 0; red_phi[k] != ph; k++)
                ;
            comb = red_op[k];
            if (comb == OP_VSUM) {
                ir_phi_add_arg(f, ph, acc[k][0]);
                continue;
            }
            val = acc[k][0];
            for (u = 1; u < un; u++) {
                val = mk(M, comb, IT_V128, val, acc[k][u]);
                f->ins[val].mem = (u8)lk;
            }
            val = mk(M, OP_VSUM, f->ins[ph].ty, val, 0);
            f->ins[val].mem = (u8)lk;
            f->ins[val].flags = (u8)(comb == OP_VSMAX ? 1 : comb == OP_VSMIN ? 2 : comb == OP_VUMAX ? 3 : comb == OP_VUMIN ? 4 : 0);
            if (comb == OP_ADD || comb == OP_FADD)
                val = mk(M, comb, f->ins[ph].ty, f->xargs[f->ins[ph].a + pp], val);
        }
        ir_phi_add_arg(f, ph, val);
    }
    if (opt_vec_report)
        fprintf(stderr, "%s: loop vectorised (%u elements at a time)\n", f->name, vf);
    return 1;
}

int opt_vectorise(Func *fn)
{
    u32 b, nblk = fn->nblk, n0 = fn->nins;
    int done = 0;
    f = fn;
    ir_resolve_all(f);
    kind = (u8 *)xmalloc((size_t)n0 * 3);
    shift = kind + n0;
    mmk = shift + n0;
    base = (u32 *)xmalloc((size_t)n0 * 5 * sizeof(u32));
    cnt = base + n0;
    map = cnt + n0;
    lpos = map + n0;
    ioff = lpos + n0;
    for (b = 1; b < nblk; b++)
        if (f->blk[b].term == TERM_BR)
            done += try_loop(b, n0);
    free(kind);
    free(base);
    return done;
}
