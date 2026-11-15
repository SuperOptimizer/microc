/* PTX backend: turns IR into NVIDIA PTX assembly text, one module for each
   translation unit. PTX has typed virtual registers without limit, so there
   is no register allocation: every IR value gets a register of its own. The
   text ends up as a string in the object file, beside a table of the host
   objects the device code refers to, for the CUDA runtime to load. */
#include "ir.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PSig {
    u32 sym;
    u8 ret;
    u8 defined;  /* its body is in this module */
    u8 external; /* supplied by the runtime */
    u8 entry;
    u32 np;
    u8 *pty;
    u32 *pblock; /* a kernel's parameter that is a block of memory: its bytes (0: a value) */
    u8 *palign;
} PSig;

typedef struct PShared {
    u32 sym, align;
    u64 size;
    int ext;
} PShared;

static Obj *PO;
static Buf body;       /* the functions */
static Buf fb;         /* the function being written */
static PSig *sigs;
static u32 nsigs, capsigs;
extern char opt_cuda_target[24];
int opt_dev_fn;
int opt_norange, opt_noifasm, opt_shsym, opt_nocondrec, opt_nodivcse, opt_nohoist, opt_nofastcall, opt_nolocal, opt_nored, opt_nonc, opt_nolatch, opt_nounroll, opt_unrollpeel, opt_nosplit, opt_noauto, opt_nolicm, opt_autolog;
u32 opt_automax = 0xffffffffU, opt_aumax = 0xffffffffU;
int opt_dev_code, opt_dolicm, opt_slotcheck, opt_norounds, opt_nosh32, opt_nochain, opt_nowide, opt_dev_parse, opt_plaindiv, opt_noasmco, opt_nopred, opt_nodist, opt_noifcall, opt_nolocal, opt_noxrem, opt_nowiden, opt_nominmax, opt_noselk, opt_noglobal, opt_premat, opt_nospecial1, opt_ifc = 6;
u32 ptx_maxntid;
static u32 *byoff;     /* of an OP_BYVAL: where in the frame its copy is */
static u32 *hostsyms;  /* host objects the device code addresses */
static u32 *kernsyms, nkern, capkern; /* the unit's kernels */
static u32 nhost, caphost;
static PShared *shared;
static u32 nshared, capshared;
static u32 modsym = 0xffffffffU;
static u32 *need;      /* called functions that still want a body or a declaration */
static u32 nneed, capneed;

/* per function */
static Func *PF;
static u32 *regno;     /* value -> 1 + register number within its class */
static u32 nreg[4];
static const char *special_load(u32 v);
static u8 *cuse; /* how many of the readers of a value read it as a condition */
static u8 *used;
static u8 *ptree; /* a sum or product of truth values that is worked out in predicate registers: 1; 2: and has no register */
static u8 *xrem; /* a 32-bit number made wider, read in another block than it is made in: made wider again where it is
                      read, so that it is the 32-bit number that stays in a register (one, not two) on the way */
static u8 *nuse;  /* how many times a value is read, up to 255 */
static u8 *fuse;  /* see fuse_find */
static u8 *fused; /* a product that became part of a fused operation: not computed by itself */
static u32 *slotoff;
static const char *fname;

static void P(const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    buf_put(&fb, tmp, strlen(tmp));
}

static void BP(Buf *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    buf_put(b, tmp, strlen(tmp));
}

void ptx_reset(void)
{
    body.len = 0;
    nsigs = nhost = nshared = nneed = nkern = 0;
    modsym = 0xffffffffU;
}

int ptx_active(void)
{
    return body.len != 0 || modsym != 0xffffffffU;
}

/* The object symbol of this unit's module record, which launches refer to. */
u32 ptx_module(Obj *o)
{
    if (modsym == 0xffffffffU)
        modsym = obj_add_sym(o, 0, SEC_UNDEF, 0, 0);
    return modsym;
}

static const char *pname(u32 sym)
{
    static char anon[64][40];
    static int which;
    const char *n;
    if (!PO->syms[sym].name) {
        char *a = anon[which++ & 63];
        sprintf(a, "__mcuda_s%u", sym);
        return a;
    }
    n = intern_str(PO->syms[sym].name);
    if (opt_target != TARGET_LINUX && n[0] == '_')
        n++; /* Mach-O's underscore */
    return n;
}

void ptx_shared(u32 sym, u64 size, u32 align, int ext)
{
    u32 k;
    for (k = 0; k < nshared; k++)
        if (shared[k].sym == sym)
            return;
    if (nshared == capshared) {
        capshared = capshared ? capshared * 2 : 16;
        shared = (PShared *)xrealloc(shared, capshared * sizeof(PShared));
    }
    shared[nshared].sym = sym;
    shared[nshared].size = size;
    shared[nshared].align = align ? align : 1;
    shared[nshared].ext = ext;
    nshared++;
}

static int is_shared(u32 sym)
{
    u32 k;
    for (k = 0; k < nshared; k++)
        if (shared[k].sym == sym)
            return 1;
    return 0;
}

static PSig *sig_find(u32 sym)
{
    u32 k;
    for (k = 0; k < nsigs; k++)
        if (sigs[k].sym == sym)
            return &sigs[k];
    return 0;
}

static PSig *sig_add(u32 sym, int ret, u32 np, const u8 *pty)
{
    PSig *s = sig_find(sym);
    if (s)
        return s;
    if (nsigs == capsigs) {
        capsigs = capsigs ? capsigs * 2 : 64;
        sigs = (PSig *)xrealloc(sigs, capsigs * sizeof(PSig));
    }
    s = &sigs[nsigs++];
    memset(s, 0, sizeof *s);
    s->sym = sym;
    s->ret = (u8)ret;
    /* (x86-64 marks a parameter that is a block of memory, 0x80, and has a place holder, 0x40, for each
       further doubleword of it: here the first is the block's address and the others do not exist) */
    s->pty = (u8 *)xmalloc(np + 1);
    s->pblock = (u32 *)xmalloc((np + 1) * sizeof(u32));
    s->palign = (u8 *)xmalloc(np + 1);
    s->np = 0;
    {
        u32 k;
        for (k = 0; k < np; k++)
            if (pty[k] & 0x40) {
                s->pblock[s->np - 1] += 8;
            } else {
                s->pblock[s->np] = pty[k] & 0x80 ? 8 : 0;
                s->palign[s->np] = pty[k] & 0x20 ? 16 : 8;
                s->pty[s->np++] = (u8)(pty[k] & 0x1f);
            }
    }
    return s;
}

/* A called function that has not been dealt with, as sym + 1; 0 when none is left.
   The caller answers with ptx_gen for one it has a body for, else ptx_extern. */
u32 ptx_next_needed(void)
{
    while (nneed) {
        u32 sym = need[--nneed];
        PSig *s = sig_find(sym);
        if (s && !s->defined && !s->external)
            return sym + 1;
    }
    return 0;
}

void ptx_extern(u32 sym)
{
    PSig *s = sig_find(sym);
    if (s)
        s->external = 1;
}

/* threadIdx and its relatives are special registers in PTX; a function that mentions one gets a copy in its frame */
static const char *const special_name[4] = {"threadIdx", "blockIdx", "blockDim", "gridDim"};
static const char *const special_reg[4] = {"%tid", "%ctaid", "%ntid", "%nctaid"};
static u32 special_off[4];

static int special_of(u32 sym)
{
    const char *n = pname(sym);
    int k;
    for (k = 0; k < 4; k++)
        if (!strcmp(n, special_name[k]))
            return k;
    return -1;
}

static void host_ref(u32 sym)
{
    u32 k;
    for (k = 0; k < nhost; k++)
        if (hostsyms[k] == sym)
            return;
    if (nhost == caphost) {
        caphost = caphost ? caphost * 2 : 32;
        hostsyms = (u32 *)xrealloc(hostsyms, caphost * sizeof(u32));
    }
    hostsyms[nhost++] = sym;
}

/* ---- registers and operands ---- */

static const char *const cls_stem[4] = {"%r", "%rd", "%f", "%fd"};
static const char *const cls_reg[4] = {".b32", ".b64", ".f32", ".f64"};

static int cls_of(int ty)
{
    switch (ty) {
    case IT_I32: return 0;
    case IT_I64: return 1;
    case IT_F32: return 2;
    case IT_F64: return 3;
    }
    die("ptx: %s: a value of an unsupported type", fname);
    return 0;
}

/* -use_fast_math: what an operation on single precision numbers is marked with */
#define FTZ(ty) (opt_fastmath && (ty) == IT_F32 ? ".ftz" : "")

static u32 RV(u32 v)
{
    while (PF->ins[v].op == OP_COPY)
        v = PF->ins[v].a;
    return v;
}

static char *obuf(void)
{
    /* names are held while an instruction is written, and that can be a long one: a call with a
       hundred arguments, a copy written out as many loads and stores */
    static char bufs[2048][48];
    static int which;
    return bufs[which++ & 2047];
}

static const char *temp(int cls)
{
    char *s = obuf();
    sprintf(s, "%s%u", cls_stem[cls], nreg[cls]++);
    return s;
}

static int remat; /* the body is being written: an address that is a constant is worked out where it is read */
static const char *kern_name;
static const char *addr_const(u32 v);
static const char *param_const(u32 v);
static int premat; /* a kernel's parameters are read where they are used (MCC_PREMAT), not held from the start */

static const char *reg(u32 v)
{
    char *s;
    int c;
    v = RV(v);
    if (remat && (PF->ins[v].op == OP_SLOT || PF->ins[v].op == OP_ADDR))
        return addr_const(v);
    if (remat && premat && PF->ins[v].op == OP_PARAM)
        return param_const(v);
    if (remat && xrem && xrem[v]) {
        const char *t = temp(cls_of(IT_I64)), *a = reg(PF->ins[v].a);
        P("\tcvt.%s %s, %s;\n", PF->ins[v].op == OP_SEXT32 ? "s64.s32" : "u64.u32", t, a);
        return t;
    }
    s = obuf();
    c = cls_of(PF->ins[v].ty);
    if (!regno[v])
        regno[v] = ++nreg[c];
    sprintf(s, "%s%u", cls_stem[c], regno[v] - 1);
    return s;
}

/* An operand: a register, or a literal for a constant. */
static const char *opnd(u32 v)
{
    Ins *i;
    char *s;
    v = RV(v);
    i = &PF->ins[v];
    if (i->op == OP_CONST) {
        s = obuf();
        if (i->ty == IT_I32)
            sprintf(s, "%d", (int)(i32)i->a);
        else
            sprintf(s, "%ld", (long)(i64)((u64)i->a | ((u64)i->b << 32)));
        return s;
    }
    if (i->op == OP_FCONST) {
        s = obuf();
        if (i->ty == IT_F32)
            sprintf(s, "0f%08X", (unsigned)i->a);
        else
            sprintf(s, "0d%08X%08X", (unsigned)i->b, (unsigned)i->a);
        return s;
    }
    return reg(v);
}

static const char *mov_of(int ty)
{
    return ty == IT_I32 ? "mov.b32" : ty == IT_I64 ? "mov.b64" : ty == IT_F32 ? "mov.f32" : "mov.f64";
}

/* The same, but always a register. */
static const char *ropnd(u32 v)
{
    Ins *i;
    const char *t;
    v = RV(v);
    i = &PF->ins[v];
    if (i->op != OP_CONST && i->op != OP_FCONST)
        return reg(v);
    t = temp(cls_of(i->ty));
    P("\t%s %s, %s;\n", mov_of(i->ty), t, opnd(v));
    return t;
}

static int special_of(u32 sym);
static int is_shared(u32 sym);
static void host_ref(u32 sym);
static u8 *ause;
static u8 *xlate; /* a 32-bit number made wider that a wide product reads as it was, where the product is */

/* The address of a local or a global object, in a register of its own for this one reader: held in one
   register from the start of the function it would be alive all through it, and a kernel has hundreds. */
static const char *addr_const(u32 v)
{
    Ins *i = &PF->ins[v];
    const char *t = temp(cls_of(i->ty));
    if (i->op == OP_ADDR && special_of(i->a) >= 0) {
        P("\tadd.u64 %s, %%SP, %ld;\n", t, (long)special_off[special_of(i->a)] + (long)(i32)i->b);
    } else if (i->op == OP_SLOT) {
        P("\tadd.u64 %s, %%SP, %ld;\n", t, (long)slotoff[i->a] + (long)(i32)i->b);
    } else {
        if (is_shared(i->a)) {
            P("\tmov.u64 %s, %s;\n\tcvta.shared.u64 %s, %s;\n", t, pname(i->a), t, t);
        } else {
            host_ref(i->a);
            P("\tmov.u64 %s, %s;\n", t, pname(i->a));
        }
        if (i->b)
            P("\tadd.u64 %s, %s, %ld;\n", t, t, (long)(i32)i->b);
    }
    return t;
}

/* The sum v, read as an address, is its first operand and a constant that the instruction can add. */
static int addr_sum(u32 v, i64 *off)
{
    Ins *i = &PF->ins[RV(v)], *c;
    if (i->op != OP_ADD || i->ty != IT_I64)
        return 0;
    c = &PF->ins[RV(i->b)];
    if (c->op != OP_CONST)
        return 0;
    *off = (i64)((u64)c->a | ((u64)c->b << 32));
    return *off >= -0x40000000 && *off <= 0x40000000;
}

/* The address a of a load or a store, as the instruction takes it: a register, or a register and a constant. */
static const char *maddr(u32 a)
{
    Ins *i;
    char *s;
    const char *base;
    i64 off = 0;
    a = RV(a);
    if (addr_sum(a, &off))
        a = RV(PF->ins[a].a);
    i = &PF->ins[a];
    if (remat && (i->op == OP_SLOT || (i->op == OP_ADDR && special_of(i->a) >= 0))) {
        base = "%SP";
        off += (i64)(i->op == OP_SLOT ? slotoff[i->a] : special_off[special_of(i->a)]) + (i64)(i32)i->b;
    } else {
        base = ropnd(a);
    }
    if (!off)
        return base;
    s = obuf();
    sprintf(s, "%s+%ld", base, (long)off);
    return s;
}

/* The address a of a load or a store when it is in the function's own frame: [%SPL+n], of the local state space
   (ld.local, LDL), rather than the generic [%SP+n] (LD, which looks up the space of each address); 0 otherwise. */
static const char *local_maddr(u32 a)
{
    Ins *i;
    char *s;
    i64 off = 0;
    a = RV(a);
    if (opt_nolocal || !remat)
        return 0;
    if (addr_sum(a, &off))
        a = RV(PF->ins[a].a);
    i = &PF->ins[a];
    if (i->op != OP_SLOT && !(i->op == OP_ADDR && special_of(i->a) >= 0))
        return 0;
    off += (i64)(i->op == OP_SLOT ? slotoff[i->a] : special_off[special_of(i->a)]) + (i64)(i32)i->b;
    s = obuf();
    sprintf(s, "%%SPL+%ld", (long)off);
    return s;
}

/* ---- shared memory by 32-bit addresses ----
   A place in shared memory is 32 bits, and a GPU's registers are 32 bits wide: an address of 64 takes two of
   them, and so does each 64-bit index it is worked out from. As NVIDIA's compiler does, the addresses that
   are a shared object's plus or minus whole numbers are worked out in 32 bits, from the low halves of the
   numbers, and the loads and stores are of shared memory; the whole (generic) address is made from that where
   something else reads it. MCC_NOSH32 turns this off. */
static u8 *sh_s;    /* the value is such an address */
static u8 *sh_t32;  /* the low half of the value is wanted in a register of its own */
static u8 *sh_g64;  /* the whole value is wanted */
static u32 *sh_reg; /* 1 + the number of the low half's register */

/* A 64-bit whole number whose low half follows from its operands' low halves. */
static int sh_arith(u32 v)
{
    Ins *i = &PF->ins[v];
    u64 c;
    if (i->ty != IT_I64 || sh_s[v])
        return 0;
    switch (i->op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
        return 1;
    case OP_SHL:
        return ir_is_const(PF, RV(i->b), &c) && c < 32;
    default:
        return 0;
    }
}

static int sh_ext(u32 v)
{
    return PF->ins[v].op == OP_SEXT32 || PF->ins[v].op == OP_ZEXT32;
}

static int sh_changed;

static void sh_want(u32 o)
{
    o = RV(o);
    if (!o || PF->ins[o].op == OP_CONST)
        return;
    if (sh_s[o] || sh_arith(o) || sh_ext(o)) {
        if (!sh_t32[o]) {
            sh_t32[o] = 1;
            sh_changed = 1;
        }
    } else if (!sh_g64[o]) {
        sh_g64[o] = 1; /* cut from the whole */
        sh_changed = 1;
    }
}

static void sh_whole(u32 o)
{
    o = RV(o);
    if (o && !sh_g64[o]) {
        sh_g64[o] = 1;
        sh_changed = 1;
    }
}

static const char *special_load(u32 v);

/* The base of the address a of a load or a store, when that is in shared memory. */
static u32 sh_base(u32 a)
{
    i64 off;
    a = RV(a);
    if (addr_sum(a, &off))
        a = RV(PF->ins[a].a);
    return sh_s[a] ? a : 0;
}

