/* arm64 backend: instruction selection, frame layout, encoding.
   Calling convention: AAPCS64 as used on Darwin, except that structures are
   always passed and returned by reference (see arm64 notes in the README). */
#include "mir.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* physical register ids: 0..30 = x0..x30, 31 = sp or zr, 32..63 = v0..v31 */
#define R_FP 29
#define R_LR 30
#define R_SP 31
#define R_ZR 31
#define R_V0 32
#define SCRATCH0 15 /* x15, x16, x17 */
#define FSCRATCH0 62 /* v30, v31 */
#define NOSLOT 0xffffffffU
#define ARGSLOT 0xfffffffeU /* pseudo slot: the incoming stack argument area */

enum {
    A_MOV, A_MOVI, A_FMOVI, A_ADR, A_ADRP, A_SLOTADDR,
    A_ADD, A_SUB, A_MUL, A_SDIV, A_UDIV, A_AND, A_ORR, A_EOR, A_LSL, A_LSR, A_ASR,
    A_ADDI, A_SUBI, A_ANDI, A_ORRI, A_EORI, A_LSLI, A_LSRI, A_ASRI, A_RORI,
    A_MSUB, A_MADD, A_SMULH, A_UMULH, A_NEG, A_MVN, A_CLZ, A_RBIT, A_REV, A_FRAMEADDR,
    A_ASM,    /* inline assembly: imm = index of the AsmStmt */
    A_DEFREG, /* no code: d is written by the inline assembly before */
    A_ALLOCA, /* d = s0 more bytes of stack */
    A_BRK,
    A_TLS,    /* d = the thread pointer plus the offset of sym + imm from it */
    A_LDAR,   /* d = [s0], acquiring; cc = log2 of the size */
    A_STLR,   /* [s0] = s1, releasing; cc = log2 of the size */
    A_ATOMIC, /* read-modify-write of [x9] with x10 (and x11); imm as the callee of an IF_ATOMIC call; old value to x9 */
    A_DMB,
    A_SXTB, A_SXTH, A_SXTW, A_UXTB, A_UXTH, A_UXTW,
    A_CMP, A_CMPI, A_FCMP, A_CCMP, A_CSET, A_CSEL, A_FCSEL,
    A_LDR, A_STR,
    A_FADD, A_FSUB, A_FMUL, A_FDIV, A_FNEG,
    A_SCVTF, A_UCVTF, A_FCVTZS, A_FCVTZU, A_FCVT,
    A_B, A_BCC, A_CBZ, A_CBNZ, A_TBZ, A_TBNZ, A_BL, A_BLR, A_TAIL, A_RET,
    /* 128-bit vectors; cc = lane kind (a memory access kind) */
    A_VLDR, /* d = [s0 + s1] */
    A_VSTR, /* [s0 + s1] = s2 */
    A_VBIN, /* imm = the IR opcode */
    A_VDUP, /* scalar s0 into every lane; imm: s0 is a floating point register */
    A_VSUM, /* scalar register d = the lanes of s0 combined; imm as OP_VSUM's flags */
    A_VSEL, /* lane mask s0 ? s1 : s2 */
    A_VIOTA /* 0, imm, 2 * imm, ... in the lanes */
};

enum { CC_EQ, CC_NE, CC_HS, CC_LO, CC_MI, CC_PL, CC_VS, CC_VC, CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE };

static const MTarget target = {
    { 0x1ff87fffUL,                 /* x0-x14, x19-x28 */
      0x3fffffffUL << 32 },         /* v0-v29 */
    0x7fffUL | (0x3fff00ffUL << 32), /* x0-x14; v0-v7, v16-v29 */
    A_MOV
};

static Func *f;
static Obj *obj;
static MFunc mf;
static u32 *uses;      /* per IR value: number of uses */
#define FUSE_OFF 1 /* add of a constant folded into an address */
#define FUSE_IDX 2 /* add of an index register folded into an address */
#define FUSE_SKIP 3 /* shift or extension that is part of a folded index */
#define FUSE_OPND 4 /* shift or multiply folded into the add, subtract or logical op that uses it */
#define LOAD_SIGNED32 8 /* access kind, here only: a 32-bit load that sign-extends to 64 bits */
#define FUSE_ROT 10 /* a shift that is half of a rotate: the OR that joins the halves does the work */
#define FUSE_BIT 9 /* and with one bit, tested by a branch */
#define FUSE_ALIAS 7 /* truncation or extension that is its operand's register, unchanged */
#define FUSE_COND 6 /* and/or of comparisons used only as a condition: a chain of conditional compares */
#define FUSE_NOT 0x80 /* while counting uses: the value is used as a value */
#define FUSE_MOV 5 /* extension made redundant by changing the load it reads */
static u8 *fused;      /* per IR value: folded into its user */
static u32 *cache_reg; /* per floating IR value: register holding it ... */
static u32 *cache_blk; /* ... in this block */
static u32 cur_blk;
static u32 *blk_index; /* IR block -> layout index */
static u32 *layout;    /* layout index -> IR block */
static u32 nlayout;
static u32 outargs;    /* bytes of outgoing stack arguments */
static u32 named_stack; /* bytes of incoming named stack arguments */
static u32 sym_memcpy, sym_memset;
static int has_calls, uses_args, uses_fp;
static u32 *alink;    /* inline assembly call -> its first OP_ASMOUT -> the next; made when first needed */
static u64 asm_saved; /* registers written by inline assembly */
static u32 ret_slot;  /* where a record returned in registers is put together */
static int uses_alloca;
/* Slots, spills and saved registers are found from sp, which stays put;
   in a function that calls alloca it does not, and they are found from x29. */
static u32 fbase = R_SP;
static i64 fbias;
static u32 gr_slot, vr_slot; /* variadic function: the argument registers, saved */
static int no_slots;  /* the function has no addressable stack objects */
static u32 tail_blk;  /* block whose return was folded into a tail call */
static u32 arg_base; /* offset from x29 to the incoming stack arguments */
static int frame_kind; /* 0: no frame, 1: frame pushed with the fp/lr pair, 2: general */
static Obj *sym_obj_owner;

/* ---- machine IR construction ---- */

static MIns *emit(int op, int w, u32 d, u32 s0, u32 s1)
{
    MIns *i;
    if (mf.n == mf.cap) {
        mf.cap = mf.cap ? mf.cap * 2 : 1024;
        mf.ins = (MIns *)xrealloc(mf.ins, mf.cap * sizeof(MIns));
    }
    i = &mf.ins[mf.n++];
    i->op = (u16)op;
    i->w = (u8)w;
    i->cc = 0;
    i->flags = 0;
    i->d = d;
    i->s0 = s0;
    i->s1 = s1;
    i->s2 = NOREG;
    i->sym = NOSLOT;
    i->imm = 0;
    i->pmask = 0;
    return i;
}

static u32 new_vreg(int cls)
{
    if (mf.nvreg == mf.capvreg) {
        mf.capvreg = mf.capvreg ? mf.capvreg * 2 : 1024;
        mf.vclass = (u8 *)xrealloc(mf.vclass, mf.capvreg);
    }
    mf.vclass[mf.nvreg] = (u8)cls;
    return VREG0 + mf.nvreg++;
}

static int is_fp(int ty)
{
    return ty == IT_F32 || ty == IT_F64 || ty == IT_V128;
}

/* register class: bit 0 = floating point and vector registers, bit 1 = all 128 bits matter */
static int cls_of(int ty)
{
    return ty == IT_V128 ? 3 : ty == IT_F32 || ty == IT_F64;
}

static int has_vec; /* the function has 128-bit values: copies move whole registers */

static int is_w(int ty)
{
    return ty == IT_I64 || ty == IT_F64;
}

/* Materialise a floating value into register d. */
static void materialise(u32 v, u32 d)
{
    Ins *i = &f->ins[v];
    MIns *m;
    switch (i->op) {
    case OP_CONST:
        m = emit(A_MOVI, i->ty == IT_I64, d, NOREG, NOREG);
        m->imm = (i64)((u64)i->a | ((u64)i->b << 32));
        break;
    case OP_FCONST:
        m = emit(A_FMOVI, i->ty == IT_F64, d, NOREG, NOREG);
        m->imm = (i64)((u64)i->a | ((u64)i->b << 32));
        break;
    case OP_ADDR:
        m = emit(A_ADR, 1, d, NOREG, NOREG);
        m->sym = i->a;
        m->imm = (i32)i->b;
        break;
    case OP_SLOT:
        m = emit(A_SLOTADDR, 1, d, NOREG, NOREG);
        m->sym = i->a;
        m->imm = (i32)i->b;
        break;
    default:
        emit(A_MOV, 1, d, VREG0 + v, NOREG);
        break;
    }
}

static int is_floating(u32 v)
{
    int op = f->ins[v].op;
    return op == OP_CONST || op == OP_FCONST || op == OP_ADDR || op == OP_SLOT;
}

/* The register holding IR value v. */
static u32 reg_of(u32 v)
{
    u32 r;
    while (fused[v] == FUSE_ALIAS)
        v = f->ins[v].a;
    if (!is_floating(v))
        return VREG0 + v;
    if (cache_blk[v] == cur_blk)
        return cache_reg[v];
    r = new_vreg(cls_of(f->ins[v].ty));
    materialise(v, r);
    cache_blk[v] = cur_blk;
    cache_reg[v] = r;
    return r;
}

/* Copy IR value v into register d (which may be physical). */
static void move_to(u32 d, u32 v)
{
    while (fused[v] == FUSE_ALIAS)
        v = f->ins[v].a;
    if (is_floating(v))
        materialise(v, d);
    else
        emit(A_MOV, 1, d, VREG0 + v, NOREG);
}

static const u8 mem_bytes[] = { 1, 1, 2, 2, 4, 8, 4, 8 };
static u32 fn_serial; /* counts functions compiled */

typedef struct Addr {
    u32 base; /* register, or NOREG when slot is set */
    u32 slot;
    i64 off;
    u32 idx; /* index register or NOREG */
    u8 flags; /* MF_SCALE, MF_SXTW */
} Addr;


/* The register holding the page of global address v, for this block. */
static u32 page_reg(u32 v)
{
    static u32 pv[8], pb[8], pr[8], pf, next;
    u32 k, r;
    MIns *m;
    if (pf != fn_serial) {
        pf = fn_serial;
        memset(pb, 0, sizeof pb);
    }
    for (k = 0; k < 8; k++)
        if (pv[k] == v && pb[k] == cur_blk)
            return pr[k];
    r = new_vreg(0);
    m = emit(A_ADRP, 1, r, NOREG, NOREG);
    m->sym = f->ins[v].a;
    m->imm = (i32)f->ins[v].b;
    k = next++ & 7;
    pv[k] = v;
    pb[k] = cur_blk;
    pr[k] = r;
    return r;
}

/* The address v for an access of kind mem (-1: no particular access). */
static void addr_of(u32 v, Addr *a, int mem)
{
    Ins *i = &f->ins[v];
    u64 c;
    a->slot = NOSLOT;
    a->off = 0;
    a->idx = NOREG;
    a->flags = 0;
    if (fused[v] == FUSE_IDX) {
        u32 x = i->a, y = i->b;
        if (fused[x] == FUSE_SKIP && fused[y] != FUSE_SKIP) {
            x = i->b;
            y = i->a;
        }
        a->base = reg_of(x);
        if (fused[y] == FUSE_SKIP && f->ins[y].op == OP_SHL) {
            a->flags |= MF_SCALE;
            y = f->ins[y].a;
        }
        if (fused[y] == FUSE_SKIP && f->ins[y].op == OP_SEXT32) {
            a->flags |= MF_SXTW;
            y = f->ins[y].a;
        }
        else if (fused[y] == FUSE_SKIP && f->ins[y].op == OP_ZEXT32) {
            a->flags |= MF_UXTW;
            y = f->ins[y].a;
        }
        a->idx = reg_of(y);
        return;
    }
    if (i->op == OP_SLOT) {
        a->base = NOREG;
        a->slot = i->a;
        a->off = (i32)i->b;
    } else if (fused[v] && i->op == OP_ADD && ir_is_const(f, i->b, &c)) {
        Ins *b = &f->ins[i->a];
        a->off = (i64)c;
        if (b->op == OP_SLOT) {
            a->base = NOREG;
            a->slot = b->a;
            a->off += (i32)b->b;
        } else {
            a->base = reg_of(i->a);
        }
    } else if (i->op == OP_ADDR && mem >= 0 && cache_blk[v] != cur_blk && !obj->syms[i->a].weak &&
               mem_bytes[mem] <= ((u32)1 << obj->syms[i->a].align) && (i->b & (mem_bytes[mem] - 1)) == 0) {
        /* a global whose alignment suits the access: page, then offset in the access */
        a->base = page_reg(v);
        a->slot = i->a;
        a->off = (i32)i->b;
        a->flags = MF_LO12;
    } else {
        a->base = reg_of(v);
    }
}

static void emit_load(u32 d, const Addr *a, int mem)
{
    MIns *m = emit(A_LDR, 0, d, a->base, a->idx);
    m->cc = (u8)mem;
    m->sym = a->slot;
    m->imm = a->off;
    m->flags = a->flags;
}

static void emit_store(u32 val, const Addr *a, int mem)
{
    MIns *m = emit(A_STR, 0, NOREG, val, a->base);
    m->s2 = a->idx;
    m->cc = (u8)mem;
    m->sym = a->slot;
    m->imm = a->off;
    m->flags = a->flags;
}

static int cmp_cc(int op)
{
    switch (op) {
    case OP_EQ: case OP_FEQ: return CC_EQ;
    case OP_NE: case OP_FNE: return CC_NE;
    case OP_SLT: return CC_LT;
    case OP_SLE: return CC_LE;
    case OP_SGT: return CC_GT;
    case OP_SGE: return CC_GE;
    case OP_ULT: return CC_LO;
    case OP_ULE: return CC_LS;
    case OP_UGT: return CC_HI;
    case OP_UGE: return CC_HS;
    case OP_FLT: return CC_MI;
    case OP_FLE: return CC_LS;
    case OP_FGT: return CC_GT;
    case OP_FGE: return CC_GE;
    }
    return CC_NE;
}

static int is_compare(int op)
{
    return (op >= OP_EQ && op <= OP_UGE) || (op >= OP_FEQ && op <= OP_FGE);
}

static int emit_cond(u32 v);

/* Emits the flag-setting instruction for compare v; returns the condition. */
static int emit_compare(u32 v)
{
    Ins *i = &f->ins[v];
    int ty = f->ins[i->a].ty;
    u64 c;
    if (fused[i->a] == FUSE_COND) /* a chain compared with zero */
        return emit_cond(i->a) ^ (i->op == OP_EQ);
    if (is_fp(ty)) {
        emit(A_FCMP, ty == IT_F64, NOREG, reg_of(i->a), reg_of(i->b));
    } else if (ir_is_const(f, i->b, &c) && (ty == IT_I64 ? c : (u64)(u32)c) < 4096) {
        MIns *m = emit(A_CMPI, ty == IT_I64, NOREG, reg_of(i->a), NOREG);
        m->imm = (i64)(ty == IT_I64 ? c : (u64)(u32)c);
    } else {
        emit(A_CMP, ty == IT_I64, NOREG, reg_of(i->a), reg_of(i->b));
    }
    return cmp_cc(i->op);
}

/* Sets the flags for condition v, a comparison or a chain of them joined
   by and/or; returns the condition code that means "true". */
static int emit_cond(u32 v)
{
    /* flags under which each condition holds */
    static const u8 nzcv_true[16] = { 4, 0, 2, 0, 8, 0, 1, 0, 2, 0, 0, 8, 0, 4, 0, 0 };
    Ins *i = &f->ins[v], *r;
    u32 l = i->a, rv = i->b;
    int lcc, rcc, w;
    u64 c;
    MIns *m;
    if (fused[v] != FUSE_COND)
        return emit_compare(v);
    if (fused[rv] != 1) {
        l = i->b;
        rv = i->a;
    }
    lcc = emit_cond(l);
    r = &f->ins[rv];
    rcc = cmp_cc(r->op);
    w = f->ins[r->a].ty == IT_I64;
    /* and: compare again if the left side held, else flags that fail;
       or: compare again if it did not hold, else flags that pass */
    if (ir_is_const(f, r->b, &c) && (w ? c : (u64)(u32)c) < 32) {
        m = emit(A_CCMP, w, NOREG, reg_of(r->a), NOREG);
        m->imm = (i64)(0x100 | ((w ? c : (u64)(u32)c) << 16));
    } else {
        m = emit(A_CCMP, w, NOREG, reg_of(r->a), reg_of(r->b));
        m->imm = 0;
    }
    if (i->op == OP_AND) {
        m->cc = (u8)lcc;
        m->imm |= nzcv_true[rcc ^ 1];
    } else {
        m->cc = (u8)(lcc ^ 1);
        m->imm |= nzcv_true[rcc];
    }
    return rcc;
}

/* Does an instruction of this kind leave the upper half of its register clear? */
static int clean32(int op)
{
    return (op >= OP_ADD && op <= OP_ZEXT16) || op == OP_FTOSI || op == OP_FTOUI || op == OP_LOAD;
}

