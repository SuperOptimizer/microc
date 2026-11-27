/* A translator from decoded PTX instructions to x86-64 code (Linux), with the contract of jit.c's for arm64: the
   code runs on the thread's frame and leaves, with the index of an instruction, wherever the interpreter is to go
   on; what an instruction computes is, bit for bit, what the interpreter computes for it.

   Simpler than jit.c: one template per instruction from the frame to the frame (no values kept in registers
   across instructions), and only the instructions most kernels spend their time in: integer arithmetic, moves,
   comparisons, selections, branches, loads and stores (generic and shared), and float addition, subtraction,
   multiplication, fused multiply-add and the plain conversions. Everything else, the warp instructions included,
   leaves the code; so does a translated instruction in its rare cases (an address below 4096, a float result
   that is a NaN), and the interpreter then does it.

   The code may be entered at the head of a straight run of instructions (instruction 0, a branch target, the
   instruction after a branch or after one that is not translated), so that what it counts (McJitCtx.n) is what
   it did: each run adds its length to r13 where it leaves or branches.

   Registers: rbx the frame, r12 the McJitCtx, r13 instructions executed, r14 and r15 the block's shared memory
   and its size; rax, rcx, rdx, xmm0..xmm2 are scratch. Within a run, a result that a later instruction of the run
   names is also kept in one of rsi, rdi, r8..r11, rbp, and read from there (the frame is written all the same, so
   leaving needs nothing; MCUDA_JIT_NOCACHE=1: not). fma is the processor's (FMA3) where it has one
   (MCUDA_JIT_NOFMA=1: the interpreter's function). MCUDA_JIT=0 turns it off; =2 says what was translated; =3 writes the code to files as well. */
#include "ptx.h"

#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>

typedef struct XStub {
    u32 at;   /* the rel32 of the branch to it */
    u32 ins;  /* the instruction the interpreter is to do */
    u32 pend; /* instructions done before it in its run */
} XStub;

typedef struct XFix {
    u32 at;     /* a rel32 */
    u32 target; /* instruction index */
} XFix;

typedef struct X {
    McFunc *f;
    u8 *p;
    u32 n, cap;
    int over;
    u32 *off;
    u8 *lead, *fixed;
    XStub *st;
    u32 nst, capst;
    XFix *fx;
    u32 nfx, capfx;
    u32 pend; /* instructions of the current run not yet added to r13 */
    s32 cs[16]; /* the slot whose value a register of the pool also holds (NOCS: none) */
    u32 cur;    /* the instruction being translated */
    u32 rr;     /* the pool's next victim */
    int nocache;
    u32 epi;  /* where the way out is */
} X;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
#define NOCS 0x7fffffff
/* registers that keep copies of slots within a run (every result is stored in the frame all the same) */
static const u8 pool[7] = {RSI, RDI, R8, R9, R10, R11, RBP};

enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

static void b1(X *x, u32 v)
{
    if (x->n >= x->cap) {
        x->over = 1;
        return;
    }
    x->p[x->n++] = (u8)v;
}

static void b4(X *x, u32 v)
{
    b1(x, v);
    b1(x, v >> 8);
    b1(x, v >> 16);
    b1(x, v >> 24);
}

static void put4(X *x, u32 at, u32 v)
{
    if (at + 4 <= x->cap) {
        x->p[at] = (u8)v;
        x->p[at + 1] = (u8)(v >> 8);
        x->p[at + 2] = (u8)(v >> 16);
        x->p[at + 3] = (u8)(v >> 24);
    }
}

/* REX (when needed) and the opcode bytes, for reg and an r/m that is a register (mod 3) or [base + disp32] */
static void rex(X *x, int w, int reg, int rm, int force)
{
    int r = (w ? 8 : 0) | (reg & 8 ? 4 : 0) | (rm & 8 ? 1 : 0);
    if (r || force)
        b1(x, 0x40 | r);
}

static void opc(X *x, u32 op)
{
    if (op > 0xffff)
        b1(x, op >> 16);
    if (op > 0xff)
        b1(x, op >> 8);
    b1(x, op);
}

/* op reg, r/m reg (pfx: a mandatory prefix such as 0x66, 0xf3 or 0, written before REX) */
static void rr(X *x, int pfx, int w, u32 op, int reg, int rm)
{
    if (pfx)
        b1(x, pfx);
    rex(x, w, reg, rm, 0);
    opc(x, op);
    b1(x, 0xc0 | (reg & 7) << 3 | (rm & 7));
}

/* op reg, [base + disp] (base not rsp or r12) */
static void rm(X *x, int pfx, int w, u32 op, int reg, int base, s32 disp)
{
    if (pfx)
        b1(x, pfx);
    rex(x, w, reg, base, 0);
    opc(x, op);
    if (disp == 0 && (base & 7) != RBP) {
        b1(x, (reg & 7) << 3 | (base & 7));
    } else if (disp >= -128 && disp < 128) {
        b1(x, 0x40 | (reg & 7) << 3 | (base & 7));
        b1(x, (u32)disp);
    } else {
        b1(x, 0x80 | (reg & 7) << 3 | (base & 7));
        b4(x, (u32)disp);
    }
}

static s32 sdisp(s32 slot)
{
    return slot * 8;
}

/* is slot a constant of the function, the same in every frame? */
static int kc(X *x, s32 slot, u64 *v)
{
    McFunc *f = x->f;
    if (slot >= 0 || (u32)(-1 - slot) >= f->nconst || x->fixed[-1 - slot])
        return 0;
    *v = f->ctemplate[f->nconst - 1 - (-1 - slot)].u;
    return 1;
}

static void movi(X *x, int r, u64 v, int w)
{
    if (!w || v <= 0xffffffffUL) {
        rex(x, 0, 0, r, 0);
        b1(x, 0xb8 | (r & 7));
        b4(x, (u32)v);
    } else if ((u64)(s64)(s32)v == v) {
        rex(x, 1, 0, r, 0);
        b1(x, 0xc7);
        b1(x, 0xc0 | (r & 7));
        b4(x, (u32)v);
    } else {
        rex(x, 1, 0, r, 0);
        b1(x, 0xb8 | (r & 7));
        b4(x, (u32)v);
        b4(x, (u32)(v >> 32));
    }
}

static void forget(X *x)
{
    int k;
    for (k = 0; k < 16; k++)
        x->cs[k] = NOCS;
}

