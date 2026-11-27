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
int opt_norange, opt_nofastexp, opt_noifasm, opt_shsym, opt_nocondrec, opt_nodivcse, opt_nohoist, opt_nofastcall, opt_nolocal, opt_nored, opt_nonc, opt_nolatch, opt_nounroll, opt_unrollpeel, opt_nosplit, opt_noauto, opt_nolicm, opt_autolog;
u32 opt_automax = 0xffffffffU, opt_aumax = 0xffffffffU;
int opt_dev_code, opt_dolicm, opt_slotcheck, opt_norounds, opt_nosh32, opt_nochain, opt_nowide, opt_dev_parse, opt_plaindiv, opt_noasmco, opt_nopred, opt_nodist, opt_noifcall, opt_nolocal, opt_noxrem, opt_nowiden, opt_nominmax, opt_noselk, opt_noglobal, opt_premat, opt_nospecial1, opt_ifc = 6, opt_noparm;
u32 ptx_maxntid;
u32 ptx_cluster;
u32 ptx_maxnreg;
static u32 *byoff;     /* of an OP_BYVAL: where in the frame its copy is */
static u32 *hostsyms;  /* host objects the device code addresses */
static u32 *kernsyms, nkern, capkern; /* the unit's kernels */
static u32 nhost, caphost;
static PShared *shared;
static u32 nshared, capshared;
static u32 *constsyms; /* the unit's __constant__ objects: .const in the PTX, read by ld.const */
static u32 nconst, capconst;
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

static u32 xseg; /* counts the labels written: a value worked out again since the last one need not be once more */