/* v is __cvta_generic_to_shared of an address in shared memory: that address, whose low half is the answer. */
static u32 sh_to_shared(u32 v)
{
    Ins *i = &PF->ins[v];
    u64 idx;
    AsmStmt *st;
    u32 x;
    if (i->op != OP_CALL || !(i->flags & IF_ASM) || !ir_is_const(PF, i->c, &idx))
        return 0;
    st = &asm_stmts[idx];
    if (st->nops != 2 || i->b != 1 || st->ops[0].out != 1 || st->ops[1].out || st->ops[0].kind != 'r' || st->ops[1].kind != 'r' ||
        strcmp(st->tmpl, "cvta.to.shared.u64 %0, %1;"))
        return 0;
    x = RV(PF->xargs[i->a]);
    return sh_s[x] ? x : 0;
}

/* The low halves that are wanted, to a fixed point. */
static void sh_lows(const u8 *live)
{
    Func *f = PF;
    u32 b, v;
    do {
        sh_changed = 0;
        for (b = 1; b < f->nblk; b++) {
            if (!live[b])
                continue;
            for (v = f->blk[b].first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op == OP_LOAD || i->op == OP_STORE) {
                    if (!special_load(v) && sh_base(i->a))
                        sh_want(sh_base(i->a));
                } else if (sh_to_shared(v)) {
                    sh_want(sh_to_shared(v));
                } else if (sh_t32[v] && ((sh_s[v] && (i->op == OP_ADD || i->op == OP_SUB)) || sh_arith(v))) {
                    sh_want(i->a);
                    if (i->op != OP_SHL)
                        sh_want(i->b);
                }
            }
        }
    } while (sh_changed);
}

static void sh_find(const u8 *live)
{
    Func *f = PF;
    u32 b, v, k, n, *p;
    int any = 0;
    for (v = 1; v < f->nins; v++)
        if (f->ins[v].op == OP_ADDR && is_shared(f->ins[v].a) && special_of(f->ins[v].a) < 0) {
            sh_s[v] = 1;
            any = 1;
        }
    if (!any)
        return;
    /* the addresses: taken to be all that could be, then those that are not are struck out */
    do {
        sh_changed = 0;
        for (b = 1; b < f->nblk; b++)
            for (v = live[b] ? f->blk[b].first : 0; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->ty == IT_I64 && !sh_s[v] && (i->op == OP_ADD || i->op == OP_SUB) && sh_s[RV(i->a)] != sh_s[RV(i->b)] &&
                    (i->op == OP_ADD || sh_s[RV(i->a)])) {
                    sh_s[v] = 1;
                    sh_changed = 1;
                }
            }
    } while (sh_changed);
    do {
        sh_changed = 0;
        for (b = 1; b < f->nblk; b++)
            for (v = live[b] ? f->blk[b].first : 0; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (sh_s[v] && (i->op == OP_ADD || i->op == OP_SUB) &&
                    !(sh_s[RV(i->a)] != sh_s[RV(i->b)] && (i->op == OP_ADD || sh_s[RV(i->a)]))) {
                    sh_s[v] = 0;
                    sh_changed = 1;
                }
            }
    } while (sh_changed);
    sh_lows(live);
    /* what is wanted whole */
    do {
        sh_changed = 0;
        for (b = 1; b < f->nblk; b++) {
            Block *bb = &f->blk[b];
            if (!live[b])
                continue;
            for (v = bb->phis; v; v = f->ins[v].next)
                for (k = 0; k < f->ins[v].b; k++)
                    sh_whole(f->xargs[f->ins[v].a + k]);
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op == OP_LOAD || i->op == OP_STORE) {
                    if (special_load(v))
                        continue;
                    if (!sh_base(i->a)) {
                        i64 off;
                        u32 x = RV(i->a);
                        sh_whole(addr_sum(x, &off) ? f->ins[x].a : x);
                    }
                    if (i->op == OP_STORE)
                        sh_whole(i->b);
                } else if (sh_s[v] || sh_ext(v) || sh_to_shared(v)) {
                    continue; /* (the whole of such an address is made from its low half) */
                } else if (sh_arith(v)) {
                    if (sh_g64[v] || !sh_t32[v]) {
                        sh_whole(i->a);
                        sh_whole(i->b);
                    }
                } else {
                    IR_OPS(f, i, p, n);
                    for (k = 0; k < n; k++)
                        sh_whole(p[k]);
                    if (i->op == OP_CALL)
                        sh_whole(i->c);
                }
            }
            if (bb->arg)
                sh_whole(bb->arg);
        }
    } while (sh_changed);
    for (v = 1; v < f->nins; v++)
        if (sh_s[v] && sh_g64[v] && f->ins[v].op != OP_ADDR)
            sh_t32[v] = 1;
    sh_lows(live);
}

/* The shared symbols a function's addresses are made from, each in a register set once at its start: "mov.b32 r,
   smem" is a cluster-window address, which ptxas makes with S2R SR_CgaCtaId and a LEA wherever it stands, and it did
   not merge them (lp_s2: 5415 such movs; the GroupNorm bank select of lp_s2.cu:166 alone 480 S2R). Opt-in, MCC_SHSYM=1:
   161 kernels ALL -6.8k, but ptxas then unrolls conv_fwd_s2_f8_k's tap loops into the z-step loop (554 -> 729 there);
   the GPU A/B decides. */
#define SHSYM_MAX 8
static u32 shsym[SHSYM_MAX], nshsym;
static char shsym_reg[SHSYM_MAX][16];

static const char *sh_name(u32 v);

static void sh_syms(void)
{
    u32 v, k;
    nshsym = 0;
    if (!sh_s || !opt_shsym)
        return;
    for (v = 1; v < PF->nins; v++) {
        Ins *i = &PF->ins[v];
        if (i->op != OP_ADDR || !sh_s[v] || !used[v])
            continue;
        for (k = 0; k < nshsym && shsym[k] != i->a; k++)
            ;
        if (k < nshsym || nshsym == SHSYM_MAX)
            continue;
        shsym[nshsym] = i->a;
        strcpy(shsym_reg[nshsym], sh_name(v));
        P("\tmov.b32 %s, %s;\n", shsym_reg[nshsym], pname(i->a));
        nshsym++;
    }
}

static const char *sh_name(u32 v)
{
    char *s = obuf();
    if (!sh_reg[v])
        sh_reg[v] = ++nreg[0];
    sprintf(s, "%s%u", cls_stem[0], sh_reg[v] - 1);
    return s;
}

/* The low half of o: a register, or a literal. */
static const char *sh_low(u32 o)
{
    Ins *i;
    char *s;
    const char *t;
    o = RV(o);
    i = &PF->ins[o];
    if (i->op == OP_CONST) {
        s = obuf();
        sprintf(s, "%d", (int)(i32)i->a);
        return s;
    }
    if (i->op == OP_ADDR && sh_s[o]) {
        u32 k;
        for (k = 0; k < nshsym && shsym[k] != i->a; k++)
            ;
        if (k < nshsym) { /* (the symbol's register, set once at the start: sh_syms) */
            if (!i->b)
                return shsym_reg[k];
            t = temp(0);
            P("\tadd.s32 %s, %s, %d;\n", t, shsym_reg[k], (int)(i32)i->b);
            return t;
        }
        t = temp(0);
        P("\tmov.b32 %s, %s;\n", t, pname(i->a));
        if (i->b)
            P("\tadd.s32 %s, %s, %d;\n", t, t, (int)(i32)i->b);
        return t;
    }
    if (sh_t32[o] && (sh_s[o] || sh_arith(o)))
        return sh_name(o);
    if (sh_t32[o] && sh_ext(o))
        return opnd(i->a);
    t = temp(0);
    P("\tcvt.u32.u64 %s, %s;\n", t, reg(o));
    return t;
}

static const char *sh_low_r(u32 o)
{
    const char *a = sh_low(o), *t;
    if (a[0] == '%')
        return a;
    t = temp(0);
    P("\tmov.b32 %s, %s;\n", t, a);
    return t;
}

/* The address a of a load or a store of shared memory, as the instruction takes it; 0 if it is not one. */
static const char *sh_maddr(u32 a)
{
    i64 off = 0;
    const char *base;
    char *s;
    if (!sh_s || !sh_base(a))
        return 0;
    a = RV(a);
    addr_sum(a, &off);
    base = sh_low_r(sh_base(a));
    if (!off)
        return base;
    s = obuf();
    sprintf(s, "%s+%ld", base, (long)off);
    return s;
}

/* Writes the low half of v, and v from it if that is wanted; says whether v is done with. */
static int sh_emit(u32 v)
{
    Ins *i = &PF->ins[v];
    const char *a, *b, *d, *op;
    int isaddr = sh_s[v] && (i->op == OP_ADD || i->op == OP_SUB);
    if (!isaddr && !(sh_t32[v] && (sh_arith(v) || sh_ext(v))))
        return 0;
    if (sh_ext(v))
        return !sh_g64[v];
    if (sh_t32[v]) {
        u64 c = 0;
        op = i->op == OP_ADD ? "add.s32" : i->op == OP_SUB ? "sub.s32" : i->op == OP_MUL ? "mul.lo.s32" : i->op == OP_AND ? "and.b32" :
             i->op == OP_OR ? "or.b32" : i->op == OP_XOR ? "xor.b32" : "shl.b32";
        a = sh_low_r(i->a);
        if (i->op == OP_SHL) {
            char *s = obuf();
            ir_is_const(PF, RV(i->b), &c);
            sprintf(s, "%u", (unsigned)c);
            b = s;
        } else {
            b = sh_low(i->b);
        }
        d = sh_name(v);
        P("\t%s %s, %s, %s;\n", op, d, a, b);
    }
    if (!isaddr)
        return !sh_g64[v];
    if (sh_g64[v]) {
        d = reg(v);
        P("\tcvt.u64.u32 %s, %s;\n\tcvta.shared.u64 %s, %s;\n", d, sh_name(v), d, d);
    }
    return 1;
}

static const char *ity(int ty, int sgn)
{
    return ty == IT_I64 ? (sgn == 's' ? "s64" : sgn == 'u' ? "u64" : "b64")
                        : (sgn == 's' ? "s32" : sgn == 'u' ? "u32" : "b32");
}

static const char *fty(int ty)
{
    return ty == IT_F64 ? "f64" : "f32";
}

static const char *vty(int ty)
{
    return ty == IT_I32 ? "b32" : ty == IT_I64 ? "b64" : fty(ty);
}

static const char *pty_name(int ty, int entry)
{
    switch (ty) {
    case IT_I32: return entry ? ".u32" : ".b32";
    case IT_I64: return entry ? ".u64" : ".b64";
    case IT_F32: return ".f32";
    case IT_F64: return ".f64";
    }
    return ".b32";
}

/* ---- comparisons ---- */

static int is_compare(int op)
{
    return (op >= OP_EQ && op <= OP_UGE) || (op >= OP_FEQ && op <= OP_FGE);
}

/* Sets %p<n> from the comparison v. */
static void set_pred_n(u32 v, int n)
{
    Ins *i = &PF->ins[v];
    int oty = PF->ins[RV(i->a)].ty;
    const char *c = "", *t;
    switch (i->op) {
    case OP_EQ: c = "eq"; t = ity(oty, 's'); break;
    case OP_NE: c = "ne"; t = ity(oty, 's'); break;
    case OP_SLT: c = "lt"; t = ity(oty, 's'); break;
    case OP_SLE: c = "le"; t = ity(oty, 's'); break;
    case OP_SGT: c = "gt"; t = ity(oty, 's'); break;
    case OP_SGE: c = "ge"; t = ity(oty, 's'); break;
    case OP_ULT: c = "lo"; t = ity(oty, 'u'); break;
    case OP_ULE: c = "ls"; t = ity(oty, 'u'); break;
    case OP_UGT: c = "hi"; t = ity(oty, 'u'); break;
    case OP_UGE: c = "hs"; t = ity(oty, 'u'); break;
    case OP_FEQ: c = "eq"; t = fty(oty); break;
    case OP_FNE: c = "neu"; t = fty(oty); break;
    case OP_FLT: c = "lt"; t = fty(oty); break;
    case OP_FLE: c = "le"; t = fty(oty); break;
    case OP_FGT: c = "gt"; t = fty(oty); break;
    default: c = "ge"; t = fty(oty); break;
    }
    P("\tsetp.%s.%s %%p%d, %s, %s;\n", c, t, n, ropnd(i->a), opnd(i->b));
}

static void set_pred(u32 v)
{
    set_pred_n(v, 0);
}

/* "and" and "or" of truth values, in predicate registers as nvcc has them: a tree whose leaves are
   comparisons (done where the tree is wanted, like a lone comparison) and numbers that are 0 or 1.
   pred_need: the predicate registers a tree takes when the operand that takes more goes first. */
static int pred_need(u32 v, int root)
{
    Ins *i;
    int l, r;
    v = RV(v);
    i = &PF->ins[v];
    if ((is_compare(i->op) && i->blk) || !(ptree[v] == 2 || (root && ptree[v])))
        return 1;
    l = pred_need(i->a, 0);
    r = pred_need(i->b, 0);
    return l == r ? l + 1 : l > r ? l : r;
}

static void pred_into(u32 v, int n, int root)
{
    Ins *i;
    v = RV(v);
    i = &PF->ins[v];
    if (is_compare(i->op) && i->blk) {
        set_pred_n(v, n);
    } else if (ptree[v] == 2 || (root && ptree[v])) {
        u32 a = i->a, b = i->b;
        if (pred_need(b, 0) > pred_need(a, 0)) {
            a = i->b;
            b = i->a;
        }
        if (n >= 14)
            die("ptx: %s: a condition too deep", fname);
        pred_into(a, n, 0);
        pred_into(b, n + 1, 0);
        P("\t%s.pred %%p%d, %%p%d, %%p%d;\n", i->op == OP_AND ? "and" : "or", n, n, n + 1);
    } else {
        P("\tsetp.ne.%s %%p%d, %s, 0;\n", ity(i->ty, 'b'), n, ropnd(v));
    }
}

/* Is v the result of a comparison, or an "and" or "or" of such? */
static int pred_bool(u32 v, int depth)
{
    Ins *i;
    v = RV(v);
    i = &PF->ins[v];
    if (is_compare(i->op) && i->blk)
        return 1;
    return (i->op == OP_AND || i->op == OP_OR) && i->ty == IT_I32 && i->blk && depth && pred_bool(i->a, depth - 1) &&
           pred_bool(i->b, depth - 1);
}

/* Sets %p0 to "v is not zero"; a comparison is done over again here, its operands being still at hand. */
static void cond_pred(u32 v)
{
    v = RV(v);
    if (is_compare(PF->ins[v].op) && PF->ins[v].blk)
        set_pred(v);
    else if (ptree[v] == 2)
        pred_into(v, 0, 0);
    else
        P("\tsetp.ne.%s %%p0, %s, 0;\n", ity(PF->ins[v].ty, 'b'), ropnd(v));
}

/* ---- calls ---- */

typedef struct Intrin {
    const char *name;
    const char *tmpl; /* $d: the result, $0..: the arguments */
} Intrin;

static const Intrin intrins[] = {
    {"sqrtf", "sqrt.rn.f32 $d, $0;"}, {"sqrt", "sqrt.rn.f64 $d, $0;"},
    {"fabsf", "abs.f32 $d, $0;"}, {"fabs", "abs.f64 $d, $0;"},
    {"fminf", "min.f32 $d, $0, $1;"}, {"fmaxf", "max.f32 $d, $0, $1;"},
    {"fmin", "min.f64 $d, $0, $1;"}, {"fmax", "max.f64 $d, $0, $1;"},
    {"floorf", "cvt.rmi.f32.f32 $d, $0;"}, {"ceilf", "cvt.rpi.f32.f32 $d, $0;"},
    {"truncf", "cvt.rzi.f32.f32 $d, $0;"}, {"rintf", "cvt.rni.f32.f32 $d, $0;"},
    {"nearbyintf", "cvt.rni.f32.f32 $d, $0;"},
    {"floor", "cvt.rmi.f64.f64 $d, $0;"}, {"ceil", "cvt.rpi.f64.f64 $d, $0;"},
    {"trunc", "cvt.rzi.f64.f64 $d, $0;"}, {"rint", "cvt.rni.f64.f64 $d, $0;"},
    {"nearbyint", "cvt.rni.f64.f64 $d, $0;"},
    {"fmaf", "fma.rn.f32 $d, $0, $1, $2;"}, {"fma", "fma.rn.f64 $d, $0, $1, $2;"},
    {"copysignf", "copysign.f32 $d, $1, $0;"}, {"copysign", "copysign.f64 $d, $1, $0;"},
    {"lrintf", "cvt.rni.s64.f32 $d, $0;"}, {"llrintf", "cvt.rni.s64.f32 $d, $0;"},
    {"lrint", "cvt.rni.s64.f64 $d, $0;"}, {"llrint", "cvt.rni.s64.f64 $d, $0;"},
    {"abs", "abs.s32 $d, $0;"}, {"labs", "abs.s64 $d, $0;"}, {"llabs", "abs.s64 $d, $0;"},
    {"__mcc_h2f", "cvt.u16.u32 %h0, $0;\n\tcvt.f32.f16 $d, %h0;"},
    {"__mcc_bf2f", "cvt.u16.u32 %h0, $0;\n\tcvt.f32.bf16 $d, %h0;"},
    {"__mcc_f2bf", "cvt.rn.bf16.f32 %h0, $0;\n\tcvt.u32.u16 $d, %h0;"},
    {"__mcc_froundbf", "cvt.rn.bf16.f32 %h0, $0;\n\tcvt.f32.bf16 $d, %h0;"},
    {"__mcc_f2h", "cvt.rn.f16.f32 %h0, $0;\n\tcvt.u32.u16 $d, %h0;"},
    {"__mcc_fround16", "cvt.rn.f16.f32 %h0, $0;\n\tcvt.f32.f16 $d, %h0;"},
    {"__mcc_dround16", "cvt.rn.f16.f64 %h0, $0;\n\tcvt.f32.f16 $d, %h0;"},
    {0, 0}
};

