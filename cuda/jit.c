/* A translator from decoded PTX instructions to arm64 code, so that a kernel's straight-line work does not go
   through the interpreter's loop. One template per instruction: operands are loaded from the thread's frame
   (x19 points at it, as R does in ptxexec.c), the result is stored back. Whatever has no template here, or
   needs the scheduler (calls, barriers, the warp-synchronous instructions), leaves the code with the index
   of that instruction, which the interpreter then executes before it enters the code again; so do the rare
   cases of a translated instruction (a bad address, a NaN in min and max and in a conversion to f16).
   What an instruction computes is, bit for bit, what the interpreter computes for it.

   Registers: x19 the frame, x20 and x21 the block's shared memory and its size, x22 instructions executed,
   x23 the McJitCtx, x24 mc_h2f_tab; x0..x3, x16 and v0..v2 are scratch. x4..x15 and v3..v7, v16..v23 hold
   copies of slots: the register that computed a result is remembered as a copy of that slot, so that later
   instructions need not load it. At the end of a forward branch what is remembered is what both ways there
   agree on; where the interpreter may enter, nothing.

   Three passes over the instructions. The first finds what each reads and sets, and from that what values
   are wanted after their basic block (liveness()): only those are stored in the frame when they are computed.
   The others stay in their register, and go to the frame if the register is needed for something else
   before their last use, or on the way out to the interpreter (which only ever reads what is wanted). The
   second pass finds what the registers hold at the end of each loop; the third, whose code is kept, begins
   a loop by loading just that, and its branches back keep it in registers (jump_back()).
   MCUDA_JIT=0 turns the translator off; =2 says what was translated. */
#include "ptx.h"

#if defined(__linux__) && defined(__aarch64__)
#include <sys/mman.h>
#define MC_JIT 1
void mc_flush_icache(void *from, void *to); /* fp_linux.s */
#elif defined(MC_JIT_DARWIN)
#include <sys/mman.h>
#define MC_JIT 1
void pthread_jit_write_protect_np(int on);
void sys_icache_invalidate(void *start, size_t len);
#endif

#ifndef MC_JIT
void mc_jit_compile(McFunc *f) { (void)f; }
void mc_jit_free(McFunc *f) { (void)f; }
#else

typedef struct Stub {
    u32 at;   /* the conditional branch that goes to it */
    u32 ins;  /* the instruction the interpreter is to do */
    u32 pend; /* instructions executed and not yet counted */
    u32 fl, nfl; /* what to store first: j->fl[fl .. fl + nfl) */
} Stub;

/* a register whose value is to be stored in its slot */
typedef struct Fl {
    s32 slot;
    u8 reg, isv;
} Fl;

typedef struct Fixup {
    u32 at;
    s32 target; /* instruction index */
    int body;   /* to the loop's body, past the loads at its head */
} Fixup;

#define NOSL 0x7fffffff
#define INF 0x7fffffff
/* what the registers hold: xs[r] is the slot that x<r> is a copy of (xk: 1 the low 32 bits only, 2 all of it;
   xz: the high 32 bits of the register are zero), vs, vk and vz the same for d<r> */
typedef struct Cache {
    s32 xs[16], vs[24];
    u8 xk[16], vk[24];
    u8 xz[16], vz[24];
} Cache;

typedef struct Snap {
    Cache c;
    s32 next;
} Snap;

typedef struct J {
    McFunc *f;
    u32 *w;
    u32 n, cap;
    int fail;
    u32 *off;
    u8 *lead;  /* 1: a branch goes there; 2: nothing is known about the registers there, and the interpreter may enter */
    u8 *fixed; /* fixed[i]: the kind of fix of the constant slot -1 - i, which is filled in per thread */
    s32 *snhead; /* per instruction: the states of the forward branches that go there */
    Snap *sn;
    u32 nsn, capsn;
    Stub *st;
    u32 nst, capst;
    Fixup *fx;
    u32 nfx, capfx;
    Fl *fl;
    u32 nfl, capfl;
    /* Loops. bst[bhead[i]], for an instruction i that is branched back to: what the registers held at the
       branch back in the pass before and is wanted at i. The code at i begins with loading just that, for who
       comes from elsewhere, and the branches back see to it that the registers hold it and go to the code
       after the loads, at boff[i]: so what a loop uses stays in registers around it. (Pass 1 has no such
       state and sees what pass 2 gets; pass 2, which knows what values are wanted how long and so keeps other
       registers, sees what pass 3 gets, whose code it is that runs.) bnew is what the present pass sees. */
    s32 *bhead;
    Cache *bst, *bnew;
    u8 *bseen;
    u32 nbst, capbst;
    u32 *boff;
    u64 *lv_lin; /* from liveness(): per block, what is wanted at its start; lv_bof: the block of an instruction */
    u32 *lv_bof;
    u32 lv_W;
    /* What is known of the instructions, found by a first pass over them (pass 1) and used by the second:
       us[ustart[i] .. ustart[i + 1]) are the slots the instruction i reads, defk[i] the slot it sets (all 64 bits
       of it; NOSL if none, or if it is not known to be all), iall[i] says that it may read anything. From that:
       last[i], of the value the instruction i computes: the last instruction of its basic block that reads it,
       INF if it may be read after the block, -1 if never; adead[i]: the slot ip->a is not read again. */
    int pass;
    u32 *ustart;
    s32 *us;
    u32 nus, capus;
    s32 *defk;
    u8 *iall;
    s32 *last;
    u8 *adead;
    s32 rdef;
    int rmulti;
    u32 pend;
    u32 exit_at, wexit_at;
    u32 cur;
    Cache c;
    /* xd[r]: the value of the slot xs[r] is in x<r> only, not in the frame; xl[r] is then the last use of it */
    u8 xd[16], vd[24];
    s32 xl[16], vl[24];
    u32 xage[16], vage[24];
    u32 xpin, vpin, tick;
    int dead; /* control does not come here from the instruction before */
    u32 stflag; /* MC_JIT_FAULT for the stub that is made next */
} J;

enum { EQ, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE };

static void emit(J *j, u32 x)
{
    if (j->n == j->cap) {
        u32 nc = j->cap ? j->cap * 2 : 1024;
        u32 *nw = (u32 *)realloc(j->w, (size_t)nc * 4);
        if (!nw) {
            j->fail = 1;
            return;
        }
        j->w = nw;
        j->cap = nc;
    }
    j->w[j->n++] = x;
}

static void movi(J *j, int r, u64 v)
{
    int i, nz = 0, nf = 0, first = 1;
    for (i = 0; i < 4; i++) {
        u32 c = (u32)(v >> (16 * i)) & 0xffff;
        nz += c == 0;
        nf += c == 0xffff;
    }
    if (nf > nz) {
        for (i = 0; i < 4; i++) {
            u32 c = (u32)(v >> (16 * i)) & 0xffff;
            if (c == 0xffff)
                continue;
            if (first)
                emit(j, 0x92800000U | (u32)i << 21 | (~c & 0xffff) << 5 | (u32)r);
            else
                emit(j, 0xf2800000U | (u32)i << 21 | c << 5 | (u32)r);
            first = 0;
        }
        if (first)
            emit(j, 0x92800000U | (u32)r); /* all ones */
        return;
    }
    for (i = 0; i < 4; i++) {
        u32 c = (u32)(v >> (16 * i)) & 0xffff;
        if (c == 0)
            continue;
        if (first)
            emit(j, 0xd2800000U | (u32)i << 21 | c << 5 | (u32)r);
        else
            emit(j, 0xf2800000U | (u32)i << 21 | c << 5 | (u32)r);
        first = 0;
    }
    if (first)
        emit(j, 0xd2800000U | (u32)r);
}

/* a constant whose value is known now */
static int kc(J *j, s32 slot, u64 *v)
{
    McFunc *f = j->f;
    if (slot >= 0 || (u32)(-1 - slot) >= f->nconst || j->fixed[-1 - slot])
        return 0;
    *v = f->ctemplate[(s32)f->nconst + slot].u;
    return 1;
}

/* a load or store of a slot: op is the instruction with a scaled offset, sz the log of the size */
static void slot_op(J *j, u32 op, int sz, int r, s32 slot)
{
    s64 o = (s64)slot * 8;
    if (o >= 0 && (o >> sz) < 4096)
        emit(j, op | (u32)(o >> sz) << 10 | 19 << 5 | (u32)r);
    else if (o >= -256 && o < 0)
        emit(j, (op & ~0x01000000U) | ((u32)o & 0x1ff) << 12 | 19 << 5 | (u32)r);
    else {
        movi(j, 16, (u64)o);
        emit(j, (op & ~0x01000000U) | 0x00206800U | 16 << 16 | 19 << 5 | (u32)r);
    }
}

/* ---- the registers that hold copies of slots ---- */

static const u8 xpool[12] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const u8 vpool[13] = {3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};

static void forget_c(Cache *c)
{
    int i;
    for (i = 0; i < 16; i++)
        c->xs[i] = NOSL;
    for (i = 0; i < 24; i++)
        c->vs[i] = NOSL;
}

static void forget(J *j)
{
    forget_c(&j->c);
}

/* keep of the state c what the state s agrees with */
static void meetc(Cache *c, Cache *s)
{
    int i;
    for (i = 0; i < 16; i++)
        if (c->xs[i] != s->xs[i])
            c->xs[i] = NOSL;
        else {
            if (s->xk[i] < c->xk[i])
                c->xk[i] = s->xk[i];
            c->xz[i] &= s->xz[i];
        }
    for (i = 0; i < 24; i++)
        if (c->vs[i] != s->vs[i])
            c->vs[i] = NOSL;
        else {
            if (s->vk[i] < c->vk[i])
                c->vk[i] = s->vk[i];
            c->vz[i] &= s->vz[i];
        }
}

static void meet(J *j, Cache *s)
{
    meetc(&j->c, s);
}

#define STX 0xf9000000U
#define STD 0xfd000000U

/* (pass 1) the instruction reads the slot */
static void use(J *j, s32 slot)
{
    if (j->pass != 1 || slot < 0 || (u32)slot >= j->f->nreg)
        return;
    if (j->nus == j->capus) {
        u32 nc = j->capus ? j->capus * 2 : 1024;
        s32 *nu = (s32 *)realloc(j->us, (size_t)nc * sizeof(s32));
        if (!nu) {
            j->fail = 1;
            return;
        }
        j->us = nu;
        j->capus = nc;
    }
    j->us[j->nus++] = slot;
}

/* The values that are in registers only and still wanted go to the frame: before something that reads
   the frame. */
static void sync(J *j)
{
    int i;
    for (i = 4; i < 16; i++)
        if (j->c.xs[i] != NOSL && j->xd[i]) {
            if (j->xl[i] >= (s32)j->cur)
                slot_op(j, STX, 3, i, j->c.xs[i]);
            j->xd[i] = 0;
        }
    for (i = 3; i < 24; i++)
        if (j->c.vs[i] != NOSL && j->vd[i]) {
            if (j->vl[i] >= (s32)j->cur)
                slot_op(j, STD, 3, i, j->c.vs[i]);
            j->vd[i] = 0;
        }
}