static void P(const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    if (strstr(fmt, ":\n"))
        xseg++;
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
    nsigs = nhost = nshared = nneed = nkern = nconst = 0;
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

void ptx_constant(u32 sym)
{
    u32 k;
    for (k = 0; k < nconst; k++)
        if (constsyms[k] == sym)
            return;
    if (nconst == capconst) {
        capconst = capconst ? capconst * 2 : 16;
        constsyms = (u32 *)xrealloc(constsyms, capconst * sizeof(u32));
    }
    constsyms[nconst++] = sym;
}

static int is_constsym(u32 sym)
{
    u32 k;
    for (k = 0; k < nconst; k++)
        if (constsyms[k] == sym)
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
static int *pm_h;  /* (a kernel's parameter blocks, below) */
static u8 *pm_s;
static u32 param_index(Func *f, u32 k);
static const char *addr_const(u32 v);
static const char *param_const(u32 v);
static int premat; /* a kernel's parameters are read where they are used (MCC_PREMAT), not held from the start */

static u32 *xc_seg;         /* where (xseg) a value was last worked out again, */
static char (*xc_name)[24]; /* and into which register */
static u32 redir_v; /* the value that is being worked out again, into redir_name */
static const char *redir_name;
static void emit_ins(u32 v);

static const char *reg(u32 v)
{
    char *s;
    int c;
    v = RV(v);
    if (v == redir_v && v)
        return redir_name;
    if (remat && (PF->ins[v].op == OP_SLOT || PF->ins[v].op == OP_ADDR))
        return addr_const(v);
    if (remat && premat && PF->ins[v].op == OP_PARAM)
        return param_const(v);
    if (remat && xrem && xrem[v] && (xrem[v] != 3 || redir_v)) {
        Ins *xi = &PF->ins[v];
        const char *t = temp(cls_of(xi->ty)), *a = reg(xi->a);
        int w = xi->ty == IT_I64 ? 64 : 32;
        u64 c = 0;
        if (xi->op == OP_SEXT32 || xi->op == OP_ZEXT32) {
            P("\tcvt.%s %s, %s;\n", xi->op == OP_SEXT32 ? "s64.s32" : "u64.u32", t, a);
            return t;
        }
        if (xrem[v] >= 2) { /* any instruction: written as it would be, into a register of its own */
            u32 sv = redir_v;
            const char *sn = redir_name;
            char *r;
            u8 was = xrem[v];
            if (xc_seg[v] == xseg) {
                r = obuf();
                strcpy(r, xc_name[v]);
                return r;
            }
            redir_v = v;
            redir_name = t;
            xrem[v] = 0;
            emit_ins(v);
            xrem[v] = was;
            redir_v = sv;
            redir_name = sn;
            r = obuf();
            strcpy(r, t);
            xc_seg[v] = xseg;
            strcpy(xc_name[v], r);
            return r;
        }
        (void)ir_is_const(PF, RV(xi->b), &c);
        if (xi->op == OP_ADD)
            P("\tadd.s%d %s, %s, %ld;\n", w, t, a, w == 64 ? (long)(i64)c : (long)(i32)c);
        else if (xi->op == OP_AND)
            P("\tand.b%d %s, %s, %ld;\n", w, t, a, w == 64 ? (long)(i64)c : (long)(i32)c);
        else
            P("\t%s%d %s, %s, %u;\n", xi->op == OP_SHL ? "shl.b" : xi->op == OP_SAR ? "shr.s" : "shr.u", w, t, a,
              (unsigned)(c & (u64)(w - 1)));
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
static int is_constsym(u32 sym);
static void host_ref(u32 sym);
static u8 *cn_s; /* the value is an address in the .const space (cn_find) */
static u8 *ause;
static u8 *wuse; /* reads of a 32-bit number made wider by products that take it as it was (wide_mul) */
static int opt_nowuse = -1; /* MCC_NOWUSE: the widening is written all the same */
static int opt_noshmask = -1; /* MCC_NOSHMASK: a shift count is copied, then masked */
static u8 *xlate; /* a 32-bit number made wider that a wide product reads as it was, where the product is */

/* The address of a local or a global object, in a register of its own for this one reader: held in one
   register from the start of the function it would be alive all through it, and a kernel has hundreds. */
static const char *addr_const(u32 v)
{
    Ins *i = &PF->ins[v];
    const char *t = temp(cls_of(i->ty));
    if (i->op == OP_ADDR && special_of(i->a) >= 0) {
        P("\tadd.u64 %s, %%SP, %ld;\n", t, (long)special_off[special_of(i->a)] + (long)(i32)i->b);
    } else if (i->op == OP_SLOT && pm_h && pm_h[i->a] >= 0) {
        /* a kernel's parameter block: its address in the .param space */
        P("\tmov.u64 %s, %s_param_%u;\n", t, kern_name, param_index(PF, (u32)pm_h[i->a]));
        if (i->b)
            P("\tadd.u64 %s, %s, %ld;\n", t, t, (long)(i32)i->b);
    } else if (i->op == OP_SLOT) {
        P("\tadd.u64 %s, %%SP, %ld;\n", t, (long)slotoff[i->a] + (long)(i32)i->b);
    } else {
        if (is_shared(i->a)) {
            P("\tmov.u64 %s, %s;\n\tcvta.shared.u64 %s, %s;\n", t, pname(i->a), t, t);
        } else if (is_constsym(i->a)) {
            /* constant memory: the address in the .const space where only ld.const reads it (cn_find), else the generic one */
            host_ref(i->a);
            if (cn_s && cn_s[v])
                P("\tmov.u64 %s, %s;\n", t, pname(i->a));
            else
                P("\tmov.u64 %s, %s;\n\tcvta.const.u64 %s, %s;\n", t, pname(i->a), t, t);
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
    if (remat && i->op == OP_SLOT && pm_h && pm_h[i->a] >= 0) {
        /* (a kernel's name is longer than obuf's: buffers of their own) */
        static char *pb[4];
        static u32 pn;
        char **t = &pb[pn++ & 3];
        off += (i64)(i32)i->b;
        *t = (char *)xrealloc(*t, strlen(kern_name) + 48);
        if (off)
            sprintf(*t, "%s_param_%u+%ld", kern_name, param_index(PF, (u32)pm_h[i->a]), (long)off);
        else
            sprintf(*t, "%s_param_%u", kern_name, param_index(PF, (u32)pm_h[i->a]));
        return *t;
    } else if (remat && (i->op == OP_SLOT || (i->op == OP_ADDR && special_of(i->a) >= 0))) {
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
    if (i->op == OP_SLOT && pm_h && pm_h[i->a] >= 0)
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
    /* (fast math: a float compare flushes subnormal operands as nvcc's -use_fast_math writes it, setp.gt.ftz.f32; without it a
       subnormal compared above 0 and the .ftz divide after it then read it as 0: ufsm hgs_exp(1e-40) gave 2^30, test_hgsdev) */
    P("\tsetp.%s%s.%s %%p%d, %s, %s;\n", c, i->op >= OP_FEQ && i->op <= OP_FGE ? FTZ(oty) : "", t, n, ropnd(i->a), opnd(i->b));
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

/* What the tree of truth values v compares is put in registers before any of its predicates is set: a value
   that is worked out where it is read (xrem) may be a choice, which sets predicates of its own; done first,
   it is found in its register when the comparison reads it. */
static void pred_warm(u32 v, int root, int depth)
{
    Ins *i;
    u32 k;
    v = RV(v);
    i = &PF->ins[v];
    if (!xrem || depth > 16)
        return;
    if (is_compare(i->op) && i->blk) {
        for (k = 0; k < 2; k++) {
            u32 x = RV(k ? i->b : i->a);
            int d;
            for (d = 0; d < 8 && xrem[x] == 1; d++)
                x = RV(PF->ins[x].a);
            if (xrem[x] >= 2 && PF->ins[x].blk)
                (void)reg(x);
        }
    } else if (ptree[v] == 2 || (root && ptree[v])) {
        pred_warm(i->a, 0, depth + 1);
        pred_warm(i->b, 0, depth + 1);
    }
}

/* Sets %p0 to "v is not zero"; a comparison is done over again here, its operands being still at hand. */
static void cond_pred(u32 v)
{
    v = RV(v);
    if (is_compare(PF->ins[v].op) && PF->ins[v].blk)
        set_pred(v);
    else if (ptree[v] == 2)
        pred_warm(v, 0, 0), pred_into(v, 0, 0);
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

/* -use_fast_math's float math, as nvcc writes it (MUFU ops, an FMUL for a base): expf / exp2f / exp10f by ex2.approx,
   logf / log2f / log10f by lg2.approx, sinf / cosf by sin / cos.approx, powf as __powf (ex2(y lg2 x)); the __ forms map
   to these names (mcuda_device.h MCUDA_DEV). tanf and tanhf stay the library's, as with nvcc. Without this every call
   went to the device library's accurate routine (41k expf calls in ufsm's PTX). MCC_NOFASTEXP: the library. */
static const char *fast_libm(u32 v, const char *name)
{
    Ins *i = &PF->ins[v];
    if (opt_nofastexp || i->ty != IT_F32 || i->b < 1 || i->b > 2)
        return 0;
    if (i->b == 2) /* powf: __powf's ex2.approx(y lg2.approx(x)) */
        return strcmp(name, "powf") ? 0 : "lg2.approx.ftz.f32 $d, $0; mul.ftz.f32 $d, $d, $1; ex2.approx.ftz.f32 $d, $d;";
    if (!strcmp(name, "sinf"))
        return "sin.approx.ftz.f32 $d, $0;";
    if (!strcmp(name, "cosf"))
        return "cos.approx.ftz.f32 $d, $0;";
    if (!strcmp(name, "exp10f"))
        return "mul.ftz.f32 $d, $0, 0f40549A78; ex2.approx.ftz.f32 $d, $d;";
    if (!strcmp(name, "log10f"))
        return "lg2.approx.ftz.f32 $d, $0; mul.ftz.f32 $d, $d, 0f3E9A209B;";
    if (!strcmp(name, "expf"))
        return "mul.ftz.f32 $d, $0, 0f3FB8AA3B; ex2.approx.ftz.f32 $d, $d;";
    if (!strcmp(name, "exp2f"))
        return "ex2.approx.ftz.f32 $d, $0;";
    if (!strcmp(name, "logf"))
        return "lg2.approx.ftz.f32 $d, $0; mul.ftz.f32 $d, $d, 0f3F317218;";
    if (!strcmp(name, "log2f"))
        return "lg2.approx.ftz.f32 $d, $0;";
    return 0;
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
/* An address read from global memory is one of global memory too (a kernel's table of descriptors, each with its
   buffers' addresses: ufsm's muon_desc_t / anvil_desc_t): ld.global / st.global, as nvcc's front end assumes (its
   PTX: cvta.to.global of the loaded value). Plain global, never .nc. MCC_NOGLLOAD: generic as before. */
static int opt_nogll = -1;

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

/* ---- constant memory ----
   An address worked out from a __constant__ object's (plus or minus whole numbers, one of several that all are) is
   one in the .const space when every reader is a load through it or such a sum: those loads are ld.const (LDC, an
   operand from the constant bank, as nvcc's) and the addresses stay in the .const space. One that goes anywhere
   else (a call, a store of the pointer, a comparison, a copy) makes all the addresses it is joined with generic
   (cvta.const) and their loads plain ld. */
static void cn_find(Func *f)
{
    u32 v, k, n, *p;
    int changed;
    u8 *esc;
    memset(cn_s, 0, f->nins + 1);
    if (!nconst)
        return;
    for (v = 1; v < f->nins; v++)
        if (f->ins[v].op == OP_ADDR && is_constsym(f->ins[v].a) && special_of(f->ins[v].a) < 0)   /* (addresses float: no block) */
            cn_s[v] = 1;
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            int is = 0;
            if (cn_s[v] || !i->blk || i->ty != IT_I64)
                continue;
            if (i->op == OP_ADD)
                is = !cn_s[RV(i->a)] != !cn_s[RV(i->b)];
            else if (i->op == OP_SUB)
                is = cn_s[RV(i->a)] && !cn_s[RV(i->b)];
            else if (i->op == OP_SELECT)
                is = cn_s[RV(i->b)] && cn_s[RV(i->c)] && !cn_s[RV(i->a)];
            else if (i->op == OP_PHI && i->b) {
                is = 1;
                for (k = 0; k < i->b; k++)
                    is &= cn_s[RV(f->xargs[i->a + k])];
            }
            if (is) {
                cn_s[v] = 1;
                changed = 1;
            }
        }
    } while (changed);
    esc = (u8 *)xmalloc((size_t)f->nins + 1);
    memset(esc, 0, f->nins + 1);
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        if (!i->blk || i->op == OP_ADDR)
            continue;
        IR_OPS(f, i, p, n);
        for (k = 0; k < n; k++) {
            u32 x = RV(p[k]);
            int ok;
            if (!x || x >= f->nins || !cn_s[x])
                continue;
            ok = (i->op == OP_LOAD && p + k == &i->a) ||
                 (cn_s[v] && (i->op == OP_ADD || i->op == OP_SUB || i->op == OP_PHI || (i->op == OP_SELECT && p + k != &i->a)));
            if (!ok)
                esc[x] = 1;
        }
    }
    /* a generic address anywhere in a joined set makes the whole set generic */
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            if (!cn_s[v] || i->op == OP_ADDR)
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]);
                if (!x || x >= f->nins || !cn_s[x] || esc[x] == esc[v])
                    continue;
                esc[x] = esc[v] = 1;
                changed = 1;
            }
        }
    } while (changed);
    for (v = 1; v < f->nins; v++)
        if (esc[v])
            cn_s[v] = 0;
    free(esc);
}

/* ---- a kernel's parameter blocks ----
   A kernel's struct parameter arrives as doublewords of the .param space that the front end stores into a slot of the
   frame (parse.c), so that members read at constant places become values. One read at a place worked out at run time
   (p.ta[q]) kept the slot: 240 bytes of local memory filled at the start and LDL in the loop, where nvcc reads the
   parameter itself (LDC from the constant bank). When the slot's only stores are those of the parameter's own
   doublewords at their own offsets and every other reader of an address worked out from it is a load (or such a sum),
   the loads are ld.param from the parameter (an address of a kernel's parameter is one of the .param space) and the
   slot and its stores go. Anything else (a copy, a call, a stored pointer, a comparison) keeps the slot. */
/* pm_h: per slot, the PTX parameter whose block it copies, or -1; pm_s: per value, an address in such a slot */

/* store v is one of the front end's, of doubleword k of a block parameter into its slot at 8k: the slot (else -1) */
static int pm_init(Func *f, u32 v, u32 *h_out)
{
    Ins *i = &f->ins[v], *a, *x;
    i64 off = 0;
    u32 r = RV(i->a), pk, h;
    if (i->op != OP_STORE || i->mem != M_64)
        return -1;
    if (addr_sum(r, &off))
        r = RV(f->ins[r].a);
    a = &f->ins[r];
    if (a->op != OP_SLOT)
        return -1;
    off += (i64)(i32)a->b;
    x = &f->ins[RV(i->b)];
    if (x->op != OP_PARAM || !(f->param_ty[x->a] & 0xc0))
        return -1;
    pk = x->a;
    for (h = pk; h && (f->param_ty[h] & 0x40); h--)
        ;
    if (off != 8 * (i64)(pk - h))
        return -1;
    *h_out = h;
    return (int)a->a;
}

static void pm_find(Func *f)
{
    u32 v, k, n, *p, h;
    int changed, again, s;
    u8 *esc;
    for (k = 0; k < f->nslots; k++)
        pm_h[k] = -2;
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        if (!i->blk || i->op != OP_STORE)
            continue;
        if ((s = pm_init(f, v, &h)) >= 0)
            pm_h[s] = pm_h[s] == -2 || pm_h[s] == (int)h ? (int)h : -1;
    }
    for (k = 0; k < f->nslots; k++)
        if (pm_h[k] == -2)
            pm_h[k] = -1;
    esc = (u8 *)xmalloc((size_t)f->nins + 1);
    do {
        again = 0;
        memset(pm_s, 0, f->nins + 1);
        for (v = 1; v < f->nins; v++)
            if (f->ins[v].op == OP_SLOT && pm_h[f->ins[v].a] >= 0)
                pm_s[v] = 1;
        do {
            changed = 0;
            for (v = 1; v < f->nins; v++) {
                Ins *i = &f->ins[v];
                int is = 0;
                if (pm_s[v] || !i->blk || i->ty != IT_I64)
                    continue;
                if (i->op == OP_ADD)
                    is = !pm_s[RV(i->a)] != !pm_s[RV(i->b)];
                else if (i->op == OP_SUB)
                    is = pm_s[RV(i->a)] && !pm_s[RV(i->b)];
                else if (i->op == OP_SELECT)
                    is = pm_s[RV(i->b)] && pm_s[RV(i->c)] && !pm_s[RV(i->a)];
                else if (i->op == OP_PHI && i->b) {
                    is = 1;
                    for (k = 0; k < i->b; k++)
                        is &= pm_s[RV(f->xargs[i->a + k])];
                }
                if (is) {
                    pm_s[v] = 1;
                    changed = 1;
                }
            }
        } while (changed);
        memset(esc, 0, f->nins + 1);
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            if (!i->blk || i->op == OP_SLOT)
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]);
                int ok;
                if (!x || x >= f->nins || !pm_s[x])
                    continue;
                ok = (i->op == OP_LOAD && p + k == &i->a) || (i->op == OP_STORE && p + k == &i->a && pm_init(f, v, &h) >= 0) ||
                     (pm_s[v] && (i->op == OP_ADD || i->op == OP_SUB || i->op == OP_PHI || (i->op == OP_SELECT && p + k != &i->a)));
                if (!ok)
                    esc[x] = 1;
            }
        }
        do {
            changed = 0;
            for (v = 1; v < f->nins; v++) {
                Ins *i = &f->ins[v];
                if (!pm_s[v] || i->op == OP_SLOT)
                    continue;
                IR_OPS(f, i, p, n);
                for (k = 0; k < n; k++) {
                    u32 x = RV(p[k]);
                    if (!x || x >= f->nins || !pm_s[x] || esc[x] == esc[v])
                        continue;
                    esc[x] = esc[v] = 1;
                    changed = 1;
                }
            }
        } while (changed);
        for (v = 1; v < f->nins; v++)
            if (esc[v] && f->ins[v].op == OP_SLOT && pm_h[f->ins[v].a] >= 0) {
                pm_h[f->ins[v].a] = -1;
                again = 1;
            }
    } while (again);
    free(esc);
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
    if (opt_nogll < 0)
        opt_nogll = getenv("MCC_NOGLLOAD") != 0;
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
            else if (i->op == OP_LOAD)
                is = !opt_nogll && gl_s[RV(i->a)] != 0;
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
            gl_s[v] = f->ins[v].op == OP_LOAD ? 1 : f->ins[v].op != OP_PARAM ? 2 : f->ins[v].mem == PARAM_GLOBAL_NC && !opt_nonc ? 2 : 1;
    do {
        changed = 0;
        for (v = 1; v < f->nins; v++) {
            Ins *i = &f->ins[v];
            u8 to;
            if (!gl_s[v] || i->op == OP_PARAM || i->op == OP_LOAD)
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
        if (opt_fastmath && fast_libm(v, name)) {
            if (!used[v])
                return;
            for (k = 0; k < i->b; k++)
                args[k] = opnd(PF->xargs[i->a + k]);
            expand(fast_libm(v, name), '$', reg(v), args, i->b);
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

extern int opt_novecmem;
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
        if (n - off >= 16 && al >= 16 && !opt_novecmem) {
            /* 16 bytes of an object of 16-byte alignment (a float4, a uint4): one vector load and store, as nvcc's */
            char t2[24];
            t = temp(1);
            strcpy(t2, t);
            t = temp(1);
            if (s)
                P("\tld.%sv2.u64 {%s, %s}, [%s%+ld];\n", ssp, t2, t, s, (long)(soff + (i64)off));
            else
                P("\tmov.b64 %s, 0;\n\tmov.b64 %s, 0;\n", t2, t);
            P("\tst.%sv2.u64 [%s%+ld], {%s, %s};\n", dsp, d, (long)(doff + (i64)off), t2, t);
            off += 16;
            continue;
        }
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

static int vec_emit(u32 v);
static u8 *vg_skip;

static void emit_ins(u32 v)
{
    Ins *i = &PF->ins[v];
    int ty = i->ty, oty;
    const char *d, *a, *b, *t;

    if (sh_s && ty == IT_I64 && sh_emit(v))
        return;
    if (xrem && xrem[v] && xrem[v] != 3)
        return; /* worked out where it is read */
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
            pred_warm(v, 1, 0);
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
        if (opt_noshmask < 0)
            opt_noshmask = getenv("MCC_NOSHMASK") != 0;
        if (!opt_noshmask && (PF->ins[cv].op == OP_ZEXT32 || PF->ins[cv].op == OP_SEXT32) && !(xrem && xrem[cv]))
            cv = RV(PF->ins[cv].a); /* a count made wider: its low 32 bits are the number it was made of */
        if (!opt_noshmask && PF->ins[cv].ty == IT_I32) {
            /* the count masked where it is (no copy first), or not at all when it is known to be below the width */
            if (ir_umax32(PF, cv) <= (u64)(ty == IT_I64 ? 63 : 31)) {
                P("\t%s.%s %s, %s, %s;\n", op, tt, reg(v), a, reg(cv));
                return;
            }
            t = temp(0);
            P("\tand.b32 %s, %s, %d;\n", t, reg(cv), ty == IT_I64 ? 63 : 31);
            P("\t%s.%s %s, %s, %s;\n", op, tt, reg(v), a, t);
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
        if (opt_nowuse < 0)
            opt_nowuse = getenv("MCC_NOWUSE") != 0;
        if (!opt_nowuse && nuse[v] < 255 && nuse[v] == wuse[v])
            return; /* every reader is a wide product, which reads the 32 bits (mul.wide) */
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
        if (vg_skip && vg_skip[v])
            return;
        if (vec_emit(v))
            return;
        if (special_load(v)) {
            P("\tmov.u32 %s, %s;\n", reg(v), special_load(v));
            return;
        }
        if ((a = sh_maddr(i->a)) != 0) {
            P("\tld.shared.%s %s, [%s];\n", lt[i->mem], reg(v), a);
            return;
        }
        if (pm_s && pm_s[RV(i->a)]) {
            P("\tld.param.%s %s, [%s];\n", lt[i->mem], reg(v), maddr(i->a));
            return;
        }
        if ((a = local_maddr(i->a)) != 0) {
            P("\tld.local.%s %s, [%s];\n", lt[i->mem], reg(v), a);
            return;
        }
        if (cn_s && cn_s[RV(i->a)]) {
            P("\tld.const.%s %s, [%s];\n", lt[i->mem], reg(v), maddr(i->a));
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
        if (vg_skip && vg_skip[v])
            return;
        if (vec_emit(v))
            return;
        if ((a = sh_maddr(i->a)) != 0) {
            b = ropnd(i->b);
            P("\tst.shared.%s [%s], %s;\n", stt[i->mem], a, b);
            return;
        }
        if (pm_s && pm_s[RV(i->a)])
            return; /* the front end's copy of a kernel's parameter block, read where it is (pm_find) */
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
static int opt_nocotl = -1;

static void coalesce(const u8 *live)
{
    Func *f = PF;
    u32 nb = f->nblk, b, v, k, n, *p, at = 0, *wh, *tl;
    u32 *pos = (u32 *)xmalloc(2 * (size_t)(f->nins + 1) * sizeof(u32)), *last = pos + f->nins + 1;
    memset(pos, 0, 2 * (size_t)(f->nins + 1) * sizeof(u32));
#define NOSHARE 0xffffffffU
#define READ_BY(x, where) \
    do { \
        u32 x_ = RV(x); \
        if (x_ && f->ins[x_].op == OP_PHI && last[x_] < (where)) \
            last[x_] = (where); \
    } while (0)
    /* A value that is not worked out where it stands but again where it is read (a comparison, a fused product,
       a widening that its readers take as it was, ...) reads its operands where its readers are: tl, the last of
       them in its block (NOSHARE if one is elsewhere, or is a phi or a block's test). MCC_NOCOTL: such a value
       reads its operands nowhere in particular (NOSHARE), as before. */
    if (opt_nocotl < 0)
        opt_nocotl = getenv("MCC_NOCOTL") != 0;
    wh = (u32 *)xmalloc(2 * (size_t)(f->nins + 1) * sizeof(u32));
    tl = wh + f->nins + 1;
    memset(wh, 0, 2 * (size_t)(f->nins + 1) * sizeof(u32));
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            int redone = is_compare(i->op) || ptree[v] || (xrem && xrem[v]) || fused[v] || i->op == OP_SELECT || (sh_s && sh_t32[v] && sh_ext(v)) || xlate[v];
            wh[v] = redone ? 0 : ++at; /* 0: what tl[v] says */
            if (redone && opt_nocotl)
                wh[v] = NOSHARE;
            pos[v] = ++at;
        }
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                tl[RV(f->xargs[f->ins[v].a + k])] = NOSHARE;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]);
                if (x && f->ins[x].blk != b)
                    tl[x] = NOSHARE;
            }
            if (i->op == OP_CALL)
                tl[RV(i->c)] = NOSHARE;
        }
        if (bb->arg)
            tl[RV(bb->arg)] = NOSHARE;
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->last; v; v = f->ins[v].prev) {
            Ins *i = &f->ins[v];
            u32 w = wh[v] ? wh[v] : tl[v];
            if (special_load(v))
                continue;
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++) {
                u32 x = RV(p[k]);
                if (x && f->ins[x].blk == b && tl[x] != NOSHARE && (w == NOSHARE || tl[x] < w))
                    tl[x] = w;
            }
        }
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                READ_BY(f->xargs[f->ins[v].a + k], NOSHARE);
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 where = wh[v] ? wh[v] : tl[v] ? tl[v] : NOSHARE;
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
    free(wh);
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

