/* IR construction, folding and printing. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void opcount_init(void);

Func *ir_func_new(void)
{
    Func *f = (Func *)xmalloc(sizeof(Func));
    opcount_init();
    memset(f, 0, sizeof *f);
    f->capins = 1024;
    f->ins = (Ins *)xmalloc(f->capins * sizeof(Ins));
    f->capblk = 64;
    f->blk = (Block *)xmalloc(f->capblk * sizeof(Block));
    f->ctab_size = 256;
    f->ctab = (u32 *)xmalloc(f->ctab_size * sizeof(u32));
    ir_func_reset(f);
    return f;
}

void ir_func_reset(Func *f)
{
    f->nins = 1;
    f->nblk = 1;
    f->nxargs = 0;
    f->nslots = 0;
    f->nparams = 0;
    f->cur = 0;
    f->has_sret = 0;
    f->variadic = 0;
    f->ret_ty = IT_VOID;
    if (f->ctab_size > 1024) { /* one huge function must not tax all later ones */
        free(f->ctab);
        f->ctab_size = 256;
        f->ctab = (u32 *)xmalloc(f->ctab_size * sizeof(u32));
    }
    memset(f->ctab, 0, f->ctab_size * sizeof(u32));
    f->ctab_used = 0;
    f->copies = 1;
    memset(&f->ins[0], 0, sizeof(Ins));
    memset(&f->blk[0], 0, sizeof(Block));
    arena_reset(&f->arena);
}

u32 ir_loc;

u32 ir_raw(Func *f, int op, int ty, u32 a, u32 b)
{
    Ins *i;
    if (f->nins == f->capins) {
        f->capins *= 2;
        f->ins = (Ins *)xrealloc(f->ins, f->capins * sizeof(Ins));
    }
    i = &f->ins[f->nins];
    i->op = (u8)op;
    i->ty = (u8)ty;
    i->mem = 0;
    i->flags = 0;
    i->a = a;
    i->b = b;
    i->c = 0;
    i->blk = 0;
    i->prev = i->next = 0;
    i->aux = 0;
    i->loc = ir_loc;
    i->var = 0;
    return f->nins++;
}

void ir_append(Func *f, u32 blk, u32 v)
{
    Block *b = &f->blk[blk];
    Ins *i = &f->ins[v];
    i->blk = blk;
    i->prev = b->last;
    i->next = 0;
    if (b->last)
        f->ins[b->last].next = v;
    else
        b->first = v;
    b->last = v;
}

void ir_insert_before(Func *f, u32 pos, u32 v)
{
    Ins *p = &f->ins[pos], *i = &f->ins[v];
    Block *b = &f->blk[p->blk];
    i->blk = p->blk;
    i->next = pos;
    i->prev = p->prev;
    if (p->prev)
        f->ins[p->prev].next = v;
    else
        b->first = v;
    p->prev = v;
}

void ir_remove(Func *f, u32 v)
{
    Ins *i = &f->ins[v];
    Block *b = &f->blk[i->blk];
    if (!i->blk)
        return;
    if (i->prev)
        f->ins[i->prev].next = i->next;
    else
        b->first = i->next;
    if (i->next)
        f->ins[i->next].prev = i->prev;
    else
        b->last = i->prev;
    i->blk = 0;
    i->prev = i->next = 0;
}

/* ---- floating values, hash-consed ---- */

static u32 cons(Func *f, int op, int ty, u32 a, u32 b)
{
    u32 h, v, mask;
    if (f->ctab_used * 2 >= f->ctab_size) {
        u32 size = f->ctab_size * 2, k;
        u32 *tab = (u32 *)xmalloc(size * sizeof(u32));
        memset(tab, 0, size * sizeof(u32));
        for (k = 0; k < f->ctab_size; k++) {
            u32 id = f->ctab[k];
            if (id) {
                Ins *i = &f->ins[id];
                h = (i->a * 2654435761U ^ i->b * 40503U ^ ((u32)i->op << 8 | i->ty)) & (size - 1);
                while (tab[h])
                    h = (h + 1) & (size - 1);
                tab[h] = id;
            }
        }
        free(f->ctab);
        f->ctab = tab;
        f->ctab_size = size;
    }
    mask = f->ctab_size - 1;
    h = (a * 2654435761U ^ b * 40503U ^ ((u32)op << 8 | (u32)ty)) & mask;
    while ((v = f->ctab[h]) != 0) {
        Ins *i = &f->ins[v];
        if (i->op == op && i->ty == ty && i->a == a && i->b == b)
            return v;
        h = (h + 1) & mask;
    }
    v = ir_raw(f, op, ty, a, b);
    f->ctab[h] = v;
    f->ctab_used++;
    return v;
}

/* Rebuilds the table of floating values from the instruction array. */
void ir_rehash(Func *f)
{
    u32 v, h, size = f->ctab_size, n = 0;
    for (v = 1; v < f->nins; v++) {
        int op = f->ins[v].op;
        n += !f->ins[v].blk && (op == OP_CONST || op == OP_FCONST || op == OP_ADDR || op == OP_SLOT);
    }
    while (size < n * 2 + 2)
        size *= 2;
    if (size != f->ctab_size) {
        free(f->ctab);
        f->ctab = (u32 *)xmalloc(size * sizeof(u32));
        f->ctab_size = size;
    }
    memset(f->ctab, 0, size * sizeof(u32));
    f->ctab_used = n;
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        if (i->blk || !(i->op == OP_CONST || i->op == OP_FCONST || i->op == OP_ADDR || i->op == OP_SLOT))
            continue;
        h = (i->a * 2654435761U ^ i->b * 40503U ^ ((u32)i->op << 8 | i->ty)) & (size - 1);
        while (f->ctab[h])
            h = (h + 1) & (size - 1);
        f->ctab[h] = v;
    }
}

u32 ir_const(Func *f, int ty, u64 v)
{
    if (ty == IT_I32)
        v = (u64)(i64)(i32)(u32)v;
    return cons(f, OP_CONST, ty, (u32)v, (u32)(v >> 32));
}

u32 ir_fconst(Func *f, int ty, double v)
{
    u64 bits = 0;
    if (ty == IT_F32) {
        float fv = (float)v;
        u32 b32;
        memcpy(&b32, &fv, 4);
        bits = b32;
    } else {
        memcpy(&bits, &v, 8);
    }
    return cons(f, OP_FCONST, ty, (u32)bits, (u32)(bits >> 32));
}

u32 ir_fconst_bits(Func *f, int ty, u64 bits)
{
    return cons(f, OP_FCONST, ty, (u32)bits, (u32)(bits >> 32));
}

u32 ir_addr(Func *f, u32 sym, i64 off)
{
    return cons(f, OP_ADDR, IT_I64, sym, (u32)(i32)off);
}

u32 ir_slot_addr(Func *f, u32 slot, i64 off)
{
    return cons(f, OP_SLOT, IT_I64, slot, (u32)(i32)off);
}

u32 ir_new_slot(Func *f, u64 size, u32 align)
{
    Slot *s;
    if (f->nslots == f->capslots) {
        f->capslots = f->capslots ? f->capslots * 2 : 64;
        f->slots = (Slot *)xrealloc(f->slots, f->capslots * sizeof(Slot));
    }
    s = &f->slots[f->nslots];
    s->size = (u32)size;
    s->align = align;
    s->off = 0;
    return f->nslots++;
}

u32 ir_add_param(Func *f, int ty)
{
    u32 v;
    if (f->nparams == f->capparams) {
        f->capparams = f->capparams ? f->capparams * 2 : 16;
        f->param_ty = (u8 *)xrealloc(f->param_ty, f->capparams);
    }
    f->param_ty[f->nparams] = (u8)ty;
    v = ir_raw(f, OP_PARAM, ty, f->nparams++, 0);
    ir_append(f, 1, v);
    return v;
}