/* Can v be evaluated by emit_cond? Marks the values it swallows. */
static int cond_chain(u32 v, int root)
{
    Ins *i = &f->ins[v];
    u32 l, r;
    if (!(i->op == OP_AND || i->op == OP_OR) || i->ty != IT_I32 || (!root && (uses[v] != 1 || fused[v])))
        return 0;
    l = i->a;
    r = i->b;
    if (!(f->ins[r].op >= OP_EQ && f->ins[r].op <= OP_UGE)) {
        l = i->b;
        r = i->a;
    }
    if (!(f->ins[r].op >= OP_EQ && f->ins[r].op <= OP_UGE) || uses[r] != 1 || fused[r] || l == r)
        return 0;
    if (f->ins[l].op >= OP_EQ && f->ins[l].op <= OP_UGE) {
        if (uses[l] != 1 || fused[l])
            return 0;
    } else if (!cond_chain(l, 0)) {
        return 0;
    }
    fused[l] = f->ins[l].op >= OP_EQ && f->ins[l].op <= OP_UGE ? 1 : FUSE_COND;
    fused[r] = 1;
    return 1;
}

/* ---- calling convention ---- */

#define LOC_STACK 0x1000 /* flag: value is a stack offset */

/* Assigns locations to arguments; returns the stack bytes used.
   Arguments from index nfixed on are variadic and always go on the stack. */
static u32 abi_assign(const u8 *tys, u32 n, u32 nfixed, int sret, u32 *locs)
{
    u32 ngr = 0, nfr = 0, stack = 0, k;
    for (k = 0; k < n; k++) {
        int ty = tys[k];
        if (sret && k == 0) {
            locs[k] = 8;
            continue;
        }
        if (k >= nfixed && opt_target != TARGET_LINUX) {
            stack = (u32)align_up(stack, 8);
            locs[k] = LOC_STACK | stack;
            stack += 8;
        } else if (is_fp(ty) && nfr < 8) {
            locs[k] = R_V0 + nfr++;
        } else if (!is_fp(ty) && ngr < 8) {
            locs[k] = ngr++;
        } else {
            /* Linux gives every argument a doubleword; macOS packs them */
            u32 size = is_w(ty) || opt_target == TARGET_LINUX ? 8 : 4;
            stack = (u32)align_up(stack, size);
            locs[k] = LOC_STACK | stack;
            stack += size;
        }
    }
    return (u32)align_up(stack, 8);
}

static int ty_mem(int ty)
{
    return ty == IT_I64 ? M_64 : ty == IT_F32 ? M_F32 : ty == IT_F64 ? M_F64 : M_32;
}

int opt_no_fp;
int arm64_text_sec = SEC_TEXT; /* the section functions are generated into */

static u32 runtime_sym(const char *name, u32 *cache)
{
    if (opt_target == TARGET_LINUX)
        name++; /* no leading underscore on ELF */
    if (sym_obj_owner != obj) {
        sym_obj_owner = obj;
        sym_memcpy = sym_memset = NOSLOT;
    }
    if (*cache == NOSLOT)
        *cache = obj_add_sym(obj, intern_cstr(name), SEC_UNDEF, 0, 1);
    return *cache;
}

/* The register of each operand of an inline assembly statement. Nothing
   is shared, so an output may be written before the inputs are read. */
static void asm_assign(const AsmStmt *st, u8 *regs)
{
    static const u8 pool[] = {9, 10, 11, 12, 13, 14, 8, 0, 1, 2, 3, 4, 5, 6, 7};
    u64 taken = st->clobbers;
    u32 k, pi = 0;
    for (k = 0; k < st->nops; k++)
        if (st->ops[k].reg >= 0)
            taken |= (u64)1 << st->ops[k].reg;
    for (k = 0; k < st->nops; k++) {
        const AsmOp *o = &st->ops[k];
        regs[k] = 0xff;
        if (o->kind == 'i' || o->tied >= 0)
            continue;
        if (o->reg >= 0) {
            regs[k] = (u8)o->reg;
            continue;
        }
        while (pi < sizeof pool && (taken >> pool[pi] & 1))
            pi++;
        if (pi == sizeof pool)
            die("%s: inline assembly has too many operands", f->name);
        regs[k] = pool[pi++];
    }
    for (k = 0; k < st->nops; k++)
        if (st->ops[k].kind != 'i' && st->ops[k].tied >= 0)
            regs[k] = st->ops[k].reg >= 0 ? (u8)st->ops[k].reg : regs[st->ops[k].tied];
}

static void sel_asm(u32 v)
{
    Ins *i = &f->ins[v];
    const AsmStmt *st = &asm_stmts[f->ins[i->c].a];
    u8 regs[32];
    u32 k, arg = 0, o;
    u64 pmask = 0, defs = st->clobbers;
    MIns *m;

    if (st->dev) {
        /* device code in a function made for both sides: the host's copy cannot do it */
        emit(A_BRK, 0, NOREG, NOREG, NOREG);
        for (o = 1; o < f->nins; o++)
            if (f->ins[o].op == OP_ASMOUT && f->ins[o].blk && f->ins[o].a == v && uses[o]) {
                int fl = f->ins[o].ty == IT_F32 || f->ins[o].ty == IT_F64;
                m = emit(fl ? A_FMOVI : A_MOVI, f->ins[o].ty == IT_I64 || f->ins[o].ty == IT_F64, VREG0 + o, NOREG, NOREG);
                m->imm = 0;
            }
        return;
    }
    asm_assign(st, regs);
    for (k = 0; k < st->nops; k++) {
        const AsmOp *op = &st->ops[k];
        if (op->kind == 'm' || (op->kind == 'r' && op->out != 1)) {
            move_to(regs[k], f->xargs[i->a + arg++]);
            pmask |= (u64)1 << regs[k];
        }
        if (op->kind == 'r' && op->out)
            defs |= (u64)1 << regs[k];
    }
    m = emit(A_ASM, 0, NOREG, NOREG, NOREG);
    m->imm = f->ins[i->c].a;
    m->pmask = pmask;
    if (defs >> R_LR & 1)
        has_calls = 1; /* the return address has to be kept in the frame */
    asm_saved |= defs;
    for (k = 0; k < 29; k++)
        if ((defs >> k & 1) && (target.allocatable[0] >> k & 1))
            emit(A_DEFREG, 1, k, NOREG, NOREG);
    if (!alink) {
        alink = (u32 *)xmalloc(f->nins * sizeof(u32));
        memset(alink, 0, f->nins * sizeof(u32));
        for (o = 1; o < f->nins; o++)
            if (f->ins[o].op == OP_ASMOUT && f->ins[o].blk) {
                alink[o] = alink[f->ins[o].a];
                alink[f->ins[o].a] = o;
            }
    }
    for (o = alink[v]; o; o = alink[o])
        if (uses[o])
            emit(A_MOV, 1, VREG0 + o, regs[f->ins[o].b], NOREG);
}

/* Atomic operations. Loads and stores are single instructions on any
   registers. The others are a loop around a pair of exclusive accesses,
   which works in fixed registers: the address in x9, operands in x10 and
   x11, x15 to x17 as scratch, and the old value comes back in x9. */
static void sel_atomic(u32 v)
{
    Ins *i = &f->ins[v];
    u32 code = f->ins[i->c].a, kind = code & 255, lg = code >> 8 & 3, d = VREG0 + v;
    int relaxed = (int)(code >> 12 & 1), mem = lg == 0 ? M_8U : lg == 1 ? M_16U : lg == 2 ? M_32 : M_64;
    MIns *m;
    Addr a;
    u32 r;

    switch (kind) {
    case ATOM_FENCE:
        emit(A_DMB, 0, NOREG, NOREG, NOREG);
        return;
    case ATOM_LOAD:
        if (relaxed) {
            addr_of(f->xargs[i->a], &a, mem);
            emit_load(d, &a, mem);
        } else {
            m = emit(A_LDAR, lg == 3, d, reg_of(f->xargs[i->a]), NOREG);
            m->cc = (u8)lg;
        }
        return;
    case ATOM_STORE:
        if (relaxed) {
            r = reg_of(f->xargs[i->a + 1]);
            addr_of(f->xargs[i->a], &a, mem);
            emit_store(r, &a, mem);
        } else {
            r = reg_of(f->xargs[i->a]);
            m = emit(A_STLR, lg == 3, NOREG, r, reg_of(f->xargs[i->a + 1]));
            m->cc = (u8)lg;
        }
        return;
    }
    move_to(9, f->xargs[i->a]);
    move_to(10, f->xargs[i->a + 1]);
    if (kind == ATOM_CAS)
        move_to(11, f->xargs[i->a + 2]);
    m = emit(A_ATOMIC, 0, NOREG, NOREG, NOREG);
    m->imm = code;
    m->pmask = kind == ATOM_CAS ? 0xe00 : 0x600;
    emit(A_DEFREG, 1, 9, NOREG, NOREG);
    if (uses[v])
        emit(A_MOV, 1, d, 9, NOREG);
}

static void sel_call(u32 v)
{
    Ins *i = &f->ins[v];
    u32 n = i->b, nfixed = f->xargs[i->a - 1], k, stack;
    u8 tys[128];
    u32 locs[128];
    u64 pmask = 0;
    MIns *m;
    Addr a;
    u32 callee = NOREG;

    if (i->flags & IF_ASM) {
        sel_asm(v);
        return;
    }
    if (f->ins[i->c].op == OP_ADDR && obj->syms[f->ins[i->c].a].isdev) {
        m = emit(A_MOVI, 0, VREG0 + v, NOREG, NOREG); /* CUDA: this is not device code */
        m->imm = 0;
        return;
    }
    if (i->flags & IF_ATOMIC) {
        sel_atomic(v);
        return;
    }
    if (i->flags & IF_ALLOCA) {
        k = (u32)f->ins[i->c].a; /* 0: take stack, 1: the stack pointer, 2: set it */
        m = k == 2 ? emit(A_ALLOCA, 1, NOREG, reg_of(f->xargs[i->a]), NOREG)
            : emit(A_ALLOCA, 1, VREG0 + v, k == 1 ? NOREG : reg_of(f->xargs[i->a]), NOREG);
        m->imm = k;
        uses_fp = uses_alloca = 1;
        return;
    }
    for (k = 0; k < n; k++)
        tys[k] = f->ins[f->xargs[i->a + k]].ty;
    stack = abi_assign(tys, n, nfixed, i->flags & IF_SRET, locs);
    if (stack > outargs)
        outargs = stack;
    if (f->ins[i->c].op != OP_ADDR || f->ins[i->c].b != 0)
        callee = reg_of(i->c);
    for (k = 0; k < n; k++) {
        if (locs[k] & LOC_STACK) {
            a.base = R_SP;
            a.idx = NOREG;
            a.flags = 0;
            a.slot = NOSLOT;
            a.off = locs[k] & ~(u32)LOC_STACK;
            emit_store(reg_of(f->xargs[i->a + k]), &a, k >= nfixed && !is_fp(tys[k]) ? M_64 : ty_mem(tys[k]));
        }
    }
    for (k = 0; k < n; k++) {
        if (k == 0 && (i->flags & IF_SRET) && i->mem)
            continue; /* the result comes back in registers and is stored below */
        if (!(locs[k] & LOC_STACK)) {
            move_to(locs[k], f->xargs[i->a + k]);
            pmask |= (u64)1 << locs[k];
        }
    }
    if (callee == NOREG && stack == 0 && no_slots && !(i->flags & IF_SRET)) {
        /* a call whose result is returned at once: nothing of this frame is
           needed any more, so the callee can return for us */
        Block *bb = &f->blk[i->blk];
        if (bb->last == v && bb->term == TERM_RET && (bb->arg == v || (!bb->arg && !uses[v]))) {
            m = emit(A_TAIL, 0, NOREG, NOREG, NOREG);
            m->sym = f->ins[i->c].a;
            m->pmask = pmask;
            tail_blk = i->blk;
            return;
        }
    }
    if (callee == NOREG && obj->syms[f->ins[i->c].a].trap) {
        /* a function declared with the error attribute: the call was to be
           optimised away, and what could not be proved dead traps */
        m = emit(A_BRK, 0, NOREG, NOREG, NOREG);
        return;
    }
    if (callee == NOREG) {
        m = emit(A_BL, 0, NOREG, NOREG, NOREG);
        m->sym = f->ins[i->c].a;
    } else {
        m = emit(A_BLR, 0, NOREG, callee, NOREG);
    }
    m->flags = MF_CALL;
    m->pmask = pmask;
    has_calls = 1;
    if ((i->flags & IF_SRET) && i->mem) {
        /* a small record comes back in x0 and x1; the place for it holds 16 bytes */
        a.base = reg_of(f->xargs[i->a]);
        a.idx = NOREG;
        a.flags = 0;
        a.slot = NOSLOT;
        a.off = 0;
        emit_store(0, &a, M_64);
        if (i->mem > 8) {
            a.off = 8;
            emit_store(1, &a, M_64);
        }
    }
    if (i->ty != IT_VOID && uses[v])
        emit(A_MOV, 1, VREG0 + v, is_fp(i->ty) ? R_V0 : 0, NOREG);
}

static void sel_runtime_call(u32 sym, u32 dst, u32 src_or_zero, int has_src, u32 size)
{
    MIns *m;
    move_to(0, dst);
    if (has_src) {
        move_to(1, src_or_zero);
    } else {
        m = emit(A_MOVI, 0, 1, NOREG, NOREG);
        m->imm = 0;
    }
    m = emit(A_MOVI, 1, 2, NOREG, NOREG);
    m->imm = size;
    m = emit(A_BL, 0, NOREG, NOREG, NOREG);
    m->sym = sym;
    m->flags = MF_CALL;
    m->pmask = 7;
    has_calls = 1;
}

static void sel_block_op(u32 v)
{
    Ins *i = &f->ins[v];
    u32 size = i->c, off = 0, t;
    Addr d, s;
    int copy = i->op == OP_MEMCPY;

    if (size == 0)
        return;
    if (size > 64) {
        if (copy)
            sel_runtime_call(runtime_sym("_memcpy", &sym_memcpy), i->a, i->b, 1, size);
        else
            sel_runtime_call(runtime_sym("_memset", &sym_memset), i->a, 0, 0, size);
        return;
    }
    addr_of(i->a, &d, -1);
    if (copy)
        addr_of(i->b, &s, -1);
    t = new_vreg(0);
    if (!copy) {
        MIns *m = emit(A_MOVI, 1, t, NOREG, NOREG);
        m->imm = 0;
    }
    while (size) {
        u32 n = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        int mem = n == 8 ? M_64 : n == 4 ? M_32 : n == 2 ? M_16U : M_8U;
        Addr da = d, sa;
        da.off += off;
        if (copy) {
            sa = s;
            sa.off += off;
            emit_load(t, &sa, mem);
        }
        emit_store(t, &da, mem);
        off += n;
        size -= n;
    }
}

int arm64_logical_imm(u64 v, int w64, u32 *enc);
#define logical_imm arm64_logical_imm

static int imm12(u64 c)
{
    return c < 4096 || ((c & 0xfff) == 0 && c < (1UL << 24));
}


/* Is v an OR of x >> n and x << (width - n), with nothing else using the
   shifts? Then it is x rotated right by n. */
static int rotate_of(u32 v, u32 *x, u64 *n)
{
    Ins *i = &f->ins[v], *l, *r;
    u64 cl, cr;
    u32 width = i->ty == IT_I64 ? 64 : 32;
    if (i->op != OP_OR)
        return 0;
    l = &f->ins[i->a];
    r = &f->ins[i->b];
    if (l->op == OP_SHL && r->op == OP_SHR) {
        Ins *t = l;
        l = r;
        r = t;
    }
    if (l->op != OP_SHR || r->op != OP_SHL || l->a != r->a || l->ty != i->ty || r->ty != i->ty)
        return 0;
    if (!ir_is_const(f, l->b, &cl) || !ir_is_const(f, r->b, &cr) || cl == 0 || cr == 0 || cl + cr != width)
        return 0;
    *x = l->a;
    *n = cl;
    return 1;
}

/* The operand y folded into an add, subtract or logical instruction: sets
   *x to the value actually read and returns the shift amount, plus the
   shift kind in bits 6-7 (lsl, lsr, asr, ror) and 256 for "inverted". */
static i64 folded_operand(u32 y, u32 *x)
{
    Ins *yi = &f->ins[y];
    u64 c = 0;
    *x = yi->a;
    switch (yi->op) {
    case OP_NOT:
        return 256;
    case OP_OR:
        rotate_of(y, x, &c);
        return (i64)c | 192;
    case OP_SHR:
        ir_is_const(f, yi->b, &c);
        return (i64)c | 64;
    case OP_SAR:
        ir_is_const(f, yi->b, &c);
        return (i64)c | 128;
    }
    ir_is_const(f, yi->b, &c);
    return (i64)c;
}

static void sel_vec(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v;
    MIns *m;
    switch (i->op) {
    case OP_VLOAD:
        m = emit(A_VLDR, 0, d, reg_of(i->a), reg_of(i->b));
        break;
    case OP_VSTORE:
        m = emit(A_VSTR, 0, NOREG, reg_of(i->a), reg_of(i->c));
        m->s2 = reg_of(i->b);
        break;
    case OP_VSPLAT:
        m = emit(A_VDUP, 0, d, reg_of(i->a), NOREG);
        m->imm = is_fp(f->ins[i->a].ty);
        break;
    case OP_VIOTA:
        m = emit(A_VIOTA, 0, d, NOREG, NOREG);
        m->imm = (i32)i->a;
        break;
    case OP_VSUM:
        m = emit(A_VSUM, 0, d, reg_of(i->a), NOREG);
        m->imm = i->flags;
        break;
    case OP_SELECT:
        m = emit(A_VSEL, 0, d, reg_of(i->a), reg_of(i->b));
        m->s2 = reg_of(i->c);
        break;
    case OP_SITOF: case OP_UITOF: case OP_FTOSI: case OP_FTOUI:
        m = emit(A_VBIN, 0, d, reg_of(i->a), NOREG);
        m->imm = i->op;
        break;
    case OP_SHL: case OP_SHR: case OP_SAR:
        m = emit(A_VBIN, 0, d, reg_of(i->a), NOREG);
        m->imm = i->op;
        m->sym = f->ins[i->b].a;
        break;
    case OP_NEG: case OP_FNEG:
        m = emit(A_VBIN, 0, d, reg_of(i->a), NOREG);
        m->imm = i->op;
        break;
    default:
        m = emit(A_VBIN, 0, d, reg_of(i->a), reg_of(i->b));
        m->imm = i->op;
    }
    m->cc = i->mem;
}

