/* The interpreter: one thread of a block runs until it leaves the kernel or has to wait for others (a barrier,
   a warp-synchronous instruction), and a scheduler takes the threads of a block in turn. Nothing here writes
   a global while blocks run: a host thread owns its Blk. */
#include "ptx.h"
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>

#define NOSLOT 0x7fffffff
#define STACK_BYTES (256UL * 1024)

static const char *const fxnames[] = {"x", "y", "z"};

/* A thread cannot go on: record why, once. */
void mc_fault(Blk *b, Thr *t, const char *fmt, const char *arg, u64 val)
{
    char why[520];
    McFunc *e = b->L->f;
    u32 line = 0;
    (void)fxnames;
    if (b->failed)
        return;
    b->failed = strncmp(fmt, "call of the undefined function", 30) == 0 ? 2 : 1; /* (2: a build error, mc_launch stops the program) */
    if (arg && strlen(arg) > 300)
        arg = "(long name)";
    sprintf(why, fmt, arg ? arg : "", val);
    if (t && t->f && t->f->code && t->ip >= t->f->code && t->ip < t->f->code + t->f->ncode)
        line = t->f->lines[t->ip - t->f->code];
    if (t)
        sprintf(b->msg, "mcuda: kernel %.80s, block (%u,%u,%u), thread (%u,%u,%u): %s at PTX line %u of %.60s%s%.60s", e->name,
                b->cta[0], b->cta[1], b->cta[2], t->xyz[0], t->xyz[1], t->xyz[2], why, line, t->f->mod->name,
                t->f != e ? ", in " : "", t->f != e ? t->f->name : "");
    else
        sprintf(b->msg, "mcuda: kernel %.80s, block (%u,%u,%u): %s", e->name, b->cta[0], b->cta[1], b->cta[2], why);
}

static u32 popc32(u32 x)
{
    x = x - (x >> 1 & 0x55555555U);
    x = (x & 0x33333333U) + (x >> 2 & 0x33333333U);
    x = (x + (x >> 4)) & 0x0f0f0f0fU;
    return x * 0x01010101U >> 24;
}

static u32 clz32(u32 x)
{
    u32 n = 0;
    if (!x)
        return 32;
    if (!(x & 0xffff0000U))
        n += 16, x <<= 16;
    if (!(x & 0xff000000U))
        n += 8, x <<= 8;
    if (!(x & 0xf0000000U))
        n += 4, x <<= 4;
    if (!(x & 0xc0000000U))
        n += 2, x <<= 2;
    if (!(x & 0x80000000U))
        n += 1;
    return n;
}

static u32 clz64(u64 x) { return x >> 32 ? clz32((u32)(x >> 32)) : 32 + clz32((u32)x); }

static u32 brev32(u32 x)
{
    x = (x >> 1 & 0x55555555U) | (x & 0x55555555U) << 1;
    x = (x >> 2 & 0x33333333U) | (x & 0x33333333U) << 2;
    x = (x >> 4 & 0x0f0f0f0fU) | (x & 0x0f0f0f0fU) << 4;
    x = (x >> 8 & 0x00ff00ffU) | (x & 0x00ff00ffU) << 8;
    return x >> 16 | x << 16;
}