/* register r is about to be overwritten */
static void clobber(X *x, int r)
{
    x->cs[r] = NOCS;
}

static int cached(X *x, s32 slot)
{
    int k;
    for (k = 0; k < 7; k++)
        if (x->cs[pool[k]] == slot)
            return pool[k];
    return -1;
}

/* r = the slot, 64 bits (w) or its low 32 zero-extended */
static void ld(X *x, int r, s32 slot, int w)
{
    u64 v;
    int c;
    if (kc(x, slot, &v))
        movi(x, r, w ? v : (u32)v, w);
    else if ((c = cached(x, slot)) >= 0) {
        if (c != r || !w)
            rr(x, 0, w, 0x8b, r, c);
    } else
        rm(x, 0, w, 0x8b, r, RBX, sdisp(slot));
}

/* does an instruction after the current one, in its run, name the slot (before setting it)? */
static int wanted(X *x, s32 slot)
{
    McFunc *f = x->f;
    u32 j;
    for (j = x->cur + 1; j < f->ncode && !x->lead[j] && j < x->cur + 64; j++) {
        Ins *q = &f->code[j];
        if (q->a == slot || q->b == slot || q->c == slot)
            return 1;
        if (q->op == OP_STV || q->op == OP_STV_S) {
            s32 *xs = f->xtra + q->e;
            u32 e;
            for (e = 0; e < (q->aux & 15u) && e < 4; e++)
                if (xs[e] == slot)
                    return 1;
        }
        if (q->d == slot && q->op != OP_STV && q->op != OP_STV_S && !(q->op >= OP_ST8 && q->op <= OP_ST64) &&
            !(q->op >= OP_ST8_S && q->op <= OP_ST64_S))
            return 0;
    }
    return 0;
}

/* the slot = r, all 64 bits (a 32-bit result is in r zero-extended); a register of the pool keeps a copy */
static void stv(X *x, int r, s32 slot)
{
    int k, v;
    rm(x, 0, 1, 0x89, r, RBX, sdisp(slot));
    for (k = 0; k < 7; k++)
        if (x->cs[pool[k]] == slot)
            x->cs[pool[k]] = NOCS;
    if (x->nocache || !wanted(x, slot))
        return;
    for (k = 0; k < 7 && pool[k] != r; k++)
        ;
    if (k < 7) {
        x->cs[r] = slot;
        return;
    }
    v = pool[x->rr++ % 7];
    rr(x, 0, 1, 0x8b, v, r); /* mov v, r */
    x->cs[v] = slot;
}

static void ldx(X *x, int xr, s32 slot, int dbl)
{
    int c = cached(x, slot);
    if (c >= 0)
        rr(x, 0x66, dbl, 0x0f6e, xr, c); /* movd / movq xmm, c */
    else
        rm(x, dbl ? 0xf2 : 0xf3, 0, 0x0f10, xr, RBX, sdisp(slot)); /* movss / movsd (constants are in the frame too) */
}

/* to stub of instruction i, if condition cc */
static void jcc_stub(X *x, int cc, u32 ins)
{
    if (x->nst == x->capst) {
        x->capst = x->capst ? 2 * x->capst : 64;
        x->st = (XStub *)realloc(x->st, x->capst * sizeof(XStub));
        if (!x->st) {
            x->over = 1;
            x->capst = x->nst = 0;
            return;
        }
    }
    b1(x, 0x0f);
    b1(x, 0x80 | cc);
    x->st[x->nst].at = x->n;
    x->st[x->nst].ins = ins;
    x->st[x->nst].pend = x->pend;
    x->nst++;
    b4(x, 0);
}

/* jump (cc < 0: always) to the code of instruction t */
static void jto(X *x, int cc, u32 t)
{
    if (x->nfx == x->capfx) {
        x->capfx = x->capfx ? 2 * x->capfx : 64;
        x->fx = (XFix *)realloc(x->fx, x->capfx * sizeof(XFix));
        if (!x->fx) {
            x->over = 1;
            x->capfx = x->nfx = 0;
            return;
        }
    }
    if (cc < 0) {
        b1(x, 0xe9);
    } else {
        b1(x, 0x0f);
        b1(x, 0x80 | cc);
    }
    x->fx[x->nfx].at = x->n;
    x->fx[x->nfx].target = t;
    x->nfx++;
    b4(x, 0);
}

/* r13 += pend (lea: the flags stay) */
static void count(X *x)
{
    if (!x->pend)
        return;
    b1(x, 0x4d);
    b1(x, 0x8d);
    b1(x, 0xad);
    b4(x, x->pend);
    x->pend = 0;
}

/* leave for the interpreter at instruction i */
static void leave(X *x, u32 i)
{
    count(x);
    movi(x, RAX, i, 0);
    b1(x, 0xe9);
    b4(x, x->epi - (x->n + 4));
}

/* d = a op b, integer, 32 or 64 bits (op: the "op r/m, reg" opcode: add 01, sub 29, and 21, or 09, xor 31) */
static void ibin(X *x, Ins *ip, int w, u32 op)
{
    ld(x, RAX, ip->a, w);
    ld(x, RCX, ip->b, w);
    rr(x, 0, w, op, RCX, RAX);
    stv(x, RAX, ip->d);
}

static void imul(X *x, Ins *ip, int w, int add)
{
    ld(x, RAX, ip->a, w);
    ld(x, RCX, ip->b, w);
    rr(x, 0, w, 0x0faf, RAX, RCX);
    if (add) {
        ld(x, RCX, ip->c, w);
        rr(x, 0, w, 0x01, RCX, RAX);
    }
    stv(x, RAX, ip->d);
}