int ir_is_const(Func *f, u32 v, u64 *val)
{
    Ins *i = &f->ins[v];
    if (i->op != OP_CONST)
        return 0;
    *val = (u64)i->a | ((u64)i->b << 32);
    return 1;
}

int ir_is_fconst(Func *f, u32 v, double *val)
{
    Ins *i = &f->ins[v];
    if (i->op != OP_FCONST)
        return 0;
    if (i->ty == IT_F32) {
        float fv;
        memcpy(&fv, &i->a, 4);
        *val = fv;
    } else {
        u64 bits = (u64)i->a | ((u64)i->b << 32);
        memcpy(val, &bits, 8);
    }
    return 1;
}

u32 ir_resolve(Func *f, u32 v)
{
    while (f->ins[v].op == OP_COPY)
        v = f->ins[v].a;
    return v;
}

void ir_replace(Func *f, u32 v, u32 with)
{
    Ins *i = &f->ins[v];
    ir_remove(f, v);
    i->op = OP_COPY;
    f->copies = 1;
    i->a = with;
}

/* Operand counts by opcode; 3 marks the variable cases. */
u8 ir_opcount_tab[OP_COUNT];

static void opcount_init(void)
{
    int op;
    for (op = 0; op < OP_COUNT; op++) {
        switch (op) {
        case OP_NOP: case OP_CONST: case OP_FCONST: case OP_ADDR: case OP_SLOT: case OP_PARAM: case OP_VASTART:
        case OP_VIOTA: case OP_FRAMEADDR:
            ir_opcount_tab[op] = 0;
            break;
        case OP_PHI: case OP_CALL: case OP_XI:
            ir_opcount_tab[op] = 255;
            break;
        case OP_SELECT: case OP_VSTORE:
            ir_opcount_tab[op] = 3;
            break;
        case OP_COPY: case OP_NEG: case OP_NOT: case OP_FNEG: case OP_LOAD: case OP_MEMZERO: case OP_PIN:
        case OP_VSPLAT: case OP_VSUM: case OP_ISCONST: case OP_CLZ: case OP_CTZ: case OP_BSWAP: case OP_POPCNT: case OP_BITCAST:
        case OP_ASMOUT: case OP_TLS: case OP_BYVAL: case OP_VMATH:
            ir_opcount_tab[op] = 1;
            break;
        default:
            ir_opcount_tab[op] = (u8)(op >= OP_SEXT8 && op <= OP_FTRUNC ? 1 : 2);
        }
    }
    ir_opcount_tab[OP_NOP] = 0;
}

u32 ir_opcount(Func *f, u32 v)
{
    Ins *i = &f->ins[v];
    u32 n = ir_opcount_tab[i->op];
    if (n != 255)
        return n;
    return i->op == OP_PHI || i->op == OP_XI ? i->b : i->b + 1;
}

u32 *ir_opref(Func *f, u32 v, u32 k)
{
    Ins *i = &f->ins[v];
    if (i->op == OP_PHI || i->op == OP_XI)
        return &f->xargs[i->a + k];
    if (i->op == OP_CALL)
        return k == 0 ? &i->c : &f->xargs[i->a + k - 1];
    return k == 0 ? &i->a : k == 1 ? &i->b : &i->c;
}

/* ---- folding ---- */

int ir_is_bool(Func *f, u32 v, int depth)
{
    Ins *i = &f->ins[v];
    if ((i->op >= OP_EQ && i->op <= OP_UGE) || (i->op >= OP_FEQ && i->op <= OP_FGE))
        return 1;
    if (i->op == OP_AND && depth) /* (of 0 or 1 and anything) */
        return ir_is_bool(f, i->b, depth - 1) || ir_is_bool(f, i->a, depth - 1);
    if (i->op == OP_OR && depth)
        return ir_is_bool(f, i->b, depth - 1) && ir_is_bool(f, i->a, depth - 1);
    return 0;
}

static int is_cmp(int op)
{
    return op >= OP_EQ && op <= OP_UGE;
}

static int invert_cmp(int op)
{
    switch (op) {
    case OP_EQ: return OP_NE;
    case OP_NE: return OP_EQ;
    case OP_SLT: return OP_SGE;
    case OP_SGE: return OP_SLT;
    case OP_SGT: return OP_SLE;
    case OP_SLE: return OP_SGT;
    case OP_ULT: return OP_UGE;
    case OP_UGE: return OP_ULT;
    case OP_UGT: return OP_ULE;
    case OP_ULE: return OP_UGT;
    case OP_FEQ: return OP_FNE;
    case OP_FNE: return OP_FEQ;
    }
    return 0;
}

static int fold_int(int op, int w64, u64 x, u64 y, u64 *out)
{
    i64 sx = (i64)x, sy = (i64)y;
    u64 ux = w64 ? x : (u64)(u32)x, uy = w64 ? y : (u64)(u32)y;
    u32 sh = (u32)y & (w64 ? 63 : 31);
    u64 r;
    switch (op) {
    case OP_ADD: r = x + y; break;
    case OP_SUB: r = x - y; break;
    case OP_MUL: r = x * y; break;
    case OP_SDIV:
        if (sy == 0)
            return 0;
        r = sy == -1 ? 0 - x : (u64)(sx / sy);
        break;
    case OP_UDIV:
        if (uy == 0)
            return 0;
        r = ux / uy;
        break;
    case OP_SREM:
        if (sy == 0)
            return 0;
        r = sy == -1 ? 0 : (u64)(sx % sy);
        break;
    case OP_UREM:
        if (uy == 0)
            return 0;
        r = ux % uy;
        break;
    case OP_AND: r = x & y; break;
    case OP_OR: r = x | y; break;
    case OP_XOR: r = x ^ y; break;
    case OP_SHL: r = x << sh; break;
    case OP_SHR: r = ux >> sh; break;
    case OP_SAR: r = sx < 0 ? ~(~x >> sh) : x >> sh; break;
    case OP_EQ: r = x == y; break;
    case OP_NE: r = x != y; break;
    case OP_SLT: r = sx < sy; break;
    case OP_SLE: r = sx <= sy; break;
    case OP_SGT: r = sx > sy; break;
    case OP_SGE: r = sx >= sy; break;
    case OP_ULT: r = ux < uy; break;
    case OP_ULE: r = ux <= uy; break;
    case OP_UGT: r = ux > uy; break;
    case OP_UGE: r = ux >= uy; break;
    default: return 0;
    }
    *out = r;
    return 1;
}