static u64 umulh(u64 a, u64 b)
{
    u64 al = a & 0xffffffffUL, ah = a >> 32, bl = b & 0xffffffffUL, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (lh & 0xffffffffUL) + (hl & 0xffffffffUL);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static u64 smulh(s64 a, s64 b)
{
    u64 r = umulh((u64)a, (u64)b);
    if (a < 0)
        r -= (u64)b;
    if (b < 0)
        r -= (u64)a;
    return r;
}

static u64 bfe(u64 a, u32 pos, u32 len, u32 msb, int sgn)
{
    u64 d = 0, sbit = 0;
    u32 i;
    pos &= 0xff;
    len &= 0xff;
    if (sgn && len) {
        u32 sp = pos + len - 1 < msb ? pos + len - 1 : msb;
        sbit = a >> sp & 1;
    }
    for (i = 0; i <= msb; i++) {
        u64 bit = (i < len && pos + i <= msb) ? a >> (pos + i) & 1 : sbit;
        d |= bit << i;
    }
    return d;
}

static u64 bfi(u64 a, u64 b, u32 pos, u32 len, u32 msb)
{
    u32 i;
    pos &= 0xff;
    len &= 0xff;
    for (i = 0; i < len && pos + i <= msb; i++)
        b = (b & ~(1UL << (pos + i))) | (a >> i & 1) << (pos + i);
    return b;
}

static u32 prmt(u32 a, u32 b, u32 c, u32 mode)
{
    u64 src = (u64)b << 32 | a;
    u32 r = 0, k, s = c & 3, idx;
    for (k = 0; k < 4; k++) {
        u32 byte;
        switch (mode) {
        case 0:
            idx = c >> (4 * k) & 15;
            byte = (u32)(src >> (8 * (idx & 7))) & 0xff;
            if (idx & 8)
                byte = byte & 0x80 ? 0xff : 0;
            r |= byte << (8 * k);
            continue;
        case 1:
            idx = s + k;
            break;
        case 2:
            idx = (s - k) & 7;
            break;
        case 3:
            idx = s;
            break;
        case 4:
            idx = k > s ? k : s;
            break;
        case 5:
            idx = k < s ? k : s;
            break;
        default:
            idx = (k & 1) + 2 * (s & 1);
            break;
        }
        r |= ((u32)(src >> (8 * idx)) & 0xff) << (8 * k);
    }
    return r;
}

static u32 fns(u32 mask, u32 base, s32 offset)
{
    s32 pos = (s32)base, inc = offset > 0 ? 1 : -1;
    u32 count;
    if (base > 31)
        return 0xffffffffU; /* what the hardware answers; the manual does not say */
    if (offset == 0)
        return mask >> pos & 1 ? (u32)pos : 0xffffffffU;
    count = (u32)(offset > 0 ? offset : -offset) - 1;
    for (; pos >= 0 && pos < 32; pos += inc)
        if (mask >> pos & 1) {
            if (count == 0)
                return (u32)pos;
            count--;
        }
    return 0xffffffffU;
}

static u32 szext(u32 a, u32 b, int clamp, int sgn)
{
    u32 b1 = b & 31, too = b >= 32 && clamp, mask = too ? 0 : 0xffffffffU << b1, sp = (b1 - 1) & 31, sb;
    sb = (b1 == 0 || too || !sgn) ? 0 : a >> sp & 1;
    return (a & ~mask) | (sb ? mask : 0);
}

static u32 bmsk(u32 a, u32 b, int clamp)
{
    u32 a1 = a & 31, mask0 = 0xffffffffU << a1, b1 = b & 31, sum = a1 + b1, mask1 = sum >= 32 ? 0 : 0xffffffffU << sum;
    int so = sum >= 32, po = 0, wo = 0;
    if (clamp) {
        if (a >= 32)
            po = 1, mask0 = 0;
        if (b >= 32)
            wo = 1;
    }
    if (so || po || wo)
        mask1 = 0;
    else if (b1 == 0)
        mask1 = 0xffffffffU;
    return mask0 & ~mask1;
}

static u32 lop3(u32 a, u32 b, u32 c, u32 lut)
{
    u32 r = 0, i;
    for (i = 0; i < 8; i++)
        if (lut >> i & 1)
            r |= (i & 4 ? a : ~a) & (i & 2 ? b : ~b) & (i & 1 ? c : ~c);
    return r;
}

static u32 dp4a(u32 a, u32 b, u32 c, u32 aux)
{
    u32 i;
    for (i = 0; i < 4; i++) {
        s32 x = aux & 1 ? (s32)(s8)(a >> (8 * i)) : (s32)(a >> (8 * i) & 0xff);
        s32 y = aux & 2 ? (s32)(s8)(b >> (8 * i)) : (s32)(b >> (8 * i) & 0xff);
        c += (u32)(x * y);
    }
    return c;
}

static u32 dp2a(u32 a, u32 b, u32 c, u32 aux)
{
    u32 i, sel = aux & 4 ? 2 : 0;
    for (i = 0; i < 2; i++) {
        s32 x = aux & 1 ? (s32)(s16)(a >> (16 * i)) : (s32)(a >> (16 * i) & 0xffff);
        s32 y = aux & 2 ? (s32)(s8)(b >> (8 * (sel + i))) : (s32)(b >> (8 * (sel + i)) & 0xff);
        c += (u32)(x * y);
    }
    return c;
}

/* What is not a number becomes 0 from a float in 32 bits, and the smallest number from a double or in 64 bits:
   that is the hardware (the manual says 0 for all). */
static u32 f2s32(double f, u32 nan)
{
    if (f != f)
        return nan;
    if (f >= 2147483648.0)
        return 0x7fffffffU;
    if (f <= -2147483648.0)
        return 0x80000000U;
    return (u32)(s32)f;
}

static u32 f2u32(double f)
{
    if (f != f || f <= 0)
        return 0;
    if (f >= 4294967296.0)
        return 0xffffffffU;
    return (u32)f;
}

static u64 f2s64(double f, u64 nan)
{
    if (f != f)
        return nan;
    if (f >= 9223372036854775808.0)
        return 0x7fffffffffffffffUL;
    if (f <= -9223372036854775808.0)
        return 0x8000000000000000UL;
    return (u64)(s64)f;
}

static u32 testp(u32 aux, Slot a)
{
    u64 e, m;
    if (aux & 16) {
        e = a.u >> 52 & 0x7ff;
        m = a.u & 0xfffffffffffffUL;
        e = e == 0x7ff ? 255 : e ? 1 : 0;
    } else {
        e = a.w >> 23 & 0xff;
        m = a.w & 0x7fffff;
    }
    switch (aux & 15) {
    case 0:
        return e != 255;
    case 1:
        return e == 255 && !m;
    case 2:
        return !(e == 255 && m);
    case 3:
        return e == 255 && m;
    case 4:
        return e != 255 && (e != 0 || !m); /* a zero is "normal" on the hardware */
    default:
        return e == 0 && m;
    }
}

/* ---- frames ---- */

static void apply_fix(Blk *b, Thr *t, McFunc *f, Slot *R)
{
    u32 i;
    Launch *L = b->L;
    for (i = 0; i < f->nfix; i++) {
        Fix *x = &f->fix[i];
        u64 v = 0;
        switch (x->kind) {
        case FX_FRAME:
            v = (u64)R + x->off;
            break;
        case FX_PARAM:
            v = (u64)L->params + x->off;
            break;
        case FX_TID_X:
        case FX_TID_Y:
        case FX_TID_Z:
            v = t->xyz[x->kind - FX_TID_X];
            break;
        case FX_NTID_X:
        case FX_NTID_Y:
        case FX_NTID_Z:
            v = L->block[x->kind - FX_NTID_X];
            break;
        case FX_CTAID_X:
        case FX_CTAID_Y:
        case FX_CTAID_Z:
            v = b->cta[x->kind - FX_CTAID_X];
            break;
        case FX_NCTAID_X:
        case FX_NCTAID_Y:
        case FX_NCTAID_Z:
            v = L->grid[x->kind - FX_NCTAID_X];
            break;
        case FX_LANEID:
            v = t->lane;
            break;
        case FX_WARPID:
            v = t->warp;
            break;
        case FX_NWARPID:
            v = b->nwarp;
            break;
        case FX_NSMID:
            v = MC_DEV_NSM;
            break;
        case FX_LANEMASK_EQ:
            v = 1U << t->lane;
            break;
        case FX_LANEMASK_LE:
            v = 0xffffffffU >> (31 - t->lane);
            break;
        case FX_LANEMASK_LT:
            v = (1U << t->lane) - 1;
            break;
        case FX_LANEMASK_GE:
            v = 0xffffffffU << t->lane;
            break;
        case FX_LANEMASK_GT:
            v = ~(0xffffffffU >> (31 - t->lane));
            break;
        case FX_DYN_SMEM:
            v = L->dyn_shared;
            break;
        case FX_TOTAL_SMEM:
            v = L->shared_bytes;
            break;
        case FX_ONE:
            v = 1;
            break;
        }
        R[x->slot].u = v;
    }
}

static int stack_need(Blk *b, Thr *t, u64 n)
{
    if (!t->stk) {
        u64 cap = n > STACK_BYTES / 2 ? n * 2 + STACK_BYTES : STACK_BYTES;
        t->stk = (u8 *)malloc(cap + 32);
        if (!t->stk) {
            mc_fault(b, t, "no memory for a thread's stack%s (%lu bytes)", 0, cap);
            return -1;
        }
        t->sp = (u8 *)(((u64)t->stk + 15) & ~15UL);
        t->stk_end = t->stk + cap;
    }
    if ((u64)(t->stk_end - t->sp) < n) {
        mc_fault(b, t, "stack overflow%s (%lu more bytes wanted)", 0, n);
        return -1;
    }
    return 0;
}

u8 *mc_stack_alloc(Blk *b, Thr *t, u64 n, u64 align)
{
    u8 *r;
    if (align < 16)
        align = 16;
    if (n > (1UL << 40) || stack_need(b, t, n + align) < 0) {
        mc_fault(b, t, "alloca%s of %lu bytes", 0, n);
        return 0;
    }
    r = (u8 *)(((u64)t->sp + align - 1) / align * align);
    t->sp = (u8 *)(((u64)r + n + 15) & ~15UL);
    return r;
}

static u8 *item_ptr(Thr *t, Slot *R, McFunc *f, s32 *it)
{
    (void)t;
    return it[0] ? (u8 *)R + (u64)f->nreg * 8 + (u32)it[1] : (u8 *)&R[it[1]];
}

static u8 *formal_ptr(Slot *R, McFunc *f, Param *q)
{
    return q->isreg ? (u8 *)&R[q->slot] : (u8 *)R + (u64)f->nreg * 8 + q->off;
}

static int valid_func(McFunc *g)
{
    McModule *m;
    McFunc *f;
    for (m = mc_modules(); m; m = m->next)
        for (f = m->funcs; f; f = f->next)
            if (f == g)
                return f->decoded;
    return 0;
}

/* t->ip is at a call instruction: enter the callee. */
static int do_call(Blk *b, Thr *t, Ins *ip, McFunc *g)
{
    McFunc *f = t->f;
    s32 *x = f->xtra + ip->a, *it;
    int nr = x[2], na = x[3], i;
    u8 *base;
    Slot *R = t->R, *N;
    if (!g) {
        g = (McFunc *)R[ip->b].u;
        if (!valid_func(g)) {
            mc_fault(b, t, "indirect call of something that is not a function%s (%lx)", 0, (u64)g);
            return -1;
        }
        if (g->nparams != na || g->nrets != nr) {
            mc_fault(b, t, "indirect call of %s with the wrong number of arguments", g->name, 0);
            return -1;
        }
    }
    if (t->ncs >= 4000) {
        mc_fault(b, t, "calls nested too deeply%s", 0, 0);
        return -1;
    }
    if (stack_need(b, t, g->frame_bytes) < 0)
        return -1;
    if (t->ncs == t->capcs) {
        t->capcs = t->capcs ? t->capcs * 2 : 8;
        t->cs = (CallRec *)realloc(t->cs, t->capcs * sizeof(CallRec));
        if (!t->cs) {
            mc_fault(b, t, "out of memory%s", 0, 0);
            return -1;
        }
    }
    base = t->sp;
    N = (Slot *)base + g->nconst;
    memcpy(base, g->ctemplate, (size_t)g->nconst * 8);
    apply_fix(b, t, g, N);
    it = x + 4 + 3 * nr;
    for (i = 0; i < na; i++, it += 3) {
        Param *q = &g->params[i];
        u32 n = q->size < (u32)it[2] ? q->size : (u32)it[2];
        if (q->isreg)
            N[q->slot].u = 0;
        memcpy(formal_ptr(N, g, q), item_ptr(t, R, f, it), n);
    }
    t->cs[t->ncs].ip = ip;
    t->cs[t->ncs].R = R;
    t->cs[t->ncs].f = f;
    t->cs[t->ncs].sp = t->sp;
    t->ncs++;
    t->sp = base + g->frame_bytes;
    t->f = g;
    t->R = N;
    t->ip = g->code;
    return 0;
}

static void do_ret(Thr *t)
{
    CallRec *c = &t->cs[--t->ncs];
    McFunc *g = t->f;
    s32 *x = c->f->xtra + c->ip->a, *it = x + 4;
    int nr = x[2], i;
    for (i = 0; i < nr; i++, it += 3) {
        Param *q = &g->rets[i];
        u32 n = q->size < (u32)it[2] ? q->size : (u32)it[2];
        u8 *dst = item_ptr(t, c->R, c->f, it);
        if (!it[0])
            c->R[it[1]].u = 0;
        memcpy(dst, formal_ptr(t->R, g, q), n);
    }
    t->sp = c->sp;
    t->f = c->f;
    t->R = c->R;
    t->ip = c->ip + 1;
}

/* ---- barriers ---- */

static void bar_release(Blk *b, u32 id, Thr *self)
{
    u32 i, all = 1, any = 0, cnt = 0;
    Bar *bar = &b->bars[id];
    for (i = 0; i < b->nthr; i++) {
        Thr *w = &b->thr[i];
        if ((w->state == TS_BAR && w->bar == id) || w == self) {
            Ins *ins = w == self ? w->ip : w->ip - 1;
            if (ins->aux >= 2) {
                u32 pv = w->R[ins->c].w & 1;
                all &= pv;
                any |= pv;
                cnt += pv;
            }
        }
    }
    for (i = 0; i < b->nthr; i++) {
        Thr *w = &b->thr[i];
        if ((w->state == TS_BAR && w->bar == id) || w == self) {
            Ins *ins = w == self ? w->ip : w->ip - 1;
            if (ins->aux >= 2)
                w->R[ins->d].u = ins->aux == 2 ? all : ins->aux == 3 ? any : cnt;
            w->state = TS_RUN;
        }
    }
    bar->arrived = 0;
    bar->nwait = 0;
}

/* A thread reaches a bar instruction (t->ip is at it). 1: it goes on; 0: it waits. */
static int bar_op(Blk *b, Thr *t, Ins *ip)
{
    u32 id = t->R[ip->a].w & 15, need = ip->b == NOSLOT ? 0 : t->R[ip->b].w;
    Bar *bar = &b->bars[id];
    bar->arrived++;
    bar->need = need;
    if (bar->arrived >= (need ? need : b->live)) {
        bar_release(b, id, ip->aux == 1 ? 0 : t);
        return 1;
    }
    if (ip->aux == 1)
        return 1;
    bar->nwait++;
    t->state = TS_BAR;
    t->bar = id;
    return 0;
}

/* A thread has left the kernel: barriers that wait for every live thread may be complete now. */
static void bar_recheck(Blk *b)
{
    u32 id;
    for (id = 0; id < 16; id++) {
        Bar *bar = &b->bars[id];
        if (bar->nwait && !bar->need && bar->arrived >= b->live)
            bar_release(b, id, 0);
    }
}

static u32 live_mask(Blk *b, u32 warp)
{
    u32 i, m = 0, base = warp * 32;
    for (i = 0; i < 32 && base + i < b->nthr; i++)
        if (b->thr[base + i].state != TS_EXIT)
            m |= 1U << i;
    return m;
}

/* ---- the instruction loop ---- */

#define RD R[ip->d]
#define RA R[ip->a]
#define RB R[ip->b]
#define RC R[ip->c]
#define RE R[ip->e]
#define EA (RA.u + (u64)(s64)ip->c)
#define CHK(ea)       \
    if ((ea) < 4096) \
        goto badaddr
#define SEA(ea)                \
    if ((ea) < shsz)           \
        (ea) += (u64)sh;       \
    else if ((ea) < 4096)      \
        goto badaddr
#define FTZ32(x) (((x) & 0x7f800000U) ? (x) : (x) & 0x80000000U)
#define ICMP(base, fld)                 \
    case base + CMP_EQ:                 \
        RD.u = RA.fld == RB.fld;        \
        ip++;                           \
        break;                          \
    case base + CMP_NE:                 \
        RD.u = RA.fld != RB.fld;        \
        ip++;                           \
        break;                          \
    case base + CMP_LT:                 \
        RD.u = RA.fld < RB.fld;         \
        ip++;                           \
        break;                          \
    case base + CMP_LE:                 \
        RD.u = RA.fld <= RB.fld;        \
        ip++;                           \
        break;                          \
    case base + CMP_GT:                 \
        RD.u = RA.fld > RB.fld;         \
        ip++;                           \
        break;                          \
    case base + CMP_GE:                 \
        RD.u = RA.fld >= RB.fld;        \
        ip++;                           \
        break
#define FCMP(base, fld, T)                           \
    case base + CMP_EQ:                              \
        RD.u = RA.fld == RB.fld;                     \
        ip++;                                        \
        break;                                       \
    case base + CMP_NE:                              \
        RD.u = RA.fld < RB.fld || RA.fld > RB.fld;   \
        ip++;                                        \
        break;                                       \
    case base + CMP_LT:                              \
        RD.u = RA.fld < RB.fld;                      \
        ip++;                                        \
        break;                                       \
    case base + CMP_LE:                              \
        RD.u = RA.fld <= RB.fld;                     \
        ip++;                                        \
        break;                                       \
    case base + CMP_GT:                              \
        RD.u = RA.fld > RB.fld;                      \
        ip++;                                        \
        break;                                       \
    case base + CMP_GE:                              \
        RD.u = RA.fld >= RB.fld;                     \
        ip++;                                        \
        break;                                       \
    case base + CMP_EQU: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x == y || x != x || y != y;           \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_NEU:                             \
        RD.u = RA.fld != RB.fld;                     \
        ip++;                                        \
        break;                                       \
    case base + CMP_LTU: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x < y || x != x || y != y;            \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_LEU: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x <= y || x != x || y != y;           \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_GTU: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x > y || x != x || y != y;            \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_GEU: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x >= y || x != x || y != y;           \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_NUM: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x == x && y == y;                     \
        ip++;                                        \
        break;                                       \
    }                                                \
    case base + CMP_NAN: {                           \
        T x = RA.fld, y = RB.fld;                    \
        RD.u = x != x || y != y;                     \
        ip++;                                        \
        break;                                       \
    }