/* the end of a basic block: what was in registers only is not wanted any more */
static void block_end(J *j)
{
    memset(j->xd, 0, sizeof j->xd);
    memset(j->vd, 0, sizeof j->vd);
}

static int findx(J *j, s32 slot, int kind)
{
    int i;
    for (i = 4; i < 16; i++)
        if (j->c.xs[i] == slot && j->c.xk[i] >= kind)
            return i;
    return -1;
}

static int findv(J *j, s32 slot, int kind)
{
    int i;
    for (i = 3; i < 24; i++)
        if (j->c.vs[i] == slot && j->c.vk[i] >= kind)
            return i;
    return -1;
}

/* a register for a new value: one that holds nothing (that is still wanted), or else the one not used for the
   longest time, whose value is stored first if it is nowhere else and still wanted */
static int iralloc(J *j)
{
    int i, best = -1;
    for (i = 0; i < 12; i++) {
        int r = xpool[i];
        if (j->xpin >> r & 1)
            continue;
        if (j->c.xs[r] == NOSL || (j->xd[r] && j->xl[r] < (s32)j->cur)) {
            best = r;
            break;
        }
        if (best < 0 || j->xage[r] < j->xage[best])
            best = r;
    }
    if (j->c.xs[best] != NOSL && j->xd[best] && j->xl[best] >= (s32)j->cur)
        slot_op(j, STX, 3, best, j->c.xs[best]);
    j->c.xs[best] = NOSL;
    j->xd[best] = 0;
    j->c.xz[best] = 0;
    j->xpin |= 1U << best;
    j->xage[best] = ++j->tick;
    return best;
}

static int fralloc(J *j)
{
    int i, best = -1;
    for (i = 0; i < 13; i++) {
        int r = vpool[i];
        if (j->vpin >> r & 1)
            continue;
        if (j->c.vs[r] == NOSL || (j->vd[r] && j->vl[r] < (s32)j->cur)) {
            best = r;
            break;
        }
        if (best < 0 || j->vage[r] < j->vage[best])
            best = r;
    }
    if (j->c.vs[best] != NOSL && j->vd[best] && j->vl[best] >= (s32)j->cur)
        slot_op(j, STD, 3, best, j->c.vs[best]);
    j->c.vs[best] = NOSL;
    j->vd[best] = 0;
    j->c.vz[best] = 0;
    j->vpin |= 1U << best;
    j->vage[best] = ++j->tick;
    return best;
}

static int usex(J *j, int r)
{
    j->xpin |= 1U << r;
    j->xage[r] = ++j->tick;
    return r;
}

static int usev(J *j, int r)
{
    j->vpin |= 1U << r;
    j->vage[r] = ++j->tick;
    return r;
}

/* the register that holds all 64 bits of a slot */
static int ux(J *j, s32 slot)
{
    u64 v;
    int r, q;
    use(j, slot);
    r = findx(j, slot, 2);
    if (r >= 0)
        return usex(j, r);
    r = iralloc(j);
    if (kc(j, slot, &v)) {
        movi(j, r, v);
        j->c.xz[r] = (u8)(v >> 32 == 0);
    } else if ((q = findv(j, slot, 2)) >= 0) {
        emit(j, 0x9e660000U | (u32)q << 5 | (u32)r); /* fmov x, d */
        j->c.xz[r] = j->c.vz[q];
    } else
        slot_op(j, 0xf9400000U, 3, r, slot);
    j->c.xs[r] = slot;
    j->c.xk[r] = 2;
    return r;
}

/* the register whose low 32 bits are those of a slot */
static int uw(J *j, s32 slot)
{
    u64 v;
    int r, q;
    use(j, slot);
    r = findx(j, slot, 1);
    if (r >= 0)
        return usex(j, r);
    r = iralloc(j);
    j->c.xk[r] = 1;
    j->c.xz[r] = 1;
    if (kc(j, slot, &v)) {
        movi(j, r, v);
        j->c.xk[r] = 2;
        j->c.xz[r] = (u8)(v >> 32 == 0);
    } else if ((q = findv(j, slot, 1)) >= 0)
        emit(j, 0x1e260000U | (u32)q << 5 | (u32)r); /* fmov w, s */
    else
        slot_op(j, 0xb9400000U, 2, r, slot);
    j->c.xs[r] = slot;
    return r;
}

/* the register that holds a slot as a float or (dbl) a double */
static int uf(J *j, s32 slot, int dbl)
{
    int r, q;
    use(j, slot);
    r = findv(j, slot, dbl ? 2 : 1);
    if (r >= 0)
        return usev(j, r);
    r = fralloc(j);
    if ((q = findx(j, slot, 2)) >= 0) {
        emit(j, 0x9e670000U | (u32)q << 5 | (u32)r); /* fmov d, x */
        j->c.vk[r] = 2;
        j->c.vz[r] = j->c.xz[q];
    } else if (!dbl && (q = findx(j, slot, 1)) >= 0) {
        emit(j, 0x1e270000U | (u32)q << 5 | (u32)r); /* fmov s, w */
        j->c.vk[r] = 1;
        j->c.vz[r] = 1;
    } else if (dbl) {
        slot_op(j, 0xfd400000U, 3, r, slot);
        j->c.vk[r] = 2;
    } else {
        slot_op(j, 0xbd400000U, 2, r, slot);
        j->c.vk[r] = 1;
        j->c.vz[r] = 1;
    }
    j->c.vs[r] = slot;
    return r;
}

static void untag(J *j, s32 d)
{
    int i;
    for (i = 4; i < 16; i++)
        if (j->c.xs[i] == d)
            j->c.xs[i] = NOSL;
    for (i = 3; i < 24; i++)
        if (j->c.vs[i] == d)
            j->c.vs[i] = NOSL;
}

/* (pass 1) the instruction sets the slot d */
static void def(J *j, s32 d)
{
    if (j->pass == 1) {
        if (j->rdef != NOSL)
            j->rmulti = 1;
        j->rdef = d;
    }
}

/* x<r> is the new value, all 64 bits, of the slot d (z: the high 32 are zero). It is stored now if it may be
   wanted after this basic block. */
static void setxz(J *j, int r, s32 d, int z)
{
    s32 l = j->last[j->cur];
    def(j, d);
    untag(j, d);
    j->c.xs[r] = d;
    j->c.xk[r] = 2;
    j->c.xz[r] = (u8)z;
    j->xd[r] = 0;
    if (l == INF)
        slot_op(j, STX, 3, r, d);
    else {
        j->xd[r] = 1;
        j->xl[r] = l;
    }
}

static void setx(J *j, int r, s32 d)
{
    setxz(j, r, d, 0);
}

/* d<r> is the new value of the slot d: a double, or (!dbl) a float with zeros above it */
static void setf(J *j, int r, s32 d, int dbl)
{
    s32 l = j->last[j->cur];
    def(j, d);
    untag(j, d);
    j->c.vs[r] = d;
    j->c.vk[r] = 2;
    j->c.vz[r] = (u8)!dbl;
    j->vd[r] = 0;
    if (l == INF)
        slot_op(j, STD, 3, r, d);
    else {
        j->vd[r] = 1;
        j->vl[r] = l;
    }
}

#define RRR(op, d, n, m) ((op) | (u32)(m) << 16 | (u32)(n) << 5 | (u32)(d))
#define RRRR(op, d, n, m, a) ((op) | (u32)(m) << 16 | (u32)(a) << 10 | (u32)(n) << 5 | (u32)(d))
#define CSEL(sf, d, n, m, c) (((sf) ? 0x9a800000U : 0x1a800000U) | (u32)(m) << 16 | (u32)(c) << 12 | (u32)(n) << 5 | (u32)(d))
#define CSET(d, c) (0x1a9f07e0U | (u32)((c) ^ 1) << 12 | (u32)(d))
#define CSINC(d, n, m, c) (0x1a800400U | (u32)(m) << 16 | (u32)(c) << 12 | (u32)(n) << 5 | (u32)(d))
#define CMPI(sf, n, imm) (((sf) ? 0xf100001fU : 0x7100001fU) | (u32)(imm) << 10 | (u32)(n) << 5)
#define CMPR(sf, n, m) (((sf) ? 0xeb00001fU : 0x6b00001fU) | (u32)(m) << 16 | (u32)(n) << 5)
#define SF(w) ((w) ? 0U : 0x80000000U)

static void flush(J *j)
{
    if (j->pend)
        emit(j, 0x91000000U | j->pend << 10 | 22 << 5 | 22);
    j->pend = 0;
}

/* leave for the interpreter at the instruction ins (ws: a warp instruction, after which the next thread of
   the warp may take over) */
static void leave(J *j, u32 ins, int ws)
{
    emit(j, 0x52800000U | (ins & 0xffff) << 5);
    if (ins >> 16)
        emit(j, 0x72a00000U | (ins >> 16) << 5);
    emit(j, 0x14000000U | (((ws ? j->wexit_at : j->exit_at) - j->n) & 0x3ffffffU));
}

static void fl_add(J *j, s32 slot, int reg, int isv)
{
    if (j->nfl == j->capfl) {
        u32 nc = j->capfl ? j->capfl * 2 : 256;
        Fl *nf = (Fl *)realloc(j->fl, (size_t)nc * sizeof(Fl));
        if (!nf) {
            j->fail = 1;
            return;
        }
        j->fl = nf;
        j->capfl = nc;
    }
    j->fl[j->nfl].slot = slot;
    j->fl[j->nfl].reg = (u8)reg;
    j->fl[j->nfl].isv = (u8)isv;
    j->nfl++;
}

/* a conditional branch (the word given, its displacement empty) to code that leaves at this instruction,
   after it has stored what is in registers only and still wanted */
static void slow(J *j, u32 word)
{
    int i;
    if (j->nst == j->capst) {
        u32 nc = j->capst ? j->capst * 2 : 256;
        Stub *ns = (Stub *)realloc(j->st, (size_t)nc * sizeof(Stub));
        if (!ns) {
            j->fail = 1;
            return;
        }
        j->st = ns;
        j->capst = nc;
    }
    j->st[j->nst].at = j->n;
    j->st[j->nst].ins = j->cur | j->stflag;
    j->st[j->nst].pend = j->pend;
    j->st[j->nst].fl = j->nfl;
    for (i = 4; i < 16; i++)
        if (j->c.xs[i] != NOSL && j->xd[i] && j->xl[i] >= (s32)j->cur)
            fl_add(j, j->c.xs[i], i, 0);
    for (i = 3; i < 24; i++)
        if (j->c.vs[i] != NOSL && j->vd[i] && j->vl[i] >= (s32)j->cur)
            fl_add(j, j->c.vs[i], i, 1);
    j->st[j->nst].nfl = j->nfl - j->st[j->nst].fl;
    j->nst++;
    emit(j, word);
}