static u64 log2u64(u64 v)
{
    u64 n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

/* ---- division by a constant ----
   The quotient is the high part of a product with a rounded-up reciprocal
   (Granlund and Montgomery). 32-bit values have room to spare in 64 bits;
   64-bit ones need the high half of a 128-bit product. */

/* floor(2^p / d) modulo 2^64, for p < 128 and d < 2^63 */
static u64 pow2_div(u32 p, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    u32 k;
    for (k = 0; k <= p; k++) {
        r = (r << 1) | (k == 0);
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    *rem = r;
    return q;
}

#define K64(x) ir_const(f, IT_I64, (u64)(x))
#define K32(v) ir_const(f, IT_I32, (u64)(u32)(v))

/* a / c without dividing; 0 when the divide instruction has to do it */
static u32 div_const(Func *f, int sgn, int ty, u32 a, u64 c)
{
    int w = ty == IT_I64;
    u64 r, mg;
    u32 l, s, t, p, q;
    if (opt_size)
        return 0;
    if (!w && opt_plaindiv && (opt_dev_fn || opt_dev_parse))
        return 0;
    if (!w)
        c = sgn ? (u64)(i64)(i32)c : (u64)(u32)c;
    if (c < 3 || c >> (w ? 62 : 31) || !(c & (c - 1)))
        return 0;
    for (l = 2; ((u64)1 << l) < c; l++)
        ;
    if (!w && (opt_dev_fn || opt_dev_parse)) {
        /* device code: a GPU's registers are 32 bits wide and it has the high half of a 32-bit product, so
           the 32-bit quotient is worked out as the 64-bit one is below, in half the width */
        if (sgn) {
            for (s = 0; s + 2 <= l; s++) {
                mg = pow2_div(32 + s, c, &r) + 1;
                if (c - r <= (u64)2 << s)
                    break;
            }
            if (s + 2 <= l) {
                p = ir_ins(f, OP_SMULH, IT_I32, a, K32(mg));
            } else {
                mg = pow2_div(31 + l, c, &r) + 1;
                p = ir_ins(f, OP_ADD, IT_I32, ir_ins(f, OP_SMULH, IT_I32, a, K32(mg)), a);
                s = l - 1;
            }
            if (s)
                p = ir_ins(f, OP_SAR, IT_I32, p, K32(s));
            return ir_ins(f, OP_ADD, IT_I32, p, ir_ins(f, OP_SHR, IT_I32, a, K32(31)));
        }
        for (s = 0; s < l; s++) {
            mg = pow2_div(32 + s, c, &r) + 1;
            if (c - r <= (u64)1 << s && !(mg >> 32)) {
                q = ir_ins(f, OP_UMULH, IT_I32, a, K32(mg));
                return s ? ir_ins(f, OP_SHR, IT_I32, q, K32(s)) : q;
            }
        }
        mg = pow2_div(32 + l, c, &r) + 1;
        t = ir_ins(f, OP_UMULH, IT_I32, a, K32(mg));
        p = ir_ins(f, OP_SHR, IT_I32, ir_ins(f, OP_SUB, IT_I32, a, t), K32(1));
        return ir_ins(f, OP_SHR, IT_I32, ir_ins(f, OP_ADD, IT_I32, p, t), K32(l - 1));
    }
    if (!w) {
        if (sgn) {
            mg = pow2_div(31 + l, c, &r) + 1;
            p = ir_ins(f, OP_MUL, IT_I64, ir_ins(f, OP_SEXT32, IT_I64, a, 0), K64(mg));
            q = ir_ins(f, OP_SAR, IT_I64, p, K64(31 + l));
            q = ir_ins(f, OP_ADD, IT_I64, q, ir_ins(f, OP_SHR, IT_I64, p, K64(63))); /* one more when negative */
        } else {
            mg = pow2_div(32 + l, c, &r) + 1;
            q = ir_ins(f, OP_UMULH, IT_I64, ir_ins(f, OP_ZEXT32, IT_I64, a, 0), K64(mg << (32 - l)));
        }
        return ir_ins(f, OP_TRUNC, IT_I32, q, 0);
    }
    if (sgn) {
        for (s = 0; s + 2 <= l; s++) {
            mg = pow2_div(64 + s, c, &r) + 1;
            if (c - r <= (u64)2 << s)
                break; /* a reciprocal below 2^63 is exact enough */
        }
        if (s + 2 <= l) {
            p = ir_ins(f, OP_SMULH, IT_I64, a, K64(mg));
        } else {
            mg = pow2_div(63 + l, c, &r) + 1; /* at least 2^63: negative as the multiplier */
            p = ir_ins(f, OP_ADD, IT_I64, ir_ins(f, OP_SMULH, IT_I64, a, K64(mg)), a);
            s = l - 1;
        }
        if (s)
            p = ir_ins(f, OP_SAR, IT_I64, p, K64(s));
        return ir_ins(f, OP_ADD, IT_I64, p, ir_ins(f, OP_SHR, IT_I64, a, K64(63)));
    }
    for (s = 0; s < l; s++) {
        mg = pow2_div(64 + s, c, &r) + 1;
        if (c - r <= (u64)1 << s) {
            /* the reciprocal fits in 64 bits */
            q = ir_ins(f, OP_UMULH, IT_I64, a, K64(mg));
            return s ? ir_ins(f, OP_SHR, IT_I64, q, K64(s)) : q;
        }
    }
    /* it has 65: the top bit is added by hand */
    mg = pow2_div(64 + l, c, &r) + 1;
    t = ir_ins(f, OP_UMULH, IT_I64, a, K64(mg));
    p = ir_ins(f, OP_SHR, IT_I64, ir_ins(f, OP_SUB, IT_I64, a, t), K64(1));
    return ir_ins(f, OP_SHR, IT_I64, ir_ins(f, OP_ADD, IT_I64, p, t), K64(l - 1));
}

/* device code only: sums are put in the order that lets them share (MCC_NOSELK: not) */
#define DEV_SUMS ((opt_dev_fn || opt_dev_parse) && !opt_noselk && (ty == IT_I32 || ty == IT_I64))

/* v is the low 16 bits of a 32-bit x, or (high) its high 16, as a whole number: x, or 0. */
/* A bound of the 32-bit value v read as an unsigned number: v <= the result. */
static u64 umax_of(Func *f, u32 v, int depth)
{
    Ins *i = &f->ins[v];
    u64 c, m, n, full = 0xffffffffUL;
    if (i->ty != IT_I32)
        return full;
    if (ir_is_const(f, v, &c))
        return c & full;
    if (is_cmp(i->op) || (i->op >= OP_FEQ && i->op <= OP_FGE))
        return 1;
    if (i->op == OP_ZEXT8 || (i->op == OP_LOAD && i->mem == M_8U))
        return 255;
    if (i->op == OP_ZEXT16 || (i->op == OP_LOAD && i->mem == M_16U))
        return 65535;
    if (!depth)
        return full;
    switch (i->op) {
    case OP_AND:
        m = umax_of(f, i->a, depth - 1);
        n = umax_of(f, i->b, depth - 1);
        return m < n ? m : n;
    case OP_SHR:
        return ir_is_const(f, i->b, &c) && c < 32 ? umax_of(f, i->a, depth - 1) >> c : full;
    case OP_SAR: /* (of a number that is not negative) */
        m = umax_of(f, i->a, depth - 1);
        return ir_is_const(f, i->b, &c) && c < 32 && m < 0x80000000UL ? m >> c : full;
    case OP_UDIV:
        return ir_is_const(f, i->b, &c) && (c & full) ? umax_of(f, i->a, depth - 1) / (c & full) : full;
    case OP_UREM:
        return ir_is_const(f, i->b, &c) && (c & full) ? (c & full) - 1 : full;
    case OP_SHL:
        if (!ir_is_const(f, i->b, &c) || c >= 32)
            return full;
        m = umax_of(f, i->a, depth - 1) << c;
        return m < full ? m : full;
    case OP_MUL:
        m = umax_of(f, i->a, depth - 1);
        n = umax_of(f, i->b, depth - 1);
        return m < 0x10000 && n < 0x10000 ? m * n : full; /* (the product of two bounds below 2^16 fits) */
    case OP_ADD:
        m = umax_of(f, i->a, depth - 1) + umax_of(f, i->b, depth - 1);
        return m < full ? m : full;
    case OP_SELECT:
        m = umax_of(f, i->b, depth - 1);
        n = umax_of(f, i->c, depth - 1);
        return m > n ? m : n;
    default:
        return full;
    }
}

static u32 half_of(Func *f, u32 v, int high)
{
    Ins *i = &f->ins[v];
    u64 c;
    if (i->ty != IT_I32)
        return 0;
    if (i->op == OP_ZEXT16 || (i->op == OP_AND && ir_is_const(f, i->b, &c) && (c & 0xffffffffUL) == 0xffff)) {
        if (!high)
            return i->a;
        v = i->a;
        i = &f->ins[v];
    }
    if (high && i->ty == IT_I32 && i->op == OP_SHR && ir_is_const(f, i->b, &c) && c == 16)
        return i->a;
    return 0;
}

/* Returns a value equivalent to the operation, or 0 if none is known. */
/* A comparison of some value with a constant, read as a bound on the value: returns 1 for "x >= *k",
   2 for "x <= *k", 0 for neither. With below set the question is which values fail the test instead
   (so "x < 5" is the bound x >= 5). *uns tells whether the order is the unsigned one. */
static int bound_of(Func *f, u32 c, int below, u32 *x, u64 *k, int *uns)
{
    Ins *i = &f->ins[c];
    int op = i->op, w, ty;
    u64 v, top, bot;
    if (op < OP_SLT || op > OP_UGE || !ir_is_const(f, i->b, &v))
        return 0;
    ty = f->ins[i->a].ty;
    w = ty == IT_I64;
    if (ty != IT_I32 && !w)
        return 0;
    *uns = op >= OP_ULT;
    if (*uns) {
        op -= OP_ULT - OP_SLT;
        if (!w)
            v = (u32)v;
        bot = 0;
        top = w ? ~(u64)0 : 0xffffffffUL;
    } else {
        bot = w ? (u64)1 << 63 : (u64)(i64)(-0x7fffffffL - 1);
        top = w ? ~(u64)0 >> 1 : 0x7fffffffUL;
    }
    if (below) /* the test that fails where this one holds */
        op = op == OP_SLT ? OP_SGE : op == OP_SLE ? OP_SGT : op == OP_SGT ? OP_SLE : OP_SLT;
    if ((op == OP_SGT && v == top) || (op == OP_SLT && v == bot))
        return 0;
    *x = i->a;
    *k = op == OP_SGT ? v + 1 : op == OP_SLT ? v - 1 : v;
    return op == OP_SGT || op == OP_SGE ? 1 : 2;
}

static u32 fold(Func *f, int op, int ty, u32 a, u32 b)
{
    Ins *ia = &f->ins[a], *ib = &f->ins[b];
    u64 x, y, r;
    double dx, dy;
    int ca, cb;

    if (op == OP_ISCONST)
        return ia->op == OP_CONST || ia->op == OP_FCONST ? ir_const(f, IT_I32, 1) : 0;
    if (IT_ISVEC(ty)) {
        /* lane by lane, the lane kind in Ins.mem (which is not known here): only what holds for every kind.
           The one vector constant is zero; a shift count is a scalar. */
        if (op < OP_ADD || op > OP_SAR)
            return 0;
        ca = ia->op == OP_CONST;
        cb = ib->op == OP_CONST && ir_is_const(f, b, &y) && y == 0;
        if (a == b && (op == OP_SUB || op == OP_XOR))
            return ir_const(f, ty, 0);
        if (a == b && (op == OP_AND || op == OP_OR))
            return a;
        if (cb && (op == OP_ADD || op == OP_SUB || op == OP_OR || op == OP_XOR || op == OP_SHL || op == OP_SHR || op == OP_SAR))
            return a;
        if (cb && (op == OP_AND || op == OP_MUL) && ib->ty == ty)
            return b;
        if (ca && ir_is_const(f, a, &x) && x == 0 && (op == OP_AND || op == OP_MUL || op == OP_SHL || op == OP_SHR || op == OP_SAR))
            return a;
        return 0;
    }
    if (op >= OP_ADD && op <= OP_SAR) {
        ca = ir_is_const(f, a, &x);
        cb = ir_is_const(f, b, &y);
        if (ca && cb) {
            if (fold_int(op, ty == IT_I64, x, y, &r))
                return ir_const(f, ty, r);
            return 0;
        }
        if (ca && !cb && (op == OP_ADD || op == OP_MUL || op == OP_AND || op == OP_OR || op == OP_XOR))
            return ir_ins(f, op, ty, b, a);
        if (op == OP_SUB && ca && !(ty == IT_I32 ? (u32)x : x))
            return ir_ins(f, OP_NEG, ty, b, 0);
        if (a == b && !ca) {
            if (op == OP_SUB || op == OP_XOR)
                return ir_const(f, ty, 0);
            if (op == OP_AND || op == OP_OR)
                return a;
        }
        if (op == OP_AND && ty == IT_I32 && !ca && !cb) {
            /* x >= 0 && x < c, c a constant that is not negative: one unsigned comparison */
            int k;
            for (k = 0; k < 2; k++) {
                Ins *lo = k ? ib : ia, *hi = k ? ia : ib;
                u64 c0, c1;
                int w64 = f->ins[lo->a].ty == IT_I64;
                if (!((lo->op == OP_SGE && ir_is_const(f, lo->b, &c0) && c0 == 0) ||
                      (lo->op == OP_SGT && ir_is_const(f, lo->b, &c0) && c0 == (w64 ? ~(u64)0 : 0xffffffffUL))))
                    continue;
                if ((hi->op != OP_SLT && hi->op != OP_SLE) || hi->a != lo->a || !ir_is_const(f, hi->b, &c1) ||
                    (c1 >> (w64 ? 63 : 31)) != 0)
                    continue;
                return ir_ins(f, hi->op == OP_SLT ? OP_ULT : OP_ULE, IT_I32, hi->a, hi->b);
            }
        }
        if (op == OP_ADD && !ca && !cb && DEV_SUMS) {
            /* device code: (x + k) + y is (x + y) + k: sums that differ in a constant share the rest, and the
               constant goes into an address */
            int k;
            if (a > b && !opt_nodist)
                return ir_ins(f, op, ty, b, a); /* (one order, so that equal sums are seen to be equal) */
            for (k = 0; k < 2; k++) {
                Ins *in = k ? ib : ia;
                u32 o = k ? a : b, p;
                if (in->op == OP_ADD && in->ty == ty && ir_is_const(f, in->b, &r) && !ir_is_const(f, in->a, &x)) {
                    p = in->a;
                    return ir_ins(f, OP_ADD, ty, ir_ins(f, OP_ADD, ty, p < o ? p : o, p < o ? o : p), ir_const(f, ty, r));
                }
            }
        }
        if ((op == OP_AND || op == OP_OR) && ty == IT_I32 && !ca && !cb && !opt_dev_fn && !opt_dev_parse) {
            /* lo <= x && x <= hi is x - lo <= hi - lo, unsigned: one comparison. Likewise
               x < lo || x > hi, the values the first leaves out. */
            u32 xa, xb;
            u64 ka, kb;
            int ua, ub, below = op == OP_OR;
            int ba = bound_of(f, a, below, &xa, &ka, &ua), bb = bound_of(f, b, below, &xb, &kb, &ub);
            if (ba && bb && ba != bb && xa == xb && ua == ub) {
                u64 lo = ba == 1 ? ka : kb, hi = ba == 1 ? kb : ka;
                int xt = f->ins[xa].ty;
                if (ua ? lo <= hi : (i64)lo <= (i64)hi)
                    return ir_ins(f, below ? OP_UGT : OP_ULE, IT_I32,
                                  ir_ins(f, OP_SUB, xt, xa, ir_const(f, xt, lo)), ir_const(f, xt, hi - lo));
            }
        }
        if (op == OP_OR && ty == IT_I32 && !ca && !cb) {
            /* the two halves of a number, put together again */
            int k;
            for (k = 0; k < 2; k++) {
                Ins *h = &f->ins[k ? a : b];
                u32 l = k ? b : a, w;
                if (h->op == OP_SHL && ir_is_const(f, h->b, &r) && r == 16 && (w = half_of(f, l, 0)) != 0 &&
                    half_of(f, h->a, 1) == w)
                    return w;
            }
        }
        if (cb) {
            if (ty == IT_I32)
                y = (u64)(i64)(i32)y;
            switch (op) {
            case OP_ADD:
            case OP_SUB:
                if (y == 0)
                    return a;
                if (op == OP_SUB)
                    y = 0 - y;
                if ((ia->op == OP_ADDR || ia->op == OP_SLOT) && ty == IT_I64) {
                    i64 off = (i64)(i32)ia->b + (i64)y;
                    if (off >= -0x40000000L && off < 0x40000000L)
                        return ia->op == OP_ADDR ? ir_addr(f, ia->a, off) : ir_slot_addr(f, ia->a, off);
                }
                if (ia->op == OP_ADD && ir_is_const(f, ia->b, &x))
                {
                    u32 aa = ia->a, k2 = ir_const(f, ty, x + y); /* (ir_const may move f->ins: ia with it) */
                    return ir_ins(f, OP_ADD, ty, aa, k2);
                }
                if (op == OP_SUB)
                    return ir_ins(f, OP_ADD, ty, a, ir_const(f, ty, y));
                break;
            case OP_OR:
            case OP_XOR:
            case OP_SHL:
            case OP_SHR:
            case OP_SAR:
                if (y == 0)
                    return a;
                if (op == OP_SHL && DEV_SUMS && !opt_nodist && ia->op == OP_ADD && ir_is_const(f, ia->b, &x) && y < 32)
                {
                    u32 aa = ia->a, k2 = ir_const(f, ty, x << y);
                    return ir_ins(f, OP_ADD, ty, ir_ins(f, OP_SHL, ty, aa, b), k2);
                }
                break;
            case OP_MUL:
                if (y == 1)
                    return a;
                if (y == 0)
                    return b;
                if (DEV_SUMS && !opt_nodist && ia->op == OP_ADD && ir_is_const(f, ia->b, &x)) /* (x + k) c is x c + k c */
                {
                    u32 aa = ia->a, k2 = ir_const(f, ty, x * y);
                    return ir_ins(f, OP_ADD, ty, ir_ins(f, OP_MUL, ty, aa, b), k2);
                }
                if ((y & (y - 1)) == 0 && (ty == IT_I64 || y < 0x80000000UL))
                    return ir_ins(f, OP_SHL, ty, a, ir_const(f, ty, log2u64(y)));
                {
                    /* (2^n + 1) << k and (2^n - 1) << k: a shift and an add or subtract */
                    u64 m = ty == IT_I32 ? (u64)(u32)y : y;
                    u32 k = 0, t = 0;
                    while (!(m & 1)) {
                        m >>= 1;
                        k++;
                    }
                    if (m > 2 && m < ((u64)1 << (ty == IT_I32 ? 30 : 62))) {
                        if (((m - 1) & (m - 2)) == 0)
                            t = ir_ins(f, OP_ADD, ty, a, ir_ins(f, OP_SHL, ty, a, ir_const(f, ty, log2u64(m - 1))));
                        else if (((m + 1) & m) == 0)
                            t = ir_ins(f, OP_SUB, ty, ir_ins(f, OP_SHL, ty, a, ir_const(f, ty, log2u64(m + 1))), a);
                        if (t)
                            return k ? ir_ins(f, OP_SHL, ty, t, ir_const(f, ty, k)) : t;
                    }
                }
                break;
            case OP_SREM:
            case OP_SDIV:
                /* signed division by 2^k rounds toward zero: bias negative values */
                if (y > 1 && (y & (y - 1)) == 0 && y < ((u64)1 << (ty == IT_I32 ? 30 : 62))) {
                    u32 k = log2u64(y), bits = ty == IT_I32 ? 32 : 64, t, q;
                    t = k == 1 ? a : ir_ins(f, OP_SAR, ty, a, ir_const(f, ty, bits - 1));
                    t = ir_ins(f, OP_SHR, ty, t, ir_const(f, ty, bits - k));
                    t = ir_ins(f, OP_ADD, ty, a, t);
                    if (op == OP_SDIV)
                        return ir_ins(f, OP_SAR, ty, t, ir_const(f, ty, k));
                    q = ir_ins(f, OP_AND, ty, t, ir_const(f, ty, 0 - y));
                    return ir_ins(f, OP_SUB, ty, a, q);
                }
                if (op == OP_SREM)
                    goto reciprocal;
                /* fall through */
            case OP_UDIV:
                if (y == 1)
                    return a;
                if (op == OP_UDIV && y && (y & (y - 1)) == 0 && (ty == IT_I64 || y < 0x80000000UL))
                    return ir_ins(f, OP_SHR, ty, a, ir_const(f, ty, log2u64(y)));
                goto reciprocal;
            case OP_UREM:
                if (y && (y & (y - 1)) == 0 && (ty == IT_I64 || y < 0x80000000UL))
                    return ir_ins(f, OP_AND, ty, a, ir_const(f, ty, y - 1));
            reciprocal: {
                u32 q = div_const(f, op == OP_SDIV || op == OP_SREM, ty, a, y);
                if (!q || op == OP_SDIV || op == OP_UDIV)
                    return q;
                return ir_ins(f, OP_SUB, ty, a, ir_ins(f, OP_MUL, ty, q, ir_const(f, ty, y)));
            }
            case OP_AND:
                if (y == 0)
                    return b;
                if (y == ~(u64)0)
                    return a;
                if ((ia->op == OP_ZEXT8 && (y & 0xff) == 0xff) || (ia->op == OP_ZEXT16 && (y & 0xffff) == 0xffff) ||
                    ((y & 1) && ty == IT_I32 && ir_is_bool(f, a, 8)) ||
                    (ia->op == OP_AND && ir_is_const(f, ia->b, &x) && (x & y) == x))
                    return a; /* the bits taken away are zeros already */
                if (ty == IT_I32 && ia->op == OP_SHR && ir_is_const(f, ia->b, &x) && x > 0 && x < 32 &&
                    (y & (0xffffffffUL >> x)) == 0xffffffffUL >> x)
                    return a; /* the bits taken away are zeros already */
                break;
            }
        }
        return 0;
    }
    if (is_cmp(op)) {
        int w64 = ia->ty == IT_I64;
        ca = ir_is_const(f, a, &x);
        cb = ir_is_const(f, b, &y);
        if (ca && cb && fold_int(op, w64, x, y, &r))
            return ir_const(f, IT_I32, r);
        if (ca && !cb) {
            /* constant on the right */
            int sw = op;
            switch (op) {
            case OP_SLT: sw = OP_SGT; break;
            case OP_SLE: sw = OP_SGE; break;
            case OP_SGT: sw = OP_SLT; break;
            case OP_SGE: sw = OP_SLE; break;
            case OP_ULT: sw = OP_UGT; break;
            case OP_ULE: sw = OP_UGE; break;
            case OP_UGT: sw = OP_ULT; break;
            case OP_UGE: sw = OP_ULE; break;
            }
            return ir_ins(f, sw, ty, b, a);
        }
        if (cb && y == 0) {
            /* unsigned comparisons with zero */
            switch (op) {
            case OP_ULE: return ir_ins(f, OP_EQ, ty, a, b);
            case OP_UGT: return ir_ins(f, OP_NE, ty, a, b);
            case OP_ULT: return ir_const(f, IT_I32, 0);
            case OP_UGE: return ir_const(f, IT_I32, 1);
            }
        }
        /* (cmp) != 0 is cmp; (cmp) == 0 is the inverse comparison */
        if (cb && y == 0 && (op == OP_EQ || op == OP_NE) && ia->ty == IT_I32) {
            if (is_cmp(ia->op) || ia->op == OP_FEQ || ia->op == OP_FNE) {
                if (op == OP_NE)
                    return a;
                return ir_ins(f, invert_cmp(ia->op), IT_I32, ia->a, ia->b);
            }
            if (ia->op >= OP_FLT && ia->op <= OP_FGE && op == OP_NE)
                return a;
            if (op == OP_NE && ir_is_bool(f, a, 8))
                return a;
        }
        return 0;
    }
    if (op == OP_BITCAST) {
        /* there and back */
        if (f->ins[a].op == OP_BITCAST && f->ins[f->ins[a].a].ty == ty)
            return f->ins[a].a;
        return 0;
    }
    switch (op) {
    case OP_CLZ:
    case OP_CTZ:
    case OP_BSWAP:
    case OP_POPCNT:
        if (ir_is_const(f, a, &x)) {
            int bits = ty == IT_I64 ? 64 : 32, k;
            if (bits == 32)
                x = (u64)(u32)x;
            r = 0;
            if (op == OP_POPCNT) {
                for (; x; x &= x - 1)
                    r++;
            } else if (op == OP_BSWAP) {
                for (k = 0; k < bits; k += 8)
                    r |= ((x >> k) & 0xff) << (bits - 8 - k);
            } else if (!x) {
                return 0;
            } else if (op == OP_CLZ) {
                while (!(x >> (bits - 1 - r)))
                    r++;
            } else {
                while (!((x >> r) & 1))
                    r++;
            }
            return ir_const(f, ty, r);
        }
        return 0;
    case OP_NEG:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, 0 - x);
        return 0;
    case OP_NOT:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, ~x);
        return 0;
    case OP_SEXT8:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, (u64)(i64)(i8)(u8)x);
        if ((ia->op == OP_LOAD && ia->mem == M_8S) || ia->op == OP_SEXT8)
            return a;
        return 0;
    case OP_SEXT16:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, (u64)(i64)(i16)(u16)x);
        if ((ia->op == OP_LOAD && (ia->mem == M_16S || ia->mem == M_8S || ia->mem == M_8U)) ||
            ia->op == OP_SEXT16 || ia->op == OP_SEXT8 || ia->op == OP_ZEXT8)
            return a;
        return 0;
    case OP_ZEXT8:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, x & 0xff);
        if ((ia->op == OP_LOAD && ia->mem == M_8U) || ia->op == OP_ZEXT8 || is_cmp(ia->op))
            return a;
        return 0;
    case OP_ZEXT16:
        if (ir_is_const(f, a, &x))
            return ir_const(f, ty, x & 0xffff);
        if ((ia->op == OP_LOAD && (ia->mem == M_16U || ia->mem == M_8U)) || ia->op == OP_ZEXT16 ||
            ia->op == OP_ZEXT8)
            return a;
        return 0;
    case OP_SEXT32:
        if (ir_is_const(f, a, &x))
            return ir_const(f, IT_I64, (u64)(i64)(i32)(u32)x);
        if ((opt_dev_fn || opt_dev_parse) && !opt_nodist && ia->op == OP_ADD && ir_is_const(f, ia->b, &x)) {
            /* device code: a sum of a number known to be small and a constant does not wrap, and the
               constant goes outside (and from there into an address) */
            u64 m = umax_of(f, ia->a, 10), k = x & 0xffffffffUL;
            if (m < 0x80000000UL && ((k & 0x80000000UL) || m + k < 0x80000000UL))
            {
                u32 aa = ia->a, k2 = ir_const(f, IT_I64, (u64)(i64)(i32)(u32)k);
                return ir_ins(f, OP_ADD, IT_I64, ir_ins(f, OP_SEXT32, IT_I64, aa, 0), k2);
            }
        }
        /* a value that cannot be negative is zero-extended, which is often free */
        if (ia->op == OP_ZEXT8 || ia->op == OP_ZEXT16 || is_cmp(ia->op) ||
            (ia->op == OP_LOAD && (ia->mem == M_8U || ia->mem == M_16U)) ||
            (ia->op == OP_AND && ir_is_const(f, ia->b, &x) && !(x & 0x80000000UL)) ||
            (ia->op == OP_SHR && ir_is_const(f, ia->b, &x) && (x & 31) != 0) ||
            (ia->op == OP_UREM && ir_is_const(f, ia->b, &x) && !(x & 0x80000000UL)))
            return ir_ins(f, OP_ZEXT32, IT_I64, a, 0);
        return 0;
    case OP_ZEXT32:
        if (ir_is_const(f, a, &x))
            return ir_const(f, IT_I64, x & 0xffffffffUL);
        return 0;
    case OP_TRUNC:
        if (ir_is_const(f, a, &x))
            return ir_const(f, IT_I32, x);
        if (ia->op == OP_SEXT32 || ia->op == OP_ZEXT32)
            return ia->a;
        return 0;
    case OP_SITOF:
        if (ir_is_const(f, a, &x))
            return ir_fconst(f, ty, ty == IT_F32 ? (double)(float)(i64)x : (double)(i64)x);
        return 0;
    case OP_UITOF:
        if (ir_is_const(f, a, &x)) {
            if (ia->ty == IT_I32)
                x &= 0xffffffffUL;
            return ir_fconst(f, ty, ty == IT_F32 ? (double)(float)x : (double)x);
        }
        return 0;
    case OP_FTOSI:
        if (ir_is_fconst(f, a, &dx) && dx > -9.0e18 && dx < 9.0e18)
            return ir_const(f, ty, (u64)(i64)dx);
        return 0;
    case OP_FTOUI:
        if (ir_is_fconst(f, a, &dx) && dx >= 0 && dx < 1.8e19)
            return ir_const(f, ty, (u64)dx);
        return 0;
    case OP_FEXT:
    case OP_FTRUNC:
        if (ir_is_fconst(f, a, &dx))
            return ir_fconst(f, ty, dx);
        return 0;
    case OP_FNEG:
        if (ir_is_fconst(f, a, &dx))
            return ir_fconst(f, ty, -dx);
        return 0;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
        if (ir_is_fconst(f, a, &dx) && ir_is_fconst(f, b, &dy)) {
            double d;
            if (ty == IT_F32) {
                float fx = (float)dx, fy = (float)dy;
                d = op == OP_FADD ? fx + fy : op == OP_FSUB ? fx - fy : op == OP_FMUL ? fx * fy : fx / fy;
            } else {
                d = op == OP_FADD ? dx + dy : op == OP_FSUB ? dx - dy : op == OP_FMUL ? dx * dy : dx / dy;
            }
            if (d != d && dx == dx && dy == dy) {
                /* a NaN made here (0/0, inf - inf): the same one whatever machine the compiler runs on
                   (x86 would make it negative) */
                u64 nan = (u64)0x7ff8 << 48;
                memcpy(&d, &nan, sizeof d);
            }
            return ir_fconst(f, ty, d);
        }
        if (!opt_dev_fn && !opt_dev_parse && (ty == IT_F32 || ty == IT_F64)) {
            /* identities: exact for every x (NaNs, infinities, zeros of either sign) unless said otherwise */
            int ca = ir_is_fconst(f, a, &dx), cb = ir_is_fconst(f, b, &dy);
            int nb = cb && dy == 0 && 1 / dy < 0, pb = cb && dy == 0 && 1 / dy > 0;
            if ((op == OP_FADD && (nb || (pb && opt_no_signed_zeros))) || (op == OP_FSUB && (pb || (nb && opt_no_signed_zeros))))
                return a; /* x + -0, x - +0 (x + +0, x - -0: -fno-signed-zeros) */
            if (op == OP_FADD && ca && dx == 0 && (1 / dx < 0 || opt_no_signed_zeros))
                return b;
            if (op == OP_FSUB && ca && dx == 0 && (1 / dx < 0 || opt_no_signed_zeros))
                return ir_ins(f, OP_FNEG, ty, b, 0); /* -0 - x (0 - x: -fno-signed-zeros) */
            if ((op == OP_FMUL || op == OP_FDIV) && cb && (dy == 1.0 || dy == -1.0))
                return dy == 1.0 ? a : ir_ins(f, OP_FNEG, ty, a, 0);
            if (op == OP_FMUL && ca && (dx == 1.0 || dx == -1.0))
                return dx == 1.0 ? b : ir_ins(f, OP_FNEG, ty, b, 0);
            if (op == OP_FDIV && cb && dy == dy && dy != 0 && dy - dy == 0) {
                /* x / c as x * (1 / c): exactly the same when c and 1 / c are both normal powers of two of the
                   type; any c under -freciprocal-math */
                double r = 1.0 / dy;
                u64 bc, br;
                int exact;
                if (ty == IT_F32) {
                    float fc = (float)dy, fr = (float)r;
                    u32 xc, xr;
                    memcpy(&xc, &fc, 4);
                    memcpy(&xr, &fr, 4);
                    exact = !(xc & 0x7fffff) && !(xr & 0x7fffff) && (xc >> 23 & 0xff) && (xc >> 23 & 0xff) != 0xff &&
                            (xr >> 23 & 0xff) && (xr >> 23 & 0xff) != 0xff;
                    r = fr;
                } else {
                    memcpy(&bc, &dy, 8);
                    memcpy(&br, &r, 8);
                    exact = !(bc & (((u64)1 << 52) - 1)) && !(br & (((u64)1 << 52) - 1)) && (bc >> 52 & 0x7ff) && (bc >> 52 & 0x7ff) != 0x7ff &&
                            (br >> 52 & 0x7ff) && (br >> 52 & 0x7ff) != 0x7ff;
                }
                if (exact || (opt_reciprocal_math && r - r == 0 && r != 0))
                    return ir_ins(f, OP_FMUL, ty, a, ir_fconst(f, ty, r));
            }
        }
        if (op == OP_FMUL && !opt_dev_fn && !opt_dev_parse) {
            /* twice x is x + x, exactly, and an addition is quicker */
            if (ir_is_fconst(f, a, &dx) && dx == 2.0)
                return ir_ins(f, OP_FADD, ty, b, b);
            if (ir_is_fconst(f, b, &dy) && dy == 2.0)
                return ir_ins(f, OP_FADD, ty, a, a);
        }
        return 0;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FLE:
    case OP_FGT:
    case OP_FGE:
        if (ir_is_fconst(f, a, &dx) && ir_is_fconst(f, b, &dy)) {
            int v = op == OP_FEQ ? dx == dy : op == OP_FNE ? dx != dy : op == OP_FLT ? dx < dy :
                    op == OP_FLE ? dx <= dy : op == OP_FGT ? dx > dy : dx >= dy;
            return ir_const(f, IT_I32, (u64)v);
        }
        return 0;
    }
    (void)ib;
    return 0;
}