static u64 ld_elem(u32 k, u64 ea)
{
    switch (k) {
    case 0:
        return *(u8 *)ea;
    case 1:
        return (u32)(s32) * (s8 *)ea;
    case 2:
        return (u64)(s64) * (s8 *)ea;
    case 3:
        return *(u16 *)ea;
    case 4:
        return (u32)(s32) * (s16 *)ea;
    case 5:
        return (u64)(s64) * (s16 *)ea;
    case 6:
        return *(u32 *)ea;
    case 7:
        return (u64)(s64) * (s32 *)ea;
    }
    return *(u64 *)ea;
}

u64 mc_jh_prmt(u64 a, u64 b, u64 c, u64 mode) { return prmt((u32)a, (u32)b, (u32)c, (u32)mode); }

u64 mc_jh_fgen(u64 sub, u64 flags, u64 a, u64 b, u64 c)
{
    Slot x, y, z;
    x.u = a;
    y.u = b;
    z.u = c;
    return mc_fgen((u32)sub, (u32)flags, x, y, z);
}

static u64 jit_exits[OP_COUNT]; /* MCUDA_STATS: how often the translated code left at an instruction of this kind */

/* a float result: the slot's high half becomes zero, so that an instruction sets all of its slot */
#define SETF(E) \
    { \
        Slot z_; \
        z_.u = 0; \
        z_.f = (E); \
        RD.u = z_.u; \
    }