/* d = a shifted by b, 0 (or the sign) once b is the width or more; kind 4 shl, 5 shr, 7 sar */
static void ishift(X *x, Ins *ip, int w, int kind)
{
    u64 k;
    u32 bits = w ? 64 : 32;
    ld(x, RAX, ip->a, w);
    if (kc(x, ip->b, &k)) {
        k = (u32)k;
        if (k >= bits && kind != 7) {
            rr(x, 0, 0, 0x31, RAX, RAX); /* xor eax, eax */
        } else {
            if (k >= bits)
                k = bits - 1;
            if (k) {
                rex(x, w, 0, RAX, 0);
                b1(x, 0xc1);
                b1(x, 0xc0 | kind << 3);
                b1(x, (u32)k);
            }
        }
    } else {
        ld(x, RCX, ip->b, 0);
        if (kind == 7) {
            movi(x, RDX, bits - 1, 0);
            rex(x, 0, 0, 0, 0);
            b1(x, 0x81); /* cmp ecx, bits */
            b1(x, 0xf9);
            b4(x, bits);
            rr(x, 0, 0, 0x0f40 | CC_AE, RCX, RDX); /* cmovae ecx, edx */
            rex(x, w, 0, RAX, 0);
            b1(x, 0xd3);
            b1(x, 0xc0 | 7 << 3);
        } else {
            rr(x, 0, 0, 0x31, RDX, RDX);
            rex(x, w, 0, RAX, 0);
            b1(x, 0xd3);
            b1(x, 0xc0 | kind << 3);
            b1(x, 0x81);
            b1(x, 0xf9);
            b4(x, bits);
            rr(x, 0, w, 0x0f40 | CC_AE, RAX, RDX); /* cmovae rax, rdx */
        }
    }
    stv(x, RAX, ip->d);
}

/* d = cc (a, b) ? b : a  (the minimum and maximum: cc is when b is taken) */
static void iminmax(X *x, Ins *ip, int w, int cc)
{
    ld(x, RAX, ip->a, w);
    ld(x, RCX, ip->b, w);
    rr(x, 0, w, 0x39, RCX, RAX); /* cmp rax, rcx */
    rr(x, 0, w, 0x0f40 | cc, RAX, RCX);
    stv(x, RAX, ip->d);
}

/* d = the condition, 0 or 1 */
static void setcc(X *x, int cc, s32 d)
{
    b1(x, 0x0f);
    b1(x, 0x90 | cc);
    b1(x, 0xc0); /* al */
    rr(x, 0, 0, 0x0fb6, RAX, RAX); /* movzx eax, al */
    stv(x, RAX, d);
}

static void icmp(X *x, Ins *ip, int w, int sgn, int cmp)
{
    static const u8 ucc[6] = {CC_E, CC_NE, CC_B, CC_BE, CC_A, CC_AE}, scc[6] = {CC_E, CC_NE, CC_L, CC_LE, CC_G, CC_GE};
    ld(x, RCX, ip->a, w);
    ld(x, RDX, ip->b, w);
    rr(x, 0, w, 0x39, RDX, RCX); /* cmp rcx, rdx */
    setcc(x, sgn ? scc[cmp] : ucc[cmp], ip->d);
}

/* setp with floats: ucomiss in the order that makes the condition one flag test (or two) */
static int fcmp(X *x, Ins *ip, int dbl, int cmp)
{
    int swap = 0, cc = -1, cc2 = -1, and2 = 0;
    switch (cmp) {
    case CMP_EQ: cc = CC_E; cc2 = CC_NP; and2 = 1; break;
    case CMP_NE: cc = CC_NE; break;
    case CMP_LT: cc = CC_A; swap = 1; break;
    case CMP_LE: cc = CC_AE; swap = 1; break;
    case CMP_GT: cc = CC_A; break;
    case CMP_GE: cc = CC_AE; break;
    case CMP_EQU: cc = CC_E; break;
    case CMP_NEU: cc = CC_NE; cc2 = CC_P; break;
    case CMP_LTU: cc = CC_B; break;
    case CMP_LEU: cc = CC_BE; break;
    case CMP_GTU: cc = CC_B; swap = 1; break;
    case CMP_GEU: cc = CC_BE; swap = 1; break;
    case CMP_NUM: cc = CC_NP; break;
    case CMP_NAN: cc = CC_P; break;
    default: return 0;
    }
    ldx(x, 0, swap ? ip->b : ip->a, dbl);
    ldx(x, 1, swap ? ip->a : ip->b, dbl);
    rr(x, dbl ? 0x66 : 0, 0, 0x0f2e, 0, 1); /* ucomiss xmm0, xmm1 */
    b1(x, 0x0f);
    b1(x, 0x90 | cc);
    b1(x, 0xc0); /* al */
    if (cc2 >= 0) {
        b1(x, 0x0f);
        b1(x, 0x90 | cc2);
        b1(x, 0xc1); /* cl */
        b1(x, and2 ? 0x20 : 0x08); /* and / or al, cl */
        b1(x, 0xc8);
    }
    rr(x, 0, 0, 0x0fb6, RAX, RAX);
    stv(x, RAX, ip->d);
    return 1;
}

/* xmm0 holds a float result: if it is a NaN, the interpreter does instruction i (nothing has been written) */
static void nan_leaves(X *x, int dbl, u32 i)
{
    rr(x, dbl ? 0x66 : 0, 0, 0x0f2e, 0, 0); /* ucomiss xmm0, xmm0 */
    jcc_stub(x, CC_P, i);
}

static void stx(X *x, int dbl, s32 d)
{
    rr(x, 0x66, dbl, 0x0f7e, 0, RAX); /* movd eax, xmm0 / movq rax, xmm0 (a float zero-extended, as SETF) */
    stv(x, RAX, d);
}

/* d = a op b (op: 58 add, 59 mul, 5c sub) */
static void fbin(X *x, Ins *ip, int dbl, u32 op, u32 i)
{
    ldx(x, 0, ip->a, dbl);
    ldx(x, 1, ip->b, dbl);
    rr(x, dbl ? 0xf2 : 0xf3, 0, 0x0f00 | op, 0, 1);
    nan_leaves(x, dbl, i);
    stx(x, dbl, ip->d);
}

/* d = fn (a, b, c), the interpreter's own function (mc_fmaf, mc_fma) */
static void fcall3(X *x, Ins *ip, int dbl, void *fn, u32 i)
{
    ldx(x, 0, ip->a, dbl);
    ldx(x, 1, ip->b, dbl);
    ldx(x, 2, ip->c, dbl);
    movi(x, RAX, (u64)fn, 1);
    b1(x, 0xff); /* call rax */
    b1(x, 0xd0);
    forget(x); /* (the call keeps none of the pool but rbp: simpler to forget all) */
    nan_leaves(x, dbl, i);
    stx(x, dbl, ip->d);
}