/* A value X that only the phi P of block s reads, on the edge from p, is written into P's register in the first
   place where P's register is not live in p: s cannot reach p again (the join after an if, a loop's way in), and
   whatever else holds that register is in what s reaches, or is another such X of another way into s. X is an
   instruction written where it stands that writes its result last. MCC_NOJOINCO: off. */
static int opt_nojoinco = -1;

static void coalesce_join(const u8 *live)
{
    Func *f = PF;
    u32 nb = f->nblk, b, v, k, n, *p, *stack, sp, u, stamp = 0;
    u32 *reach, *cnt, *mine;
    if (opt_nojoinco < 0)
        opt_nojoinco = getenv("MCC_NOJOINCO") != 0;
    if (opt_nojoinco)
        return;
    reach = (u32 *)xmalloc((nb + 1) * sizeof(u32));
    stack = (u32 *)xmalloc((nb + 1) * sizeof(u32));
    cnt = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
    mine = (u32 *)xmalloc((f->nins + 1) * sizeof(u32)); /* X given P's register here: the block s of P */
    memset(reach, 0, (nb + 1) * sizeof(u32));
    memset(cnt, 0, (f->nins + 1) * sizeof(u32));
    memset(mine, 0, (f->nins + 1) * sizeof(u32));
    /* every read, phis' included */
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                cnt[RV(f->xargs[f->ins[v].a + k])]++;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                cnt[RV(p[k])]++;
            if (i->op == OP_CALL)
                cnt[RV(i->c)]++;
        }
        if (bb->arg)
            cnt[RV(bb->arg)] += 2;
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b] || !bb->phis)
            continue;
        /* what b reaches */
        stamp++;
        sp = 0;
        stack[sp++] = b;
        reach[b] = stamp;
        while (sp) {
            Block *cb = &f->blk[stack[--sp]];
            u32 ns = cb->term == TERM_BR ? 2 : cb->term == TERM_JMP ? 1 : 0;
            for (k = 0; k < ns; k++) {
                u32 t = cb->succ[k];
                if (live[t] && reach[t] != stamp)
                    reach[t] = stamp, stack[sp++] = t;
            }
        }
        for (v = bb->phis; v; v = f->ins[v].next) {
            Ins *pi = &f->ins[v];
            if (!used[v])
                continue;
            for (k = 0; k < pi->b; k++) {
                u32 x = RV(f->xargs[pi->a + k]), pb = bb->preds[k], r, j;
                Ins *xi = &f->ins[x];
                int ok = 0;
                if (!x || x == v || !live[pb] || reach[pb] == stamp || xi->blk != pb || cnt[x] != 1 || !used[x] || regno[x] ||
                    cls_of(xi->ty) != cls_of(pi->ty) || is_compare(xi->op) || ptree[x] || (xrem && xrem[x]) || fused[x] ||
                    (sh_s && sh_t32[x] && sh_ext(x)) || xlate[x] || (vg_skip && vg_skip[x]) || special_load(x))
                    continue;
                for (j = 0; j < k; j++)
                    if (bb->preds[j] == pb)
                        break;
                if (j < k)
                    continue; /* (two ways from pb) */
                switch (xi->op) {
                case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
                case OP_SHL: case OP_SHR: case OP_SAR: case OP_FADD: case OP_FSUB: case OP_FDIV:
                    ok = !(xi->op == OP_ADD && nuse[x] < 255 && nuse[x] == ause[x]);
                    break;
                case OP_FMUL:
                    ok = 1;
                    break;
                case OP_LOAD:
                    ok = xi->mem <= M_F64 && !(xi->flags & ~(IF_NC | IF_ALIGN));
                    break;
                default:
                    break;
                }
                if (!ok)
                    continue;
                (void)reg(v);
                r = regno[v];
                /* who else holds the register */
                for (u = 1; u < f->nins && ok; u++) {
                    Ins *ui = &f->ins[u];
                    if (u == v || regno[u] != r || !ui->blk || cls_of(ui->ty) != cls_of(pi->ty))
                        continue;
                    if (reach[ui->blk] == stamp)
                        continue; /* in what b reaches */
                    if (mine[u] == b && ui->blk != pb)
                        continue; /* another way's value */
                    ok = 0;
                }
                if (!ok)
                    continue;
                regno[x] = r;
                mine[x] = b;
            }
        }
    }
    free(reach);
    free(stack);
    free(cnt);
    free(mine);
}

/* y + (an object's address + k), k not 0 (an unrolled loop's a[k][tx]: bs + 64 * k + ...): (y + the address) + k, the
   first part one value for the whole block, which every such sum of the block shares, and k where the address is
   read ([%r32+64]) rather than mov bs, add 64, add y each time. MCC_NOADDRSPLIT: off. */
static void addr_split(Func *f)
{
    static int off = -1;
    u32 b, v;
    if (off < 0)
        off = getenv("MCC_NOADDRSPLIT") != 0;
    if (off)
        return;
    for (b = 1; b < f->nblk; b++) {
        u32 my[16], ms[16], mb[16], nm = 0;
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 a, c, y, k, base, sym;
            i32 o;
            if (i->op != OP_ADD || i->ty != IT_I64)
                continue;
            a = RV(i->a);
            c = RV(i->b);
            if (f->ins[a].op == OP_ADDR) {
                u32 t = a;
                a = c;
                c = t;
            }
            if (f->ins[c].op != OP_ADDR || f->ins[a].op == OP_CONST || f->ins[a].op == OP_ADDR)
                continue;
            y = a;
            sym = f->ins[c].a;
            o = (i32)f->ins[c].b;
            for (k = 0; k < nm && !(my[k] == y && ms[k] == sym); k++)
                ;
            if (!o) {
                /* y + the address itself: the base, if there is none yet */
                if (k == nm && nm < 16) {
                    my[nm] = y;
                    ms[nm] = sym;
                    mb[nm++] = v;
                } else if (k < nm) {
                    i->a = mb[k];
                    i->b = ir_const(f, IT_I64, 0);
                }
                continue;
            }
            if (k == nm) {
                if (nm == 16)
                    continue;
                base = ir_raw(f, OP_ADD, IT_I64, y, ir_addr(f, sym, 0));
                ir_insert_before(f, v, base);
                my[nm] = y;
                ms[nm] = sym;
                mb[nm++] = base;
            } else {
                base = mb[k];
            }
            i = &f->ins[v];
            i->a = base;
            i->b = ir_const(f, IT_I64, (u64)(i64)o);
        }
    }
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

static int opt_noh16 = -1; /* MCC_NOH16: the conversions stay */

/* Is the 32-bit value v one whose bits above the low 16 are zero (a 16-bit float's bits as they are read or made)? */
static int bits16(Func *f, u32 v)
{
    Ins *i = &f->ins[v];
    const char *n;
    u64 c;
    if (i->ty != IT_I32)
        return 0;
    if (i->op == OP_LOAD)
        return i->mem == M_16U || i->mem == M_8U;
    if (i->op == OP_ZEXT16 || i->op == OP_ZEXT8)
        return 1;
    if (i->op == OP_CONST)
        return (u32)i->a <= 0xffffU;
    if (i->op == OP_AND)
        return (ir_is_const(f, RV(i->b), &c) && (u32)c <= 0xffffU) || (ir_is_const(f, RV(i->a), &c) && (u32)c <= 0xffffU);
    n = callee_name(v);
    return n && (!strcmp(n, "__mcc_f2h") || !strcmp(n, "__mcc_f2bf"));
}

/* A float that was rounded to a 16-bit float and is then stored as one, or rounded again, is rounded once:
   the conversion for the store does it (the front end rounds where the type changes and converts where
   the value is stored).
   A 16-bit float that is only turned into a float and back (a __half read and stored or packed again: the front end
   works on its float) is its bits as they were, and a float that was a 16-bit float is that rounded: ufsm's fstem_fwd_k
   had 154 cvt.f32.f16 and 209 cvt.rn.f16.f32 to nvcc's 49 and 74 (which copies the bits). (A NaN's bits stay as they
   were read, as with nvcc; through the floats they were quieted.) The conversion to a float reads the low 16 bits:
   a mask or a zero extension of what it converts goes. */