static int run(Blk *b, Thr *t)
{
    Ins *ip = t->ip;
    Slot *R = t->R;
    u8 *sh = b->shared;
    u64 shsz = b->shared_size, n = 0, ea = 0;
    int ret;
    Slot x, y;
    McJit *jf = t->f->jit;
    McJitCtx jc;
    jc.sh = sh;
    jc.shsz = shsz;
    jc.h2f = mc_h2f_tab;
    jc.blk = b;
    jc.thr = t;
    for (;;) {
        if (jf) {
            u32 at = jf->off[ip - t->f->code];
            if (!(at & MC_JIT_NOENTER)) {
                jc.lane = 0;
                jc.nl = 1;
                at = mc_jit_enter(jf, R, &jc, at);
                n += jc.n;
                if (at & MC_JIT_FAULT) {
                    n++;
                    goto fault;
                }
                ip = t->f->code + at;
                if (mc_stats_on)
                    jit_exits[ip->op < OP_COUNT ? ip->op : 0]++;
            }
        }
        n++;
        switch (ip->op) {
        case OP_NOP:
            ip++;
            break;
        case OP_BRA:
            ip += ip->a;
            break;
        case OP_BRX: {
            s32 *x = t->f->xtra + ip->e;
            if (RA.w >= (u32)x[0]) {
                t->ip = ip;
                mc_fault(b, t, "brx.idx%s: the index %lu is outside the list of targets", 0, (u64)RA.w);
                goto fault;
            }
            ip = t->f->code + x[1 + RA.w];
            break;
        }
        case OP_BRT:
            ip += RA.w ? ip->b : 1;
            break;
        case OP_BRF:
            ip += RA.w ? 1 : ip->b;
            break;
        case OP_SKIPF:
            ip += RA.w ? 1 : 1 + ip->b;
            break;
        case OP_SKIPT:
            ip += RA.w ? 1 + ip->b : 1;
            break;
        case OP_CALL:
            t->ip = ip;
            t->R = R;
            if (do_call(b, t, ip, (McFunc *)((u64)(u32)t->f->xtra[ip->a] | (u64)(u32)t->f->xtra[ip->a + 1] << 32)) < 0)
                goto fault;
            ip = t->ip;
            R = t->R;
            jf = t->f->jit;
            break;
        case OP_CALLI:
            t->ip = ip;
            t->R = R;
            if (do_call(b, t, ip, 0) < 0)
                goto fault;
            ip = t->ip;
            R = t->R;
            jf = t->f->jit;
            break;
        case OP_CALLB:
            t->ip = ip;
            t->R = R;
            if (mc_builtin_call(b, t, ip->aux, ip) < 0)
                goto fault;
            ip++;
            break;
        case OP_BADFUNC:
            t->ip = ip;
            mc_fault(b, t, "call of the undefined function %s", ((McFunc *)((u64)(u32)t->f->xtra[ip->a] | (u64)(u32)t->f->xtra[ip->a + 1] << 32))->name, 0);
            goto fault;
        case OP_RET:
            if (!t->ncs) {
                ret = RUN_EXIT;
                goto out;
            }
            do_ret(t);
            ip = t->ip;
            R = t->R;
            jf = t->f->jit;
            break;
        case OP_EXIT:
            ret = RUN_EXIT;
            goto out;
        case OP_TRAP:
            t->ip = ip;
            mc_fault(b, t, "trap%s", 0, 0);
            goto fault;
        case OP_BAR:
            t->ip = ip;
            t->R = R;
            if (!bar_op(b, t, ip)) {
                ip++;
                ret = RUN_PARK;
                goto out;
            }
            ip++;
            break;
        case OP_WSYNC:
            t->state = TS_WSYNC;
            ret = RUN_PARK;
            goto out;
        case OP_CLOCK:
            RD.u = b->icount + n;
            ip++;
            break;
        case OP_ACTIVEMASK:
            RD.u = live_mask(b, t->warp);
            ip++;
            break;
        case OP_ALLOCA:
            t->ip = ip;
            ea = (u64)mc_stack_alloc(b, t, RA.u, (u64)ip->b);
            if (!ea)
                goto fault;
            RD.u = ea;
            ip++;
            break;
        case OP_STACKSAVE:
            t->ip = ip;
            if (stack_need(b, t, 0) < 0)
                goto fault;
            RD.u = (u64)t->sp;
            ip++;
            break;
        case OP_STACKRESTORE:
            ea = RA.u;
            if (!t->stk || ea < (u64)t->stk || ea > (u64)t->stk_end) {
                t->ip = ip;
                mc_fault(b, t, "stackrestore%s of a value that stacksave did not give (%lx)", 0, ea);
                goto fault;
            }
            t->sp = (u8 *)ea;
            ip++;
            break;

        case OP_MOV:
            RD.u = RA.u;
            ip++;
            break;
        case OP_MOV32:
            RD.u = RA.w;
            ip++;
            break;
        case OP_ZX8:
            RD.u = RA.w & 0xff;
            ip++;
            break;
        case OP_ZX16:
            RD.u = RA.w & 0xffff;
            ip++;
            break;
        case OP_SX8_32:
            RD.u = (u32)(s32)(s8)RA.w;
            ip++;
            break;
        case OP_SX16_32:
            RD.u = (u32)(s32)(s16)RA.w;
            ip++;
            break;
        case OP_SX8_64:
            RD.s = (s64)(s8)RA.w;
            ip++;
            break;
        case OP_SX16_64:
            RD.s = (s64)(s16)RA.w;
            ip++;
            break;
        case OP_SX32_64:
            RD.s = (s64)RA.i;
            ip++;
            break;
        case OP_PNOT:
            RD.u = (RA.w ^ 1) & 1;
            ip++;
            break;
        case OP_PAND:
            RD.u = RA.w & RB.w & 1;
            ip++;
            break;
        case OP_POR:
            RD.u = (RA.w | RB.w) & 1;
            ip++;
            break;
        case OP_PXOR:
            RD.u = (RA.w ^ RB.w) & 1;
            ip++;
            break;

        case OP_ADD32:
            RD.u = (u32)(RA.w + RB.w);
            ip++;
            break;
        case OP_SUB32:
            RD.u = (u32)(RA.w - RB.w);
            ip++;
            break;
        case OP_MUL32:
            RD.u = (u32)(RA.w * RB.w);
            ip++;
            break;
        case OP_MULHIU32:
            RD.u = (u32)((u64)RA.w * RB.w >> 32);
            ip++;
            break;
        case OP_MULHIS32:
            RD.u = (u32)((u64)((s64)RA.i * RB.i) >> 32);
            ip++;
            break;
        case OP_MULWU32:
            RD.u = (u64)RA.w * RB.w;
            ip++;
            break;
        case OP_MULWS32:
            RD.s = (s64)RA.i * RB.i;
            ip++;
            break;
        case OP_MAD32:
            RD.u = (u32)(RA.w * RB.w + RC.w);
            ip++;
            break;
        case OP_MADHIU32:
            RD.u = (u32)((u32)((u64)RA.w * RB.w >> 32) + RC.w);
            ip++;
            break;
        case OP_MADHIS32:
            RD.u = (u32)((u32)((u64)((s64)RA.i * RB.i) >> 32) + RC.w);
            ip++;
            break;
        case OP_MADWU32:
            RD.u = (u64)RA.w * RB.w + RC.u;
            ip++;
            break;
        case OP_MADWS32:
            RD.u = (u64)((s64)RA.i * RB.i) + RC.u;
            ip++;
            break;
        case OP_MUL24:
        case OP_MUL24HIU:
        case OP_MUL24HIS: {
            s64 p, q;
            if (ip->aux) {
                p = (s32)(RA.w << 8) >> 8;
                q = (s32)(RB.w << 8) >> 8;
            } else {
                p = RA.w & 0xffffff;
                q = RB.w & 0xffffff;
            }
            p *= q;
            RD.u = ip->op == OP_MUL24 ? (u32)p : (u32)((u64)p >> 16);
            ip++;
            break;
        }
        case OP_DIVU32:
            RD.u = RB.w ? RA.w / RB.w : 0xffffffffU;
            ip++;
            break;
        case OP_DIVS32:
            RD.u = RB.i == 0 ? 0xffffffffU : RB.i == -1 ? (u32)(0 - RA.w) : (u32)(RA.i / RB.i);
            ip++;
            break;
        case OP_REMU32:
            RD.u = RB.w ? RA.w % RB.w : RA.w;
            ip++;
            break;
        case OP_REMS32:
            RD.u = RB.i == 0 ? RA.w : RB.i == -1 ? 0 : (u32)(RA.i % RB.i);
            ip++;
            break;
        case OP_ABS32:
            RD.u = RA.i < 0 ? (u32)(0 - RA.w) : RA.w;
            ip++;
            break;
        case OP_NEG32:
            RD.u = (u32)(0 - RA.w);
            ip++;
            break;
        case OP_MINU32:
            RD.u = RA.w < RB.w ? RA.w : RB.w;
            ip++;
            break;
        case OP_MINS32:
            RD.u = RA.i < RB.i ? RA.w : RB.w;
            ip++;
            break;
        case OP_MAXU32:
            RD.u = RA.w > RB.w ? RA.w : RB.w;
            ip++;
            break;
        case OP_MAXS32:
            RD.u = RA.i > RB.i ? RA.w : RB.w;
            ip++;
            break;
        case OP_POPC32:
            RD.u = popc32(RA.w);
            ip++;
            break;
        case OP_CLZ32:
            RD.u = clz32(RA.w);
            ip++;
            break;
        case OP_BFINDU32:
        case OP_BFINDS32: {
            u32 v = RA.w, r;
            if (ip->op == OP_BFINDS32 && (v & 0x80000000U))
                v = ~v;
            r = v ? 31 - clz32(v) : 0xffffffffU;
            if (ip->aux && v)
                r = 31 - r;
            RD.u = r;
            ip++;
            break;
        }
        case OP_BREV32:
            RD.u = brev32(RA.w);
            ip++;
            break;
        case OP_BFEU32:
            RD.u = (u32)bfe(RA.w, RB.w, RC.w, 31, 0);
            ip++;
            break;
        case OP_BFES32:
            RD.u = (u32)bfe(RA.w, RB.w, RC.w, 31, 1);
            ip++;
            break;
        case OP_BFI32:
            RD.u = (u32)bfi(RA.w, RB.w, RC.w, RE.w, 31);
            ip++;
            break;
        case OP_SHL32:
            RD.u = RB.w >= 32 ? 0 : (u32)(RA.w << RB.w);
            ip++;
            break;
        case OP_SHRU32:
            RD.u = RB.w >= 32 ? 0 : RA.w >> RB.w;
            ip++;
            break;
        case OP_SHRS32:
            RD.u = (u32)(RA.i >> (RB.w >= 32 ? 31 : RB.w));
            ip++;
            break;
        case OP_AND32:
            RD.u = RA.w & RB.w;
            ip++;
            break;
        case OP_OR32:
            RD.u = RA.w | RB.w;
            ip++;
            break;
        case OP_XOR32:
            RD.u = RA.w ^ RB.w;
            ip++;
            break;
        case OP_NOT32:
            RD.u = (u32)~RA.w;
            ip++;
            break;
        case OP_CNOT32:
            RD.u = RA.w == 0;
            ip++;
            break;
        case OP_SHFL: {
            u32 s = ip->aux ? (RC.w > 32 ? 32 : RC.w) : RC.w & 31;
            RD.u = s == 0 ? RB.w : s == 32 ? RA.w : (u32)(RB.w << s | RA.w >> (32 - s));
            ip++;
            break;
        }
        case OP_SHFR: {
            u32 s = ip->aux ? (RC.w > 32 ? 32 : RC.w) : RC.w & 31;
            RD.u = s == 0 ? RA.w : s == 32 ? RB.w : (u32)(RB.w << (32 - s) | RA.w >> s);
            ip++;
            break;
        }
        case OP_PRMT:
            RD.u = prmt(RA.w, RB.w, RC.w, ip->aux);
            ip++;
            break;
        case OP_ADDCC32: {
            u64 r = (u64)RA.w + RB.w;
            RD.u = (u32)r;
            t->cf = (u32)(r >> 32);
            ip++;
            break;
        }
        case OP_ADDC32: {
            u64 r = (u64)RA.w + RB.w + t->cf;
            RD.u = (u32)r;
            if (ip->aux)
                t->cf = (u32)(r >> 32);
            ip++;
            break;
        }
        case OP_SUBCC32: {
            u64 r = (u64)RA.w - RB.w;
            RD.u = (u32)r;
            t->cf = (u32)(r >> 32) & 1;
            ip++;
            break;
        }
        case OP_SUBC32: {
            u64 r = (u64)RA.w - RB.w - t->cf;
            RD.u = (u32)r;
            if (ip->aux)
                t->cf = (u32)(r >> 32) & 1;
            ip++;
            break;
        }
        case OP_SADU32:
            RD.u = (u32)(RC.w + (RA.w < RB.w ? RB.w - RA.w : RA.w - RB.w));
            ip++;
            break;
        case OP_SADS32:
            RD.u = (u32)(RC.w + (RA.i < RB.i ? RB.w - RA.w : RA.w - RB.w));
            ip++;
            break;
        case OP_DP4A:
            RD.u = dp4a(RA.w, RB.w, RC.w, ip->aux);
            ip++;
            break;
        case OP_DP2A:
            RD.u = dp2a(RA.w, RB.w, RC.w, ip->aux);
            ip++;
            break;
        case OP_FNS:
            RD.u = fns(RA.w, RB.w, RC.i);
            ip++;
            break;
        case OP_SZEXT:
            RD.u = szext(RA.w, RB.w, ip->aux & 1, ip->aux & 2);
            ip++;
            break;
        case OP_BMSK:
            RD.u = bmsk(RA.w, RB.w, ip->aux & 1);
            ip++;
            break;
        case OP_LOP3:
            RD.u = lop3(RA.w, RB.w, RC.w, ip->aux);
            ip++;
            break;
        case OP_ADDSAT32: {
            s64 r = (s64)RA.i + RB.i;
            RD.u = (u32)(r > 2147483647L ? 2147483647L : r < -2147483648L ? -2147483648L : r);
            ip++;
            break;
        }
        case OP_SUBSAT32: {
            s64 r = (s64)RA.i - RB.i;
            RD.u = (u32)(r > 2147483647L ? 2147483647L : r < -2147483648L ? -2147483648L : r);
            ip++;
            break;
        }

        case OP_ADD64:
            RD.u = RA.u + RB.u;
            ip++;
            break;
        case OP_SUB64:
            RD.u = RA.u - RB.u;
            ip++;
            break;
        case OP_MUL64:
            RD.u = RA.u * RB.u;
            ip++;
            break;
        case OP_MULHIU64:
            RD.u = umulh(RA.u, RB.u);
            ip++;
            break;
        case OP_MULHIS64:
            RD.u = smulh(RA.s, RB.s);
            ip++;
            break;
        case OP_MAD64:
            RD.u = RA.u * RB.u + RC.u;
            ip++;
            break;
        case OP_MADHIU64:
            RD.u = umulh(RA.u, RB.u) + RC.u;
            ip++;
            break;
        case OP_MADHIS64:
            RD.u = smulh(RA.s, RB.s) + RC.u;
            ip++;
            break;
        case OP_DIVU64:
            RD.u = RB.u ? RA.u / RB.u : ~0UL;
            ip++;
            break;
        case OP_DIVS64:
            RD.u = RB.s == 0 ? ~0UL : RB.s == -1 ? 0 - RA.u : (u64)(RA.s / RB.s);
            ip++;
            break;
        case OP_REMU64:
            RD.u = RB.u ? RA.u % RB.u : RA.u;
            ip++;
            break;
        case OP_REMS64:
            RD.u = RB.s == 0 ? RA.u : RB.s == -1 ? 0 : (u64)(RA.s % RB.s);
            ip++;
            break;
        case OP_ABS64:
            RD.u = RA.s < 0 ? 0 - RA.u : RA.u;
            ip++;
            break;
        case OP_NEG64:
            RD.u = 0 - RA.u;
            ip++;
            break;
        case OP_MINU64:
            RD.u = RA.u < RB.u ? RA.u : RB.u;
            ip++;
            break;
        case OP_MINS64:
            RD.u = RA.s < RB.s ? RA.u : RB.u;
            ip++;
            break;
        case OP_MAXU64:
            RD.u = RA.u > RB.u ? RA.u : RB.u;
            ip++;
            break;
        case OP_MAXS64:
            RD.u = RA.s > RB.s ? RA.u : RB.u;
            ip++;
            break;
        case OP_POPC64:
            RD.u = popc32((u32)RA.u) + popc32((u32)(RA.u >> 32));
            ip++;
            break;
        case OP_CLZ64:
            RD.u = clz64(RA.u);
            ip++;
            break;
        case OP_BFINDU64:
        case OP_BFINDS64: {
            u64 v = RA.u;
            u32 r;
            if (ip->op == OP_BFINDS64 && (v >> 63))
                v = ~v;
            r = v ? 63 - clz64(v) : 0xffffffffU;
            if (ip->aux && v)
                r = 63 - r;
            RD.u = r;
            ip++;
            break;
        }
        case OP_BREV64:
            RD.u = (u64)brev32((u32)RA.u) << 32 | brev32((u32)(RA.u >> 32));
            ip++;
            break;
        case OP_BFEU64:
            RD.u = bfe(RA.u, RB.w, RC.w, 63, 0);
            ip++;
            break;
        case OP_BFES64:
            RD.u = bfe(RA.u, RB.w, RC.w, 63, 1);
            ip++;
            break;
        case OP_BFI64:
            RD.u = bfi(RA.u, RB.u, RC.w, RE.w, 63);
            ip++;
            break;
        case OP_SHL64:
            RD.u = RB.w >= 64 ? 0 : RA.u << RB.w;
            ip++;
            break;
        case OP_SHRU64:
            RD.u = RB.w >= 64 ? 0 : RA.u >> RB.w;
            ip++;
            break;
        case OP_SHRS64:
            RD.s = RA.s >> (RB.w >= 64 ? 63 : RB.w);
            ip++;
            break;
        case OP_AND64:
            RD.u = RA.u & RB.u;
            ip++;
            break;
        case OP_OR64:
            RD.u = RA.u | RB.u;
            ip++;
            break;
        case OP_XOR64:
            RD.u = RA.u ^ RB.u;
            ip++;
            break;
        case OP_NOT64:
            RD.u = ~RA.u;
            ip++;
            break;
        case OP_CNOT64:
            RD.u = RA.u == 0;
            ip++;
            break;
        case OP_ADDCC64: {
            u64 r = RA.u + RB.u;
            t->cf = r < RA.u;
            RD.u = r;
            ip++;
            break;
        }
        case OP_ADDC64: {
            u64 r = RA.u + RB.u, r2 = r + t->cf;
            u32 c = (r < RA.u) | (r2 < r);
            RD.u = r2;
            if (ip->aux)
                t->cf = c;
            ip++;
            break;
        }
        case OP_SUBCC64: {
            u64 r = RA.u - RB.u;
            t->cf = RA.u < RB.u;
            RD.u = r;
            ip++;
            break;
        }
        case OP_SUBC64: {
            u64 r = RA.u - RB.u, r2 = r - t->cf;
            u32 c = (RA.u < RB.u) | (r < (u64)t->cf);
            RD.u = r2;
            if (ip->aux)
                t->cf = c;
            ip++;
            break;
        }
        case OP_SADU64:
            RD.u = RC.u + (RA.u < RB.u ? RB.u - RA.u : RA.u - RB.u);
            ip++;
            break;
        case OP_SADS64:
            RD.u = RC.u + (RA.s < RB.s ? RB.u - RA.u : RA.u - RB.u);
            ip++;
            break;

            ICMP(OP_SETP_U32, w);
            ICMP(OP_SETP_S32, i);
            ICMP(OP_SETP_U64, u);
            ICMP(OP_SETP_S64, s);
            FCMP(OP_SETP_F32, f, float)
            FCMP(OP_SETP_F64, d, double)
        case OP_SELP:
            RD.u = RC.w ? RA.u : RB.u;
            ip++;
            break;
        case OP_SELP32:
            RD.u = RC.w ? RA.w : RB.w;
            ip++;
            break;
        case OP_SLCT_S32:
            RD.u = RC.i >= 0 ? RA.u : RB.u;
            ip++;
            break;
        case OP_SLCT_F32:
            RD.u = RC.f >= 0.0f ? RA.u : RB.u;
            ip++;
            break;
        case OP_SLCT_F32FTZ:
            RD.u = (!(RC.w & 0x7f800000U) || RC.f >= 0.0f) ? RA.u : RB.u;
            ip++;
            break;

        case OP_FADD32:
            SETF(RA.f + RB.f);
            ip++;
            break;
        case OP_FSUB32:
            SETF(RA.f - RB.f);
            ip++;
            break;
        case OP_FMUL32:
            SETF(RA.f * RB.f);
            ip++;
            break;
        case OP_FDIV32:
            SETF(RA.f / RB.f);
            ip++;
            break;
        case OP_FMA32:
            SETF(mc_fmaf(RA.f, RB.f, RC.f));
            ip++;
            break;
        case OP_FADD32Z:
            x.w = FTZ32(RA.w);
            y.w = FTZ32(RB.w);
            x.f = x.f + y.f;
            RD.u = FTZ32(x.w);
            ip++;
            break;
        case OP_FSUB32Z:
            x.w = FTZ32(RA.w);
            y.w = FTZ32(RB.w);
            x.f = x.f - y.f;
            RD.u = FTZ32(x.w);
            ip++;
            break;
        case OP_FMUL32Z:
            x.w = FTZ32(RA.w);
            y.w = FTZ32(RB.w);
            x.f = x.f * y.f;
            RD.u = FTZ32(x.w);
            ip++;
            break;
        case OP_FDIV32Z:
            x.w = FTZ32(RA.w);
            y.w = FTZ32(RB.w);
            x.f = x.f / y.f;
            RD.u = FTZ32(x.w);
            ip++;
            break;
        case OP_FMA32Z: {
            Slot z;
            x.w = FTZ32(RA.w);
            y.w = FTZ32(RB.w);
            z.w = FTZ32(RC.w);
            x.f = mc_fmaf(x.f, y.f, z.f);
            RD.u = FTZ32(x.w);
            ip++;
            break;
        }
        case OP_FMIN32: {
            float p = RA.f, q = RB.f;
            if (p != p)
                RD.u = q != q ? 0x7fffffffU : RB.w;
            else if (q != q)
                RD.u = RA.w;
            else
                RD.u = p < q ? RA.w : q < p ? RB.w : RA.w | RB.w;
            ip++;
            break;
        }
        case OP_FMAX32: {
            float p = RA.f, q = RB.f;
            if (p != p)
                RD.u = q != q ? 0x7fffffffU : RB.w;
            else if (q != q)
                RD.u = RA.w;
            else
                RD.u = p > q ? RA.w : q > p ? RB.w : RA.w & RB.w;
            ip++;
            break;
        }
        case OP_FABS32:
            RD.u = RA.w & 0x7fffffffU;
            ip++;
            break;
        case OP_FNEG32:
            RD.u = RA.w ^ 0x80000000U;
            ip++;
            break;
        case OP_FSQRT32:
            SETF(mc_sqrtf(RA.f));
            ip++;
            break;
        case OP_FRSQRT32:
            SETF((float)(1.0 / mc_sqrt((double)RA.f)));
            ip++;
            break;
        case OP_FRCP32:
            SETF(1.0f / RA.f);
            ip++;
            break;
        case OP_FADD64:
            RD.d = RA.d + RB.d;
            ip++;
            break;
        case OP_FSUB64:
            RD.d = RA.d - RB.d;
            ip++;
            break;
        case OP_FMUL64:
            RD.d = RA.d * RB.d;
            ip++;
            break;
        case OP_FDIV64:
            RD.d = RA.d / RB.d;
            ip++;
            break;
        case OP_FMA64:
            RD.d = mc_fma(RA.d, RB.d, RC.d);
            ip++;
            break;
        case OP_FMIN64: {
            double p = RA.d, q = RB.d;
            if (p != p)
                RD.u = q != q ? 0x7fffffffffffffffUL : RB.u;
            else if (q != q)
                RD.u = RA.u;
            else
                RD.u = p < q ? RA.u : q < p ? RB.u : RA.u | RB.u;
            ip++;
            break;
        }
        case OP_FMAX64: {
            double p = RA.d, q = RB.d;
            if (p != p)
                RD.u = q != q ? 0x7fffffffffffffffUL : RB.u;
            else if (q != q)
                RD.u = RA.u;
            else
                RD.u = p > q ? RA.u : q > p ? RB.u : RA.u & RB.u;
            ip++;
            break;
        }
        case OP_FABS64:
            RD.u = RA.u & 0x7fffffffffffffffUL;
            ip++;
            break;
        case OP_FNEG64:
            RD.u = RA.u ^ 0x8000000000000000UL;
            ip++;
            break;
        case OP_FSQRT64:
            RD.d = mc_sqrt(RA.d);
            ip++;
            break;
        case OP_FGEN:
            RD.u = mc_fgen(ip->aux, (u32)ip->e, RA, RB, RC);
            ip++;
            break;
        case OP_TESTP:
            RD.u = testp(ip->aux, RA);
            ip++;
            break;

        case OP_CVT_F32_S32:
            SETF((float)RA.i);
            ip++;
            break;
        case OP_CVT_F32_U32:
            SETF((float)RA.w);
            ip++;
            break;
        case OP_CVT_F64_S32:
            RD.d = (double)RA.i;
            ip++;
            break;
        case OP_CVT_F64_U32:
            RD.d = (double)RA.w;
            ip++;
            break;
        case OP_CVT_F32_S64:
            SETF((float)RA.s);
            ip++;
            break;
        case OP_CVT_F32_U64:
            SETF((float)RA.u);
            ip++;
            break;
        case OP_CVT_F64_S64:
            RD.d = (double)RA.s;
            ip++;
            break;
        case OP_CVT_F64_U64:
            RD.d = (double)RA.u;
            ip++;
            break;
        case OP_CVT_S32_F32Z:
            RD.u = f2s32((double)RA.f, 0);
            ip++;
            break;
        case OP_CVT_U32_F32Z:
            RD.u = f2u32((double)RA.f);
            ip++;
            break;
        case OP_CVT_S32_F64Z:
            RD.u = f2s32(RA.d, 0x80000000U);
            ip++;
            break;
        case OP_CVT_S64_F32Z:
            RD.u = f2s64((double)RA.f, 0x8000000000000000UL);
            ip++;
            break;
        case OP_CVT_S64_F64Z:
            RD.u = f2s64(RA.d, 0x8000000000000000UL);
            ip++;
            break;
        case OP_CVT_F64_F32:
            RD.d = (double)RA.f;
            ip++;
            break;
        case OP_CVT_F32_F64:
            SETF((float)RA.d);
            ip++;
            break;
        case OP_CVT_F32_F16:
            SETF(mc_h2f_tab[RA.w & 0xffff]);
            ip++;
            break;
        case OP_CVT_F16_F32:
            RD.u = mc_enc(&mc_fmt_f16, (double)RA.f, MC_RN, 0);
            ip++;
            break;
        case OP_CVT:
            RD.u = mc_cvt((u32)ip->e, RA.u, RB.u, 0);
            ip++;
            break;

        case OP_LD8:
            ea = EA;
            CHK(ea);
            RD.u = *(u8 *)ea;
            ip++;
            break;
        case OP_LD8S32:
            ea = EA;
            CHK(ea);
            RD.u = (u32)(s32) * (s8 *)ea;
            ip++;
            break;
        case OP_LD8S64:
            ea = EA;
            CHK(ea);
            RD.s = *(s8 *)ea;
            ip++;
            break;
        case OP_LD16:
            ea = EA;
            CHK(ea);
            RD.u = *(u16 *)ea;
            ip++;
            break;
        case OP_LD16S32:
            ea = EA;
            CHK(ea);
            RD.u = (u32)(s32) * (s16 *)ea;
            ip++;
            break;
        case OP_LD16S64:
            ea = EA;
            CHK(ea);
            RD.s = *(s16 *)ea;
            ip++;
            break;
        case OP_LD32:
            ea = EA;
            CHK(ea);
            RD.u = *(u32 *)ea;
            ip++;
            break;
        case OP_LD32S64:
            ea = EA;
            CHK(ea);
            RD.s = *(s32 *)ea;
            ip++;
            break;
        case OP_LD64:
            ea = EA;
            CHK(ea);
            RD.u = *(u64 *)ea;
            ip++;
            break;
        case OP_ST8:
            ea = EA;
            CHK(ea);
            *(u8 *)ea = (u8)RB.w;
            ip++;
            break;
        case OP_ST16:
            ea = EA;
            CHK(ea);
            *(u16 *)ea = (u16)RB.w;
            ip++;
            break;
        case OP_ST32:
            ea = EA;
            CHK(ea);
            *(u32 *)ea = RB.w;
            ip++;
            break;
        case OP_ST64:
            ea = EA;
            CHK(ea);
            *(u64 *)ea = RB.u;
            ip++;
            break;
        case OP_LD8_S:
            ea = EA;
            SEA(ea);
            RD.u = *(u8 *)ea;
            ip++;
            break;
        case OP_LD8S32_S:
            ea = EA;
            SEA(ea);
            RD.u = (u32)(s32) * (s8 *)ea;
            ip++;
            break;
        case OP_LD8S64_S:
            ea = EA;
            SEA(ea);
            RD.s = *(s8 *)ea;
            ip++;
            break;
        case OP_LD16_S:
            ea = EA;
            SEA(ea);
            RD.u = *(u16 *)ea;
            ip++;
            break;
        case OP_LD16S32_S:
            ea = EA;
            SEA(ea);
            RD.u = (u32)(s32) * (s16 *)ea;
            ip++;
            break;
        case OP_LD16S64_S:
            ea = EA;
            SEA(ea);
            RD.s = *(s16 *)ea;
            ip++;
            break;
        case OP_LD32_S:
            ea = EA;
            SEA(ea);
            RD.u = *(u32 *)ea;
            ip++;
            break;
        case OP_LD32S64_S:
            ea = EA;
            SEA(ea);
            RD.s = *(s32 *)ea;
            ip++;
            break;
        case OP_LD64_S:
            ea = EA;
            SEA(ea);
            RD.u = *(u64 *)ea;
            ip++;
            break;
        case OP_ST8_S:
            ea = EA;
            SEA(ea);
            *(u8 *)ea = (u8)RB.w;
            ip++;
            break;
        case OP_ST16_S:
            ea = EA;
            SEA(ea);
            *(u16 *)ea = (u16)RB.w;
            ip++;
            break;
        case OP_ST32_S:
            ea = EA;
            SEA(ea);
            *(u32 *)ea = RB.w;
            ip++;
            break;
        case OP_ST64_S:
            ea = EA;
            SEA(ea);
            *(u64 *)ea = RB.u;
            ip++;
            break;
        case OP_LDV:
        case OP_LDV_S: {
            static const u8 esz[9] = {1, 1, 1, 2, 2, 2, 4, 4, 8};
            u32 i, vn = ip->aux & 15, k = ip->aux >> 4;
            s32 *xs = t->f->xtra + ip->e;
            Slot v[4];
            ea = EA;
            if (ip->op == OP_LDV_S) {
                SEA(ea);
            } else
                CHK(ea);
            for (i = 0; i < vn; i++)
                v[i].u = ld_elem(k, ea + i * esz[k]);
            for (i = 0; i < vn; i++)
                R[xs[i]].u = v[i].u;
            ip++;
            break;
        }
        case OP_STV:
        case OP_STV_S: {
            u32 i, vn = ip->aux & 15, k = ip->aux >> 4;
            s32 *xs = t->f->xtra + ip->e;
            ea = EA;
            if (ip->op == OP_STV_S) {
                SEA(ea);
            } else
                CHK(ea);
            for (i = 0; i < vn; i++) {
                u64 v = R[xs[i]].u;
                switch (k) {
                case 0:
                    *(u8 *)(ea + i) = (u8)v;
                    break;
                case 1:
                    *(u16 *)(ea + 2 * i) = (u16)v;
                    break;
                case 2:
                    *(u32 *)(ea + 4 * i) = (u32)v;
                    break;
                default:
                    *(u64 *)(ea + 8 * i) = v;
                    break;
                }
            }
            ip++;
            break;
        }
        case OP_ATOM:
            ea = EA;
            CHK(ea);
            RD.u = mc_atom(b, ip->aux, (u8 *)ea, RB.u, RE.u);
            ip++;
            break;
        case OP_ATOM_S:
            ea = EA;
            SEA(ea);
            RD.u = mc_atom(b, ip->aux, (u8 *)ea, RB.u, RE.u);
            ip++;
            break;
        case OP_CPASYNC: {
            s32 *xs = t->f->xtra + ip->e;
            u64 src = RB.u + (u64)(s64)ip->c, size = R[xs[0]].w, ssz = xs[1] == NOSLOT ? size : R[xs[1]].w;
            ea = RA.u + (u64)(s64)ip->d;
            SEA(ea);
            if (ssz > size)
                ssz = size;
            if (ssz) {
                if (src < 4096) {
                    ea = src;
                    goto badaddr;
                }
                memcpy((u8 *)ea, (u8 *)src, ssz);
            }
            if (ssz < size)
                memset((u8 *)ea + ssz, 0, size - ssz);
            ip++;
            break;
        }
        case OP_CVTA_TO_SHARED:
            ea = RA.u;
            RD.u = ea - (u64)sh < shsz ? ea - (u64)sh : ea;
            ip++;
            break;
        case OP_CVTA_SHARED:
            ea = RA.u;
            RD.u = ea < shsz ? ea + (u64)sh : ea;
            ip++;
            break;
        case OP_ISSPACEP: {
            /* local: this thread's frames; shared: the block's; global: whatever else (constants and parameters too) */
            McFunc *ef = b->L->f;
            u8 *fr = (u8 *)t->R0 - (u64)ef->nconst * 8;
            int is_sh, is_lo;
            ea = RA.u;
            is_sh = ea < shsz || ea - (u64)sh < shsz;
            is_lo = !is_sh && (ea - (u64)fr < ef->frame_bytes || (t->stk && ea - (u64)t->stk < (u64)(t->stk_end - t->stk)));
            if (ip->aux == SP_SHARED)
                RD.u = (u64)is_sh;
            else if (ip->aux == SP_LOCAL)
                RD.u = (u64)is_lo;
            else if (ip->aux == SP_GLOBAL)
                RD.u = (u64)(!is_sh && !is_lo);
            else
                RD.u = 0;
            ip++;
            break;
        }
        default:
            t->ip = ip;
            mc_fault(b, t, "internal error: bad opcode%s %lu", 0, ip->op);
            goto fault;
        }
    }
badaddr:
    t->ip = ip;
    mc_fault(b, t, "memory access%s at the bad address 0x%lx", 0, ea);
fault:
    ret = RUN_FAULT;
    t->R = R;
    b->icount += n;
    return ret;
out:
    t->ip = ip;
    t->R = R;
    b->icount += n;
    return ret;
}