/* (pass 2) At the head of a loop: load what the state p says the registers hold there. */
static void preload(J *j, Cache *p)
{
    int i;
    u64 v;
    for (i = 4; i < 16; i++)
        if (p->xs[i] != NOSL) {
            j->c.xs[i] = p->xs[i];
            j->c.xk[i] = p->xk[i];
            j->c.xz[i] = 0;
            j->xd[i] = 0;
            j->xage[i] = ++j->tick;
            if (kc(j, p->xs[i], &v)) {
                movi(j, i, v);
                j->c.xk[i] = 2;
            } else if (p->xk[i] == 2)
                slot_op(j, 0xf9400000U, 3, i, p->xs[i]);
            else
                slot_op(j, 0xb9400000U, 2, i, p->xs[i]);
        }
    for (i = 3; i < 24; i++)
        if (p->vs[i] != NOSL) {
            j->c.vs[i] = p->vs[i];
            j->c.vk[i] = p->vk[i];
            j->c.vz[i] = 0;
            j->vd[i] = 0;
            j->vage[i] = ++j->tick;
            if (p->vk[i] == 2)
                slot_op(j, 0xfd400000U, 3, i, p->vs[i]);
            else
                slot_op(j, 0xbd400000U, 2, i, p->vs[i]);
        }
}

/* (pass 2) Before a branch back to the head of a loop: code that makes the registers hold what the state p
   says, from what they hold now and from the frame (which has all that is wanted after a block). The record
   of what they hold is not changed: the code is on the way of the branch only. */
static void conform(J *j, Cache *p)
{
    Cache s;
    int i, q;
    u64 v;
    s = j->c;
    for (i = 4; i < 16; i++) {
        s32 slot = p->xs[i];
        if (slot == NOSL || (s.xs[i] == slot && s.xk[i] >= p->xk[i]))
            continue;
        for (q = 4; q < 16; q++)
            if (s.xs[q] == slot && s.xk[q] >= p->xk[i])
                break;
        if (q < 16)
            emit(j, 0xaa0003e0U | (u32)q << 16 | (u32)i); /* mov */
        else {
            for (q = 3; q < 24; q++)
                if (s.vs[q] == slot && s.vk[q] >= p->xk[i])
                    break;
            if (kc(j, slot, &v))
                movi(j, i, v);
            else if (q < 24)
                emit(j, (s.vk[q] == 2 ? 0x9e660000U : 0x1e260000U) | (u32)q << 5 | (u32)i); /* fmov x, d or w, s */
            else if (p->xk[i] == 2)
                slot_op(j, 0xf9400000U, 3, i, slot);
            else
                slot_op(j, 0xb9400000U, 2, i, slot);
        }
        s.xs[i] = slot;
        s.xk[i] = p->xk[i];
    }
    for (i = 3; i < 24; i++) {
        s32 slot = p->vs[i];
        if (slot == NOSL || (s.vs[i] == slot && s.vk[i] >= p->vk[i]))
            continue;
        for (q = 3; q < 24; q++)
            if (s.vs[q] == slot && s.vk[q] >= p->vk[i])
                break;
        if (q < 24)
            emit(j, 0x1e604000U | (u32)q << 5 | (u32)i); /* fmov d, d */
        else {
            for (q = 4; q < 16; q++)
                if (s.xs[q] == slot && s.xk[q] >= p->vk[i])
                    break;
            if (q < 16)
                emit(j, (s.xk[q] == 2 ? 0x9e670000U : 0x1e270000U) | (u32)q << 5 | (u32)i); /* fmov d, x or s, w */
            else if (p->vk[i] == 2)
                slot_op(j, 0xfd400000U, 3, i, slot);
            else
                slot_op(j, 0xbd400000U, 2, i, slot);
        }
        s.vs[i] = slot;
        s.vk[i] = p->vk[i];
    }
}

static void fixup(J *j, s32 target, int body)
{
    if (j->nfx == j->capfx) {
        u32 nc = j->capfx ? j->capfx * 2 : 256;
        Fixup *nf = (Fixup *)realloc(j->fx, (size_t)nc * sizeof(Fixup));
        if (!nf) {
            j->fail = 1;
            return;
        }
        j->fx = nf;
        j->capfx = nc;
    }
    j->fx[j->nfx].at = j->n;
    j->fx[j->nfx].target = target;
    j->fx[j->nfx].body = body;
    j->nfx++;
}

/* a branch back (the word given: b, cbz or cbnz) to the head of a loop */
static void jump_back(J *j, u32 word, s32 target)
{
    Cache *p = 0;
    if (j->pass == 1 && j->bhead[target] < 0) {
        if (j->nbst == j->capbst) {
            u32 nc = j->capbst ? j->capbst * 2 : 16;
            Cache *nb = (Cache *)realloc(j->bst, (size_t)nc * sizeof(Cache));
            if (!nb) {
                j->fail = 1;
                return;
            }
            j->bst = nb;
            j->capbst = nc;
        }
        forget_c(&j->bst[j->nbst]);
        j->bhead[target] = (s32)j->nbst++;
    }
    if (j->pass > 1 && j->bhead[target] >= 0)
        p = &j->bst[j->bhead[target]];
    if (j->pass < 3 && j->bnew && j->bhead[target] >= 0 && !j->bseen[j->bhead[target]]) {
        /* What the registers hold at the first branch back, the nearest: the others, which may as well be
           the way into the loop from code that lies after it, will adapt to that. */
        j->bseen[j->bhead[target]] = 1;
        j->bnew[j->bhead[target]] = j->c;
    }
    if ((word & 0xfc000000U) == 0x14000000U) {
        if (p)
            conform(j, p);
        fixup(j, target, 1);
        emit(j, word);
    } else {
        u32 at = j->n;
        emit(j, word ^ 0x01000000U); /* the other of cbz and cbnz, to go around */
        if (p)
            conform(j, p);
        if (j->n == at + 1) {
            j->n = at;
            fixup(j, target, 1);
            emit(j, word);
        } else {
            fixup(j, target, 1);
            emit(j, 0x14000000U);
            if (!j->fail)
                j->w[at] |= ((j->n - at) & 0x7ffff) << 5;
        }
    }
    block_end(j);
}

/* a branch (the word given) to the code of the instruction target; a forward one takes the state of the
   registers along */
static void jump(J *j, u32 word, s32 target)
{
    if (target <= (s32)j->cur) {
        jump_back(j, word, target);
        return;
    }
    fixup(j, target, 0);
    emit(j, word);
    block_end(j);
    if (j->lead[target] < 2) {
        if (j->nsn == j->capsn) {
            u32 nc = j->capsn ? j->capsn * 2 : 64;
            Snap *ns = (Snap *)realloc(j->sn, (size_t)nc * sizeof(Snap));
            if (!ns) {
                j->fail = 1;
                return;
            }
            j->sn = ns;
            j->capsn = nc;
        }
        j->sn[j->nsn].c = j->c;
        j->sn[j->nsn].next = j->snhead[target];
        j->snhead[target] = (s32)j->nsn++;
    }
}

/* The address RA.u + c of a load or store (sh: of the shared space, or generic), checked. The result is the
   register and, in *disp, what the instruction has to add to it; size is the log of the access's size. */
static int addr(J *j, Ins *ip, int sh, int size, u32 *disp)
{
    s64 c = ip->c;
    int b = ux(j, ip->a), k = ip->a < 0 && (u32)(-1 - ip->a) < j->f->nconst ? j->fixed[-1 - ip->a] : 0;
    *disp = 0;
    if (!sh && (k == FX_FRAME || k == FX_PARAM) && c >= 0 && c < 0x100000) {
        /* an address in the frame or in the parameters: nothing to check */
        if ((c & ((1 << size) - 1)) == 0 && (c >> size) < 4096) {
            *disp = (u32)(c >> size);
            return b;
        }
    } else if (!sh && c == 0) {
        emit(j, CMPI(1, b, 1) | 0x00400000U); /* cmp xb, #4096 */
        slow(j, 0x54000000U | LO);
        return b;
    }
    if (c >= 0 && c < 4096)
        emit(j, 0x91000000U | (u32)c << 10 | (u32)b << 5);
    else if (c < 0 && c > -4096)
        emit(j, 0xd1000000U | (u32)-c << 10 | (u32)b << 5);
    else {
        movi(j, 16, (u64)c);
        emit(j, RRR(0x8b000000U, 0, b, 16));
    }
    if (sh) {
        emit(j, CMPR(1, 0, 21));
        emit(j, RRR(0x8b000000U, 1, 0, 20));
        emit(j, CSEL(1, 0, 1, 0, LO));
    }
    if (sh || !(k == FX_FRAME || k == FX_PARAM) || c < 0) {
        emit(j, 0xf140041fU); /* cmp x0, #4096 */
        slow(j, 0x54000000U | LO);
    }
    return 0;
}

static const u32 ld_op[9] = {0x39400000U, 0x39c00000U, 0x39800000U, 0x79400000U, 0x79c00000U,
                             0x79800000U, 0xb9400000U, 0xb9800000U, 0xf9400000U};
static const u8 ld_size[9] = {0, 0, 0, 1, 1, 1, 2, 2, 3};
static const u8 ld_z[9] = {1, 1, 0, 1, 1, 0, 1, 0, 0};
static const u32 st_op[4] = {0x39000000U, 0x79000000U, 0xb9000000U, 0xf9000000U};
static const u8 icond_u[6] = {EQ, NE, LO, LS, HI, HS};
static const u8 icond_s[6] = {EQ, NE, LT, LE, GT, GE};

/* d = a op b on integers (w: of 32 bits); op has no registers in it */
static void bin(J *j, Ins *ip, int w, u32 op)
{
    int a = w ? uw(j, ip->a) : ux(j, ip->a), b = w ? uw(j, ip->b) : ux(j, ip->b), d = iralloc(j);
    emit(j, RRR(op, d, a, b));
    setxz(j, d, ip->d, !(op >> 31)); /* (an instruction of 32 bits clears the rest of its register) */
}

/* d = op a, the operand in the instruction's n field (m: its m field) */
static void un(J *j, Ins *ip, int w, int m, u32 op)
{
    int a = w ? uw(j, ip->a) : ux(j, ip->a), d = iralloc(j);
    emit(j, op | (u32)a << (m ? 16 : 5) | (u32)d);
    setxz(j, d, ip->d, !(op >> 31));
}