static void round_once(void)
{
    Func *f = PF;
    u32 v;
    int any = 0, rep = 0;
    if (opt_noh16 < 0)
        opt_noh16 = getenv("MCC_NOH16") != 0;
    for (v = 1; v < f->nins; v++) {
        const char *n = callee_name(v), *m;
        u32 x;
        if (!n || strncmp(n, "__mcc_", 6))
            continue;
        x = RV(f->xargs[f->ins[v].a]);
        if (!strcmp(n, "__mcc_h2f") || !strcmp(n, "__mcc_bf2f")) {
            Ins *xi = &f->ins[x];
            u64 c;
            if (xi->ty == IT_I32 && (xi->op == OP_ZEXT16 || (xi->op == OP_AND && ir_is_const(f, RV(xi->b), &c) && ((u32)c & 0xffffU) == 0xffffU))) {
                f->xargs[f->ins[v].a] = RV(xi->a);
                any = 1;
            }
            continue;
        }
        if (strncmp(n, "__mcc_f", 7))
            continue;
        m = callee_name(x);
        if (!m)
            continue;
        if (((!strcmp(n, "__mcc_f2h") && !strcmp(m, "__mcc_h2f")) || (!strcmp(n, "__mcc_f2bf") && !strcmp(m, "__mcc_bf2f"))) &&
            !opt_noh16 && f->ins[v].ty == IT_I32 && bits16(f, RV(f->xargs[f->ins[x].a]))) {
            ir_replace(f, v, RV(f->xargs[f->ins[x].a]));
            any = rep = 1;
            continue;
        }
        if (((!strcmp(n, "__mcc_fround16") && !strcmp(m, "__mcc_h2f")) || (!strcmp(n, "__mcc_froundbf") && !strcmp(m, "__mcc_bf2f"))) &&
            !opt_noh16) {
            ir_replace(f, v, x);
            any = rep = 1;
            continue;
        }
        if (((!strcmp(n, "__mcc_f2bf") || !strcmp(n, "__mcc_froundbf")) && !strcmp(m, "__mcc_froundbf")) ||
            ((!strcmp(n, "__mcc_f2h") || !strcmp(n, "__mcc_fround16")) && !strcmp(m, "__mcc_fround16"))) {
            f->xargs[f->ins[v].a] = f->xargs[f->ins[x].a];
            any = 1;
        }
    }
    if (rep)
        ir_resolve_all(f);
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
    /* (a way back: s numbered no later than b and dominating it. The number alone does not tell: a loop's exit block made
       before the unrolled copies is numbered below them, and an early break out of `#pragma unroll` then made its 32 moves
       on the way on as well, every copy: conv1_bwd_fused_k's co loop, 512 moves a voxel step. Dominance alone admitted
       later-numbered heads too, and f8s / fwd_s2_f4 lost 6-8 % (R11e 11-26). MCC_LATCHNUM=1: the number test only) */
    if (s > b || t == s || !sb->phis || (!getenv("MCC_LATCHNUM") && !opt_dominates(s, b)))
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
/* Is x an address in shared memory, or part of one, that is kept as 32 bits? Those are written in their own way. */
static int sh_any(u32 x)
{
    x = RV(x);
    return sh_s && (sh_s[x] || sh_t32[x]);
}

/* Value x is read in block b: if it is cheap to work out again from a value and a constant, and was made in another
   block, it is made again where it is read, and so holds no register in between (the value it is made from is then
   read in b; so are the operands of a comparison, which is always done where its result is wanted). */
static void xrem_mark(Func *f, u32 x, u32 b, int depth)
{
    Ins *xi;
    x = RV(x);
    xi = &f->ins[x];
    if (!x || !xi->blk || depth > 8 || xi->op == OP_PHI)
        return; /* (a phi's a is where its arguments are, not a value: read as one it was past the end of the
                   instructions once jump threading had made phis with many arguments) */
    if (is_compare(xi->op) || ptree[x]) { /* (a tree of truth values is worked out where a branch or a choice reads it) */
        xrem_mark(f, xi->a, b, depth + 1);
        xrem_mark(f, xi->b, b, depth + 1);
        return;
    }
    if (xrem[x]) {
        xrem_mark(f, xi->a, b, depth + 1);
        return;
    }
    if (xi->blk == b || !used[x] || xlate[x] || f->ins[RV(xi->a)].op == OP_CONST)
        return;
    if (xi->op == OP_SEXT32 || xi->op == OP_ZEXT32 ||
        (opt_noxrem != 2 && (xi->ty == IT_I32 || xi->ty == IT_I64) && f->ins[RV(xi->b)].op == OP_CONST && !fused[x] &&
         !sh_any(x) && !sh_any(xi->a) &&
         !ptree[x] && !(nuse[x] < 255 && nuse[x] == ause[x]) &&
         (xi->op == OP_ADD || xi->op == OP_AND || xi->op == OP_SHR || xi->op == OP_SAR ||
          (xi->op == OP_SHL && xi->ty == IT_I32)))) {
        xrem[x] = 1;
        xrem_mark(f, xi->a, b, depth + 1);
    }
}

/* The leaves of value x for xrem_live: x itself, or what it is made from where it is itself worked out where read. */
static u32 xrem_leaf(Func *f, u32 x)
{
    int d;
    x = RV(x);
    for (d = 0; d < 8 && x && xrem[x] == 1; d++)
        x = RV(f->ins[x].a);
    return x;
}

/* The second kind of value that is worked out where it is read: one instruction on two values (a sum, a product, ...),
   made in one block and read in others, where at each of those both values are alive in any case. The register it held
   between is then free at no cost but the instruction, done again. Which values are alive where is found for the
   operands of such instructions only (sets of them per block, by the usual iteration). Where a value is read is where
   its reader is written: for a comparison or a value of the first kind, where that in turn is read (xl_walk). */
typedef struct XTree {
    u32 leaf[4]; /* what the value is made from: values that are not themselves worked out again */
    u32 in[16];  /* and by way of which instructions, read cnt times in the tree */
    u8 cnt[16];
    u8 nl, ni;
} XTree;
static u32 *xl_num, *xl_gen, *xl_in, xl_w, *xl_ci;
static u8 *xl_bad;
static XTree *xl_t;

static int xl_alive(Func *f, u32 o, u32 b)
{
    if (f->ins[o].op == OP_PARAM)
        return 1; /* (ptxas reads a parameter where it is wanted) */
    return xl_num[o] && (xl_in[(size_t)b * xl_w + (xl_num[o] >> 5)] >> (xl_num[o] & 31) & 1);
}

/* Can instruction x be done again where its result is read? */
static int xl_cheap(Func *f, u32 x)
{
    Ins *i = &f->ins[x];
    if (!i->blk || !used[x] || xrem[x] || (i->ty != IT_I32 && i->ty != IT_I64) || fused[x] || ptree[x] || xlate[x] ||
        sh_any(x) || sh_any(i->a) || sh_any(i->b) || (i->op == OP_SELECT && sh_any(i->c)) ||
        (nuse[x] < 255 && nuse[x] == ause[x]))
        return 0;
    switch (i->op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR: case OP_SELECT:
        return 1;
    case OP_MUL:
        return i->ty == IT_I32;
    case OP_SHL:
        return i->ty == IT_I32 && f->ins[RV(i->b)].op == OP_CONST;
    case OP_SHR: case OP_SAR:
        return f->ins[RV(i->b)].op == OP_CONST;
    default:
        return 0;
    }
}

static int xl_expand(Func *f, XTree *t, u32 x, int depth);

/* The leaves of what instruction x reads. A condition is worked out where it is wanted from what it compares. */
static int xl_cond(Func *f, XTree *t, u32 c, int depth)
{
    Ins *ci;
    c = RV(c);
    ci = &f->ins[c];
    if (depth < 12 && ci->blk && (is_compare(ci->op) || ptree[c]))
        return (ptree[c] ? xl_cond(f, t, ci->a, depth + 1) && xl_cond(f, t, ci->b, depth + 1)
                         : xl_expand(f, t, ci->a, depth + 1) && xl_expand(f, t, ci->b, depth + 1));
    return xl_expand(f, t, c, depth);
}

static int xl_operands(Func *f, XTree *t, u32 x, int depth)
{
    Ins *i = &f->ins[x];
    if (i->op == OP_SELECT)
        return xl_cond(f, t, i->a, depth) && xl_expand(f, t, i->b, depth) && xl_expand(f, t, i->c, depth);
    return xl_expand(f, t, i->a, depth) && xl_expand(f, t, i->b, depth);
}

/* The leaves of x, an operand (at some depth) of the tree t. */
static int xl_expand(Func *f, XTree *t, u32 x, int depth)
{
    u32 k;
    x = xrem_leaf(f, x);
    if (!x || xrem[x])
        return 0;
    if (!f->ins[x].blk && f->ins[x].op != OP_PARAM)
        return 1; /* a constant or an address: written where it is read */
    for (k = 0; k < t->ni; k++)
        if (t->in[k] == x) {
            t->cnt[k]++;
            return 1;
        }
    if (depth < 7 && nuse[x] <= 3 && t->ni < 16 && xl_cheap(f, x)) {
        t->cnt[t->ni] = 1;
        t->in[t->ni++] = x;
        return xl_operands(f, t, x, depth + 1);
    }
    for (k = 0; k < t->nl; k++)
        if (t->leaf[k] == x)
            return 1;
    if (t->nl == 4)
        return 0;
    t->leaf[t->nl++] = x;
    return 1;
}

/* x is read in block b. mode 0: note a value read in its own block; 1: note what is read from another block;
   2: note a value read where its operands are not both alive. */
static void xl_walk(Func *f, u32 x, u32 b, int mode, int depth)
{
    Ins *xi;
    x = RV(x);
    xi = &f->ins[x];
    if (!x || !xi->blk || depth > 8)
        return;
    if (is_compare(xi->op) || ptree[x]) {
        xl_walk(f, xi->a, b, mode, depth + 1);
        xl_walk(f, xi->b, b, mode, depth + 1);
        return;
    }
    if (xrem[x] == 1 || (sh_s && sh_t32[x] && sh_ext(x) && !sh_g64[x])) { /* (the second: read by sums of 32 bits) */
        xl_walk(f, xi->a, b, mode, depth + 1);
        return;
    }
    if (mode == 0) {
        if (xi->blk == b)
            xl_bad[x] = 1;
    } else if (mode == 1) {
        if (xl_num[x] && xi->blk != b)
            xl_gen[(size_t)b * xl_w + (xl_num[x] >> 5)] |= (u32)1 << (xl_num[x] & 31);
    } else if (xi->blk != b && xl_ci[x]) {
        XTree *t = &xl_t[xl_ci[x] - 1];
        u32 k;
        for (k = 0; k < t->nl; k++)
            if (!xl_alive(f, t->leaf[k], b))
                xl_bad[x] = 1;
    }
}

/* Every read of a value by the body of block b (not by phis), given to xl_walk. */
static void xl_block(Func *f, u32 b, int mode)
{
    Block *bb = &f->blk[b];
    u32 v, k, n, *p;
    for (v = bb->first; v; v = f->ins[v].next) {
        Ins *i = &f->ins[v];
        if (is_compare(i->op) || ptree[v] == 2 || xrem[v] == 1 || (sh_s && sh_t32[v] && sh_ext(v) && !sh_g64[v]))
            continue; /* read where v is read */
        IR_OPS(f, i, p, n);
        for (k = 0; k < n; k++)
            xl_walk(f, p[k], b, mode, 0);
    }
    if (bb->arg)
        xl_walk(f, bb->arg, b, mode, 0);
}

static void xrem_live(Func *f, const u8 *live)
{
    u32 nb = f->nblk, b, v, k, n, *p, ns = 0, w, *num, *in, *out, *gen, *def, *cand, nc = 0, c, ct = 0, *ci;
    u8 *bad;
    int ch;
    xl_t = 0;
    ci = (u32 *)xmalloc((size_t)(f->nins + 1) * sizeof(u32));
    memset(ci, 0, (size_t)(f->nins + 1) * sizeof(u32));
    xl_ci = ci;
    num = (u32 *)xmalloc((size_t)(f->nins + 1) * sizeof(u32));
    cand = (u32 *)xmalloc((size_t)(f->nins + 1) * sizeof(u32));
    bad = (u8 *)xmalloc((size_t)f->nins + 1);
    memset(num, 0, (size_t)(f->nins + 1) * sizeof(u32));
    memset(bad, 0, (size_t)f->nins + 1);
    /* a value read in its own block, by a phi, or by an instruction that is written who knows where stays as it is */
    xl_bad = bad;
    xl_num = num;
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                bad[RV(f->xargs[f->ins[v].a + k])] = 2;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            int far = ptree[v] == 1 || fused[v] || xlate[v] || (sh_s && sh_t32[v] && sh_ext(v) && sh_g64[v]) || special_load(v);
            IR_OPS(f, i, p, n);
            for (k = 0; k < n; k++)
                if (far)
                    bad[RV(p[k])] = 3;
            if (i->op == OP_CALL)
                bad[RV(i->c)] = 4;
        }
        xl_block(f, b, 0);
    }
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->first; v; v = f->ins[v].next) {
            XTree *t;
            if (bad[v] || nuse[v] > 8 || !xl_cheap(f, v))
                continue;
            if (nc == ct) {
                XTree *nt = (XTree *)xmalloc((size_t)(ct ? 2 * ct : 64) * sizeof(XTree));
                if (ct)
                    memcpy(nt, xl_t, (size_t)ct * sizeof(XTree));
                free(xl_t);
                xl_t = nt;
                ct = ct ? 2 * ct : 64;
            }
            t = &xl_t[nc];
            t->nl = t->ni = 0;
            if (!xl_operands(f, t, v, 1) || !t->nl)
                continue;
            for (k = 0; k < t->nl; k++)
                if (!num[t->leaf[k]])
                    num[t->leaf[k]] = ++ns;
            cand[nc++] = v;
            ci[v] = nc;
        }
    }
    if (!nc) {
        free(num);
        free(cand);
        free(bad);
        free(ci);
        free(xl_t);
        return;
    }
    w = (ns + 32) / 32;
    in = (u32 *)xmalloc(4 * (size_t)nb * w * sizeof(u32));
    out = in + (size_t)nb * w;
    gen = out + (size_t)nb * w;
    def = gen + (size_t)nb * w;
    memset(in, 0, 4 * (size_t)nb * w * sizeof(u32));
    xl_gen = gen;
    xl_in = in;
    xl_w = w;
#define XBIT(set, blk, x) ((set)[(size_t)(blk) * w + (num[x] >> 5)] |= (u32)1 << (num[x] & 31))
    for (b = 1; b < nb; b++) {
        Block *bb = &f->blk[b];
        if (!live[b])
            continue;
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++) {
                u32 x = xrem_leaf(f, f->xargs[f->ins[v].a + k]);
                if (num[x] && live[bb->preds[k]])
                    XBIT(out, bb->preds[k], x);
            }
        xl_block(f, b, 1);
    }
    for (v = 1; v < f->nins; v++)
        if (num[v])
            XBIT(def, f->ins[v].blk, v); /* a value is not alive on the way into the block that makes it */
    /* (out[b] also holds what the phis of its successors read) */
    do {
        ch = 0;
        for (b = nb - 1; b >= 1; b--) {
            Block *bb = &f->blk[b];
            int ns2 = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0, j;
            if (!live[b])
                continue;
            for (j = 0; j < ns2; j++) {
                u32 t = bb->succ[j];
                for (k = 0; k < w; k++) {
                    u32 o = out[(size_t)b * w + k] | in[(size_t)t * w + k];
                    if (o != out[(size_t)b * w + k]) {
                        out[(size_t)b * w + k] = o;
                        ch = 1;
                    }
                }
            }
            for (k = 0; k < w; k++) {
                u32 o = in[(size_t)b * w + k] | ((gen[(size_t)b * w + k] | out[(size_t)b * w + k]) & ~def[(size_t)b * w + k]);
                if (o != in[(size_t)b * w + k]) {
                    in[(size_t)b * w + k] = o;
                    ch = 1;
                }
            }
        }
    } while (ch);
#undef XBIT
    /* the candidates whose every reader stands where both operands are alive */
    memset(bad, 0, (size_t)f->nins + 1);
    for (b = 1; b < nb; b++)
        if (live[b])
            xl_block(f, b, 2);
    for (c = 0; c < nc; c++)
        if (!bad[cand[c]]) {
            xrem[cand[c]] = 2;
            for (k = 0; k < xl_t[c].ni; k++) {
                u32 x = xl_t[c].in[k]; /* read from outside the tree too: kept as well (3) */
                if (xrem[x] != 2)
                    xrem[x] = xl_t[c].cnt[k] == nuse[x] && !xrem[x] ? 2 : 3;
            }
        }
    free(in);
    free(num);
    free(cand);
    free(bad);
    free(ci);
    free(xl_t);
}

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
            if (!i->blk || i->ty != IT_I32 || zero[v] || i->op == OP_PHI)
                continue; /* (a phi's a, b are where its arguments are and how many) */
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

/* ---- loads of neighbouring bytes, put together ----
   b[k] | b[k + 1] << 8 (ufsm's MX scale pairs: gys[pp] | gys[pp + 1] << 8) was two LDS.U8, a shift and an or; nvcc reads
   one LDS.U16 where the address is known to be even (pp = x * NCHP + 2 j) and takes the bytes apart with PRMT where it
   needs them. Here: an or (or a sum) of a load of mem M_8U / M_16U at A and of one of the same kind at A + w shifted
   left by 8 w, both read only there, in one block with no write, call or barrier between them, and A known to be a
   multiple of 2 w (ld_tz: the trailing zero bits of an address from its symbol's alignment, constants, sums, products
   and shifts), becomes one load of twice the width at A, in place of the first; twice over, four bytes are one load.
   MCC_NOLDCOMB: off. */
int opt_noldcomb;

/* the number of low zero bits that address or number v is known to have (64: it is 0) */
static u32 ld_tz(u32 v, int d)
{
    Ins *i;
    u32 a, b, k;
    u64 c;
    v = RV(v);
    i = &PF->ins[v];
    if (d > 16)
        return 0;
    switch (i->op) {
    case OP_CONST:
        c = (u64)i->a | (u64)i->b << 32;
        if (i->ty == IT_I32)
            c &= 0xffffffffU;
        if (!c)
            return 64;
        for (k = 0; !(c & 1); k++)
            c >>= 1;
        return k;
    case OP_ADDR: {
        u32 al = 1;
        for (k = 0; k < nshared; k++)
            if (shared[k].sym == i->a)
                al = shared[k].align;
        for (a = 0; al > 1 && !(al & 1) && a < 16; a++)
            al >>= 1;
        b = i->b ? 0 : 64;
        if (i->b)
            for (c = (u32)i->b; !(c & 1); c >>= 1)
                b++;
        return a < b ? a : b;
    }
    case OP_ADD: case OP_SUB: case OP_OR:
        a = ld_tz(i->a, d + 1);
        b = ld_tz(i->b, d + 1);
        return a < b ? a : b;
    case OP_AND:
        a = ld_tz(i->a, d + 1);
        b = ld_tz(i->b, d + 1);
        return a > b ? a : b;
    case OP_MUL:
        a = ld_tz(i->a, d + 1) + ld_tz(i->b, d + 1);
        return a > 64 ? 64 : a;
    case OP_SHL:
        if (!ir_is_const(PF, RV(i->b), &c))
            return 0;
        a = ld_tz(i->a, d + 1) + (u32)(c & 63);
        return a > 64 ? 64 : a;
    case OP_SEXT32: case OP_ZEXT32: case OP_TRUNC:
        return ld_tz(i->a, d + 1);
    case OP_SELECT:
        a = ld_tz(i->b, d + 1);
        b = ld_tz(i->c, d + 1);
        return a < b ? a : b;
    }
    return 0;
}