/* ---- a block ---- */

static const char *ws_name(Ins *ip)
{
    static const char *const names[] = {"bar.warp.sync", "shfl.sync", "shfl.sync", "shfl.sync", "shfl.sync", "vote.sync", "vote.sync", "vote.sync",
                                        "vote.sync", "match.sync", "match.sync", "match.sync", "match.sync", "redux.sync", "mma.sync",
                                        "ldmatrix.sync", "activemask"};
    return ip->op == OP_WSYNC ? names[ip->aux] : "an instruction";
}

static u32 ws_mask(Thr *t)
{
    s32 ms = t->f->xtra[t->ip->e];
    return ms == NOSLOT ? 0xffffffffU : t->R[ms].w;
}

/* Run the threads of one warp as far as they go. 1: something ran; 0: nothing could; -1: a fault. */
static int run_warp(Blk *b, u32 warp)
{
    u32 base = warp * 32, nl = b->nthr - base < 32 ? b->nthr - base : 32, i, j;
    int any = 0, prog;
    Thr *grp[32];
    Thr *all[32];
    McJitCtx jc;
    jc.sh = b->shared;
    jc.shsz = b->shared_size;
    jc.h2f = mc_h2f_tab;
    jc.blk = b;
    for (i = 0; i < 32; i++)
        all[i] = i < nl ? &b->thr[base + i] : 0;
    do {
        u32 nws = 0; /* threads that came, in this round, to the warp instruction at0 */
        Ins *at0 = 0;
        prog = 0;
        for (i = 0; i < nl; i++) {
            Thr *t = &b->thr[base + i];
            McJit *jf;
            if (t->state != TS_RUN)
                continue;
            prog = 1;
            if ((jf = t->f->jit) != 0) {
                /* the usual course in a kernel of warp instructions, without run(): translated code from
                   one of them to the next */
                u32 at = jf->off[t->ip - t->f->code];
                if (!(at & MC_JIT_NOENTER)) {
                    Ins *ip;
                    u32 k, cnt;
                    /* the threads after this one that are at the same place go along: each runs to its next
                       warp instruction, one after the other as they would here */
                    jc.R[0] = t->R;
                    jc.thrs[0] = t;
                    for (k = i + 1; k < nl; k++) {
                        Thr *o = &b->thr[base + k];
                        if (o->state != TS_RUN || o->ip != t->ip || o->f != t->f)
                            break;
                        jc.R[k - i] = o->R;
                        jc.thrs[k - i] = o;
                    }
                    cnt = k - i;
                    jc.lane = 0;
                    jc.nl = cnt;
                    jc.thr = t;
                    at = mc_jit_enter(jf, t->R, &jc, at);
                    b->icount += jc.n + jc.lane;
                    for (k = 0; k < jc.lane; k++) {
                        Thr *o = &b->thr[base + i + k];
                        ip = o->f->code + jc.ex[k];
                        o->ip = ip;
                        o->state = TS_WSYNC;
                        if (!nws)
                            at0 = ip;
                        nws += ip == at0;
                    }
                    if (mc_stats_on)
                        jit_exits[OP_WSYNC] += jc.lane;
                    i += (u32)jc.lane;
                    if (jc.lane == cnt) {
                        i--;
                        continue;
                    }
                    /* this one left for another reason */
                    t = &b->thr[base + i];
                    if (at & MC_JIT_FAULT) {
                        b->icount++;
                        return -1;
                    }
                    ip = t->f->code + at;
                    t->ip = ip;
                    if (mc_stats_on)
                        jit_exits[ip->op < OP_COUNT ? ip->op : 0]++;
                }
            }
            switch (run(b, t)) {
            case RUN_FAULT:
                return -1;
            case RUN_EXIT:
                t->state = TS_EXIT;
                b->live--;
                bar_recheck(b);
                break;
            }
        }
        if (nws == 32 && ws_mask(all[0]) == 0xffffffffU) {
            /* a whole warp at one instruction, as good as always: no need to look who is where */
            b->cur = all[0];
            if (mc_wsync(b, warp, all, 0xffffffffU, 32) < 0)
                return -1;
            for (j = 0; j < 32; j++) {
                all[j]->ip++;
                all[j]->state = TS_RUN;
            }
            any = 1;
            continue;
        }
        for (i = 0; i < nl; i++) {
            Thr *t = &b->thr[base + i];
            u32 mask, live, n = 0;
            if (t->state != TS_WSYNC)
                continue;
            if (t->ip->aux == WS_ACTIVEMASK) {
                /* activemask: the threads that are at this instruction when none of the warp runs any more.
                   (On a device the threads of a warp go in step, and those that execute an instruction
                   together are its active ones.) */
                mask = 0;
                for (j = 0; j < nl; j++) {
                    Thr *o = &b->thr[base + j];
                    grp[j] = 0;
                    if (o->state == TS_RUN)
                        break;
                    if (o->state == TS_WSYNC && o->ip == t->ip) {
                        grp[j] = o;
                        mask |= 1U << j;
                        n++;
                    }
                }
                if (j < nl)
                    continue; /* someone may still come */
                for (; j < 32; j++)
                    grp[j] = 0;
                b->cur = t;
                if (mc_wsync(b, warp, grp, mask, n) < 0)
                    return -1;
                for (j = 0; j < nl; j++)
                    if (grp[j]) {
                        grp[j]->ip++;
                        grp[j]->state = TS_RUN;
                    }
                prog = 1;
                continue;
            }
            live = live_mask(b, warp);
            mask = (ws_mask(t) | 1U << i) & live;
            for (j = 0; j < nl; j++) {
                Thr *o = &b->thr[base + j];
                grp[j] = 0;
                if (!(mask >> j & 1))
                    continue;
                if (o->state != TS_WSYNC || o->ip->aux != t->ip->aux)
                    break;
                grp[j] = o;
                n++;
            }
            for (; j < 32 && j >= nl; j++)
                grp[j] = 0;
            if (j < nl)
                continue; /* someone of the group is not there yet */
            b->cur = t;
            if (mc_wsync(b, warp, grp, mask, n) < 0)
                return -1;
            for (j = 0; j < nl; j++)
                if (grp[j]) {
                    grp[j]->ip++;
                    grp[j]->state = TS_RUN;
                }
            prog = 1;
        }
        any |= prog;
    } while (prog);
    return any;
}