static void addsub(J *j, Ins *ip, int w, int sub)
{
    u64 v;
    if (kc(j, ip->b, &v)) {
        if (w)
            v = (u64)(s64)(s32)(u32)v;
        if (v < 4096 || 0 - v < 4096) {
            int a = w ? uw(j, ip->a) : ux(j, ip->a), d = iralloc(j);
            if (v >= 4096) {
                v = 0 - v;
                sub = !sub;
            }
            emit(j, (sub ? 0x51000000U : 0x11000000U) | SF(w) | (u32)v << 10 | (u32)a << 5 | (u32)d);
            setxz(j, d, ip->d, w);
            return;
        }
    }
    bin(j, ip, w, (sub ? 0x4b000000U : 0x0b000000U) | SF(w));
}

/* kind 0: shl, 1: shr unsigned, 2: shr signed */
static void shift(J *j, Ins *ip, int w, int kind)
{
    u64 v;
    u32 bits = w ? 32 : 64;
    int a = w ? uw(j, ip->a) : ux(j, ip->a), b, d;
    if (kc(j, ip->b, &v)) {
        u32 k = (u32)v;
        d = iralloc(j);
        if (k >= bits && kind != 2)
            emit(j, 0xd2800000U | (u32)d);
        else {
            u32 immr, imms, op = kind == 2 ? 0x13000000U : 0x53000000U;
            if (k >= bits)
                k = bits - 1;
            if (kind == 0) {
                immr = (bits - k) & (bits - 1);
                imms = bits - 1 - k;
            } else {
                immr = k;
                imms = bits - 1;
            }
            if (!w)
                op |= 0x80400000U;
            emit(j, op | immr << 16 | imms << 10 | (u32)a << 5 | (u32)d);
        }
    } else {
        b = uw(j, ip->b);
        d = iralloc(j);
        if (kind == 2) {
            emit(j, 0x52800000U | (bits - 1) << 5); /* mov w0, #bits - 1 */
            emit(j, CMPI(0, b, bits - 1));
            emit(j, CSEL(0, 0, b, 0, LO));
            emit(j, RRR(0x1ac02800U | SF(w), d, a, 0));
        } else {
            emit(j, RRR((kind ? 0x1ac02400U : 0x1ac02000U) | SF(w), d, a, b));
            emit(j, CMPI(0, b, bits));
            emit(j, CSEL(!w, d, d, 31, LO));
        }
    }
    setx(j, d, ip->d);
}

static void icmp(J *j, Ins *ip, int w, int cond)
{
    u64 v;
    int a = w ? uw(j, ip->a) : ux(j, ip->a), d;
    if (kc(j, ip->b, &v) && (w ? (u32)v : v) < 4096)
        emit(j, CMPI(!w, a, (u32)v));
    else {
        int b = w ? uw(j, ip->b) : ux(j, ip->b);
        emit(j, CMPR(!w, a, b));
    }
    d = iralloc(j);
    emit(j, CSET(d, cond));
    setx(j, d, ip->d);
}

static void fcmp(J *j, Ins *ip, int dbl, int cmp)
{
    int a = uf(j, ip->a, dbl), b = uf(j, ip->b, dbl), d = iralloc(j);
    emit(j, (dbl ? 0x1e602000U : 0x1e202000U) | (u32)b << 16 | (u32)a << 5);
    switch (cmp) {
    case CMP_EQ:
        emit(j, CSET(d, EQ));
        break;
    case CMP_NE:
        emit(j, CSET(d, MI));
        emit(j, CSINC(d, d, 31, LE));
        break;
    case CMP_LT:
        emit(j, CSET(d, MI));
        break;
    case CMP_LE:
        emit(j, CSET(d, LS));
        break;
    case CMP_GT:
        emit(j, CSET(d, GT));
        break;
    case CMP_GE:
        emit(j, CSET(d, GE));
        break;
    case CMP_EQU:
        emit(j, CSET(d, EQ));
        emit(j, CSINC(d, d, 31, VC));
        break;
    case CMP_NEU:
        emit(j, CSET(d, NE));
        break;
    case CMP_LTU:
        emit(j, CSET(d, LT));
        break;
    case CMP_LEU:
        emit(j, CSET(d, LE));
        break;
    case CMP_GTU:
        emit(j, CSET(d, HI));
        break;
    case CMP_GEU:
        emit(j, CSET(d, PL));
        break;
    case CMP_NUM:
        emit(j, CSET(d, VC));
        break;
    default:
        emit(j, CSET(d, VS));
        break;
    }
    setx(j, d, ip->d);
}

/* float operation of two (three: fma) operands; op is that of floats, without registers */
static void fbin(J *j, Ins *ip, int dbl, int three, u32 op)
{
    int a = uf(j, ip->a, dbl), b = uf(j, ip->b, dbl), c = three ? uf(j, ip->c, dbl) : 0, d = fralloc(j);
    emit(j, op | (dbl ? 0x00400000U : 0) | (u32)b << 16 | (u32)c << 10 | (u32)a << 5 | (u32)d);
    setf(j, d, ip->d, dbl);
}

static void fun(J *j, Ins *ip, int dbl, u32 op)
{
    int a = uf(j, ip->a, dbl), d = fralloc(j);
    emit(j, op | (dbl ? 0x00400000U : 0) | (u32)a << 5 | (u32)d);
    setf(j, d, ip->d, dbl);
}

static void fminmax(J *j, Ins *ip, int dbl, u32 op)
{
    u32 ty = dbl ? 0x00400000U : 0;
    int a = uf(j, ip->a, dbl), b = uf(j, ip->b, dbl), v;
    emit(j, 0x1e202000U | ty | (u32)b << 16 | (u32)a << 5);
    slow(j, 0x54000000U | VS);
    v = fralloc(j);
    emit(j, op | ty | (u32)b << 16 | (u32)a << 5 | (u32)v);
    setf(j, v, ip->d, dbl);
}

/* int to float: op without registers; x: the operand has 64 bits */
static void i2f(J *j, Ins *ip, int x, int dbl, u32 op)
{
    int a = x ? ux(j, ip->a) : uw(j, ip->a), d = fralloc(j);
    emit(j, op | (u32)a << 5 | (u32)d);
    setf(j, d, ip->d, dbl);
}

/* nan: what is not a number becomes the top bit (ptxexec.c, f2s32), where fcvtzs has 0 */
static void f2i(J *j, Ins *ip, int dbl, u32 op, int nan)
{
    int a = uf(j, ip->a, dbl), d = iralloc(j);
    emit(j, op | (u32)a << 5 | (u32)d);
    if (nan) {
        emit(j, (dbl ? 0x1e602000U : 0x1e202000U) | (u32)a << 16 | (u32)a << 5); /* fcmp a, a */
        emit(j, 0x54000047U);                                                    /* b.vc over the next */
        emit(j, (op >> 31 ? 0xd2f00000U : 0x52b00000U) | (u32)d);                /* movz d, #0x8000, lsl #48 or #16 */
    }
    setx(j, d, ip->d);
}

/* An argument of a call: the slot's 64 bits (w: its low 32, the rest zero) in x<n>, n <= 4 (x4 last: it may
   hold another argument's slot). sync() comes before the first. */
static void arg(J *j, int n, s32 slot, int w)
{
    u64 v;
    int r;
    use(j, slot);
    if (kc(j, slot, &v))
        movi(j, n, w ? v & 0xffffffffU : v);
    else if ((r = findx(j, slot, w ? 1 : 2)) >= 0)
        emit(j, (w ? 0x2a0003e0U : 0xaa0003e0U) | (u32)r << 16 | (u32)n);
    else
        slot_op(j, w ? 0xb9400000U : 0xf9400000U, w ? 2 : 3, n, slot);
}

/* Call a C function; its result becomes the slot d. Nothing is in the registers afterwards. */
static void call(J *j, void *fn, s32 d)
{
    movi(j, 16, (u64)fn);
    emit(j, 0xd63f0200U); /* blr x16 */
    forget(j);
    slot_op(j, 0xf9000000U, 3, 0, d);
}