/* the processor's fused multiply-add (FMA3, with the operating system saving the ymm state) */
static int has_fma(void)
{
    static int known = -1;
    unsigned a = 1, b, c = 0, d, lo, hi;
    if (known >= 0)
        return known;
    known = 0;
    if (getenv("MCUDA_JIT_NOFMA"))
        return 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(c >> 12 & 1) || !(c >> 27 & 1) || !(c >> 28 & 1))
        return 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    (void)b;
    (void)d;
    (void)hi;
    known = (lo & 6) == 6;
    return known;
}

/* d = a * b + c by vfmadd231: the same as the interpreter's correctly rounded mc_fmaf / mc_fma where the result
   is a normal number or a zero; a subnormal, an infinity or a NaN (where the interpreter's rounding is its own
   affair) leaves for the interpreter */
static void hwfma(X *x, Ins *ip, int dbl, u32 i)
{
    u32 zj;
    ldx(x, 0, ip->c, dbl);
    ldx(x, 1, ip->a, dbl);
    ldx(x, 2, ip->b, dbl);
    b1(x, 0xc4); /* vfmadd231ss / sd xmm0, xmm1, xmm2 */
    b1(x, 0xe2);
    b1(x, dbl ? 0xf1 : 0x71);
    b1(x, 0xb9);
    b1(x, 0xc2);
    rr(x, 0x66, dbl, 0x0f7e, 0, RAX); /* movd / movq rax, xmm0 */
    if (dbl) {
        movi(x, RCX, 0x7fffffffffffffffUL, 1);
        rr(x, 0, 1, 0x21, RAX, RCX); /* and rcx, rax */
        b1(x, 0x74); /* jz done: a zero is the same either way */
        zj = x->n;
        b1(x, 0);
        movi(x, RDX, 0x0010000000000000UL, 1);
        rr(x, 0, 1, 0x29, RDX, RCX); /* sub rcx, rdx */
        movi(x, RDX, 0x7fe0000000000000UL, 1);
        rr(x, 0, 1, 0x39, RDX, RCX); /* cmp rcx, rdx */
    } else {
        rr(x, 0, 0, 0x89, RAX, RCX); /* mov ecx, eax */
        b1(x, 0x81); /* and ecx, 0x7fffffff */
        b1(x, 0xe1);
        b4(x, 0x7fffffffU);
        b1(x, 0x74); /* jz done */
        zj = x->n;
        b1(x, 0);
        b1(x, 0x81); /* sub ecx, 0x800000 */
        b1(x, 0xe9);
        b4(x, 0x800000U);
        b1(x, 0x81); /* cmp ecx, 0x7f000000 */
        b1(x, 0xf9);
        b4(x, 0x7f000000U);
    }
    jcc_stub(x, CC_AE, i);
    if (zj < x->cap)
        x->p[zj] = (u8)(x->n - (zj + 1));
    stv(x, RAX, ip->d);
}

/* int to float: from a, 32 bits signed (sx) or unsigned (taken as 64 bits signed) */
static void i2f(X *x, Ins *ip, int dbl, int sx)
{
    if (sx)
        rm(x, 0, 1, 0x63, RAX, RBX, sdisp(ip->a)); /* movsxd rax, [a] */
    else
        ld(x, RAX, ip->a, 0);
    rr(x, 0, 0, 0x0f57, 0, 0); /* xorps xmm0, xmm0 */
    rr(x, dbl ? 0xf2 : 0xf3, 1, 0x0f2a, 0, RAX); /* cvtsi2ss / cvtsi2sd xmm0, rax */
    stx(x, dbl, ip->d);
}

/* the address of a load or store in rax: generic (sh = 0) or of the shared space */
static void addr(X *x, Ins *ip, int sh, u32 i)
{
    ld(x, RAX, ip->a, 1);
    if (ip->c) {
        rex(x, 1, 0, RAX, 0);
        b1(x, 0x05); /* add rax, imm32 (sign-extended, as EA) */
        b4(x, (u32)ip->c);
    }
    if (sh) {
        u32 j1, j2;
        rr(x, 0, 1, 0x39, R15, RAX); /* cmp rax, r15 */
        b1(x, 0x73);                 /* jae over */
        j1 = x->n;
        b1(x, 0);
        rr(x, 0, 1, 0x01, R14, RAX); /* add rax, r14 */
        b1(x, 0xeb);                 /* jmp done */
        j2 = x->n;
        b1(x, 0);
        if (j1 < x->cap)
            x->p[j1] = (u8)(x->n - (j1 + 1));
        rex(x, 1, 0, RAX, 0);
        b1(x, 0x3d); /* cmp rax, 4096 */
        b4(x, 4096);
        jcc_stub(x, CC_B, i);
        if (j2 < x->cap)
            x->p[j2] = (u8)(x->n - (j2 + 1));
    } else {
        rex(x, 1, 0, RAX, 0);
        b1(x, 0x3d);
        b4(x, 4096);
        jcc_stub(x, CC_B, i);
    }
}