static void deadlock(Blk *b)
{
    u32 i, shown = 0, waiting = 0;
    char why[300];
    Thr *first = 0;
    why[0] = 0;
    for (i = 0; i < b->nthr; i++) {
        Thr *t = &b->thr[i];
        Ins *at;
        if (t->state != TS_BAR && t->state != TS_WSYNC)
            continue;
        waiting++;
        if (shown >= 3)
            continue;
        at = t->state == TS_BAR ? t->ip - 1 : t->ip;
        if (!first)
            first = t;
        sprintf(why + strlen(why), "%sthread (%u,%u,%u) waits at %s on PTX line %u", shown ? "; " : "", t->xyz[0], t->xyz[1], t->xyz[2],
                t->state == TS_BAR ? "a barrier" : ws_name(at), t->f->lines[at - t->f->code]);
        shown++;
    }
    if (first && first->state == TS_BAR)
        first->ip--;
    mc_fault(b, first, "deadlock: %s; %lu threads wait in all, the first", why, (u64)waiting);
}

static int run_block(Blk *b, int sync)
{
    McFunc *f = b->L->f;
    u32 i, w;
    b->live = b->nthr;
    memset(b->bars, 0, sizeof b->bars);
    if (b->shared_size)
        memset(b->shared, 0, b->shared_size);
    for (i = 0; i < b->nthr; i++) {
        Thr *t = &b->thr[i];
        t->ip = f->code;
        t->R = t->R0;
        t->f = f;
        t->state = TS_RUN;
        t->ncs = 0;
        t->cf = 0;
        if (t->stk)
            t->sp = (u8 *)(((u64)t->stk + 15) & ~15UL);
        apply_fix(b, t, f, t->R0);
    }
    if (!sync) {
        for (i = 0; i < b->nthr; i++)
            if (run(b, &b->thr[i]) == RUN_FAULT)
                return -1;
        return 0;
    }
    while (b->live) {
        int any = 0, r;
        for (w = 0; w < b->nwarp; w++) {
            r = run_warp(b, w);
            if (r < 0)
                return -1;
            any |= r;
        }
        if (!any && b->live) {
            deadlock(b);
            return -1;
        }
    }
    return 0;
}