u32 ir_fold_at(Func *f, u32 v)
{
    Ins *i = &f->ins[v];
    u32 n0 = f->nins, saved = f->cur, r, k;
    f->cur = 0;
    r = fold(f, i->op, i->ty, i->a, i->b);
    f->cur = saved;
    /* instructions created by the folder go in front of v, in creation order */
    for (k = n0; k < f->nins; k++) {
        int op = f->ins[k].op;
        if (op != OP_CONST && op != OP_FCONST && op != OP_ADDR && op != OP_SLOT && !f->ins[k].blk)
            ir_insert_before(f, v, k);
    }
    return r == v ? 0 : r;
}

#define RESOLVE(x) \
    do { \
        while (ins[x].op == OP_COPY) \
            (x) = ins[x].a; \
    } while (0)

void ir_resolve_all(Func *f)
{
    u32 b, v, k, n;
    Ins *ins = f->ins;
    u32 *xa = f->xargs;
    if (!f->copies)
        return;
    f->copies = 0;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        for (v = bb->phis; v; v = ins[v].next) {
            u32 *p = xa + ins[v].a;
            n = ins[v].b;
            for (k = 0; k < n; k++)
                RESOLVE(p[k]);
        }
        for (v = bb->first; v; v = ins[v].next) {
            Ins *i = &ins[v];
            n = ir_opcount_tab[i->op];
            if (n == 255) { /* call: callee and arguments (an intrinsic's c is a number) */
                u32 *p = xa + i->a;
                if (i->op == OP_CALL)
                    RESOLVE(i->c);
                for (k = 0; k < i->b; k++)
                    RESOLVE(p[k]);
                continue;
            }
            if (n >= 1)
                RESOLVE(i->a);
            if (n >= 2)
                RESOLVE(i->b);
            if (n == 3)
                RESOLVE(i->c);
        }
        if (bb->arg)
            RESOLVE(bb->arg);
    }
}