/* Is the call v one that becomes an instruction or two of arithmetic (see intrins)? Asked while the
   function is optimised: such a call may be made where its result is not wanted (if-conversion). */
Obj *ptx_obj;

int ptx_pure_call(Func *f, u32 v)
{
    Ins *i = &f->ins[v], *c;
    const Intrin *in;
    const char *name;
    PSig *s;
    u32 callee = i->c;
    if (i->op != OP_CALL || (i->flags & (IF_ASM | IF_ALLOCA)) || !ptx_obj)
        return 0;
    while (f->ins[callee].op == OP_COPY)
        callee = f->ins[callee].a;
    c = &f->ins[callee];
    if (c->op != OP_ADDR || c->b || !ptx_obj->syms[c->a].name)
        return 0;
    s = sig_find(c->a);
    if (s && (s->defined || s->entry))
        return 0;
    name = intern_str(ptx_obj->syms[c->a].name);
    for (in = intrins; in->name; in++)
        if (strcmp(in->name, name) == 0)
            return 1;
    return 0;
}

/* -use_fast_math: single precision math calls as nvcc writes them: flushing denormals (.ftz), and fmaf(a, b, 0) a
   product, fmaf(1, x, 0) x (no signed zeros). ufsm's fmaf(1.f, x, 0.f) (a product kept out of a later sum) came out
   as an FFMA each: conv_bwd_w_f8_k<1,6,mx4,mx8,0> 1074 FFMA, nvcc none. MCC_NOFASTCALL: as without fast math. */
static int fconst_is(u32 v, u32 bits)
{
    Ins *c = &PF->ins[RV(v)];
    return c->op == OP_FCONST && c->ty == IT_F32 && (u32)c->a == bits;
}

static const char *fast_tmpl(u32 v, const char *name, const char *tmpl)
{
    Ins *i = &PF->ins[v];
    if (opt_nofastcall)
        return tmpl;
    if (!strcmp(name, "sqrtf"))
        return "sqrt.approx.ftz.f32 $d, $0;";
    if (!strcmp(name, "fabsf"))
        return "abs.ftz.f32 $d, $0;";
    if (!strcmp(name, "fminf"))
        return "min.ftz.f32 $d, $0, $1;";
    if (!strcmp(name, "fmaxf"))
        return "max.ftz.f32 $d, $0, $1;";
    if (strcmp(name, "fmaf") || i->b != 3)
        return tmpl;
    if (fconst_is(PF->xargs[i->a + 2], 0) || fconst_is(PF->xargs[i->a + 2], 0x80000000u)) {
        if (fconst_is(PF->xargs[i->a], 0x3f800000u))
            return "mov.f32 $d, $1;";
        if (fconst_is(PF->xargs[i->a + 1], 0x3f800000u))
            return "mov.f32 $d, $0;";
        return "mul.ftz.f32 $d, $0, $1;";
    }
    return "fma.rn.ftz.f32 $d, $0, $1, $2;";
}

/* Writes a template, with $d, $0.. or %0.. replaced by operands. */
static void expand(const char *t, char mark, const char *dst, const char **ops, u32 nops)
{
    P("\t");
    for (; *t; t++) {
        if (*t == mark && t[1] == 'd' && mark == '$') {
            P("%s", dst);
            t++;
        } else if (*t == mark && t[1] >= '0' && t[1] <= '9') {
            u32 n = (u32)(t[1] - '0');
            t++;
            if (t[1] >= '0' && t[1] <= '9') {
                n = n * 10 + (u32)(t[1] - '0');
                t++;
            }
            if (n >= nops)
                die("ptx: %s: an assembly template names operand %u, which it does not have", fname, n);
            P("%s", ops[n]);
        } else if (*t == '%' && t[1] == '%' && mark == '%') {
            P("%%");
            t++;
        } else {
            buf_u8(&fb, (u8)*t);
        }
    }
    if (fb.len && fb.p[fb.len - 1] != '\n')
        P("\n");
}

/* ---- global memory ----
   A kernel's pointer parameters are addresses of global memory (nothing else can be handed to one), and so is
   such an address plus or minus a whole number, and a choice between two. Loads, stores and atomic
   operations through them say so: one for memory of any kind finds out which kind as the program runs.
   MCC_NOGLOBAL turns this off. */
static u8 *gl_s; /* the value is such an address */

/* The addresses that are of the function's own stack whichever way they are reached: a local object's, and
   what is computed from one. Read and written as local memory (ld.local, st.local), as nvcc does. */
static u8 *lc_s;

static void lc_find(Func *f)
{
    u32 v, k;
    int changed;
    memset(lc_s, 0, f->nins + 1);
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            int is = 0;
            if (lc_s[v] || i->ty != IT_I64)
                continue;
            if (i->op == OP_SLOT)
                is = 1;
            else if (!i->blk)
                continue;
            else if (i->op == OP_BYVAL)
                is = 1;
            else if (i->op == OP_ADD)
                is = lc_s[RV(i->a)] || lc_s[RV(i->b)];
            else if (i->op == OP_SUB)
                is = lc_s[RV(i->a)];
            else if (i->op == OP_SELECT)
                is = lc_s[RV(i->b)] || lc_s[RV(i->c)];
            else if (i->op == OP_PHI)
                for (k = 0; k < i->b; k++)
                    is |= lc_s[RV(f->xargs[i->a + k])];
            if (is) {
                lc_s[v] = 1;
                changed = 1;
            }
        }
    } while (changed);
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            int ok = 1;
            if (!lc_s[v] || i->op == OP_SLOT || i->op == OP_BYVAL)
                continue;
            if (i->op == OP_ADD) {
                ok = lc_s[RV(i->a)] != lc_s[RV(i->b)];
            } else if (i->op == OP_SUB) {
                ok = lc_s[RV(i->a)] && !lc_s[RV(i->b)];
            } else if (i->op == OP_SELECT) {
                ok = lc_s[RV(i->b)] && lc_s[RV(i->c)];
            } else {
                for (k = 0; k < i->b; k++)
                    if (!lc_s[RV(f->xargs[i->a + k])])
                        ok = 0;
            }
            if (!ok) {
                lc_s[v] = 0;
                changed = 1;
            }
        }
    } while (changed);
}

/* The address a (a register, perhaps with a constant) of local memory, as local memory wants it. */
static const char *lc_addr(const char *a)
{
    static char out[96];
    const char *plus = strchr(a, '+'), *t;
    if (strncmp(a, "%SP", 3) == 0 && (a[3] == '+' || !a[3])) {
        sprintf(out, "%%SPL%s", a + 3);
        return out;
    }
    t = temp(cls_of(IT_I64));
    if (plus) {
        char r[48];
        size_t n = (size_t)(plus - a);
        memcpy(r, a, n);
        r[n] = 0;
        P("\tcvta.to.local.u64 %s, %s;\n", t, r);
        sprintf(out, "%s%s", t, plus);
    } else {
        P("\tcvta.to.local.u64 %s, %s;\n", t, a);
        sprintf(out, "%s", t);
    }
    return out;
}

/* (a pointer that is one of several is of global memory when the others are, or are null: a null arm is
   neutral, level 2) */
#define GL_LEVEL(x) (f->ins[x].op == OP_CONST && !f->ins[x].a && !f->ins[x].b ? 2 : gl_s[x])
static void gl_find(Func *f)
{
    u32 v, k, n, *p;
    int changed;
    /* first what is computed from a pointer parameter at all (the least such set: a number that is counted
       up from 0 is not in it), then, among those, what is so computed whichever way it is reached;
       2: also only read, through a "const T *__restrict__" parameter of the kernel (ld.global.nc, as nvcc) */
    memset(gl_s, 0, f->nins + 1);
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            int is = 0;
            if (gl_s[v] || !i->blk || i->ty != IT_I64)
                continue;
            if (i->op == OP_PARAM)
                is = i->mem == PARAM_GLOBAL || i->mem == PARAM_GLOBAL_NC;
            else if (i->op == OP_ADD)
                is = gl_s[RV(i->a)] || gl_s[RV(i->b)];
            else if (i->op == OP_SUB)
                is = gl_s[RV(i->a)];
            else if (i->op == OP_SELECT)
                is = gl_s[RV(i->b)] || gl_s[RV(i->c)];
            else if (i->op == OP_PHI)
                for (k = 0; k < i->b; k++)
                    is |= gl_s[RV(f->xargs[i->a + k])];
            if (is) {
                gl_s[v] = 1;
                changed = 1;
            }
        }
    } while (changed);
    for (v = 1; v < f->nins; v++)
        if (gl_s[v])
            gl_s[v] = f->ins[v].op != OP_PARAM ? 2 : f->ins[v].mem == PARAM_GLOBAL_NC && !opt_nonc ? 2 : 1;
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            u8 to;
            if (!gl_s[v] || i->op == OP_PARAM)
                continue;
            if (i->op == OP_ADD) {
                u8 x = gl_s[RV(i->a)], y = gl_s[RV(i->b)];
                to = !x != !y ? (x ? x : y) : 0;
            } else if (i->op == OP_SUB) {
                to = !gl_s[RV(i->b)] ? gl_s[RV(i->a)] : 0;
            } else if (i->op == OP_SELECT) {
                u8 x = GL_LEVEL(RV(i->b)), y = GL_LEVEL(RV(i->c));
                to = x < y ? x : y;
            } else {
                to = 2;
                for (k = 0; k < i->b; k++)
                    if (GL_LEVEL(RV(f->xargs[i->a + k])) < to)
                        to = GL_LEVEL(RV(f->xargs[i->a + k]));
            }
            if (to < gl_s[v]) {
                gl_s[v] = to;
                changed = 1;
            }
        }
    } while (changed);
    /* written through after all (a cast takes the const away), or handed to what may write: plain global memory */
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        int w = 0;
        if (!i->blk)
            continue;
        if (i->op == OP_STORE || i->op == OP_MEMZERO || i->op == OP_MEMCPY || i->op == OP_VSTORE) {
            w = gl_s[RV(i->a)] == 2;
        } else if (i->op == OP_CALL) {
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                if (gl_s[RV(p[k])] == 2)
                    w = 1;
            if (w && (i->flags & IF_ASM)) {
                /* an assembly statement that only reads: a load, a copy into shared memory, a prefetch */
                u64 idx;
                const char *t = ir_is_const(f, i->c, &idx) ? asm_stmts[idx].tmpl : 0;
                w = !t || strstr(t, "st.") || strstr(t, "atom") || strstr(t, "red.") || strstr(t, "sust") || strstr(t, "cp.reduce");
            }
        }
        if (w) {
            for (k = 1; k < f->nins; k++)
                if (gl_s[k] == 2)
                    gl_s[k] = 1;
            return;
        }
    }
}

/* The template of an assembly statement, whose atomic operations on [%n] are told that the address is of
   global memory when operand n is one. */