/* one instruction; 0 if it has no template */
static int one(X *x, u32 i)
{
    Ins *ip = &x->f->code[i];
    u32 op = ip->op;
    int sh;
    if (op >= OP_SETP_U32 && op < OP_SETP_F32) {
        u32 g = (op - OP_SETP_U32) / 6, c = (op - OP_SETP_U32) % 6;
        icmp(x, ip, g >= 2, g & 1, (int)c);
        return 1;
    }
    if (op >= OP_SETP_F32 && op < OP_SETP_END)
        return fcmp(x, ip, op >= OP_SETP_F64, (int)((op - OP_SETP_F32) % 14));
    switch (op) {
    case OP_NOP:
        return 1;
    case OP_MOV:
        ld(x, RAX, ip->a, 1);
        stv(x, RAX, ip->d);
        return 1;
    case OP_MOV32:
        ld(x, RAX, ip->a, 0);
        stv(x, RAX, ip->d);
        return 1;
    case OP_ZX8:
    case OP_ZX16:
    case OP_SX8_32:
    case OP_SX16_32:
        rm(x, 0, 0, op == OP_ZX8 ? 0x0fb6 : op == OP_ZX16 ? 0x0fb7 : op == OP_SX8_32 ? 0x0fbe : 0x0fbf, RAX, RBX, sdisp(ip->a));
        stv(x, RAX, ip->d);
        return 1;
    case OP_SX8_64:
    case OP_SX16_64:
        rm(x, 0, 1, op == OP_SX8_64 ? 0x0fbe : 0x0fbf, RAX, RBX, sdisp(ip->a));
        stv(x, RAX, ip->d);
        return 1;
    case OP_SX32_64:
        rm(x, 0, 1, 0x63, RAX, RBX, sdisp(ip->a));
        stv(x, RAX, ip->d);
        return 1;
    case OP_PNOT:
        ld(x, RAX, ip->a, 0);
        b1(x, 0x83); /* xor eax, 1 */
        b1(x, 0xf0);
        b1(x, 1);
        b1(x, 0x83); /* and eax, 1 */
        b1(x, 0xe0);
        b1(x, 1);
        stv(x, RAX, ip->d);
        return 1;
    case OP_ADD32: ibin(x, ip, 0, 0x01); return 1;
    case OP_SUB32: ibin(x, ip, 0, 0x29); return 1;
    case OP_AND32: ibin(x, ip, 0, 0x21); return 1;
    case OP_OR32: ibin(x, ip, 0, 0x09); return 1;
    case OP_XOR32: ibin(x, ip, 0, 0x31); return 1;
    case OP_ADD64: ibin(x, ip, 1, 0x01); return 1;
    case OP_SUB64: ibin(x, ip, 1, 0x29); return 1;
    case OP_AND64: ibin(x, ip, 1, 0x21); return 1;
    case OP_OR64: ibin(x, ip, 1, 0x09); return 1;
    case OP_XOR64: ibin(x, ip, 1, 0x31); return 1;
    case OP_MUL32: imul(x, ip, 0, 0); return 1;
    case OP_MAD32: imul(x, ip, 0, 1); return 1;
    case OP_MUL64: imul(x, ip, 1, 0); return 1;
    case OP_MAD64: imul(x, ip, 1, 1); return 1;
    case OP_MULWU32:
    case OP_MULHIU32:
        ld(x, RAX, ip->a, 0);
        ld(x, RCX, ip->b, 0);
        rr(x, 0, 1, 0x0faf, RAX, RCX);
        if (op == OP_MULHIU32) {
            rex(x, 1, 0, RAX, 0);
            b1(x, 0xc1); /* shr rax, 32 */
            b1(x, 0xe8);
            b1(x, 32);
        }
        stv(x, RAX, ip->d);
        return 1;
    case OP_MULWS32:
    case OP_MULHIS32:
        rm(x, 0, 1, 0x63, RAX, RBX, sdisp(ip->a));
        rm(x, 0, 1, 0x63, RCX, RBX, sdisp(ip->b));
        rr(x, 0, 1, 0x0faf, RAX, RCX);
        if (op == OP_MULHIS32) {
            rex(x, 1, 0, RAX, 0);
            b1(x, 0xc1); /* shr rax, 32: the high half, zero-extended */
            b1(x, 0xe8);
            b1(x, 32);
        }
        stv(x, RAX, ip->d);
        return 1;
    case OP_SHL32: ishift(x, ip, 0, 4); return 1;
    case OP_SHRU32: ishift(x, ip, 0, 5); return 1;
    case OP_SHRS32: ishift(x, ip, 0, 7); return 1;
    case OP_SHL64: ishift(x, ip, 1, 4); return 1;
    case OP_SHRU64: ishift(x, ip, 1, 5); return 1;
    case OP_SHRS64: ishift(x, ip, 1, 7); return 1;
    case OP_NOT32:
    case OP_NEG32:
    case OP_NOT64:
    case OP_NEG64: {
        int w = op == OP_NOT64 || op == OP_NEG64;
        ld(x, RAX, ip->a, w);
        rr(x, 0, w, 0xf7, op == OP_NOT32 || op == OP_NOT64 ? 2 : 3, RAX);
        stv(x, RAX, ip->d);
        return 1;
    }
    case OP_MINU32: iminmax(x, ip, 0, CC_AE); return 1;
    case OP_MINS32: iminmax(x, ip, 0, CC_GE); return 1;
    case OP_MAXU32: iminmax(x, ip, 0, CC_BE); return 1;
    case OP_MAXS32: iminmax(x, ip, 0, CC_LE); return 1;
    case OP_MINU64: iminmax(x, ip, 1, CC_AE); return 1;
    case OP_MINS64: iminmax(x, ip, 1, CC_GE); return 1;
    case OP_MAXU64: iminmax(x, ip, 1, CC_BE); return 1;
    case OP_MAXS64: iminmax(x, ip, 1, CC_LE); return 1;
    case OP_SELP:
    case OP_SELP32: {
        int w = op == OP_SELP;
        ld(x, RAX, ip->a, w);
        ld(x, RCX, ip->b, w);
        ld(x, RDX, ip->c, 0);
        rr(x, 0, 0, 0x85, RDX, RDX); /* test edx, edx */
        rr(x, 0, w, 0x0f40 | CC_E, RAX, RCX);
        stv(x, RAX, ip->d);
        return 1;
    }
    case OP_PAND:
    case OP_POR:
    case OP_PXOR:
        ld(x, RAX, ip->a, 0);
        ld(x, RCX, ip->b, 0);
        rr(x, 0, 0, op == OP_PAND ? 0x21 : op == OP_POR ? 0x09 : 0x31, RCX, RAX);
        b1(x, 0x83);
        b1(x, 0xe0);
        b1(x, 1);
        stv(x, RAX, ip->d);
        return 1;
    case OP_FNEG32:
    case OP_FABS32:
        ld(x, RAX, ip->a, 0);
        b1(x, op == OP_FNEG32 ? 0x35 : 0x25); /* xor / and eax, imm32 */
        b4(x, op == OP_FNEG32 ? 0x80000000U : 0x7fffffffU);
        stv(x, RAX, ip->d);
        return 1;
    case OP_FNEG64:
    case OP_FABS64:
        ld(x, RAX, ip->a, 1);
        movi(x, RCX, op == OP_FNEG64 ? 0x8000000000000000UL : 0x7fffffffffffffffUL, 1);
        rr(x, 0, 1, op == OP_FNEG64 ? 0x31 : 0x21, RCX, RAX);
        stv(x, RAX, ip->d);
        return 1;
    case OP_FADD32: fbin(x, ip, 0, 0x58, i); return 1;
    case OP_FMUL32: fbin(x, ip, 0, 0x59, i); return 1;
    case OP_FSUB32: fbin(x, ip, 0, 0x5c, i); return 1;
    case OP_FADD64: fbin(x, ip, 1, 0x58, i); return 1;
    case OP_FMUL64: fbin(x, ip, 1, 0x59, i); return 1;
    case OP_FSUB64: fbin(x, ip, 1, 0x5c, i); return 1;
    case OP_FMA32:
    case OP_FMA64:
        if (has_fma())
            hwfma(x, ip, op == OP_FMA64, i);
        else
            fcall3(x, ip, op == OP_FMA64, op == OP_FMA64 ? (void *)mc_fma : (void *)mc_fmaf, i);
        return 1;
    case OP_CVT_F32_S32: i2f(x, ip, 0, 1); return 1;
    case OP_CVT_F32_U32: i2f(x, ip, 0, 0); return 1;
    case OP_CVT_F64_S32: i2f(x, ip, 1, 1); return 1;
    case OP_CVT_F64_U32: i2f(x, ip, 1, 0); return 1;
    case OP_CVT_F64_F32:
    case OP_CVT_F32_F64: {
        int to_d = op == OP_CVT_F64_F32;
        ldx(x, 0, ip->a, !to_d);
        rr(x, to_d ? 0xf3 : 0xf2, 0, 0x0f5a, 0, 0); /* cvtss2sd / cvtsd2ss xmm0, xmm0 */
        nan_leaves(x, to_d, i);
        stx(x, to_d, ip->d);
        return 1;
    }
    case OP_MULHIU64:
    case OP_MULHIS64:
        ld(x, RAX, ip->a, 1);
        ld(x, RCX, ip->b, 1);
        rr(x, 0, 1, 0xf7, op == OP_MULHIU64 ? 4 : 5, RCX); /* mul / imul rcx: rdx the high half */
        stv(x, RDX, ip->d);
        return 1;
    case OP_DIVU32: case OP_DIVS32: case OP_REMU32: case OP_REMS32:
    case OP_DIVU64: case OP_DIVS64: case OP_REMU64: case OP_REMS64: {
        int w = op >= OP_DIVU64, sg = op == OP_DIVS32 || op == OP_REMS32 || op == OP_DIVS64 || op == OP_REMS64;
        int rem = op == OP_REMU32 || op == OP_REMS32 || op == OP_REMU64 || op == OP_REMS64;
        ld(x, RAX, ip->a, w);
        ld(x, RCX, ip->b, w);
        rr(x, 0, w, 0x85, RCX, RCX); /* test: a divisor 0 (or -1) is the interpreter's */
        jcc_stub(x, CC_E, i);
        if (sg) {
            rex(x, w, 0, RCX, 0);
            b1(x, 0x83); /* cmp rcx, -1 */
            b1(x, 0xf9);
            b1(x, 0xff);
            jcc_stub(x, CC_E, i);
            if (w)
                b1(x, 0x48);
            b1(x, 0x99); /* cdq / cqo */
        } else {
            rr(x, 0, 0, 0x31, RDX, RDX);
        }
        rr(x, 0, w, 0xf7, sg ? 7 : 6, RCX); /* idiv / div */
        stv(x, rem ? RDX : RAX, ip->d); /* (32 bits: div wrote eax and edx, zero-extended) */
        return 1;
    }
    case OP_CVT_F32_F16:
        b1(x, 0x49); /* mov rcx, [r12 + h2f] (r12: with a SIB byte) */
        b1(x, 0x8b);
        b1(x, 0x4c);
        b1(x, 0x24);
        b1(x, (u32)offsetof(McJitCtx, h2f));
        rm(x, 0, 0, 0x0fb7, RAX, RBX, sdisp(ip->a)); /* movzx eax, word [a] */
        b1(x, 0x8b); /* mov eax, [rcx + rax * 4] */
        b1(x, 0x04);
        b1(x, 0x81);
        stv(x, RAX, ip->d);
        return 1;
    case OP_CVT_F16_F32:
        ldx(x, 0, ip->a, 0);
        rr(x, 0xf3, 0, 0x0f5a, 0, 0); /* cvtss2sd: (double) RA.f */
        forget(x);
        movi(x, RDI, (u64)&mc_fmt_f16, 1);
        movi(x, RSI, MC_RN, 0);
        rr(x, 0, 0, 0x31, RDX, RDX);
        movi(x, RAX, (u64)mc_enc, 1);
        b1(x, 0xff);
        b1(x, 0xd0);
        forget(x);
        rr(x, 0, 0, 0x89, RAX, RAX); /* (u32) */
        stv(x, RAX, ip->d);
        return 1;
    case OP_CVT_F32_S64:
    case OP_CVT_F64_S64:
        ld(x, RAX, ip->a, 1);
        rr(x, 0, 0, 0x0f57, 0, 0);
        rr(x, op == OP_CVT_F64_S64 ? 0xf2 : 0xf3, 1, 0x0f2a, 0, RAX);
        stx(x, op == OP_CVT_F64_S64, ip->d);
        return 1;
    case OP_FSQRT32:
    case OP_FSQRT64: {
        int dbl = op == OP_FSQRT64;
        ldx(x, 0, ip->a, dbl);
        rr(x, dbl ? 0xf2 : 0xf3, 0, 0x0f51, 0, 0); /* sqrtss / sqrtsd: correctly rounded, as mc_sqrtf / mc_sqrt */
        nan_leaves(x, dbl, i);
        stx(x, dbl, ip->d);
        return 1;
    }
    case OP_CVT_S32_F32Z:
    case OP_CVT_S32_F64Z:
    case OP_CVT_U32_F32Z: {
        /* truncated to 64 bits: what does not fit (a NaN, out of range) is the interpreter's */
        rm(x, op == OP_CVT_S32_F64Z ? 0xf2 : 0xf3, 1, 0x0f2c, RAX, RBX, sdisp(ip->a)); /* cvttss2si rax, [a] */
        if (op == OP_CVT_U32_F32Z) {
            rr(x, 0, 1, 0x89, RAX, RCX);
            rex(x, 1, 0, RCX, 0);
            b1(x, 0xc1); /* shr rcx, 32 */
            b1(x, 0xe9);
            b1(x, 32);
            jcc_stub(x, CC_NE, i);
        } else {
            rr(x, 0, 1, 0x63, RCX, RAX); /* movsxd rcx, eax */
            rr(x, 0, 1, 0x39, RAX, RCX); /* cmp rcx, rax */
            jcc_stub(x, CC_NE, i);
            rr(x, 0, 0, 0x89, RAX, RAX); /* mov eax, eax */
        }
        stv(x, RAX, ip->d);
        return 1;
    }
    case OP_LDV:
    case OP_LDV_S:
    case OP_STV:
    case OP_STV_S: {
        static const u8 esz[9] = {1, 1, 1, 2, 2, 2, 4, 4, 8};
        static const u32 lop[9] = {0x0fb6, 0x0fbe, 0x0fbe, 0x0fb7, 0x0fbf, 0x0fbf, 0x8b, 0x63, 0x8b};
        static const u8 lw[9] = {0, 0, 1, 0, 0, 1, 0, 1, 1};
        u32 vn = ip->aux & 15, kk = ip->aux >> 4, e;
        s32 *xs = x->f->xtra + ip->e;
        int st = op == OP_STV || op == OP_STV_S;
        if (vn < 1 || vn > 4 || (st ? kk > 3 : kk > 8))
            return 0;
        addr(x, ip, op == OP_LDV_S || op == OP_STV_S, i);
        if (st) {
            for (e = 0; e < vn; e++) {
                s32 dsp = (s32)(e << kk);
                ld(x, RCX, xs[e], kk == 3);
                if (kk == 0)
                    rm(x, 0, 0, 0x88, RCX, RAX, dsp);
                else if (kk == 1)
                    rm(x, 0x66, 0, 0x89, RCX, RAX, dsp);
                else
                    rm(x, 0, kk == 3, 0x89, RCX, RAX, dsp);
            }
        } else {
            /* all read first, then written (the address may be in one of the slots) */
            static const int tmp[4] = {RCX, RDX, RSI, RDI};
            clobber(x, RSI);
            clobber(x, RDI);
            for (e = 0; e < vn; e++)
                rm(x, 0, lw[kk], lop[kk], tmp[e], RAX, (s32)(e * esz[kk]));
            for (e = vn; e-- > 0;)
                stv(x, tmp[e], xs[e]); /* (rdi and rsi first: the copies of rcx and rdx may take their places) */
        }
        return 1;
    }
    case OP_LD8: case OP_LD8S32: case OP_LD8S64: case OP_LD16: case OP_LD16S32: case OP_LD16S64:
    case OP_LD32: case OP_LD32S64: case OP_LD64:
    case OP_LD8_S: case OP_LD8S32_S: case OP_LD8S64_S: case OP_LD16_S: case OP_LD16S32_S: case OP_LD16S64_S:
    case OP_LD32_S: case OP_LD32S64_S: case OP_LD64_S: {
        u32 k;
        sh = op >= OP_LD8_S;
        k = op - (sh ? OP_LD8_S : OP_LD8);
        addr(x, ip, sh, i);
        switch (k) {
        case 0: rm(x, 0, 0, 0x0fb6, RAX, RAX, 0); break;
        case 1: rm(x, 0, 0, 0x0fbe, RAX, RAX, 0); break;
        case 2: rm(x, 0, 1, 0x0fbe, RAX, RAX, 0); break;
        case 3: rm(x, 0, 0, 0x0fb7, RAX, RAX, 0); break;
        case 4: rm(x, 0, 0, 0x0fbf, RAX, RAX, 0); break;
        case 5: rm(x, 0, 1, 0x0fbf, RAX, RAX, 0); break;
        case 6: rm(x, 0, 0, 0x8b, RAX, RAX, 0); break;
        case 7: rm(x, 0, 1, 0x63, RAX, RAX, 0); break;
        default: rm(x, 0, 1, 0x8b, RAX, RAX, 0); break;
        }
        stv(x, RAX, ip->d);
        return 1;
    }
    case OP_ST8: case OP_ST16: case OP_ST32: case OP_ST64:
    case OP_ST8_S: case OP_ST16_S: case OP_ST32_S: case OP_ST64_S: {
        u32 k;
        sh = op >= OP_ST8_S;
        k = op - (sh ? OP_ST8_S : OP_ST8);
        addr(x, ip, sh, i);
        ld(x, RCX, ip->b, k == 3);
        switch (k) {
        case 0: rm(x, 0, 0, 0x88, RCX, RAX, 0); break;
        case 1: rm(x, 0x66, 0, 0x89, RCX, RAX, 0); break;
        case 2: rm(x, 0, 0, 0x89, RCX, RAX, 0); break;
        default: rm(x, 0, 1, 0x89, RCX, RAX, 0); break;
        }
        return 1;
    }
    }
    return 0;
}