u32 ir_ins(Func *f, int op, int ty, u32 a, u32 b)
{
    u32 v = fold(f, op, ty, a, b);
    if (v)
        return v;
    v = ir_raw(f, op, ty, a, b);
    if (f->cur)
        ir_append(f, f->cur, v);
    return v;
}

u32 ir_load(Func *f, int ty, int mem, u32 addr)
{
    u32 v = ir_raw(f, OP_LOAD, ty, addr, 0);
    f->ins[v].mem = (u8)mem;
    if (f->cur)
        ir_append(f, f->cur, v);
    return v;
}

void ir_store(Func *f, int mem, u32 addr, u32 val)
{
    u32 v;
    /* a narrow store truncates by itself */
    for (;;) {
        int op = f->ins[val].op;
        if ((mem <= M_8U && (op == OP_SEXT8 || op == OP_ZEXT8 || op == OP_SEXT16 || op == OP_ZEXT16)) ||
            (mem <= M_16U && (op == OP_SEXT16 || op == OP_ZEXT16)))
            val = f->ins[val].a;
        else
            break;
    }
    v = ir_raw(f, OP_STORE, IT_VOID, addr, val);
    f->ins[v].mem = (u8)mem;
    if (f->cur)
        ir_append(f, f->cur, v);
}

static u32 xalloc(Func *f, u32 n)
{
    u32 at = f->nxargs;
    if (f->nxargs + n > f->capxargs) {
        f->capxargs = f->capxargs ? f->capxargs * 2 : 256;
        while (f->nxargs + n > f->capxargs)
            f->capxargs *= 2;
        f->xargs = (u32 *)xrealloc(f->xargs, f->capxargs * sizeof(u32));
    }
    f->nxargs += n;
    return at;
}