static const char *gl_tmpl(const char *t, const u32 *vals, u32 nops)
{
    static char out[512];
    const char *q = strstr(t, "[%"), *s;
    char *o = out;
    u32 n = 0;
    if (!gl_s || !q || q[2] < '0' || q[2] > '9')
        return t;
    for (s = q + 2; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (u32)(*s - '0');
    if (*s != ']' || n >= nops || !vals[n] || !gl_s[RV(vals[n])] || strstr(q + 2, "[%"))
        return t;
    if (strlen(t) > 400)
        return t;
    for (s = t; *s; ) {
        int at = (s == t || s[-1] == ' ' || s[-1] == ';' || s[-1] == '\t' || s[-1] == '{' || s[-1] == '\n');
        u32 len = at && !strncmp(s, "atom.", 5) ? 5 : at && !strncmp(s, "red.", 4) ? 4 : 0;
        if (len && strncmp(s + len, "global.", 7) && strncmp(s + len, "shared.", 7) && strncmp(s + len, "local.", 6)) {
            memcpy(o, s, len);
            o += len;
            memcpy(o, "global.", 7);
            o += 7;
            s += len;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
    return out;
}

static void emit_asm(u32 v)
{
    Ins *i = &PF->ins[v];
    u64 idx;
    AsmStmt *st;
    const char *ops[32], *post[32];
    char *names[32];
    u32 k, a = 0, w, nh = 1, vals[32];
    ir_is_const(PF, i->c, &idx);
    st = &asm_stmts[idx];
    for (k = 0; k < st->nops; k++) {
        AsmOp *o = &st->ops[k];
        post[k] = 0;
        vals[k] = 0;
        if (o->kind == 'i') {
            ops[k] = o->text ? o->text : "0";
            continue;
        }
        if (o->kind == 'm')
            die("ptx: %s: memory operands in assembly are not supported", fname);
        if (o->out) {
            /* the register of the value that reads this output, if anything does */
            const char *r = 0;
            /* (anywhere: they need not follow side by side, each may be stored at once, and the
               optimiser moves them like any other value) */
            for (w = 1; w < PF->nins; w++)
                if (PF->ins[w].op == OP_ASMOUT && PF->ins[w].blk && PF->ins[w].a == v && PF->ins[w].b == k)
                    r = reg(w);
            if (!r)
                r = temp(cls_of(o->ty));
            if (o->out == 2) {
                const char *from = opnd(PF->xargs[i->a + a]);
                if (strcmp(from, r))
                    P("\t%s %s, %s;\n", mov_of(o->ty), r, from);
                a++;
            }
            if (o->cls == 'h') {
                names[k] = (char *)obuf();
                sprintf(names[k], "%%h%u", nh++);
                if (o->out == 2)
                    P("\tcvt.u16.u32 %s, %s;\n", names[k], r);
                post[k] = r;
                ops[k] = names[k];
            } else {
                ops[k] = r;
            }
        } else {
            u32 av = PF->xargs[i->a + a];
            a++;
            vals[k] = av;
            if (o->cls == 'h') {
                names[k] = (char *)obuf();
                sprintf(names[k], "%%h%u", nh++);
                P("\tcvt.u16.u32 %s, %s;\n", names[k], ropnd(av));
                ops[k] = names[k];
            } else {
                ops[k] = opnd(av);
            }
        }
        if (nh > 11)
            die("ptx: %s: too many 16-bit assembly operands", fname);
    }
    {
        /* an atomic operation whose old value nothing reads is a reduction (red), as nvcc writes atomicAdd(p, v)
           standing alone: the GPU need not send the value back (mcuda_device.h's "atom.OP %0, [%1], %2;") */
        const char *t = st->tmpl, *e;
        char red[96], shd[96];
        int unread = st->nops == 3 && st->ops[0].out == 1 && !st->ops[1].out && !st->ops[2].out && !opt_nored;
        for (w = 1; unread && w < PF->nins; w++)
            if (PF->ins[w].op == OP_ASMOUT && PF->ins[w].blk && PF->ins[w].a == v && PF->ins[w].b == 0)
                unread = 0;
        if (unread && !strncmp(t, "atom.", 5) && (e = strstr(t, " %0, [%1], %2;")) != 0 && !e[14] && e - t < 64 &&
            !memchr(t, ' ', (size_t)(e - t)) && !strstr(t, ".exch") && !strstr(t, ".cas")) {
            /* red has add / min / max / inc / dec / and / or / xor only: an unread atomicExch stays atom (ptxas: "Illegal
               operation '.exch' for instruction 'red'", lp_f4wgrad_i0 / i4's gy_unsafe flag) */
            sprintf(red, "red.%.*s [%%1], %%2;", (int)(e - t - 5), t + 5);
            t = red;
        }
        if (st->nops == 3 && !st->ops[1].out && vals[1] && sh_s && !opt_nored && strlen(t) < 80 &&
            (!strncmp(t, "atom.", 5) || !strncmp(t, "red.", 4)) && strstr(t, "[%1]") && !strstr(t, "global.") &&
            !strstr(t, "shared") && !strstr(t, "local.")) {
            /* ... and one on shared memory is told so, with the 32-bit address (as nvcc's atom.shared / red.shared) */
            const char *sa = sh_maddr(vals[1]);
            if (sa) {
                u32 n = t[0] == 'a' ? 5 : 4;
                sprintf(shd, "%.*sshared.%s", (int)n, t, t + n);
                ops[1] = sa;
                t = shd;
            }
        }
        expand(gl_tmpl(t, vals, st->nops), '%', "", ops, st->nops);
    }
    for (k = 0; k < st->nops; k++)
        if (post[k])
            P("\tcvt.u32.u16 %s, %s;\n", post[k], ops[k]);
}

static void need_fn(u32 sym)
{
    if (nneed == capneed) {
        capneed = capneed ? capneed * 2 : 32;
        need = (u32 *)xrealloc(need, capneed * sizeof(u32));
    }
    need[nneed++] = sym;
}

static void emit_call(u32 v)
{
    Ins *i = &PF->ins[v];
    u32 callee = RV(i->c), k, sym;
    const char *name, *args[128];
    u8 ptys[128];
    const Intrin *in;
    PSig *s;

    if (i->flags & IF_ASM) {
        u32 x = sh_s ? sh_to_shared(v) : 0, w;
        if (x) {
            /* where in shared memory an address there is: its low half */
            for (w = 1; w < PF->nins; w++)
                if (PF->ins[w].op == OP_ASMOUT && PF->ins[w].blk && PF->ins[w].a == v && PF->ins[w].b == 0) {
                    const char *lo = sh_low_r(x);
                    P("\tcvt.u64.u32 %s, %s;\n", reg(w), lo);
                }
            return;
        }
        emit_asm(v);
        return;
    }
    if (i->flags & IF_ALLOCA)
        die("ptx: %s: alloca and variable length arrays are not supported in device code", fname);
    if (PF->ins[callee].op != OP_ADDR || PF->ins[callee].b)
        die("ptx: %s: calls through pointers are not supported in device code", fname);
    if (i->b > 120)
        die("ptx: %s: too many arguments", fname);
    sym = PF->ins[callee].a;
    if (PO->syms[sym].isdev) {
        P("\tmov.b32 %s, 1;\n", reg(v));
        return;
    }
    name = pname(sym);
    s = sig_find(sym);
    if (!s || (!s->defined && !s->entry)) {
        for (in = intrins; in->name; in++)
            if (strcmp(in->name, name) == 0) {
                if (!used[v])
                    return; /* arithmetic that nothing reads */
                for (k = 0; k < i->b; k++)
                    args[k] = opnd(PF->xargs[i->a + k]);
                expand(opt_fastmath ? fast_tmpl(v, name, in->tmpl) : in->tmpl, '$', i->ty != IT_VOID ? reg(v) : "", args, i->b);
                return;
            }
    }
    for (k = 0; k < i->b; k++) {
        ptys[k] = PF->ins[RV(PF->xargs[i->a + k])].ty;
        args[k] = ropnd(PF->xargs[i->a + k]);
    }
    if (!s) {
        s = sig_add(sym, i->ty, i->b, ptys);
        need_fn(sym);
    }
    P("\t{\n");
    for (k = 0; k < i->b; k++) {
        P("\t.param %s param%u;\n", pty_name(ptys[k], 0), k);
        P("\tst.param.%s [param%u+0], %s;\n", vty(ptys[k]), k, args[k]);
    }
    if (i->ty != IT_VOID)
        P("\t.param %s retval0;\n\tcall.uni (retval0), ", pty_name(i->ty, 0));
    else
        P("\tcall.uni ");
    P("$F%u$", sym); /* the name is settled when the module is finished */
    if (i->b) {
        P(", (");
        for (k = 0; k < i->b; k++)
            P("%sparam%u", k ? ", " : "", k);
        P(")");
    }
    P(";\n");
    if (i->ty != IT_VOID)
        P("\tld.param.%s %s, [retval0+0];\n", vty(i->ty), reg(v));
    P("\t}\n");
}

/* ---- block copies ---- */

/* n bytes from src (0: zeros) to dst, in loads and stores no wider than al, which both addresses are known
   to be multiples of: a GPU faults on an access that is not aligned to its own width. */
/* The base and the space ("local." or "global.", else "") of address a, for emit_copy: a place in the frame is
   %SPL and its offset (*off), not the generic %SP. */
static const char *copy_base(u32 a, char *buf, i64 *off, const char **space)
{
    Ins *i;
    u32 r = RV(a);
    *off = 0;
    *space = "";
    if (!opt_nolocal && remat) {
        i64 o = 0;
        if (addr_sum(r, &o))
            r = RV(PF->ins[r].a);
        i = &PF->ins[r];
        if (i->op == OP_SLOT || (i->op == OP_ADDR && special_of(i->a) >= 0)) {
            *off = o + (i64)(i->op == OP_SLOT ? slotoff[i->a] : special_off[special_of(i->a)]) + (i64)(i32)i->b;
            *space = "local.";
            strcpy(buf, "%SPL");
            return buf;
        }
    }
    if (gl_s && gl_s[RV(a)])
        *space = "global.";
    strcpy(buf, ropnd(a));
    return buf;
}

static void emit_copy(u32 dst, u32 src, u64 n, u32 al)
{
    const char *d, *s = 0, *t, *dsp, *ssp = "";
    char dn[48], sn[48];
    u64 off = 0;
    i64 doff, soff = 0;
    d = copy_base(dst, dn, &doff, &dsp);
    if (src)
        s = copy_base(src, sn, &soff, &ssp);
    if (n > 128) {
        /* too long to write out: a loop over the whole units, and the rest written out below */
        static u32 nloop;
        u32 w = al >= 8 ? 8 : al >= 4 ? 4 : al >= 2 ? 2 : 1;
        const char *ty = w == 8 ? "u64" : w == 4 ? "u32" : w == 2 ? "u16" : "u8";
        char ix[24], ad[24];
        u64 whole = n - n % w;
        strcpy(ix, temp(1));
        strcpy(ad, temp(1));
        t = temp(w == 8 ? 1 : 0);
        P("\tmov.u64 %s, 0;\nLM%u:\n", ix, nloop);
        if (s)
            P("\tadd.u64 %s, %s, %s;\n\tld.%s%s %s, [%s%+ld];\n", ad, s, ix, ssp, ty, t, ad, (long)soff);
        else
            P("\t%s %s, 0;\n", w == 8 ? "mov.b64" : "mov.b32", t);
        P("\tadd.u64 %s, %s, %s;\n\tst.%s%s [%s%+ld], %s;\n", ad, d, ix, dsp, ty, ad, (long)doff, t);
        P("\tadd.u64 %s, %s, %u;\n\tsetp.lt.u64 %%p0, %s, %lu;\n\t@%%p0 bra LM%u;\n", ix, ix, w, ix, (unsigned long)whole, nloop);
        nloop++;
        off = whole;
    }
    while (off < n) {
        u32 w = n - off >= 8 && al >= 8 ? 8 : n - off >= 4 && al >= 4 ? 4 : n - off >= 2 && al >= 2 ? 2 : 1;
        const char *ty = w == 8 ? "u64" : w == 4 ? "u32" : w == 2 ? "u16" : "u8";
        t = temp(w == 8 ? 1 : 0);
        if (s)
            P("\tld.%s%s %s, [%s%+ld];\n", ssp, ty, t, s, (long)(soff + (i64)off));
        else
            P("\t%s %s, 0;\n", w == 8 ? "mov.b64" : "mov.b32", t);
        P("\tst.%s%s [%s%+ld], %s;\n", dsp, ty, d, (long)(doff + (i64)off), t);
        off += w;
    }
}

/* ---- instructions ---- */

static void emit_fused(u32 v);

/* A 64-bit division is slow on a GPU and the numbers are mostly small: when neither has a bit above the
   low 32, the 32-bit division is done instead (as LLVM's does for this target). Not for a constant divisor,
   which needs no division at all. */
static int div_short(u32 v, Ins *i, const char *what, int sg)
{
    static u32 ndiv;
    char d[32], a[32], b[32], t[32], x[32], y[32];
    if (opt_size || PF->ins[RV(i->b)].op == OP_CONST || PF->ins[RV(i->a)].op == OP_CONST)
        return 0;
    strcpy(d, reg(v));
    strcpy(a, ropnd(i->a));
    strcpy(b, ropnd(i->b));
    strcpy(t, temp(1));
    strcpy(x, temp(0));
    strcpy(y, temp(0));
    P("\tor.b64 %s, %s, %s;\n\tshr.u64 %s, %s, 32;\n\tsetp.ne.u64 %%p1, %s, 0;\n\t@%%p1 bra LDS%u;\n", t, a, b, t, t, t, ndiv);
    P("\tcvt.u32.u64 %s, %s;\n\tcvt.u32.u64 %s, %s;\n\t%s.u32 %s, %s, %s;\n", x, a, y, b, what, x, x, y);
    P("\tcvt.u64.u32 %s, %s;\n\tbra.uni LDE%u;\nLDS%u:\n", d, x, ndiv, ndiv);
    P("\t%s.%c64 %s, %s, %s;\nLDE%u:\n", what, sg, d, a, b, ndiv);
    ndiv++;
    return 1;
}

/* The 64-bit product v is of two 32-bit numbers made wider (or of one and a constant that fits): 0, or 's' or
   'u' for how they were made wider, and the two as they were. A GPU multiplies 32 bits into 64 in one
   instruction; the product of two 64-bit registers is several. */
static int wide_kind(u32 v, u32 *x, u32 *y)
{
    Ins *i = &PF->ins[v], *a, *b;
    u64 c;
    int shl = i->op == OP_SHL;
    if ((i->op != OP_MUL && !shl) || i->ty != IT_I64 || opt_nowide)
        return 0;
    a = &PF->ins[RV(i->a)];
    if (shl) {
        /* shifted left as it is made wider: times a power of two */
        if ((a->op != OP_SEXT32 && a->op != OP_ZEXT32) || !ir_is_const(PF, RV(i->b), &c) || c < 1 || c > 30)
            return 0;
        *x = RV(a->a);
        *y = 0;
        return a->op == OP_SEXT32 ? 's' : 'u';
    }
    b = &PF->ins[RV(i->b)];
    if (a->op != OP_SEXT32 && a->op != OP_ZEXT32)
        return 0;
    *x = RV(a->a);
    *y = 0;
    if (b->op == a->op) {
        *y = RV(b->a);
    } else if (ir_is_const(PF, RV(i->b), &c)) {
        if (a->op == OP_SEXT32 ? (i64)c < -(i64)0x7fffffff || (i64)c > (i64)0x7fffffff : c > 0xffffffffUL)
            return 0;
    } else {
        return 0;
    }
    return a->op == OP_SEXT32 ? 's' : 'u';
}

static int wide_mul(u32 v)
{
    Ins *i = &PF->ins[v];
    u32 x, y;
    u64 c = 0;
    int k = wide_kind(v, &x, &y);
    const char *a;
    if (!k)
        return 0;
    a = ropnd(x);
    if (y) {
        P("\tmul.wide.%c32 %s, %s, %s;\n", k, reg(v), a, opnd(y));
    } else {
        ir_is_const(PF, RV(i->b), &c);
        if (i->op == OP_SHL)
            c = (u64)1 << c;
        if (k == 's')
            P("\tmul.wide.s32 %s, %s, %d;\n", reg(v), a, (int)(i64)c);
        else
            P("\tmul.wide.u32 %s, %s, %lu;\n", reg(v), a, (unsigned long)c);
    }
    return 1;
}

static void emit_ins(u32 v)
{
    Ins *i = &PF->ins[v];
    int ty = i->ty, oty;
    const char *d, *a, *b, *t;

    if (sh_s && ty == IT_I64 && sh_emit(v))
        return;
    switch (i->op) {
    case OP_NOP: case OP_COPY: case OP_PARAM: case OP_ASMOUT:
        return;
    case OP_BYVAL:
        P("\tadd.u64 %s, %%SP, %lu;\n", reg(v), (unsigned long)byoff[v]);
        emit_copy(v, i->a, i->b, i->c);
        return;
    case OP_ADD:
        if (nuse[v] < 255 && nuse[v] == ause[v])
            return; /* read only as addresses, which add the constant themselves */
        P("\tadd.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_SUB: P("\tsub.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_MUL:
        if (ty == IT_I64 && wide_mul(v))
            return;
        P("\tmul.lo.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_SDIV:
        if (ty == IT_I64 && div_short(v, i, "div", 's'))
            return;
        P("\tdiv.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_UDIV:
        if (ty == IT_I64 && div_short(v, i, "div", 'u'))
            return;
        P("\tdiv.%s %s, %s, %s;\n", ity(ty, 'u'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_SREM:
        if (ty == IT_I64 && div_short(v, i, "rem", 's'))
            return;
        P("\trem.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_UREM:
        if (ty == IT_I64 && div_short(v, i, "rem", 'u'))
            return;
        P("\trem.%s %s, %s, %s;\n", ity(ty, 'u'), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_AND: case OP_OR:
        if (ptree[v] == 2)
            return;
        if (ptree[v]) {
            pred_into(v, 0, 1);
            P("\tselp.b32 %s, 1, 0, %%p0;\n", reg(v));
            return;
        }
        if (i->op == OP_AND) {
            P("\tand.%s %s, %s, %s;\n", ity(ty, 'b'), reg(v), ropnd(i->a), opnd(i->b));
            return;
        }
        P("\tor.%s %s, %s, %s;\n", ity(ty, 'b'), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_XOR: P("\txor.%s %s, %s, %s;\n", ity(ty, 'b'), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_SHL:
    case OP_SHR:
    case OP_SAR: {
        /* C leaves a count of the width or more undefined; hardware takes it modulo the width, as here */
        const char *op = i->op == OP_SHL ? "shl" : "shr", *tt = ity(ty, i->op == OP_SAR ? 's' : i->op == OP_SHR ? 'u' : 'b');
        u32 cv = RV(i->b);
        u64 c;
        if (ty == IT_I64 && i->op == OP_SHL && wide_mul(v))
            return;
        a = ropnd(i->a);
        if (ir_is_const(PF, cv, &c)) {
            P("\t%s.%s %s, %s, %u;\n", op, tt, reg(v), a, (unsigned)(c & (ty == IT_I64 ? 63 : 31)));
            return;
        }
        t = temp(0);
        if (PF->ins[cv].ty == IT_I64)
            P("\tcvt.u32.u64 %s, %s;\n", t, reg(cv));
        else
            P("\tmov.b32 %s, %s;\n", t, reg(cv));
        P("\tand.b32 %s, %s, %d;\n", t, t, ty == IT_I64 ? 63 : 31);
        P("\t%s.%s %s, %s, %s;\n", op, tt, reg(v), a, t);
        return;
    }
    case OP_NEG: P("\tneg.%s %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a)); return;
    case OP_NOT: P("\tnot.%s %s, %s;\n", ity(ty, 'b'), reg(v), ropnd(i->a)); return;
    case OP_FADD:
    case OP_FSUB:
        if (fuse[v]) {
            emit_fused(v);
            return;
        }
        /* with -fmad=false the rounding is stated, which keeps an assembler from fusing too */
        P("\t%s%s%s.%s %s, %s, %s;\n", i->op == OP_FADD ? "add" : "sub", opt_fmad ? "" : ".rn", FTZ(ty), fty(ty), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_FMUL:
        if (fused[v] && fused[v] == nuse[v])
            return; /* every reader took it into a fused operation */
        P("\tmul%s%s.%s %s, %s, %s;\n", opt_fmad ? "" : ".rn", FTZ(ty), fty(ty), reg(v), ropnd(i->a), opnd(i->b));
        return;
    case OP_FDIV: P("\tdiv.%s.%s %s, %s, %s;\n", opt_fastmath && ty == IT_F32 ? "approx.ftz" : "rn", fty(ty), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_FNEG:
        if (fused[v])
            return;
        P("\tneg.%s %s, %s;\n", fty(ty), reg(v), ropnd(i->a));
        return;
    case OP_SEXT8: P("\tcvt.%s.s8 %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a)); return;
    case OP_SEXT16: P("\tcvt.%s.s16 %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a)); return;
    case OP_ZEXT8: P("\tand.%s %s, %s, 255;\n", ity(ty, 'b'), reg(v), ropnd(i->a)); return;
    case OP_ZEXT16: P("\tand.%s %s, %s, 65535;\n", ity(ty, 'b'), reg(v), ropnd(i->a)); return;
    case OP_BITCAST: P("\tmov.b%d %s, %s;\n", ty == IT_I64 || ty == IT_F64 ? 64 : 32, reg(v), ropnd(i->a)); return;
    case OP_SEXT32: case OP_ZEXT32:
        if (xrem && xrem[v])
            return;
        P("\tcvt.%s %s, %s;\n", i->op == OP_SEXT32 ? "s64.s32" : "u64.u32", reg(v), ropnd(i->a));
        return;
    case OP_TRUNC: P("\tcvt.u32.u64 %s, %s;\n", reg(v), ropnd(i->a)); return;
    case OP_SITOF:
    case OP_UITOF:
        oty = PF->ins[RV(i->a)].ty;
        P("\tcvt.rn.%s.%s %s, %s;\n", fty(ty), ity(oty, i->op == OP_SITOF ? 's' : 'u'), reg(v), ropnd(i->a));
        return;
    case OP_FTOSI:
    case OP_FTOUI:
        oty = PF->ins[RV(i->a)].ty;
        P("\tcvt.rzi%s.%s.%s %s, %s;\n", FTZ(oty), ity(ty, i->op == OP_FTOSI ? 's' : 'u'), fty(oty), reg(v), ropnd(i->a));
        return;
    case OP_FEXT: P("\tcvt%s.f64.f32 %s, %s;\n", FTZ(IT_F32), reg(v), ropnd(i->a)); return;
    case OP_FTRUNC: P("\tcvt.rn%s.f32.f64 %s, %s;\n", FTZ(IT_F32), reg(v), ropnd(i->a)); return;
    case OP_UMULH: P("\tmul.hi.%s %s, %s, %s;\n", ity(ty, 'u'), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_SMULH: P("\tmul.hi.%s %s, %s, %s;\n", ity(ty, 's'), reg(v), ropnd(i->a), opnd(i->b)); return;
    case OP_ISCONST: P("\tmov.b32 %s, 0;\n", reg(v)); return;
    case OP_CLZ:
    case OP_CTZ:
    case OP_POPCNT:
        oty = PF->ins[RV(i->a)].ty;
        a = ropnd(i->a);
        if (i->op == OP_CTZ) {
            t = temp(cls_of(oty));
            P("\tbrev.%s %s, %s;\n", ity(oty, 'b'), t, a);
            a = t;
        }
        d = ty == IT_I64 ? temp(0) : reg(v);
        P("\t%s.%s %s, %s;\n", i->op == OP_POPCNT ? "popc" : "clz", ity(oty, 'b'), d, a);
        if (ty == IT_I64)
            P("\tcvt.u64.u32 %s, %s;\n", reg(v), d);
        return;
    case OP_BSWAP:
        if (ty != IT_I32) {
            /* the halves change places, each with its bytes turned round */
            const char *lo = temp(0), *hi = temp(0), *t = temp(1), *w = temp(1);
            a = ropnd(i->a);
            P("\tcvt.u32.u64 %s, %s;\n", lo, a);
            P("\tshr.u64 %s, %s, 32;\n", t, a);
            P("\tcvt.u32.u64 %s, %s;\n", hi, t);
            P("\tprmt.b32 %s, %s, 0, 0x0123;\n", lo, lo);
            P("\tprmt.b32 %s, %s, 0, 0x0123;\n", hi, hi);
            P("\tcvt.u64.u32 %s, %s;\n", t, lo);
            P("\tshl.b64 %s, %s, 32;\n", t, t);
            P("\tcvt.u64.u32 %s, %s;\n", w, hi);
            P("\tor.b64 %s, %s, %s;\n", reg(v), t, w);
            return;
        }
        P("\tprmt.b32 %s, %s, 0, 0x0123;\n", reg(v), ropnd(i->a));
        return;
    case OP_LOAD: {
        static const char *const lt[] = {"s8", "u8", "s16", "u16", "u32", "u64", "f32", "f64"};
        if (special_load(v)) {
            P("\tmov.u32 %s, %s;\n", reg(v), special_load(v));
            return;
        }
        if ((a = sh_maddr(i->a)) != 0) {
            P("\tld.shared.%s %s, [%s];\n", lt[i->mem], reg(v), a);
            return;
        }
        if ((a = local_maddr(i->a)) != 0) {
            P("\tld.local.%s %s, [%s];\n", lt[i->mem], reg(v), a);
            return;
        }
        a = maddr(i->a);
        if (lc_s && lc_s[RV(i->a)]) {
            a = lc_addr(a);
            P("\tld.local.%s %s, [%s];\n", lt[i->mem], reg(v), a);
            return;
        }
        P("\tld.%s%s %s, [%s];\n", (i->flags & IF_NC) && !opt_nonc ? "global.nc." : gl_s && gl_s[RV(i->a)] ? gl_s[RV(i->a)] == 2 ? "global.nc." : "global." : "",
          lt[i->mem], reg(v), a);
        return;
    }
    case OP_STORE: {
        static const char *const stt[] = {"u8", "u8", "u16", "u16", "u32", "u64", "f32", "f64"};
        if ((a = sh_maddr(i->a)) != 0) {
            b = ropnd(i->b);
            P("\tst.shared.%s [%s], %s;\n", stt[i->mem], a, b);
            return;
        }
        if ((a = local_maddr(i->a)) != 0) {
            b = ropnd(i->b);
            P("\tst.local.%s [%s], %s;\n", stt[i->mem], a, b);
            return;
        }
        a = maddr(i->a);
        b = ropnd(i->b);
        if (lc_s && lc_s[RV(i->a)]) {
            a = lc_addr(a);
            P("\tst.local.%s [%s], %s;\n", stt[i->mem], a, b);
            return;
        }
        P("\tst.%s%s [%s], %s;\n", gl_s && gl_s[RV(i->a)] ? "global." : "", stt[i->mem], a, b);
        return;
    }
    case OP_CALL:
        emit_call(v);
        return;
    case OP_MEMCPY:
        emit_copy(i->a, i->b, i->c, i->mem);
        return;
    case OP_MEMZERO:
        emit_copy(i->a, 0, i->c, i->mem);
        return;
    case OP_SELECT: {
        /* the smaller or the larger of the two numbers compared: one instruction, and no predicate */
        Ins *ci = &PF->ins[RV(i->a)];
        u32 x = RV(ci->a), y = RV(ci->b), sb = RV(i->b), sc = RV(i->c);
        if (ci->blk && ((sb == x && sc == y) || (sb == y && sc == x)) && x != y && !opt_nominmax) {
            /* min.f32 / max.f32 give the other operand for a NaN where the select gives b: only when no NaN can come
               (-ffinite-math-only). nvcc's -use_fast_math does not promise that, and code that guards against NaN
               (x < lo ? lo : x) must keep its select */
            int isf = ci->op >= OP_FLT && ci->op <= OP_FGE && opt_finite_math;
            int lt = ci->op == OP_SLT || ci->op == OP_SLE || ci->op == OP_ULT || ci->op == OP_ULE || (isf && (ci->op == OP_FLT || ci->op == OP_FLE));
            int gt = ci->op == OP_SGT || ci->op == OP_SGE || ci->op == OP_UGT || ci->op == OP_UGE || (isf && (ci->op == OP_FGT || ci->op == OP_FGE));
            if (lt || gt) {
                int uns = ci->op >= OP_ULT && ci->op <= OP_UGE;
                int mn = lt == (sb == x);
                a = ropnd(x);
                b = opnd(y);
                P("\t%s.%s %s, %s, %s;\n", mn ? "min" : "max", isf ? fty(ty) : ity(ty, uns ? 'u' : 's'), reg(v), a, b);
                return;
            }
        }
    }
        a = opnd(i->b);
        b = opnd(i->c);
        cond_pred(i->a);
        P("\tselp.%s %s, %s, %s, %%p0;\n", vty(ty), reg(v), a, b);
        return;
    case OP_PIN:
        P("\t%s %s, %s;\n", mov_of(ty), reg(v), opnd(i->a));
        return;
    default:
        if (is_compare(i->op)) {
            if (nuse[v] < 255 && nuse[v] == cuse[v])
                return;
            set_pred(v);
            P("\tselp.b32 %s, 1, 0, %%p0;\n", reg(v));
            return;
        }
        die("ptx: %s: IR operation %d has no PTX form", fname, i->op);
    }
}

/* The moves that give the phis of block s their values on the edge from p; nth tells apart
   two edges between the same pair of blocks. */
static int edge_dry; /* 1: edge_moves only finds out whether there is any move, and makes it 2 if so */

static void edge_moves(u32 p, u32 s, u32 nth)
{
    Block *sb = &PF->blk[s];
    u32 k, v, n = 0, seen = 0, pi = 0;
    const char *tmp[64], *dst[64];
    int clash = 0;
    if (!sb->phis)
        return;
    for (k = 0; k < sb->npreds; k++)
        if (sb->preds[k] == p) {
            pi = k;
            if (seen++ == nth)
                break;
        }
    for (v = sb->phis; v; v = PF->ins[v].next) {
        u32 src = RV(PF->xargs[PF->ins[v].a + pi]);
        if (!used[v])
            continue;
        if (PF->ins[src].op == OP_PHI && PF->ins[src].blk == s && src != v)
            clash = 1;
        if (regno[src] && PF->ins[src].op == OP_ASMOUT && regno[src] != regno[v]) {
            u32 u; /* a result that took over a phi's register (coalesce_asm) */
            for (u = sb->phis; u; u = PF->ins[u].next)
                if (u != v && used[u] && regno[u] == regno[src] && cls_of(PF->ins[u].ty) == cls_of(PF->ins[src].ty))
                    clash = 1;
        }
    }
    for (v = sb->phis; v; v = PF->ins[v].next) {
        u32 src = RV(PF->xargs[PF->ins[v].a + pi]);
        if (!used[v] || src == v || !src)
            continue;
        if (regno[src] && regno[src] == regno[v] && PF->ins[src].op != OP_CONST && cls_of(PF->ins[src].ty) == cls_of(PF->ins[v].ty))
            continue; /* they share a register (coalesce) */
        if (edge_dry) {
            edge_dry = 2;
            return;
        }
        if (!clash) {
            P("\t%s %s, %s;\n", mov_of(PF->ins[v].ty), reg(v), opnd(src));
            continue;
        }
        if (n == 64)
            die("ptx: %s: too many phis", fname);
        tmp[n] = temp(cls_of(PF->ins[v].ty));
        dst[n] = reg(v);
        P("\t%s %s, %s;\n", mov_of(PF->ins[v].ty), tmp[n], opnd(src));
        n++;
    }
    for (k = 0; k < n; k++) {
        /* the class decides the mnemonic; read it back from the register's stem */
        const char *m = dst[k][1] == 'f' ? (dst[k][2] == 'd' ? "mov.f64" : "mov.f32")
                                         : (dst[k][2] == 'd' ? "mov.b64" : "mov.b32");
        P("\t%s %s, %s;\n", m, dst[k], tmp[k]);
    }
}

static void mark(u32 v)
{
    v = RV(v);
    if (v) {
        used[v] = 1;
        if (nuse[v] < 255)
            nuse[v]++;
    }
}

/* A member of threadIdx, blockIdx, blockDim or gridDim that v reads: the register that holds it, which is
   read where it is wanted; the object in memory is only made for other uses of its address. */
static const char *special_load(u32 v)
{
    static char buf[4][16];
    static u32 turn;
    Ins *i = &PF->ins[v], *a;
    int sp;
    char *r;
    if (i->op != OP_LOAD || i->mem != M_32)
        return 0; /* (only a load's first operand is known to be a value) */
    a = &PF->ins[RV(i->a)];
    if (a->op != OP_ADDR || (sp = special_of(a->a)) < 0 || a->b > 8 || (a->b & 3))
        return 0;
    r = buf[turn++ & 3];
    sprintf(r, "%s.%c", special_reg[sp], "xyz"[a->b >> 2]);
    return r;
}

/* A condition known here: "is this device code" is 1, and so is its comparison with 0. -1: not known. */
static int known_cond(u32 v)
{
    Ins *i;
    int inv = 0;
    v = RV(v);
    i = &PF->ins[v];
    if ((i->op == OP_NE || i->op == OP_EQ) && PF->ins[RV(i->b)].op == OP_CONST && !PF->ins[RV(i->b)].a && !PF->ins[RV(i->b)].b) {
        inv = i->op == OP_EQ;
        i = &PF->ins[RV(i->a)];
    }
    if (i->op == OP_CALL && !(i->flags & (IF_ASM | IF_ALLOCA)) && PF->ins[RV(i->c)].op == OP_ADDR && PO->syms[PF->ins[RV(i->c)].a].isdev)
        return !inv;
    return -1;
}

/* v is read as a condition: such a reading does the comparison over again (cond_pred), so a comparison
   with no other reader need not be computed where it stands. */
static void mark_cond(u32 v)
{
    v = RV(v);
    if (v && cuse[v] < 255)
        cuse[v]++;
}

/* nvcc's -fmad, which is on unless turned off: a product that only one sum or difference reads, in the same
   block, is not rounded by itself: the two become one fused operation. What is fused is what comes out of a
   real GPU for nvcc's code (tests/cuda/fmad.cu), which is more than nvcc's PTX shows, the assembler fusing
   products and sums by itself: differences are fused like sums, also through a negation of the product; a
   product with a second reader is not; and of two products that are added, the one of lower rank is taken,
   as LLVM orders the operands of a sum (rank: the number of a parameter, of a value read from memory or
   given by a call its place in the function, of anything else one more than its operands').
   fuse[v] of the sum: 1 or 2, the operand that is the product; + 4: behind a negation. */
static u32 *frank;
static u8 *addu;

static void rank_all(const u8 *live)
{
    Func *f = PF;
    u32 b, v, k, n, *p, r = 2 + f->nparams;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        u32 base = ++r << 16;
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            frank[v] = ++base;
        /* nvcc orders its sums before it turns a for or while loop over: the loop's variables are then in a
           block of their own, before the body, and rank below everything that the body reads from memory */
        if (bb->lhdr && bb->phis)
            base = ++r << 16;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 m = 0;
            if (i->op == OP_LOAD || i->op == OP_CALL || i->op == OP_ASMOUT || i->op == OP_STORE) {
                frank[v] = ++base;
                continue;
            }
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]), xr = f->ins[x].op == OP_PARAM ? 3 + f->ins[x].a : frank[x];
                if (xr > m)
                    m = xr;
            }
            frank[v] = i->op == OP_FNEG || i->op == OP_NOT ? m : m + 1;
        }
    }
}

static u32 rank_of(u32 x)
{
    x = RV(x);
    return PF->ins[x].op == OP_PARAM ? 3 + PF->ins[x].a : frank[x];
}

/* The product that operand x of the sum v is, or stands negated behind (*neg). */
static u32 fuse_product(u32 v, u32 x, int *neg)
{
    Ins *m;
    x = RV(x);
    m = &PF->ins[x];
    *neg = 0;
    if (m->op == OP_FNEG && m->blk == PF->ins[v].blk && nuse[x] == 1) {
        x = RV(m->a);
        m = &PF->ins[x];
        *neg = 1;
    }
    /* every reader of the product is such a sum (counted in frank beforehand): then it is not computed at all */
    return m->op == OP_FMUL && m->blk == PF->ins[v].blk && nuse[x] < 255 && addu[x] == nuse[x] ? x : 0;
}

/* addu[m]: how many of the readers of the product m are sums or differences of its block (a negation with
   one reader, a sum, counts as that sum). */
static void count_sums(void)
{
    u32 v, k;
    for (v = 1; v < PF->nins; v++) {
        Ins *i = &PF->ins[v];
        if (!used[v] || !i->blk || (i->op != OP_FADD && i->op != OP_FSUB) || RV(i->a) == RV(i->b))
            continue;
        for (k = 0; k < 2; k++) {
            u32 x = RV(k ? i->b : i->a);
            Ins *m = &PF->ins[x];
            if (m->op == OP_FNEG && m->blk == i->blk && nuse[x] == 1) {
                x = RV(m->a);
                m = &PF->ins[x];
            }
            if (m->op == OP_FMUL && m->blk == i->blk && addu[x] < 255)
                addu[x]++;
        }
    }
}

static void fuse_find(void)
{
    u32 v, ma, mb;
    int na, nb;
    for (v = 1; v < PF->nins; v++) {
        Ins *i = &PF->ins[v];
        if (!used[v] || !i->blk || (i->op != OP_FADD && i->op != OP_FSUB))
            continue;
        ma = fuse_product(v, i->a, &na);
        mb = RV(i->b) != RV(i->a) ? fuse_product(v, i->b, &nb) : 0;
        if (ma && mb && i->op == OP_FADD && rank_of(i->b) < rank_of(i->a))
            ma = 0;
        if (ma) {
            fuse[v] = (u8)(1 + 4 * na);
            fused[ma]++;
            if (na)
                fused[RV(i->a)] = 1;
        } else if (mb) {
            fuse[v] = (u8)(2 + 4 * nb);
            fused[mb]++;
            if (nb)
                fused[RV(i->b)] = 1;
        }
    }
}

/* The sum or difference v as a fused operation: +-(x y) +- z. */
static void emit_fused(u32 v)
{
    Ins *i = &PF->ins[v], *m;
    int ty = i->ty, pi = fuse[v] & 3, sp = 0, sz = 0;
    u32 pv = RV(pi == 1 ? i->a : i->b);
    const char *x, *y, *z, *t;
    if (fuse[v] & 4) {
        pv = RV(PF->ins[pv].a);
        sp = 1;
    }
    m = &PF->ins[pv];
    if (i->op == OP_FSUB) {
        if (pi == 1)
            sz = 1;
        else
            sp ^= 1;
    }
    x = ropnd(m->a);
    y = ropnd(m->b);
    z = ropnd(pi == 1 ? i->b : i->a);
    if (sp) {
        t = temp(cls_of(ty));
        P("\tneg.%s %s, %s;\n", fty(ty), t, x);
        x = t;
    }
    if (sz) {
        t = temp(cls_of(ty));
        P("\tneg.%s %s, %s;\n", fty(ty), t, z);
        z = t;
    }
    P("\tfma.rn%s.%s %s, %s, %s, %s;\n", FTZ(ty), fty(ty), reg(v), x, y, z);
}

/* (float)((double)x + (double)y) is x + y in single precision: the double sum of two floats, rounded once
   more, is their float sum (and so for differences and products). LLVM makes that of it before it fuses. */
static void narrow_sums(void)
{
    u32 v;
    int any = 0;
    for (v = 1; v < PF->nins; v++) {
        Ins *i = &PF->ins[v], *d, *x, *y;
        if (i->op != OP_FTRUNC || !i->blk)
            continue;
        d = &PF->ins[RV(i->a)];
        if ((d->op != OP_FADD && d->op != OP_FSUB && d->op != OP_FMUL) || !d->blk)
            continue;
        x = &PF->ins[RV(d->a)];
        y = &PF->ins[RV(d->b)];
        if (x->op != OP_FEXT || y->op != OP_FEXT)
            continue;
        i->op = d->op;
        i->a = x->a;
        i->b = y->a;
        any = 1;
    }
    if (any)
        opt_dce(PF); /* the double sum is gone: its operands have one reader less */
}

/* A value that a loop carries round (a counter, a running sum) is a phi P of the loop's first block and the
   value X that is computed from it, which P takes on the way back. Where P is read for the last time no later
   than X is computed, the two share a register and the way back needs no move. Asked for here: X is in P's
   block, made by one instruction that writes its result last, and every reader of P is an instruction of
   that block at or before X that reads its operands where it stands (a comparison is done over again where
   its result is wanted, and a fused product where its sum is). */
static void coalesce(const u8 *live)
{
    Func *f = PF;
    u32 nb = f->nblk, b, v, k, n, *p, at = 0;
    u32 *pos = (u32 *)xmalloc(2 * (size_t)(f->nins + 1) * sizeof(u32)), *last = pos + f->nins + 1;
    memset(pos, 0, 2 * (size_t)(f->nins + 1) * sizeof(u32));
#define NOSHARE 0xffffffffU
#define READ_BY(x, where) \
    do { \
        u32 x_ = RV(x); \
        if (x_ && f->ins[x_].op == OP_PHI && last[x_] < (where)) \
            last[x_] = (where); \
    } while (0)
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                READ_BY(f->xargs[f->ins[v].a + k], NOSHARE);
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 where = is_compare(i->op) || ptree[v] || (xrem && xrem[v]) || fused[v] || i->op == OP_SELECT || (sh_s && sh_t32[v] && sh_ext(v)) || xlate[v] ? NOSHARE : ++at;
            pos[v] = ++at;
            if (special_load(v))
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]);
                READ_BY(x, x && f->ins[x].blk == b ? where : NOSHARE);
            }
            if (i->op == OP_CALL)
                READ_BY(i->c, NOSHARE);
        }
        if (bb->arg)
            READ_BY(bb->arg, NOSHARE);
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next) {
            if (last[v] == NOSHARE)
                continue;
            for (k = 0; k < f->ins[v].b; k++) {
                u32 x = RV(f->xargs[f->ins[v].a + k]);
                Ins *xi = &f->ins[x];
                int ok = 0;
                if (!x || !live[bb->preds[k]] || xi->blk != b || !pos[x] || regno[x] || !used[x] || pos[x] < last[v] ||
                    cls_of(xi->ty) != cls_of(f->ins[v].ty))
                    continue;
                switch (xi->op) {
                case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
                case OP_FADD: case OP_FSUB: case OP_FDIV:
                    ok = 1;
                    break;
                case OP_FMUL:
                    ok = !fused[x];
                    break;
                default:
                    break;
                }
                if (!ok)
                    continue;
                (void)reg(v);
                regno[x] = regno[v];
                break;
            }
        }
    }
#undef READ_BY
#undef NOSHARE
    free(pos);
}

/* An assembly statement's operand that is read and written ("+f": a tensor product's running sums) is
   given its value by a move before the statement. Where the value read is itself such a result or a phi, of
   the statement's own block, and nothing else reads it, the result takes its register and there is no move;
   a sum that a loop carries round then stays in one register all the way. */
static void coalesce_asm(const u8 *live)
{
    Func *f = PF;
    u32 nb = f->nblk, b, v, k, n, *p;
    u8 *cnt;
    if (opt_noasmco)
        return;
    cnt = (u8 *)xmalloc(f->nins + 1);
    memset(cnt, 0, f->nins + 1);
#define COUNT(x) \
    do { \
        u32 x_ = RV(x); \
        if (x_ && cnt[x_] < 2) \
            cnt[x_]++; \
    } while (0)
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                if (RV(f->xargs[f->ins[v].a + k]) != v)
                    COUNT(f->xargs[f->ins[v].a + k]);
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                COUNT(p[k]);
            if (i->op == OP_CALL)
                COUNT(i->c);
        }
        if (bb->arg)
            COUNT(bb->arg);
    }
#undef COUNT
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v], *ai;
            AsmStmt *st;
            u64 idx;
            u32 a = 0, x;
            if (i->op != OP_ASMOUT || regno[v] || !used[v])
                continue;
            ai = &f->ins[i->a];
            if (ai->op != OP_CALL || !(ai->flags & IF_ASM) || ai->blk != b || !ir_is_const(f, ai->c, &idx))
                continue;
            st = &asm_stmts[idx];
            if (i->b >= st->nops || st->ops[i->b].out != 2 || st->ops[i->b].cls == 'h')
                continue;
            for (k = 0; k < i->b; k++)
                if (st->ops[k].kind != 'i' && (!st->ops[k].out || st->ops[k].out == 2))
                    a++;
            x = RV(f->xargs[ai->a + a]);
            if (!x || cnt[x] != 1 || f->ins[x].blk != b || (f->ins[x].op != OP_PHI && f->ins[x].op != OP_ASMOUT) ||
                cls_of(f->ins[x].ty) != cls_of(i->ty) || !used[x])
                continue;
            (void)reg(x);
            regno[v] = regno[x];
        }
    }
    free(cnt);
}

/* The name of the function that the call v calls, if it is a plain call of a named function. */
static const char *callee_name(u32 v)
{
    Ins *i = &PF->ins[v], *c;
    if (i->op != OP_CALL || !i->blk || (i->flags & (IF_ASM | IF_ALLOCA)) || i->b != 1)
        return 0;
    c = &PF->ins[RV(i->c)];
    if (c->op != OP_ADDR || c->b || !PO->syms[c->a].name)
        return 0;
    return intern_str(PO->syms[c->a].name);
}

/* A float that was rounded to a 16-bit float and is then stored as one, or rounded again, is rounded once:
   the conversion for the store does it (the front end rounds where the type changes and converts where
   the value is stored). */
static void round_once(void)
{
    Func *f = PF;
    u32 v;
    int any = 0;
    for (v = 1; v < f->nins; v++) {
        const char *n = callee_name(v), *m;
        u32 x;
        if (!n || strncmp(n, "__mcc_f", 7))
            continue;
        x = RV(f->xargs[f->ins[v].a]);
        m = callee_name(x);
        if (!m)
            continue;
        if (((!strcmp(n, "__mcc_f2bf") || !strcmp(n, "__mcc_froundbf")) && !strcmp(m, "__mcc_froundbf")) ||
            ((!strcmp(n, "__mcc_f2h") || !strcmp(n, "__mcc_fround16")) && !strcmp(m, "__mcc_fround16"))) {
            f->xargs[f->ins[v].a] = f->xargs[f->ins[x].a];
            any = 1;
        }
    }
    if (any)
        opt_dce(f);
}

/* The way back of a loop, from block b to its head s, with moves for s's phis, the other way out t without:
   whether the moves can be made before the branch, on both ways (the way back is then one conditional
   branch, as nvcc writes it; with the moves in a block of their own after the branch out, ptxas peels a
   copy of the loop: conv_bwd_w_s2_f8_k, 48 MMAs for 24). They can when no register they write is read on
   the way out: not by anything that t reaches before s comes again, nor by what is computed again where
   it is read (a comparison, a fused product, ...), nor through a value that shares the register. */
static int moves_early(const u8 *live, u32 b, u32 s, u32 t)
{
    Func *f = PF;
    Block *sb = &f->blk[s];
    u32 k, v, n, *p, pi = 0, nb = f->nblk, *stack, sp = 0, w, x;
    u8 *hit, *reach;
    int ok = 1, more;
    if (s > b || t == s || !sb->phis)
        return 0;
    for (k = 0; k < sb->npreds; k++)
        if (sb->preds[k] == b)
            pi = k;
    hit = (u8 *)xmalloc(f->nins + 1);
    memset(hit, 0, f->nins + 1);
    /* the registers the moves write */
    for (v = sb->phis; v; v = f->ins[v].next) {
        u32 src = RV(f->xargs[f->ins[v].a + pi]);
        if (!used[v] || src == v || !src)
            continue;
        if (regno[src] && regno[src] == regno[v] && f->ins[src].op != OP_CONST && cls_of(f->ins[src].ty) == cls_of(f->ins[v].ty))
            continue;
        hit[v] = 1;
    }
    for (w = 1; w < f->nins; w++)
        if (f->ins[w].blk && regno[w] && !hit[w])
            for (v = sb->phis; v; v = f->ins[v].next)
                if (hit[v] && regno[w] == regno[v] && cls_of(f->ins[w].ty) == cls_of(f->ins[v].ty)) {
                    hit[w] = 1;
                    break;
                }
    /* what is computed again where it is read reads them there */
    do {
        more = 0;
        for (w = 1; w < f->nins; w++) {
            Ins *i = &f->ins[w];
            if (hit[w] || !i->blk || i->op == OP_PHI ||
                !(is_compare(i->op) || fused[w] || i->op == OP_SELECT || (sh_s && sh_t32[w] && sh_ext(w)) || xlate[w]))
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                if (hit[RV(p[k])]) {
                    hit[w] = 1;
                    more = 1;
                    break;
                }
        }
    } while (more);
    /* the blocks t reaches before s */
    reach = (u8 *)xmalloc(nb);
    memset(reach, 0, nb);
    stack = (u32 *)xmalloc(nb * sizeof(u32));
    reach[t] = 1;
    stack[sp++] = t;
    while (sp) {
        Block *bb = &f->blk[stack[--sp]];
        n = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        for (k = 0; k < n; k++) {
            x = bb->succ[k];
            if (x != s && live[x] && !reach[x]) {
                reach[x] = 1;
                stack[sp++] = x;
            }
        }
    }
    for (x = 1; x < nb && ok; x++) {
        Block *bb = &f->blk[x];
        if (!live[x])
            continue;
        for (v = bb->phis; v && ok; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++) {
                u32 from = bb->preds[k];
                if (hit[RV(f->xargs[f->ins[v].a + k])] && (reach[from] || (from == b && x != s))) {
                    ok = 0;
                    break;
                }
            }
        if (!reach[x])
            continue;
        for (v = bb->first; v && ok; v = f->ins[v].next) {
            IR_OPS(f, &f->ins[v], p, n);
            for (k = 0; k < n; k++)
                if (hit[RV(p[k])])
                    ok = 0;
            if (f->ins[v].op == OP_CALL && hit[RV(f->ins[v].c)])
                ok = 0;
        }
        if (bb->arg && hit[RV(bb->arg)])
            ok = 0;
    }
    free(hit);
    free(reach);
    free(stack);
    return ok;
}

/* The place among the PTX parameters of the function's parameter k. */
static u32 param_index(Func *f, u32 k)
{
    u32 j, n = 0;
    for (j = 0; j < k; j++)
        n += !(f->param_ty[j] & 0x40);
    return n;
}

/* Loads the kernel's parameter v into the register given. */
static const char *param_load(u32 v, const char *into)
{
    Func *f = PF;
    u32 pk = f->ins[v].a;
    if (f->param_ty[pk] & 0xc0) {
        /* a doubleword of a parameter that is a block of memory */
        u32 h = pk;
        while (f->param_ty[h] & 0x40)
            h--;
        P("\tld.param.u64 %s, [%s_param_%u+%u];\n", into, kern_name, param_index(f, h), 8 * (pk - h));
    } else {
        P("\tld.param.%s %s, [%s_param_%u];\n", f->ins[v].ty == IT_I32 ? "u32" : f->ins[v].ty == IT_I64 ? "u64" : fty(f->ins[v].ty), into,
          kern_name, param_index(f, pk));
    }
    return into;
}

static const char *param_const(u32 v)
{
    return param_load(v, temp(cls_of(PF->ins[v].ty)));
}

static void proto(Buf *b, PSig *s, const char *name, int with_names)
{
    u32 k;
    (void)with_names;
    if (s->entry) {
        BP(b, ".visible .entry %s(", name);
    } else {
        BP(b, ".func ");
        if (s->ret != IT_VOID)
            BP(b, "(.param %s func_retval0) ", pty_name(s->ret, 0));
        BP(b, "%s (", name);
    }
    for (k = 0; k < s->np; k++)
        if (s->entry && s->pblock[k]) /* a kernel is given the block itself: memory of the host is not the device's */
            BP(b, "%s.param .align %u .b8 %s_param_%u[%u]", k ? ", " : "", s->palign[k], name, k, s->pblock[k]);
        else
            BP(b, "%s.param %s %s_param_%u", k ? ", " : "", pty_name(s->pty[k], s->entry), name, k);
    BP(b, ")");
}

/* Writes function f, a kernel (entry) or a device function. */
/* The thread's and the block's numbers (threadIdx.x and the like) are read once, in the first block: reading
   one is a slow instruction, a register holds it for nothing, and a load where the source reads it is done
   again after every store and in every trip around a loop. Only with MCC_SPECIAL1: it measured the same. */
static void special_once(Func *f)
{
    char names[24][48]; /* (copies: the name given is held only while an instruction is written) */
    u32 first[24], n = 0, v, k, n0 = f->nins, any = 0;
    if (!f->blk[1].first)
        return;
    for (v = 1; v < n0; v++) {
        const char *nm;
        if (f->ins[v].op != OP_LOAD || !f->ins[v].blk || (nm = special_load(v)) == 0)
            continue;
        for (k = 0; k < n && strcmp(names[k], nm); k++)
            ;
        if (k == n) {
            u32 l;
            if (n == 24)
                continue;
            l = ir_raw(f, OP_LOAD, f->ins[v].ty, f->ins[v].a, 0);
            f->ins[l].mem = f->ins[v].mem;
            ir_insert_before(f, f->blk[1].first, l);
            if (strlen(nm) >= sizeof names[0])
                continue;
            strcpy(names[n], nm);
            first[n++] = l;
        }
        ir_replace(f, v, first[k]);
        any = 1;
    }
    if (any) {
        f->copies = 1;
        ir_resolve_all(f);
    }
}

/* ---- the ranges of 32-bit values, for the sign corrections of divisions by a constant ----
   ir.c divides by a constant as it builds the code, before anything is known of the values: a signed quotient
   gets x >> 31 added (one more when x is negative), a signed remainder by 2^k a bias from x >> 31. Most of what
   kernels divide is a loop counter, a thread number or a sum of them, never negative: nvcc writes none of those
   corrections in lp_s2 (mul.hi.u32), mcc wrote 1352. A value's range here comes from constants, the arithmetic
   on them, the special registers, and a loop counter's start and the comparison that keeps the loop going (no
   assumption that a sum does not wrap: a range that leaves 32 bits is no range). MCC_NORANGE: off. */
#define RG_MIN (-(i64)0x80000000L)
#define RG_MAX ((i64)0x7fffffffL)
static i64 *rg_lo, *rg_hi;
static u8 *rg_st; /* 0 not yet, 1 being worked out (a cycle: no range), 2 known */
static u32 rg_depth;

static void rg_of(u32 v, i64 *lo, i64 *hi);

/* floor(x / 2^k), for k < 63, without shifting a negative number */
static i64 rg_fl(i64 x, u32 k)
{
    i64 d = (i64)1 << k;
    return x >= 0 ? x / d : -((-x + d - 1) / d);
}

static int rg_fit(i64 lo, i64 hi)
{
    return lo >= RG_MIN && hi <= RG_MAX && lo <= hi;
}

/* the most x can be when condition w is true (sense 1) or false (sense 0); RG_MAX when it says nothing */
static i64 rg_cond_ub(u32 w, int sense, u32 x)
{
    Ins *c = &PF->ins[RV(w)];
    u32 l, r;
    i64 nlo, nhi;
    int op;
    if (c->op < OP_SLT || c->op > OP_UGE)
        return RG_MAX;
    l = RV(c->a);
    r = RV(c->b);
    if (PF->ins[l].ty != IT_I32 || PF->ins[r].ty != IT_I32)
        return RG_MAX;
    op = c->op;
    if (r == x) { /* N op x: as x op' N */
        u32 t = l;
        l = r;
        r = t;
        op = op == OP_SLT ? OP_SGT : op == OP_SLE ? OP_SGE : op == OP_SGT ? OP_SLT : op == OP_SGE ? OP_SLE :
             op == OP_ULT ? OP_UGT : op == OP_ULE ? OP_UGE : op == OP_UGT ? OP_ULT : OP_ULE;
    }
    if (l != x)
        return RG_MAX;
    if (!sense) /* not (x op N) */
        op = op == OP_SLT ? OP_SGE : op == OP_SLE ? OP_SGT : op == OP_SGT ? OP_SLE : op == OP_SGE ? OP_SLT :
             op == OP_ULT ? OP_UGE : op == OP_ULE ? OP_UGT : op == OP_UGT ? OP_ULE : OP_ULT;
    rg_of(r, &nlo, &nhi);
    switch (op) {
    case OP_SLT: return nhi - 1;
    case OP_SLE: return nhi;
    case OP_ULT: return nlo >= 0 ? nhi - 1 : RG_MAX; /* (below a non-negative N unsigned: 0 .. N - 1) */
    case OP_ULE: return nlo >= 0 ? nhi : RG_MAX;
    }
    return RG_MAX;
}

/* phi p of block h: a loop counter (each way back brings p + c, c > 0) whose way back is taken only while a
   comparison of p or p + c with a bounded value holds */
static int rg_phi(u32 p, i64 *lo, i64 *hi)
{
    Func *f = PF;
    Ins *pi = &f->ins[p];
    Block *hb = &f->blk[pi->blk];
    i64 l = RG_MAX, h = RG_MIN, a, b;
    u32 k, j, nb = f->nblk;
    int ivs = 0;
    for (k = 0; k < pi->b; k++) {
        u32 x = RV(f->xargs[pi->a + k]), pr = hb->preds[k];
        Ins *xi = &f->ins[x];
        u64 c;
        if (x == p)
            continue;
        if (xi->op == OP_ADD && RV(xi->a) == p && ir_is_const(f, RV(xi->b), &c) && (i64)(i32)(u32)c > 0) {
            /* the way back: what keeps it going bounds p + c */
            i64 cc = (i64)(i32)(u32)c, ub = RG_MAX, t;
            Block *pb = &f->blk[pr];
            if (pb->term == TERM_BR && (pb->succ[0] == pi->blk) != (pb->succ[1] == pi->blk)) {
                int sense = pb->succ[0] == pi->blk;
                t = rg_cond_ub(pb->arg, sense, x);
                ub = t < ub ? t : ub;
                t = rg_cond_ub(pb->arg, sense, p);
                ub = t < RG_MAX && t + cc < ub ? t + cc : ub;
            }
            for (j = 1; j < nb && ub == RG_MAX; j++) {
                Block *bb = &f->blk[j];
                int s;
                if (bb->term != TERM_BR || bb->succ[0] == bb->succ[1])
                    continue;
                for (s = 0; s < 2; s++) {
                    u32 sb = bb->succ[s];
                    if (f->blk[sb].npreds != 1 || sb == pi->blk || !opt_dominates(sb, pr))
                        continue;
                    t = rg_cond_ub(bb->arg, s == 0, x);
                    ub = t < ub ? t : ub;
                    t = rg_cond_ub(bb->arg, s == 0, p);
                    ub = t < RG_MAX && t + cc < ub ? t + cc : ub;
                }
            }
            if (ub >= RG_MAX)
                return 0;
            h = ub > h ? ub : h;
            if (h + cc > RG_MAX)
                return 0; /* (p + c could wrap: then the comparison bounds nothing) */
            ivs = 1;
            continue;
        }
        rg_of(x, &a, &b);
        l = a < l ? a : l;
        h = b > h ? b : h;
    }
    if (!ivs || l > h)
        return 0;
    for (k = 0; k < pi->b; k++) { /* (every step again, against the final h) */
        Ins *xi = &f->ins[RV(f->xargs[pi->a + k])];
        u64 c;
        if (xi->op == OP_ADD && RV(xi->a) == p && ir_is_const(f, RV(xi->b), &c) && h + (i64)(i32)(u32)c > RG_MAX)
            return 0;
    }
    *lo = l;
    *hi = h;
    return 1;
}

static void rg_of(u32 v, i64 *lo, i64 *hi)
{
    Func *f = PF;
    Ins *i;
    i64 a0, a1, b0, b1, l = RG_MIN, h = RG_MAX;
    u64 c;
    int sp;
    v = RV(v);
    i = &f->ins[v];
    *lo = RG_MIN;
    *hi = RG_MAX;
    if (i->op == OP_CONST) {
        *lo = *hi = (i64)(i32)(u32)i->a;
        return;
    }
    if (i->ty != IT_I32 || rg_st[v] == 1 || rg_depth > 200)
        return;
    if (rg_st[v] == 2) {
        *lo = rg_lo[v];
        *hi = rg_hi[v];
        return;
    }
    rg_st[v] = 1;
    rg_depth++;
    switch (i->op) {
    case OP_ADD: case OP_SUB: case OP_MUL:
        rg_of(i->a, &a0, &a1);
        rg_of(i->b, &b0, &b1);
        { /* (i64 holds any of these of two i32 bounds; rg_fit below drops what leaves 32 bits) */
            if (i->op == OP_ADD)
                l = a0 + b0, h = a1 + b1;
            else if (i->op == OP_SUB)
                l = a0 - b1, h = a1 - b0;
            else {
                i64 p[4];
                int k;
                p[0] = a0 * b0, p[1] = a0 * b1, p[2] = a1 * b0, p[3] = a1 * b1;
                l = h = p[0];
                for (k = 1; k < 4; k++)
                    l = p[k] < l ? p[k] : l, h = p[k] > h ? p[k] : h;
            }
        }
        break;
    case OP_SHL:
        rg_of(i->a, &a0, &a1);
        if (ir_is_const(f, RV(i->b), &c) && c < 31)
            l = a0 * ((i64)1 << c), h = a1 * ((i64)1 << c);
        break;
    case OP_SHR: case OP_SAR:
        if (!ir_is_const(f, RV(i->b), &c) || (c &= 31) == 0)
            break;
        rg_of(i->a, &a0, &a1);
        if (i->op == OP_SAR || a0 >= 0)
            l = rg_fl(a0, (u32)c), h = rg_fl(a1, (u32)c);
        else
            l = 0, h = (i64)(0xffffffffUL >> c);
        break;
    case OP_AND:
        rg_of(i->a, &a0, &a1);
        rg_of(i->b, &b0, &b1);
        if (a0 >= 0 || b0 >= 0)
            l = 0, h = a0 >= 0 && b0 >= 0 ? (a1 < b1 ? a1 : b1) : a0 >= 0 ? a1 : b1;
        break;
    case OP_OR: case OP_XOR:
        rg_of(i->a, &a0, &a1);
        rg_of(i->b, &b0, &b1);
        if (a0 >= 0 && b0 >= 0) {
            i64 m = a1 > b1 ? a1 : b1, p = 1;
            while (p <= m)
                p <<= 1;
            l = 0, h = p - 1;
        }
        break;
    case OP_SMULH: case OP_UMULH:
        rg_of(i->a, &a0, &a1);
        if (ir_is_const(f, RV(i->b), &c)) {
            i64 k = i->op == OP_SMULH ? (i64)(i32)(u32)c : (i64)(u32)c;
            if (i->op == OP_UMULH && a0 < 0)
                break;
            if (a0 == RG_MIN || a1 == RG_MAX)
                break;
            l = rg_fl(a0 * k, 32), h = rg_fl(a1 * k, 32);
            if (l > h) {
                i64 t = l;
                l = h, h = t;
            }
        }
        break;
    case OP_SELECT:
        rg_of(i->b, &a0, &a1);
        rg_of(i->c, &b0, &b1);
        l = a0 < b0 ? a0 : b0, h = a1 > b1 ? a1 : b1;
        break;
    case OP_EQ: case OP_NE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE: case OP_ULT: case OP_ULE: case OP_UGT:
    case OP_UGE: case OP_FEQ: case OP_FNE: case OP_FLT: case OP_FLE: case OP_FGT: case OP_FGE:
        l = 0, h = 1;
        break;
    case OP_ZEXT8: l = 0, h = 255; break;
    case OP_ZEXT16: l = 0, h = 65535; break;
    case OP_SEXT8: l = -128, h = 127; break;
    case OP_SEXT16: l = -32768, h = 32767; break;
    case OP_CLZ: case OP_CTZ: case OP_POPCNT: l = 0, h = 32; break;
    case OP_LOAD:
        if (i->mem == M_8U)
            l = 0, h = 255;
        else if (i->mem == M_16U)
            l = 0, h = 65535;
        else if (i->mem == M_32 && f->ins[RV(i->a)].op == OP_ADDR && (sp = special_of(f->ins[RV(i->a)].a)) >= 0)
            l = sp & 1 ? 1 : 0, h = sp == 0 ? 1023 : sp == 2 ? 1024 : RG_MAX; /* tid, ctaid, ntid, nctaid */
        break;
    case OP_PHI:
        if (!rg_phi(v, &l, &h)) {
            l = RG_MIN;
            h = RG_MAX;
        }
        break;
    }
    if (!rg_fit(l, h))
        l = RG_MIN, h = RG_MAX;
    rg_depth--;
    rg_lo[v] = l;
    rg_hi[v] = h;
    rg_st[v] = 2;
    *lo = l;
    *hi = h;
}

/* x >> 31 (logical or arithmetic) of an x known not to be negative is 0, and so is what it is the bias of */
static void range_fold(Func *f)
{
    u32 v, n = f->nins, any = 0;
    u8 *zero;
    i64 lo, hi;
    if (opt_norange)
        return;
    rg_lo = (i64 *)xmalloc((size_t)(n + 1) * sizeof(i64));
    rg_hi = (i64 *)xmalloc((size_t)(n + 1) * sizeof(i64));
    rg_st = (u8 *)xmalloc((size_t)n + 1);
    zero = (u8 *)xmalloc((size_t)n + 1);
    memset(rg_st, 0, (size_t)n + 1);
    memset(zero, 0, (size_t)n + 1);
    rg_depth = 0;
    opt_dom(f);
    for (v = 1; v < n; v++) {
        Ins *i = &f->ins[v];
        u64 c;
        if (!i->blk || i->ty != IT_I32 || (i->op != OP_SHR && i->op != OP_SAR) || !ir_is_const(f, RV(i->b), &c) || (c & 31) != 31)
            continue;
        rg_of(i->a, &lo, &hi);
        if (lo >= 0) {
            zero[v] = 1;
            any = 1;
        }
    }
    if (any) {
        /* (x >> 31) >> k, x + (x >> 31): what reads them, in the order made */
        for (v = 1; v < n; v++) {
            Ins *i = &f->ins[v];
            u32 a, b;
            if (!i->blk || i->ty != IT_I32 || zero[v])
                continue;
            a = RV(i->a);
            b = RV(i->b);
            if ((i->op == OP_SHR || i->op == OP_SAR || i->op == OP_SHL || i->op == OP_AND) && a < n && zero[a])
                zero[v] = 1;
            else if ((i->op == OP_ADD || i->op == OP_OR || i->op == OP_XOR) && b < n && zero[b])
                ir_replace(f, v, a);
            else if ((i->op == OP_ADD || i->op == OP_OR || i->op == OP_XOR) && a < n && zero[a])
                ir_replace(f, v, b);
        }
        for (v = 1; v < n; v++)
            if (zero[v])
                ir_replace(f, v, ir_const(f, IT_I32, 0));
        f->copies = 1;
        ir_resolve_all(f);
        opt_dce(f);
    }
    free(rg_lo);
    free(rg_hi);
    free(rg_st);
    free(zero);
}

void ptx_gen(Func *f, Obj *o, int entry)
{
    u32 b, v, k, n, *p, frame = 0, nb = f->nblk, last_loc = 0;
    int kc;
    u8 *live;
    u32 *stack, sp = 0;
    PSig *s;
    Buf head;
    const char *name;

    PO = o;
    PF = f;
    name = pname(f->sym);
    fname = name;
    if (entry && opt_nospecial1)
        special_once(f);
    range_fold(f);
    if (entry) {
        if (nkern == capkern) {
            capkern = capkern ? capkern * 2 : 32;
            kernsyms = (u32 *)xrealloc(kernsyms, capkern * sizeof(u32));
        }
        kernsyms[nkern++] = f->sym;
    }
    fb.len = 0;
    regno = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
    memset(regno, 0, (f->nins + 1) * sizeof(u32));
    used = (u8 *)xmalloc(f->nins + 1);
    memset(used, 0, f->nins + 1);
    nuse = (u8 *)xmalloc(7 * (size_t)(f->nins + 1));
    memset(nuse, 0, 7 * (size_t)(f->nins + 1));
    fuse = nuse + f->nins + 1;
    fused = fuse + f->nins + 1;
    cuse = fused + f->nins + 1;
    ause = cuse + f->nins + 1;
    xlate = ause + f->nins + 1;
    ptree = xlate + f->nins + 1;
    slotoff = (u32 *)xmalloc((f->nslots + 1) * sizeof(u32));
    memset(nreg, 0, sizeof nreg);
    sh_s = 0;
    if (nshared && !opt_nosh32) {
        sh_s = (u8 *)xmalloc(3 * (size_t)(f->nins + 1));
        memset(sh_s, 0, 3 * (size_t)(f->nins + 1));
        sh_t32 = sh_s + f->nins + 1;
        sh_g64 = sh_t32 + f->nins + 1;
        sh_reg = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
        memset(sh_reg, 0, (f->nins + 1) * sizeof(u32));
    }
    if (f->variadic)
        die("ptx: %s: variadic functions are not supported in device code", name);

    s = sig_find(f->sym);
    if (!s)
        s = sig_add(f->sym, f->ret_ty, f->nparams, f->param_ty);
    if (s->np != param_index(f, f->nparams) || s->ret != f->ret_ty)
        die("ptx: %s was called with other argument types than it is defined with", name);
    s->defined = 1;
    s->entry = (u8)entry;

    /* the blocks that can be reached */
    live = (u8 *)xmalloc(nb + 1);
    memset(live, 0, nb + 1);
    stack = (u32 *)xmalloc((nb + 1) * sizeof(u32));
    stack[sp++] = 1;
    live[1] = 1;
    while (sp) {
        Block *bb = &f->blk[stack[--sp]];
        n = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        if (n == 2 && (kc = known_cond(bb->arg)) >= 0) {
            /* only one way is ever taken: the block ends in a jump */
            bb->term = TERM_JMP;
            bb->succ[0] = bb->succ[kc ? 0 : 1];
            bb->arg = 0;
            n = 1;
        }
        for (k = 0; k < n; k++)
            if (!live[bb->succ[k]]) {
                live[bb->succ[k]] = 1;
                stack[sp++] = bb->succ[k];
            }
    }
    narrow_sums();
    if (!opt_noifcall)
        round_once();
    /* the values that are read */
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                if (live[bb->preds[k]])
                    mark(f->xargs[f->ins[v].a + k]);
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            if (special_load(v))
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                mark(p[k]);
            if (i->op == OP_SELECT)
                mark_cond(i->a);
            if (i->op == OP_LOAD || i->op == OP_STORE) {
                i64 off;
                u32 x = RV(i->a);
                if (addr_sum(x, &off) && ause[x] < 255)
                    ause[x]++;
            }
            if (i->op == OP_CALL && !(i->flags & (IF_ASM | IF_ALLOCA)) && f->ins[RV(i->c)].op != OP_ADDR)
                mark(i->c);
        }
        if (bb->arg)
            mark(bb->arg);
        if (bb->term == TERM_BR)
            mark_cond(bb->arg);
    }
    xrem = 0;
    if (!opt_noxrem) {
        xrem = (u8 *)xmalloc((size_t)f->nins + 1);
        memset(xrem, 0, (size_t)f->nins + 1);
        for (b = 1; b < nb; b++) {
            Block *bb = &f->blk[b];
            if (!live[b])
                continue;
#define XREM(x) \
    do { \
        u32 x_ = RV(x); \
        Ins *xi_ = &f->ins[x_]; \
        if (x_ && (xi_->op == OP_SEXT32 || xi_->op == OP_ZEXT32) && xi_->blk && xi_->blk != b && used[x_] && !xlate[x_] && \
            !(sh_s && sh_t32[x_]) && f->ins[RV(xi_->a)].op != OP_CONST) \
            xrem[x_] = 1; \
    } while (0)
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                IR_OPS(f, i, p, n);
                for (k = 0; k < n; k++)
                    XREM(p[k]);
            }
            if (bb->arg)
                XREM(bb->arg);
#undef XREM
        }
    }
    /* the trees of truth values; a tree that is only read as a condition, in one or two places, is done
       over again there and has no register (pass one: readers before what they read) */
    for (v = f->nins - 1; v >= 1 && !opt_nopred; v--) {
        Ins *i = &f->ins[v];
        if (!used[v] || !i->blk || !live[i->blk] || (i->op != OP_AND && i->op != OP_OR) || !pred_bool(v, 8))
            continue;
        ptree[v] = nuse[v] < 255 && nuse[v] == cuse[v] && cuse[v] <= 2 ? 2 : 1;
        mark_cond(i->a);
        mark_cond(i->b);
    }
    /* one that has a register and reads nothing but comparisons and numbers that have registers is as well
       done on the numbers */
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        u32 a, b;
        if (ptree[v] != 1)
            continue;
        a = RV(i->a);
        b = RV(i->b);
        if (ptree[a] == 2 || ptree[b] == 2 || (is_compare(f->ins[a].op) && is_compare(f->ins[b].op)))
            continue;
        ptree[v] = 0;
        if (cuse[a] && cuse[a] < 255)
            cuse[a]--;
        if (cuse[b] && cuse[b] < 255)
            cuse[b]--;
    }
    for (v = 1; v < f->nins; v++)
        if (f->ins[v].op == OP_PHI && f->ins[v].blk && live[f->ins[v].blk])
            used[v] = 1; /* kept simple: every phi of a live block is given its moves */

    if (opt_fmad) {
        frank = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
        memset(frank, 0, (f->nins + 1) * sizeof(u32));
        addu = (u8 *)xmalloc(f->nins + 1);
        memset(addu, 0, f->nins + 1);
        rank_all(live);
        count_sums();
        fuse_find();
        free(frank);
        free(addu);
    }
    if (sh_s)
        sh_find(live);
    lc_s = 0;
    if (!opt_nolocal) {
        lc_s = (u8 *)xmalloc((size_t)f->nins + 1);
        lc_find(f);
    }
    gl_s = 0;
    if (entry && !opt_noglobal) {
        gl_s = (u8 *)xmalloc((size_t)f->nins + 1);
        gl_find(f);
    }
    for (v = 1; v < f->nins; v++) {
        u32 x, y;
        if ((f->ins[v].op == OP_MUL || f->ins[v].op == OP_SHL) && f->ins[v].blk && wide_kind(v, &x, &y)) {
            xlate[RV(f->ins[v].a)] = 1;
            if (y)
                xlate[RV(f->ins[v].b)] = 1;
        }
    }
    coalesce(live);
    coalesce_asm(live);

    /* the frame */
    for (k = 0; k < f->nslots; k++)
        slotoff[k] = 0xffffffffU;
    for (v = 1; v < f->nins; v++)
        if (used[v] && f->ins[v].op == OP_SLOT && slotoff[f->ins[v].a] == 0xffffffffU) {
            Slot *sl = &f->slots[f->ins[v].a];
            u32 al = sl->align ? sl->align : 1;
            frame = (frame + al - 1) & ~(al - 1);
            slotoff[f->ins[v].a] = frame;
            frame += sl->size ? sl->size : 1;
        }
    /* x86-64: an argument passed as a block of memory is a copy of it in this frame, and its address */
    byoff = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
    for (v = 1; v < f->nins; v++)
        if (used[v] && f->ins[v].op == OP_BYVAL) {
            u32 al = f->ins[v].c > 8 ? f->ins[v].c : 8;
            frame = (frame + al - 1) & ~(al - 1);
            byoff[v] = frame;
            frame += f->ins[v].b ? f->ins[v].b : 1;
        }

    for (k = 0; k < 4; k++)
        special_off[k] = 0xffffffffU;
    for (v = 1; v < f->nins; v++)
        if (used[v] && f->ins[v].op == OP_ADDR) {
            int sp = special_of(f->ins[v].a);
            if (sp >= 0 && special_off[sp] == 0xffffffffU) {
                frame = (frame + 3) & ~3U;
                special_off[sp] = frame;
                frame += 12;
            }
        }

    premat = entry && opt_premat;
    /* prologue: parameters, then the addresses the code uses */
    kern_name = name;
    for (v = 1; v < f->nins; v++)
        if (f->ins[v].op == OP_PARAM && used[v]) {
            if (entry && premat)
                continue;
            if (entry)
                param_load(v, reg(v));
            else
                P("\tld.param.%s %s, [%s_param_%u];\n", vty(f->ins[v].ty), reg(v), name, param_index(f, f->ins[v].a));
        }
    if (frame)
        P("\tmov.u64 %%SPL, __local_depot;\n\tcvta.local.u64 %%SP, %%SPL;\n");
    for (k = 0; k < 4; k++)
        if (special_off[k] != 0xffffffffU) {
            const char *tr = temp(0);
            P("\tmov.u32 %s, %s.x;\n\tst.u32 [%%SP+%u], %s;\n", tr, special_reg[k], special_off[k], tr);
            P("\tmov.u32 %s, %s.y;\n\tst.u32 [%%SP+%u], %s;\n", tr, special_reg[k], special_off[k] + 4, tr);
            P("\tmov.u32 %s, %s.z;\n\tst.u32 [%%SP+%u], %s;\n", tr, special_reg[k], special_off[k] + 8, tr);
        }
    /* (the addresses of locals and of global objects are worked out where they are read: addr_const) */
    sh_syms();
    remat = 1;

    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        u32 next;
        if (!live[b])
            continue;
        for (next = b + 1; next < nb && !live[next]; next++)
            ;
        P("L%u:\n", b);
        if (bb->nounroll)
            P("\t.pragma \"nounroll\";\n");
        for (v = bb->first; v; v = f->ins[v].next) {
            if (opt_debug && f->ins[v].loc && f->ins[v].loc != last_loc) {
                /* -g: the line of what follows, for the driver's compiler to put in its line table */
                u32 file, line, col;
                last_loc = f->ins[v].loc;
                dwarf_where(last_loc, &file, &line, &col);
                P("\t.loc %u %u %u\n", file, line, col);
            }
            emit_ins(v);
        }
        switch (bb->term) {
        case TERM_JMP:
            edge_moves(b, bb->succ[0], 0);
            if (bb->succ[0] != next)
                P("\tbra.uni L%u;\n", bb->succ[0]);
            break;
        case TERM_BR: {
            /* a block that the next one follows is fallen into, and an edge without moves is branched along
               directly */
            u32 s0 = bb->succ[0], s1 = bb->succ[1];
            int m0, m1;
            edge_dry = 1;
            edge_moves(b, s0, 0);
            m0 = edge_dry == 2;
            edge_dry = 1;
            edge_moves(b, s1, s0 == s1 ? 1 : 0);
            m1 = edge_dry == 2;
            edge_dry = 0;
            cond_pred(bb->arg);
            if (m0 && m1) {
                P("\t@%%p0 bra L%u_t;\n", b);
                edge_moves(b, s1, s0 == s1 ? 1 : 0);
                P("\tbra.uni L%u;\nL%u_t:\n", s1, b);
                edge_moves(b, s0, 0);
                if (s0 != next)
                    P("\tbra.uni L%u;\n", s0);
            } else if (m0 && !opt_nolatch && moves_early(live, b, s0, s1)) {
                edge_moves(b, s0, 0);
                P("\t@%%p0 bra L%u;\n", s0);
                if (s1 != next)
                    P("\tbra.uni L%u;\n", s1);
            } else if (m1 && !opt_nolatch && moves_early(live, b, s1, s0)) {
                edge_moves(b, s1, s0 == s1 ? 1 : 0);
                P("\t@!%%p0 bra L%u;\n", s1);
                if (s0 != next)
                    P("\tbra.uni L%u;\n", s0);
            } else if (m0) {
                P("\t@!%%p0 bra L%u;\n", s1);
                edge_moves(b, s0, 0);
                if (s0 != next)
                    P("\tbra.uni L%u;\n", s0);
            } else if (m1 || s0 != next) {
                P("\t@%%p0 bra L%u;\n", s0);
                edge_moves(b, s1, s0 == s1 ? 1 : 0);
                if (s1 != next)
                    P("\tbra.uni L%u;\n", s1);
            } else {
                P("\t@!%%p0 bra L%u;\n", s1);
            }
            break;
        }
        case TERM_RET:
            if (bb->arg && f->ret_ty != IT_VOID)
                P("\tst.param.%s [func_retval0+0], %s;\n", vty(f->ret_ty), ropnd(bb->arg));
            P("\tret;\n");
            break;
        default:
            P("\ttrap;\n");
            break;
        }
    }
    remat = 0;

    memset(&head, 0, sizeof head);
    s = sig_find(f->sym); /* the table may have moved since: the body's calls add to it */
    proto(&head, s, name, 1);
    if (entry && (ptx_maxntid & 0xffff))
        BP(&head, "\n.maxntid %u, 1, 1", ptx_maxntid & 0xffff);
    if (entry && ptx_maxntid >> 16)
        BP(&head, "\n.minnctapersm %u", ptx_maxntid >> 16);
    BP(&head, "\n{\n");
    for (k = 0; k < 4; k++)
        if (nreg[k])
            BP(&head, "\t.reg %s %s<%u>;\n", cls_reg[k], cls_stem[k], nreg[k]);
    BP(&head, "\t.reg .pred %%p<16>;\n\t.reg .b16 %%h<12>;\n");
    if (frame)
        BP(&head, "\t.reg .b64 %%SP;\n\t.reg .b64 %%SPL;\n\t.local .align 16 .b8 __local_depot[%u];\n", frame);
    buf_put(&body, head.p, head.len);
    buf_put(&body, fb.p, fb.len);
    BP(&body, "}\n\n");
    buf_free(&head);
    free(regno);
    free(used);
    free(nuse);
    if (sh_s) {
        free(sh_s);
        free(sh_reg);
        sh_s = 0;
    }
    free(gl_s);
    free(xrem);
    xrem = 0;
    free(lc_s);
    lc_s = 0;
    gl_s = 0;
    free(slotoff);
    free(byoff);
    free(live);
    free(stack);
}