static int is_branch(u32 op)
{
    return op == OP_BRA || op == OP_BRT || op == OP_BRF || op == OP_SKIPF || op == OP_SKIPT;
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
    X xb, *x = &xb;
    McFunc *fn = f;
    u32 i, k, ntr = 0, size = 0;
    int lastok = 1;
    McJit *jt = 0;
    void *mem = MAP_FAILED;
    int mode = jit_mode();
    if (!mode || f->jit || !f->code || !f->ncode || f->ncode > 0x100000)
        return;
    memset(x, 0, sizeof *x);
    x->f = fn;
    forget(x);
    x->nocache = getenv("MCUDA_JIT_NOCACHE") != 0;
    x->off = (u32 *)malloc((size_t)f->ncode * 4);
    x->lead = (u8 *)calloc(f->ncode + 1, 1);
    x->fixed = (u8 *)calloc(f->nconst + 1, 1);
    if (!x->off || !x->lead || !x->fixed)
        goto done;
    for (i = 0; i < f->nfix; i++)
        if (f->fix[i].slot < 0 && (u32)(-1 - f->fix[i].slot) < f->nconst)
            x->fixed[-1 - f->fix[i].slot] = 1;
    /* the heads of the runs */
    x->lead[0] = 1;
    for (i = 0; i < f->ncode; i++) {
        Ins *ip = &f->code[i];
        s64 t = -1;
        switch (ip->op) {
        case OP_BRA: t = (s64)i + ip->a; break;
        case OP_BRT:
        case OP_BRF: t = (s64)i + ip->b; break;
        case OP_SKIPF:
        case OP_SKIPT: t = (s64)i + 1 + ip->b; break;
        case OP_BRX: {
            s32 *xt = f->xtra + ip->e;
            for (k = 0; k < (u32)xt[0]; k++)
                if (xt[1 + k] >= 0 && (u32)xt[1 + k] < f->ncode)
                    x->lead[xt[1 + k]] = 1;
            break;
        }
        default: break;
        }
        if (t >= 0 && t < (s64)f->ncode)
            x->lead[t] = 1;
        else if (t >= 0)
            goto done; /* (a branch out of the function: left to the interpreter) */
        if (is_branch(ip->op) || ip->op == OP_BRX)
            x->lead[i + 1] = 1;
    }
    x->cap = f->ncode * 96 + 4096;
    size = (x->cap + 4095) & ~4095U;
    mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        goto done;
    x->p = (u8 *)mem;
    x->cap = size;
    /* the way in: (rdi the frame, rsi the McJitCtx, rdx where to start) */
    b1(x, 0x53);       /* push rbx */
    b1(x, 0x55);       /* push rbp */
    b1(x, 0x41); b1(x, 0x54); /* push r12 */
    b1(x, 0x41); b1(x, 0x55); /* push r13 */
    b1(x, 0x41); b1(x, 0x56); /* push r14 */
    b1(x, 0x41); b1(x, 0x57); /* push r15 */
    b1(x, 0x48); b1(x, 0x83); b1(x, 0xec); b1(x, 8); /* sub rsp, 8 (calls see the stack aligned) */
    rr(x, 0, 1, 0x89, RDI, RBX); /* mov rbx, rdi */
    rr(x, 0, 1, 0x89, RSI, R12); /* mov r12, rsi */
    rr(x, 0, 0, 0x31, R13, R13); /* xor r13d, r13d */
    rm(x, 0, 1, 0x8b, R14, RSI, (s32)offsetof(McJitCtx, sh));
    rm(x, 0, 1, 0x8b, R15, RSI, (s32)offsetof(McJitCtx, shsz));
    b1(x, 0xff); /* jmp rdx */
    b1(x, 0xe2);
    /* the way out: eax the instruction */
    x->epi = x->n;
    b1(x, 0x4d); b1(x, 0x89); b1(x, 0x6c); b1(x, 0x24); b1(x, (u32)offsetof(McJitCtx, n)); /* mov [r12 + n], r13 */
    b1(x, 0x48); b1(x, 0x83); b1(x, 0xc4); b1(x, 8); /* add rsp, 8 */
    b1(x, 0x41); b1(x, 0x5f);
    b1(x, 0x41); b1(x, 0x5e);
    b1(x, 0x41); b1(x, 0x5d);
    b1(x, 0x41); b1(x, 0x5c);
    b1(x, 0x5d);
    b1(x, 0x5b);
    b1(x, 0xc3);
    for (i = 0; i < f->ncode && !x->over; i++) {
        Ins *ip = &f->code[i];
        u32 at;
        x->cur = i;
        if (x->lead[i]) {
            count(x);
            forget(x);
        }
        at = x->n;
        x->off[i] = x->lead[i] ? at : at | MC_JIT_NOENTER;
        switch (ip->op) {
        case OP_BRA:
            x->pend++;
            count(x);
            jto(x, -1, i + ip->a);
            ntr++;
            continue;
        case OP_BRT:
        case OP_BRF:
        case OP_SKIPF:
        case OP_SKIPT: {
            s32 t = ip->op == OP_BRT || ip->op == OP_BRF ? (s32)i + ip->b : (s32)i + 1 + ip->b;
            ld(x, RAX, ip->a, 0);
            rr(x, 0, 0, 0x85, RAX, RAX); /* test eax, eax */
            x->pend++;
            count(x);
            jto(x, ip->op == OP_BRT || ip->op == OP_SKIPT ? CC_NE : CC_E, (u32)t);
            ntr++;
            continue;
        }
        default:
            break;
        }
        if (one(x, i)) {
            x->pend++;
            ntr++;
            if (i == f->ncode - 1)
                lastok = 0;
        } else {
            /* the interpreter does it; the next instruction is the head of a run */
            x->off[i] = at | MC_JIT_NOENTER;
            leave(x, i);
            x->lead[i + 1] = 1;
        }
    }
    if (lastok == 0)
        goto done; /* the last instruction is translated and could fall off the end */
    for (k = 0; k < x->nst && !x->over; k++) {
        put4(x, x->st[k].at, x->n - (x->st[k].at + 4));
        x->pend = x->st[k].pend;
        leave(x, x->st[k].ins);
    }
    for (k = 0; k < x->nfx && !x->over; k++)
        put4(x, x->fx[k].at, (x->off[x->fx[k].target] & ~MC_JIT_NOENTER) - (x->fx[k].at + 4));
    if (x->over || !ntr)
        goto done;
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0)
        goto done;
    jt = (McJit *)malloc(sizeof *jt);
    if (!jt)
        goto done;
    jt->code = (u32 *)mem;
    jt->off = x->off;
    jt->size = size;
    f->jit = jt;
    x->off = 0;
    mem = MAP_FAILED;
    if (mode >= 2)
        fprintf(stderr, "mcuda: %s: %u of %u instructions translated, %u bytes\n", f->name, ntr, f->ncode, x->n);
    if (mode >= 3) {
        /* MCUDA_JIT=3: the code in jit_NAME.bin (objdump -D -b binary -m i386:x86-64), where each instruction
           starts in jit_NAME.off */
        char nm[200];
        FILE *o;
        sprintf(nm, "jit_%.80s.bin", f->name);
        if ((o = fopen(nm, "wb")) != 0) {
            fwrite(mem, 1, x->n, o);
            fclose(o);
        }
        sprintf(nm, "jit_%.80s.off", f->name);
        if ((o = fopen(nm, "w")) != 0) {
            for (i = 0; i < f->ncode; i++)
                fprintf(o, "%u %x op %u line %u\n", i, jt->off[i] & ~MC_JIT_NOENTER, f->code[i].op, f->lines ? f->lines[i] : 0);
            fclose(o);
        }
    }
done:
    if (mem != MAP_FAILED)
        munmap(mem, size);
    free(x->off);
    free(x->lead);
    free(x->fixed);
    free(x->st);
    free(x->fx);
}
#endif