static void sel_ins(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v;
    int w = is_w(i->ty), op = 0;
    u64 c;
    MIns *m;
    Addr a;

    if (i->ty == IT_V128 || i->op == OP_VSTORE || i->op == OP_VSUM) {
        sel_vec(v);
        return;
    }

    switch (i->op) {
    case OP_PARAM:
        return; /* handled at function entry */
    case OP_ADD:
    case OP_SUB:
        if (fused[v])
            return;
        if (ir_is_const(f, i->b, &c)) {
            u64 pos = w ? c : (u64)(u32)c, neg = w ? 0 - c : (u64)(u32)(0 - c);
            if (imm12(pos) || imm12(neg)) {
                int sub = i->op == OP_SUB;
                if (!imm12(pos)) {
                    sub = !sub;
                    pos = neg;
                }
                m = emit(sub ? A_SUBI : A_ADDI, w, d, reg_of(i->a), NOREG);
                m->imm = (i64)pos;
                return;
            }
        }
        {
            u32 x = i->a, y = i->b;
            if (fused[x] == FUSE_OPND && i->op == OP_ADD) {
                x = i->b;
                y = i->a;
            }
            if (fused[y] == FUSE_OPND) {
                Ins *yi = &f->ins[y];
                if (yi->op == OP_MUL) {
                    u32 rx = reg_of(x);
                    m = emit(i->op == OP_ADD ? A_MADD : A_MSUB, w, d, reg_of(yi->a), reg_of(yi->b));
                    m->s2 = rx;
                } else {
                    u32 src;
                    i64 sh = folded_operand(y, &src);
                    m = emit(i->op == OP_ADD ? A_ADD : A_SUB, w, d, reg_of(x), reg_of(src));
                    m->imm = sh;
                }
                return;
            }
        }
        emit(i->op == OP_ADD ? A_ADD : A_SUB, w, d, reg_of(i->a), reg_of(i->b));
        return;
    case OP_MUL:
        if (fused[v])
            return;
        op = A_MUL;
        goto binary;
    case OP_SDIV: op = A_SDIV; goto binary;
    case OP_UDIV: op = A_UDIV; goto binary;
    case OP_UMULH: op = A_UMULH; goto binary;
    case OP_SMULH: op = A_SMULH; goto binary;
    case OP_FADD: op = A_FADD; goto binary;
    case OP_FSUB: op = A_FSUB; goto binary;
    case OP_FMUL: op = A_FMUL; goto binary;
    case OP_FDIV: op = A_FDIV; goto binary;
    binary:
        emit(op, w, d, reg_of(i->a), reg_of(i->b));
        return;
    case OP_SREM:
    case OP_UREM: {
        u32 q = new_vreg(0), ra = reg_of(i->a), rb = reg_of(i->b);
        emit(i->op == OP_SREM ? A_SDIV : A_UDIV, w, q, ra, rb);
        m = emit(A_MSUB, w, d, q, rb);
        m->s2 = ra;
        return;
    }
    case OP_AND:
    case OP_OR:
    case OP_XOR: {
        u32 enc;
        if (fused[v] == FUSE_COND || fused[v] == FUSE_BIT)
            return;
        op = i->op == OP_AND ? A_AND : i->op == OP_OR ? A_ORR : A_EOR;
        if (ir_is_const(f, i->b, &c) && logical_imm(w ? c : (u64)(u32)c, w, &enc)) {
            m = emit(op + (A_ANDI - A_AND), w, d, reg_of(i->a), NOREG);
            m->imm = enc;
            return;
        }
        if (fused[v] == FUSE_OPND)
            return; /* a rotate folded into its user */
        if (fused[i->b] == FUSE_OPND) {
            u32 src;
            i64 sh = folded_operand(i->b, &src);
            m = emit(op, w, d, reg_of(i->a), reg_of(src));
            m->imm = sh;
            return;
        }
        if (i->op == OP_OR && fused[i->a] == FUSE_ROT) {
            u32 src;
            rotate_of(v, &src, &c);
            m = emit(A_RORI, w, d, reg_of(src), NOREG);
            m->imm = (i64)c;
            return;
        }
        goto binary;
    }
    case OP_SHL:
    case OP_SHR:
    case OP_SAR:
        if (fused[v])
            return;
        op = i->op == OP_SHL ? A_LSL : i->op == OP_SHR ? A_LSR : A_ASR;
        if (ir_is_const(f, i->b, &c)) {
            m = emit(op + (A_LSLI - A_LSL), w, d, reg_of(i->a), NOREG);
            m->imm = (i64)(c & (w ? 63 : 31));
            return;
        }
        goto binary;
    case OP_NEG: op = A_NEG; goto unary;
    case OP_ASMOUT:
        return; /* moved out of its register where the assembly is */
    case OP_CLZ: op = A_CLZ; goto unary;
    case OP_TLS:
        if (f->ins[i->a].op != OP_ADDR)
            die("internal: thread-local address of no symbol");
        m = emit(A_TLS, 1, d, NOREG, NOREG);
        m->sym = f->ins[i->a].a;
        m->imm = (i32)f->ins[i->a].b;
        return;
    case OP_BSWAP: op = A_REV; goto unary;
    case OP_CTZ: {
        u32 t = new_vreg(0);
        emit(A_RBIT, w, t, reg_of(i->a), NOREG);
        emit(A_CLZ, w, d, t, NOREG);
        return;
    }
    case OP_FRAMEADDR:
        m = emit(A_FRAMEADDR, 1, d, NOREG, NOREG);
        m->imm = i->a;
        if (i->a < 2)
            uses_fp = 1; /* needs a frame record of its own */
        return;
    case OP_NOT:
        if (fused[v] == FUSE_OPND)
            return; /* folded into bic, orn or eon */
        op = A_MVN;
        goto unary;
    case OP_FNEG: op = A_FNEG; goto unary;
    case OP_SEXT8: op = A_SXTB; goto ext;
    case OP_SEXT16: op = A_SXTH; goto ext;
    case OP_ZEXT8: op = A_UXTB; goto ext;
    case OP_ZEXT16: op = A_UXTH; goto ext;
    ext:
        if (fused[v] == FUSE_MOV) {
            emit(A_MOV, 1, d, reg_of(i->a), NOREG);
            return;
        }
        goto unary;
    case OP_SEXT32:
        if (fused[v])
            return;
        op = A_SXTW;
        goto unary;
    case OP_ZEXT32: {
        if (fused[v])
            return;
        op = A_UXTW;
        goto unary;
    }
    case OP_TRUNC:
        if (fused[v])
            return;
        op = A_MOV;
        goto unary;
    unary:
        emit(op, w, d, reg_of(i->a), NOREG);
        return;
    case OP_SITOF:
    case OP_UITOF:
        m = emit(i->op == OP_SITOF ? A_SCVTF : A_UCVTF, w, d, reg_of(i->a), NOREG);
        m->cc = f->ins[i->a].ty == IT_I64;
        return;
    case OP_FTOSI:
    case OP_FTOUI:
        m = emit(i->op == OP_FTOSI ? A_FCVTZS : A_FCVTZU, w, d, reg_of(i->a), NOREG);
        m->cc = f->ins[i->a].ty == IT_F64;
        return;
    case OP_FEXT:
    case OP_FTRUNC:
        emit(A_FCVT, w, d, reg_of(i->a), NOREG);
        return;
    case OP_LOAD:
        addr_of(i->a, &a, i->mem);
        emit_load(d, &a, (i->flags & 0x80) ? LOAD_SIGNED32 : i->mem);
        return;
    case OP_STORE: {
        u32 val;
        addr_of(i->a, &a, i->mem);
        if (ir_is_const(f, i->b, &c) && c == 0)
            val = R_ZR;
        else
            val = reg_of(i->b);
        emit_store(val, &a, i->mem);
        return;
    }
    case OP_CALL:
        sel_call(v);
        return;
    case OP_MEMCPY:
    case OP_MEMZERO:
        sel_block_op(v);
        return;
    case OP_VASTART:
        m = emit(A_SLOTADDR, 1, d, NOREG, NOREG);
        if (i->a == 0) {
            m->sym = ARGSLOT;
            m->imm = named_stack;
            uses_args = 1;
        } else {
            /* the end of the saved general (1) or floating (2) argument registers */
            m->sym = i->a == 2 && vr_slot != NOSLOT ? vr_slot : gr_slot;
            m->imm = i->a == 2 && vr_slot != NOSLOT ? 128 : 64;
        }
        return;
    case OP_PIN:
        move_to(d, i->a);
        return;
    case OP_SELECT: {
        u32 rb = reg_of(i->b), rc = reg_of(i->c);
        int cc = CC_NE;
        if (fused[i->a] == 1 || fused[i->a] == FUSE_COND) {
            cc = emit_cond(i->a);
        } else {
            m = emit(A_CMPI, f->ins[i->a].ty == IT_I64, NOREG, reg_of(i->a), NOREG);
            m->imm = 0;
        }
        m = emit(is_fp(i->ty) ? A_FCSEL : A_CSEL, w, d, rb, rc);
        m->cc = (u8)cc;
        return;
    }
    default:
        if (is_compare(i->op)) {
            if (fused[v])
                return;
            op = emit_compare(v);
            m = emit(A_CSET, 0, d, NOREG, NOREG);
            m->cc = (u8)op;
            return;
        }
        die("internal: instruction selection: op %d", i->op);
    }
}

/* Copies for the phis of successor s along the edge from block b. */
static void sel_phi_moves(u32 b, u32 s)
{
    Block *sb = &f->blk[s];
    u32 k, p, pred = 0;
    Buf tmp;
    u32 n = 0;

    if (!sb->phis)
        return;
    for (k = 0; k < sb->npreds; k++)
        if (sb->preds[k] == b)
            pred = k;
    memset(&tmp, 0, sizeof tmp);
    /* a source that is itself overwritten by this parallel copy is saved first */
    for (p = sb->phis; p; p = f->ins[p].next) {
        u32 src = f->xargs[f->ins[p].a + pred], t = NOREG;
        if (src != p && f->ins[src].op == OP_PHI && f->ins[src].blk == s) {
            t = new_vreg(cls_of(f->ins[src].ty));
            emit(A_MOV, 1, t, VREG0 + src, NOREG);
        }
        buf_put(&tmp, &t, sizeof t);
    }
    for (p = sb->phis; p; p = f->ins[p].next, n++) {
        u32 src = f->xargs[f->ins[p].a + pred], t;
        memcpy(&t, tmp.p + n * sizeof(u32), sizeof t);
        if (src == p)
            continue;
        if (t != NOREG)
            emit(A_MOV, 1, VREG0 + p, t, NOREG);
        else
            move_to(VREG0 + p, src);
    }
    buf_free(&tmp);
}

/* If branch condition v tests a single bit (x & bit, or that compared
   with zero), returns the "and"; else 0. */
static u32 bit_test(u32 v)
{
    Ins *i = &f->ins[v];
    u64 c;
    if ((i->op == OP_EQ || i->op == OP_NE) && ir_is_const(f, i->b, &c) && c == 0) {
        if (fused[v] != 1 || uses[v] != 1)
            return 0;
        v = i->a;
        i = &f->ins[v];
    }
    if (i->op != OP_AND || uses[v] != 1 || !ir_is_const(f, i->b, &c))
        return 0;
    if (i->ty != IT_I64)
        c = (u32)c;
    return c && !(c & (c - 1)) ? v : 0;
}

static void sel_term(u32 b, u32 next)
{
    Block *bb = &f->blk[b];
    MIns *m;
    switch (bb->term) {
    case TERM_JMP:
        sel_phi_moves(b, bb->succ[0]);
        if (bb->succ[0] != next) {
            m = emit(A_B, 0, NOREG, NOREG, NOREG);
            m->imm = blk_index[bb->succ[0]];
        }
        break;
    case TERM_BR: {
        u32 t = bb->succ[0], e = bb->succ[1], bitv = bit_test(bb->arg);
        u32 other = 0;
        u64 zero;
        int cc;
        if (t != next && e != next && f->blk[e].freq > f->blk[t].freq && f->blk[t].freq &&
            !(fused[bb->arg] == 1 && is_fp(f->ins[f->ins[bb->arg].a].ty))) {
            /* neither follows: the conditional branch goes to the likelier one */
            other = t;
            next = t;
        }
        if (bitv && fused[bitv] == FUSE_BIT) {
            /* one bit decides: tbz / tbnz */
            int op = bitv != bb->arg && f->ins[bb->arg].op == OP_EQ ? A_TBZ : A_TBNZ;
            u32 bit = 0;
            ir_is_const(f, f->ins[bitv].b, &zero);
            while (!(zero >> bit & 1))
                bit++;
            if (t == next) {
                op = op == A_TBZ ? A_TBNZ : A_TBZ;
                t = e;
                e = next;
            }
            m = emit(op, 0, NOREG, reg_of(f->ins[bitv].a), NOREG);
            m->imm = blk_index[t];
            m->cc = (u8)bit;
            if (e != next || other) {
                m = emit(A_B, 0, NOREG, NOREG, NOREG);
                m->imm = blk_index[other ? other : e];
            }
            break;
        }
        if (fused[bb->arg] == 1 && (f->ins[bb->arg].op == OP_EQ || f->ins[bb->arg].op == OP_NE) &&
            fused[f->ins[bb->arg].a] != FUSE_COND && ir_is_const(f, f->ins[bb->arg].b, &zero) && zero == 0) {
            /* compare with zero: cbz / cbnz */
            Ins *ci = &f->ins[bb->arg];
            int op = ci->op == OP_EQ ? A_CBZ : A_CBNZ;
            if (t == next) {
                op = op == A_CBZ ? A_CBNZ : A_CBZ;
                t = e;
                e = next;
            }
            m = emit(op, f->ins[ci->a].ty == IT_I64, NOREG, reg_of(ci->a), NOREG);
            m->imm = blk_index[t];
        } else if (fused[bb->arg] == 1 || fused[bb->arg] == FUSE_COND) {
            cc = emit_cond(bb->arg);
            if (t == next) {
                /* invert; unordered float compares must still fall to e */
                int ty = f->ins[f->ins[bb->arg].a].ty;
                if (fused[bb->arg] == 1 && is_fp(ty)) {
                    m = emit(A_BCC, 0, NOREG, NOREG, NOREG);
                    m->cc = (u8)cc;
                    m->imm = blk_index[t];
                    m = emit(A_B, 0, NOREG, NOREG, NOREG);
                    m->imm = blk_index[e];
                    break;
                }
                cc ^= 1;
                t = e;
                e = next;
            }
            m = emit(A_BCC, 0, NOREG, NOREG, NOREG);
            m->cc = (u8)cc;
            m->imm = blk_index[t];
        } else {
            int op = A_CBNZ;
            if (t == next) {
                op = A_CBZ;
                t = e;
                e = next;
            }
            m = emit(op, f->ins[bb->arg].ty == IT_I64, NOREG, reg_of(bb->arg), NOREG);
            m->imm = blk_index[t];
        }
        if (e != next || other) {
            m = emit(A_B, 0, NOREG, NOREG, NOREG);
            m->imm = blk_index[other ? other : e];
        }
        break;
    }
    case TERM_RET:
        m = 0;
        if (tail_blk == b)
            break;
        if (f->has_sret > 1) {
            Addr ra;
            ra.base = ra.idx = NOREG;
            ra.flags = 0;
            ra.slot = ret_slot;
            ra.off = 0;
            emit_load(0, &ra, M_64);
            if (f->has_sret > 9) {
                ra.off = 8;
                emit_load(1, &ra, M_64);
            }
            m = emit(A_RET, 0, NOREG, NOREG, NOREG);
            m->pmask = f->has_sret > 9 ? 3 : 1;
        } else if (bb->arg) {
            u32 r = is_fp(f->ins[bb->arg].ty) ? R_V0 : 0;
            move_to(r, bb->arg);
            m = emit(A_RET, 0, NOREG, NOREG, NOREG);
            m->pmask = (u64)1 << r;
        } else {
            emit(A_RET, 0, NOREG, NOREG, NOREG);
        }
        break;
    default:
        emit(A_RET, 0, NOREG, NOREG, NOREG);
        break;
    }
}

/* ---- IR preparation ---- */