/* The code of one instruction; 0 if it has none. */
static int one(J *j, Ins *ip)
{
    u32 op = ip->op, disp;
    u64 v;
    int a, b, c, d;
    if (op >= OP_SETP_U32 && op < OP_SETP_END) {
        if (op < OP_SETP_S32)
            icmp(j, ip, 1, icond_u[op - OP_SETP_U32]);
        else if (op < OP_SETP_U64)
            icmp(j, ip, 1, icond_s[op - OP_SETP_S32]);
        else if (op < OP_SETP_S64)
            icmp(j, ip, 0, icond_u[op - OP_SETP_U64]);
        else if (op < OP_SETP_F32)
            icmp(j, ip, 0, icond_s[op - OP_SETP_S64]);
        else if (op < OP_SETP_F64)
            fcmp(j, ip, 0, (int)(op - OP_SETP_F32));
        else
            fcmp(j, ip, 1, (int)(op - OP_SETP_F64));
        return 1;
    }
    if ((op >= OP_LD8 && op <= OP_LD64) || (op >= OP_LD8_S && op <= OP_LD64_S)) {
        int sh = op >= OP_LD8_S, k = (int)(sh ? op - OP_LD8_S : op - OP_LD8);
        b = addr(j, ip, sh, ld_size[k], &disp);
        d = iralloc(j);
        emit(j, ld_op[k] | disp << 10 | (u32)b << 5 | (u32)d);
        setxz(j, d, ip->d, ld_z[k]);
        return 1;
    }
    if ((op >= OP_ST8 && op <= OP_ST64) || (op >= OP_ST8_S && op <= OP_ST64_S)) {
        int sh = op >= OP_ST8_S, k = (int)(sh ? op - OP_ST8_S : op - OP_ST8);
        b = addr(j, ip, sh, k, &disp);
        a = k == 3 ? ux(j, ip->b) : uw(j, ip->b);
        emit(j, st_op[k] | disp << 10 | (u32)b << 5 | (u32)a);
        return 1;
    }
    switch (op) {
    case OP_NOP:
        break;
    case OP_MOV:
        if (kc(j, ip->a, &v)) {
            d = iralloc(j);
            movi(j, d, v);
            setxz(j, d, ip->d, v >> 32 == 0);
            break;
        }
        use(j, ip->a);
        if (ip->a == ip->d)
            break;
        a = findx(j, ip->a, 2);
        b = findv(j, ip->a, 2);
        if (j->adead[j->cur] && a >= 0) {
            /* the register changes hands */
            setxz(j, usex(j, a), ip->d, j->c.xz[a]);
        } else if (j->adead[j->cur] && b >= 0) {
            c = j->c.vz[b];
            setf(j, usev(j, b), ip->d, 1);
            j->c.vz[b] = (u8)c;
        } else if (a < 0 && b < 0) {
            d = iralloc(j);
            slot_op(j, 0xf9400000U, 3, d, ip->a);
            setx(j, d, ip->d);
        } else if (a < 0) {
            c = j->c.vz[b];
            usev(j, b);
            d = fralloc(j);
            emit(j, 0x1e604000U | (u32)b << 5 | (u32)d); /* fmov d, d */
            setf(j, d, ip->d, 1);
            j->c.vz[d] = (u8)c;
        } else {
            a = ux(j, ip->a);
            d = iralloc(j);
            emit(j, 0xaa0003e0U | (u32)a << 16 | (u32)d);
            setxz(j, d, ip->d, j->c.xz[a]);
        }
        break;
    case OP_MOV32:
        if (kc(j, ip->a, &v)) {
            d = iralloc(j);
            movi(j, d, v & 0xffffffffU);
            setxz(j, d, ip->d, 1);
            break;
        }
        use(j, ip->a);
        a = findx(j, ip->a, 1);
        b = findv(j, ip->a, 1);
        if (ip->a != ip->d && j->adead[j->cur] && a >= 0 && j->c.xz[a])
            setxz(j, usex(j, a), ip->d, 1);
        else if (ip->a != ip->d && j->adead[j->cur] && b >= 0 && j->c.vz[b])
            setf(j, usev(j, b), ip->d, 0);
        else if (a < 0 && b < 0) {
            d = iralloc(j);
            slot_op(j, 0xb9400000U, 2, d, ip->a);
            setxz(j, d, ip->d, 1);
        } else if (a < 0) {
            usev(j, b);
            d = fralloc(j);
            emit(j, 0x1e204000U | (u32)b << 5 | (u32)d); /* fmov s, s */
            setf(j, d, ip->d, 0);
        } else {
            a = uw(j, ip->a);
            d = iralloc(j);
            emit(j, 0x2a0003e0U | (u32)a << 16 | (u32)d);
            setxz(j, d, ip->d, 1);
        }
        break;
    case OP_ZX8:
        un(j, ip, 1, 0, 0x12001c00U);
        break;
    case OP_ZX16:
        un(j, ip, 1, 0, 0x12003c00U);
        break;
    case OP_SX8_32:
        un(j, ip, 1, 0, 0x13001c00U);
        break;
    case OP_SX16_32:
        un(j, ip, 1, 0, 0x13003c00U);
        break;
    case OP_SX8_64:
        un(j, ip, 1, 0, 0x93401c00U);
        break;
    case OP_SX16_64:
        un(j, ip, 1, 0, 0x93403c00U);
        break;
    case OP_SX32_64:
        un(j, ip, 1, 0, 0x93407c00U);
        break;
    case OP_NEG32:
        un(j, ip, 1, 1, 0x4b0003e0U);
        break;
    case OP_NOT32:
        un(j, ip, 1, 1, 0x2a2003e0U);
        break;
    case OP_CLZ32:
        un(j, ip, 1, 0, 0x5ac01000U);
        break;
    case OP_BREV32:
        un(j, ip, 1, 0, 0x5ac00000U);
        break;
    case OP_FABS32:
        un(j, ip, 1, 0, 0x12007800U);
        break;
    case OP_FNEG32:
        un(j, ip, 1, 0, 0x52010000U);
        break;
    case OP_NEG64:
        un(j, ip, 0, 1, 0xcb0003e0U);
        break;
    case OP_NOT64:
        un(j, ip, 0, 1, 0xaa2003e0U);
        break;
    case OP_CLZ64:
        un(j, ip, 0, 0, 0xdac01000U);
        break;
    case OP_BREV64:
        un(j, ip, 0, 0, 0xdac00000U);
        break;
    case OP_FABS64:
        un(j, ip, 0, 0, 0x9240f800U);
        break;
    case OP_FNEG64:
        un(j, ip, 0, 0, 0xd2410000U);
        break;
    case OP_PNOT:
        a = uw(j, ip->a);
        d = iralloc(j);
        emit(j, 0x52000000U | (u32)a << 5 | (u32)d);
        emit(j, 0x12000000U | (u32)d << 5 | (u32)d);
        setx(j, d, ip->d);
        break;
    case OP_PAND:
    case OP_POR:
    case OP_PXOR:
        a = uw(j, ip->a);
        b = uw(j, ip->b);
        d = iralloc(j);
        emit(j, RRR(op == OP_PAND ? 0x0a000000U : op == OP_POR ? 0x2a000000U : 0x4a000000U, d, a, b));
        emit(j, 0x12000000U | (u32)d << 5 | (u32)d);
        setx(j, d, ip->d);
        break;
    case OP_ADD32:
        addsub(j, ip, 1, 0);
        break;
    case OP_SUB32:
        addsub(j, ip, 1, 1);
        break;
    case OP_ADD64:
        addsub(j, ip, 0, 0);
        break;
    case OP_SUB64:
        addsub(j, ip, 0, 1);
        break;
    case OP_MUL32:
        bin(j, ip, 1, 0x1b007c00U);
        break;
    case OP_MUL64:
        bin(j, ip, 0, 0x9b007c00U);
        break;
    case OP_AND32:
        bin(j, ip, 1, 0x0a000000U);
        break;
    case OP_OR32:
        bin(j, ip, 1, 0x2a000000U);
        break;
    case OP_XOR32:
        bin(j, ip, 1, 0x4a000000U);
        break;
    case OP_AND64:
        bin(j, ip, 0, 0x8a000000U);
        break;
    case OP_OR64:
        bin(j, ip, 0, 0xaa000000U);
        break;
    case OP_XOR64:
        bin(j, ip, 0, 0xca000000U);
        break;
    case OP_MULWU32:
        bin(j, ip, 1, 0x9ba07c00U);
        break;
    case OP_MULWS32:
        bin(j, ip, 1, 0x9b207c00U);
        break;
    case OP_MULHIU32:
    case OP_MULHIS32:
        a = uw(j, ip->a);
        b = uw(j, ip->b);
        d = iralloc(j);
        emit(j, RRR(op == OP_MULHIU32 ? 0x9ba07c00U : 0x9b207c00U, d, a, b));
        emit(j, 0xd360fc00U | (u32)d << 5 | (u32)d); /* lsr xd, xd, #32 */
        setx(j, d, ip->d);
        break;
    case OP_MULHIU64:
        bin(j, ip, 0, 0x9bc07c00U);
        break;
    case OP_MULHIS64:
        bin(j, ip, 0, 0x9b407c00U);
        break;
    case OP_MAD32:
        a = uw(j, ip->a);
        b = uw(j, ip->b);
        c = uw(j, ip->c);
        d = iralloc(j);
        emit(j, RRRR(0x1b000000U, d, a, b, c));
        setx(j, d, ip->d);
        break;
    case OP_MAD64:
        a = ux(j, ip->a);
        b = ux(j, ip->b);
        c = ux(j, ip->c);
        d = iralloc(j);
        emit(j, RRRR(0x9b000000U, d, a, b, c));
        setx(j, d, ip->d);
        break;
    case OP_MADWU32:
    case OP_MADWS32:
        a = uw(j, ip->a);
        b = uw(j, ip->b);
        c = ux(j, ip->c);
        d = iralloc(j);
        emit(j, RRRR(op == OP_MADWU32 ? 0x9ba00000U : 0x9b200000U, d, a, b, c));
        setx(j, d, ip->d);
        break;
    case OP_DIVU32:
    case OP_DIVS32:
    case OP_DIVU64:
    case OP_DIVS64: {
        int w = op == OP_DIVU32 || op == OP_DIVS32, sg = op == OP_DIVS32 || op == OP_DIVS64;
        a = w ? uw(j, ip->a) : ux(j, ip->a);
        b = w ? uw(j, ip->b) : ux(j, ip->b);
        d = iralloc(j);
        emit(j, RRR((sg ? 0x1ac00c00U : 0x1ac00800U) | SF(w), 0, a, b));
        emit(j, CMPI(!w, b, 0));
        emit(j, 0x5a800000U | SF(w) | 31 << 16 | NE << 12 | 0 << 5 | (u32)d); /* csinv d, x0, xzr, ne */
        setx(j, d, ip->d);
        break;
    }
    case OP_REMU32:
    case OP_REMS32:
    case OP_REMU64:
    case OP_REMS64: {
        int w = op == OP_REMU32 || op == OP_REMS32, sg = op == OP_REMS32 || op == OP_REMS64;
        a = w ? uw(j, ip->a) : ux(j, ip->a);
        b = w ? uw(j, ip->b) : ux(j, ip->b);
        d = iralloc(j);
        emit(j, RRR((sg ? 0x1ac00c00U : 0x1ac00800U) | SF(w), 0, a, b));
        emit(j, RRRR(0x1b008000U | SF(w), d, 0, b, a)); /* msub d, x0, b, a */
        setx(j, d, ip->d);
        break;
    }
    case OP_ABS32:
    case OP_ABS64: {
        int w = op == OP_ABS32;
        a = w ? uw(j, ip->a) : ux(j, ip->a);
        d = iralloc(j);
        emit(j, CMPI(!w, a, 0));
        emit(j, 0x5a800400U | SF(w) | (u32)a << 16 | GE << 12 | (u32)a << 5 | (u32)d); /* csneg d, a, a, ge */
        setx(j, d, ip->d);
        break;
    }
    case OP_MINU32:
    case OP_MINS32:
    case OP_MAXU32:
    case OP_MAXS32:
        a = uw(j, ip->a);
        b = uw(j, ip->b);
        d = iralloc(j);
        emit(j, CMPR(0, a, b));
        emit(j, CSEL(0, d, a, b, op == OP_MINU32 ? LO : op == OP_MINS32 ? LT : op == OP_MAXU32 ? HI : GT));
        setx(j, d, ip->d);
        break;
    case OP_MINU64:
    case OP_MINS64:
    case OP_MAXU64:
    case OP_MAXS64:
        a = ux(j, ip->a);
        b = ux(j, ip->b);
        d = iralloc(j);
        emit(j, CMPR(1, a, b));
        emit(j, CSEL(1, d, a, b, op == OP_MINU64 ? LO : op == OP_MINS64 ? LT : op == OP_MAXU64 ? HI : GT));
        setx(j, d, ip->d);
        break;
    case OP_CNOT32:
    case OP_CNOT64:
        a = op == OP_CNOT32 ? uw(j, ip->a) : ux(j, ip->a);
        d = iralloc(j);
        emit(j, CMPI(op == OP_CNOT64, a, 0));
        emit(j, CSET(d, EQ));
        setx(j, d, ip->d);
        break;
    case OP_SHL32:
        shift(j, ip, 1, 0);
        break;
    case OP_SHRU32:
        shift(j, ip, 1, 1);
        break;
    case OP_SHRS32:
        shift(j, ip, 1, 2);
        break;
    case OP_SHL64:
        shift(j, ip, 0, 0);
        break;
    case OP_SHRU64:
        shift(j, ip, 0, 1);
        break;
    case OP_SHRS64:
        shift(j, ip, 0, 2);
        break;
    case OP_SELP:
    case OP_SELP32:
        if (kc(j, ip->c, &v)) {
            a = op == OP_SELP ? ux(j, (u32)v ? ip->a : ip->b) : uw(j, (u32)v ? ip->a : ip->b);
            d = iralloc(j);
            emit(j, (op == OP_SELP ? 0xaa0003e0U : 0x2a0003e0U) | (u32)a << 16 | (u32)d);
            setx(j, d, ip->d);
            break;
        }
        c = uw(j, ip->c);
        a = op == OP_SELP ? ux(j, ip->a) : uw(j, ip->a);
        b = op == OP_SELP ? ux(j, ip->b) : uw(j, ip->b);
        d = iralloc(j);
        emit(j, CMPI(0, c, 0));
        emit(j, CSEL(op == OP_SELP, d, a, b, NE));
        setx(j, d, ip->d);
        break;
    case OP_CVTA_SHARED:
        a = ux(j, ip->a);
        d = iralloc(j);
        emit(j, CMPR(1, a, 21));
        emit(j, RRR(0x8b000000U, 0, a, 20));
        emit(j, CSEL(1, d, 0, a, LO));
        setx(j, d, ip->d);
        break;
    case OP_CVTA_TO_SHARED:
        a = ux(j, ip->a);
        d = iralloc(j);
        emit(j, RRR(0xcb000000U, 0, a, 20));
        emit(j, CMPR(1, 0, 21));
        emit(j, CSEL(1, d, 0, a, LO));
        setx(j, d, ip->d);
        break;

    case OP_FADD32:
        fbin(j, ip, 0, 0, 0x1e202800U);
        break;
    case OP_FSUB32:
        fbin(j, ip, 0, 0, 0x1e203800U);
        break;
    case OP_FMUL32:
        fbin(j, ip, 0, 0, 0x1e200800U);
        break;
    case OP_FDIV32:
        fbin(j, ip, 0, 0, 0x1e201800U);
        break;
    case OP_FMA32:
        fbin(j, ip, 0, 1, 0x1f000000U);
        break;
    case OP_FADD64:
        fbin(j, ip, 1, 0, 0x1e202800U);
        break;
    case OP_FSUB64:
        fbin(j, ip, 1, 0, 0x1e203800U);
        break;
    case OP_FMUL64:
        fbin(j, ip, 1, 0, 0x1e200800U);
        break;
    case OP_FDIV64:
        fbin(j, ip, 1, 0, 0x1e201800U);
        break;
    case OP_FMA64:
        fbin(j, ip, 1, 1, 0x1f000000U);
        break;
    case OP_FMIN32:
        fminmax(j, ip, 0, 0x1e205800U);
        break;
    case OP_FMAX32:
        fminmax(j, ip, 0, 0x1e204800U);
        break;
    case OP_FMIN64:
        fminmax(j, ip, 1, 0x1e205800U);
        break;
    case OP_FMAX64:
        fminmax(j, ip, 1, 0x1e204800U);
        break;
    case OP_FSQRT32:
        fun(j, ip, 0, 0x1e21c000U);
        break;
    case OP_FSQRT64:
        fun(j, ip, 1, 0x1e21c000U);
        break;
    case OP_FRCP32:
        a = uf(j, ip->a, 0);
        d = fralloc(j);
        emit(j, 0x1e2e1000U);                       /* fmov s0, #1.0 */
        emit(j, RRR(0x1e201800U, d, 0, a));         /* fdiv sd, s0, sa */
        setf(j, d, ip->d, 0);
        break;

    case OP_CVT_F32_S32:
        i2f(j, ip, 0, 0, 0x1e220000U);
        break;
    case OP_CVT_F32_U32:
        i2f(j, ip, 0, 0, 0x1e230000U);
        break;
    case OP_CVT_F64_S32:
        i2f(j, ip, 0, 1, 0x1e620000U);
        break;
    case OP_CVT_F64_U32:
        i2f(j, ip, 0, 1, 0x1e630000U);
        break;
    case OP_CVT_F32_S64:
        i2f(j, ip, 1, 0, 0x9e220000U);
        break;
    case OP_CVT_F32_U64:
        i2f(j, ip, 1, 0, 0x9e230000U);
        break;
    case OP_CVT_F64_S64:
        i2f(j, ip, 1, 1, 0x9e620000U);
        break;
    case OP_CVT_F64_U64:
        i2f(j, ip, 1, 1, 0x9e630000U);
        break;
    case OP_CVT_S32_F32Z:
        f2i(j, ip, 0, 0x1e380000U, 0);
        break;
    case OP_CVT_U32_F32Z:
        f2i(j, ip, 0, 0x1e390000U, 0);
        break;
    case OP_CVT_S64_F32Z:
        f2i(j, ip, 0, 0x9e380000U, 1);
        break;
    case OP_CVT_S32_F64Z:
        f2i(j, ip, 1, 0x1e780000U, 1);
        break;
    case OP_CVT_S64_F64Z:
        f2i(j, ip, 1, 0x9e780000U, 1);
        break;
    case OP_CVT_F64_F32:
        a = uf(j, ip->a, 0);
        d = fralloc(j);
        emit(j, 0x1e22c000U | (u32)a << 5 | (u32)d);
        setf(j, d, ip->d, 1);
        break;
    case OP_CVT_F32_F64:
        a = uf(j, ip->a, 1);
        d = fralloc(j);
        emit(j, 0x1e624000U | (u32)a << 5 | (u32)d);
        setf(j, d, ip->d, 0);
        break;
    case OP_CVT_F32_F16:
        a = uw(j, ip->a);
        d = fralloc(j);
        emit(j, 0x12003c00U | (u32)a << 5);                   /* and w0, wa, #0xffff */
        emit(j, 0xbc607800U | 0 << 16 | 24 << 5 | (u32)d);     /* ldr sd, [x24, x0, lsl #2] */
        setf(j, d, ip->d, 0);
        break;
    case OP_CVT_F16_F32:
        a = uf(j, ip->a, 0);
        emit(j, 0x1e202000U | (u32)a << 16 | (u32)a << 5); /* fcmp sa, sa */
        slow(j, 0x54000000U | VS);
        d = iralloc(j);
        emit(j, 0x1e23c000U | (u32)a << 5);  /* fcvt h0, sa */
        emit(j, 0x1e260000U | (u32)d);       /* fmov wd, s0 */
        setx(j, d, ip->d);
        break;
    case OP_PRMT:
        if (ip->aux == 0 && kc(j, ip->c, &v) && !(v & 0x8888)) {
            /* the bytes are known places of the two operands */
            int k, first = 1;
            a = uw(j, ip->a);
            b = uw(j, ip->b);
            d = iralloc(j);
            for (k = 0; k < 4; k++) {
                u32 idx = (u32)(v >> (4 * k)) & 7, src = idx < 4 ? (u32)a : (u32)b, lsb = 8 * (idx & 3);
                /* ubfx w0, src, #lsb, #8, then into its place */
                emit(j, 0x53000000U | lsb << 16 | (lsb + 7) << 10 | src << 5 | (first ? (u32)d : 0));
                if (!first)
                    emit(j, 0x2a000000U | 0 << 16 | (u32)(8 * k) << 10 | (u32)d << 5 | (u32)d); /* orr wd, wd, w0, lsl #8k */
                first = 0;
            }
            setx(j, d, ip->d);
            break;
        }
        sync(j);
        arg(j, 0, ip->a, 1);
        arg(j, 1, ip->b, 1);
        arg(j, 2, ip->c, 1);
        movi(j, 3, ip->aux);
        call(j, (void *)mc_jh_prmt, ip->d);
        break;
    case OP_CALLB:
        /* mc_builtin_call(b, t, id, ip), with t->ip = ip for what it may have to say */
        sync(j);
        movi(j, 3, (u64)ip);
        emit(j, 0xf94016e1U); /* ldr x1, [x23, #40] */
        emit(j, 0xf94012e0U); /* ldr x0, [x23, #32] */
        emit(j, 0xf9000023U); /* str x3, [x1] */
        movi(j, 2, ip->aux);
        movi(j, 16, (u64)mc_builtin_call);
        emit(j, 0xd63f0200U);
        forget(j);
        emit(j, CMPI(0, 0, 0));
        j->stflag = MC_JIT_FAULT;
        slow(j, 0x54000000U | LT);
        j->stflag = 0;
        break;
    case OP_CVT:
        sync(j);
        arg(j, 1, ip->a, 0);
        arg(j, 2, ip->b, 0);
        movi(j, 0, (u32)ip->e);
        movi(j, 3, 0);
        call(j, (void *)mc_cvt, ip->d);
        break;
    case OP_FGEN:
        sync(j);
        arg(j, 2, ip->a, 0);
        arg(j, 3, ip->b, 0);
        arg(j, 4, ip->c, 0);
        movi(j, 0, ip->aux);
        movi(j, 1, (u32)ip->e);
        call(j, (void *)mc_jh_fgen, ip->d);
        break;
    case OP_ATOM:
    case OP_ATOM_S:
        sync(j);
        b = addr(j, ip, op == OP_ATOM_S, 0, &disp);
        emit(j, 0x91000000U | disp << 10 | (u32)b << 5 | 2); /* add x2, xb, #disp */
        arg(j, 3, ip->b, 0);
        arg(j, 4, ip->e, 0);
        emit(j, 0xf94012e0U); /* ldr x0, [x23, #32] */
        movi(j, 1, ip->aux);
        call(j, (void *)mc_atom, ip->d);
        break;
    default:
        return 0;
    }
    return 1;
}