/* the address of a load as a base value and a constant offset */
static u32 ld_split(u32 a, i64 *off)
{
    a = RV(a);
    *off = 0;
    if (addr_sum(a, off))
        a = RV(PF->ins[a].a);
    return a;
}

/* nothing between loads x and y (in that order, one block) writes memory or waits */
static int ld_clear(u32 x, u32 y)
{
    u32 v;
    for (v = PF->ins[x].next; v && v != y; v = PF->ins[v].next) {
        int op = PF->ins[v].op;
        if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_ASMOUT || op == OP_XI)
            return 0;
    }
    return v == y;
}

/* the leaves of the or / sum v (its inner nodes read only there): loads of M_8U / M_16U, each perhaps shifted left by a
   constant; 0 when anything else is in it */
#define LD_MAX 4
static int ld_leaves(Func *f, u32 v, const u8 *uses, int root, u32 *ld, u32 *sh, u32 *nodes, u32 *n, u32 *nn)
{
    Ins *i = &f->ins[RV(v)];
    u64 c;
    v = RV(v);
    if ((i->op == OP_OR || i->op == OP_ADD) && i->ty == IT_I32 && (root || uses[v] == 1)) {
        if (*nn >= 2 * LD_MAX)
            return 0;
        nodes[(*nn)++] = v;
        return ld_leaves(f, i->a, uses, 0, ld, sh, nodes, n, nn) && ld_leaves(f, i->b, uses, 0, ld, sh, nodes, n, nn);
    }
    if (*n >= LD_MAX)
        return 0;
    if (i->op == OP_SHL && uses[v] == 1 && ir_is_const(f, RV(i->b), &c) && c < 32 && !(c & 7)) {
        nodes[(*nn)++] = v;
        sh[*n] = (u32)c;
        v = RV(i->a);
        i = &f->ins[v];
    } else {
        sh[*n] = 0;
    }
    if (i->op != OP_LOAD || (i->mem != M_8U && i->mem != M_16U) || uses[v] != 1 || (i->flags & ~(IF_NC | IF_ALIGN)))
        return 0;
    ld[(*n)++] = v;
    return 1;
}

static int ld_combine(Func *f)
{
    u32 v, k, n, *p, b, done = 0;
    u8 *uses;
    if (opt_noldcomb)
        return 0;
    uses = (u8 *)xmalloc((size_t)f->nins + 1);
    memset(uses, 0, f->nins + 1);
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        if (!i->blk)
            continue;
        IR_OPS(f, i, p, n);
        for (k = 0; k < n; k++) {
            u32 x = RV(p[k]);
            if (x && x < f->nins && uses[x] < 255)
                uses[x]++;
        }
    }
    for (b = 1; b < f->nblk; b++)
        if (f->blk[b].term == TERM_BR || f->blk[b].term == TERM_RET) {
            u32 x = RV(f->blk[b].arg);
            if (x && x < f->nins && uses[x] < 255)
                uses[x]++;
        }
    /* roots last made first: an inner or that is itself a whole pair is taken with its tree */
    for (v = f->nins - 1; v >= 1; v--) {
        Ins *i = &f->ins[v];
        u32 ld[LD_MAX], sh[LD_MAX], nodes[2 * LD_MAX + LD_MAX], nl = 0, nn = 0, base = 0, lo = 0, first, last, W = 0, j, bytes = 0;
        i64 off[LD_MAX], omin = 0;
        int ok = 1;
        if (!i->blk || i->ty != IT_I32 || (i->op != OP_OR && i->op != OP_ADD))
            continue;
        if (!ld_leaves(f, v, uses, 1, ld, sh, nodes, &nl, &nn) || nl < 2)
            continue;
        for (j = 0; j < nl && ok; j++) {
            u32 bj = ld_split(f->ins[ld[j]].a, &off[j]);
            Ins *bi = &f->ins[bj];
            if (bi->op == OP_ADDR) { /* (a symbol: its offset is part of the address's constant) */
                off[j] += (i64)(i32)bi->b;
                bj = 0x80000000U | bi->a;
            }
            if (j == 0)
                base = bj;
            ok = bj == base && f->ins[ld[j]].blk == i->blk;
        }
        if (!ok)
            continue;
        for (j = 0; j < nl; j++)
            if (j == 0 || off[j] < omin)
                omin = off[j], lo = j;
        for (j = 0; j < nl && ok; j++) {
            u32 w = M_SIZE(f->ins[ld[j]].mem), m = ((1u << w) - 1) << (u32)(off[j] - omin);
            ok = off[j] - omin < 4 && (i64)sh[j] == 8 * (off[j] - omin) && !(bytes & m);
            bytes |= m;
            W += w;
        }
        if (!ok || (W != 2 && W != 4) || bytes != (1u << W) - 1)
            continue;
        if (ld_tz(f->ins[ld[lo]].a, 0) < (W == 2 ? 1u : 2u))
            continue;
        /* nothing between the first and the last of them writes memory or waits */
        first = last = 0;
        for (j = i->blk ? f->blk[i->blk].first : 0; j; j = f->ins[j].next) {
            u32 t;
            int is = 0;
            for (t = 0; t < nl; t++)
                is |= ld[t] == j;
            if (is) {
                if (!first)
                    first = j;
                last = j;
            }
        }
        if (!first || first == last || !ld_clear(first, last))
            continue;
        f->ins[ld[lo]].mem = W == 2 ? M_16U : M_32;
        for (j = 0; j < nl; j++)
            f->ins[ld[lo]].flags &= f->ins[ld[j]].flags;
        ir_replace(f, v, ld[lo]);
        for (j = 0; j < nl; j++)
            uses[ld[j]] = 0;
        for (j = 0; j < nn; j++)
            uses[nodes[j]] = 0;
        done++;
    }
    free(uses);
    return (int)done;
}

/* ---- loops that run at most once ----
   for (i = threadIdx.x; i < N; i += S) with S >= N (a block of 256 threads loading 256 or fewer things) runs its body
   once or not at all, but its body stayed a loop: rotated, the test of i + S < N at its end kept the work that
   depends only on the thread (the division of i into row and column, the swizzle) inside the loop, and so inside
   the loop around it, where nvcc's front end makes the loop an if and the work leaves the outer loop with the rest
   of what does not change in it. ufsm's s2b streaming kernel: per tap 53 instructions from S2R TID on, -5 % of the
   kernel (11.66 -> 11.10 ms). A loop's latch test "x < N" of x = p + S (S > 0), p the counter's phi, is false when
   the least value p starts with plus S is already at least the most N can be, and p + S cannot wrap; it becomes the
   constant false, and simplify_cfg drops the way back. MCC_NOSTRIP: off. */
int opt_nostrip;
int ptx_single_trip(Func *f)
{
    u32 b, n = f->nins, k;
    int done = 0;
    if (opt_nostrip || opt_norange || !ptx_obj)
        return 0;
    PF = f;
    PO = ptx_obj;
    rg_lo = (i64 *)xmalloc((size_t)(n + 1) * sizeof(i64));
    rg_hi = (i64 *)xmalloc((size_t)(n + 1) * sizeof(i64));
    rg_st = (u8 *)xmalloc((size_t)n + 1);
    memset(rg_st, 0, (size_t)n + 1);
    rg_depth = 0;
    opt_dom(f);
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        Ins *c, *xi, *pi;
        u32 x, nn, p, h, s;
        u64 sc;
        i64 S, ilo = RG_MAX, ihi = RG_MIN, lo, hi, nlo, nhi, plo;
        int op, ok = 1;
        if (bb->term != TERM_BR || bb->succ[0] == bb->succ[1])
            continue;
        c = &f->ins[RV(bb->arg)];
        if (c->op != OP_SLT && c->op != OP_SLE && c->op != OP_ULT && c->op != OP_ULE)
            continue;
        op = c->op;
        x = RV(c->a);
        nn = RV(c->b);
        xi = &f->ins[x];
        if (xi->ty != IT_I32 || xi->op != OP_ADD || !ir_is_const(f, RV(xi->b), &sc) || (i64)(i32)(u32)sc <= 0)
            continue;
        S = (i64)(i32)(u32)sc;
        p = RV(xi->a);
        pi = &f->ins[p];
        if (pi->op != OP_PHI || pi->ty != IT_I32)
            continue;
        h = pi->blk;
        s = bb->succ[0];
        if (s != h || !opt_dominates(h, b))
            continue; /* (the way back when the test holds: a latch of p's loop) */
        for (k = 0; k < pi->b && ok; k++) {
            u32 a = RV(f->xargs[pi->a + k]), pr = f->blk[h].preds[k];
            if (pr == b) {
                ok = a == x;
                continue;
            }
            if (opt_dominates(h, pr)) {
                ok = 0; /* another way back */
                continue;
            }
            rg_of(a, &lo, &hi);
            ilo = lo < ilo ? lo : ilo;
            ihi = hi > ihi ? hi : ihi;
        }
        if (!ok || ilo > ihi)
            continue;
        rg_of(nn, &nlo, &nhi);
        if (op == OP_ULT || op == OP_ULE) {
            if (ilo < 0 || nlo < 0)
                continue;
        }
        /* p is a start value or an x that passed the test: at most max(ihi, nhi) */
        plo = ilo;
        if ((ihi > nhi ? ihi : nhi) + S > RG_MAX)
            continue;
        if (op == OP_SLT || op == OP_ULT ? plo + S >= nhi : plo + S > nhi) {
            bb->arg = ir_const(f, IT_I32, 0);
            done++;
        }
    }
    free(rg_lo);
    free(rg_hi);
    free(rg_st);
    rg_lo = rg_hi = 0;
    rg_st = 0;
    return done;
}

/* ---- vector loads and stores ----
   A struct copy through a pointer is taken apart into its fields (opt_split): a uint4 / float4 copied from global memory was four
   ld.global.u32, a __half2 two ld.global.u16 (and as many stores), where nvcc reads the whole object with one ld.v4.u32 / one 32-bit
   load, since the type promises its alignment (ufsm's fstem_fwd_k: 32 LDG.E.U16 and 16 STG.E.U16 more than nvcc in its MMA loop).
   Loads (stores) of one block, of one base address at consecutive places, n = 2 or 4 of them of one size, are one ld.v<n> (st.v<n>)
   when the first one's address is known to be aligned to the whole (its IF_ALIGN, or what ld_tz finds) and nothing between them
   writes memory (for stores: reads or writes it, or waits). The group's loads are made where its first one is; its stores where its
   last one is. Global / generic and shared memory only. MCC_NOVEC: off. */
int opt_novecmem = -1;
static u32 *vg_mem;  /* per value: 1 + the index in vg_list of its group, for the value that emits it; 0 */
static u8 *vg_skip;  /* per value: a member of a group that another one emits */
static u32 *vg_list; /* groups: n, then the n members by ascending address */
static u32 nvg_list, capvg_list;
static u32 *vg_regs[4]; /* (while groups are found) per register class and number: how many values have it */

/* 0 not to be grouped; 1 shared memory (sh_maddr); 2 the last path of OP_LOAD / OP_STORE (global or generic) */
static int vec_space(u32 v)
{
    Ins *i = &PF->ins[v];
    u32 a = RV(i->a);
    if (i->op == OP_LOAD && special_load(v))
        return 0;
    if (sh_s && sh_base(a))
        return 1;
    {
        i64 o;
        Ins *bi = &PF->ins[ld_split(a, &o)];
        if (bi->op == OP_SLOT || (bi->op == OP_ADDR && special_of(bi->a) >= 0))
            return 0; /* (the frame: local_maddr's, which is known only while the code is written) */
    }
    if ((pm_s && pm_s[a]) || local_maddr(i->a) || (cn_s && cn_s[a]) || (lc_s && lc_s[a]))
        return 0;
    return 2;
}

static int vec_blocks(int op)
{
    return op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_ASMOUT || op == OP_XI ||
           op == OP_VSTORE || op == OP_BYVAL;
}