static void split_critical_edges(void)
{
    u32 b, k, nblk = f->nblk;
    for (b = 1; b < nblk; b++) {
        u32 made[16], nmade = 0; /* positions among b's predecessors of the blocks made for it */
        if (!f->blk[b].phis || f->blk[b].npreds < 2)
            continue;
        for (k = 0; k < f->blk[b].npreds; k++) {
            u32 p = f->blk[b].preds[k], n = 0, s, j, v;
            if (f->blk[p].term != TERM_BR)
                continue;
            /* an edge that brings the same values as one already split shares its block */
            for (j = 0; j < nmade && !n; j++) {
                n = f->blk[b].preds[made[j]];
                for (v = f->blk[b].phis; v; v = f->ins[v].next)
                    if (f->xargs[f->ins[v].a + k] != f->xargs[f->ins[v].a + made[j]])
                        n = 0;
            }
            if (n && f->blk[p].succ[0] != f->blk[p].succ[1]) {
                ir_add_pred(f, n, p);
                for (s = 0; s < 2; s++)
                    if (f->blk[p].succ[s] == b)
                        f->blk[p].succ[s] = n;
                for (v = f->blk[b].phis; v; v = f->ins[v].next) {
                    for (j = k; j + 1 < f->ins[v].b; j++)
                        f->xargs[f->ins[v].a + j] = f->xargs[f->ins[v].a + j + 1];
                    f->ins[v].b--;
                }
                for (j = k; j + 1 < f->blk[b].npreds; j++)
                    f->blk[b].preds[j] = f->blk[b].preds[j + 1];
                f->blk[b].npreds--;
                k--;
                continue;
            }
            n = ir_new_block(f);
            f->blk[n].term = TERM_JMP;
            f->blk[n].succ[0] = b;
            f->blk[n].flag = 1;
            if (f->blk[p].freq && f->blk[b].freq)
                f->blk[n].freq = f->blk[p].freq < f->blk[b].freq ? f->blk[p].freq : f->blk[b].freq;
            ir_add_pred(f, n, p);
            f->blk[b].preds[k] = n;
            for (s = 0; s < 2; s++)
                if (f->blk[p].succ[s] == b)
                    f->blk[p].succ[s] = n;
            if (nmade < 16)
                made[nmade++] = k;
        }
    }
}

static u32 rpo_n;

static void rpo_visit(u32 b)
{
    /* iterative depth-first search; blocks are numbered in postorder */
    u32 *stack = (u32 *)xmalloc(f->nblk * 2 * sizeof(u32));
    u32 sp = 0;
    stack[sp++] = b;
    stack[sp++] = 0;
    f->blk[b].flag = 2;
    while (sp) {
        u32 k = stack[sp - 1], cb = stack[sp - 2];
        Block *bb = &f->blk[cb];
        u32 ns = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        if (k < ns) {
            /* visit the fall-through candidate (succ[1]) last so it follows in RPO */
            u32 s = bb->succ[ns - 1 - k];
            stack[sp - 1] = k + 1;
            if (f->blk[s].flag != 2) {
                f->blk[s].flag = 2;
                stack[sp++] = s;
                stack[sp++] = 0;
            }
        } else {
            layout[rpo_n++] = cb;
            sp -= 2;
        }
    }
    free(stack);
}

/* ---- layout from a profile ----
   Blocks are joined into chains along the edges taken most often, heaviest
   first, so the common path through the function falls through. Chains keep
   the order their first blocks had, except that those never executed go to
   the end of the function. */

typedef struct Edge {
    u32 w, from, to, n;
} Edge;

static int edge_cmp(const void *a, const void *b)
{
    const Edge *x = (const Edge *)a, *y = (const Edge *)b;
    if (x->w != y->w)
        return x->w > y->w ? -1 : 1;
    return x->n < y->n ? -1 : x->n > y->n;
}

#define COUNT(b) (f->blk[b].freq ? f->blk[b].freq - 1 : 1)

static void profile_layout(void)
{
    Edge *e = (Edge *)xmalloc((size_t)nlayout * 2 * sizeof(Edge));
    u32 *chain = (u32 *)xmalloc((size_t)f->nblk * 4 * sizeof(u32)); /* chain a block is in, by its first block */
    u32 *next = chain + f->nblk, *tail = next + f->nblk, *out = tail + f->nblk;
    u32 k, s, ne = 0, n = 0, pass;
    for (k = 0; k < nlayout; k++) {
        u32 b = layout[k];
        Block *bb = &f->blk[b];
        u32 ns = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        chain[b] = tail[b] = b;
        next[b] = 0;
        for (s = 0; s < ns; s++) {
            u32 t = bb->succ[s], w;
            if (ns == 2 && bb->succ[0] == bb->succ[1] && s)
                break;
            w = f->blk[t].npreds == 1 ? COUNT(t) : ns == 1 ? COUNT(b) : COUNT(b) < COUNT(t) ? COUNT(b) : COUNT(t);
            if (!w && (COUNT(b) || COUNT(t)))
                continue; /* nothing cold goes on the end of a path that runs */
            e[ne].w = w;
            e[ne].from = b;
            e[ne].to = t;
            e[ne].n = ne;
            ne++;
        }
    }
    qsort(e, ne, sizeof(Edge), edge_cmp);
    for (k = 0; k < ne; k++) {
        u32 a = e[k].from, b = e[k].to, ca = chain[a], cb = chain[b];
        if (b == 1 || ca == cb || tail[ca] != a || cb != b)
            continue;
        next[a] = b;
        tail[ca] = tail[cb];
        for (; b; b = next[b])
            chain[b] = ca;
    }
    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < nlayout; k++) {
            u32 b = layout[k], cold = 1;
            if (chain[b] != b)
                continue;
            for (s = b; s; s = next[s])
                if (COUNT(s))
                    cold = 0;
            if (cold != pass)
                continue;
            for (s = b; s; s = next[s])
                out[n++] = s;
        }
    }
    memcpy(layout, out, nlayout * sizeof(u32));
    free(e);
    free(chain);
}

/* How likely is the branch at the end of b to go to its first successor,
   in sixteenths? A guess from the shape of the code; the weights were fitted
   to profiles of mcc compiling itself, where they call 63% of executed
   branches right (a coin calls 45%, the profile itself 81%). */
static u32 branch_chance(u32 b)
{
    Block *bb = &f->blk[b];
    Ins *c = &f->ins[bb->arg];
    u32 n[2], st[2], cr[2], rt[2], s, v;
    int sc;
    switch (c->op) {
    case OP_EQ: sc = -3; break;
    case OP_NE: sc = 2; break;
    case OP_SLT: case OP_SLE: sc = 3; break;
    case OP_SGT: case OP_SGE: sc = 2; break;
    case OP_ULT: sc = 5; break;
    case OP_UGT: sc = 3; break;
    case OP_ULE: case OP_UGE: sc = 0; break;
    default: sc = -2;
    }
    for (s = 0; s < 2; s++) {
        u32 t = bb->succ[s], hops = 0, calls = 0;
        while (f->blk[t].flag == 1 && f->blk[t].term == TERM_JMP && !f->blk[t].first && hops++ < 4)
            t = f->blk[t].succ[0];
        n[s] = st[s] = 0;
        for (v = f->blk[t].first; v; v = f->ins[v].next) {
            n[s]++;
            calls += f->ins[v].op == OP_CALL;
            st[s] |= f->ins[v].op == OP_STORE;
        }
        rt[s] = f->blk[t].term == TERM_RET;
        cr[s] = calls && rt[s];
    }
    /* the smaller side, the one that stores nothing, the one that does not call and return */
    if (n[0] != n[1])
        sc += n[0] < n[1] ? 2 : -2;
    if (st[0] != st[1])
        sc += st[0] ? -2 : 2;
    if (cr[0] != cr[1])
        sc += cr[0] ? -4 : 4;
    if (rt[0] != rt[1])
        sc += rt[0] ? -1 : 1;
    sc += 8;
    return sc < 1 ? 1 : sc > 15 ? 15 : (u32)sc;
}

/* ---- frequencies without a profile ----
   Blocks are visited in reverse postorder, each passing its frequency on to
   its successors; an edge to a block not later in that order closes a loop,
   whose header is taken to run LOOP_TIMES as often as it is entered. */
#define LOOP_TIMES 8

static void estimate_freq(void)
{
    u32 *fr = (u32 *)xmalloc((size_t)f->nblk * 2 * sizeof(u32)), *pos = fr + f->nblk;
    u32 k, s;
    memset(fr, 0, f->nblk * sizeof(u32));
    for (k = 0; k < nlayout; k++)
        pos[layout[k]] = k;
    fr[1] = 1024;
    for (k = 0; k < nlayout; k++) {
        u32 b = layout[k], x = fr[b], p0;
        Block *bb = &f->blk[b];
        int header = 0, back0, back1;
        for (s = 0; s < bb->npreds; s++)
            if (pos[bb->preds[s]] >= k)
                header = 1;
        if (header)
            x = x > 0x4000000 ? x : x * LOOP_TIMES;
        if (!x)
            x = 1;
        fr[b] = x;
        if (bb->term == TERM_JMP) {
            if (pos[bb->succ[0]] > k)
                fr[bb->succ[0]] += x;
        } else if (bb->term == TERM_BR) {
            back0 = pos[bb->succ[0]] <= k;
            back1 = pos[bb->succ[1]] <= k;
            if (back0 && back1)
                continue;
            if (back0 || back1) {
                /* the way out of a loop */
                fr[bb->succ[back0 ? 1 : 0]] += x / LOOP_TIMES;
                continue;
            }
            p0 = branch_chance(b); /* of 16 */
            fr[bb->succ[0]] += (u32)((u64)x * p0 / 16);
            fr[bb->succ[1]] += (u32)((u64)x * (16 - p0) / 16);
        }
    }
    for (k = 0; k < nlayout; k++)
        f->blk[layout[k]].freq = fr[layout[k]] + 2; /* never "known not to run" */
    free(fr);
}

static void prepare(void)
{
    u32 b, v, k, n;
    split_critical_edges();
    layout = (u32 *)xmalloc(f->nblk * sizeof(u32));
    blk_index = (u32 *)xmalloc(f->nblk * sizeof(u32));
    rpo_n = 0;
    rpo_visit(1);
    nlayout = rpo_n;
    for (k = 0; k < nlayout / 2; k++) {
        u32 t = layout[k];
        layout[k] = layout[nlayout - 1 - k];
        layout[nlayout - 1 - k] = t;
    }
    if (!f->blk[1].freq && opt_level >= 3 && !opt_size)
        estimate_freq(); /* worth 1% in speed, costs 2% in size: not for the default */
    if (f->blk[1].freq > 1)
        profile_layout();
    for (k = 0; k < nlayout; k++)
        blk_index[layout[k]] = k;
    /* Loop rotation: a loop header that only tests the condition moves below
       the block that jumps back to it, so each iteration has one branch. */
    for (k = 1; k < nlayout; k++) {
        Block *lb = &f->blk[layout[k]];
        u32 h, hk;
        if (lb->term != TERM_JMP)
            continue;
        h = lb->succ[0];
        hk = blk_index[h];
        if (hk == 0 || hk >= k || f->blk[h].term != TERM_BR || f->blk[h].flag == 3)
            continue;
        f->blk[h].flag = 3; /* rotate each header once */
        for (n = hk; n < k; n++) {
            layout[n] = layout[n + 1];
            blk_index[layout[n]] = n;
        }
        layout[k] = h;
        blk_index[h] = k;
    }

    uses = (u32 *)xmalloc(f->nins * sizeof(u32));
    fused = (u8 *)xmalloc(f->nins);
    cache_reg = (u32 *)xmalloc(f->nins * sizeof(u32));
    cache_blk = (u32 *)xmalloc(f->nins * sizeof(u32));
    memset(uses, 0, f->nins * sizeof(u32));
    memset(fused, 0, f->nins);
    memset(cache_blk, 0, f->nins * sizeof(u32));
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        for (v = bb->phis; v; v = f->ins[v].next) {
            for (n = 0; n < f->ins[v].b; n++) {
                u32 x = f->xargs[f->ins[v].a + n];
                uses[x]++;
                fused[x] = FUSE_NOT;
            }
        }
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            int sel = i->op == OP_SELECT;
            u32 *ops;
            IR_OPS(f, i, ops, n);
            if (i->op == OP_CALL) {
                uses[i->c]++;
                fused[i->c] = FUSE_NOT;
            }
            for (b = 0; b < n; b++) {
                u32 x = ops[b];
                uses[x]++;
                if (b || !sel)
                    fused[x] = FUSE_NOT; /* used as a value, not as a condition */
            }
        }
        if (bb->arg) {
            uses[bb->arg]++;
            if (bb->term != TERM_BR)
                fused[bb->arg] = FUSE_NOT;
        }
    }
    /* a comparison used only as the condition of branches and selects is
       never made into a value: each of them sets the flags again */
    b = 0;
    for (v = 1; v < f->nins; v++) {
        int op = f->ins[v].op, cond = fused[v] != FUSE_NOT && uses[v];
        fused[v] = (u8)(cond && is_compare(op));
        /* a truncation leaves the low half where it is; so does extending
           what a 32-bit operation produced, since that cleared the upper half */
        if (uses[v] && f->ins[v].blk && (op == OP_TRUNC || (op == OP_ZEXT32 && clean32(f->ins[f->ins[v].a].op))))
            fused[v] = FUSE_ALIAS;
        if (cond && (op == OP_AND || op == OP_OR)) {
            fused[v] = FUSE_NOT; /* decided below, when all the comparisons are settled */
            b = 1;
        }
    }
    for (v = 1; b && v < f->nins; v++) {
        if (fused[v] == FUSE_NOT) {
            fused[v] = 0;
            if (cond_chain(v, 1))
                fused[v] = FUSE_COND;
        }
    }
    /* likewise a chain that a condition compares with zero */
    for (v = 1; v < f->nins; v++) {
        Ins *i = &f->ins[v];
        u64 c;
        if (fused[v] == 1 && (i->op == OP_EQ || i->op == OP_NE) && (f->ins[i->a].op == OP_AND || f->ins[i->a].op == OP_OR) &&
            ir_is_const(f, i->b, &c) && c == 0 && cond_chain(i->a, 0))
            fused[i->a] = FUSE_COND;
    }
    /* cache_reg doubles as a count of each value's uses as a memory address */
    memset(cache_reg, 0, f->nins * sizeof(u32));
    for (k = 0; k < nlayout; k++)
        for (v = f->blk[layout[k]].first; v; v = f->ins[v].next)
            if (f->ins[v].op == OP_LOAD || f->ins[v].op == OP_STORE)
                cache_reg[f->ins[v].a]++;
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        u64 c;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            if (i->op == OP_SEXT32 && !fused[v] && f->ins[i->a].op == OP_LOAD && f->ins[i->a].mem == M_32 &&
                uses[i->a] == 1) {
                /* load it sign-extended */
                f->ins[i->a].flags |= 0x80;
                fused[v] = FUSE_ALIAS;
                continue;
            }
            if (i->op >= OP_SEXT8 && i->op <= OP_ZEXT16 && f->ins[i->a].op == OP_LOAD && uses[i->a] == 1 &&
                (i->ty == IT_I32 || i->op == OP_ZEXT8 || i->op == OP_ZEXT16)) {
                /* an extension of a narrow load: load with that extension */
                Ins *l = &f->ins[i->a];
                int want = i->op == OP_SEXT8 ? M_8S : i->op == OP_ZEXT8 ? M_8U : i->op == OP_SEXT16 ? M_16S : M_16U;
                if ((l->mem ^ want) == 1) {
                    l->mem = (u8)want;
                    fused[v] = FUSE_MOV;
                }
                continue;
            }
            if (i->op == OP_LOAD || i->op == OP_STORE) {
                Ins *a = &f->ins[i->a];
                if (a->op != OP_ADD)
                    continue;
                if (ir_is_const(f, a->b, &c)) {
                    /* a constant offset folds into every access that uses it */
                    if ((i64)c > -0x40000000L && (i64)c < 0x40000000L && uses[i->a] == cache_reg[i->a])
                        fused[i->a] = FUSE_OFF;
                    continue;
                }
                if (uses[i->a] != cache_reg[i->a])
                    continue; /* also used as a value */
                if (f->ins[a->a].op != OP_SLOT && f->ins[a->b].op != OP_SLOT && !fused[i->a]) {
                    /* base + index, optionally scaled by the access size and sign-extended */
                    u32 size = i->mem <= M_8U ? 1 : i->mem <= M_16U ? 2 : i->mem == M_32 || i->mem == M_F32 ? 4 : 8;
                    u32 y = a->b, sh;
                    u64 k;
                    if (f->ins[y].op != OP_SHL && f->ins[a->a].op == OP_SHL)
                        y = a->a;
                    sh = y;
                    if (f->ins[y].op == OP_SHL && uses[y] == 1 && !fused[y] && ir_is_const(f, f->ins[y].b, &k) &&
                        ((u64)1 << k) == size && size > 1)
                        y = f->ins[y].a;
                    else
                        sh = 0;
                    if (!sh && y != a->b)
                        y = a->b;
                    fused[i->a] = FUSE_IDX;
                    if (sh)
                        fused[sh] = FUSE_SKIP;
                    if ((f->ins[y].op == OP_SEXT32 || f->ins[y].op == OP_ZEXT32) && uses[y] == 1 && !fused[y])
                        fused[y] = FUSE_SKIP;
                }
            }
        }
    }
    /* a branch on one bit; the branch reaches 32KB, so only in small functions */
    for (k = 0; k < nlayout && f->nins < 1500; k++) {
        Block *bb = &f->blk[layout[k]];
        if (bb->term == TERM_BR && (v = bit_test(bb->arg)) != 0 && !fused[v])
            fused[v] = FUSE_BIT;
    }
    /* a shift by a constant or a multiplication used once, by an add or
       subtract (or a shift used by a logical operation), folds into it */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        u64 c;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 t, y;
            int arith = i->op == OP_ADD || i->op == OP_SUB;
            if (fused[v] || !(arith || i->op == OP_AND || i->op == OP_OR || i->op == OP_XOR))
                continue;
            if (ir_is_const(f, i->b, &c) || ir_is_const(f, i->a, &c))
                continue;
            if (i->op == OP_OR && rotate_of(v, &y, &c) && uses[i->a] == 1 && uses[i->b] == 1 && !fused[i->a] &&
                !fused[i->b]) {
                fused[i->a] = fused[i->b] = FUSE_ROT;
                continue;
            }
            for (t = 0; t < 2; t++) {
                Ins *yi;
                int ok;
                y = t ? i->a : i->b;
                if (t && i->op == OP_SUB)
                    break;
                yi = &f->ins[y];
                if (uses[y] != 1 || fused[y] || yi->ty != i->ty || i->a == i->b)
                    continue;
                ok = (yi->op == OP_SHL || yi->op == OP_SHR || yi->op == OP_SAR) && ir_is_const(f, yi->b, &c) &&
                     c < (i->ty == IT_I64 ? 64 : 32);
                if (yi->op == OP_MUL && arith && yi->blk == i->blk)
                    ok = 1;
                if (!arith && (yi->op == OP_NOT || (yi->op == OP_OR && fused[yi->a] == FUSE_ROT)))
                    ok = 1;
                if (ok) {
                    fused[y] = FUSE_OPND;
                    if (t && !arith) { /* the logical operations read it as their second operand */
                        i->a = i->b;
                        i->b = y;
                    }
                    break;
                }
            }
        }
    }
    memset(cache_reg, 0, f->nins * sizeof(u32));
}