/* ---- which values are wanted after their basic block ---- */

static void use_items(J *j, s32 *it, int n)
{
    for (; n > 0; n--, it += 3)
        if (!it[0])
            use(j, it[1]);
}

/* (pass 1) What an instruction reads that is not translated, or that reads the frame itself. 0: not known. */
static int model(J *j, Ins *ip)
{
    McFunc *f = j->f;
    s32 *x;
    int i;
    switch (ip->op) {
    case OP_EXIT:
    case OP_TRAP:
    case OP_BADFUNC:
        return 1;
    case OP_BRX:
        use(j, ip->a);
        return 1;
    case OP_RET:
        for (i = 0; i < f->nrets; i++)
            if (f->rets[i].isreg)
                use(j, f->rets[i].slot);
        return 1;
    case OP_CALLI:
        use(j, ip->b);
        /* fall through */
    case OP_CALL:
    case OP_CALLB:
        x = f->xtra + ip->a;
        use_items(j, x + 4 + 3 * x[2], x[3]);
        return 1;
    case OP_BAR:
        use(j, ip->a);
        use(j, ip->b);
        use(j, ip->c); /* (bar.red) */
        return 1;
    case OP_WSYNC:
        use(j, ip->a);
        use(j, ip->b);
        use(j, ip->c);
        if ((u32)ip->e < f->nxtra)
            use(j, f->xtra[ip->e]); /* the mask of the threads that take part */
        if (ip->aux == WS_MMA) {
            /* the registers of C, A, B, C again and of the scales, after ten words */
            s32 q, n;
            x = f->xtra + ip->e;
            if (x[7] < 0 || x[7] > 64 || x[8] < 0 || x[8] > 64 || x[9] < 0 || x[9] > 64)
                return 0;
            n = 10 + 2 * x[9] + x[7] + x[8] + 16;
            for (q = 10; q < n && (u32)(ip->e + q) < f->nxtra; q++)
                use(j, x[q]);
        }
        return 1;
    }
    return 0;
}