u32 ir_call(Func *f, int ty, u32 callee, const u32 *args, u32 nargs, u32 nfixed, int flags)
{
    u32 at = xalloc(f, nargs + 1), v, k;
    f->xargs[at] = nfixed;
    for (k = 0; k < nargs; k++)
        f->xargs[at + 1 + k] = args[k];
    v = ir_raw(f, OP_CALL, ty, at + 1, nargs);
    f->ins[v].c = callee;
    f->ins[v].flags = (u8)flags;
    if (f->cur)
        ir_append(f, f->cur, v);
    return v;
}

/* An x86-64 intrinsic (OP_XI): entry id of x86i_tab applied to args. */
u32 ir_xi(Func *f, int ty, u32 id, const u32 *args, u32 nargs)
{
    u32 at = xalloc(f, nargs ? nargs : 1), v, k;
    for (k = 0; k < nargs; k++)
        f->xargs[at + k] = args[k];
    v = ir_raw(f, OP_XI, ty, at, nargs);
    f->ins[v].c = id;
    if (f->cur)
        ir_append(f, f->cur, v);
    return v;
}

u32 ir_phi(Func *f, u32 blk, int ty)
{
    Block *b = &f->blk[blk];
    u32 at = xalloc(f, b->npreds), v, k;
    for (k = 0; k < b->npreds; k++)
        f->xargs[at + k] = 0;
    v = ir_raw(f, OP_PHI, ty, at, b->npreds);
    f->ins[v].blk = blk;
    f->ins[v].next = b->phis;
    b->phis = v;
    return v;
}