/* Does f or anything it may call wait for other threads? */
static int uses_sync(McFunc *f, u32 stamp)
{
    u32 i;
    McFunc *g;
    if (f->stamp == stamp)
        return 0;
    f->stamp = stamp;
    if (f->uses_sync)
        return 1;
    for (i = 0; i < f->ncallees; i++)
        if (uses_sync(f->callees[i], stamp))
            return 1;
    for (g = f->mod->funcs; g; g = g->next)
        if (g->addr_taken && g->decoded && uses_sync(g, stamp))
            return 1;
    return 0;
}

static u32 stamps;
static u64 total_instructions;

u64 mc_instructions(void) { return total_instructions; }

/* The buffers of a host thread, kept from one launch to the next. */
static struct Worker {
    u8 *frames;
    u64 frames_cap;
    u8 *shared;
    u64 shared_cap;
    Thr *thr;
    u32 thr_cap;
} workers[MC_MAX_WORKERS];

/* A launch that the host threads share: each takes blocks by number until none is left or one has failed. */
typedef struct Job {
    Launch *L;
    int sync, rc;
    u32 nthr;
    u64 next, total, chunk, icount, wsync_n, wsync_usec;
    char msg[900];
} Job;

static void job_fail(Job *j, int rc, const char *msg)
{
    mcuda_lock(MC_LOCK_JOB);
    if (!j->rc) {
        j->rc = rc;
        strncpy(j->msg, msg, sizeof j->msg - 1);
    }
    mcuda_unlock(MC_LOCK_JOB);
}