typedef struct Lv {
    u32 n, W;
    u32 *bof;  /* the block of an instruction */
    u64 *lin;  /* per block: what is wanted at its start */
} Lv;

static void lv_or(Lv *v, u64 *out, s64 target)
{
    u32 w;
    u64 *in;
    if (target < 0 || target >= (s64)v->n)
        return;
    in = v->lin + (size_t)v->bof[target] * v->W;
    for (w = 0; w < v->W; w++)
        out[w] |= in[w];
}

/* what is wanted after the instruction e, the last of a block */
static void lv_out(J *j, Lv *v, u32 e, u64 *out)
{
    McFunc *f = j->f;
    Ins *ip = &f->code[e];
    memset(out, 0, (size_t)v->W * 8);
    switch (ip->op) {
    case OP_BRA:
        lv_or(v, out, (s64)e + ip->a);
        return;
    case OP_BRT:
    case OP_BRF:
        lv_or(v, out, (s64)e + ip->b);
        break;
    case OP_SKIPF:
    case OP_SKIPT:
        lv_or(v, out, (s64)e + 1 + ip->b);
        break;
    case OP_BRX: {
        s32 *x = f->xtra + ip->e, k;
        for (k = 0; k < x[0]; k++)
            lv_or(v, out, x[1 + k]);
        return;
    }
    case OP_RET:
    case OP_EXIT:
    case OP_TRAP:
    case OP_BADFUNC:
        return;
    }
    lv_or(v, out, (s64)e + 1);
}

/* From what pass 1 found: last[] and adead[]. If this gives up, everything counts as wanted, as it did. */
static void liveness(J *j)
{
    McFunc *f = j->f;
    Lv v;
    u32 n = f->ncode, nr = f->nreg, W = (nr + 63) / 64, nb = 0, i, b, w, k, nall;
    u8 *bl = 0, *hasall = 0;
    u32 *bstart = 0, *stamp = 0, *kcnt = 0, *alls = 0;
    u64 *gen = 0, *kill = 0, *out = 0;
    s32 *st = 0;
    int changed;
    memset(&v, 0, sizeof v);
    if (!nr || !n)
        return;
    bl = (u8 *)calloc(n + 2, 1);
    v.bof = (u32 *)malloc((size_t)n * 4);
    if (!bl || !v.bof)
        goto done;
    for (i = 0; i < n; i++) {
        if (j->lead[i])
            bl[i] = 1;
        switch (f->code[i].op) {
        case OP_BRA:
        case OP_BRT:
        case OP_BRF:
        case OP_SKIPF:
        case OP_SKIPT:
        case OP_BRX:
        case OP_RET:
        case OP_EXIT:
        case OP_TRAP:
        case OP_BADFUNC:
            bl[i + 1] = 1;
            break;
        }
    }
    bl[0] = 1;
    for (i = 0; i < n; i++) {
        nb += bl[i];
        v.bof[i] = nb - 1;
    }
    if ((u64)nb * W > 0x400000)
        goto done;
    bstart = (u32 *)malloc((size_t)(nb + 1) * 4);
    hasall = (u8 *)calloc(nb, 1);
    v.lin = (u64 *)calloc((size_t)nb * W, 8);
    gen = (u64 *)calloc((size_t)nb * W, 8);
    kill = (u64 *)calloc((size_t)nb * W, 8);
    out = (u64 *)malloc((size_t)W * 8);
    st = (s32 *)malloc((size_t)nr * sizeof(s32));
    stamp = (u32 *)calloc(nr, 4);
    kcnt = (u32 *)malloc((size_t)nr * 4);
    alls = (u32 *)malloc((size_t)n * 4);
    if (!bstart || !hasall || !v.lin || !gen || !kill || !out || !st || !stamp || !kcnt || !alls)
        goto done;
    v.n = n;
    v.W = W;
    for (i = 0, b = 0; i < n; i++)
        if (bl[i])
            bstart[b++] = i;
    bstart[nb] = n;
    for (b = 0; b < nb; b++) {
        u64 *g = gen + (size_t)b * W, *kl = kill + (size_t)b * W;
        for (i = bstart[b]; i < bstart[b + 1] && !hasall[b]; i++) {
            s32 d = j->defk[i];
            for (k = j->ustart[i]; k < j->ustart[i + 1]; k++) {
                s32 u = j->us[k];
                if (!(kl[u >> 6] >> (u & 63) & 1))
                    g[u >> 6] |= (u64)1 << (u & 63);
            }
            if (j->iall[i])
                hasall[b] = 1;
            else if (d != NOSL)
                kl[d >> 6] |= (u64)1 << (d & 63);
        }
    }
    do {
        changed = 0;
        for (b = nb; b-- > 0;) {
            u64 *g = gen + (size_t)b * W, *kl = kill + (size_t)b * W, *in = v.lin + (size_t)b * W;
            lv_out(j, &v, bstart[b + 1] - 1, out);
            for (w = 0; w < W; w++) {
                u64 x = g[w] | ((hasall[b] ? ~(u64)0 : out[w]) & ~kl[w]);
                if (x != in[w]) {
                    in[w] = x;
                    changed = 1;
                }
            }
        }
    } while (changed);
    for (b = 0; b < nb; b++) {
        lv_out(j, &v, bstart[b + 1] - 1, out);
        nall = 0;
        for (i = bstart[b + 1]; i-- > bstart[b];) {
            Ins *ip = &f->code[i];
            s32 d = j->defk[i], a = ip->a;
#define LV_INIT(s) \
    if (stamp[s] != b + 1) { \
        stamp[s] = b + 1; \
        st[s] = (out[(s) >> 6] >> ((s) & 63) & 1) ? INF : -1; \
        kcnt[s] = 0; \
    }
            if (d != NOSL && !j->iall[i]) {
                s32 l;
                LV_INIT(d)
                l = st[d];
                if (l != INF && nall > kcnt[d] && (s32)alls[kcnt[d]] > l)
                    l = (s32)alls[kcnt[d]];
                j->last[i] = l;
                st[d] = -1;
                kcnt[d] = nall;
            }
            if ((ip->op == OP_MOV || ip->op == OP_MOV32) && a >= 0 && (u32)a < nr && a != d && d != NOSL) {
                LV_INIT(a)
                j->adead[i] = (u8)(st[a] == -1 && kcnt[a] == nall);
            }
            for (k = j->ustart[i]; k < j->ustart[i + 1]; k++) {
                s32 u = j->us[k];
                LV_INIT(u)
                if (st[u] == -1)
                    st[u] = (s32)i;
            }
            if (j->iall[i])
                alls[nall++] = i;
        }
    }
#undef LV_INIT
    j->lv_lin = v.lin;
    j->lv_bof = v.bof;
    j->lv_W = W;
    v.lin = 0;
    v.bof = 0;
done:
    free(bl);
    free(v.bof);
    free(v.lin);
    free(bstart);
    free(hasall);
    free(gen);
    free(kill);
    free(out);
    free(st);
    free(stamp);
    free(kcnt);
    free(alls);
}

/* The end of pass 1 or 2: what the registers held at the branches back becomes what the loops of the next
   pass keep in them; of that, only what is wanted at the head of the loop is worth it. */
static void loops_next(J *j)
{
    u32 i;
    int k;
    if (!j->bnew)
        return;
    for (i = 0; i < j->f->ncode; i++)
        if (j->bhead[i] >= 0) {
            Cache *p = &j->bst[j->bhead[i]];
            if (!j->bseen[j->bhead[i]])
                continue;
            *p = j->bnew[j->bhead[i]];
            if (j->lv_lin) {
                u64 *in = j->lv_lin + (size_t)j->lv_bof[i] * j->lv_W;
                for (k = 4; k < 16; k++)
                    if (p->xs[k] != NOSL && p->xs[k] >= 0 && !(in[p->xs[k] >> 6] >> (p->xs[k] & 63) & 1))
                        p->xs[k] = NOSL;
                for (k = 3; k < 24; k++)
                    if (p->vs[k] != NOSL && p->vs[k] >= 0 && !(in[p->vs[k] >> 6] >> (p->vs[k] & 63) & 1))
                        p->vs[k] = NOSL;
            }
        }
    memset(j->bseen, 0, j->nbst);
}

static int jit_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("MCUDA_JIT");
        mode = e && *e ? atoi(e) : 1;
        if (mode < 0)
            mode = 0;
    }
    return mode;
}

void mc_jit_free(McFunc *f)
{
    McJit *jt = f->jit;
    if (!jt)
        return;
    f->jit = 0;
    munmap((void *)jt->code, (size_t)jt->size);
    free(jt->off);
    free(jt);
}