static void vec_find(Func *f)
{
    u32 b, v;
    if (opt_novecmem < 0)
        opt_novecmem = getenv("MCC_NOVEC") != 0;
    vg_mem = (u32 *)xmalloc(((size_t)f->nins + 1) * sizeof(u32));
    vg_skip = (u8 *)xmalloc((size_t)f->nins + 1);
    memset(vg_mem, 0, ((size_t)f->nins + 1) * sizeof(u32));
    memset(vg_skip, 0, (size_t)f->nins + 1);
    nvg_list = 0;
    if (opt_novecmem)
        return;
    for (b = 0; b < 4; b++) {
        vg_regs[b] = (u32 *)xmalloc(((size_t)nreg[b] + 2) * sizeof(u32));
        memset(vg_regs[b], 0, ((size_t)nreg[b] + 2) * sizeof(u32));
    }
    for (v = 1; v < f->nins; v++)
        if (regno[v] && f->ins[v].ty != IT_VOID && regno[v] <= nreg[cls_of(f->ins[v].ty)] + 1)
            vg_regs[cls_of(f->ins[v].ty)][regno[v]]++;
    for (b = 1; b < f->nblk; b++) {
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 m[4], base, x, n = 1, sz, k, want, tz, last, steps = 0;
            i64 off, o2;
            int sp, ld = i->op == OP_LOAD;
            if ((i->op != OP_LOAD && i->op != OP_STORE) || vg_skip[v] || i->mem > M_F64)
                continue;
            sz = M_SIZE(i->mem);
            base = ld_split(i->a, &off);
            sp = vec_space(v);
            if (!sp)
                continue;
            m[0] = v;
            for (x = i->next; x && n < 4 && steps < 64; x = f->ins[x].next, steps++) {
                Ins *xi = &f->ins[x];
                if (xi->op == i->op && !vg_skip[x] && xi->mem <= M_F64 && M_SIZE(xi->mem) == sz &&
                    (sz == 4 || sz == 8 || xi->mem == i->mem) && (xi->flags & IF_NC) == (i->flags & IF_NC) &&
                    ld_split(xi->a, &o2) == base && o2 == off + (i64)(n * sz) && vec_space(x) == sp &&
                    (!ld || !gl_s || gl_s[RV(xi->a)] == gl_s[RV(i->a)]) && (ld || !gl_s || !gl_s[RV(xi->a)] == !gl_s[RV(i->a)])) {
                    m[n++] = x;
                    continue;
                }
                if (vec_blocks(xi->op) || (!ld && xi->op == OP_LOAD))
                    break;
            }
            if (n == 3)
                n = 2;
            /* the alignment: what the address sum shows, or what the type of the copied object promises */
            tz = ld_tz(i->a, 0);
            if ((i->flags & IF_ALIGN) > tz)
                tz = i->flags & IF_ALIGN;
            for (; n >= 2; n /= 2) {
                want = n * sz;
                if (want > 16)
                    continue;
                for (k = 0; (1u << k) < want; k++)
                    ;
                if (tz >= k)
                    break;
            }
            if (n < 2)
                continue;
            /* each member's value (a load's result, a store's operand) must have a register of its own: its life now starts
               earlier (loads) or ends later (stores), and coalesce lets a loop's phi share one with the value it takes back */
            for (k = 0; k < n; k++) {
                u32 w = ld ? m[k] : RV(f->ins[m[k]].b);
                if (f->ins[w].op != OP_CONST && f->ins[w].op != OP_FCONST && regno[w] && vg_regs[cls_of(f->ins[w].ty)][regno[w]] != 1)
                    break;
            }
            if (k < n)
                continue;
            /* a store's values must have registers: the group is emitted at its last store, by then all are known */
            last = m[n - 1];
            if (nvg_list + 5 > capvg_list) {
                capvg_list = capvg_list ? capvg_list * 2 : 256;
                vg_list = (u32 *)xrealloc(vg_list, capvg_list * sizeof(u32));
            }
            vg_list[nvg_list] = n;
            for (k = 0; k < n; k++) {
                vg_list[nvg_list + 1 + k] = m[k];
                vg_skip[m[k]] = 1;
            }
            vg_mem[ld ? v : last] = nvg_list + 1;
            vg_skip[ld ? v : last] = 0;
            nvg_list += 1 + n;
        }
    }
    for (b = 0; b < 4; b++) {
        free(vg_regs[b]);
        vg_regs[b] = 0;
    }
}