static void job_run(void *arg, int w)
{
    Job *j = (Job *)arg;
    Launch *L = j->L;
    McFunc *f = L->f;
    struct Worker *wk = &workers[w];
    Blk *b;
    u32 nthr = j->nthr, i;
    u64 fb, need, k, end;
    b = (Blk *)calloc(1, sizeof(Blk));
    fb = (f->frame_bytes + 15UL) & ~15UL;
    need = fb * nthr + 64;
    if (wk->frames_cap < need) {
        free(wk->frames);
        wk->frames = (u8 *)malloc(need);
        wk->frames_cap = wk->frames ? need : 0;
    }
    need = L->shared_bytes + 256;
    if (wk->shared_cap < need) {
        free(wk->shared);
        wk->shared = (u8 *)malloc(need);
        wk->shared_cap = wk->shared ? need : 0;
    }
    if (wk->thr_cap < nthr) {
        wk->thr = (Thr *)realloc(wk->thr, nthr * sizeof(Thr));
        if (wk->thr)
            memset(wk->thr + wk->thr_cap, 0, (nthr - wk->thr_cap) * sizeof(Thr));
        wk->thr_cap = wk->thr ? nthr : 0;
    }
    if (!b || !wk->frames || !wk->shared || !wk->thr) {
        free(b);
        job_fail(j, 2, "mc_launch: out of memory");
        return;
    }
    b->L = L;
    b->thr = wk->thr;
    b->nthr = nthr;
    b->nwarp = (nthr + 31) / 32;
    b->frames = (u8 *)(((u64)wk->frames + 15) & ~15UL);
    b->shared = (u8 *)(((u64)wk->shared + 127) & ~127UL);
    b->shared_size = L->shared_bytes;
    for (i = 0; i < nthr; i++) {
        Thr *t = &b->thr[i];
        u8 *base = b->frames + fb * i;
        memcpy(base, f->ctemplate, (size_t)f->nconst * 8);
        t->R0 = (Slot *)base + f->nconst;
        t->tid = i;
        t->lane = i & 31;
        t->warp = i >> 5;
        t->xyz[0] = i % L->block[0];
        t->xyz[1] = i / L->block[0] % L->block[1];
        t->xyz[2] = i / L->block[0] / L->block[1];
    }
    for (;;) {
        mcuda_lock(MC_LOCK_JOB);
        k = j->next;
        end = j->rc ? k : k + j->chunk > j->total ? j->total : k + j->chunk;
        j->next = end;
        mcuda_unlock(MC_LOCK_JOB);
        if (k >= end)
            break;
        for (; k < end; k++) {
            b->cta[0] = (u32)(k % L->grid[0]);
            b->cta[1] = (u32)(k / L->grid[0] % L->grid[1]);
            b->cta[2] = (u32)(k / L->grid[0] / L->grid[1]);
            if (run_block(b, j->sync) < 0) {
                job_fail(j, b->failed == 2 ? 218 : 719, b->msg);
                break;
            }
        }
    }
    mcuda_lock(MC_LOCK_JOB);
    j->icount += b->icount;
    j->wsync_n += b->wsync_n;
    j->wsync_usec += b->wsync_usec;
    mcuda_unlock(MC_LOCK_JOB);
    for (i = 0; i < nthr; i++) {
        Thr *t = &b->thr[i];
        free(t->stk);
        t->stk = 0;
        t->sp = t->stk_end = 0;
    }
    free(b);
}

/* MCUDA_STATS=1 (or a number of lines): when the program ends, the kernels that took the most time. */
static int stats_on = -1;

static u64 now_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (u64)tv.tv_sec * 1000000UL + (u64)tv.tv_usec;
}

static void stats_print(void)
{
    McModule *m;
    McFunc *f, *best;
    u64 tl = 0, tb = 0, ti = 0, tu = 0, last = ~0UL;
    int k, lines = stats_on > 1 ? stats_on : 20;
    for (m = mc_modules(); m; m = m->next)
        for (f = m->funcs; f; f = f->next) {
            tl += f->st_launches;
            tb += f->st_blocks;
            ti += f->st_instr;
            tu += f->st_usec;
        }
    fprintf(stderr, "mcuda: %lu launches, %lu blocks, %lu PTX instructions, %.2f s in kernels, %d host threads\n",
            (unsigned long)tl, (unsigned long)tb, (unsigned long)ti, (double)tu / 1e6, mcuda_nworkers());
    for (k = 0; k < lines; k++) {
        best = 0;
        for (m = mc_modules(); m; m = m->next)
            for (f = m->funcs; f; f = f->next)
                if (f->st_launches && f->st_usec <= last && (!best || f->st_usec > best->st_usec) &&
                    !(f->st_usec == last && f->st_launches >> 63))
                    best = f;
        if (!best)
            break;
        last = best->st_usec;
        fprintf(stderr, "mcuda: %8.2f s %8lu launches %12lu instructions %10lu warp operations (%.2f thread-s)  %.90s\n",
                (double)best->st_usec / 1e6, (unsigned long)best->st_launches, (unsigned long)best->st_instr,
                (unsigned long)best->st_wsync, (double)best->st_wusec / 1e6, best->name);
        best->st_launches |= 1UL << 63; /* printed */
    }
    for (k = 0; k < 8; k++) {
        u32 i, bi = 0;
        for (i = 1; i < OP_COUNT; i++)
            if (jit_exits[i] > jit_exits[bi])
                bi = i;
        if (!jit_exits[bi])
            break;
        fprintf(stderr, "mcuda: translated code left %lu times at instructions of kind %u\n", (unsigned long)jit_exits[bi], bi);
        jit_exits[bi] = 0;
    }
}

static void stats_signal(int sig)
{
    stats_print(); /* a program that is stopped for taking too long is the one whose numbers are wanted */
    _exit(128 + sig);
}

int mc_launch_buf(McFunc *f, const u32 grid[3], const u32 block[3], u64 shared_bytes, const void *params)
{
    u64 t0 = 0;
    Launch L;
    Job *j;
    u32 i;
    int rc, nw;
    if (!f || !grid || !block || !grid[0] || !grid[1] || !grid[2] || !block[0] || !block[1] || !block[2]) {
        mc_set_error("mc_launch: a grid or block dimension is zero");
        return 1;
    }
    if ((u64)block[0] * block[1] * block[2] > MC_DEV_MAX_THREADS) {
        mc_set_error("mc_launch: more than 1024 threads in a block");
        return 1;
    }
    if (!f->is_entry) {
        mc_set_error("mc_launch: not a kernel (.entry)");
        return 1;
    }
    j = (Job *)calloc(1, sizeof(Job));
    if (!j) {
        mc_set_error("mc_launch: out of memory");
        return 2;
    }
    mcuda_lock(MC_LOCK_MODULE);
    if (!f->decoded && mc_func_decode(f) < 0) {
        mcuda_unlock(MC_LOCK_MODULE);
        fprintf(stderr, "mcuda: %s\n", mc_error());
        free(j);
        return 218;
    }
    j->sync = uses_sync(f, ++stamps);
    if (stats_on < 0) {
        const char *e = getenv("MCUDA_STATS");
        stats_on = e ? atoi(e) : 0;
        mc_stats_on = stats_on;
        if (stats_on) {
            atexit(stats_print);
            signal(SIGTERM, stats_signal);
            signal(SIGINT, stats_signal);
        }
    }
    mcuda_unlock(MC_LOCK_MODULE);
    if (stats_on)
        t0 = now_usec();
    L.f = f;
    for (i = 0; i < 3; i++) {
        L.grid[i] = grid[i];
        L.block[i] = block[i];
    }
    L.dyn_shared = shared_bytes;
    L.shared_bytes = f->mod->shared_static + shared_bytes;
    L.params = (u8 *)params;
    j->L = &L;
    j->nthr = block[0] * block[1] * block[2];
    j->total = (u64)grid[0] * grid[1] * grid[2];
    /* threads take the blocks a few at a time; a small launch is not worth waking anyone for */
    nw = mcuda_nworkers();
    if (nw > MC_MAX_WORKERS)
        nw = MC_MAX_WORKERS;
    if ((u64)nw > j->total)
        nw = (int)j->total;
    if (j->total * j->nthr < 256)
        nw = 1;
    j->chunk = j->total / ((u64)nw * 8);
    if (!j->chunk)
        j->chunk = 1;
    mcuda_par(nw, job_run, j);
    rc = j->rc;
    if (rc) {
        if (rc == 719 || rc == 218)
            fprintf(stderr, "%s\n", j->msg);
        mc_set_error(j->msg);
    }
    if (rc == 218) {
        /* a call of a function nothing defines: a build error, not a fault of this launch's data (as mcudahw's load_fatal: the GPU's run
           time stops at the first launch too). MCUDA_LOAD_SOFT=1: return the error instead */
        const char *v = getenv("MCUDA_LOAD_SOFT");
        if (!(v && v[0] && v[0] != '0')) {
            fprintf(stderr, "mcuda: fatal: kernel %s calls a function nothing defines; stopping (MCUDA_LOAD_SOFT=1: return the error)\n", f->name);
            fflush(stderr);
            abort();
        }
        fprintf(stderr, "mcuda: warning: MCUDA_LOAD_SOFT: the launch returns the error\n");
        rc = 719; /* (what it returned before: a fault of the launch) */
    }
    mcuda_lock(MC_LOCK_MODULE);
    total_instructions += j->icount;
    if (stats_on) {
        f->st_launches++;
        f->st_blocks += j->total;
        f->st_instr += j->icount;
        f->st_wsync += j->wsync_n;
        f->st_wusec += (u64)((double)j->wsync_usec * 1e6 / (double)mc_tickhz());
        f->st_usec += now_usec() - t0;
    }
    mcuda_unlock(MC_LOCK_MODULE);
    free(j);
    return rc;
}

int mc_launch(McFunc *f, const u32 grid[3], const u32 block[3], u64 shared_bytes, void **args)
{
    u8 small[512], *buf = small;
    int i, rc;
    if (!f) {
        mc_set_error("mc_launch: no kernel");
        return 1;
    }
    if (f->param_bytes > sizeof small - 16) {
        buf = (u8 *)malloc(f->param_bytes + 16);
        if (!buf)
            return 2;
    }
    if (f->nparams && !args) {
        mc_set_error("mc_launch: no arguments for a kernel that takes some");
        return 1;
    }
    for (i = 0; i < f->nparams; i++)
        memcpy(buf + f->params[i].off, args[i], f->params[i].size);
    rc = mc_launch_buf(f, grid, block, shared_bytes, buf);
    if (buf != small)
        free(buf);
    return rc;
}