static void select_all(void)
{
    u32 k, v, n = 0;
    u8 tys[128];
    u32 locs[128];
    Addr a;

    mf.n = 0;
    mf.nblk = 0;
    mf.nvreg = 0;
    outargs = 0;
    has_calls = uses_args = uses_fp = 0;
    asm_saved = 0;
    uses_alloca = 0;
    free(alink);
    alink = 0;
    tail_blk = 0;
    no_slots = 1;
    {
        u32 sk;
        for (sk = 0; sk < f->nslots; sk++)
            if (f->slots[sk].size)
                no_slots = 0;
    }
    /* one virtual register per IR value */
    if (mf.capvreg < f->nins) {
        mf.capvreg = f->nins * 2;
        mf.vclass = (u8 *)xrealloc(mf.vclass, mf.capvreg);
    }
    has_vec = 0;
    for (v = 0; v < f->nins; v++) {
        mf.vclass[v] = (u8)cls_of(f->ins[v].ty);
        has_vec |= mf.vclass[v] & 2;
    }
    mf.nvreg = f->nins;
    if (mf.capblk < nlayout) {
        mf.capblk = nlayout * 2;
        mf.blk = (MBlock *)xrealloc(mf.blk, mf.capblk * sizeof(MBlock));
    }
    mf.nblk = nlayout;

    if (f->nparams > 127)
        die("too many parameters in %s", f->name);
    for (k = 0; k < f->nparams; k++)
        tys[k] = f->param_ty[k];
    named_stack = abi_assign(tys, f->nparams, f->nparams, f->has_sret, locs);
    ret_slot = gr_slot = vr_slot = NOSLOT;
    if (f->has_sret > 1) {
        ret_slot = ir_new_slot(f, 16, 8);
        no_slots = 0;
    }
    if (f->variadic && opt_target == TARGET_LINUX) {
        for (v = 1; v < f->nins; v++)
            if (f->ins[v].op == OP_VASTART && f->ins[v].blk) {
                gr_slot = ir_new_slot(f, 64, 8);
                if (!opt_no_fp)
                    vr_slot = ir_new_slot(f, 128, 16);
                no_slots = 0;
                break;
            }
    }

    for (k = 0; k < nlayout; k++) {
        u32 b = layout[k];
        Block *bb = &f->blk[b];
        MBlock *mb = &mf.blk[k];
        mb->first = mf.n;
        mb->freq = bb->freq;
        /* constants and addresses already in registers stay usable in a
           block entered only from the block just before it */
        if (!(k > 0 && bb->npreds == 1 && bb->preds[0] == layout[k - 1]))
            cur_blk = b;
        if (k == 0) {
            if (gr_slot != NOSLOT) {
                /* what came in registers is saved where va_arg finds it */
                u32 r;
                a.base = a.idx = NOREG;
                a.flags = 0;
                for (r = 0; r < 8; r++) {
                    a.slot = gr_slot;
                    a.off = 8 * r;
                    emit_store(r, &a, M_64);
                    if (vr_slot != NOSLOT) {
                        a.slot = vr_slot;
                        a.off = 16 * r;
                        emit_store(R_V0 + r, &a, M_F64);
                    }
                }
            }
            /* incoming arguments: registers first, then stack */
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op == OP_PARAM && i->a == 0 && f->has_sret > 1) {
                    MIns *sm = emit(A_SLOTADDR, 1, VREG0 + v, NOREG, NOREG);
                    sm->sym = ret_slot;
                } else if (i->op == OP_PARAM && !(locs[i->a] & LOC_STACK))
                    emit(A_MOV, 1, VREG0 + v, locs[i->a], NOREG);
            }
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op == OP_PARAM && (locs[i->a] & LOC_STACK)) {
                    a.base = NOREG;
                    a.idx = NOREG;
                    a.flags = 0;
                    a.slot = ARGSLOT;
                    a.off = locs[i->a] & ~(u32)LOC_STACK;
                    uses_args = 1;
                    emit_load(VREG0 + v, &a, ty_mem(i->ty));
                }
            }
        }
        for (v = bb->first; v; v = f->ins[v].next)
            sel_ins(v);
        sel_term(b, k + 1 < nlayout ? layout[k + 1] : 0);
        mb->n = mf.n - mb->first;
        mb->nsucc = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        for (n = 0; n < mb->nsucc; n++)
            mb->succ[n] = blk_index[bb->succ[n]];
    }
}

/* ---- encoding ---- */

typedef struct Fixup {
    u32 off;
    u32 target; /* layout index */
    u8 kind;    /* 0: b, 1: b.cond / cbz / cbnz */
} Fixup;

static Buf *text;
static Fixup *fixups;
static u32 nfixups, capfixups;
static u32 spill_base, save_base, frame_size;
static u64 saved_regs;

static void put(u32 insn)
{
    buf_u32(text, insn);
}

/* Encodes v as an AArch64 bitmask immediate (N:immr:imms), if possible. */
int arm64_logical_imm(u64 v, int w64, u32 *enc)
{
    u32 e, ones, r;
    u64 elt, mask, pat;
    if (!w64)
        v = (v & 0xffffffffUL) | (v << 32);
    if (v == 0 || v == ~(u64)0)
        return 0;
    for (e = 2; e < 64; e *= 2) {
        mask = ((u64)1 << e) - 1;
        elt = v & mask;
        /* periodic with period e? */
        for (r = e; r < 64; r += e)
            if (((v >> r) & mask) != elt)
                break;
        if (r >= 64)
            break;
    }
    mask = e == 64 ? ~(u64)0 : ((u64)1 << e) - 1;
    elt = v & mask;
    ones = 0;
    for (r = 0; r < e; r++)
        ones += (u32)(elt >> r) & 1;
    pat = ones == 64 ? ~(u64)0 : ((u64)1 << ones) - 1;
    for (r = 0; r < e; r++) {
        /* rotate elt right by r within e bits */
        u64 rot = r == 0 ? elt : ((elt >> r) | (elt << (e - r))) & mask;
        if (rot == pat) {
            u32 immr = (e - r) % e;
            u32 imms = ((~(e * 2 - 1)) & 0x3f) | (ones - 1);
            *enc = ((e == 64) << 12) | (immr << 6) | imms;
            return 1;
        }
    }
    return 0;
}

static void mov_imm(u32 rd, u64 v, int w64)
{
    u32 hw[4], k, n = w64 ? 4 : 2, nz = 0, nf = 0, first = 1;
    u32 sf = w64 ? 0x80000000U : 0;
    if (!w64)
        v &= 0xffffffffUL;
    for (k = 0; k < n; k++) {
        hw[k] = (u32)(v >> (16 * k)) & 0xffff;
        nz += hw[k] == 0;
        nf += hw[k] == 0xffff;
    }
    if (nf > nz) {
        for (k = 0; k < n; k++) {
            if (hw[k] == 0xffff)
                continue;
            if (first)
                put(sf | 0x12800000 | (k << 21) | ((~hw[k] & 0xffff) << 5) | rd); /* movn */
            else
                put(sf | 0x72800000 | (k << 21) | (hw[k] << 5) | rd); /* movk */
            first = 0;
        }
        if (first)
            put(sf | 0x12800000 | rd); /* all ones */
    } else {
        for (k = 0; k < n; k++) {
            if (hw[k] == 0)
                continue;
            put(sf | (first ? 0x52800000 : 0x72800000) | (k << 21) | (hw[k] << 5) | rd);
            first = 0;
        }
        if (first)
            put(sf | 0x52800000 | rd);
    }
}

/* rd = rn + imm; rn or rd may be sp. tmp is used for very large offsets. */
static void add_imm(u32 rd, u32 rn, i64 imm, u32 tmp)
{
    u32 op = 0x91000000;
    u64 u = (u64)imm;
    if (imm < 0) {
        op = 0xd1000000;
        u = 0 - u;
    }
    if (u < (1UL << 24)) {
        if (u >> 12) {
            put(op | 0x400000 | ((u32)(u >> 12) << 10) | (rn << 5) | rd);
            rn = rd;
        }
        if ((u & 0xfff) || !(u >> 12))
            put(op | ((u32)(u & 0xfff) << 10) | (rn << 5) | rd);
        return;
    }
    mov_imm(tmp, u, 1);
    put((imm < 0 ? 0xcb206000U : 0x8b206000U) | (tmp << 16) | (rn << 5) | rd); /* extended register form */
}

/* Which spill slot each integer scratch register currently holds (NOREG:
   unknown), so that back-to-back uses of a spilled value reload it once. */
static u32 scache[3];

static void scache_clear(void)
{
    scache[0] = scache[1] = scache[2] = NOREG;
}

/* Spill slot l has a new value, held by register r (or by none). */
static void scache_set(u32 l, u32 r)
{
    u32 k;
    for (k = 0; k < 3; k++)
        if (scache[k] == l)
            scache[k] = NOREG;
    if (r >= SCRATCH0 && r < SCRATCH0 + 3)
        scache[r - SCRATCH0] = l;
}

static void ldst(int mem, int store, u32 rt, u32 base, i64 off, u32 tmp)
{
    static const u32 load_op[] = { 0x39c00000, 0x39400000, 0x79c00000, 0x79400000,
                                   0xb9400000, 0xf9400000, 0xbd400000, 0xfd400000, 0xb9800000 };
    static const u32 store_op[] = { 0x39000000, 0x39000000, 0x79000000, 0x79000000,
                                    0xb9000000, 0xf9000000, 0xbd000000, 0xfd000000 };
    static const u8 sizes[] = { 1, 1, 2, 2, 4, 8, 4, 8, 4 };
    u32 op = store ? store_op[mem] : load_op[mem], size = sizes[mem];
    rt &= 31;
    if (off >= 0 && off % size == 0 && off / size < 4096) {
        put(op | ((u32)(off / size) << 10) | (base << 5) | rt);
    } else if (off >= -256 && off <= 255) {
        put((op & ~0x01000000U) | (((u32)off & 0x1ff) << 12) | (base << 5) | rt);
    } else {
        if (tmp >= SCRATCH0 && tmp < SCRATCH0 + 3)
            scache[tmp - SCRATCH0] = NOREG;
        mov_imm(tmp, (u64)off, 1);
        put((op & ~0x01000000U) | 0x00206800 | (tmp << 16) | (base << 5) | rt);
    }
}

static void mov_reg(u32 d, u32 s)
{
    if (d == s)
        return;
    if (d >= R_V0 && has_vec)
        put(0x4ea01c00 | ((s & 31) << 16) | ((s & 31) << 5) | (d & 31)); /* mov v.16b, v.16b */
    else if (d >= R_V0)
        put(0x1e604000 | ((s & 31) << 5) | (d & 31)); /* fmov d, d */
    else
        put(0xaa0003e0 | (s << 16) | d);
}

static void add_fixup(u32 target, int kind)
{
    if (nfixups == capfixups) {
        capfixups = capfixups ? capfixups * 2 : 256;
        fixups = (Fixup *)xrealloc(fixups, capfixups * sizeof(Fixup));
    }
    fixups[nfixups].off = (u32)text->len;
    fixups[nfixups].target = target;
    fixups[nfixups].kind = (u8)kind;
    nfixups++;
}

/* Saves or restores the callee-saved registers in use, in pairs where possible. */
static void save_restore(int restore)
{
    u32 regs[32], n = 0, k;
    for (k = 0; k < 64; k++)
        if (saved_regs >> k & 1)
            regs[n++] = k;
    for (k = 0; k < n; k++) {
        i64 off = fbias + (i64)save_base + 8 * k;
        if (k + 1 < n && (regs[k] >= R_V0) == (regs[k + 1] >= R_V0) && off >= 0 && off <= 504) {
            u32 op = regs[k] >= R_V0 ? 0x6d000000U : 0xa9000000U;
            if (restore)
                op |= 0x00400000;
            put(op | ((u32)(off / 8) << 15) | ((regs[k + 1] & 31) << 10) | (fbase << 5) | (regs[k] & 31));
            k++;
        } else {
            ldst(regs[k] >= R_V0 ? M_F64 : M_64, !restore, regs[k], fbase, off, 16);
        }
    }
}

static u32 epi_off; /* where this function's epilogue was first emitted, or 0 */

static void unframe(void)
{
    save_restore(1);
    if (frame_kind == 1) {
        put(0xa8c07bfd | (((frame_size / 8) & 0x7f) << 15)); /* ldp x29, x30, [sp], #frame */
    } else if (frame_kind == 2) {
        if (frame_size || uses_alloca)
            put(0x910003bf); /* mov sp, x29 */
        put(0xa8c17bfd);     /* ldp x29, x30, [sp], #16 */
    }
}

static int noframe; /* encoding a path that runs before the frame is made */

static void epilogue(void)
{
    u32 start = (u32)text->len;
    if (noframe) {
        put(0xd65f03c0); /* ret */
        return;
    }
    if (epi_off) {
        /* one copy serves every return */
        put(0x14000000 | ((u32)(((i64)epi_off - (i64)start) / 4) & 0x3ffffff));
        return;
    }
    unframe();
    put(0xd65f03c0); /* ret */
    if (text->len - start >= 12)
        epi_off = start; /* long enough to be worth a jump */
}

/* shifted-register operand: amount, kind (bits 22-23) and inversion (bit 21) from MIns.imm */
#define SHIFTED(imm) ((((u32)(imm) & 63) << 10) | (((u32)(imm) >> 6 & 3) << 22) | (((u32)(imm) >> 8 & 1) << 21))

/* Puts the operands into the text of an inline assembly statement and assembles it here. */
static void asm_emit(MIns *i)
{
    const AsmStmt *st = &asm_stmts[i->imm];
    u8 regs[32];
    Buf b;
    const char *t, *file;
    char tmp[16];
    u32 line, col, k;

    asm_assign(st, regs);
    memset(&b, 0, sizeof b);
    loc_decode(st->loc, &file, &line, &col);
    for (t = st->tmpl; *t; t++) {
        int mod = 0;
        const AsmOp *op;
        if (*t != '%' || *++t == '%') {
            buf_u8(&b, (u8)*t);
            continue;
        }
        if (((*t | 32) >= 'a' && (*t | 32) <= 'z') && (t[1] == '[' || (t[1] >= '0' && t[1] <= '9')))
            mod = *t++;
        if (*t == '[') {
            const char *e = strchr(t, ']');
            u32 name = e ? intern(t + 1, (size_t)(e - t - 1)) : 0;
            for (k = 0; k < st->nops; k++)
                if (st->ops[k].name == name && name)
                    break;
            t = e ? e : t;
        } else if (*t >= '0' && *t <= '9') {
            k = (u32)(*t - '0');
            if (t[1] >= '0' && t[1] <= '9')
                k = k * 10 + (u32)(*++t - '0');
        } else {
            k = st->nops;
        }
        if (k >= st->nops)
            die("%s:%u: error: bad operand reference in inline assembly", file, line);
        op = &st->ops[k];
        if (op->kind == 'i') {
            buf_put(&b, op->text, strlen(op->text));
        } else {
            sprintf(tmp, op->kind == 'm' || mod == 'a' ? "[x%u]" : mod == 'w' ? "w%u" : "x%u", (u32)regs[k]);
            buf_put(&b, tmp, strlen(tmp));
        }
    }
    buf_align(text, 4);
    if (opt_asm_text) {
        /* -S: the text is what was asked for */
        buf_put(opt_asm_text, b.p, b.len);
        buf_u8(opt_asm_text, '\n');
    } else {
        gas_text((char *)b.p, b.len, arm64_text_sec, file, line, 0);
    }
    buf_free(&b);
    text = &obj->sec[arm64_text_sec]; /* new sections may have moved the table */
    buf_align(text, 4);
}