void ir_phi_add_arg(Func *f, u32 phi, u32 val)
{
    u32 n = f->ins[phi].b, at = xalloc(f, n + 1);
    memcpy(&f->xargs[at], &f->xargs[f->ins[phi].a], n * sizeof(u32));
    f->xargs[at + n] = val;
    f->ins[phi].a = at;
    f->ins[phi].b = n + 1;
}

/* ---- blocks ---- */

u32 ir_new_block(Func *f)
{
    if (f->nblk == f->capblk) {
        f->capblk *= 2;
        f->blk = (Block *)xrealloc(f->blk, f->capblk * sizeof(Block));
    }
    memset(&f->blk[f->nblk], 0, sizeof(Block));
    return f->nblk++;
}

void ir_add_pred(Func *f, u32 blk, u32 pred)
{
    Block *b = &f->blk[blk];
    if (b->npreds == b->cappreds) {
        u32 cap = b->cappreds ? b->cappreds * 2 : 4;
        u32 *p = (u32 *)arena_alloc_raw(&f->arena, cap * sizeof(u32));
        if (b->npreds)
            memcpy(p, b->preds, b->npreds * sizeof(u32));
        b->preds = p;
        b->cappreds = cap;
    }
    b->preds[b->npreds++] = pred;
}

void ir_jmp(Func *f, u32 target)
{
    Block *b;
    if (!f->cur)
        return;
    b = &f->blk[f->cur];
    b->term = TERM_JMP;
    b->succ[0] = target;
    ir_add_pred(f, target, f->cur);
    f->cur = 0;
}