/* the group that value v emits: its vector instruction; 0 if v emits none */
static int vec_emit(u32 v)
{
    static const char *const lt[] = {"s8", "u8", "s16", "u16", "b32", "b64", "b32", "b64"};
    Ins *i = &PF->ins[v];
    u32 g, n, k, first;
    const char *a, *sp;
    char ops[160];
    size_t o = 0;
    if (!vg_mem || !vg_mem[v])
        return 0;
    g = vg_mem[v] - 1;
    n = vg_list[g];
    first = vg_list[g + 1];
    for (k = 0; k < n; k++) {
        u32 x = vg_list[g + 1 + k];
        const char *r;
        if (i->op == OP_LOAD) {
            r = reg(x);
        } else {
            u32 val = PF->ins[x].b;
            r = ropnd(val);
            if (r[0] != '%') { /* (a vector's elements are registers) */
                const char *t = temp(cls_of(PF->ins[RV(val)].ty));
                P("\tmov.%s %s, %s;\n", PF->ins[RV(val)].ty == IT_F32 ? "f32" : PF->ins[RV(val)].ty == IT_F64 ? "f64" :
                                         PF->ins[RV(val)].ty == IT_I64 ? "b64" : "b32", t, r);
                r = t;
            }
        }
        o += (size_t)sprintf(ops + o, "%s%.30s", k ? ", " : "", r);
    }
    if ((a = sh_maddr(PF->ins[first].a)) != 0) {
        sp = "shared.";
    } else {
        a = maddr(PF->ins[first].a);
        if (i->op == OP_LOAD)
            sp = (i->flags & IF_NC) && !opt_nonc ? "global.nc." : gl_s && gl_s[RV(i->a)] ? gl_s[RV(i->a)] == 2 ? "global.nc." : "global." : "";
        else
            sp = gl_s && gl_s[RV(PF->ins[first].a)] ? "global." : "";
    }
    if (i->op == OP_LOAD)
        P("\tld.%sv%u.%s {%s}, [%s];\n", sp, n, lt[i->mem], ops, a);
    else
        P("\tst.%sv%u.%s [%s], {%s};\n", sp, n, lt[i->mem], a, ops);
    return 1;
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
    addr_split(f);
    if (ld_combine(f)) {
        /* (twice: a pair of halves made of pairs of bytes) */
        f->copies = 1;
        ir_resolve_all(f);
        if (ld_combine(f)) {
            f->copies = 1;
            ir_resolve_all(f);
        }
        opt_dce(f);
    }
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
    nuse = (u8 *)xmalloc(8 * (size_t)(f->nins + 1));
    memset(nuse, 0, 8 * (size_t)(f->nins + 1));
    wuse = nuse + 7 * (size_t)(f->nins + 1);
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
    if (opt_noshmask < 0)
        opt_noshmask = getenv("MCC_NOSHMASK") != 0;
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
            if ((i->op == OP_SHL || i->op == OP_SHR || i->op == OP_SAR) && !opt_noshmask) {
                /* a shift reads a count made wider as it was (the shift's case in emit_ins) */
                u32 r = RV(i->b);
                if ((f->ins[r].op == OP_ZEXT32 || f->ins[r].op == OP_SEXT32) && wuse[r] < 255)
                    wuse[r]++;
            }
            if ((i->op == OP_MUL || i->op == OP_SHL) && i->ty == IT_I64) {
                u32 wx, wy;
                if (wide_kind(v, &wx, &wy)) {
                    u32 r = RV(i->a);
                    if (wuse[r] < 255)
                        wuse[r]++;
                    if (wy && (r = RV(i->b)) != 0 && wuse[r] < 255)
                        wuse[r]++;
                }
            }
            if (i->op == OP_CALL && !(i->flags & (IF_ASM | IF_ALLOCA)) && f->ins[RV(i->c)].op != OP_ADDR)
                mark(i->c);
        }
        if (bb->arg)
            mark(bb->arg);
        if (bb->term == TERM_BR)
            mark_cond(bb->arg);
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
    /* (after the trees: what a tree reads is read where the tree is worked out; and after the addresses in shared
       memory, which are written in their own way and left alone) */
    xrem = 0;
    if (opt_noxrem != 1) {
        xrem = (u8 *)xmalloc((size_t)f->nins + 1);
        memset(xrem, 0, (size_t)f->nins + 1);
        xc_seg = (u32 *)xmalloc((size_t)(f->nins + 1) * sizeof(u32));
        memset(xc_seg, 0, (size_t)(f->nins + 1) * sizeof(u32));
        xc_name = (char (*)[24])xmalloc((size_t)(f->nins + 1) * 24);
        xseg = 1;
        for (b = 1; b < nb; b++) {
            Block *bb = &f->blk[b];
            if (!live[b])
                continue;
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                IR_OPS(f, i, p, n);
                for (k = 0; k < n; k++)
                    xrem_mark(f, p[k], b, 0);
            }
            if (bb->arg)
                xrem_mark(f, bb->arg, b, 0);
        }
        if (opt_noxrem < 2)
            xrem_live(f, live);
    }

    lc_s = 0;
    if (!opt_nolocal) {
        lc_s = (u8 *)xmalloc((size_t)f->nins + 1);
        lc_find(f);
    }
    cn_s = (u8 *)xmalloc((size_t)f->nins + 1);
    cn_find(f);
    pm_h = 0;
    pm_s = 0;
    if (entry && !opt_noparm) {
        pm_h = (int *)xmalloc(((size_t)f->nslots + 1) * sizeof(int));
        pm_s = (u8 *)xmalloc((size_t)f->nins + 1);
        pm_find(f);
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
    coalesce_join(live);
    vec_find(f);
    opt_dom(f); /* (moves_early asks which edges are ways back) */

    /* the frame */
    for (k = 0; k < f->nslots; k++)
        slotoff[k] = 0xffffffffU;
    for (v = 1; v < f->nins; v++)
        if (used[v] && f->ins[v].op == OP_SLOT && slotoff[f->ins[v].a] == 0xffffffffU && !(pm_h && pm_h[f->ins[v].a] >= 0)) {
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
    if (entry && ptx_maxnreg)
        BP(&head, "\n.maxnreg %u", ptx_maxnreg);
    /* (sm_90 on: clusters; for an older target the kernel is built without, as one block per cluster is) */
    if (entry && ptx_cluster && atoi(opt_cuda_target + 3) >= 90)
        BP(&head, "\n.reqnctapercluster %u, %u, %u", ptx_cluster & 0xff, ptx_cluster >> 8 & 0xff, ptx_cluster >> 16 & 0xff);
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
    free(vg_mem);
    free(vg_skip);
    vg_mem = 0;
    vg_skip = 0;
    free(xrem);
    xrem = 0;
    free(xc_seg);
    free(xc_name);
    xc_seg = 0;
    xc_name = 0;
    free(lc_s);
    lc_s = 0;
    free(cn_s);
    cn_s = 0;
    free(pm_h);
    free(pm_s);
    pm_h = 0;
    pm_s = 0;
    gl_s = 0;
    free(slotoff);
    free(byoff);
    free(live);
    free(stack);
}

/* Puts bytes into a section and returns a new symbol for them. */
/* ---- packing the module text (cuda/include/mcuda_pack.h has the layout and the unpacker) ----
   LZ77 over the whole unit: two hash tables, one keyed by 8 bytes (chains of depth PK_DEPTH) and one by 32 bytes
   at every 8th position, which finds the long repeats of one template's instantiations however far back they
   are; greedy with one step of lazy matching. Measured on lp_f4fwd_i0p (18.7 MB of PTX): 5.3x in 0.37 s (depth
   4); depth 1 5.1x in 0.28 s, depth 16 5.5x in 0.92 s; zstd -1 6.4x (a 128 KB window: 2.4x less there). */
#define PK_DEPTH 4
#define PK_MINM 6 /* MCUDA_PACK_MINM */
static u64 pk_ld8(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static u32 pk_h8(const u8 *p, int hb) { return (u32)(((pk_ld8(p) << 16) * ((u64)0x9E3779B9u << 32 | 0x7F4A7C15u)) >> (64 - hb)); }
static u32 pk_h32(const u8 *p, int hb)
{
    u64 v = pk_ld8(p) ^ pk_ld8(p + 8) * 31 ^ pk_ld8(p + 16) * 1009 ^ pk_ld8(p + 24) * 65537;
    return (u32)((v * ((u64)0x9E3779B9u << 32 | 0x7F4A7C15u)) >> (64 - hb));
}
static size_t pk_len(const u8 *a, const u8 *b, const u8 *e)
{
    const u8 *s = b;
    while (b + 8 <= e && pk_ld8(a) == pk_ld8(b))
        a += 8, b += 8;
    while (b < e && *a == *b)
        a++, b++;
    return (size_t)(b - s);
}
typedef struct {
    const u8 *in;
    size_t n;
    int hb;
    u32 *h8, *h32, *chain;
} Pk;
static void pk_best(Pk *k, size_t i, size_t *bl, size_t *bo)
{
    u32 c;
    int d;
    *bl = 0, *bo = 0;
    if (i + 32 <= k->n && (c = k->h32[pk_h32(k->in + i, k->hb)]) != 0xffffffffu) {
        size_t l = pk_len(k->in + c, k->in + i, k->in + k->n);
        if (l > *bl)
            *bl = l, *bo = i - c;
    }
    for (c = k->h8[pk_h8(k->in + i, k->hb)], d = 0; d < PK_DEPTH && c != 0xffffffffu; d++, c = k->chain[c]) {
        size_t l = pk_len(k->in + c, k->in + i, k->in + k->n);
        if (l > *bl)
            *bl = l, *bo = i - c;
    }
}
static void pk_ins(Pk *k, size_t i)
{
    u32 h = pk_h8(k->in + i, k->hb);
    k->chain[i] = k->h8[h];
    k->h8[h] = (u32)i;
    if (i + 32 <= k->n && (i & 7) == 0)
        k->h32[pk_h32(k->in + i, k->hb)] = (u32)i;
}
/* out = "\0MPZ", n (u64), m (u64), P (u64), in[0..P) plain (the declarations), the tokens (m bytes) of in[P..n) */
static void ptx_pack(Buf *out, const u8 *in, size_t n, size_t pro)
{
    Pk k;
    size_t lit = pro, i = 0, bl, bo, l2, o2, at, j;
    int hb = 10;
    while (hb < 22 && ((size_t)1 << hb) < n)
        hb++;
    k.in = in, k.n = n, k.hb = hb;
    k.h8 = (u32 *)xmalloc(sizeof(u32) << hb);
    k.h32 = (u32 *)xmalloc(sizeof(u32) << hb);
    k.chain = (u32 *)xmalloc(sizeof(u32) * (n + 1));
    memset(k.h8, 0xff, sizeof(u32) << hb);
    memset(k.h32, 0xff, sizeof(u32) << hb);
    buf_put(out, "\0MPZ", 4);
    buf_u64(out, (u64)n);
    at = out->len;
    buf_u64(out, 0);
    buf_u64(out, (u64)pro);
    buf_put(out, in, pro);
    for (; i < pro && i + 8 <= n; i++) /* (the bodies may copy from the declarations) */
        pk_ins(&k, i);
    i = pro;
    while (i + 8 <= n) {
        pk_best(&k, i, &bl, &bo);
        if (bl < PK_MINM) {
            pk_ins(&k, i++);
            continue;
        }
        pk_ins(&k, i);
        if (i + 9 <= n) {
            pk_best(&k, i + 1, &l2, &o2);
            if (l2 > bl + 1) { /* (lazy: a longer match one byte on) */
                i++;
                continue;
            }
        }
        buf_uleb(out, (u64)(i - lit));
        buf_put(out, in + lit, i - lit);
        buf_uleb(out, (u64)(bl - PK_MINM));
        buf_uleb(out, (u64)bo);
        for (j = i + 1; j < i + bl && j + 8 <= n; j++)
            pk_ins(&k, j);
        i += bl;
        lit = i;
    }
    buf_uleb(out, (u64)(n - lit));
    buf_put(out, in + lit, n - lit);
    put_u64le(out->p + at, (u64)(out->len - at - 16 - pro));
    free(k.h8);
    free(k.h32);
    free(k.chain);
}

/* ---- the MPY form (cuda/include/mcuda_pack.h has the layout and the decoder) ----
   LZ77 with three repeat offsets, then canonical prefix codes for literals, literal counts, copy lengths and
   offsets. PTX instantiations of one template differ in a few constants and register numbers, so a copy breaks
   off and resumes at the same distance: a repeat offset costs ~1-3 bits where a new one costs ~25. Prices (1/16
   bit) come from a greedy parse of the first MPY_SAMPLE bytes, then the whole text is parsed once with them:
   --cuda-ptx-pack=2 greedily (one step of lazy matching; a repeat of MPY_REPTAKE bytes is taken at once, chains
   of depth 1), --cuda-ptx-pack=3 optimally (forward dynamic programming over windows of MPY_OPTW positions; a copy
   of MPY_NICE bytes ends a window; chains of depth 4). Measured on ufsm's 124 release units (337.6 MB of PTX):
   MPZ 87.9 MB, pack 2 43.5 MB, pack 3 34.2 MB (zstd -9 --long 37.3, -19 28.9); lp_f4fwd_i0p (18.6 MB) pack 2
   ~1.9 G cycles (MPZ ~1.8 G), pack 3 ~10 G. The decoder reads ~1 GB/s. */
#define MPY_MINM 3 /* MCUDA_MPY_MINM */
#define MPY_MAXL 11
#define MPY_NLIT 256
#define MPY_NLEN 88
#define MPY_NOF 91
#define MPY_SAMPLE (2u << 20)
#define MPY_REPTAKE 16
#define MPY_OPTW 1024
#define MPY_NICE 128
#define MPY_INF 0x7fffffffffffffffL

typedef struct {
    u32 ll, ml;
    int oc; /* 0..2 a repeat offset, 3 a new one */
    u64 off;
} MpySeq;
typedef struct {
    int lit[MPY_NLIT], ll[MPY_NLEN], ml[MPY_NLEN], of[MPY_NOF];
} MpyPrice;
typedef struct {
    const u8 *in;
    size_t n;
    int hb, depth;
    u32 *h, *h32, *chain;
    MpySeq *s;
    size_t ns, cap;
} Mpy;

/* the bucket code of v, its count of plain bits nb and their value ex */
static int mpy_bk(u64 v, int *nb, u64 *ex)
{
    int b = 4;
    if (v < 16) {
        *nb = 0, *ex = 0;
        return (int)v;
    }
    while (b < 63 && (v >> (b + 1)) != 0)
        b++;
    *nb = b - 1;
    *ex = v & (((u64)1 << (b - 1)) - 1);
    return 16 + 2 * (b - 4) + (int)((v >> (b - 1)) & 1);
}
static int mpy_pv(const int *t, u64 v)
{
    int nb;
    u64 ex;
    int c = mpy_bk(v, &nb, &ex);
    return t[c] + nb * 16;
}

/* lengths of a prefix code of at most MPY_MAXL bits for freq[0..n) (n <= 256): Huffman's, with the counts halved
   until it fits */
static void mpy_lens(const u64 *freq, int n, u8 *len)
{
    u64 f[MPY_NLIT], w[2 * MPY_NLIT];
    int par[2 * MPY_NLIT], alive[2 * MPY_NLIT], sym[MPY_NLIT], used = 0, i;
    for (i = 0; i < n; i++) {
        f[i] = freq[i];
        len[i] = 0;
        if (f[i])
            sym[used++] = i;
    }
    if (used == 0)
        return;
    if (used == 1) {
        len[sym[0]] = 1;
        return;
    }
    for (;;) {
        int nn = used, k, ok = 1;
        for (i = 0; i < used; i++)
            w[i] = f[sym[i]], alive[i] = 1, par[i] = -1;
        for (k = 0; k < used - 1; k++) {
            int a = -1, b = -1, j;
            for (j = 0; j < nn; j++)
                if (alive[j]) {
                    if (a < 0 || w[j] < w[a])
                        b = a, a = j;
                    else if (b < 0 || w[j] < w[b])
                        b = j;
                }
            w[nn] = w[a] + w[b], alive[nn] = 1, par[nn] = -1;
            alive[a] = alive[b] = 0, par[a] = par[b] = nn;
            nn++;
        }
        for (i = 0; i < used; i++) {
            int d = 0, j = i;
            while (par[j] >= 0)
                j = par[j], d++;
            len[sym[i]] = (u8)d;
            if (d > MPY_MAXL)
                ok = 0;
        }
        if (ok)
            return;
        for (i = 0; i < used; i++)
            f[sym[i]] = (f[sym[i]] + 1) / 2;
    }
}
/* the codes of lengths len[], bit-reversed (the stream is least significant bit first) */
static void mpy_codes(const u8 *len, int nsym, u32 *code)
{
    int cnt[MPY_MAXL + 1], next[MPY_MAXL + 1], s, l, c = 0;
    memset(cnt, 0, sizeof cnt);
    for (s = 0; s < nsym; s++)
        cnt[len[s]]++;
    cnt[0] = 0;
    for (l = 1; l <= MPY_MAXL; l++) {
        c = (c + cnt[l - 1]) << 1;
        next[l] = c;
    }
    for (s = 0; s < nsym; s++)
        if (len[s]) {
            int v = next[len[s]]++, r = 0, k;
            for (k = 0; k < len[s]; k++)
                r |= ((v >> k) & 1) << (len[s] - 1 - k);
            code[s] = (u32)r;
        }
}
static void mpy_price(MpyPrice *P, const u8 *l0, const u8 *l1, const u8 *l2, const u8 *l3)
{
    int i;
    for (i = 0; i < MPY_NLIT; i++)
        P->lit[i] = (l0[i] ? l0[i] : 14) * 16;
    for (i = 0; i < MPY_NLEN; i++)
        P->ll[i] = (l1[i] ? l1[i] : 14) * 16, P->ml[i] = (l2[i] ? l2[i] : 14) * 16;
    for (i = 0; i < MPY_NOF; i++)
        P->of[i] = (l3[i] ? l3[i] : 14) * 16;
}

static size_t mpy_len(const u8 *a, const u8 *b, const u8 *e)
{
    const u8 *s = b;
    while (b + 8 <= e && pk_ld8(a) == pk_ld8(b))
        a += 8, b += 8;
    while (b < e && *a == *b)
        a++, b++;
    return (size_t)(b - s);
}
static void mpy_ins(Mpy *m, size_t i)
{
    u32 h;
    if (i + 8 > m->n)
        return;
    h = pk_h8(m->in + i, m->hb);
    m->chain[i] = m->h[h];
    m->h[h] = (u32)i;
    if (i + 32 <= m->n && (i & 7) == 0)
        m->h32[pk_h32(m->in + i, m->hb)] = (u32)i;
}
static void mpy_seq(Mpy *m, size_t ll, size_t ml, int oc, u64 off)
{
    if (m->ns == m->cap) {
        m->cap = m->cap ? 2 * m->cap : 1024;
        m->s = (MpySeq *)xrealloc(m->s, m->cap * sizeof *m->s);
    }
    m->s[m->ns].ll = (u32)ll, m->s[m->ns].ml = (u32)ml, m->s[m->ns].oc = oc, m->s[m->ns].off = off;
    m->ns++;
}
static void mpy_rep(u64 *r, int oc, u64 off)
{
    u64 o = oc < 3 ? r[oc] : off;
    if (oc == 0)
        return;
    if (oc != 1) /* (a repeat 1 swaps with repeat 0; repeat 2 and a new offset push the others back) */
        r[2] = r[1];
    r[1] = r[0];
    r[0] = o;
}
static void mpy_start(Mpy *m, const u8 *in, size_t n, int depth)
{
    int hb = 10;
    while (hb < 24 && ((size_t)1 << hb) < n)
        hb++;
    m->in = in, m->n = n, m->hb = hb, m->depth = depth;
    m->h = (u32 *)xmalloc(sizeof(u32) << hb);
    m->h32 = (u32 *)xmalloc(sizeof(u32) << hb);
    m->chain = (u32 *)xmalloc(sizeof(u32) * (n + 1));
    memset(m->h, 0xff, sizeof(u32) << hb);
    memset(m->h32, 0xff, sizeof(u32) << hb);
    m->s = 0, m->ns = m->cap = 0;
}
static void mpy_stop(Mpy *m)
{
    free(m->h);
    free(m->h32);
    free(m->chain);
}

/* ---- the greedy parse ---- */
typedef struct {
    size_t l;
    int oc;
    u64 off;
    long g; /* what it saves over literals, 1/16 bit */
} MpyCand;
static void mpy_best(Mpy *m, const MpyPrice *P, const u32 *cum, size_t i, const u64 *rep, MpyCand *c)
{
    const u8 *in = m->in, *e = in + m->n;
    int r, d;
    u32 x;
    c->l = 0, c->g = 0, c->oc = 0, c->off = 0;
    for (r = 0; r < 3; r++)
        if (rep[r] <= i) {
            size_t l = mpy_len(in + i - rep[r], in + i, e);
            long g;
            if (l < MPY_MINM)
                continue;
            g = (long)(u32)(cum[i + l] - cum[i]) - mpy_pv(P->ml, l - MPY_MINM) - P->of[r];
            if (g > c->g)
                c->g = g, c->l = l, c->oc = r, c->off = rep[r];
        }
    if (i + 8 > m->n || c->l >= MPY_REPTAKE)
        return;
    if (i + 32 <= m->n && (x = m->h32[pk_h32(in + i, m->hb)]) != 0xffffffffu && x < i) {
        size_t l = mpy_len(in + x, in + i, e);
        long g = l < MPY_MINM ? 0 : (long)(u32)(cum[i + l] - cum[i]) - mpy_pv(P->ml, l - MPY_MINM) - mpy_pv(P->of + 3, i - x);
        if (g > c->g)
            c->g = g, c->l = l, c->oc = 3, c->off = i - x;
    }
    for (x = m->h[pk_h8(in + i, m->hb)], d = 0; d < m->depth && x != 0xffffffffu; d++, x = m->chain[x]) {
        size_t l = mpy_len(in + x, in + i, e);
        long g = l < MPY_MINM ? 0 : (long)(u32)(cum[i + l] - cum[i]) - mpy_pv(P->ml, l - MPY_MINM) - mpy_pv(P->of + 3, i - x);
        if (g > c->g)
            c->g = g, c->l = l, c->oc = 3, c->off = i - x;
    }
    if (c->oc == 3)
        for (r = 0; r < 3; r++)
            if (c->off == rep[r])
                c->oc = r;
}
/* the sequences of in[pro..n) into m->s */
static void mpy_greedy(Mpy *m, size_t pro, const MpyPrice *P)
{
    const u8 *in = m->in;
    size_t n = m->n, i, lit = pro;
    u64 rep[3];
    u32 *cum = (u32 *)xmalloc(sizeof(u32) * (n + 1)); /* (literal prices summed, mod 2^32: differences are exact) */
    rep[0] = 1, rep[1] = 2, rep[2] = 3;
    cum[0] = 0;
    for (i = 0; i < n; i++)
        cum[i + 1] = cum[i] + (u32)P->lit[in[i]];
    for (i = 0; i < pro; i++) /* (the bodies may copy from the declarations) */
        mpy_ins(m, i);
    i = pro;
    while (i < n) {
        MpyCand c, c1;
        size_t j;
        mpy_best(m, P, cum, i, rep, &c);
        mpy_ins(m, i);
        if (c.l == 0) {
            i++;
            continue;
        }
        if (i + 1 < n && !(c.oc < 3 && c.l >= MPY_REPTAKE)) {
            mpy_best(m, P, cum, i + 1, rep, &c1); /* (lazy: a better copy one byte on) */
            if (c1.l && c1.g - P->lit[in[i]] > c.g) {
                i++;
                continue;
            }
        }
        mpy_seq(m, i - lit, c.l, c.oc, c.off);
        mpy_rep(rep, c.oc, c.off);
        for (j = i + 1; j < i + c.l; j++)
            mpy_ins(m, j);
        i += c.l;
        lit = i;
    }
    free(cum);
}

/* ---- the optimal parse ----
   Node j of a window is position i0 + j: its price, and the step that reached it (a literal, len 0, or a copy of
   len from node from); its literal run and repeat offsets are filled in from that step when the node is reached in
   order. A copy candidate relaxes only the lengths the cheaper candidates before it did not reach (repeat 0, 1, 2,
   then chain matches nearest first). */
typedef struct {
    u32 len, litrun;
    int oc, from;
    u64 off, rep[3];
} MpyNode;
static void mpy_opt(Mpy *m, size_t pro, const MpyPrice *P)
{
    const u8 *in = m->in;
    size_t n = m->n, i0, lit = pro;
    int W = MPY_OPTW + MPY_NICE + 8, j;
    MpyNode *nd = (MpyNode *)xmalloc(sizeof(MpyNode) * (size_t)W);
    long *pr = (long *)xmalloc(sizeof(long) * (size_t)W);
    int *mlp = (int *)xmalloc(sizeof(int) * (size_t)W), *path = (int *)xmalloc(sizeof(int) * (size_t)W);
    u64 rep[3];
    rep[0] = 1, rep[1] = 2, rep[2] = 3;
    for (j = 0; j < W; j++)
        mlp[j] = j >= MPY_MINM ? mpy_pv(P->ml, (u64)(j - MPY_MINM)) : 0;
    for (i0 = 0; i0 < pro; i0++)
        mpy_ins(m, i0);
    i0 = pro;
    while (i0 < n) {
        int e = -1, lj = -1, reach = 0, np, q;
        size_t lL = 0;
        int lOC = 0;
        u64 lOFF = 0;
        pr[0] = 0;
        nd[0].litrun = (u32)(i0 - lit), nd[0].from = -1, nd[0].len = 0;
        nd[0].rep[0] = rep[0], nd[0].rep[1] = rep[1], nd[0].rep[2] = rep[2];
        for (j = 0;; j++) {
            size_t p = i0 + (size_t)j, best = MPY_MINM - 1, cl[40], done = MPY_MINM - 1;
            const u8 *ce = in + (p + MPY_NICE < n ? p + MPY_NICE : n);
            int co[40], nc = 0, r, d, k;
            u64 cf[40];
            u32 x;
            long base;
            MpyNode *N = &nd[j];
            if (j > 0) {
                MpyNode *F = &nd[N->from];
                N->rep[0] = F->rep[0], N->rep[1] = F->rep[1], N->rep[2] = F->rep[2];
                if (N->len == 0)
                    N->litrun = F->litrun + 1;
                else
                    N->litrun = 0, mpy_rep(N->rep, N->oc, N->off);
            }
            if (p >= n || j >= MPY_OPTW) {
                e = j;
                break;
            }
            while (reach < j + 1)
                pr[++reach] = MPY_INF;
            if (pr[j] + P->lit[in[p]] < pr[j + 1]) {
                pr[j + 1] = pr[j] + P->lit[in[p]];
                nd[j + 1].len = 0, nd[j + 1].from = j;
            }
            for (r = 0; r < 3; r++)
                if (N->rep[r] <= p) {
                    size_t l = mpy_len(in + p - N->rep[r], in + p, ce);
                    if (l >= MPY_MINM)
                        cl[nc] = l, co[nc] = r, cf[nc] = N->rep[r], nc++;
                }
            if (p + 8 <= n) {
                size_t rb = 0;
                for (k = 0; k < nc; k++)
                    if (cl[k] > rb)
                        rb = cl[k];
                for (x = m->h[pk_h8(in + p, m->hb)], d = 0; d < m->depth && x != 0xffffffffu; d++, x = m->chain[x]) {
                    size_t l = mpy_len(in + x, in + p, ce);
                    if (l > best)
                        best = l, cl[nc] = l, co[nc] = 3, cf[nc] = p - x, nc++;
                }
                if (best > rb)
                    rb = best;
                if (rb < 32 && p + 32 <= n && (x = m->h32[pk_h32(in + p, m->hb)]) != 0xffffffffu && x < p) {
                    size_t l = mpy_len(in + x, in + p, ce);
                    if (l > rb)
                        cl[nc] = l, co[nc] = 3, cf[nc] = p - x, nc++;
                }
            }
            mpy_ins(m, p);
            for (k = 0; k < nc; k++)
                if (co[k] == 3)
                    for (r = 0; r < 3; r++)
                        if (cf[k] == N->rep[r])
                            co[k] = r;
            for (k = 0; k < nc; k++)
                if (cl[k] >= MPY_NICE && cl[k] > lL)
                    lL = cl[k], lOC = co[k], lOFF = cf[k], lj = j;
            if (lj >= 0) { /* a long copy here: the window ends, the copy is taken after it */
                e = j;
                break;
            }
            base = pr[j] + mpy_pv(P->ll, N->litrun);
            for (k = 0; k < nc; k++) {
                size_t l, L = cl[k];
                long ofp = co[k] < 3 ? P->of[co[k]] : mpy_pv(P->of + 3, cf[k]);
                if ((size_t)j + L > (size_t)(MPY_OPTW + MPY_NICE))
                    L = (size_t)(MPY_OPTW + MPY_NICE - j);
                while ((size_t)reach < (size_t)j + L)
                    pr[++reach] = MPY_INF;
                for (l = done + 1; l <= L; l++) {
                    long c = base + mlp[l] + ofp;
                    if (c < pr[(size_t)j + l]) {
                        MpyNode *T = &nd[(size_t)j + l];
                        pr[(size_t)j + l] = c;
                        T->len = (u32)l, T->oc = co[k], T->off = cf[k], T->from = j;
                    }
                }
                if (L > done)
                    done = L;
            }
        }
        for (np = 0, q = e; q > 0; q = nd[q].from)
            path[np++] = q;
        for (q = np - 1; q >= 0; q--) {
            MpyNode *T = &nd[path[q]];
            if (T->len) {
                size_t at = i0 + (size_t)T->from;
                mpy_seq(m, at - lit, T->len, T->oc, T->off);
                lit = at + T->len;
            }
        }
        rep[0] = nd[e].rep[0], rep[1] = nd[e].rep[1], rep[2] = nd[e].rep[2];
        i0 += (size_t)e;
        if (lj >= 0) {
            size_t k2;
            int oc = lOC, r;
            lL = mpy_len(in + i0 - lOFF, in + i0, in + n); /* (seen capped at MPY_NICE) */
            if (oc == 3)
                for (r = 0; r < 3; r++)
                    if (lOFF == rep[r])
                        oc = r;
            mpy_seq(m, i0 - lit, lL, oc, lOFF);
            mpy_rep(rep, oc, lOFF);
            for (k2 = i0 + 1; k2 < i0 + lL; k2++)
                mpy_ins(m, k2);
            i0 += lL;
            lit = i0;
        }
    }
    free(nd);
    free(pr);
    free(mlp);
    free(path);
}

static void mpy_stats(const u8 *in, size_t n, size_t pro, const MpySeq *s, size_t ns, u64 *f0, u64 *f1, u64 *f2, u64 *f3)
{
    size_t k, i = pro, j;
    int nb;
    u64 ex;
    memset(f0, 0, MPY_NLIT * sizeof *f0);
    memset(f1, 0, MPY_NLEN * sizeof *f1);
    memset(f2, 0, MPY_NLEN * sizeof *f2);
    memset(f3, 0, MPY_NOF * sizeof *f3);
    for (k = 0; k < ns; k++) {
        for (j = 0; j < s[k].ll; j++)
            f0[in[i + j]]++;
        i += s[k].ll + s[k].ml;
        f1[mpy_bk(s[k].ll, &nb, &ex)]++;
        f2[mpy_bk(s[k].ml - MPY_MINM, &nb, &ex)]++;
        f3[s[k].oc < 3 ? s[k].oc : 3 + mpy_bk(s[k].off, &nb, &ex)]++;
    }
    for (; i < n; i++)
        f0[in[i]]++;
}

typedef struct {
    Buf *o;
    u64 b;
    int n;
} MpyBits;
static void mpy_put(MpyBits *w, u64 v, int k)
{
    if (k > 32) {
        mpy_put(w, v & 0xffffffffu, 32);
        mpy_put(w, v >> 32, k - 32);
        return;
    }
    w->b |= (v & (((u64)1 << k) - 1)) << w->n;
    w->n += k;
    while (w->n >= 8) {
        buf_u8(w->o, (u32)(w->b & 255));
        w->b >>= 8;
        w->n -= 8;
    }
}

/* out = "\0MPY", n, m, P, in[0..P) plain, the coded body (m bytes); level 2 greedy, 3 optimal */
static void ptx_pack_mpy(Buf *out, const u8 *in, size_t n, size_t pro, int level)
{
    Mpy m;
    MpyPrice P;
    u64 f0[MPY_NLIT], f1[MPY_NLEN], f2[MPY_NLEN], f3[MPY_NOF];
    u8 l0[MPY_NLIT], l1[MPY_NLEN], l2[MPY_NLEN], l3[MPY_NOF];
    u32 c0[MPY_NLIT], c1[MPY_NLEN], c2[MPY_NLEN], c3[MPY_NOF];
    size_t k, i, at, sn = n;
    const u8 *L[4];
    int N[4], t;
    MpyBits w;

    /* prices: a greedy parse of a sample, from defaults */
    memset(l0, 6, sizeof l0), memset(l1, 4, sizeof l1), memset(l2, 4, sizeof l2), memset(l3, 6, sizeof l3);
    l3[0] = 1, l3[1] = 3, l3[2] = 4;
    mpy_price(&P, l0, l1, l2, l3);
    if (sn > pro + MPY_SAMPLE)
        sn = pro + MPY_SAMPLE;
    mpy_start(&m, in, sn, 1);
    mpy_greedy(&m, pro, &P);
    mpy_stats(in, sn, pro, m.s, m.ns, f0, f1, f2, f3);
    mpy_lens(f0, MPY_NLIT, l0), mpy_lens(f1, MPY_NLEN, l1), mpy_lens(f2, MPY_NLEN, l2), mpy_lens(f3, MPY_NOF, l3);
    mpy_stop(&m);
    free(m.s);
    /* the whole text with those prices */
    mpy_price(&P, l0, l1, l2, l3);
    mpy_start(&m, in, n, level >= 3 ? 4 : 1);
    if (level >= 3)
        mpy_opt(&m, pro, &P);
    else
        mpy_greedy(&m, pro, &P);
    mpy_stop(&m);
    mpy_stats(in, n, pro, m.s, m.ns, f0, f1, f2, f3);
    mpy_lens(f0, MPY_NLIT, l0), mpy_lens(f1, MPY_NLEN, l1), mpy_lens(f2, MPY_NLEN, l2), mpy_lens(f3, MPY_NOF, l3);
    mpy_codes(l0, MPY_NLIT, c0), mpy_codes(l1, MPY_NLEN, c1), mpy_codes(l2, MPY_NLEN, c2), mpy_codes(l3, MPY_NOF, c3);

    buf_put(out, "\0MPY", 4);
    buf_u64(out, (u64)n);
    at = out->len;
    buf_u64(out, 0);
    buf_u64(out, (u64)pro);
    buf_put(out, in, pro);
    buf_uleb(out, (u64)m.ns);
    L[0] = l0, L[1] = l1, L[2] = l2, L[3] = l3;
    N[0] = MPY_NLIT, N[1] = MPY_NLEN, N[2] = MPY_NLEN, N[3] = MPY_NOF;
    for (t = 0; t < 4; t++)
        for (k = 0; k < (size_t)N[t]; k += 2)
            buf_u8(out, (u32)(L[t][k] | (k + 1 < (size_t)N[t] ? L[t][k + 1] << 4 : 0)));
    w.o = out, w.b = 0, w.n = 0;
    for (i = pro, k = 0; k < m.ns; k++) {
        const MpySeq *s = &m.s[k];
        size_t j;
        int nb, c;
        u64 ex;
        c = mpy_bk(s->ll, &nb, &ex);
        mpy_put(&w, c1[c], l1[c]);
        mpy_put(&w, ex, nb);
        for (j = 0; j < s->ll; j++)
            mpy_put(&w, c0[in[i + j]], l0[in[i + j]]);
        i += s->ll + s->ml;
        c = mpy_bk(s->ml - MPY_MINM, &nb, &ex);
        mpy_put(&w, c2[c], l2[c]);
        mpy_put(&w, ex, nb);
        if (s->oc < 3)
            mpy_put(&w, c3[s->oc], l3[s->oc]);
        else {
            c = mpy_bk(s->off, &nb, &ex);
            mpy_put(&w, c3[3 + c], l3[3 + c]);
            mpy_put(&w, ex, nb);
        }
    }
    for (; i < n; i++)
        mpy_put(&w, c0[in[i]], l0[in[i]]);
    if (w.n)
        buf_u8(out, (u32)(w.b & 255));
    put_u64le(out->p + at, (u64)(out->len - at - 16 - pro));
    free(m.s);
}

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
    size_t prologue = 0;
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
        const char *sp = is_constsym(hostsyms[k]) ? "const" : "global";
        if (hs->dsize)
            BP(&t, ".extern .%s .align %u .b8 %s[%lu];\n", sp, 1U << hs->align, pname(hostsyms[k]), (unsigned long)hs->dsize);
        else
            BP(&t, ".extern .%s .align 1 .b8 %s[];\n", sp, pname(hostsyms[k]));
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
    /* the bodies, with the names of called functions filled in (what is ahead of them, the declarations, stays plain
       in a packed record: mcudahw scans every unit's declarations, and unpacks only the bodies it runs) */
    prologue = t.len;
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
    if (opt_ptx_raw || opt_ptx_pack == 0) {
        textsym = put_data(o, SEC_CONST, t.p, t.len, 1);
    } else { /* (packed: mcuda_pack.h; the run time unpacks it before it reads it) */
        Buf z = {0};
        if (opt_ptx_pack == 1)
            ptx_pack(&z, t.p, t.len, prologue);
        else
            ptx_pack_mpy(&z, t.p, t.len, prologue, opt_ptx_pack);
        if (opt_ptx_file) { /* (MPF: the packed record in its own file, the object names it) */
            Buf r = {0};
            u64 h1 = (u64)0xcbf29ce4u << 32 | 0x84222325u, h2 = (u64)0x84222325u << 32 | 0xcbf29ce4u; /* MCUDA_PACK_FNV1 / 2 */
            u64 pr = (u64)0x100u << 32 | 0x1b3u;                                                       /* the FNV prime */
            size_t k;
            for (k = 0; k < z.len; k++) {
                h1 = (h1 ^ z.p[k]) * pr;
                h2 = (h2 ^ z.p[k]) * pr;
            }
            os_write_file(opt_ptx_file, z.p, z.len, 0);
            buf_put(&r, "\0MPF", 4);
            buf_u64(&r, (u64)t.len);
            buf_u64(&r, (u64)z.len);
            buf_u64(&r, (u64)prologue);
            buf_put(&r, t.p, prologue);
            buf_u64(&r, h1);
            buf_u64(&r, h2);
            buf_put(&r, opt_ptx_file, strlen(opt_ptx_file) + 1);
            textsym = put_data(o, SEC_CONST, r.p, r.len, 1);
            buf_free(&r);
        } else
            textsym = put_data(o, SEC_CONST, z.p, z.len, 1);
        buf_free(&z);
    }

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