void mc_jit_compile(McFunc *f)
{
    J jb, *j = &jb;
    u32 i, nun = 0, nun2, size;
    static u32 cnt[OP_COUNT];
    McJit *jt = 0;
    void *mem;
    int mode = jit_mode();
    if (!mode || f->jit || !f->code || !f->ncode)
        return;
    memset(j, 0, sizeof *j);
    j->f = f;
    j->off = (u32 *)malloc((size_t)f->ncode * 4);
    j->lead = (u8 *)calloc(f->ncode + 1, 1);
    j->fixed = (u8 *)calloc(f->nconst + 1, 1);
    j->snhead = (s32 *)malloc((size_t)(f->ncode + 1) * sizeof(s32));
    j->ustart = (u32 *)malloc((size_t)(f->ncode + 1) * 4);
    j->defk = (s32 *)malloc((size_t)(f->ncode + 1) * sizeof(s32));
    j->last = (s32 *)malloc((size_t)(f->ncode + 1) * sizeof(s32));
    j->iall = (u8 *)calloc(f->ncode + 1, 1);
    j->adead = (u8 *)calloc(f->ncode + 1, 1);
    j->bhead = (s32 *)malloc((size_t)(f->ncode + 1) * sizeof(s32));
    j->boff = (u32 *)malloc((size_t)(f->ncode + 1) * 4);
    if (!j->off || !j->lead || !j->fixed || !j->snhead || !j->ustart || !j->defk || !j->last || !j->iall || !j->adead ||
        !j->bhead || !j->boff)
        goto done;
    for (i = 0; i <= f->ncode; i++) {
        j->defk[i] = NOSL;
        j->last[i] = INF;
        j->bhead[i] = -1;
    }
    for (i = 0; i < f->nfix; i++)
        if (f->fix[i].slot < 0 && (u32)(-1 - f->fix[i].slot) < f->nconst)
            j->fixed[-1 - f->fix[i].slot] = (u8)f->fix[i].kind;
    /* where control may arrive from elsewhere */
    j->lead[0] = 2;
    for (i = 0; i < f->ncode; i++) {
        Ins *ip = &f->code[i];
        s64 t = -1;
        switch (ip->op) {
        case OP_BRA:
            t = (s64)i + ip->a;
            break;
        case OP_BRT:
        case OP_BRF:
            t = (s64)i + ip->b;
            break;
        case OP_SKIPF:
        case OP_SKIPT:
            t = (s64)i + 1 + ip->b;
            break;
        case OP_BRX: {
            s32 *x = f->xtra + ip->e, k;
            for (k = 0; k < x[0]; k++)
                if (x[1 + k] >= 0 && (u32)x[1 + k] < f->ncode)
                    j->lead[x[1 + k]] = 2;
            break;
        }
        default:
            continue;
        }
        if (ip->op != OP_BRX && (t < 0 || t >= (s64)f->ncode))
            goto done; /* not something to translate */
        if (t >= 0 && (t <= (s64)i || !j->lead[t]))
            j->lead[t] = (u8)(t <= (s64)i ? 2 : 1);
    }
    /* the way in: mc_jit_enter(R, ctx, code) */
    emit(j, 0xa9bb7bfdU); /* stp x29, x30, [sp, #-80]! */
    emit(j, 0xa90153f3U); /* stp x19, x20, [sp, #16] */
    emit(j, 0xa9025bf5U); /* stp x21, x22, [sp, #32] */
    emit(j, 0xa90363f7U); /* stp x23, x24, [sp, #48] */
    emit(j, 0xa9046bf9U); /* stp x25, x26, [sp, #64] */
    emit(j, 0xaa0003f3U); /* mov x19, x0 */
    emit(j, 0xaa0103f7U); /* mov x23, x1 */
    emit(j, 0xf9400034U); /* ldr x20, [x1] */
    emit(j, 0xf9400435U); /* ldr x21, [x1, #8] */
    emit(j, 0xf9400c38U); /* ldr x24, [x1, #24] */
    emit(j, 0xd2800016U); /* mov x22, #0 */
    emit(j, 0xaa0203f9U); /* mov x25, x2 */
    emit(j, 0xd61f0040U); /* br x2 */
    /* the way out at a warp instruction: the next thread of the warp, if the caller gave one (McJitCtx) */
    j->wexit_at = j->n;
    emit(j, 0xf9401ae1U); /* ldr x1, [x23, #48]: lane */
    emit(j, 0x910902e2U); /* add x2, x23, #576: ex */
    emit(j, 0xb8217840U); /* str w0, [x2, x1, lsl #2] */
    emit(j, 0x91000421U); /* add x1, x1, #1 */
    emit(j, 0xf9001ae1U); /* str x1, [x23, #48] */
    emit(j, 0xf9401ee2U); /* ldr x2, [x23, #56]: nl */
    emit(j, 0xeb02003fU); /* cmp x1, x2 */
    emit(j, 0x540000e2U); /* b.hs the way out */
    emit(j, 0x910102e2U); /* add x2, x23, #64: R */
    emit(j, 0xf8617853U); /* ldr x19, [x2, x1, lsl #3] */
    emit(j, 0x910502e2U); /* add x2, x23, #320: thrs */
    emit(j, 0xf8617843U); /* ldr x3, [x2, x1, lsl #3] */
    emit(j, 0xf90016e3U); /* str x3, [x23, #40]: thr */
    emit(j, 0xd61f0320U); /* br x25 */
    j->exit_at = j->n;
    emit(j, 0xf9000af6U); /* str x22, [x23, #16] */
    emit(j, 0xa94153f3U);
    emit(j, 0xa9425bf5U);
    emit(j, 0xa94363f7U);
    emit(j, 0xa9446bf9U); /* ldp x25, x26, [sp, #64] */
    emit(j, 0xa8c57bfdU); /* ldp x29, x30, [sp], #80 */
    emit(j, 0xd65f03c0U);
    for (j->pass = 1; j->pass <= 3 && !j->fail; j->pass++) {
        u32 n0 = j->n;
        for (i = 0; i <= f->ncode; i++)
            j->snhead[i] = -1;
        j->nsn = j->nst = j->nfx = j->nfl = 0;
        j->pend = 0;
        j->dead = 0;
        forget(j);
        block_end(j);
        for (i = 0; i < f->ncode && !j->fail; i++) {
            Ins *ip = &f->code[i];
            int lead = j->lead[i];
            u32 at;
            s32 s;
            if (lead || j->pend >= 4000)
                flush(j);
            if (lead)
                block_end(j);
            if (j->pass == 1) {
                j->ustart[i] = j->nus;
                j->rdef = NOSL;
                j->rmulti = 0;
            }
            /* what the registers hold here */
            if (lead == 2)
                forget(j);
            else if (lead) {
                s = j->snhead[i];
                if (j->dead && s >= 0) {
                    j->c = j->sn[s].c;
                    s = j->sn[s].next;
                } else if (j->dead)
                    forget(j);
                for (; s >= 0; s = j->sn[s].next)
                    meet(j, &j->sn[s].c);
            } else if (j->dead)
                forget(j);
            j->dead = 0;
            j->off[i] = j->n | (lead == 2 ? 0 : MC_JIT_NOENTER);
            if (lead == 2 && j->pass >= 2 && j->bhead[i] >= 0)
                preload(j, &j->bst[j->bhead[i]]);
            j->boff[i] = j->n;
            j->cur = i;
            j->xpin = j->vpin = 0;
            switch (ip->op) {
            case OP_BRA:
                j->pend++;
                flush(j);
                jump(j, 0x14000000U, (s32)i + ip->a);
                j->dead = 1;
                continue;
            case OP_BRT:
            case OP_BRF:
            case OP_SKIPT:
            case OP_SKIPF: {
                int r = uw(j, ip->a);
                j->pend++;
                flush(j);
                jump(j, (ip->op == OP_BRT || ip->op == OP_SKIPT ? 0x35000000U : 0x34000000U) | (u32)r,
                     ip->op == OP_BRT || ip->op == OP_BRF ? (s32)i + ip->b : (s32)i + 1 + ip->b);
                continue;
            }
            }
            at = j->n;
            if (one(j, ip)) {
                j->pend++;
                if (j->pass == 1) {
                    if (ip->op == OP_CALLB) {
                        j->nus = j->ustart[i];
                        model(j, ip);
                    } else if (!j->rmulti)
                        j->defk[i] = j->rdef;
                }
                continue;
            }
            j->n = at;
            if (j->pass == 1) {
                j->nus = j->ustart[i];
                if (!model(j, ip)) {
                    j->nus = j->ustart[i];
                    j->iall[i] = 1;
                }
            }
            sync(j);
            flush(j);
            leave(j, i, ip->op == OP_WSYNC);
            j->off[i] |= MC_JIT_NOENTER;
            j->lead[i + 1] = 2;
            j->dead = 1;
            if (j->pass == 3) {
                nun++;
                if (mode >= 3 && ip->op < OP_COUNT)
                    cnt[ip->op]++;
            }
        }
        if (j->pass == 2) {
            loops_next(j);
            j->n = n0;
        }
        if (j->pass == 1) {
            j->ustart[f->ncode] = j->nus;
            if (j->nbst) {
                j->bnew = (Cache *)malloc((size_t)j->nbst * sizeof(Cache));
                j->bseen = (u8 *)calloc(j->nbst, 1);
                if (!j->bnew || !j->bseen) {
                    free(j->bnew);
                    j->bnew = 0;
                }
            }
            liveness(j);
            j->n = n0;
        }
    }
    for (i = 0; i < j->nst && !j->fail; i++) {
        Stub *s = &j->st[i];
        j->w[s->at] |= ((j->n - s->at) & 0x7ffff) << 5;
        for (nun2 = 0; nun2 < s->nfl; nun2++) {
            Fl *q = &j->fl[s->fl + nun2];
            slot_op(j, q->isv ? STD : STX, 3, q->reg, q->slot);
        }
        j->pend = s->pend;
        flush(j);
        leave(j, s->ins, 0);
    }
    if (j->fail || j->n >= 0x38000)
        goto done; /* (conditional branches reach a megabyte) */
    for (i = 0; i < j->nfx; i++) {
        Fixup *x = &j->fx[i];
        u32 d = (x->body ? j->boff[x->target] : j->off[x->target] & ~MC_JIT_NOENTER) - x->at;
        if ((j->w[x->at] & 0xfc000000U) == 0x14000000U)
            j->w[x->at] |= d & 0x3ffffffU;
        else
            j->w[x->at] |= (d & 0x7ffff) << 5;
    }
    size = (j->n * 4 + 16383) & ~16383U;
    jt = (McJit *)calloc(1, sizeof(McJit));
    if (!jt)
        goto done;
#ifdef MC_JIT_DARWIN
    mem = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | 0x800 /* MAP_JIT */, -1, 0);
#else
    mem = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (mem == MAP_FAILED) {
        free(jt);
        goto done;
    }
#ifdef MC_JIT_DARWIN
    pthread_jit_write_protect_np(0);
    memcpy(mem, j->w, (size_t)j->n * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(mem, (size_t)j->n * 4);
#else
    memcpy(mem, j->w, (size_t)j->n * 4);
    mc_flush_icache(mem, (u8 *)mem + (size_t)j->n * 4);
#endif
    jt->code = (u32 *)mem;
    jt->size = size;
    jt->off = j->off;
    j->off = 0;
    f->jit = jt;
    if (mode >= 2) {
        fprintf(stderr, "mcuda jit: %.80s: %u instructions, %u left to the interpreter, %u bytes of code\n", f->name,
                f->ncode, nun, j->n * 4);
        if (mode >= 3) {
            for (i = 0; i < OP_COUNT; i++)
                if (cnt[i]) {
                    fprintf(stderr, "    op %u: %u\n", i, cnt[i]);
                    cnt[i] = 0;
                }
        }
        if (mode >= 4) {
            char nm[200];
            FILE *fp;
            sprintf(nm, "jit-%.150s.bin", f->name);
            fp = fopen(nm, "wb");
            if (fp) {
                fwrite(j->w, 4, j->n, fp);
                fclose(fp);
            }
        }
    }
done:
    free(j->w);
    free(j->off);
    free(j->lead);
    free(j->fixed);
    free(j->snhead);
    free(j->sn);
    free(j->st);
    free(j->fx);
    free(j->fl);
    free(j->ustart);
    free(j->us);
    free(j->defk);
    free(j->iall);
    free(j->last);
    free(j->adead);
    free(j->bhead);
    free(j->bst);
    free(j->boff);
    free(j->bnew);
    free(j->bseen);
    free(j->lv_lin);
    free(j->lv_bof);
}
#endif