void ir_br(Func *f, u32 cond, u32 t, u32 e)
{
    Block *b;
    u64 c;
    if (!f->cur)
        return;
    if (ir_is_const(f, cond, &c)) {
        ir_jmp(f, c ? t : e);
        return;
    }
    if (t == e) {
        ir_jmp(f, t);
        return;
    }
    b = &f->blk[f->cur];
    b->term = TERM_BR;
    b->arg = cond;
    b->succ[0] = t;
    b->succ[1] = e;
    ir_add_pred(f, t, f->cur);
    ir_add_pred(f, e, f->cur);
    f->cur = 0;
}

void ir_ret(Func *f, u32 val)
{
    Block *b;
    if (!f->cur)
        return;
    b = &f->blk[f->cur];
    b->term = TERM_RET;
    b->arg = val;
    f->cur = 0;
}

/* ---- printing ---- */

static const char *const op_names[OP_COUNT] = {
    "nop", "const", "fconst", "addr", "slot", "param", "copy", "phi",
    "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor", "shl", "shr", "sar",
    "neg", "not", "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
    "fadd", "fsub", "fmul", "fdiv", "fneg", "feq", "fne", "flt", "fle", "fgt", "fge",
    "sext8", "sext16", "zext8", "zext16", "sext32", "zext32", "trunc",
    "sitof", "uitof", "ftosi", "ftoui", "fext", "ftrunc", "umulh", "smulh", "isconst", "clz", "ctz", "bswap", "popcnt",
    "bitcast", "tls", "asmout",
    "load", "store", "call", "memcpy", "memzero", "vastart", "select", "frameaddr", "pin", "byval",
    "vload", "vstore", "vsplat", "vsum", "vsmax", "vsmin", "vumax", "vumin", "viota", "vmath", "xi"
};
static const char *const ty_names[] = { "void", "i32", "i64", "f32", "f64", "v128", "v256" };
static const char *const mem_names[] = { "s8", "u8", "s16", "u16", "m32", "m64", "f32", "f64", "v128", "v256" };

static void dump_val(Func *f, u32 v)
{
    Ins *i;
    u64 c;
    double d;
    v = ir_resolve(f, v);
    i = &f->ins[v];
    if (ir_is_const(f, v, &c))
        printf("%ld", (long)c);
    else if (ir_is_fconst(f, v, &d))
        printf("%g%s", d, i->ty == IT_F32 ? "f" : "d");
    else if (i->op == OP_ADDR)
        printf("@%u%+d", i->a, (int)i->b);
    else if (i->op == OP_SLOT)
        printf("$%u%+d", i->a, (int)i->b);
    else
        printf("v%u", v);
}

void ir_dump(Func *f)
{
    u32 b, v, k;
    printf("func %s\n", f->name ? f->name : "?");
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        if (b != 1 && bb->npreds == 0 && !bb->first && bb->term == TERM_NONE)
            continue;
        printf("b%u:", b);
        if (bb->npreds) {
            printf("  ;");
            for (k = 0; k < bb->npreds; k++)
                printf(" b%u", bb->preds[k]);
        }
        printf("\n");
        for (v = bb->phis; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            printf("  v%u = phi %s", v, ty_names[i->ty]);
            for (k = 0; k < i->b; k++) {
                printf(k ? ", " : " ");
                dump_val(f, f->xargs[i->a + k]);
            }
            printf("\n");
        }
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            printf("  ");
            if (i->ty != IT_VOID)
                printf("v%u = ", v);
            printf("%s", op_names[i->op]);
            if (i->ty != IT_VOID)
                printf(" %s", ty_names[i->ty]);
            switch (i->op) {
            case OP_PARAM:
                printf(" %u", i->a);
                break;
            case OP_LOAD:
                printf(" %s ", mem_names[i->mem]);
                dump_val(f, i->a);
                break;
            case OP_STORE:
                printf(" %s ", mem_names[i->mem]);
                dump_val(f, i->a);
                printf(", ");
                dump_val(f, i->b);
                break;
            case OP_CALL:
                printf(" ");
                dump_val(f, i->c);
                printf("(");
                for (k = 0; k < i->b; k++) {
                    if (k)
                        printf(", ");
                    dump_val(f, f->xargs[i->a + k]);
                }
                printf(")");
                break;
            case OP_MEMCPY:
                printf(" ");
                dump_val(f, i->a);
                printf(", ");
                dump_val(f, i->b);
                printf(", %u", i->c);
                break;
            case OP_MEMZERO:
                printf(" ");
                dump_val(f, i->a);
                printf(", %u", i->c);
                break;
            case OP_VASTART:
                break;
            case OP_XI:
                printf(" #%u(", i->c);
                for (k = 0; k < i->b; k++) {
                    if (k)
                        printf(", ");
                    dump_val(f, f->xargs[i->a + k]);
                }
                printf(")");
                break;
            case OP_SELECT:
                printf(" ");
                dump_val(f, i->a);
                printf(", ");
                dump_val(f, i->b);
                printf(", ");
                dump_val(f, i->c);
                break;
            default:
                printf(" ");
                dump_val(f, i->a);
                if (i->b) {
                    printf(", ");
                    dump_val(f, i->b);
                }
                break;
            }
            printf("\n");
        }
        switch (bb->term) {
        case TERM_JMP:
            printf("  jmp b%u\n", bb->succ[0]);
            break;
        case TERM_BR:
            printf("  br ");
            dump_val(f, bb->arg);
            printf(", b%u, b%u\n", bb->succ[0], bb->succ[1]);
            break;
        case TERM_RET:
            printf("  ret");
            if (bb->arg) {
                printf(" ");
                dump_val(f, bb->arg);
            }
            printf("\n");
            break;
        default:
            printf("  <no terminator>\n");
            break;
        }
    }
}