/* Puts bytes into a section and returns a new symbol for them. */
static u32 put_data(Obj *o, int sec, const void *p, size_t n, u32 align)
{
    u64 off = obj_reserve(o, sec, n, align);
    u32 sym;
    buf_put(&o->sec[sec], p, n);
    sym = obj_add_sym(o, 0, sec, off, 0);
    o->syms[sym].kind = 2;
    o->syms[sym].size = n;
    return sym;
}

/* Finishes the module: the text, the table of host objects, and the record that names both. */
void ptx_finish(Obj *o)
{
    Buf t, names, tab;
    u32 k, textsym, namesym, tabsym, modname;
    u64 off;
    const char *q, *e;

    if (!ptx_active())
        return;
    PO = o;
    memset(&t, 0, sizeof t);
    memset(&names, 0, sizeof names);
    memset(&tab, 0, sizeof tab);
    /* (the GPUs after sm_90 have instructions that only a later version of the language knows) */
    BP(&t, "//\n// Generated by mcc\n//\n\n.version %s\n.target %s\n.address_size 64\n\n",
       atoi(opt_cuda_target + 3) > 90 ? "9.1" : "8.5", opt_cuda_target);
    if (opt_debug) {
        /* the files the .loc lines of the bodies name */
        u32 nf = dwarf_nfiles();
        for (k = 1; k < nf; k++) {
            const char *c = dwarf_file_name(k);
            BP(&t, ".file %u \"", k);
            for (; *c; c++) {
                if (*c == '"' || *c == '\\')
                    buf_u8(&t, '\\');
                buf_u8(&t, (u8)*c);
            }
            BP(&t, "\"\n");
        }
        BP(&t, "\n");
    }
    for (k = 0; k < nshared; k++) {
        if (shared[k].ext)
            BP(&t, ".extern .shared .align %u .b8 %s[];\n", shared[k].align, pname(shared[k].sym));
        else
            BP(&t, ".shared .align %u .b8 %s[%lu];\n", shared[k].align, pname(shared[k].sym),
               (unsigned long)(shared[k].size ? shared[k].size : 1));
    }
    for (k = 0; k < nhost; k++) {
        /* with its size and alignment when the declaration gave them: a run time that makes the object on a
           device needs both */
        ObjSym *hs = &o->syms[hostsyms[k]];
        if (hs->dsize)
            BP(&t, ".extern .global .align %u .b8 %s[%lu];\n", 1U << hs->align, pname(hostsyms[k]), (unsigned long)hs->dsize);
        else
            BP(&t, ".extern .global .align 1 .b8 %s[];\n", pname(hostsyms[k]));
    }
    /* (declared once, as the C library has it: the code may call it by name too) */
    BP(&t, ".extern .func (.param .b64 func_retval0) __mcuda_memcpy (.param .b64 __mcuda_memcpy_param_0, "
           ".param .b64 __mcuda_memcpy_param_1, .param .b64 __mcuda_memcpy_param_2);\n");
    BP(&t, ".extern .func __mcuda_bzero (.param .b64 __mcuda_bzero_param_0, .param .b64 __mcuda_bzero_param_1);\n");
    for (k = 0; k < nsigs; k++) {
        PSig *s = &sigs[k];
        char nm[600];
        if (!s->defined && (strcmp(pname(s->sym), "memcpy") == 0 || strcmp(pname(s->sym), "bzero") == 0))
            continue;
        if (!s->defined)
            sprintf(nm, "__mcuda_%.500s", pname(s->sym));
        else
            sprintf(nm, "%.500s", pname(s->sym));
        if (!s->defined)
            BP(&t, ".extern ");
        proto(&t, s, nm, 0);
        BP(&t, ";\n");
    }
    BP(&t, "\n");
    /* the bodies, with the names of called functions filled in */
    q = (const char *)body.p;
    e = q + body.len;
    while (q < e) {
        if (q[0] == '$' && q + 1 < e && q[1] == 'F') {
            u32 sym = 0;
            PSig *s;
            q += 2;
            while (q < e && *q >= '0' && *q <= '9')
                sym = sym * 10 + (u32)(*q++ - '0');
            q++; /* the closing $ */
            s = sig_find(sym);
            BP(&t, "%s%s", s && s->defined ? "" : "__mcuda_", pname(sym));
        } else {
            buf_u8(&t, (u8)*q++);
        }
    }
    buf_u8(&t, 0);
    textsym = put_data(o, SEC_CONST, t.p, t.len, 1);

    /* names, then pairs of (name, address) ending with a null pair */
    for (k = 0; k < nhost + nkern; k++) {
        const char *n = pname(k < nhost ? hostsyms[k] : kernsyms[k - nhost]);
        buf_put(&names, n, strlen(n) + 1);
    }
    modname = (u32)names.len;
    buf_put(&names, o->name ? o->name : "", strlen(o->name ? o->name : "") + 1);
    namesym = put_data(o, SEC_CONST, names.p, names.len, 1);
    /* (after the objects, the kernels: each with the address its name has in host code, which is what
       cudaFuncSetAttribute and its like are given) */
    buf_zero(&tab, (size_t)(nhost + nkern + 1) * 16);
    tabsym = put_data(o, SEC_DATA, tab.p, tab.len, 8);
    off = 0;
    for (k = 0; k < nhost + nkern; k++) {
        u32 hsym = k < nhost ? hostsyms[k] : kernsyms[k - nhost];
        const char *n = pname(hsym);
        obj_add_reloc(o, SEC_DATA, (u32)(o->syms[tabsym].value + k * 16), R_ABS64, namesym, (i64)off);
        obj_add_reloc(o, SEC_DATA, (u32)(o->syms[tabsym].value + k * 16 + 8), R_ABS64, hsym, 0);
        off += strlen(n) + 1;
    }
    /* struct __mcuda_module { const char *ptx; void *handle; const char *name; const struct sym *syms; } */
    tab.len = 0;
    buf_zero(&tab, 32);
    off = obj_reserve(o, SEC_DATA, 32, 8);
    buf_put(&o->sec[SEC_DATA], tab.p, 32);
    ptx_module(o);
    o->syms[modsym].sec = SEC_DATA;
    o->syms[modsym].value = off;
    o->syms[modsym].kind = 2;
    o->syms[modsym].size = 32;
    obj_add_reloc(o, SEC_DATA, (u32)off, R_ABS64, textsym, 0);
    obj_add_reloc(o, SEC_DATA, (u32)off + 16, R_ABS64, namesym, (i64)modname);
    obj_add_reloc(o, SEC_DATA, (u32)off + 24, R_ABS64, tabsym, 0);
    if (opt_target == TARGET_LINUX) {
        /* the address of the record goes into a section of its own: the run time walks it (between the
           linker's __start_ and __stop_ symbols) to find kernels and device functions that one
           translation unit launches or calls and another defines */
        int ms = obj_section(o, intern_cstr("mcuda_modules"), SF_ALLOC | SF_WRITE);
        u64 at = obj_reserve(o, ms, 8, 8);
        buf_put(&o->sec[ms], tab.p, 8);
        obj_add_reloc(o, ms, (u32)at, R_ABS64, modsym, 0);
    }
    if (opt_ptx_out) {
        FILE *fp = fopen(opt_ptx_out, "w");
        if (!fp)
            die("cannot write %s", opt_ptx_out);
        fwrite(t.p, 1, t.len - 1, fp);
        fclose(fp);
    }
    buf_free(&t);
    buf_free(&names);
    buf_free(&tab);
}