static void encode(MIns *i)
{
    u32 src[3], s[3], d = NOREG, dloc = NOREG, k, nint = 0, nfp = 0, tmp, rd, rn, rm;
    u32 sf = i->w ? 0x80000000U : 0;
    u32 fsz = i->w ? 0x00400000U : 0; /* double precision bit */
    u64 used_scratch = 0;

    if (i->op == A_DEFREG)
        return;
    if (i->op == A_ASM) {
        asm_emit(i);
        scache_clear();
        return;
    }
    src[0] = i->s0;
    src[1] = i->s1;
    src[2] = i->s2;
    if (i->d != NOREG)
        dloc = i->d < VREG0 ? i->d : mf.loc[i->d - VREG0];

    /* register-to-register copy: spilled operands move directly */
    if (i->op == A_MOV) {
        u32 sloc = i->s0 < VREG0 ? i->s0 : mf.loc[i->s0 - VREG0];
        int fp = i->d >= VREG0 ? mf.vclass[i->d - VREG0] : i->d >= R_V0;
        if (dloc == NOREG)
            return; /* dead definition */
        if (dloc >= SPILL0 && sloc >= SPILL0) {
            u32 t = fp ? FSCRATCH0 : SCRATCH0;
            for (k = 0; k < 3 && !fp; k++)
                if (scache[k] == sloc)
                    break;
            if (!fp && k < 3) {
                t = SCRATCH0 + k; /* already loaded */
            } else {
                if (!fp)
                    scache[0] = NOREG;
                ldst(fp ? M_F64 : M_64, 0, t, fbase, fbias + (i64)spill_base + 8 * (sloc - SPILL0), 16);
            }
            ldst(fp ? M_F64 : M_64, 1, t, fbase, fbias + (i64)spill_base + 8 * (dloc - SPILL0), t == 16 ? 17 : 16);
            if (!fp)
                scache_set(dloc, t);
        } else if (dloc >= SPILL0) {
            ldst(fp ? M_F64 : M_64, 1, sloc, fbase, fbias + (i64)spill_base + 8 * (dloc - SPILL0), 16);
            scache_set(dloc, NOREG);
        } else if (sloc >= SPILL0) {
            ldst(fp ? M_F64 : M_64, 0, dloc, fbase, fbias + (i64)spill_base + 8 * (sloc - SPILL0), 16);
        } else {
            mov_reg(dloc, sloc);
        }
        return;
    }

    if (i->flags & MF_CALL)
        scache_clear();
    /* spilled integer operands that a scratch register already holds */
    for (k = 0; k < 3; k++) {
        u32 r = src[k], l, j;
        if (r == NOREG || r < VREG0 || (l = mf.loc[r - VREG0]) < SPILL0 || mf.vclass[r - VREG0])
            continue;
        for (j = 0; j < 3; j++)
            if (scache[j] == l)
                used_scratch |= (u64)1 << (SCRATCH0 + j);
    }
    for (k = 0; k < 3; k++) {
        u32 r = src[k], l;
        s[k] = NOREG;
        if (r == NOREG)
            continue;
        l = r < VREG0 ? r : mf.loc[r - VREG0];
        if (l >= SPILL0) {
            int fp = mf.vclass[r - VREG0];
            u32 t, j;
            if (fp) {
                t = FSCRATCH0 + nfp++;
                if (spill_base + 8 * (l - SPILL0) > 32000)
                    scache[2] = NOREG;
                ldst(M_F64, 0, t, fbase, fbias + (i64)spill_base + 8 * (l - SPILL0), 17);
            } else {
                for (j = 0; j < 3; j++)
                    if (scache[j] == l)
                        break;
                if (j < 3) {
                    t = SCRATCH0 + j;
                } else {
                    t = SCRATCH0;
                    while (used_scratch >> t & 1)
                        t++;
                    /* the scratch itself can hold a large offset while loading */
                    ldst(M_64, 0, t, fbase, fbias + (i64)spill_base + 8 * (l - SPILL0), t);
                    scache[t - SCRATCH0] = l;
                }
            }
            (void)nint;
            l = t;
        }
        if (l >= SCRATCH0 && l < SCRATCH0 + 3)
            used_scratch |= (u64)1 << l;
        s[k] = l;
    }
    tmp = SCRATCH0;
    while (used_scratch >> tmp & 1)
        tmp++;
    if (tmp > SCRATCH0 + 2)
        tmp = SCRATCH0; /* all three hold operands: the result may replace one */
    if (dloc != NOREG) {
        if (dloc >= SPILL0)
            d = (i->d >= VREG0 && mf.vclass[i->d - VREG0]) ? FSCRATCH0 : tmp;
        else
            d = dloc;
    } else if (i->d != NOREG) {
        /* unused definition of a virtual register: still needs somewhere to go */
        d = mf.vclass[i->d - VREG0] ? FSCRATCH0 : tmp;
    }
    if (d >= SCRATCH0 && d < SCRATCH0 + 3)
        scache[d - SCRATCH0] = NOREG; /* about to be overwritten */
    rd = d & 31;
    rn = s[0] & 31;
    rm = s[1] & 31;

    switch (i->op) {
    case A_MOVI:
        mov_imm(d, (u64)i->imm, i->w);
        break;
    case A_FMOVI:
        if (i->imm == 0) {
            put((i->w ? 0x9e6703e0U : 0x1e2703e0U) | rd);
        } else {
            u32 t = d == NOREG || tmp == d ? 16 : tmp;
            if (t >= SCRATCH0 && t < SCRATCH0 + 3)
                scache[t - SCRATCH0] = NOREG;
            mov_imm(t, (u64)i->imm, i->w);
            put((i->w ? 0x9e670000U : 0x1e270000U) | (t << 5) | rd);
        }
        break;
    case A_ADR:
        if (obj->syms[i->sym].weak && obj->syms[i->sym].sec == SEC_UNDEF) {
            /* a weak symbol that nothing may define has address 0, which is
               out of reach of the instructions below: its address is loaded */
            obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_GOTPAGE21, i->sym, 0);
            put(0x90000000 | rd);
            obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_GOTPAGEOFF12, i->sym, 0);
            put(0xf9400000 | (rd << 5) | rd);
            if (i->imm)
                add_imm(d, rd, i->imm, d);
            break;
        }
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_PAGE21, i->sym, i->imm);
        put(0x90000000 | rd);
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_PAGEOFF12, i->sym, i->imm);
        put(0x91000000 | (rd << 5) | rd);
        break;
    case A_ADRP:
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_PAGE21, i->sym, i->imm);
        put(0x90000000 | rd);
        break;
    case A_SLOTADDR:
        if (i->sym == ARGSLOT)
            add_imm(d, frame_kind == 1 ? R_SP : R_FP, (i64)arg_base + i->imm, d);
        else
            add_imm(d, fbase, fbias + (i64)f->slots[i->sym].off + i->imm, d);
        break;
    case A_ADD: put(sf | 0x0b000000 | (rm << 16) | SHIFTED(i->imm) | (rn << 5) | rd); break;
    case A_SUB: put(sf | 0x4b000000 | (rm << 16) | SHIFTED(i->imm) | (rn << 5) | rd); break;
    case A_MUL: put(sf | 0x1b007c00 | (rm << 16) | (rn << 5) | rd); break;
    case A_SDIV: put(sf | 0x1ac00c00 | (rm << 16) | (rn << 5) | rd); break;
    case A_UDIV: put(sf | 0x1ac00800 | (rm << 16) | (rn << 5) | rd); break;
    case A_AND: put(sf | 0x0a000000 | (rm << 16) | SHIFTED(i->imm) | (rn << 5) | rd); break;
    case A_ORR: put(sf | 0x2a000000 | (rm << 16) | SHIFTED(i->imm) | (rn << 5) | rd); break;
    case A_EOR: put(sf | 0x4a000000 | (rm << 16) | SHIFTED(i->imm) | (rn << 5) | rd); break;
    case A_LSL: put(sf | 0x1ac02000 | (rm << 16) | (rn << 5) | rd); break;
    case A_LSR: put(sf | 0x1ac02400 | (rm << 16) | (rn << 5) | rd); break;
    case A_ASR: put(sf | 0x1ac02800 | (rm << 16) | (rn << 5) | rd); break;
    case A_ADDI:
    case A_SUBI: {
        u32 op = i->op == A_ADDI ? 0x11000000 : 0x51000000;
        u64 u = (u64)i->imm;
        if (u >= 4096)
            put(sf | op | 0x400000 | ((u32)(u >> 12) << 10) | (rn << 5) | rd);
        else
            put(sf | op | ((u32)u << 10) | (rn << 5) | rd);
        break;
    }
    case A_ANDI: put(sf | 0x12000000 | ((u32)i->imm << 10) | (rn << 5) | rd); break;
    case A_ORRI: put(sf | 0x32000000 | ((u32)i->imm << 10) | (rn << 5) | rd); break;
    case A_EORI: put(sf | 0x52000000 | ((u32)i->imm << 10) | (rn << 5) | rd); break;
    case A_RORI: /* extr rd, rn, rn, #imm */
        put((i->w ? 0x93c00000U : 0x13800000U) | (rn << 16) | ((u32)i->imm << 10) | (rn << 5) | rd);
        break;
    case A_LSLI:
    case A_LSRI:
    case A_ASRI: {
        u32 bits = i->w ? 64 : 32, sh = (u32)i->imm, immr, imms;
        u32 op = i->op == A_ASRI ? 0x13000000 : 0x53000000;
        if (i->w)
            op |= 0x80400000U;
        if (i->op == A_LSLI) {
            immr = (bits - sh) % bits;
            imms = bits - 1 - sh;
        } else {
            immr = sh;
            imms = bits - 1;
        }
        put(op | (immr << 16) | (imms << 10) | (rn << 5) | rd);
        break;
    }
    case A_MSUB: put(sf | 0x1b008000 | (rm << 16) | ((s[2] & 31) << 10) | (rn << 5) | rd); break;
    case A_MADD: put(sf | 0x1b000000 | (rm << 16) | ((s[2] & 31) << 10) | (rn << 5) | rd); break;
    case A_SMULH: put(0x9b407c00 | (rm << 16) | (rn << 5) | rd); break;
    case A_UMULH: put(0x9bc07c00 | (rm << 16) | (rn << 5) | rd); break;
    case A_NEG: put(sf | 0x4b0003e0 | (rn << 16) | rd); break;
    case A_CLZ: put(sf | 0x5ac01000 | (rn << 5) | rd); break;
    case A_BRK: put(0xd4210000); break; /* brk #0x800 */
    case A_TLS:
        put(0xd53bd040 | rd); /* mrs xd, tpidr_el0 */
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_TPREL_HI12, i->sym, i->imm);
        put(0x91400000 | (rd << 5) | rd); /* add xd, xd, #:tprel_hi12:sym, lsl #12 */
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_TPREL_LO12, i->sym, i->imm);
        put(0x91000000 | (rd << 5) | rd); /* add xd, xd, #:tprel_lo12_nc:sym */
        break;
    case A_LDAR: put(0x08dffc00 | ((u32)i->cc << 30) | (rn << 5) | rd); break;
    case A_STLR: put(0x089ffc00 | ((u32)i->cc << 30) | (rn << 5) | rm); break;
    case A_DMB: put(0xd5033bbf); break; /* dmb ish */
    case A_ATOMIC: {
        /* x16 the old value, x17 the new one or the store's verdict, x15 the verdict when x17 is taken */
        u32 code = (u32)i->imm, kind = code & 255, sz = (code >> 8 & 3) << 30, wide = sz == 0xc0000000U ? 0x80000000U : 0;
        u32 ld = (code >> 12 & 1 ? 0x085f7c00U : 0x085ffc00U) | sz | (9 << 5);
        u32 st = (code >> 12 & 1 ? 0x08007c00U : 0x0800fc00U) | sz | (9 << 5);
        u32 top = (u32)text->len, back;
        scache_clear();
        put(ld | 16); /* ldaxr w16, [x9] */
        if (kind == ATOM_CAS) {
            put(wide | 0x6b0a021f);  /* cmp w16, w10 */
            put(0x54000061);         /* b.ne past the loop */
            put(st | (17 << 16) | 11); /* stlxr w17, w11, [x9] */
            k = 17;
        } else if (kind == ATOM_XCHG) {
            put(st | (17 << 16) | 10);
            k = 17;
        } else {
            static const u32 ops[] = {0x0b0a0211, 0x4b0a0211, 0x0a0a0211, 0x2a0a0211, 0x4a0a0211, 0x0a0a0211};
            put(wide | ops[kind - ATOM_ADD]); /* op w17, w16, w10 */
            if (kind == ATOM_NAND)
                put(wide | 0x2a3103f1); /* mvn w17, w17 */
            put(st | (15 << 16) | 17); /* stlxr w15, w17, [x9] */
            k = 15;
        }
        back = (top - (u32)text->len) >> 2;
        put(0x35000000 | ((back & 0x7ffff) << 5) | k); /* cbnz wk, top */
        put(0xaa1003e9); /* mov x9, x16 */
        break;
    }
    case A_ALLOCA:
        if (i->imm == 1) {
            put(0x910003e0 | rd); /* mov xd, sp */
            break;
        }
        if (i->imm == 2) {
            put(0x9100001f | (rn << 5)); /* mov sp, xn */
            scache_clear();
            break;
        }
        put(0x91003c10 | (rn << 5)); /* add x16, xn, #15 */
        put(0x927cee10);             /* and x16, x16, #-16 */
        put(0xcb3063ff);             /* sub sp, sp, x16 */
        add_imm(d, R_SP, (i64)align_up(outargs, 16), d);
        scache_clear();
        break;
    case A_RBIT: put(sf | 0x5ac00000 | (rn << 5) | rd); break;
    case A_REV: put((i->w ? 0xdac00c00U : 0x5ac00800U) | (rn << 5) | rd); break;
    case A_FRAMEADDR:
        if (i->imm == 0)
            put(0x910003a0 | rd); /* mov xd, x29 */
        else if (i->imm == 1)
            put(0xf94007a0 | rd); /* ldr xd, [x29, #8] */
        else if (i->imm == 3)
            put(0x910003e0 | rd); /* mov xd, sp */
        else
            put(0x10000000 | rd); /* adr xd, . */
        break;
    case A_MVN: put(sf | 0x2a2003e0 | (rn << 16) | rd); break;
    case A_SXTB: put(0x13001c00 | (rn << 5) | rd); break;
    case A_SXTH: put(0x13003c00 | (rn << 5) | rd); break;
    case A_SXTW: put(0x93407c00 | (rn << 5) | rd); break;
    case A_UXTB: put(0x53001c00 | (rn << 5) | rd); break;
    case A_UXTH: put(0x53003c00 | (rn << 5) | rd); break;
    case A_UXTW: put(0x2a0003e0 | (rn << 16) | rd); break; /* mov w, w */
    case A_CMP: put(sf | 0x6b00001f | (rm << 16) | (rn << 5)); break;
    case A_CMPI: put(sf | 0x7100001f | ((u32)i->imm << 10) | (rn << 5)); break;
    case A_CCMP:
        if (i->imm & 0x100)
            put(sf | 0x7a400800 | ((u32)(i->imm >> 16) << 16) | ((u32)i->cc << 12) | (rn << 5) | ((u32)i->imm & 15));
        else
            put(sf | 0x7a400000 | (rm << 16) | ((u32)i->cc << 12) | (rn << 5) | ((u32)i->imm & 15));
        break;
    case A_FCMP: put(0x1e202000 | fsz | (rm << 16) | (rn << 5)); break;
    case A_CSET: put(0x1a9f07e0 | ((u32)(i->cc ^ 1) << 12) | rd); break;
    case A_CSEL: put(sf | 0x1a800000 | (rm << 16) | ((u32)i->cc << 12) | (rn << 5) | rd); break;
    case A_FCSEL: put(0x1e200c00 | fsz | (rm << 16) | ((u32)i->cc << 12) | (rn << 5) | rd); break;
    case A_LDR:
    case A_STR: {
        int store = i->op == A_STR;
        u32 base = store ? s[1] : s[0];
        i64 off = i->imm;
        if (i->flags & MF_LO12) {
            obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_PAGEOFF12, i->sym, i->imm);
            off = 0;
        } else if (i->sym == ARGSLOT) {
            base = frame_kind == 1 ? R_SP : R_FP; /* a small frame leaves x29 alone */
            off += arg_base;
        } else if (i->sym != NOSLOT) {
            base = fbase;
            off += fbias + f->slots[i->sym].off;
        }
        if ((store ? s[2] : s[1]) != NOREG) {
            static const u32 lop[] = { 0x39c00000, 0x39400000, 0x79c00000, 0x79400000,
                                       0xb9400000, 0xf9400000, 0xbd400000, 0xfd400000, 0xb9800000 };
            static const u32 sop[] = { 0x39000000, 0x39000000, 0x79000000, 0x79000000,
                                       0xb9000000, 0xf9000000, 0xbd000000, 0xfd000000 };
            u32 idx = store ? s[2] : s[1], rt = (store ? s[0] : d) & 31;
            put(((store ? sop[i->cc] : lop[i->cc]) & ~0x01000000U) | 0x00200800 | (idx << 16) |
                ((i->flags & MF_SXTW) ? 0xc000U : (i->flags & MF_UXTW) ? 0x4000U : 0x6000U) | ((i->flags & MF_SCALE) ? 0x1000U : 0) | (base << 5) | rt);
        } else if (store) {
            u32 t = SCRATCH0;
            while (t == s[0] || t == base)
                t++;
            ldst(i->cc, 1, s[0], base, off, t);
        } else {
            /* the destination can hold a large offset, unless it is a float register or the base */
            ldst(i->cc, 0, d, base, off, d >= R_V0 || d == base ? (base == 16 ? 17 : 16) : d);
        }
        break;
    }
    case A_FADD: put(0x1e202800 | fsz | (rm << 16) | (rn << 5) | rd); break;
    case A_FSUB: put(0x1e203800 | fsz | (rm << 16) | (rn << 5) | rd); break;
    case A_FMUL: put(0x1e200800 | fsz | (rm << 16) | (rn << 5) | rd); break;
    case A_FDIV: put(0x1e201800 | fsz | (rm << 16) | (rn << 5) | rd); break;
    case A_FNEG: put(0x1e214000 | fsz | (rn << 5) | rd); break;
    case A_VLDR: put(0x3ce06800 | (rm << 16) | (rn << 5) | rd); break;
    case A_VSTR: put(0x3ca06800 | (rm << 16) | (rn << 5) | (s[2] & 31)); break;
    case A_VDUP: {
        u32 size = i->cc == M_F32 ? 2 : i->cc == M_F64 || i->cc == M_64 ? 3 : i->cc == M_32 ? 2 : i->cc >= M_16S ? 1 : 0;
        put((i->imm ? 0x4e000400 : 0x4e000c00) | ((u32)1 << size << 16) | (rn << 5) | rd);
        break;
    }
    case A_VSUM:
        if (i->cc == M_F64) {
            put(0x7e70d800 | (rn << 5) | rd); /* faddp d, v.2d */
        } else if (i->cc == M_F32) {
            put(0x6e20d400 | (rn << 16) | (rn << 5) | 31); /* faddp v31.4s, v.4s, v.4s */
            put(0x7e30d800 | (31 << 5) | rd);              /* faddp s, v31.2s */
        } else if (i->imm >= 5) {
            u32 size = i->cc >= M_16S ? 1 : 0;
            put((i->imm == 5 ? 0x6e303800 : 0x4e303800) | (size << 22) | (rn << 5) | 31); /* uaddlv, saddlv */
            put(0x1e260000 | (31 << 5) | rd);                                              /* fmov w, s31 */
            if (i->imm == 6 && !size)
                put(0x13003c00 | (rd << 5) | rd); /* sxth: sixteen bytes sum to sixteen bits */
        } else if (i->imm) {
            static const u32 across[] = { 0, 0x4e30a800, 0x4e31a800, 0x6e30a800, 0x6e31a800 }; /* smaxv sminv umaxv uminv */
            put(across[i->imm] | (2 << 22) | (rn << 5) | 31);
            put(0x1e260000 | (31 << 5) | rd); /* fmov w, s31 */
        } else if (i->cc == M_64) {
            put(0x5ef1b800 | (rn << 5) | 31); /* addp d31, v.2d */
            put(0x9e660000 | (31 << 5) | rd); /* fmov x, d31 */
        } else {
            u32 size = i->cc == M_32 ? 2 : i->cc >= M_16S ? 1 : 0;
            put(0x4e31b800 | (size << 22) | (rn << 5) | 31); /* addv */
            put(0x1e260000 | (31 << 5) | rd);                /* fmov w, s31 */
        }
        break;
    case A_VBIN: {
        u32 size = i->cc == M_64 ? 3 : i->cc == M_32 ? 2 : i->cc >= M_16S ? 1 : 0, sz = i->cc == M_F64, op;
        switch ((int)i->imm) {
        case OP_ADD: op = 0x4e208400 | (size << 22); break;
        case OP_SUB: op = 0x6e208400 | (size << 22); break;
        case OP_MUL: op = 0x4e209c00 | (size << 22); break;
        case OP_AND: op = 0x4e201c00; break;
        case OP_OR: op = 0x4ea01c00; break;
        case OP_XOR: op = 0x6e201c00; break;
        case OP_FADD: op = 0x4e20d400 | (sz << 22); break;
        case OP_FSUB: op = 0x4ea0d400 | (sz << 22); break;
        case OP_FMUL: op = 0x6e20dc00 | (sz << 22); break;
        case OP_FDIV: op = 0x6e20fc00 | (sz << 22); break;
        case OP_VSMAX: op = 0x4e206400 | (size << 22); break;
        case OP_VSMIN: op = 0x4e206c00 | (size << 22); break;
        case OP_VUMAX: op = 0x6e206400 | (size << 22); break;
        case OP_VUMIN: op = 0x6e206c00 | (size << 22); break;
        /* comparisons: "less" is "greater" with the operands exchanged; "not equal" is undone below */
        case OP_EQ: case OP_NE: op = 0x6e208c00 | (size << 22); break;
        case OP_SLT: k = rn; rn = rm; rm = k; /* fall through */
        case OP_SGT: op = 0x4e203400 | (size << 22); break;
        case OP_SLE: k = rn; rn = rm; rm = k; /* fall through */
        case OP_SGE: op = 0x4e203c00 | (size << 22); break;
        case OP_ULT: k = rn; rn = rm; rm = k; /* fall through */
        case OP_UGT: op = 0x6e203400 | (size << 22); break;
        case OP_ULE: k = rn; rn = rm; rm = k; /* fall through */
        case OP_UGE: op = 0x6e203c00 | (size << 22); break;
        case OP_FEQ: case OP_FNE: op = 0x4e20e400 | (sz << 22); break;
        case OP_FLT: k = rn; rn = rm; rm = k; /* fall through */
        case OP_FGT: op = 0x6ea0e400 | (sz << 22); break;
        case OP_FLE: k = rn; rn = rm; rm = k; /* fall through */
        case OP_FGE: op = 0x6e20e400 | (sz << 22); break;
        case OP_SITOF: op = 0x4e21d800 | ((u32)(i->cc == M_F64) << 22); rm = 0; break;
        case OP_UITOF: op = 0x6e21d800 | ((u32)(i->cc == M_F64) << 22); rm = 0; break;
        case OP_FTOSI: op = 0x4ea1b800 | ((u32)(i->cc == M_64) << 22); rm = 0; break;
        case OP_FTOUI: op = 0x6ea1b800 | ((u32)(i->cc == M_64) << 22); rm = 0; break;
        case OP_NEG: op = 0x6e20b800 | (size << 22); rm = 0; break;
        case OP_FNEG: op = 0x6ea0f800 | (sz << 22); rm = 0; break;
        /* shifts by a constant: the lane width and the count share a field */
        case OP_SHL: op = 0x4f005400; rm = ((u32)8 << size) + i->sym; break;
        case OP_SHR: op = 0x6f000400; rm = ((u32)16 << size) - i->sym; break;
        case OP_SAR: op = 0x4f000400; rm = ((u32)16 << size) - i->sym; break;
        default: die("internal: vector operation"); return;
        }
        put(op | (rm << 16) | (rn << 5) | rd);
        if (i->imm == OP_NE || i->imm == OP_FNE)
            put(0x6e205800 | (rd << 5) | rd); /* not */
        break;
    }
    case A_VIOTA: {
        u64 st = (u64)i->imm, lo, hi;
        if (i->cc == M_64) {
            lo = 0;
            hi = st;
        } else {
            lo = (st & 0xffffffffU) << 32;
            hi = ((st * 2) & 0xffffffffU) | ((st * 3) & 0xffffffffU) << 32;
        }
        scache[1] = NOREG;
        mov_imm(16, lo, 1);
        put(0x9e670000 | (16 << 5) | rd); /* fmov d, x16 */
        mov_imm(16, hi, 1);
        put(0x4e181c00 | (16 << 5) | rd); /* mov v.d[1], x16 */
        break;
    }
    case A_VSEL: {
        /* the instruction overwrites one of its three operands: pick the form whose victim is rd */
        u32 rc = s[2] & 31;
        if (rd == rm)
            put(0x6ee01c00 | (rn << 16) | (rc << 5) | rd); /* bif: take the other value where the mask is clear */
        else if (rd == rc)
            put(0x6ea01c00 | (rn << 16) | (rm << 5) | rd); /* bit: take it where the mask is set */
        else {
            if (rd != rn)
                put(0x4ea01c00 | (rn << 16) | (rn << 5) | rd);
            put(0x6e601c00 | (rc << 16) | (rm << 5) | rd); /* bsl */
        }
        break;
    }
    case A_SCVTF: put((i->cc ? 0x80000000U : 0) | 0x1e220000 | fsz | (rn << 5) | rd); break;
    case A_UCVTF: put((i->cc ? 0x80000000U : 0) | 0x1e230000 | fsz | (rn << 5) | rd); break;
    case A_FCVTZS: put(sf | 0x1e380000 | (i->cc ? 0x00400000U : 0) | (rn << 5) | rd); break;
    case A_FCVTZU: put(sf | 0x1e390000 | (i->cc ? 0x00400000U : 0) | (rn << 5) | rd); break;
    case A_FCVT:
        /* w is the destination precision */
        put((i->w ? 0x1e22c000U : 0x1e624000U) | (rn << 5) | rd);
        break;
    case A_B:
        add_fixup((u32)i->imm, 0);
        put(0x14000000);
        break;
    case A_BCC:
        add_fixup((u32)i->imm, 1);
        put(0x54000000 | i->cc);
        break;
    case A_TBZ:
    case A_TBNZ:
        add_fixup((u32)i->imm, 2);
        put((i->op == A_TBZ ? 0x36000000U : 0x37000000U) | ((u32)(i->cc >> 5) << 31) | ((u32)(i->cc & 31) << 19) | rn);
        break;
    case A_CBZ:
    case A_CBNZ:
        add_fixup((u32)i->imm, 1);
        put(sf | (i->op == A_CBZ ? 0x34000000U : 0x35000000U) | rn);
        break;
    case A_BL:
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_CALL26, i->sym, 0);
        put(0x94000000);
        break;
    case A_TAIL:
        if (frame_kind == 0 || !saved_regs) {
            unframe();
            obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_CALL26, i->sym, 0);
            put(0x14000000); /* b: the callee returns to our caller */
        } else {
            /* registers to restore: an ordinary call and the shared epilogue are shorter */
            obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_CALL26, i->sym, 0);
            put(0x94000000);
            epilogue();
        }
        break;
    case A_BLR:
        put(0xd63f0000 | (rn << 5));
        break;
    case A_RET:
        epilogue();
        break;
    default:
        die("internal: encode op %d", i->op);
    }

    if (i->flags & MF_CALL)
        scache_clear();
    if (dloc != NOREG && dloc >= SPILL0) {
        int fp = d >= R_V0;
        ldst(fp ? M_F64 : M_64, 1, d, fbase, fbias + (i64)spill_base + 8 * (dloc - SPILL0), d == 16 ? 17 : 16);
        scache_set(dloc, fp ? NOREG : d);
    }
}

static void prologue(void)
{
    if (frame_kind == 1) {
        /* x29 keeps pointing at the caller's frame record: sp does not move
           again, so everything here is addressed from it */
        put(0xa9807bfd | (((0 - frame_size / 8) & 0x7f) << 15)); /* stp x29, x30, [sp, #-frame]! */
    } else if (frame_kind == 2) {
        put(0xa9bf7bfd); /* stp x29, x30, [sp, #-16]! */
        put(0x910003fd); /* mov x29, sp */
        if (frame_size)
            add_imm(R_SP, R_SP, -(i64)frame_size, 16);
    }
    save_restore(0);
}

/* Does the instruction need the frame, the link register or a saved register? */
static int touches_frame(const MIns *i)
{
    u32 r[4], k;
    if ((i->flags & MF_CALL) || i->op == A_BL || i->op == A_BLR || i->op == A_TAIL || i->op == A_RET || i->op == A_SLOTADDR)
        return 1;
    if ((i->op == A_LDR || i->op == A_STR) && !(i->flags & MF_LO12) && i->sym != NOSLOT)
        return 1;
    r[0] = i->d;
    r[1] = i->s0;
    r[2] = i->s1;
    r[3] = i->s2;
    for (k = 0; k < 4; k++) {
        u32 l = r[k];
        if (l == NOREG)
            continue;
        if (l >= VREG0)
            l = mf.loc[l - VREG0];
        if (l >= 64 || (saved_regs >> l & 1) || (l >= 29 && l <= 31))
            return 1;
    }
    return 0;
}

static int is_cond_branch(int op)
{
    return op == A_BCC || op == A_CBZ || op == A_CBNZ || op == A_TBZ || op == A_TBNZ;
}

static u32 rloc(u32 r);

/* parameter copies into saved registers that wait until the frame exists */
static u32 late[8], nlate;

/* A short path from block b to a return that only moves values between
   scratch registers can run without the frame. With emit set, encodes it.
   A read of a parameter copy that has not been made yet reads the argument
   register instead. */
static int early_return(u32 b, int emit_it)
{
    u32 count = 0, hops = 0, n, j;
    u8 gone[8];
    memset(gone, 0, sizeof gone);
    while (b < mf.nblk && hops++ < 4) {
        MBlock *mb = &mf.blk[b];
        u32 nextb = b + 1;
        for (n = mb->first; n < mb->first + mb->n; n++) {
            MIns *i = &mf.ins[n];
            if (i->op == A_RET) {
                if (emit_it)
                    encode(i);
                return 1;
            }
            if (i->op == A_B) {
                nextb = (u32)i->imm;
                break;
            }
            if (i->op != A_MOV && i->op != A_MOVI && i->op != A_FMOVI)
                return 0;
            {
                MIns t = *i;
                u32 dl = t.d == NOREG || t.d < VREG0 ? t.d : mf.loc[t.d - VREG0];
                for (j = 0; j < nlate; j++) {
                    MIns *m = &mf.ins[late[j]];
                    if (t.op == A_MOV && t.s0 == m->d) {
                        if (gone[j])
                            return 0;
                        t.s0 = m->s0;
                    }
                    if (dl == m->s0)
                        gone[j] = 1; /* the argument register is overwritten */
                    if (t.d == m->d)
                        return 0;
                }
                if (touches_frame(&t) || ++count > 6)
                    return 0;
                if (emit_it && !(t.op == A_MOV && t.d != NOREG && rloc(t.d) == rloc(t.s0)))
                    encode(&t);
            }
        }
        b = nextb;
    }
    return 0;
}

/* ---- load and store pairs ----
   Two neighbouring loads, or stores, of the same size at neighbouring
   offsets from the same base become one ldp or stp. */
static int pair_part(const MIns *i, u32 *rt, u32 *base, i64 *off)
{
    u32 v = i->op == A_STR ? i->s0 : i->d, b = i->op == A_STR ? i->s1 : i->s0, x = i->op == A_STR ? i->s2 : i->s1;
    if ((i->op != A_LDR && i->op != A_STR) || (i->cc != M_32 && i->cc != M_64) || (i->flags & MF_LO12) ||
        x != NOREG || v == NOREG)
        return 0;
    v = rloc(v);
    if (v >= R_V0 || (v >= SCRATCH0 && v < SCRATCH0 + 3))
        return 0;
    *off = i->imm;
    if (i->sym == ARGSLOT) {
        b = frame_kind == 1 ? R_SP : R_FP;
        *off += arg_base;
    } else if (i->sym != NOSLOT) {
        b = fbase;
        *off += fbias + f->slots[i->sym].off;
    } else {
        if (b == NOREG)
            return 0;
        b = rloc(b);
        if (b >= R_V0 || (b >= SCRATCH0 && b < SCRATCH0 + 3))
            return 0;
    }
    *rt = v;
    *base = b;
    return 1;
}

/* Encodes a and b as a pair if they allow it. */
static int encode_pair(const MIns *a, const MIns *b)
{
    u32 ra, rb, ba, bb, t;
    i64 oa, ob, size = a->cc == M_64 ? 8 : 4, lo;
    if (a->op != b->op || a->cc != b->cc || !pair_part(a, &ra, &ba, &oa) || !pair_part(b, &rb, &bb, &ob) || ba != bb)
        return 0;
    lo = oa < ob ? oa : ob;
    if ((oa > ob ? oa - ob : ob - oa) != size || lo % size || lo / size < -64 || lo / size > 63)
        return 0;
    /* a load must not change the base before the other load uses it, and
       the two loads need different destinations */
    if (a->op == A_LDR && (ra == rb || (ra == ba && ba != R_SP)))
        return 0;
    if (ob < oa) {
        t = ra;
        ra = rb;
        rb = t;
    }
    put((size == 8 ? 0xa9000000U : 0x29000000U) | (a->op == A_LDR ? 0x00400000U : 0) |
        (((u32)(lo / size) & 0x7f) << 15) | ((rb & 31) << 10) | (ba << 5) | (ra & 31));
    return 1;
}

/* ---- tail merging ----
   Blocks that go to the same place often end with the same instructions
   (same operation on the same registers). One of them keeps the code; the
   others jump into it. */
static u32 *tm_head, *tm_next, *tm_end, *tm_cut, *tm_lab, *lab_at;
static u32 tm_capblk, tm_capins;

static u32 rloc(u32 r)
{
    return r == NOREG || r < VREG0 ? r : mf.loc[r - VREG0];
}

static int nop_move(const MIns *i)
{
    return i->op == A_MOV && i->d != NOREG && rloc(i->d) == rloc(i->s0);
}

static int same_ins(const MIns *a, const MIns *b)
{
    return a->op == b->op && a->w == b->w && a->cc == b->cc && a->flags == b->flags && a->sym == b->sym &&
           a->imm == b->imm && a->pmask == b->pmask && rloc(a->d) == rloc(b->d) && rloc(a->s0) == rloc(b->s0) &&
           rloc(a->s1) == rloc(b->s1) && rloc(a->s2) == rloc(b->s2);
}

/* Number of code-producing instructions the tails of blocks h and g share;
   *hp and *gp get the first shared instruction of each. */
static u32 common_tail(u32 h, u32 g, u32 *hp, u32 *gp)
{
    u32 i = tm_end[h], j = tm_end[g], lo_i = mf.blk[h].first, lo_j = mf.blk[g].first, n = 0;
    for (;;) {
        while (i > lo_i && nop_move(&mf.ins[i - 1]))
            i--;
        while (j > lo_j && nop_move(&mf.ins[j - 1]))
            j--;
        if (i == lo_i || j == lo_j || is_cond_branch(mf.ins[i - 1].op) || !same_ins(&mf.ins[i - 1], &mf.ins[j - 1]))
            break;
        i--;
        j--;
        n++;
        *hp = i;
        *gp = j;
    }
    return n;
}

static void merge_tails(const u32 *fwd, int skip_first)
{
    u32 k, nlab = 0, key_ret = mf.nblk + 1;
    if (tm_capblk < mf.nblk + 2) {
        tm_capblk = (mf.nblk + 2) * 2;
        tm_head = (u32 *)xrealloc(tm_head, tm_capblk * sizeof(u32));
        tm_next = (u32 *)xrealloc(tm_next, tm_capblk * sizeof(u32));
        tm_end = (u32 *)xrealloc(tm_end, tm_capblk * sizeof(u32));
        tm_cut = (u32 *)xrealloc(tm_cut, tm_capblk * sizeof(u32));
        tm_lab = (u32 *)xrealloc(tm_lab, tm_capblk * sizeof(u32));
    }
    if (tm_capins < mf.n + 1) {
        tm_capins = (mf.n + 1) * 2;
        lab_at = (u32 *)xrealloc(lab_at, tm_capins * sizeof(u32));
    }
    memset(tm_head, 0xff, (mf.nblk + 2) * sizeof(u32));
    memset(lab_at, 0, (mf.n + 1) * sizeof(u32));
    for (k = 0; k < mf.nblk; k++) {
        MBlock *mb = &mf.blk[k];
        MIns *last = &mf.ins[mb->first + mb->n - 1];
        u32 key, h, best = 0, bh = 0, bg = 0, tries = 0;
        tm_cut[k] = NOREG;
        if ((mb->nsucc & 16) || mb->n < 2 || (k == 0 && skip_first))
            continue;
        tm_end[k] = mb->first + mb->n - 1;
        if (last->op == A_RET) {
            key = key_ret;
        } else if (last->op == A_B) {
            key = fwd[(u32)last->imm];
        } else if (!is_cond_branch(last->op) && last->op != A_TAIL && (mb->nsucc & 3) == 1 && k + 1 < mf.nblk &&
                   !(mf.blk[k + 1].nsucc & 16)) {
            key = fwd[k + 1]; /* falls into the next block */
            tm_end[k] = mb->first + mb->n;
        } else {
            continue;
        }
        for (h = tm_head[key]; h != NOREG && tries++ < 6; h = tm_next[h]) {
            u32 hp = 0, gp = 0, n = common_tail(h, k, &hp, &gp);
            if (n > best) {
                best = n;
                bh = hp;
                bg = gp;
            }
        }
        if (best >= 2) {
            if (!lab_at[bh])
                lab_at[bh] = mf.nblk + 1 + nlab++;
            tm_lab[k] = lab_at[bh];
            tm_cut[k] = bg;
        } else {
            tm_next[k] = tm_head[key];
            tm_head[key] = k;
        }
    }
}

void arm64_gen(Func *fn, Obj *o)
{
    u32 k, n, off, start, nsaved, pass;
    u32 *blk_off, *fwd;
    u32 wrap_ret = NOREG, wrap_other = 0, wrap_br = 0; /* shrink-wrapping: see below */
    int wrap_invert = 0;

    if (opt_arch == ARCH_X64) {
        x64_gen(fn, o);
        return;
    }
    opt_finish(fn);
    f = fn;
    obj = o;
    text = &o->sec[arm64_text_sec];
    fn_serial++;
    epi_off = 0;
    prepare();
    select_all();
    ra_run(&mf, &target);
    if (has_vec) {
        u32 r;
        for (r = 0; r < mf.nvreg; r++)
            if ((mf.vclass[r] & 2) && mf.loc[r] >= SPILL0)
                die("internal: vector register spilled in %s", f->name);
    }

    /* Frame layout, from sp upwards: outgoing arguments, slots, spills, saved
       registers. Small frames without outgoing stack arguments are pushed
       together with the fp/lr pair, which then sits at the bottom. */
    saved_regs = (mf.used | (asm_saved & target.allocatable[0])) & ~target.caller_saved;
    nsaved = 0;
    for (k = 0; k < 64; k++)
        nsaved += (u32)(saved_regs >> k & 1);
    for (pass = 0; pass < 2; pass++) {
        off = pass == 0 ? 16 : (u32)align_up(outargs, 16);
        for (k = 0; k < f->nslots; k++) {
            Slot *s = &f->slots[k];
            if (!s->size)
                continue;
            off = (u32)align_up(off, s->align ? s->align : 1);
            s->off = (i32)off;
            off += s->size;
        }
        spill_base = (u32)align_up(off, 8);
        save_base = spill_base + 8 * mf.nspill;
        frame_size = (u32)align_up(save_base + 8 * nsaved, 16);
        if (pass == 0) {
            if (frame_size == 16 && !has_calls && !uses_args && !uses_fp) {
                frame_kind = 0; /* leaf with nothing to keep: no frame at all */
                frame_size = 0;
                break;
            }
            if (outargs == 0 && frame_size <= 504 && !uses_fp) {
                frame_kind = 1;
                arg_base = frame_size;
                break;
            }
        } else {
            frame_kind = 2;
            arg_base = 16;
        }
    }

    fbase = R_SP;
    fbias = 0;
    if (uses_alloca) {
        fbase = R_FP;
        fbias = -(i64)frame_size;
    }
    buf_align(text, 4);
    if (o->align[arm64_text_sec] < 4)
        o->align[arm64_text_sec] = 4;
    start = (u32)text->len;
    o->syms[f->sym].sec = (u16)arm64_text_sec;
    o->syms[f->sym].kind = 1;
    o->syms[f->sym].value = start;

    /* Shrink-wrapping, the common case only: the first block ends in a
       conditional branch, and one side reaches a return through nothing
       but moves between scratch registers ("if (!p) return 0;"). Then the
       frame is made after that branch, and the early return costs a
       compare, a branch and a ret. */
    nlate = 0;
    if (frame_kind != 0 && mf.nblk > 1 && mf.blk[0].n) {
        MBlock *mb = &mf.blk[0];
        u32 last = mb->first + mb->n - 1, br = last, other = 1;
        int ok = 1;
        if (mf.ins[last].op == A_B && mb->n >= 2) {
            other = (u32)mf.ins[last].imm;
            br = last - 1;
        }
        if (!is_cond_branch(mf.ins[br].op))
            ok = 0;
        for (k = 0; ok && k < mf.nblk; k++) /* the first block must run once */
            for (n = 0; n < (mf.blk[k].nsucc & 3); n++)
                if (mf.blk[k].succ[n] == 0)
                    ok = 0;
        /* A parameter that lives across calls is copied to a saved register
           first thing. Such copies wait until the frame exists; up to the
           branch the block reads the argument register instead. */
        for (pass = 0; pass < 2 && ok; pass++) {
            nlate = 0;
            for (n = mb->first; ok && n <= br; n++) {
                MIns t = mf.ins[n];
                u32 *src[3], dl, j;
                dl = t.d == NOREG || t.d < VREG0 ? t.d : mf.loc[t.d - VREG0];
                if (t.op == A_MOV && t.d != NOREG && t.d >= VREG0 && dl < 64 && (saved_regs >> dl & 1) && t.s0 < 29 &&
                    (target.caller_saved >> t.s0 & 1) && nlate < 8 && n < br) {
                    for (j = 0; j < nlate; j++)
                        if (mf.ins[late[j]].d == t.d)
                            ok = 0;
                    late[nlate++] = n;
                    continue;
                }
                src[0] = &t.s0;
                src[1] = &t.s1;
                src[2] = &t.s2;
                for (j = 0; j < nlate; j++) {
                    MIns *m = &mf.ins[late[j]];
                    for (k = 0; k < 3; k++)
                        if (*src[k] == m->d)
                            *src[k] = m->s0;
                    if (dl == m->s0 || t.d == m->d)
                        ok = 0; /* the argument register or the copy changes */
                }
                if (touches_frame(&t) || (n < br && (is_cond_branch(t.op) || t.op == A_B)))
                    ok = 0;
                if (pass == 1)
                    mf.ins[n] = t;
            }
            if (pass == 0 && ok) {
                u32 taken = (u32)mf.ins[br].imm;
                if (early_return(taken, 0)) {
                    wrap_ret = taken;
                    wrap_other = other;
                } else if (early_return(other, 0)) {
                    wrap_ret = other;
                    wrap_other = taken;
                    wrap_invert = 1;
                } else {
                    ok = 0;
                }
                wrap_br = br;
            }
        }
        if (!ok)
            wrap_ret = NOREG;
    }
    if (wrap_ret == NOREG)
        prologue();

    /* beyond the blocks: one entry for the early return, then tail-merge labels */
    blk_off = (u32 *)xmalloc((mf.nblk + 1) * 4 * sizeof(u32));
    fwd = blk_off + (mf.nblk + 1) * 2;
    for (k = mf.nblk; k < (mf.nblk + 1) * 2; k++)
        fwd[k] = k;
    /* a block that turned out to hold nothing but a jump is bypassed */
    for (k = 0; k < mf.nblk; k++) {
        MBlock *mb = &mf.blk[k];
        fwd[k] = k;
        mb->nsucc &= 3;
        for (n = mb->first; n < mb->first + mb->n; n++) {
            MIns *i = &mf.ins[n];
            if (i->op == A_B && n == mb->first + mb->n - 1) {
                fwd[k] = (u32)i->imm;
                mb->nsucc |= 8; /* nothing but a jump */
                n++;
                break;
            } else if (i->op == A_RET && n == mb->first + mb->n - 1) {
                mb->nsucc |= 32; /* nothing but a return */
                break;
            } else if (i->op != A_MOV || i->d == NOREG ||
                       (i->d < VREG0 ? i->d : mf.loc[i->d - VREG0]) != (i->s0 < VREG0 ? i->s0 : mf.loc[i->s0 - VREG0])) {
                break;
            }
        }
        /* nsucc bit 2: the block emits no code and falls through */
        if (n == mb->first + mb->n && fwd[k] == k)
            mb->nsucc |= 4;
    }
    for (k = 0; k < mf.nblk; k++) {
        u32 t = fwd[k], hops = 0;
        while (fwd[t] != t && hops++ < 8)
            t = fwd[t];
        fwd[k] = t;
    }
    /* Jumps to a block that only jumps go straight to where it goes, so such
       a block is reached only by falling into it. After a block that ends
       in a jump or a return it is dead. */
    for (k = 0; k < mf.nblk; k++)
        mf.blk[fwd[k]].nsucc |= 64; /* still a target (a long chain of jumps is not followed to its end) */
    for (k = 1; k < mf.nblk; k++) {
        MBlock *pb = &mf.blk[k - 1];
        if ((mf.blk[k].nsucc & (8 | 64)) == 8 && fwd[k] != k && pb->n) {
            int op = mf.ins[pb->first + pb->n - 1].op;
            if (op == A_B || op == A_RET || op == A_TAIL)
                mf.blk[k].nsucc |= 4 | 16;
        }
    }
    if (wrap_ret != NOREG) {
        /* blocks of the early return that nothing else reaches are not emitted in place */
        u32 b = wrap_ret, from = 0, hops = 0;
        while (b < mf.nblk && hops++ < 4) {
            MBlock *mb = &mf.blk[b];
            u32 preds = 0, nextb = b + 1, last;
            for (k = 0; k < mf.nblk; k++)
                for (n = 0; n < (mf.blk[k].nsucc & 3); n++)
                    preds += mf.blk[k].succ[n] == b && k != from;
            if (preds || b == 0)
                break;
            last = mb->first + mb->n - 1;
            if (mb->n && mf.ins[last].op == A_RET)
                nextb = mf.nblk;
            else if (mb->n && mf.ins[last].op == A_B)
                nextb = (u32)mf.ins[last].imm;
            mb->nsucc |= 4 | 16;
            from = b;
            b = nextb;
        }
    }
    merge_tails(fwd, wrap_ret != NOREG);
    nfixups = 0;
    for (k = 0; k < mf.nblk; k++) {
        MBlock *mb = &mf.blk[k];
        blk_off[k] = (u32)text->len;
        scache_clear();
        if (mb->nsucc & 16)
            continue; /* a jump its predecessor took over */
        /* a conditional branch over a block that only jumps: branch on the
           opposite condition to where that block goes, and drop it */
        if (k == 0 && wrap_ret != NOREG) {
            MIns c, j;
            u32 t = 1, target = fwd[wrap_other];
            for (n = mb->first; n < wrap_br; n++) {
                for (t = 0; t < nlate && late[t] != n; t++)
                    ;
                if (t == nlate)
                    encode(&mf.ins[n]);
            }
            t = 1;
            c = mf.ins[wrap_br];
            if (wrap_invert) {
                if (c.op == A_BCC)
                    c.cc ^= 1;
                else
                    c.op = (u16)(c.op == A_CBZ ? A_CBNZ : c.op == A_CBNZ ? A_CBZ : c.op == A_TBZ ? A_TBNZ : A_TBZ);
            }
            c.imm = mf.nblk;
            encode(&c);
            prologue();
            for (n = 0; n < nlate; n++)
                encode(&mf.ins[late[n]]);
            while (t < target && (mf.blk[t].nsucc & 4))
                t++;
            if (t != target) {
                memset(&j, 0, sizeof j);
                j.op = A_B;
                j.d = j.s0 = j.s1 = j.s2 = NOREG;
                j.sym = NOSLOT;
                j.imm = wrap_other;
                if (mf.blk[target].nsucc & 32)
                    epilogue();
                else
                    encode(&j);
            }
            continue;
        }
        if (mb->n && k + 2 < mf.nblk && (mf.blk[k + 1].nsucc & 8)) {
            MIns *i = &mf.ins[mb->first + mb->n - 1];
            if (i->op == A_BCC || i->op == A_CBZ || i->op == A_CBNZ || i->op == A_TBZ || i->op == A_TBNZ) {
                u32 t = k + 2, target = fwd[(u32)i->imm];
                while (t < target && (mf.blk[t].nsucc & 4))
                    t++;
                if (t == target && fwd[k + 1] != k + 1) {
                    if (i->op == A_BCC)
                        i->cc ^= 1;
                    else
                        i->op = (u16)(i->op == A_CBZ ? A_CBNZ : i->op == A_CBNZ ? A_CBZ : i->op == A_TBZ ? A_TBNZ : A_TBZ);
                    i->imm = fwd[k + 1];
                    mf.blk[k + 1].nsucc |= 16;
                }
            }
        }
        for (n = mb->first; n < mb->first + mb->n; n++) {
            MIns *i = &mf.ins[n];
            if (lab_at[n]) { /* other blocks join here */
                blk_off[lab_at[n]] = (u32)text->len;
                scache_clear();
            }
            if (n == tm_cut[k]) { /* the rest is in another block */
                MIns j;
                memset(&j, 0, sizeof j);
                j.op = A_B;
                j.d = j.s0 = j.s1 = j.s2 = NOREG;
                j.sym = NOSLOT;
                j.imm = tm_lab[k];
                encode(&j);
                break;
            }
            /* a jump to the block that follows is not needed */
            if (i->op == A_B && n == mb->first + mb->n - 1) {
                u32 t = k + 1, target = fwd[(u32)i->imm];
                while (t < target && (mf.blk[t].nsucc & 4))
                    t++;
                if (t == target)
                    continue;
                /* a jump to a bare return returns */
                if (mf.blk[target].nsucc & 32) {
                    epilogue();
                    continue;
                }
            }
            if ((i->op == A_LDR || i->op == A_STR) && n + 1 < mb->first + mb->n && !lab_at[n + 1] &&
                n + 1 != tm_cut[k] && encode_pair(i, i + 1)) {
                n++;
                continue;
            }
            encode(i);
        }
    }
    if (wrap_ret != NOREG) {
        blk_off[mf.nblk] = (u32)text->len;
        noframe = 1;
        early_return(wrap_ret, 1);
        noframe = 0;
    }
    for (k = 0; k < nfixups; k++) {
        Fixup *fx = &fixups[k];
        i64 delta = ((i64)blk_off[fwd[fx->target]] - (i64)fx->off) / 4;
        u32 insn = get_u32le(text->p + fx->off);
        if (fx->kind == 0) {
            insn |= (u32)delta & 0x3ffffff;
        } else if (fx->kind == 2) {
            if (delta < -0x2000 || delta >= 0x2000)
                die("internal: bit-test branch out of range in %s", f->name);
            insn |= ((u32)delta & 0x3fff) << 5;
        } else {
            if (delta < -0x40000 || delta >= 0x40000)
                die("function %s is too large for conditional branches", f->name);
            insn |= ((u32)delta & 0x7ffff) << 5;
        }
        put_u32le(text->p + fx->off, insn);
    }
    free(blk_off);
    free(layout);
    free(blk_index);
    free(uses);
    free(fused);
    free(cache_reg);
    free(cache_blk);
}
