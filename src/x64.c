/* x86-64 backend: instruction selection, frame layout, encoding.
   Calling convention: System V AMD64. Records are classified by the front
   end (parse.c): what goes in registers arrives here as scalar arguments,
   what goes in memory as an OP_BYVAL argument or a block parameter.
   Machine instructions are kept in three-address form with virtual
   registers until they are encoded; the encoder makes the two-address
   forms and reads spilled operands from the frame. */
#include "mir.h"
#include "x86i.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* physical register ids: 0..15 as the processor numbers them, 32..47 = xmm0..xmm15 */
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
#define XMM0 32
#define SCR0 R10 /* never allocated: for spilled operands and long sequences */
#define SCR1 R11
#define FSCR0 (XMM0 + 14)
#define FSCR1 (XMM0 + 15)
#define FSCR2 (XMM0 + 13) /* a third, for the vector instructions that read three registers */
#define NOSLOT 0xffffffffU
#define ARGSLOT 0xfffffffeU /* pseudo slot: the incoming stack argument area */
#define BIT(r) ((u64)1 << (r))

/* MIns.flags (bit 0 is MF_CALL) */
#define XF_SLOT 2      /* memory operand: sym is a stack slot (or ARGSLOT), imm the offset in it */
#define XF_SYM 4       /* memory operand: sym is an object symbol, imm the offset from it */
#define XF_SCALE(f) ((f) >> 4 & 3) /* log2 of the index scale */
#define XF_SCALE_SHIFT 4

/* an opcode: the byte, then what goes before it */
#define O_0F 0x100       /* the 0f escape */
#define O_66 0x660000    /* a prefix */
#define O_F2 0xf20000
#define O_F3 0xf30000
#define O_B8R 0x1000000  /* the reg field names a byte register */
#define O_B8M 0x2000000  /* the r/m field names a byte register */
#define O_38 0x4000000   /* 0f 38 (SSSE3, SSE4.1) */
#define O_3A 0x8000000   /* 0f 3a */

/* parameter kinds beyond the IT_* codes (Func.param_ty), set by the front end */
#define PT_BLK 0x80  /* the parameter is a block on the stack that starts here; its value is the address */
#define PT_PAD 0x40  /* eight more bytes of the block before */
#define PT_A16 0x20  /* with PT_BLK: the block is aligned to 16 bytes */

enum {
    X_MOV, X_MOVI, X_FMOVI, X_LEA, X_SLOTADDR,
    X_LEAX,                  /* d = s0 + (s1 << imm), imm 1..3 */
    X_ADD, X_SUB, X_IMUL, X_AND, X_OR, X_XOR,
    X_ADDI, X_IMULI, X_ANDI, X_ORI, X_XORI,
    X_SHL, X_SHR, X_SAR,     /* the count is in rcx */
    X_SHLI, X_SHRI, X_SARI, X_RORI,
    X_NEG, X_NOT, X_BSWAP, X_BSF, X_CLZ,
    X_CQO,                   /* rdx = the sign of rax */
    X_IDIV, X_DIV,           /* rdx:rax by s0; quotient to rax, remainder to rdx */
    X_MULH, X_IMULH,         /* rdx = the high half of rax * s0 */
    X_MOVSX8, X_MOVSX16, X_MOVZX8, X_MOVZX16, X_MOVSXD, X_MOVZXD,
    X_CMP, X_CMPI, X_TEST, X_TESTI, X_FCMP,
    X_SETCC, X_CMOV, X_FSEL, /* d = cc ? s0 : s1 */
    X_LOAD,                  /* d = [s0 + s1 * scale + imm]; cc = access kind */
    X_STORE,                 /* [s1 + s2 * scale + imm] = s0 */
    X_STOREI,                /* [s1 + s2 * scale + (i32)imm] = imm >> 32 */
    X_ADDM, X_SUBM, X_IMULM, X_ANDM, X_ORM, X_XORM, /* d = s0 op [s1 + s2 * scale + imm] */
    X_FADDM, X_FSUBM, X_FMULM, X_FDIVM,
    X_CMPM,                  /* s0 with [s1 + s2 * scale + imm]; cc: the memory operand is the left one */
    X_CMPMI,                 /* [s1 + s2 * scale + (i32)imm] with imm >> 32; cc = access kind */
    X_RMW,                   /* [s1 + s2 * scale + imm] op= s0; cc = access kind, w = which (the processor's number: */
    X_RMWI,                  /* [s1 + s2 * scale + (i32)imm] op= imm >> 32                0 add 1 or 4 and 5 sub 6 xor) */
    X_FADD, X_FSUB, X_FMUL, X_FDIV, X_FNEG,
    X_FMAX, X_FMIN,          /* maxss / minss d = s0 > s1 ? s0 : s1 (s1 if unordered): C's a > b ? a : b exactly */
    X_SITOF, X_UITOF, X_FTOSI, X_FTOUI, X_FCVT, /* cc: the source is 64 bits wide */
    X_BITCAST,               /* movd / movq between the register files; cc: to a floating-point register */
    X_JMP, X_JCC, X_CALL, X_CALLR, X_TAIL, X_RET,
    X_ALLOCA, X_FRAMEADDR, X_TLS,
    X_ATOMIC,                /* read-modify-write of [rdi] with rsi (and rdx); imm as the callee of an IF_ATOMIC call; old value to rax */
    X_MFENCE, X_UD2,
    X_VASAVE,                /* variadic function: the argument registers go to slot sym */
    X_ASM,                   /* inline assembly: imm = index of the AsmStmt */
    X_DEFREG,                /* no code: d is written by the instruction before */
    /* 128-bit vectors (SSE2, SSE4.1 where opt_x64_sse41 allows), from opt_vec.c. Vector registers are never spilled. */
    X_VBIN,   /* d = s0 op s1: imm = the opcode (O_* bits), with an immediate byte imm >> 32 & 0xff if bit 40 is set; w: they commute */
    X_VUN,    /* d = op s0 (op d, s0 [, imm8], d not read): imm as for X_VBIN */
    X_VSHI,   /* d = s0 shifted: imm = the opcode, cc = its /digit, sym = the count */
    X_VSEL,   /* d = s0 ? s1 : s2, bit by bit */
    X_VPAIR,  /* d = the general registers s0 (low half) and s1 (high half) */
    X_VSPLAT, /* d = the scalar s0 (w: in an xmm register) in every lane; cc = lane kind */
    X_VSUM,   /* scalar d = the lanes of s0 combined; cc = lane kind, imm = OP_VSUM's flags */
    X_VLD,    /* d = [s0 + s1]; w: four elements of the narrow kind cc there, widened to 32 bits */
    X_VST,    /* [s1 + s2] = s0; w: its four 32-bit lanes narrowed to the kind cc */
    X_VONES,  /* d = all ones (imm 1: all zeros) */
    X_VGATH,  /* d = four elements of kind cc at s0 + (lane k of s1) << sym, each widened to 32 bits (SSE4.1) */
    X_FUN,    /* scalar d = a function of the scalar s0, done in line (sel_math): cc says how, imm the opcode */
    /* vectors (AVX, AVX2, F16C, BMI2): a register's width is its class (VC_V128, VC_V256) */
    X_VEX,                   /* one VEX instruction: imm = encoding (x86i.h) | bit 32: an immediate byte, bits 40-47;
                                w = form (XF_*); d the result, s0 s1 s2 the sources in the form's order */
    X_VEXM,                  /* one VEX instruction with a memory operand [s1 + s2 * scale + (i32)imm], encoding imm >> 32,
                                immediate byte cc if w & 2; w & 1: it writes memory from s0, else d = what it reads
                                (w & 4: with s0 as vvvv) */
    X_YZERO, X_YONES,        /* d = all zeros, all ones */
    X_YSIGN,                 /* d = the sign bit of each lane of kind cc */
    X_YSPLAT,                /* d = s0 in every lane of kind cc; w: 256 bits */
    X_YMOVLO,                /* d = the low 128 bits of s0, the rest zero */
    X_GATHER,                /* d = gather [s1 + s2 * cc] under the mask in FSCR1 (w: a mask was set; else all), merged
                                into s0 (or zero); imm = encoding */
    X_CPUID,                 /* rax = register cc of cpuid(imm, imm >> 32) */
    X_RAW,                   /* the bytes of imm, most significant first */
    X_PREFETCH,              /* prefetch [s0 + s1 * scale + imm] with hint digit cc */
    X_KMOVK,                 /* k1 = the general register s0 (kmovw), for the next instruction's mask */
    X_KTEST                  /* general d = the mask of vptestm (encoding imm) on s0, s1: k1, then kmov of cc bytes */
};

/* conditions: the processor's own, then two that take two tests after a
   floating point comparison (equal: ZF and not PF; not equal: the opposite) */
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G,
       CC_FEQ, CC_FNE };

static const MTarget target = {
    { 0xf3cfUL,               /* rax rcx rdx rbx rsi rdi r8 r9 r12-r15 */
      0x1fffUL << 32 },       /* xmm0-xmm12 */
    0x3c7UL | (0xffffUL << 32), /* rax rcx rdx rsi rdi r8 r9; every xmm */
    X_MOV
};

static const u8 arg_gr[6] = { RDI, RSI, RDX, RCX, R8, R9 };

static Func *f;
static Obj *obj;
static MFunc mf;
static u32 *uses;      /* per IR value: number of uses */
static u32 *vuses;     /* ... of them as a value (not the condition of a branch or a select) */
#define FUSE_CMP 1   /* a comparison used only as a condition: whoever uses it sets the flags */
#define FUSE_OFF 2   /* add of a constant folded into an address */
#define FUSE_IDX 3   /* add of an index register folded into an address */
#define FUSE_SKIP 4  /* shift that is the scale of a folded index */
#define FUSE_MOV 5   /* extension made redundant by changing the load it reads */
#define FUSE_LOAD 6  /* load that is the memory operand of its one user */
#define FUSE_TEST 9  /* and whose result only says zero or not: a test where that is asked */
#define FUSE_RMW 8   /* operation on a loaded value, stored back: done in memory by the store */
#define FUSE_ALIAS 7 /* truncation or extension that is its operand's register, unchanged */
#define FUSE_BR2 10  /* an and / or of comparisons that only decides a branch: one conditional branch per comparison */
#define BR2_MAX 8     /* (at most so many comparisons) */
#define FUSE_DIVR 11 /* quotient or remainder that an earlier division of the same operands in its block also gave */
#define FUSE_FMA 12  /* a product that its one user, a sum or difference, takes into one vfmadd (-ffp-contract) */
static u32 br2_leaves(u32 v, int op, u32 *lv, u32 n, int check, u32 blk);
static u32 div_pair(u32 v, int first);
#define FUSE_NOT 0x80 /* while counting uses: the value is used as a value */
#define LOAD_SIGNED32 11 /* access kind, here only: a 32-bit load that sign-extends to 64 bits */
static u8 *fused;
static u8 *fma_sum; /* per IR value: a sum or difference that takes its product into a vfmadd */
static u32 *cache_reg; /* per floating IR value: register holding it ... */
static u32 *cache_blk; /* ... in this block */
static u32 cur_blk;
static u32 *blk_index; /* IR block -> layout index */
static u32 *layout;    /* layout index -> IR block */
static u32 nlayout;
static u32 outargs;     /* bytes of outgoing stack arguments */
static u32 named_stack; /* bytes of incoming named stack arguments */
static u32 sym_memcpy, sym_memset;
static Obj *sym_obj_owner;
static u32 fconst_sym(u64 bits, u32 size);
static int has_calls, uses_args, uses_fp, uses_alloca;
static u32 ret_slot;  /* where a record returned in registers is put together */
static u32 sret_slot; /* where the address of a record returned in memory is kept */
static u32 va_slot;   /* variadic function: the register save area */
static u32 *alink;    /* inline assembly call -> its first OP_ASMOUT -> the next; made when first needed */
static u64 asm_saved; /* registers written by inline assembly */
static u32 fn_serial;
/* -g: the source location of each machine instruction, and of the IR instruction being selected */
static u32 *mloc, capmloc, sel_loc;

/* ---- machine IR construction ---- */

static MIns *emit(int op, int w, u32 d, u32 s0, u32 s1)
{
    MIns *i;
    if (mf.n == mf.cap) {
        mf.cap = mf.cap ? mf.cap * 2 : 1024;
        mf.ins = (MIns *)xrealloc(mf.ins, mf.cap * sizeof(MIns));
    }
    if (opt_debug) {
        if (mf.n >= capmloc) {
            capmloc = mf.cap;
            mloc = (u32 *)xrealloc(mloc, capmloc * sizeof(u32));
        }
        mloc[mf.n] = sel_loc;
    }
    i = &mf.ins[mf.n++];
    i->op = (u16)op;
    i->w = (u8)w;
    i->cc = 0;
    switch (op) { /* the two-address ones: allocation hints */
    case X_ADD: case X_IMUL: case X_AND: case X_OR: case X_XOR: case X_FADD: case X_FMUL:
        i->flags = MF_TIED | MF_COMMUT;
        break;
    case X_FMAX: case X_FMIN:
        i->flags = MF_TIED | MF_NOTS1;
        break;
    case X_SUB: case X_FSUB: case X_FDIV: case X_ADDI: case X_ANDI: case X_ORI: case X_XORI:
    case X_SHL: case X_SHR: case X_SAR: case X_SHLI: case X_SHRI: case X_SARI: case X_RORI:
    case X_NEG: case X_NOT: case X_BSWAP: case X_FNEG:
    case X_ADDM: case X_SUBM: case X_IMULM: case X_ANDM: case X_ORM: case X_XORM:
    case X_FADDM: case X_FSUBM: case X_FMULM: case X_FDIVM:
        i->flags = MF_TIED;
        break;
    default:
        i->flags = 0;
    }
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
    return ty == IT_F32 || ty == IT_F64;
}

/* register class bits beyond the one ra.c reads (bit 0: an xmm register) */
/* register classes: bit 0 an xmm register; bit 1 all 128 bits of it matter, bit 3 all 256; bit 4 it may be spilled (ra.c
   keeps a vector without it in a register whatever it costs: sel_vec's legacy SSE code has no spill slots) */
#define VC_V128 2
#define VC_V256 (2 | 8)
#define VC_V512 (2 | 8 | 32)
#define VC_SPILL 16
static int fn_avx;  /* the function is compiled for AVX, or uses intrinsics or 256-bit vectors (VEX code anyway) */
static int vlegacy; /* the function has 128-bit operations in sel_vec's legacy SSE: its 128-bit vectors are not spilled */

static int is_vec(int ty)
{
    return ty == IT_V128 || ty == IT_V256 || ty == IT_V512;
}

/* held in an xmm register */
static int is_xmm(int ty)
{
    return is_fp(ty) || is_vec(ty);
}

static int cls_of(int ty)
{
    return ty == IT_V512 ? 1 | VC_V512 | VC_SPILL : ty == IT_V256 ? 1 | VC_V256 | VC_SPILL : ty == IT_V128 ? 1 | VC_V128 | (vlegacy ? 0 : VC_SPILL) : is_fp(ty);
}

static int is_w(int ty)
{
    return ty == IT_I64 || ty == IT_F64;
}

static int fits32(i64 v)
{
    return v >= -0x7fffffffL - 1 && v <= 0x7fffffffL;
}

/* The integer constant v as an instruction of its type would see it. */
static int const_of(u32 v, i64 *c)
{
    u64 u;
    if (!ir_is_const(f, v, &u))
        return 0;
    *c = f->ins[v].ty == IT_I64 ? (i64)u : (i64)(i32)(u32)u;
    return 1;
}

/* Materialise a floating value into register d. */
static void sel_splat_const(u32 v, u32 d);

static void materialise(u32 v, u32 d)
{
    Ins *i = &f->ins[v];
    MIns *m;
    switch (i->op) {
    case OP_CONST:
        if (is_vec(i->ty)) {
            emit(X_YZERO, 0, d, NOREG, NOREG); /* the one vector constant */
            break;
        }
        m = emit(X_MOVI, i->ty == IT_I64, d, NOREG, NOREG);
        m->imm = (i64)((u64)i->a | ((u64)i->b << 32));
        break;
    case OP_FCONST:
        m = emit(X_FMOVI, i->ty == IT_F64, d, NOREG, NOREG);
        m->imm = (i64)((u64)i->a | ((u64)i->b << 32));
        break;
    case OP_VSPLAT:
        sel_splat_const(v, d);
        break;
    case OP_ADDR:
        m = emit(X_LEA, 1, d, NOREG, NOREG);
        m->sym = i->a;
        m->imm = (i32)i->b;
        break;
    case OP_SLOT:
        m = emit(X_SLOTADDR, 1, d, NOREG, NOREG);
        m->sym = i->a;
        m->imm = (i32)i->b;
        break;
    default:
        emit(X_MOV, 1, d, VREG0 + v, NOREG);
        break;
    }
}

/* A splat of a constant: a broadcast from read-only data (sel_splat_const). */
static int is_const_splat(u32 v)
{
    Ins *i = &f->ins[v];
    int op;
    if (i->op != OP_VSPLAT)
        return 0;
    op = f->ins[i->a].op;
    return op == OP_CONST || op == OP_FCONST;
}

static int is_floating(u32 v)
{
    int op = f->ins[v].op;
    return op == OP_CONST || op == OP_FCONST || op == OP_ADDR || op == OP_SLOT;
}

/* A read-only data symbol holding the n bytes at p (n = 1, 2, 4 or 8), shared by equal constants. */
static u32 const_sym(const u8 *p, u32 n)
{
    static u32 csym[64], cn;
    u32 k;
    u64 off;
    Buf *sec = &obj->sec[SEC_CONST];
    for (k = 0; k < cn; k++) {
        ObjSym *s;
        if (csym[k] >= obj->nsyms)
            continue;
        s = &obj->syms[csym[k]];
        if (s->sec == SEC_CONST && !s->name && s->dsize == n && s->value + n <= sec->len &&
            memcmp(sec->p + s->value, p, n) == 0)
            return csym[k];
    }
    off = obj_reserve(obj, SEC_CONST, n, n);
    buf_put(sec, p, n);
    k = obj_add_sym(obj, 0, SEC_CONST, off, 0);
    obj->syms[k].dsize = n;
    if (cn == 64)
        cn = 0;
    csym[cn++] = k;
    return k;
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
        emit(X_MOV, 1, d, VREG0 + v, NOREG);
}

static const u8 mem_bytes[] = { 1, 1, 2, 2, 4, 8, 4, 8, 16, 32, 64, 4 };

typedef struct Addr {
    u32 base;  /* register or NOREG */
    u32 idx;   /* index register or NOREG */
    u32 sym;   /* slot or symbol, by flags */
    i64 off;
    u8 flags;  /* XF_SLOT, XF_SYM, scale */
} Addr;

/* The address v as a memory operand. */
static void addr_of(u32 v, Addr *a)
{
    Ins *i = &f->ins[v];
    i64 c;
    a->base = a->idx = NOREG;
    a->sym = NOSLOT;
    a->off = 0;
    a->flags = 0;
    while (fused[v] == FUSE_ALIAS) {
        v = i->a;
        i = &f->ins[v];
    }
    if (fused[v] == FUSE_IDX) {
        u32 x = i->a, y = i->b;
        if (fused[x] == FUSE_SKIP) {
            x = i->b;
            y = i->a;
        }
        if (fused[y] != FUSE_SKIP && fused[x] != FUSE_OFF && f->ins[x].op != OP_SLOT &&
            (fused[y] == FUSE_OFF || f->ins[y].op == OP_SLOT)) {
            x = i->b; /* the one with a constant part is the base */
            y = i->a;
        }
        /* the base: a local, or a register, and constants added to it that are there for addresses only */
        while (fused[x] == FUSE_OFF && const_of(f->ins[x].b, &c)) {
            a->off += c;
            x = f->ins[x].a;
        }
        if (f->ins[x].op == OP_SLOT) {
            a->flags = XF_SLOT;
            a->sym = f->ins[x].a;
            a->off += (i32)f->ins[x].b;
        } else {
            a->base = reg_of(x);
        }
        if (fused[y] == FUSE_SKIP) {
            u64 sh = 0;
            ir_is_const(f, f->ins[y].b, &sh);
            a->flags |= (u8)(sh << XF_SCALE_SHIFT);
            y = f->ins[y].a;
        }
        a->idx = reg_of(y);
        return;
    }
    if (i->op == OP_SLOT) {
        a->flags = XF_SLOT;
        a->sym = i->a;
        a->off = (i32)i->b;
    } else if (i->op == OP_ADDR && cache_blk[v] != cur_blk) {
        a->flags = XF_SYM;
        a->sym = i->a;
        a->off = (i32)i->b;
    } else if (fused[v] == FUSE_OFF && const_of(i->b, &c)) {
        Ins *b = &f->ins[i->a];
        a->off = c;
        if (b->op == OP_SLOT) {
            a->flags = XF_SLOT;
            a->sym = b->a;
            a->off += (i32)b->b;
        } else if (b->op == OP_ADDR && fits32(a->off + (i32)b->b)) {
            a->flags = XF_SYM;
            a->sym = b->a;
            a->off += (i32)b->b;
        } else {
            a->base = reg_of(i->a);
        }
    } else {
        a->base = reg_of(v);
    }
}

static void set_mem(MIns *m, const Addr *a)
{
    m->sym = a->sym;
    m->imm = a->off;
    m->flags = (u8)((m->flags & (MF_TIED | MF_COMMUT | MF_NOTS1)) | a->flags);
}

static void emit_load(u32 d, const Addr *a, int mem)
{
    MIns *m = emit(X_LOAD, 0, d, a->base, a->idx);
    m->cc = (u8)mem;
    set_mem(m, a);
}

static void emit_store(u32 val, const Addr *a, int mem)
{
    MIns *m = emit(X_STORE, 0, NOREG, val, a->base);
    m->s2 = a->idx;
    m->cc = (u8)mem;
    set_mem(m, a);
}

static void emit_store_imm(i64 val, const Addr *a, int mem)
{
    MIns *m = emit(X_STOREI, 0, NOREG, NOREG, a->base);
    m->s2 = a->idx;
    m->cc = (u8)mem;
    set_mem(m, a);
    m->imm = (i64)(((u64)val << 32) | (u32)a->off);
}

static void slot_addr(Addr *a, u32 slot, i64 off)
{
    a->base = a->idx = NOREG;
    a->flags = XF_SLOT;
    a->sym = slot;
    a->off = off;
}

static void reg_addr(Addr *a, u32 base, i64 off)
{
    a->base = base;
    a->idx = NOREG;
    a->flags = 0;
    a->sym = NOSLOT;
    a->off = off;
}

static int is_compare(int op)
{
    return (op >= OP_EQ && op <= OP_UGE) || (op >= OP_FEQ && op <= OP_FGE);
}

/* the flags of a & b, for an and that is not made into a value */
static void emit_test(u32 v)
{
    Ins *i = &f->ins[v];
    int w = i->ty == IT_I64;
    i64 c;
    MIns *m;
    if (const_of(i->b, &c) && fits32(c)) {
        m = emit(X_TESTI, w, NOREG, reg_of(i->a), NOREG);
        m->imm = c;
    } else {
        u32 ra = reg_of(i->a);
        emit(X_TEST, w, NOREG, ra, reg_of(i->b));
    }
}

static void emit_cmp_mem_imm(const Addr *a, int mem, i64 val)
{
    MIns *m = emit(X_CMPMI, mem == M_64, NOREG, NOREG, a->base);
    m->s2 = a->idx;
    m->cc = (u8)mem;
    set_mem(m, a);
    m->imm = (i64)(((u64)val << 32) | (u32)a->off);
}

/* Emits the flag-setting instruction for compare v; returns the condition. */
static int emit_compare(u32 v)
{
    Ins *i = &f->ins[v];
    int ty = f->ins[i->a].ty;
    i64 c;
    MIns *m;
    if (is_fp(ty)) {
        u32 ra = reg_of(i->a), rb = reg_of(i->b);
        switch (i->op) {
        case OP_FEQ: emit(X_FCMP, ty == IT_F64, NOREG, ra, rb); return CC_FEQ;
        case OP_FNE: emit(X_FCMP, ty == IT_F64, NOREG, ra, rb); return CC_FNE;
        case OP_FLT: emit(X_FCMP, ty == IT_F64, NOREG, rb, ra); return CC_A;
        case OP_FLE: emit(X_FCMP, ty == IT_F64, NOREG, rb, ra); return CC_AE;
        case OP_FGT: emit(X_FCMP, ty == IT_F64, NOREG, ra, rb); return CC_A;
        default: emit(X_FCMP, ty == IT_F64, NOREG, ra, rb); return CC_AE;
        }
    }
    if (fused[i->a] == FUSE_LOAD || fused[i->b] == FUSE_LOAD) {
        int left = fused[i->a] == FUSE_LOAD;
        Ins *l = &f->ins[left ? i->a : i->b];
        Addr a;
        if (left && const_of(i->b, &c)) {
            addr_of(l->a, &a);
            emit_cmp_mem_imm(&a, l->mem, c);
        } else {
            u32 r = reg_of(left ? i->b : i->a);
            addr_of(l->a, &a);
            m = emit(X_CMPM, ty == IT_I64, NOREG, r, a.base);
            m->s2 = a.idx;
            m->cc = (u8)left;
            set_mem(m, &a);
        }
    } else if (const_of(i->b, &c) && c == 0 && fused[i->a] == FUSE_TEST) {
        emit_test(i->a);
    } else if (const_of(i->b, &c) && fits32(c)) {
        if (c == 0) {
            u32 r = reg_of(i->a);
            emit(X_TEST, ty == IT_I64, NOREG, r, r);
        } else {
            m = emit(X_CMPI, ty == IT_I64, NOREG, reg_of(i->a), NOREG);
            m->imm = c;
        }
    } else {
        u32 ra = reg_of(i->a);
        emit(X_CMP, ty == IT_I64, NOREG, ra, reg_of(i->b));
    }
    switch (i->op) {
    case OP_EQ: return CC_E;
    case OP_NE: return CC_NE;
    case OP_SLT: return CC_L;
    case OP_SLE: return CC_LE;
    case OP_SGT: return CC_G;
    case OP_SGE: return CC_GE;
    case OP_ULT: return CC_B;
    case OP_ULE: return CC_BE;
    case OP_UGT: return CC_A;
    }
    return CC_AE;
}

/* The flags for "v is not zero", v a value or a fused comparison; returns the condition. */
static int emit_cond(u32 v)
{
    u32 r;
    if (fused[v] == FUSE_CMP)
        return emit_compare(v);
    if (fused[v] == FUSE_LOAD) {
        Addr a;
        addr_of(f->ins[v].a, &a);
        emit_cmp_mem_imm(&a, f->ins[v].mem, 0);
        return CC_NE;
    }
    if (fused[v] == FUSE_TEST) {
        emit_test(v);
        return CC_NE;
    }
    r = reg_of(v);
    emit(X_TEST, f->ins[v].ty == IT_I64, NOREG, r, r);
    return CC_NE;
}

/* Does an instruction of this kind leave the upper half of its register clear? */
static int clean32(int op)
{
    return (op >= OP_ADD && op <= OP_ZEXT16) || op == OP_FTOSI || op == OP_FTOUI || op == OP_LOAD;
}

/* ---- calling convention ---- */

#define LOC_STACK 0x40000000U /* flag: the rest is a stack offset */
#define LOC_NONE 0xffffffffU
#define AK_INT 0
#define AK_FP 1
#define AK_BLK 2  /* a block of bsize bytes on the stack */
#define AK_SKIP 3 /* not passed */

static u8 *a_kind;
static u32 *a_size, *a_loc, a_cap;

static void args_reserve(u32 n)
{
    if (n <= a_cap)
        return;
    a_cap = n * 2;
    a_kind = (u8 *)xrealloc(a_kind, a_cap);
    a_size = (u32 *)xrealloc(a_size, a_cap * sizeof(u32));
    a_loc = (u32 *)xrealloc(a_loc, a_cap * sizeof(u32));
}

/* Assigns locations to the n arguments described by a_kind and a_size (the
   size of a block; bit 31: it is aligned to 16 bytes); returns the stack
   bytes used. *nfr gets the number of xmm registers taken. */
static u32 abi_assign(u32 n, u32 *nfr_out)
{
    u32 ngr = 0, nfr = 0, stack = 0, k;
    for (k = 0; k < n; k++) {
        switch (a_kind[k]) {
        case AK_SKIP:
            a_loc[k] = LOC_NONE;
            break;
        case AK_BLK:
            if (a_size[k] >> 31)
                stack = (u32)align_up(stack, 16);
            a_loc[k] = LOC_STACK | stack;
            stack += (u32)align_up(a_size[k] & 0x7fffffffU, 8);
            break;
        case AK_FP:
            if (nfr < 8) {
                a_loc[k] = XMM0 + nfr++;
            } else {
                a_loc[k] = LOC_STACK | stack;
                stack += 8;
            }
            break;
        default:
            if (ngr < 6) {
                a_loc[k] = arg_gr[ngr++];
            } else {
                a_loc[k] = LOC_STACK | stack;
                stack += 8;
            }
        }
    }
    if (nfr_out)
        *nfr_out = nfr;
    return (u32)align_up(stack, 16);
}

static int ty_mem(int ty)
{
    return ty == IT_I64 ? M_64 : ty == IT_F32 ? M_F32 : ty == IT_F64 ? M_F64 : ty == IT_V128 ? M_V128 : ty == IT_V256 ? M_V256 : ty == IT_V512 ? M_V512 : M_32;
}

static u32 runtime_sym(const char *name, u32 *cache)
{
    if (sym_obj_owner != obj) {
        sym_obj_owner = obj;
        sym_memcpy = sym_memset = NOSLOT;
    }
    if (*cache == NOSLOT)
        *cache = obj_add_sym(obj, intern_cstr(name), SEC_UNDEF, 0, 1);
    return *cache;
}

/* A floating constant (size 4 or 8), or a sign mask (16: the 8 bytes twice), in the constant section: a movss / movsd or
   an arithmetic operand from there is one instruction where mov imm + movd were two and took a general register */
static u32 fconst_sym(u64 bits, u32 size)
{
    u8 b[16];
    u32 k;
    for (k = 0; k < 16; k++)
        b[k] = (u8)(bits >> (8 * (k & 7)));
    return const_sym(b, size);
}

/* memcpy(dst, src, size) or memset(dst, 0, size); the addresses are in registers already or are IR values */
static void runtime_call(u32 sym, u32 dreg, u32 dv, u32 sreg, u32 sv, int has_src, u32 size)
{
    MIns *m;
    if (dreg != NOREG)
        emit(X_MOV, 1, RDI, dreg, NOREG);
    else
        move_to(RDI, dv);
    if (!has_src) {
        m = emit(X_MOVI, 0, RSI, NOREG, NOREG);
        m->imm = 0;
    } else if (sreg != NOREG) {
        emit(X_MOV, 1, RSI, sreg, NOREG);
    } else {
        move_to(RSI, sv);
    }
    m = emit(X_MOVI, 1, RDX, NOREG, NOREG);
    m->imm = size;
    m = emit(X_CALL, 0, NOREG, NOREG, NOREG);
    m->sym = sym;
    m->flags = MF_CALL;
    m->pmask = BIT(RDI) | BIT(RSI) | BIT(RDX);
    has_calls = 1;
}

/* Copies (or with no source clears) size bytes, a doubleword at a time and then the rest. */
static void block_move(const Addr *d, const Addr *s, u32 size)
{
    u32 off = 0, t = NOREG;
    if (s)
        t = new_vreg(0);
    while (size) {
        u32 n = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        int mem = n == 8 ? M_64 : n == 4 ? M_32 : n == 2 ? M_16U : M_8U;
        Addr da = *d, sa;
        da.off += off;
        if (s) {
            sa = *s;
            sa.off += off;
            emit_load(t, &sa, mem);
            emit_store(t, &da, mem);
        } else {
            emit_store_imm(0, &da, mem);
        }
        off += n;
        size -= n;
    }
}

static void plain_addr(u32 v, Addr *a)
{
    Ins *i = &f->ins[v];
    if (i->op == OP_SLOT)
        slot_addr(a, i->a, (i32)i->b);
    else
        reg_addr(a, reg_of(v), 0);
}

static void sel_block_op(u32 v)
{
    Ins *i = &f->ins[v];
    u32 size = i->c;
    Addr d, s;
    int copy = i->op == OP_MEMCPY;

    if (size == 0)
        return;
    if (size > 64) {
        if (copy)
            runtime_call(runtime_sym("memcpy", &sym_memcpy), NOREG, i->a, NOREG, i->b, 1, size);
        else
            runtime_call(runtime_sym("memset", &sym_memset), NOREG, i->a, NOREG, 0, 0, size);
        return;
    }
    plain_addr(i->a, &d);
    if (copy)
        plain_addr(i->b, &s);
    block_move(&d, copy ? &s : 0, size);
}

static void sel_asm(u32 v);

/* Atomic operations. Loads and stores are ordinary ones (a store that must
   be seen in order is followed by a fence). The others work in fixed
   registers: the address in rdi, the operand in rsi (compare and swap: the
   expected value in rax, the new one in rdx), and the old value comes back in rax. */
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
        emit(X_MFENCE, 0, NOREG, NOREG, NOREG);
        return;
    case ATOM_LOAD:
        plain_addr(f->xargs[i->a], &a);
        emit_load(d, &a, mem);
        return;
    case ATOM_STORE:
        r = reg_of(f->xargs[i->a + 1]);
        plain_addr(f->xargs[i->a], &a);
        emit_store(r, &a, mem);
        if (!relaxed)
            emit(X_MFENCE, 0, NOREG, NOREG, NOREG);
        return;
    }
    move_to(RDI, f->xargs[i->a]);
    if (kind == ATOM_CAS) {
        move_to(RAX, f->xargs[i->a + 1]);
        move_to(RDX, f->xargs[i->a + 2]);
    } else {
        move_to(RSI, f->xargs[i->a + 1]);
    }
    m = emit(X_ATOMIC, 0, RAX, NOREG, NOREG);
    m->imm = code;
    m->pmask = kind == ATOM_CAS ? BIT(RDI) | BIT(RAX) | BIT(RDX) : BIT(RDI) | BIT(RSI);
    if (kind != ATOM_CAS)
        emit(X_DEFREG, 1, RSI, NOREG, NOREG);
    if (uses[v])
        emit(X_MOV, 1, d, RAX, NOREG);
}

int opt_x64_fma;
int opt_math_errno = 1; /* sqrt of a negative number sets errno (as the C library's does): not done in line; -fno-math-errno */

/* A call of one of the mathematical library's functions that one instruction does: in line. A function of that name
   defined in this file is the program's own and is called. */
static int sel_math(u32 v)
{
    static const struct { const char *name; u8 how; u8 sse41; u8 errno_; u32 op; u8 imm; } fns[] = {
        { "sqrt", 0, 0, 1, O_F2 | O_0F | 0x51, 0 },
        { "fabs", 1, 0, 0, 0, 0 },
        { "floor", 0, 1, 0, O_66 | O_0F | O_3A | 0x0b, 9 },   /* roundsd: down, no inexact */
        { "ceil", 0, 1, 0, O_66 | O_0F | O_3A | 0x0b, 10 },
        { "trunc", 0, 1, 0, O_66 | O_0F | O_3A | 0x0b, 11 },
        { "nearbyint", 0, 1, 0, O_66 | O_0F | O_3A | 0x0b, 12 }, /* the current rounding, no inexact */
        { "rint", 0, 1, 0, O_66 | O_0F | O_3A | 0x0b, 4 },
        { "lrint", 2, 0, 0, O_F2 | O_0F | 0x2d, 0 },          /* cvtsd2si: the current rounding */
        { "llrint", 2, 0, 0, O_F2 | O_0F | 0x2d, 0 },
        { 0, 0, 0, 0, 0, 0 }
    };
    Ins *i = &f->ins[v];
    const char *nm;
    u32 sym, x, k;
    size_t bl;
    int isf;
    MIns *m;
    if (f->ins[i->c].op != OP_ADDR || f->ins[i->c].b || i->b != 1 || (i->flags & (IF_SRET | IF_ASM | IF_ATOMIC | IF_ALLOCA)))
        return 0;
    sym = f->ins[i->c].a;
    if (obj->syms[sym].sec != SEC_UNDEF && obj->syms[sym].sec != SEC_NONE)
        return 0;
    nm = intern_str(obj->syms[sym].name);
    x = f->xargs[i->a];
    bl = strlen(nm);
    isf = bl > 1 && nm[bl - 1] == 'f';
    if (f->ins[x].ty != (isf ? IT_F32 : IT_F64))
        return 0;
    for (k = 0; fns[k].name; k++)
        if (strlen(fns[k].name) == bl - (size_t)isf && strncmp(nm, fns[k].name, bl - (size_t)isf) == 0)
            break;
    if (!fns[k].name || (fns[k].sse41 && !opt_x64_sse41) || (fns[k].errno_ && opt_math_errno))
        return 0;
    if (fns[k].how == 2 ? i->ty != IT_I64 && i->ty != IT_I32 : i->ty != f->ins[x].ty)
        return 0;
    m = emit(X_FUN, fns[k].how == 2 ? i->ty == IT_I64 : !isf, VREG0 + v, reg_of(x), NOREG);
    m->cc = fns[k].how;
    m->imm = (i64)fns[k].op | (i64)fns[k].imm << 32;
    if (isf && fns[k].how != 1)
        m->imm = (m->imm & ~(i64)0xff00ff) | (fns[k].how == 0 && fns[k].sse41 ? (O_66 | 0x0a) : (O_F3 | (fns[k].op & 0xff)));
    m->sym = isf; /* the argument is single precision */
    return 1;
}

static void sel_call(u32 v)
{
    Ins *i = &f->ins[v];
    u32 n = i->b, nfixed = f->xargs[i->a - 1], k, stack, nfr = 0;
    u64 pmask = 0;
    MIns *m;
    Addr a;
    u32 callee = NOREG;
    int sret_regs = (i->flags & IF_SRET) && i->mem, ymm = i->ty == IT_V256 || i->ty == IT_V512;

    if (i->flags & IF_ASM) {
        sel_asm(v);
        return;
    }
    if (f->ins[i->c].op == OP_ADDR && obj->syms[f->ins[i->c].a].isdev) {
        m = emit(X_MOVI, 0, VREG0 + v, NOREG, NOREG); /* CUDA: this is not device code */
        m->imm = 0;
        return;
    }
    if (i->flags & IF_ATOMIC) {
        sel_atomic(v);
        return;
    }
    if (sel_math(v))
        return;
    if (i->flags & IF_ALLOCA) {
        k = (u32)f->ins[i->c].a; /* 0: take stack, 1: the stack pointer, 2: set it */
        m = k == 2 ? emit(X_ALLOCA, 1, NOREG, reg_of(f->xargs[i->a]), NOREG)
            : emit(X_ALLOCA, 1, VREG0 + v, k == 1 ? NOREG : reg_of(f->xargs[i->a]), NOREG);
        m->imm = k;
        uses_fp = uses_alloca = 1;
        return;
    }
    args_reserve(n + 1);
    for (k = 0; k < n; k++) {
        Ins *ai = &f->ins[f->xargs[i->a + k]];
        if (k == 0 && sret_regs) {
            a_kind[k] = AK_SKIP;
        } else if (ai->op == OP_BYVAL) {
            a_kind[k] = AK_BLK;
            a_size[k] = ai->b | (ai->c > 8 ? 0x80000000U : 0);
        } else {
            a_kind[k] = is_xmm(ai->ty) ? AK_FP : AK_INT;
            ymm |= ai->ty == IT_V256 || ai->ty == IT_V512;
        }
    }
    stack = abi_assign(n, &nfr);
    if (stack > outargs)
        outargs = stack;
    if (f->ins[i->c].op != OP_ADDR || f->ins[i->c].b != 0)
        callee = reg_of(i->c);
    /* blocks first: a long one is copied by a call */
    for (k = 0; k < n; k++) {
        if (a_kind[k] == AK_BLK) {
            Ins *ai = &f->ins[f->xargs[i->a + k]];
            u32 off = a_loc[k] & ~LOC_STACK, src = reg_of(f->xargs[i->a + k]);
            if (ai->b > 128) {
                u32 t = new_vreg(0);
                m = emit(X_ADDI, 1, t, RSP, NOREG);
                m->imm = off;
                runtime_call(runtime_sym("memcpy", &sym_memcpy), t, 0, src, 0, 1, ai->b);
                /* the registers of this call's own arguments were not disturbed: none is set yet */
            } else {
                Addr s;
                reg_addr(&a, RSP, off);
                reg_addr(&s, src, 0);
                block_move(&a, &s, ai->b);
            }
        }
    }
    for (k = 0; k < n; k++) {
        if (a_kind[k] != AK_BLK && a_loc[k] != LOC_NONE && (a_loc[k] & LOC_STACK)) {
            int ty = f->ins[f->xargs[i->a + k]].ty;
            reg_addr(&a, RSP, a_loc[k] & ~LOC_STACK);
            emit_store(reg_of(f->xargs[i->a + k]), &a, is_xmm(ty) ? ty_mem(ty) : M_64);
        }
    }
    for (k = 0; k < n; k++) {
        if (a_loc[k] != LOC_NONE && !(a_loc[k] & LOC_STACK)) {
            move_to(a_loc[k], f->xargs[i->a + k]);
            pmask |= BIT(a_loc[k]);
        }
    }
    if (callee == NOREG && obj->syms[f->ins[i->c].a].trap) {
        /* a function declared with the error attribute: the call was to be
           optimised away, and what could not be proved dead traps */
        emit(X_UD2, 0, NOREG, NOREG, NOREG);
        return;
    }
    if (nfixed != 0xffffffffU) {
        /* a function that takes any arguments wants to know how many are in xmm registers */
        m = emit(X_MOVI, 0, RAX, NOREG, NOREG);
        m->imm = nfr;
        pmask |= BIT(RAX);
    }
    if (callee == NOREG) {
        m = emit(X_CALL, 0, NOREG, NOREG, NOREG);
        m->sym = f->ins[i->c].a;
    } else {
        m = emit(X_CALLR, 0, NOREG, callee, NOREG);
    }
    m->flags = MF_CALL;
    m->pmask = pmask;
    m->cc = (u8)ymm; /* 256-bit arguments or result: no vzeroupper before it */
    has_calls = 1;
    if (sret_regs) {
        /* a small record comes back in two registers, each rax/rdx or xmm0/xmm1 by its class; the place for it holds 16 bytes */
        u32 r0 = i->flags & IF_SSE0 ? XMM0 : RAX;
        u32 r1 = i->flags & IF_SSE1 ? (i->flags & IF_SSE0 ? XMM0 + 1 : XMM0) : (i->flags & IF_SSE0 ? RAX : RDX);
        plain_addr(f->xargs[i->a], &a);
        emit_store(r0, &a, r0 >= XMM0 ? M_F64 : M_64);
        if (i->mem > 8) {
            a.off += 8;
            emit_store(r1, &a, r1 >= XMM0 ? M_F64 : M_64);
        }
    }
    if (i->ty != IT_VOID && uses[v])
        emit(X_MOV, 1, VREG0 + v, is_xmm(i->ty) ? XMM0 : RAX, NOREG);
}

/* ---- vectors: AVX / AVX2 / F16C / BMI2 ----
   A vector value is one register, xmm or ymm by its type (IT_V128, IT_V256); lane operations carry the
   lane kind in Ins.mem. Everything is written with VEX encodings (the upper half of a ymm register is
   cleared or kept as the instruction says, never by a legacy SSE instruction in between). */

#define VE(opc, map, pp, w) ((u32)(opc) | (u32)(map) << 8 | (u32)(pp) << 10 | (u32)(w) << 12)
#define VE_L 0x2000u
#define VE_Z 0x20000u /* 512 bits (EVEX) */
#define VE_DIGIT(d) ((u32)(d) << 14)

static int lane_bits(int mem)
{
    switch (mem) {
    case M_8S:
    case M_8U:
        return 8;
    case M_16S:
    case M_16U:
        return 16;
    case M_32:
    case M_F32:
        return 32;
    }
    return 64;
}

/* The VEX instruction (L clear) for lane operation op on lanes of kind mem, or 0 if there is none. */
static u32 vec_op_enc(int op, int mem)
{
    int b = lane_bits(mem), fl = mem == M_F32 || mem == M_F64, pd = mem == M_F64;
    switch (op) {
    case OP_FADD: return VE(0x58, 1, pd, 0);
    case OP_FSUB: return VE(0x5c, 1, pd, 0);
    case OP_FMUL: return VE(0x59, 1, pd, 0);
    case OP_FDIV: return VE(0x5e, 1, pd, 0);
    case OP_ADD: return fl ? 0 : VE(b == 8 ? 0xfc : b == 16 ? 0xfd : b == 32 ? 0xfe : 0xd4, 1, 1, 0);
    case OP_SUB: return fl ? 0 : VE(b == 8 ? 0xf8 : b == 16 ? 0xf9 : b == 32 ? 0xfa : 0xfb, 1, 1, 0);
    case OP_MUL: return fl ? 0 : b == 16 ? VE(0xd5, 1, 1, 0) : b == 32 ? VE(0x40, 2, 1, 0) : 0;
    case OP_AND: return VE(0xdb, 1, 1, 0);
    case OP_OR: return VE(0xeb, 1, 1, 0);
    case OP_XOR: return VE(0xef, 1, 1, 0);
    case OP_VSMAX:
        return fl ? VE(0x5f, 1, pd, 0) : b == 8 ? VE(0x3c, 2, 1, 0) : b == 16 ? VE(0xee, 1, 1, 0) : b == 32 ? VE(0x3d, 2, 1, 0) : 0;
    case OP_VSMIN:
        return fl ? VE(0x5d, 1, pd, 0) : b == 8 ? VE(0x38, 2, 1, 0) : b == 16 ? VE(0xea, 1, 1, 0) : b == 32 ? VE(0x39, 2, 1, 0) : 0;
    case OP_VUMAX:
        return fl ? 0 : b == 8 ? VE(0xde, 1, 1, 0) : b == 16 ? VE(0x3e, 2, 1, 0) : b == 32 ? VE(0x3f, 2, 1, 0) : 0;
    case OP_VUMIN:
        return fl ? 0 : b == 8 ? VE(0xda, 1, 1, 0) : b == 16 ? VE(0x3a, 2, 1, 0) : b == 32 ? VE(0x3b, 2, 1, 0) : 0;
    }
    return 0;
}

/* the lane kind of the 512-bit operation being selected (-1: the encodings say W themselves) */
static int zlane = -1;

/* EVEX wants W set for the 64-bit-lane forms that VEX writes without it (vpaddq, vaddpd, vpsllq, vpbroadcastq ...);
   the bitwise operations are the same bits as their 32-bit forms */
static u32 evex_w(u32 enc)
{
    u32 op = XE_OPC(enc);
    if (!(enc & VE_Z) || zlane < 0 || (zlane != M_64 && zlane != M_F64))
        return enc;
    if (XE_MAP(enc) == 1 && XE_PP(enc) == 1 && (op == 0xdb || op == 0xdf || op == 0xeb || op == 0xef))
        return enc;
    return enc | 0x1000u;
}

/* d = instruction enc in form (XF_*) on s0 s1 s2; imm8 < 0: no immediate byte */
static MIns *emit_vex(u32 enc, int form, u32 d, u32 s0, u32 s1, u32 s2, int imm8)
{
    MIns *m;
    enc = evex_w(enc);
    m = emit(X_VEX, form, d, s0, s1);
    m->s2 = s2;
    m->imm = (i64)((u64)enc | (imm8 >= 0 ? (u64)1 << 32 | (u64)(u8)imm8 << 40 : 0));
    return m;
}

/* the internal form of a test: reg = s0, rm = s1, nothing written but the flags */
#define XF_TEST 16
/* ... and of -ffp-contract's vfmadd: d = s0 * s1 + s2 by a 213 encoding; the product commutes, so the 231 form (d the
   addend) or either factor in d's place saves the move */
#define XF_FMA 17

/* May the r/m operand of VEX instruction enc be memory? Not for the mask moves (the memory form is no instruction) nor
   for vmovhlps / vmovlhps (whose memory forms are other instructions). */
static int rm_mem_ok(u32 enc)
{
    u32 op = XE_OPC(enc);
    if (XE_MAP(enc) != 1)
        return 1;
    return op != 0x50 && op != 0xd7 && op != 0x12 && op != 0x16 && op != 0xc5 && op != 0xf7;
}

static int xt_is_vec(int xt)
{
    return xt >= XT_M512 && xt <= XT_M128I;
}

/* The operand of vector instruction u that its encoding can take from memory (its r/m), or 0. */
static u32 vec_mem_operand(u32 u)
{
    Ins *x = &f->ins[u];
    if (x->op == OP_XI) {
        const X86I *t = &x86i_tab[x->c];
        u32 r[3], n = 0, k;
        if (t->kind != XK_VEX || !xt_is_vec(t->ret) || !rm_mem_ok(t->enc))
            return 0;
        for (k = 0; k < x->b && k < 5; k++)
            if (t->arg[k] != XT_IMM) {
                if (n == 3)
                    return 0;
                r[n++] = f->xargs[x->a + k];
            }
        if (t->form == XF_RVM && n == 2 && r[0] != r[1])
            return r[1];
        if (t->form == XF_RVMS && n == 2 && r[0] != r[1])
            return r[0];
        if (t->form == XF_RM && n == 1)
            return r[0];
        return 0;
    }
    if (is_vec(x->ty) && ir_opcount(f, u) == 2 && vec_op_enc(x->op, x->mem) && x->a != x->b)
        return x->b;
    return 0;
}

static int refers(u32 u, u32 v)
{
    u32 n = ir_opcount(f, u), k;
    for (k = 0; k < n; k++)
        if (*ir_opref(f, u, k) == v)
            return 1;
    return 0;
}

/* d = instruction enc on register s0 (vvvv; NOREG: none) and, as its r/m, the memory the fused load ld reads */
static void emit_vex_load(u32 enc, u32 d, u32 s0, u32 ld, int imm8)
{
    Addr a;
    MIns *m;
    addr_of(f->ins[ld].a, &a);
    m = emit(X_VEXM, (s0 != NOREG ? 4 : 0) | (imm8 >= 0 ? 2 : 0), d, s0, a.base);
    m->s2 = a.idx;
    m->sym = a.sym;
    m->flags = a.flags;
    if (!fits32(a.off))
        die("%s: address offset out of range", f->name);
    m->imm = (i64)((u64)enc << 32 | (u32)a.off);
    m->cc = (u8)(imm8 >= 0 ? imm8 : 0);
}

/* d (or, with st, the memory from s0) = instruction enc with the memory operand a; s0 as vvvv when not st */
static void emit_vexm(u32 enc, int st, u32 d, u32 s0, Addr *a, int imm8)
{
    MIns *m;
    enc = evex_w(enc);
    m = emit(X_VEXM, (st ? 1 : s0 != NOREG ? 4 : 0) | (imm8 >= 0 ? 2 : 0), st ? NOREG : d, s0, a->base);
    m->s2 = a->idx;
    m->sym = a->sym;
    m->flags = a->flags;
    if (!fits32(a->off))
        die("%s: address offset out of range", f->name);
    m->imm = (i64)((u64)enc << 32 | (u32)a->off);
    m->cc = (u8)(imm8 >= 0 ? imm8 : 0);
}

/* a new vector register of 16 or 32 bytes */
static u32 vtmp_w(int wide)
{
    return new_vreg(cls_of(wide ? IT_V256 : IT_V128));
}

static u32 const_sym(const u8 *p, u32 n);

/* a new register with the n bytes at p repeated over 16 or 32 bytes (n = 1, 2, 4, 8, 16 or 32) */
static u32 vconst_w(int wide, const u8 *p, u32 n)
{
    u8 b[32];
    u32 k, t = vtmp_w(wide), w = wide ? 32 : 16;
    Addr a;
    for (k = 0; k < w; k++)
        b[k] = p[k % n];
    memset(&a, 0, sizeof a);
    a.base = a.idx = NOREG;
    a.sym = const_sym(b, w);
    a.flags = XF_SYM;
    emit_vexm(0x110 | (wide ? VE_L : 0), 0, t, NOREG, &a, -1); /* vmovups */
    return t;
}

static u32 vsplat_w(int wide, u64 val, int bytes)
{
    u8 b[8];
    int k;
    for (k = 0; k < 8; k++)
        b[k] = (u8)(val >> (8 * k));
    return vconst_w(wide, b, (u32)bytes);
}

/* the address of OP_VLOAD / OP_VSTORE v: a + b, or a + c for a store */
static void vaddr(u32 v, Addr *a)
{
    Ins *i = &f->ins[v];
    u32 off = i->op == OP_VLOAD ? i->b : i->c, base = reg_of(i->a);
    i64 c;
    if (const_of(off, &c) && fits32(c)) {
        reg_addr(a, base, c);
    } else {
        reg_addr(a, base, 0);
        a->idx = reg_of(off);
    }
}

static void sel_xi(u32 v);
static void sel_avx_more(u32 v);

/* Does sel_avx (VEX) make v? Intrinsics, 256-bit values and whole-vector loads and stores always; in a function compiled
   for AVX also the 128-bit operations it knows. The rest of the 128-bit work is sel_vec's (SSE2 / SSE4.1). */
static int avx_takes(u32 v)
{
    Ins *i = &f->ins[v];
    int op = i->op;
    if (op == OP_XI || ((op == OP_LOAD || op == OP_STORE) && i->mem >= M_V128))
        return 1;
    if (op == OP_PARAM || op == OP_CALL)
        return 0;
    if (op == OP_SELECT)
        return fn_avx && is_vec(i->ty) && is_vec(f->ins[i->a].ty);
    if (i->ty == IT_V256 || i->ty == IT_V512)
        return 1;
    if (i->ty != IT_V128 && op != OP_VSTORE && op != OP_VSUM)
        return 0;
    if (!fn_avx)
        return 0;
    switch (op) {
    case OP_CONST:
    case OP_VSPLAT:
    case OP_NEG:
    case OP_FNEG:
    case OP_NOT:
        return 1;
    case OP_VLOAD:
    case OP_VSTORE:
    case OP_VSUM:
    case OP_VIOTA:
    case OP_VGATHER:
    case OP_VMATH:
    case OP_SITOF:
    case OP_FTOSI:
    case OP_FEQ: case OP_FNE: case OP_FLT: case OP_FLE: case OP_FGT: case OP_FGE:
        return 1;
    case OP_SHL:
    case OP_SHR:
    case OP_SAR:
        return lane_bits(i->mem) != 8 && !(op == OP_SAR && lane_bits(i->mem) == 64) &&
               !(lane_bits(i->mem) == 16 && !is_floating(i->b) && f->ins[i->b].ty == IT_V128);
    default:
        return vec_op_enc(op, i->mem) != 0 || (op >= OP_EQ && op <= OP_UGE);
    }
}

static void sel_avx(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v, L = i->ty == IT_V512 ? VE_Z : i->ty == IT_V256 ? VE_L : 0, enc, t;
    int wc = i->ty == IT_V512 ? 2 : i->ty == IT_V256; /* 0 xmm, 1 ymm, 2 zmm */
    int op = i->op, b;
    i64 c;
    MIns *m;
    Addr a;

    zlane = i->ty == IT_V512 ? i->mem : -1;

    switch (op) {
    case OP_XI:
        sel_xi(v);
        return;
    case OP_LOAD:
        if (fused[v] == FUSE_LOAD)
            return; /* the memory operand of its user */
        addr_of(i->a, &a);
        emit_load(d, &a, i->mem);
        return;
    case OP_STORE: {
        u32 val = reg_of(i->b);
        addr_of(i->a, &a);
        emit_store(val, &a, i->mem);
        return;
    }
    case OP_VLOAD:
    case OP_VSTORE:
        if (i->flags & 0x80) {
            sel_avx_more(v);
            return;
        }
        {
        /* (opt_vec.c's: the address is a + b, or a + c for a store) */
        u32 off = op == OP_VLOAD ? i->b : i->c, base = reg_of(i->a);
        int mem = (op == OP_VLOAD ? i->ty : f->ins[i->b].ty) == IT_V256 ? M_V256 : M_V128;
        if (const_of(off, &c) && fits32(c)) {
            reg_addr(&a, base, c);
        } else {
            reg_addr(&a, base, 0);
            a.idx = reg_of(off);
        }
        if (op == OP_VLOAD)
            emit_load(d, &a, mem);
        else
            emit_store(reg_of(i->b), &a, mem);
        return;
    }
    case OP_CONST:
        materialise(v, d);
        return;
    case OP_VSPLAT: {
        int lane = i->mem, sty = f->ins[i->a].ty;
        if (is_const_splat(v)) {
            /* a broadcast from read-only data, where the splat is (out of loops): kept in a register, or, if ra spills
               it, one load per use */
            sel_splat_const(v, d);
            return;
        }
        if (sty == IT_F32)
            lane = M_F32;
        else if (sty == IT_F64)
            lane = M_F64;
        else if (sty == IT_I64 && lane_bits(lane) < 64)
            lane = M_64;
        m = emit(X_YSPLAT, wc, d, reg_of(i->a), NOREG);
        m->cc = (u8)lane;
        return;
    }
    case OP_SHL:
    case OP_SHR:
    case OP_SAR:
        b = lane_bits(i->mem);
        if (b == 8 || (op == OP_SAR && b == 64))
            die("%s: no x86-64 instruction shifts %d-bit lanes this way", f->name, b);
        if (const_of(i->b, &c)) {
            if (c >= b) {
                if (op != OP_SAR) {
                    emit(X_YZERO, 0, d, NOREG, NOREG);
                    return;
                }
                c = b - 1;
            }
            emit_vex(VE(b == 16 ? 0x71 : b == 32 ? 0x72 : 0x73, 1, 1, 0) | L | VE_DIGIT(op == OP_SHL ? 6 : op == OP_SHR ? 2 : 4), XF_VMI,
                     d, reg_of(i->a), NOREG, NOREG, (int)c);
        } else if (is_vec(f->ins[i->b].ty)) {
            if (b == 16)
                die("%s: no x86-64 instruction shifts 16-bit lanes by lanes", f->name);
            emit_vex(VE(op == OP_SHL ? 0x47 : op == OP_SHR ? 0x45 : 0x46, 2, 1, b == 64) | L, XF_RVM, d, reg_of(i->a), reg_of(i->b),
                     NOREG, -1);
        } else {
            t = new_vreg(1 | VC_V128);
            emit_vex(VE(0x6e, 1, 1, f->ins[i->b].ty == IT_I64), XF_RM, t, reg_of(i->b), NOREG, NOREG, -1); /* vmovd */
            enc = op == OP_SHL ? 0xf0 : op == OP_SHR ? 0xd0 : 0xe0;
            emit_vex(VE(enc + (b == 16 ? 1 : b == 32 ? 2 : 3), 1, 1, 0) | L, XF_RVM, d, reg_of(i->a), t, NOREG, -1);
        }
        return;
    case OP_NEG:
    case OP_FNEG:
    case OP_NOT:
        t = new_vreg(cls_of(i->ty));
        if (op == OP_NOT) {
            emit(X_YONES, wc, t, NOREG, NOREG);
            emit_vex(VE(0xef, 1, 1, 0) | L, XF_RVM, d, reg_of(i->a), t, NOREG, -1);
        } else if (op == OP_FNEG || i->mem == M_F32 || i->mem == M_F64) {
            m = emit(X_YSIGN, wc, t, NOREG, NOREG);
            m->cc = i->mem == M_F64 ? M_F64 : M_F32;
            emit_vex(VE(0xef, 1, 1, 0) | L, XF_RVM, d, reg_of(i->a), t, NOREG, -1);
        } else {
            emit(X_YZERO, 0, t, NOREG, NOREG);
            emit_vex(vec_op_enc(OP_SUB, i->mem) | L, XF_RVM, d, t, reg_of(i->a), NOREG, -1);
        }
        return;
    case OP_VSUM:
    case OP_VIOTA:
    case OP_VGATHER:
    case OP_VMATH:
    case OP_SITOF:
    case OP_FTOSI:
    case OP_SELECT:
        sel_avx_more(v);
        return;
    default:
        enc = vec_op_enc(op, i->mem);
        if (!enc) {
            if ((op >= OP_EQ && op <= OP_UGE) || (op >= OP_FEQ && op <= OP_FGE)) {
                sel_avx_more(v);
                return;
            }
            break;
        }
        if (fused[i->b] == FUSE_LOAD)
            emit_vex_load(enc | L, d, reg_of(i->a), i->b, -1);
        else
            emit_vex(enc | L, XF_RVM, d, reg_of(i->a), reg_of(i->b), NOREG, -1);
        return;
    }
    die("internal: x86-64 vector operation %d (lanes %d) in %s", op, i->mem, f->name);
}

/* d = the constant splat v, broadcast from read-only data */
static void sel_splat_const(u32 v, u32 d)
{
    Ins *i = &f->ins[v], *c = &f->ins[i->a];
    int sty = c->ty, bits = sty == IT_F32 ? 32 : sty == IT_F64 ? 64 : lane_bits(i->mem);
    u64 val = (u64)c->a | ((u64)c->b << 32);
    u8 b[8];
    u32 k, enc;
    MIns *m;
    if (sty == IT_I64 && bits < 64)
        bits = 64;
    for (k = 0; k < 8; k++)
        b[k] = (u8)(val >> (8 * k));
    enc = (bits == 8 ? 0x678 : bits == 16 ? 0x679 : bits == 32 ? 0x618 : 0x659) | (i->ty == IT_V256 ? 0x2000u : 0);
    if (bits == 64 && i->ty == IT_V256)
        enc = 0x2619; /* vbroadcastsd */
    if (i->ty == IT_V512)
        enc = (bits == 8 ? 0x678 : bits == 16 ? 0x679 : bits == 32 ? 0x618 : 0x1619) | VE_Z; /* (EVEX: vbroadcastsd is W1) */
    m = emit(X_VEXM, 0, d, NOREG, NOREG);
    m->s2 = NOREG;
    m->sym = const_sym(b, (u32)bits / 8);
    m->flags = XF_SYM;
    m->imm = (i64)((u64)enc << 32);
}

/* sel_avx for what opt_vec.c makes besides lane arithmetic: sums across lanes, lane numbers, sqrt / rounding, conversions,
   bitwise selects, comparisons, widening loads and narrowing stores; 128 or 256 bits, VEX encoded. The results are those of
   sel_vec's SSE code for the same operations. */
static void sel_avx_more(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v, L = i->ty == IT_V256 ? VE_L : 0, t, u, ra, rb;
    int op = i->op, k = i->mem, n = lane_bits(k) / 8, pd = k == M_F64, wide = i->ty == IT_V256;
    MIns *m;
    Addr a;

    switch (op) {
    case OP_VLOAD: {
        int nk = i->flags & 0x7f, byte = nk == M_8S || nk == M_8U, sgn = nk == M_8S || nk == M_16S;
        vaddr(v, &a);
        emit_vexm(VE(byte ? (sgn ? 0x21 : 0x31) : (sgn ? 0x23 : 0x33), 2, 1, 0) | L, 0, d, NOREG, &a, -1); /* vpmov[sz]x b/w d */
        return;
    }
    case OP_VSTORE: {
        /* each lane's low bits, sign-extended so that the saturating packs keep them as they are */
        int nk = i->flags & 0x7f, byte = nk == M_8S || nk == M_8U;
        u32 x = reg_of(i->b);
        wide = f->ins[i->b].ty == IT_V256;
        L = wide ? VE_L : 0;
        t = vtmp_w(wide);
        emit_vex(VE(0x72, 1, 1, 0) | L | VE_DIGIT(6), XF_VMI, t, x, NOREG, NOREG, byte ? 24 : 16); /* vpslld */
        u = vtmp_w(wide);
        emit_vex(VE(0x72, 1, 1, 0) | L | VE_DIGIT(4), XF_VMI, u, t, NOREG, NOREG, byte ? 24 : 16); /* vpsrad */
        t = vtmp_w(wide);
        emit_vex(VE(0x6b, 1, 1, 0) | L, XF_RVM, t, u, u, NOREG, -1); /* vpackssdw */
        if (wide) {
            u = vtmp_w(1);
            emit_vex(VE(0x00, 3, 1, 1) | VE_L, XF_RM, u, t, NOREG, NOREG, 0x08); /* vpermq: the two halves' words together */
            t = u;
        }
        if (byte) {
            u = vtmp_w(0);
            emit_vex(VE(0x63, 1, 1, 0), XF_RVM, u, t, t, NOREG, -1); /* vpacksswb */
            t = u;
        }
        vaddr(v, &a);
        emit_vexm(wide ? (byte ? VE(0xd6, 1, 1, 0) : VE(0x7f, 1, 2, 0)) : byte ? VE(0x7e, 1, 1, 0) : VE(0xd6, 1, 1, 0), 1, NOREG, t,
                  &a, -1); /* vmovq / vmovdqu / vmovd */
        return;
    }
    case OP_VSUM: {
        u32 x = reg_of(i->a), lo, hi;
        int fl = (int)i->flags, ak = k, fsum = (ak == M_F32 || ak == M_F64);
        if (f->ins[i->a].ty == IT_V256) {
            hi = new_vreg(1 | VC_V128);
            emit_vex(VE(0x39, 3, 1, 0) | VE_L, XF_MR, hi, x, NOREG, NOREG, 1); /* vextracti128 */
            lo = new_vreg(1 | VC_V128);
            emit(X_YMOVLO, 0, lo, x, NOREG);
            if ((fl == 0 && !fsum) || fl >= 5) {
                /* each half summed (in its wider total), then the two added */
                u32 s1 = new_vreg(cls_of(i->ty)), s2 = new_vreg(cls_of(i->ty));
                m = emit(X_VSUM, 0, s1, lo, NOREG);
                m->cc = (u8)ak;
                m->imm = fl;
                m = emit(X_VSUM, 0, s2, hi, NOREG);
                m->cc = (u8)ak;
                m->imm = fl;
                emit(X_ADD, is_w(i->ty), d, s1, s2);
                return;
            }
            {
                static const u8 across[5] = {0, 0x3d, 0x39, 0x3f, 0x3b}; /* vpmaxsd vpminsd vpmaxud vpminud */
                u32 c = new_vreg(1 | VC_V128);
                u32 enc = !fl ? VE(0x58, 1, ak == M_F64, 0)                          /* vaddps / pd */
                          : fsum ? VE(fl == 1 ? 0x5f : 0x5d, 1, ak == M_F64, 0) /* vmaxps / vminps (pd) */
                                 : VE(across[fl], 2, 1, 0);
                emit_vex(enc, XF_RVM, c, lo, hi, NOREG, -1);
                x = c;
            }
        } else {
            /* sel_vec's SSE code does the rest, on a register it may keep (it has no spill slots) */
            t = new_vreg(1 | VC_V128);
            emit(X_MOV, 1, t, x, NOREG);
            x = t;
        }
        m = emit(X_VSUM, 0, d, x, NOREG);
        m->cc = (u8)ak;
        m->imm = fl;
        return;
    }
    case OP_VIOTA: {
        u8 b[32];
        u64 st = (u64)(i64)(i32)i->a;
        u32 j, w = wide ? 32 : 16, nl = w / (u32)n;
        for (j = 0; j < nl; j++) {
            u64 x = st * j;
            int q;
            for (q = 0; q < n; q++)
                b[j * (u32)n + (u32)q] = (u8)(x >> (8 * q));
        }
        t = vconst_w(wide, b, w);
        emit(X_MOV, 1, d, t, NOREG);
        return;
    }
    case OP_VGATHER: {
        /* vpgatherdd / vgatherdps, every lane (AVX2: opt_vec.c makes these only then); an element narrower than four
           bytes is read as the four bytes there (opt_vec.c's range check keeps the reads inside the object: at most 3
           bytes past an element it reads anyway) and cut to its width */
        int ek = i->flags, sc = (int)i->c, eb = lane_bits(ek);
        u32 idx = reg_of(i->b), g = eb == 32 ? d : vtmp_w(wide);
        if (sc > 3) {
            u32 t2 = vtmp_w(wide);
            emit_vex(VE(0x72, 1, 1, 0) | L | VE_DIGIT(6), XF_VMI, t2, idx, NOREG, NOREG, sc); /* vpslld */
            idx = t2;
            sc = 0;
        }
        m = emit(X_GATHER, 0, g, NOREG, reg_of(i->a));
        m->s2 = idx;
        m->cc = (u8)(1 << sc);
        m->imm = (i64)(VE(ek == M_F32 ? 0x92 : 0x90, 2, 1, 0) | L);
        if (eb == 32)
            return;
        if (ek == M_8U || ek == M_16U) {
            emit_vex(VE(0xdb, 1, 1, 0) | L, XF_RVM, d, g, vsplat_w(wide, eb == 8 ? 0xff : 0xffff, 4), NOREG, -1); /* vpand */
        } else {
            t = vtmp_w(wide);
            emit_vex(VE(0x72, 1, 1, 0) | L | VE_DIGIT(6), XF_VMI, t, g, NOREG, NOREG, 32 - eb); /* vpslld */
            emit_vex(VE(0x72, 1, 1, 0) | L | VE_DIGIT(4), XF_VMI, d, t, NOREG, NOREG, 32 - eb); /* vpsrad */
        }
        return;
    }
    case OP_VMATH: {
        static const u8 rnd[6] = {0, 9, 10, 11, 4, 12};
        if (i->b == VM_SQRT)
            emit_vex(VE(0x51, 1, pd, 0) | L, XF_RM, d, reg_of(i->a), NOREG, NOREG, -1); /* vsqrtps / pd */
        else
            emit_vex(VE(pd ? 0x09 : 0x08, 3, 1, 0) | L, XF_RM, d, reg_of(i->a), NOREG, NOREG, rnd[i->b]); /* vroundps / pd */
        return;
    }
    case OP_SITOF:
    case OP_FTOSI:
        emit_vex(VE(0x5b, 1, op == OP_SITOF ? 0 : 2, 0) | L, XF_RM, d, reg_of(i->a), NOREG, NOREG, -1); /* vcvtdq2ps, vcvttps2dq */
        return;
    case OP_SELECT: {
        u32 c = reg_of(i->a);
        if (i->ty != IT_V512) {
            /* the mask is whole lanes (a comparison's): vpblendvb, whatever their width */
            emit_vex(VE(0x4c, 3, 1, 0) | L, XF_RVMR, d, reg_of(i->c), reg_of(i->b), c, -1);
            return;
        }
        t = vtmp_w(wide);
        emit_vex(VE(0xdb, 1, 1, 0) | L, XF_RVM, t, c, reg_of(i->b), NOREG, -1); /* vpand */
        u = vtmp_w(wide);
        emit_vex(VE(0xdf, 1, 1, 0) | L, XF_RVM, u, c, reg_of(i->c), NOREG, -1); /* vpandn */
        emit_vex(VE(0xeb, 1, 1, 0) | L, XF_RVM, d, t, u, NOREG, -1);          /* vpor */
        return;
    }
    default:
        break;
    }
    ra = reg_of(i->a);
    rb = reg_of(i->b);
    if (op >= OP_FEQ && op <= OP_FGE) {
        /* vcmpps / vcmppd: 0 eq, 1 lt, 2 le, 4 neq (true where unordered, as != is) */
        int p = op == OP_FEQ ? 0 : op == OP_FNE ? 4 : op == OP_FLT || op == OP_FGT ? 1 : 2;
        if (op == OP_FGT || op == OP_FGE) {
            t = ra;
            ra = rb;
            rb = t;
        }
        emit_vex(VE(0xc2, 1, pd, 0) | L, XF_RVM, d, ra, rb, NOREG, p);
        return;
    }
    if (op >= OP_EQ && op <= OP_UGE) {
        /* vpcmpeq, vpcmpgt: "less" is "greater" turned round, "or equal" the opposite of the strict one turned round;
           without sign, both flipped in the top bit first */
        u32 eq = n == 8 ? VE(0x29, 2, 1, 0) : VE(n == 1 ? 0x74 : n == 2 ? 0x75 : 0x76, 1, 1, 0);
        u32 gt = n == 8 ? VE(0x37, 2, 1, 0) : VE(n == 1 ? 0x64 : n == 2 ? 0x65 : 0x66, 1, 1, 0);
        int sw = 0, neg = 0;
        if (op == OP_EQ || op == OP_NE) {
            if (op == OP_EQ) {
                emit_vex(eq | L, XF_RVM, d, ra, rb, NOREG, -1);
                return;
            }
            t = vtmp_w(wide);
            emit_vex(eq | L, XF_RVM, t, ra, rb, NOREG, -1);
            neg = 1;
        } else {
            if (op >= OP_ULT) {
                u32 bias = vsplat_w(wide, (u64)1 << (8 * n - 1), n), x = vtmp_w(wide), y = vtmp_w(wide);
                emit_vex(VE(0xef, 1, 1, 0) | L, XF_RVM, x, ra, bias, NOREG, -1);
                emit_vex(VE(0xef, 1, 1, 0) | L, XF_RVM, y, rb, bias, NOREG, -1);
                ra = x;
                rb = y;
                op += OP_SLT - OP_ULT;
            }
            switch (op) {
            case OP_SGT: break;
            case OP_SLT: sw = 1; break;
            case OP_SGE: sw = 1; neg = 1; break; /* a >= b: not (b > a) */
            default: neg = 1; break;              /* a <= b: not (a > b) */
            }
            if (sw) {
                t = ra;
                ra = rb;
                rb = t;
            }
            if (!neg) {
                emit_vex(gt | L, XF_RVM, d, ra, rb, NOREG, -1);
                return;
            }
            t = vtmp_w(wide);
            emit_vex(gt | L, XF_RVM, t, ra, rb, NOREG, -1);
        }
        u = vtmp_w(wide);
        emit(X_YONES, wide, u, NOREG, NOREG);
        emit_vex(VE(0xef, 1, 1, 0) | L, XF_RVM, d, t, u, NOREG, -1); /* not */
        return;
    }
    die("internal: x86-64 AVX instruction selection: op %d in %s", op, f->name);
}

/* An intrinsic (OP_XI): the instruction its table entry describes. */
static void sel_xi(u32 v)
{
    Ins *i = &f->ins[v];
    const X86I *x = &x86i_tab[i->c];
    u32 d = x->ret == XT_VOID ? NOREG : VREG0 + v, r[5], k, n = 0, enc = x->enc, t, t2;
    u32 *args = f->xargs + i->a;
    int imm = x->extra & 0x100 ? (int)(x->extra & 0xff) : -1;
    i64 c = 0;
    MIns *m;
    Addr a;

    zlane = -1; /* the table's encodings say W */
    for (k = 0; k < i->b && k < 5; k++)
        if (x->arg[k] == XT_IMM) {
            if (!const_of(args[k], &c))
                die("%s: an argument of _%s must be a constant", f->name, x->name);
            imm = (int)(c & 0xff);
        }
    switch (x->kind) {
    case XK_VEX:
        t = vec_mem_operand(v);
        if (t && fused[t] == FUSE_LOAD) {
            /* the r/m operand is in memory: the other source, if there is one, is vvvv */
            u32 other = NOREG;
            for (k = 0; k < i->b; k++)
                if (x->arg[k] != XT_IMM && args[k] != t)
                    other = reg_of(args[k]);
            emit_vex_load(enc, d, other, t, imm);
            return;
        }
        for (k = 0; k < i->b; k++)
            if (x->arg[k] != XT_IMM)
                r[n++] = reg_of(args[k]);
        for (k = n; k < 3; k++)
            r[k] = NOREG;
        if (x->form == XF_RVMS)
            emit_vex(enc, XF_RVM, d, r[1], r[0], NOREG, imm);
        else if (x->form == XF_DVM)
            /* b first into d, a as vvvv: the 213 form computes vvvv * d + rm, and a NaN in the first factor (vvvv)
               is the one that comes out, as clang places _mm256_fmadd_ps(a, b, c) */
            emit_vex(enc, XF_DVM, d, r[1], r[0], r[2], imm);
        else
            emit_vex(enc, x->form, d, r[0], r[1], r[2], imm);
        return;
    case XK_VLDM:
    case XK_VSTM:
        if (x->kind == XK_VSTM)
            t = reg_of(args[1]);
        else
            t = NOREG;
        addr_of(args[0], &a);
        m = emit(X_VEXM, x->kind == XK_VSTM, x->kind == XK_VSTM ? NOREG : d, t, a.base);
        m->s2 = a.idx;
        m->sym = a.sym;
        m->flags = a.flags;
        if (!fits32(a.off))
            die("%s: address offset out of range", f->name);
        m->imm = (i64)((u64)enc << 32 | (u32)a.off);
        return;
    case XK_MASKOP:
        emit(X_KMOVK, 0, NOREG, reg_of(args[1]), NOREG);
        emit_vex(enc, XF_DVMK, d, reg_of(args[0]), reg_of(args[2]), reg_of(args[3]), imm);
        return;
    case XK_KTEST:
        m = emit(X_KTEST, 0, d, reg_of(args[0]), reg_of(args[1]));
        m->imm = enc;
        m->cc = (u8)(x->ret == XT_U64 ? 8 : x->ret == XT_U32 ? 4 : 2);
        return;
    case XK_LO:
        emit(X_YMOVLO, 0, d, reg_of(args[0]), NOREG);
        return;
    case XK_BCAST128:
        t = reg_of(args[0]);
        emit_vex(VE(0x38, 3, 1, 0) | VE_L, XF_RVM, d, t, t, NOREG, 1); /* vinserti128 d, t, t, 1 */
        return;
    case XK_EXTRACT: {
        int bits = lane_bits((int)x->extra), per = 128 / bits;
        t = reg_of(args[0]);
        imm &= 256 / bits - 1;
        if (imm >= per) {
            t2 = new_vreg(1 | VC_V128);
            emit_vex(VE(0x39, 3, 1, 0) | VE_L, XF_MR, t2, t, NOREG, NOREG, 1); /* vextracti128 */
            t = t2;
            imm -= per;
        }
        if (bits == 16)
            emit_vex(VE(0xc5, 1, 1, 0), XF_RM, d, t, NOREG, NOREG, imm); /* vpextrw */
        else if (bits == 32 && imm == 0)
            emit_vex(VE(0x7e, 1, 1, 0), XF_MR, d, t, NOREG, NOREG, -1); /* vmovd */
        else
            emit_vex(VE(bits == 8 ? 0x14 : 0x16, 3, 1, bits == 64), XF_MR, d, t, NOREG, NOREG, imm); /* vpextrb / d / q */
        return;
    }
    case XK_TESTZ:
    case XK_TESTC:
        emit_vex(enc, XF_TEST, NOREG, reg_of(args[0]), reg_of(args[1]), NOREG, -1);
        m = emit(X_SETCC, 0, d, NOREG, NOREG);
        m->cc = x->kind == XK_TESTZ ? CC_E : CC_B;
        return;
    case XK_GATHER:
    case XK_MGATHER: {
        int masked = x->kind == XK_MGATHER;
        u32 src = masked ? reg_of(args[0]) : NOREG, base = reg_of(args[masked]), idx = reg_of(args[masked + 1]);
        if (imm != 1 && imm != 2 && imm != 4 && imm != 8)
            die("%s: the scale of _%s must be 1, 2, 4 or 8", f->name, x->name);
        if (masked)
            emit(X_MOV, 1, FSCR1, reg_of(args[3]), NOREG);
        m = emit(X_GATHER, masked, d, src, base);
        m->s2 = idx;
        m->cc = (u8)imm;
        m->imm = enc;
        return;
    }
    case XK_CVTSH:
        t = new_vreg(1 | VC_V128);
        emit_vex(VE(0x6e, 1, 1, 0), XF_RM, t, reg_of(args[0]), NOREG, NOREG, -1); /* vmovd */
        emit_vex(enc, XF_RM, d, t, NOREG, NOREG, -1);                             /* vcvtph2ps */
        return;
    case XK_CVTSS_SH:
        t = new_vreg(1 | VC_V128);
        t2 = new_vreg(0);
        emit_vex(enc, XF_MR, t, reg_of(args[0]), NOREG, NOREG, imm); /* vcvtps2ph */
        emit_vex(VE(0x7e, 1, 1, 0), XF_MR, t2, t, NOREG, NOREG, -1); /* vmovd */
        emit(X_MOVZX16, 0, d, t2, NOREG);
        return;
    case XK_CPUID: {
        i64 leaf = 0, sub = 0, reg = 0;
        const_of(args[0], &leaf);
        const_of(args[1], &sub);
        const_of(args[2], &reg);
        m = emit(X_CPUID, 0, RAX, NOREG, NOREG);
        m->imm = (i64)((u64)(u32)leaf | (u64)(u32)sub << 32);
        m->cc = (u8)reg;
        emit(X_DEFREG, 1, RCX, NOREG, NOREG);
        emit(X_DEFREG, 1, RDX, NOREG, NOREG);
        emit(X_MOV, 1, d, RAX, NOREG);
        return;
    }
    case XK_RAW:
        m = emit(X_RAW, 0, NOREG, NOREG, NOREG);
        m->imm = enc;
        return;
    case XK_PREFETCH:
        addr_of(args[0], &a);
        m = emit(X_PREFETCH, 0, NOREG, a.base, a.idx);
        set_mem(m, &a);
        m->cc = (u8)(imm == 3 ? 1 : imm == 2 ? 2 : imm == 1 ? 3 : 0);
        return;
    }
    die("internal: intrinsic _%s reached the x86-64 back end", x->name);
}

/* ---- vectors (opt_vec.c's IT_V128 values) ---- */

#define VP66 (O_66 | O_0F)
#define VIMM(b) ((i64)1 << 40 | (i64)(u8)(b) << 32)

static int lane_bytes(int k)
{
    return mem_bytes[k];
}

static u32 vbin(u32 d, i64 code, u32 a, u32 b, int commut)
{
    MIns *m = emit(X_VBIN, commut, d, a, b);
    m->imm = code;
    return d;
}

static u32 vtmp(void)
{
    return new_vreg(3);
}

/* a vector of two 64-bit constants */
static u32 vconst(u32 d, u64 lo, u64 hi)
{
    u32 a, b;
    MIns *m;
    if (lo == hi && (lo == 0 || lo == ~(u64)0)) {
        m = emit(X_VONES, 0, d, NOREG, NOREG);
        m->imm = lo == 0;
        return d;
    }
    a = new_vreg(0);
    b = new_vreg(0);
    m = emit(X_MOVI, 1, a, NOREG, NOREG);
    m->imm = (i64)lo;
    m = emit(X_MOVI, 1, b, NOREG, NOREG);
    m->imm = (i64)hi;
    emit(X_VPAIR, 0, d, a, b);
    return d;
}

/* every lane of lane kind k with the constant c */
static u32 vsplat_const(u32 d, int k, u64 c)
{
    int n = lane_bytes(k);
    u64 x = n == 8 ? c : n == 4 ? (c & 0xffffffffU) * 0x100000001UL : n == 2 ? (c & 0xffff) * 0x0001000100010001UL : (c & 0xff) * 0x0101010101010101UL;
    return vconst(d, x, x);
}

static u32 vnot(u32 d, u32 a)
{
    u32 t = vtmp();
    emit(X_VONES, 0, t, NOREG, NOREG);
    return vbin(d, VP66 | 0xef, a, t, 1); /* pxor */
}

/* the opcode of pmaxub and its like for lanes of n bytes */
static i64 minmax_code(int op, int n)
{
    static const u32 codes[3][4] = {
        /* smax smin umax umin */
        { O_38 | 0x3c, O_38 | 0x38, 0xde, 0xda },        /* bytes */
        { 0xee, 0xea, O_38 | 0x3e, O_38 | 0x3a },        /* words */
        { O_38 | 0x3d, O_38 | 0x39, O_38 | 0x3f, O_38 | 0x3b }, /* dwords */
    };
    return VP66 | codes[n == 1 ? 0 : n == 2 ? 1 : 2][op - OP_VSMAX];
}

static void sel_vec(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v, ra, rb;
    int k = i->mem, n = lane_bytes(k), fl = k == M_F32 || k == M_F64, sh = n == 1 ? 0 : n == 2 ? 1 : n == 4 ? 2 : 3;
    MIns *m;
    switch (i->op) {
    case OP_VLOAD:
        m = emit(X_VLD, (i->flags & 0x80) != 0, d, reg_of(i->a), reg_of(i->b));
        m->cc = i->flags & 0x7f;
        return;
    case OP_VSTORE:
        m = emit(X_VST, (i->flags & 0x80) != 0, NOREG, reg_of(i->b), reg_of(i->a));
        m->s2 = reg_of(i->c);
        m->cc = i->flags & 0x7f;
        return;
    case OP_VSPLAT: {
        u64 c;
        if (ir_is_const(f, i->a, &c)) {
            vsplat_const(d, k, c);
            return;
        }
        if (f->ins[i->a].op == OP_FCONST) {
            c = (u64)f->ins[i->a].a | (u64)f->ins[i->a].b << 32;
            vsplat_const(d, k, k == M_F32 ? c & 0xffffffffU : c);
            return;
        }
        m = emit(X_VSPLAT, is_fp(f->ins[i->a].ty), d, reg_of(i->a), NOREG);
        m->cc = (u8)k;
        return;
    }
    case OP_VIOTA: {
        u64 st = (u64)(i64)(i32)i->a;
        if (n == 8)
            vconst(d, 0, st);
        else
            vconst(d, (st & 0xffffffffU) << 32, ((st * 2) & 0xffffffffU) | ((st * 3) & 0xffffffffU) << 32);
        return;
    }
    case OP_VSUM:
        m = emit(X_VSUM, 0, d, reg_of(i->a), NOREG);
        m->cc = (u8)k;
        m->imm = i->flags;
        return;
    case OP_SELECT:
        m = emit(X_VSEL, 0, d, reg_of(i->a), reg_of(i->b));
        m->s2 = reg_of(i->c);
        return;
    case OP_SHL: case OP_SHR: case OP_SAR: {
        /* psllw pslld psllq, psrlw ..., psraw psrad: 66 0f 71..73 /6 /2 /4 */
        m = emit(X_VSHI, 0, d, reg_of(i->a), NOREG);
        m->imm = VP66 | (0x70 + sh);
        m->cc = i->op == OP_SHL ? 6 : i->op == OP_SHR ? 2 : 4;
        m->sym = f->ins[i->b].a;
        return;
    }
    case OP_VGATHER:
        m = emit(X_VGATH, 0, d, reg_of(i->a), reg_of(i->b));
        m->cc = i->flags;
        m->sym = i->c;
        return;
    case OP_VMATH: {
        /* sqrtps sqrtpd; roundps roundpd: down 9, up 10, toward zero 11, the current way 4 (12: without inexact) */
        static const u8 rnd[6] = { 0, 9, 10, 11, 4, 12 };
        m = emit(X_VUN, 0, d, reg_of(i->a), NOREG);
        m->imm = i->b == VM_SQRT ? (n == 8 ? VP66 : O_0F) | 0x51 : VP66 | O_3A | (n == 8 ? 0x09 : 0x08) | VIMM(rnd[i->b]);
        return;
    }
    case OP_SITOF: /* cvtdq2ps */
    case OP_FTOSI: /* cvttps2dq */
        m = emit(X_VUN, 0, d, reg_of(i->a), NOREG);
        m->imm = (i->op == OP_SITOF ? 0 : O_F3) | O_0F | 0x5b;
        return;
    case OP_NEG: {
        static const u8 psub[4] = { 0xf8, 0xf9, 0xfa, 0xfb };
        u32 z = vtmp();
        ra = reg_of(i->a);
        m = emit(X_VONES, 0, z, NOREG, NOREG);
        m->imm = 1; /* zeros */
        vbin(d, VP66 | psub[sh], z, ra, 0);
        return;
    }
    case OP_FNEG: {
        u32 t = vtmp();
        ra = reg_of(i->a);
        vsplat_const(t, k, k == M_F32 ? 0x80000000U : 0x8000000000000000UL);
        vbin(d, VP66 | 0xef, ra, t, 1);
        return;
    }
    default:
        break;
    }
    ra = reg_of(i->a);
    rb = reg_of(i->b);
    switch (i->op) {
    case OP_ADD: {
        static const u8 padd[4] = { 0xfc, 0xfd, 0xfe, 0xd4 };
        vbin(d, VP66 | padd[sh], ra, rb, 1);
        return;
    }
    case OP_SUB: {
        static const u8 psub[4] = { 0xf8, 0xf9, 0xfa, 0xfb };
        vbin(d, VP66 | psub[sh], ra, rb, 0);
        return;
    }
    case OP_MUL: vbin(d, n == 2 ? VP66 | 0xd5 : VP66 | O_38 | 0x40, ra, rb, 1); return; /* pmullw, pmulld */
    case OP_AND: vbin(d, VP66 | 0xdb, ra, rb, 1); return;
    case OP_OR: vbin(d, VP66 | 0xeb, ra, rb, 1); return;
    case OP_XOR: vbin(d, VP66 | 0xef, ra, rb, 1); return;
    case OP_FADD: vbin(d, (n == 8 ? VP66 : O_0F) | 0x58, ra, rb, 1); return;
    case OP_FSUB: vbin(d, (n == 8 ? VP66 : O_0F) | 0x5c, ra, rb, 0); return;
    case OP_FMUL: vbin(d, (n == 8 ? VP66 : O_0F) | 0x59, ra, rb, 1); return;
    case OP_FDIV: vbin(d, (n == 8 ? VP66 : O_0F) | 0x5e, ra, rb, 0); return;
    case OP_VSMAX: case OP_VSMIN: case OP_VUMAX: case OP_VUMIN:
        if (fl) { /* maxps maxpd minps minpd (-ffast-math reductions) */
            vbin(d, (n == 8 ? VP66 : O_0F) | (i->op == OP_VSMAX ? 0x5f : 0x5d), ra, rb, 0); /* (the second where unordered: in order) */
            return;
        }
        vbin(d, minmax_code(i->op, n), ra, rb, 1);
        return;
    case OP_FEQ: case OP_FNE: case OP_FLT: case OP_FLE: case OP_FGT: case OP_FGE: {
        /* cmpps / cmppd: 0 eq, 1 lt, 2 le, 4 neq (true where unordered, as != is) */
        int p = i->op == OP_FEQ ? 0 : i->op == OP_FNE ? 4 : i->op == OP_FLT || i->op == OP_FGT ? 1 : 2;
        if (i->op == OP_FGT || i->op == OP_FGE) {
            u32 t = ra;
            ra = rb;
            rb = t;
        }
        vbin(d, (n == 8 ? VP66 : O_0F) | 0xc2 | VIMM(p), ra, rb, p == 0 || p == 4);
        return;
    }
    default:
        break;
    }
    if (i->op >= OP_EQ && i->op <= OP_UGE) {
        /* pcmpeq, pcmpgt: "less" is "greater" turned round, "or equal" the opposite of the strict one turned round;
           without sign, both flipped in the top bit first */
        static const u8 peq[3] = { 0x74, 0x75, 0x76 }, pgt[3] = { 0x64, 0x65, 0x66 };
        int op = i->op, sw = 0, neg = 0;
        if (op == OP_EQ || op == OP_NE) {
            if (op == OP_NE) {
                u32 t = vtmp();
                vbin(t, VP66 | peq[sh], ra, rb, 1);
                vnot(d, t);
            } else {
                vbin(d, VP66 | peq[sh], ra, rb, 1);
            }
            return;
        }
        if (op >= OP_ULT) {
            u32 bias = vtmp(), x = vtmp(), y = vtmp();
            vsplat_const(bias, k, (u64)1 << (8 * n - 1));
            vbin(x, VP66 | 0xef, ra, bias, 1);
            vbin(y, VP66 | 0xef, rb, bias, 1);
            ra = x;
            rb = y;
            op += OP_SLT - OP_ULT;
        }
        switch (op) {
        case OP_SGT: break;
        case OP_SLT: sw = 1; break;
        case OP_SGE: sw = 1; neg = 1; break; /* a >= b: not (b > a) */
        default: neg = 1; break;              /* a <= b: not (a > b) */
        }
        if (sw) {
            u32 t = ra;
            ra = rb;
            rb = t;
        }
        if (neg) {
            u32 t = vtmp();
            vbin(t, VP66 | pgt[sh], ra, rb, 0);
            vnot(d, t);
        } else {
            vbin(d, VP66 | pgt[sh], ra, rb, 0);
        }
        return;
    }
    (void)fl;
    die("internal: x86-64 vector instruction selection: op %d in %s", i->op, f->name);
}

static void sel_ins(u32 v)
{
    Ins *i = &f->ins[v];
    u32 d = VREG0 + v;
    int w = is_w(i->ty), op = 0;
    i64 c;
    MIns *m;
    Addr a;

    if (avx_takes(v)) {
        sel_avx(v);
        return;
    }
    if ((i->ty == IT_V128 && i->op != OP_PARAM && i->op != OP_CALL &&
         !(i->op == OP_SELECT && !is_vec(f->ins[i->a].ty))) ||
        i->op == OP_VSTORE || i->op == OP_VSUM) {
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
        if (const_of(i->b, &c) && fits32(c) && c != -0x7fffffffL - 1) {
            m = emit(X_ADDI, w, d, reg_of(i->a), NOREG);
            m->imm = i->op == OP_ADD ? c : -c;
            return;
        }
        if (i->op == OP_ADD && (fused[i->b] == FUSE_SKIP || fused[i->a] == FUSE_SKIP)) {
            /* lea d, [a + b * 2^k] */
            u32 y = fused[i->b] == FUSE_SKIP ? i->b : i->a, x = y == i->b ? i->a : i->b;
            u64 sh = 0;
            ir_is_const(f, f->ins[y].b, &sh);
            m = emit(X_LEAX, w, d, reg_of(x), reg_of(f->ins[y].a));
            m->imm = (i64)sh;
            return;
        }
        op = i->op == OP_ADD ? X_ADD : X_SUB;
        goto binary;
    case OP_MUL:
        if (const_of(i->b, &c) && fits32(c)) {
            m = emit(X_IMULI, w, d, reg_of(i->a), NOREG);
            m->imm = c;
            return;
        }
        op = X_IMUL;
        goto binary;
    case OP_FADD:
    case OP_FSUB:
        if (fused[i->a] == FUSE_FMA || fused[i->b] == FUSE_FMA) {
            /* (-ffp-contract) a * b + c in one rounding: the 213 form, d = s1 * s0 + s2 with s0 first in d */
            int left = fused[i->a] == FUSE_FMA, dbl = i->ty == IT_F64;
            Ins *p = &f->ins[left ? i->a : i->b];
            u32 c2 = left ? i->b : i->a;
            /* a*b + c: vfmadd (a9); a*b - c: vfmsub (ab); c - a*b: vfnmadd (ad) */
            u32 opc = i->op == OP_FADD ? 0xa9 : left ? 0xab : 0xad;
            emit_vex(VE(opc, 2, 1, dbl), XF_FMA, d, reg_of(p->a), reg_of(p->b), reg_of(c2), -1);
            return;
        }
        op = i->op == OP_FADD ? X_FADD : X_FSUB;
        goto binary;
    case OP_FMUL:
        if (fused[v] == FUSE_FMA)
            return; /* in its user's vfmadd */
        op = X_FMUL;
        goto binary;
    case OP_FDIV: op = X_FDIV; goto binary;
    binary: {
        u32 ra;
        if (fused[i->a] == FUSE_LOAD || fused[i->b] == FUSE_LOAD) {
            int left = fused[i->a] == FUSE_LOAD;
            ra = reg_of(left ? i->b : i->a);
            addr_of(f->ins[left ? i->a : i->b].a, &a);
            switch (op) {
            case X_ADD: op = X_ADDM; break;
            case X_SUB: op = X_SUBM; break;
            case X_IMUL: op = X_IMULM; break;
            case X_AND: op = X_ANDM; break;
            case X_OR: op = X_ORM; break;
            case X_XOR: op = X_XORM; break;
            case X_FADD: op = X_FADDM; break;
            case X_FSUB: op = X_FSUBM; break;
            case X_FMUL: op = X_FMULM; break;
            default: op = X_FDIVM; break;
            }
            m = emit(op, i->ty == IT_I64 || i->ty == IT_F64, d, ra, a.base);
            m->s2 = a.idx;
            set_mem(m, &a);
            return;
        }
        if (op == X_FADD || op == X_FSUB || op == X_FMUL || op == X_FDIV) {
            /* a constant operand straight from the constant section: no register for it (a DCT has dozens) */
            int cb = f->ins[i->b].op == OP_FCONST, ca = !cb && (op == X_FADD || op == X_FMUL) && f->ins[i->a].op == OP_FCONST;
            if (cb || ca) {
                Ins *k = &f->ins[ca ? i->a : i->b];
                Addr ka;
                memset(&ka, 0, sizeof ka);
                ka.base = ka.idx = NOREG;
                ka.flags = XF_SYM;
                ka.sym = fconst_sym(w ? ((u64)k->a | (u64)k->b << 32) : k->a, w ? 8 : 4);
                switch (op) {
                case X_FADD: op = X_FADDM; break;
                case X_FSUB: op = X_FSUBM; break;
                case X_FMUL: op = X_FMULM; break;
                default: op = X_FDIVM; break;
                }
                m = emit(op, w, d, reg_of(ca ? i->b : i->a), NOREG);
                m->s2 = NOREG;
                set_mem(m, &ka);
                return;
            }
        }
        ra = reg_of(i->a);
        m = emit(op, w, d, ra, reg_of(i->b));
        if (op == X_FSUB || op == X_FDIV || op == X_SUB)
            m->flags |= MF_NOTS1;
        return;
    }
    case OP_SDIV:
    case OP_UDIV:
    case OP_SREM:
    case OP_UREM: {
        int sgn = i->op == OP_SDIV || i->op == OP_SREM, rem = i->op == OP_SREM || i->op == OP_UREM;
        u32 ra, rb, pv;
        if (fused[v] == FUSE_DIVR)
            return; /* the earlier division set it */
        pv = div_pair(v, 0);
        ra = reg_of(i->a), rb = reg_of(i->b);
        emit(X_MOV, 1, RAX, ra, NOREG);
        if (sgn) {
            m = emit(X_CQO, w, RDX, NOREG, NOREG);
            m->pmask = BIT(RAX);
        } else {
            m = emit(X_MOVI, 0, RDX, NOREG, NOREG);
            m->imm = 0;
        }
        m = emit(sgn ? X_IDIV : X_DIV, w, RAX, rb, NOREG);
        m->pmask = BIT(RAX) | BIT(RDX);
        emit(X_DEFREG, 1, RDX, NOREG, NOREG);
        emit(X_MOV, 1, d, rem ? RDX : RAX, NOREG);
        if (pv && fused[pv] == FUSE_DIVR)
            emit(X_MOV, 1, VREG0 + pv, rem ? RAX : RDX, NOREG);
        return;
    }
    case OP_UMULH:
    case OP_SMULH: {
        u32 ra = reg_of(i->a), rb = reg_of(i->b);
        emit(X_MOV, 1, RAX, ra, NOREG);
        m = emit(i->op == OP_UMULH ? X_MULH : X_IMULH, 1, RDX, rb, NOREG);
        m->pmask = BIT(RAX);
        emit(X_DEFREG, 1, RAX, NOREG, NOREG);
        emit(X_MOV, 1, d, RDX, NOREG);
        return;
    }
    case OP_AND:
    case OP_OR:
    case OP_XOR:
        if (fused[v])
            return;
        op = i->op == OP_AND ? X_AND : i->op == OP_OR ? X_OR : X_XOR;
        if (const_of(i->b, &c)) {
            if (i->op == OP_AND && w && c == 0xffffffffL) {
                emit(X_MOVZXD, 0, d, reg_of(i->a), NOREG);
                return;
            }
            if (fits32(c)) {
                m = emit(op + (X_ANDI - X_AND), w, d, reg_of(i->a), NOREG);
                m->imm = c;
                return;
            }
        }
        goto binary;
    case OP_SHL:
    case OP_SHR:
    case OP_SAR:
        if (fused[v])
            return;
        op = i->op == OP_SHL ? X_SHL : i->op == OP_SHR ? X_SHR : X_SAR;
        if (const_of(i->b, &c)) {
            m = emit(op + (X_SHLI - X_SHL), w, d, reg_of(i->a), NOREG);
            m->imm = c & (w ? 63 : 31);
            return;
        } else {
            u32 ra = reg_of(i->a);
            move_to(RCX, i->b);
            m = emit(op, w, d, ra, NOREG);
            m->pmask = BIT(RCX);
        }
        return;
    case OP_NEG: op = X_NEG; goto unary;
    case OP_NOT: op = X_NOT; goto unary;
    case OP_FNEG: op = X_FNEG; goto unary;
    case OP_CLZ: op = X_CLZ; goto unary;
    case OP_CTZ: op = X_BSF; goto unary;
    case OP_BSWAP: op = X_BSWAP; goto unary;
    case OP_ASMOUT:
        return; /* moved out of its register where the assembly is */
    case OP_BYVAL:
    case OP_PIN:
        move_to(d, i->a);
        return;
    case OP_TLS:
        if (f->ins[i->a].op != OP_ADDR)
            die("internal: thread-local address of no symbol");
        m = emit(X_TLS, 1, d, NOREG, NOREG);
        m->sym = f->ins[i->a].a;
        m->imm = (i32)f->ins[i->a].b;
        return;
    case OP_FRAMEADDR:
        m = emit(X_FRAMEADDR, 1, d, NOREG, NOREG);
        m->imm = i->a;
        if (i->a < 2)
            uses_fp = 1;
        return;
    case OP_SEXT8: op = X_MOVSX8; goto ext;
    case OP_SEXT16: op = X_MOVSX16; goto ext;
    case OP_ZEXT8: op = X_MOVZX8; goto ext;
    case OP_ZEXT16: op = X_MOVZX16; goto ext;
    ext:
        if (fused[v] == FUSE_MOV) {
            emit(X_MOV, 1, d, reg_of(i->a), NOREG);
            return;
        }
        goto unary;
    case OP_SEXT32:
        if (fused[v])
            return;
        op = X_MOVSXD;
        goto unary;
    case OP_ZEXT32:
        if (fused[v])
            return;
        op = X_MOVZXD;
        w = 0;
        goto unary;
    case OP_TRUNC:
        if (fused[v])
            return;
        op = X_MOV;
        w = 1;
        goto unary;
    unary:
        emit(op, w, d, reg_of(i->a), NOREG);
        return;
    case OP_SITOF:
    case OP_UITOF:
        m = emit(i->op == OP_SITOF ? X_SITOF : X_UITOF, w, d, reg_of(i->a), NOREG);
        m->cc = f->ins[i->a].ty == IT_I64;
        return;
    case OP_FTOSI:
    case OP_FTOUI:
        m = emit(i->op == OP_FTOSI ? X_FTOSI : X_FTOUI, w, d, reg_of(i->a), NOREG);
        m->cc = f->ins[i->a].ty == IT_F64;
        return;
    case OP_FEXT:
    case OP_FTRUNC:
        emit(X_FCVT, w, d, reg_of(i->a), NOREG);
        return;
    case OP_BITCAST: /* i32 <-> f32, i64 <-> f64: the bits in the other register file */
        m = emit(X_BITCAST, i->ty == IT_I64 || i->ty == IT_F64, d, reg_of(i->a), NOREG);
        m->cc = i->ty == IT_F32 || i->ty == IT_F64;
        return;
    case OP_LOAD:
        if (fused[v] == FUSE_LOAD)
            return;
        addr_of(i->a, &a);
        emit_load(d, &a, (i->flags & 0x80) ? LOAD_SIGNED32 : i->mem);
        return;
    case OP_STORE: {
        Ins *bi = &f->ins[i->b];
        u64 bits = (u64)bi->a | ((u64)bi->b << 32);
        if (fused[i->b] == FUSE_RMW) {
            int alu = bi->op == OP_ADD ? 0 : bi->op == OP_OR ? 1 : bi->op == OP_AND ? 4 : bi->op == OP_SUB ? 5 : 6;
            u32 other = fused[bi->a] == FUSE_LOAD ? bi->b : bi->a;
            if (const_of(other, &c)) {
                addr_of(i->a, &a);
                m = emit(X_RMWI, alu, NOREG, NOREG, a.base);
                m->s2 = a.idx;
                m->cc = i->mem;
                set_mem(m, &a);
                m->imm = (i64)(((u64)c << 32) | (u32)a.off);
            } else {
                u32 val = reg_of(other);
                addr_of(i->a, &a);
                m = emit(X_RMW, alu, NOREG, val, a.base);
                m->s2 = a.idx;
                m->cc = i->mem;
                set_mem(m, &a);
            }
            return;
        }
        if (const_of(i->b, &c) && (i->mem < M_64 || fits32(c))) {
            addr_of(i->a, &a);
            emit_store_imm(c, &a, i->mem);
        } else if (bi->op == OP_FCONST && (i->mem == M_F32 || fits32((i64)bits))) {
            addr_of(i->a, &a);
            emit_store_imm((i64)bits, &a, i->mem == M_F32 ? M_32 : M_64);
        } else {
            u32 val = reg_of(i->b);
            addr_of(i->a, &a);
            emit_store(val, &a, i->mem);
        }
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
        m = emit(X_SLOTADDR, 1, d, NOREG, NOREG);
        if (i->a == 0) {
            m->sym = ARGSLOT;
            m->imm = named_stack;
            uses_args = 1;
        } else {
            m->sym = va_slot; /* where the argument registers were saved */
        }
        return;
    case OP_SELECT: {
        u32 rb, rc;
        int cc;
        if (is_xmm(i->ty) && (f->ins[i->a].op == OP_FLT || f->ins[i->a].op == OP_FGT) && fused[i->a] == FUSE_CMP) {
            /* p < q ? p : q is minss p, q and p > q ? p : q maxss p, q, NaNs and signed zeros included (the second
               operand when unordered or equal); with the arms the other way round, the other one of q, p */
            Ins *c = &f->ins[i->a];
            int lt = c->op == OP_FLT;
            if (i->b == c->a && i->c == c->b) {
                emit(lt ? X_FMIN : X_FMAX, w, d, reg_of(c->a), reg_of(c->b));
                return;
            }
            if (i->b == c->b && i->c == c->a) {
                emit(lt ? X_FMAX : X_FMIN, w, d, reg_of(c->b), reg_of(c->a));
                return;
            }
        }
        rb = reg_of(i->b);
        rc = reg_of(i->c);
        cc = emit_cond(i->a);
        m = emit(is_xmm(i->ty) ? X_FSEL : X_CMOV, w, d, rb, rc);
        m->cc = (u8)cc;
        return;
    }
    default:
        if (is_compare(i->op)) {
            if (fused[v])
                return;
            op = emit_compare(v);
            m = emit(X_SETCC, 0, d, NOREG, NOREG);
            m->cc = (u8)op;
            return;
        }
        die("internal: x86-64 instruction selection: op %d in %s", i->op, f->name);
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
            emit(X_MOV, 1, t, VREG0 + src, NOREG);
        }
        buf_put(&tmp, &t, sizeof t);
    }
    for (p = sb->phis; p; p = f->ins[p].next, n++) {
        u32 src = f->xargs[f->ins[p].a + pred], t;
        memcpy(&t, tmp.p + n * sizeof(u32), sizeof t);
        if (src == p)
            continue;
        if (t != NOREG)
            emit(X_MOV, 1, VREG0 + p, t, NOREG);
        else
            move_to(VREG0 + p, src);
    }
    buf_free(&tmp);
}

static void sel_term(u32 b, u32 next)
{
    Block *bb = &f->blk[b];
    MIns *m;
    Addr ra;
    switch (bb->term) {
    case TERM_JMP:
        sel_phi_moves(b, bb->succ[0]);
        if (bb->succ[0] != next) {
            m = emit(X_JMP, 0, NOREG, NOREG, NOREG);
            m->imm = blk_index[bb->succ[0]];
        }
        break;
    case TERM_BR: {
        u32 t = bb->succ[0], e = bb->succ[1];
        int cc;
        if (fused[bb->arg] == FUSE_BR2) {
            /* all but the last comparison: an and leaves for e where one fails, an or for t where one holds */
            u32 lv[BR2_MAX], nl, n2;
            int isand = f->ins[bb->arg].op == OP_AND;
            for (n2 = 0; n2 < BR2_MAX; n2++)
                lv[n2] = 0;
            nl = br2_leaves(bb->arg, f->ins[bb->arg].op, lv, 0, 1, b);
            for (n2 = 0; n2 + 1 < nl; n2++) {
                cc = emit_cond(lv[n2]);
                m = emit(X_JCC, 0, NOREG, NOREG, NOREG);
                m->cc = (u8)(isand ? cc ^ 1 : cc);
                m->imm = blk_index[isand ? e : t];
            }
            cc = emit_cond(lv[nl - 1]);
        } else {
            cc = emit_cond(bb->arg);
        }
        if (t != next && e != next && f->blk[e].freq > f->blk[t].freq && f->blk[t].freq) {
            /* neither follows: the conditional branch goes to the likelier one */
            cc ^= 1;
            t = e;
            e = bb->succ[0];
        } else if (t == next) {
            cc ^= 1;
            t = e;
            e = next;
        }
        m = emit(X_JCC, 0, NOREG, NOREG, NOREG);
        m->cc = (u8)cc;
        m->imm = blk_index[t];
        if (e != next) {
            m = emit(X_JMP, 0, NOREG, NOREG, NOREG);
            m->imm = blk_index[e];
        }
        break;
    }
    case TERM_RET:
        if (f->has_sret > 1) {
            /* a small record: its two halves, each in the next register of its class */
            u32 size = (f->has_sret & 0x3f) - 1;
            u32 r0 = f->has_sret & 0x40 ? XMM0 : RAX;
            u32 r1 = f->has_sret & 0x80 ? (f->has_sret & 0x40 ? XMM0 + 1 : XMM0) : (f->has_sret & 0x40 ? RAX : RDX);
            slot_addr(&ra, ret_slot, 0);
            emit_load(r0, &ra, r0 >= XMM0 ? M_F64 : M_64);
            if (size > 8) {
                ra.off = 8;
                emit_load(r1, &ra, r1 >= XMM0 ? M_F64 : M_64);
            }
            m = emit(X_RET, 0, NOREG, NOREG, NOREG);
            m->pmask = BIT(r0) | (size > 8 ? BIT(r1) : 0);
        } else if (f->has_sret == 1) {
            /* the address the caller gave for the record goes back to it */
            slot_addr(&ra, sret_slot, 0);
            emit_load(RAX, &ra, M_64);
            m = emit(X_RET, 0, NOREG, NOREG, NOREG);
            m->pmask = BIT(RAX);
        } else if (bb->arg) {
            u32 r = is_xmm(f->ins[bb->arg].ty) ? XMM0 : RAX;
            move_to(r, bb->arg);
            m = emit(X_RET, 0, NOREG, NOREG, NOREG);
            m->pmask = BIT(r);
            m->cc = (u8)(f->ins[bb->arg].ty == IT_V256 || f->ins[bb->arg].ty == IT_V512); /* the result is in ymm0 / zmm0: no vzeroupper */
        } else {
            emit(X_RET, 0, NOREG, NOREG, NOREG);
        }
        break;
    default:
        emit(X_RET, 0, NOREG, NOREG, NOREG);
        break;
    }
}

/* ---- IR preparation ---- */

static void split_critical_edges(void)
{
    u32 b, k, nblk = f->nblk;
    for (b = 1; b < nblk; b++) {
        if (!f->blk[b].phis || f->blk[b].npreds < 2)
            continue;
        for (k = 0; k < f->blk[b].npreds; k++) {
            u32 p = f->blk[b].preds[k], n, s;
            if (f->blk[p].term != TERM_BR)
                continue;
            n = ir_new_block(f);
            f->blk[n].term = TERM_JMP;
            f->blk[n].succ[0] = b;
            f->blk[n].flag = 1;
            if (f->blk[p].freq && f->blk[b].freq)
                f->blk[n].freq = f->blk[p].freq < f->blk[b].freq ? f->blk[p].freq : f->blk[b].freq;
            ir_add_pred(f, n, p);
            f->blk[b].preds[k] = n;
            for (s = 0; s < 2; s++)
                if (f->blk[p].succ[s] == b) {
                    f->blk[p].succ[s] = n;
                    break; /* both ways to b: each gets a block of its own */
                }
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

/* Of the folded sum v of a base and an index: the base, as addr_of takes it. */
static u32 idx_base(u32 v)
{
    u32 x = f->ins[v].a, y = f->ins[v].b;
    if (fused[x] == FUSE_SKIP)
        return y;
    if (fused[y] != FUSE_SKIP && f->ins[x].op != OP_SLOT && f->ins[x].op != OP_ADD &&
        (f->ins[y].op == OP_ADD || f->ins[y].op == OP_SLOT))
        return y;
    return x;
}

/* Is v a shift that an address can have as the scale of its index? */
static int is_scale(u32 v)
{
    u64 sh;
    return f->ins[v].op == OP_SHL && !fused[v] && f->ins[v].ty == IT_I64 && ir_is_const(f, f->ins[v].b, &sh) &&
           sh >= 1 && sh <= 3;
}

/* The comparisons at the leaves of v, a tree of op (OP_AND / OP_OR) each node and leaf used once, in the order they are
   evaluated, into lv; how many (0xffffffff if v is not such a tree). check: only count; else the inner nodes become FUSE_BR2. */
static u32 br2_leaves(u32 v, int op, u32 *lv, u32 n, int check, u32 blk)
{
    Ins *i = &f->ins[v];
    u32 k, o[2];
    if (n == 0xffffffffU)
        return n;
    if (i->op == op && i->ty == IT_I32 && uses[v] == 1 && i->blk == blk && (!fused[v] || fused[v] == FUSE_BR2)) {
        if (!check)
            fused[v] = FUSE_BR2;
        o[0] = i->a;
        o[1] = i->b;
        for (k = 0; k < 2; k++) {
            n = br2_leaves(o[k], op, lv, n, check, blk);
            if (n == 0xffffffffU)
                return n;
        }
        return n;
    }
    /* a value in a register of its own (any value for an or, a truth value for an and): tested where the branch is.
       That is how an || chain's tests that a loop does not change, grouped by opt.c and taken out of it, cost one
       test and branch per iteration. */
    if (!is_compare(i->op) && i->ty == IT_I32 && i->op != OP_CONST && (!fused[v] || fused[v] == FUSE_ALIAS || fused[v] == FUSE_MOV) &&
        (op == OP_OR || ir_is_bool(f, v, 3)) && n < BR2_MAX) {
        lv[n] = v;
        return n + 1;
    }
    /* a leaf used elsewhere only as a condition (a later if on the same test): those set the flags again */
    if (!is_compare(i->op) || vuses[v] != 1 || (fused[v] && fused[v] != FUSE_CMP) || n >= BR2_MAX || i->blk != blk)
        return 0xffffffffU;
    lv[n] = v;
    return n + 1;
}

/* v is computed into a register of its own (not a load folded into its user, an address part, a condition) */
static int reg_value(u32 v)
{
    return !fused[v] || fused[v] == FUSE_ALIAS || fused[v] == FUSE_MOV;
}

/* v is a shift by 1, 2 or 3 of type ty that can be the scaled index of an lea */
static int lea_scale(u32 v, int ty)
{
    u64 sh;
    return f->ins[v].op == OP_SHL && !fused[v] && f->ins[v].ty == ty && ir_is_const(f, f->ins[v].b, &sh) && sh >= 1 && sh <= 3;
}

/* The later division in v's block with v's operands that gives the other half (quotient / remainder) of
   v's, or 0. first: only for a v that is not itself such a partner. */
static u32 div_pair(u32 v, int first)
{
    Ins *i = &f->ins[v];
    int sgn, rem;
    u32 u;
    if (i->op != OP_SDIV && i->op != OP_UDIV && i->op != OP_SREM && i->op != OP_UREM)
        return 0;
    if (fused[v] || (first && !uses[v]))
        return 0;
    sgn = i->op == OP_SDIV || i->op == OP_SREM;
    rem = i->op == OP_SREM || i->op == OP_UREM;
    for (u = i->next; u; u = f->ins[u].next) {
        Ins *x = &f->ins[u];
        if (x->a != i->a || x->b != i->b || x->ty != i->ty || !uses[u] || (fused[u] && fused[u] != FUSE_DIVR))
            continue;
        if (x->op == (sgn ? (rem ? OP_SDIV : OP_SREM) : (rem ? OP_UDIV : OP_UREM)))
            return u;
    }
    return 0;
}

static void prepare(void)
{
    u32 b, v, k, n;
    split_critical_edges();
    fn_avx = f->avx;
    for (v = 1; v < f->nins && !fn_avx; v++)
        if (f->ins[v].blk && (f->ins[v].op == OP_XI || f->ins[v].ty == IT_V256 || f->ins[v].ty == IT_V512))
            fn_avx = 1;
    vlegacy = 0;
    for (v = 1; v < f->nins && !vlegacy; v++) {
        Ins *i = &f->ins[v];
        if (i->blk && !avx_takes(v) &&
            ((i->ty == IT_V128 && i->op != OP_PARAM && i->op != OP_CALL && !(i->op == OP_SELECT && !is_vec(f->ins[i->a].ty))) ||
             i->op == OP_VSTORE || i->op == OP_VSUM))
            vlegacy = 1;
    }
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
        estimate_freq();
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
    vuses = (u32 *)xmalloc(f->nins * sizeof(u32));
    memset(vuses, 0, f->nins * sizeof(u32));
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
                vuses[x]++;
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
                vuses[i->c]++;
                fused[i->c] = FUSE_NOT;
            }
            for (b = 0; b < n; b++) {
                u32 x = ops[b];
                uses[x]++;
                if (b || !sel) {
                    vuses[x]++;
                    fused[x] = FUSE_NOT; /* used as a value, not as a condition */
                }
            }
        }
        if (bb->arg) {
            uses[bb->arg]++;
            if (bb->term != TERM_BR) {
                vuses[bb->arg]++;
                fused[bb->arg] = FUSE_NOT;
            }
        }
    }
    /* a comparison used only as the condition of branches and selects is
       never made into a value: each of them sets the flags again */
    for (v = 1; v < f->nins; v++) {
        int op = f->ins[v].op, cond = fused[v] != FUSE_NOT && uses[v];
        fused[v] = (u8)(cond && is_compare(op) ? FUSE_CMP : 0);
        /* a truncation leaves the low half where it is; so does extending
           what a 32-bit operation produced, since that cleared the upper half */
        if (uses[v] && f->ins[v].blk && (op == OP_TRUNC || (op == OP_ZEXT32 && clean32(f->ins[f->ins[v].a].op))))
            fused[v] = FUSE_ALIAS;
    }
    /* -ffp-contract with FMA: a product used only by a sum or difference is rounded with it, in one vfmadd */
    free(fma_sum);
    fma_sum = 0;
    if (opt_fp_contract && opt_x64_fma) {
        fma_sum = (u8 *)xmalloc(f->nins);
        memset(fma_sum, 0, f->nins);
    }
    for (k = 0; opt_fp_contract && opt_x64_fma && k < nlayout; k++)
        for (v = f->blk[layout[k]].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 side;
            if ((i->op != OP_FADD && i->op != OP_FSUB) || (i->ty != IT_F32 && i->ty != IT_F64))
                continue;
            for (side = 0; side < 2; side++) {
                u32 u = side ? i->b : i->a;
                Ins *p = &f->ins[u];
                if (p->op == OP_FMUL && p->ty == i->ty && uses[u] == 1 && !fused[u] && p->blk && i->a != i->b) {
                    fused[u] = FUSE_FMA;
                    fma_sum[v] = 1;
                    break;
                }
            }
        }
    /* cache_reg doubles as a count of each value's uses as a memory address */
    memset(cache_reg, 0, f->nins * sizeof(u32));
    for (k = 0; k < nlayout; k++)
        for (v = f->blk[layout[k]].first; v; v = f->ins[v].next)
            if (f->ins[v].op == OP_LOAD || f->ins[v].op == OP_STORE)
                cache_reg[f->ins[v].a]++;
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        i64 c;
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
                if (a->op != OP_ADD || fused[i->a] || uses[i->a] != cache_reg[i->a])
                    continue; /* also used as a value */
                if (const_of(a->b, &c)) {
                    /* a constant offset folds into every access that uses it */
                    if (c > -0x40000000L && c < 0x40000000L)
                        fused[i->a] = FUSE_OFF;
                    continue;
                }
                if (!(f->ins[a->a].op == OP_SLOT && f->ins[a->b].op == OP_SLOT) && f->ins[a->a].op != OP_CONST) {
                    /* base + index, the index optionally scaled */
                    u32 y = a->b;
                    fused[i->a] = FUSE_IDX;
                    if (a->a == a->b)
                        continue;
                    if (!is_scale(y))
                        y = a->a;
                    if (is_scale(y))
                        cache_blk[y]++; /* here: in how many addresses the shift is the scale */
                }
            }
        }
    }
    /* a shift that is nothing but the scale of indexes is not computed */
    for (v = 1; v < f->nins; v++) {
        if (cache_blk[v] && cache_blk[v] == uses[v])
            fused[v] = FUSE_SKIP;
        cache_blk[v] = 0;
    }
    /* nor is "p + constant" that is nothing but the base of such addresses (and addresses itself): the
       constant goes into each of them, and p is what the loop keeps in a register */
    for (v = 1; v < f->nins; v++)
        if (fused[v] == FUSE_IDX) {
            u32 x = idx_base(v);
            if (f->ins[x].op == OP_ADD && !fused[x])
                cache_blk[x]++;
        }
    for (v = 1; v < f->nins; v++) {
        i64 kc;
        if (cache_blk[v] && f->ins[v].op == OP_ADD && !fused[v] && cache_blk[v] + cache_reg[v] == uses[v] &&
            const_of(f->ins[v].b, &kc) && kc > -0x40000000L && kc < 0x40000000L && f->ins[f->ins[v].a].op != OP_CONST)
            fused[v] = FUSE_OFF;
        cache_blk[v] = 0;
    }
    /* a branch on an and (or an or) of comparisons, each used there only: a branch per comparison, as
       && (||) would be, instead of setcc / movzx / and of each and a test of the result */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        u32 x = bb->arg, lv[BR2_MAX], nl;
        if (bb->term != TERM_BR || (f->ins[x].op != OP_AND && f->ins[x].op != OP_OR) || uses[x] != 1 || fused[x] ||
            f->ins[x].ty != IT_I32)
            continue;
        nl = br2_leaves(x, f->ins[x].op, lv, 0, 1, layout[k]);
        if (nl == 0xffffffffU || nl < 2)
            continue;
        {
            /* the comparisons are made at the end of the block: nothing after the first of them may write memory
               (a load folded into one would move past it) */
            u32 u, first = 0, bad = 0;
            for (u = bb->first; u; u = f->ins[u].next) {
                int op = f->ins[u].op;
                for (n = 0; n < nl && !first; n++)
                    if (lv[n] == u)
                        first = u;
                if (first && (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_VSTORE ||
                              op == OP_XI || op == OP_ASMOUT))
                    bad = 1;
            }
            if (bad)
                continue;
        }
        br2_leaves(x, f->ins[x].op, lv, 0, 0, layout[k]);
        for (n = 0; n < nl; n++)
            if (is_compare(f->ins[lv[n]].op))
                fused[lv[n]] = FUSE_CMP;
    }
    /* an and used once, to branch on whether it is zero, or by comparisons with zero that are
       themselves only conditions */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        u32 x = bb->arg;
        i64 c;
        if (bb->term == TERM_BR && f->ins[x].op == OP_AND && uses[x] == 1 && !fused[x])
            fused[x] = FUSE_TEST;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            if ((i->op == OP_EQ || i->op == OP_NE) && fused[v] == FUSE_CMP && f->ins[i->a].op == OP_AND &&
                uses[i->a] == 1 && !fused[i->a] && const_of(i->b, &c) && c == 0)
                fused[i->a] = FUSE_TEST;
        }
    }
    /* load, operation, store to the same place: one instruction on memory, where the store is */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v], *o, *l;
            u32 u, lv, other;
            i64 c;
            if (i->op != OP_STORE || uses[i->b] != 1 || fused[i->b] || mem_bytes[i->mem] == 2 || i->mem > M_64)
                continue;
            o = &f->ins[i->b];
            if (o->op != OP_ADD && o->op != OP_SUB && o->op != OP_AND && o->op != OP_OR && o->op != OP_XOR)
                continue;
            if ((o->ty == IT_I64) != (i->mem == M_64) || o->a == o->b)
                continue;
            lv = o->a;
            l = &f->ins[lv];
            if (!(l->op == OP_LOAD && l->a == i->a && uses[lv] == 1) && o->op != OP_SUB) {
                lv = o->b;
                l = &f->ins[lv];
            }
            if (l->op != OP_LOAD || l->a != i->a || uses[lv] != 1 || fused[lv] || (l->flags & 0x80) ||
                mem_bytes[l->mem] != mem_bytes[i->mem] || l->mem > M_64)
                continue;
            other = lv == o->a ? o->b : o->a;
            if (const_of(other, &c) && !fits32(c))
                continue;
            /* the load is earlier in this block, and nothing between writes memory */
            for (u = bb->first; u && u != lv && u != v; u = f->ins[u].next)
                ;
            if (u != lv)
                continue;
            for (u = l->next; u != v; u = f->ins[u].next) {
                int op = f->ins[u].op;
                if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_VSTORE)
                    break;
            }
            if (u != v)
                continue;
            fused[lv] = FUSE_LOAD;
            fused[i->b] = FUSE_RMW;
        }
    }
    /* A load used once, by an operation after it in its block that can take the operand from memory,
       with nothing between that may write memory, is left to that operation. A comparison here is
       one made where the block ends. */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 u, usr = 0;
            int full, ok = 0;
            i64 c;
            if (i->op != OP_LOAD || uses[v] != 1 || fused[v] || (i->flags & 0x80))
                continue;
            full = (i->mem == M_32 && i->ty == IT_I32) || (i->mem == M_64 && i->ty == IT_I64) ||
                   (i->mem == M_F32 && i->ty == IT_F32) || (i->mem == M_F64 && i->ty == IT_F64);
            for (u = i->next; u; u = f->ins[u].next) {
                Ins *x = &f->ins[u];
                int op = x->op;
                if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_VSTORE)
                    break;
                if (usr || (x->a != v && x->b != v) || x->a == x->b)
                    continue;
                if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_AND || op == OP_OR || op == OP_XOR ||
                    (op >= OP_FADD && op <= OP_FDIV)) {
                    int commut = op != OP_SUB && op != OP_FSUB && op != OP_FDIV;
                    if (full && !fused[u] && !(fma_sum && fma_sum[u]) && x->ty == i->ty && (x->b == v || commut) &&
                        fused[x->a == v ? x->b : x->a] != FUSE_LOAD && /* one memory operand each */
                        !const_of(x->a == v ? x->b : x->a, &c))
                        ok = 1;
                    break;
                }
                if (op >= OP_EQ && op <= OP_UGE) {
                    if (fused[u] != FUSE_CMP || uses[u] != 1 || bb->arg != u || bb->term != TERM_BR ||
                        fused[x->a == v ? x->b : x->a] == FUSE_LOAD)
                        break;
                    if (full && !is_fp(i->ty)) {
                        if (!const_of(x->a, &c) && (!const_of(x->b, &c) || (fits32(c) && x->a == v)))
                            usr = u;
                    } else if ((op == OP_EQ || op == OP_NE) && x->a == v && const_of(x->b, &c) &&
                               ((i->mem == M_8U && c >= 0 && c <= 255) || (i->mem == M_8S && c >= -128 && c <= 127))) {
                        usr = u;
                    }
                    if (!usr)
                        break;
                }
            }
            if (!u && usr)
                ok = 1;
            if (!u && !usr && bb->arg == v && bb->term == TERM_BR && !is_fp(i->ty) &&
                (i->mem == M_8S || i->mem == M_8U || full))
                ok = 1;
            if (ok)
                fused[v] = FUSE_LOAD;
        }
    }
    /* a whole vector loaded for one vector instruction later in its block, nothing between that may write
       memory: that instruction's memory operand */
    for (k = 0; k < nlayout; k++) {
        Block *bb = &f->blk[layout[k]];
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 u;
            if (i->op != OP_LOAD || i->mem < M_V128 || uses[v] != 1 || fused[v])
                continue;
            for (u = i->next; u; u = f->ins[u].next) {
                Ins *x = &f->ins[u];
                int op = x->op;
                if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_VSTORE ||
                    (op == OP_XI && (x->flags & XI_EFFECT)))
                    break;
                if (vec_mem_operand(u) == v) {
                    fused[v] = FUSE_LOAD;
                    break;
                }
                if (refers(u, v))
                    break;
            }
        }
    }
    /* a + (b << 1..3), the shift used nowhere else, is one lea (x * 3, 5, 9 come out of the strength reduction as
       that); last, so that only an add no other fusion took (a read-modify-write store, an address) is looked at */
    for (k = 0; k < nlayout; k++)
        for (v = f->blk[layout[k]].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            i64 c;
            u32 y;
            if (i->op != OP_ADD || fused[v] || (i->ty != IT_I32 && i->ty != IT_I64) || const_of(i->b, &c))
                continue;
            y = lea_scale(i->b, i->ty) && uses[i->b] == 1 ? i->b : lea_scale(i->a, i->ty) && uses[i->a] == 1 ? i->a : 0;
            if (y && reg_value(y == i->b ? i->a : i->b) && reg_value(f->ins[y].a))
                fused[y] = FUSE_SKIP;
        }
    /* x / y and x % y in one block: one div gives both (i / sy, i % sy of the index -> coordinates idiom) */
    for (k = 0; k < nlayout; k++)
        for (v = f->blk[layout[k]].first; v; v = f->ins[v].next)
            if (div_pair(v, 1))
                fused[div_pair(v, 0)] = FUSE_DIVR;
    memset(cache_reg, 0, f->nins * sizeof(u32));
}

static void select_all(void)
{
    u32 k, v, n = 0, np = f->nparams;
    Addr a;
    MIns *m;

    mf.n = 0;
    mf.nblk = 0;
    mf.nvreg = 0;
    outargs = 0;
    has_calls = uses_args = uses_fp = uses_alloca = 0;
    asm_saved = 0;
    free(alink);
    alink = 0;
    /* one virtual register per IR value */
    if (mf.capvreg < f->nins) {
        mf.capvreg = f->nins * 2;
        mf.vclass = (u8 *)xrealloc(mf.vclass, mf.capvreg);
    }
    for (v = 0; v < f->nins; v++)
        mf.vclass[v] = (u8)cls_of(f->ins[v].ty);
    mf.nvreg = f->nins;
    if (mf.capblk < nlayout) {
        mf.capblk = nlayout * 2;
        mf.blk = (MBlock *)xrealloc(mf.blk, mf.capblk * sizeof(MBlock));
    }
    mf.nblk = nlayout;

    args_reserve(np + 1);
    for (k = 0; k < np; k++) {
        int ty = f->param_ty[k];
        a_size[k] = 8 | (ty & PT_A16 ? 0x80000000U : 0);
        a_kind[k] = (u8)(ty & (PT_BLK | PT_PAD) ? AK_BLK : is_xmm(ty) ? AK_FP : AK_INT);
    }
    ret_slot = sret_slot = va_slot = NOSLOT;
    if (f->has_sret > 1) {
        ret_slot = ir_new_slot(f, 16, 8);
        a_kind[0] = AK_SKIP;
    } else if (f->has_sret == 1) {
        sret_slot = ir_new_slot(f, 8, 8);
    }
    named_stack = abi_assign(np, 0);
    /* abi_assign rounds up to 16; the unnamed arguments follow the last named one */
    {
        u32 top = 0;
        for (k = 0; k < np; k++)
            if (a_loc[k] != LOC_NONE && (a_loc[k] & LOC_STACK) && (a_loc[k] & ~LOC_STACK) + 8 > top)
                top = (a_loc[k] & ~LOC_STACK) + 8;
        named_stack = top;
    }
    if (f->variadic) {
        for (v = 1; v < f->nins; v++)
            if (f->ins[v].op == OP_VASTART && f->ins[v].blk) {
                va_slot = ir_new_slot(f, 176, 16);
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
            if (va_slot != NOSLOT) {
                m = emit(X_VASAVE, 0, NOREG, NOREG, NOREG);
                m->sym = va_slot;
                m->pmask = BIT(RAX) | BIT(RDI) | BIT(RSI) | BIT(RDX) | BIT(RCX) | BIT(R8) | BIT(R9) | ((u64)0xff << XMM0);
            }
            if (sret_slot != NOSLOT) {
                slot_addr(&a, sret_slot, 0);
                emit_store(RDI, &a, M_64);
            }
            /* incoming arguments: registers first, then stack */
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op != OP_PARAM)
                    continue;
                if (i->a == 0 && f->has_sret > 1) {
                    m = emit(X_SLOTADDR, 1, VREG0 + v, NOREG, NOREG);
                    m->sym = ret_slot;
                } else if (!(a_loc[i->a] & LOC_STACK)) {
                    emit(X_MOV, 1, VREG0 + v, a_loc[i->a], NOREG);
                }
            }
            for (v = bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if (i->op != OP_PARAM || (i->a == 0 && f->has_sret > 1) || !(a_loc[i->a] & LOC_STACK))
                    continue;
                uses_args = 1;
                if (f->param_ty[i->a] & PT_BLK) {
                    m = emit(X_SLOTADDR, 1, VREG0 + v, NOREG, NOREG);
                    m->sym = ARGSLOT;
                    m->imm = a_loc[i->a] & ~LOC_STACK;
                } else {
                    slot_addr(&a, ARGSLOT, a_loc[i->a] & ~LOC_STACK);
                    emit_load(VREG0 + v, &a, ty_mem(i->ty));
                }
            }
        }
        for (v = bb->first; v; v = f->ins[v].next) {
            if (f->ins[v].loc)
                sel_loc = f->ins[v].loc;
            sel_ins(v);
        }
        sel_term(b, k + 1 < nlayout ? layout[k + 1] : 0);
        mb->n = mf.n - mb->first;
        mb->nsucc = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        for (n = 0; n < mb->nsucc; n++)
            mb->succ[n] = blk_index[bb->succ[n]];
    }
}

/* ---- encoding ---- */

/* a jump to a block: written with a 32-bit displacement, shortened afterwards (relax) */
enum { FK_JMP, FK_JCC, FK_JCC_PAIR /* one of two for one condition */, FK_JCC_JP /* behind a jp over it */ };
typedef struct Fixup {
    u32 off;    /* of the 32-bit displacement */
    u32 target; /* layout index */
    u8 kind;
    u8 state;   /* 0 as written, 1 short, 2 gone */
} Fixup;

static Buf *text;
static Fixup *fixups;
static u32 nfixups, capfixups;
static u32 spill_base, frame_size;
static u64 saved_regs;
static int has_frame;
/* The frame addressed from rsp (no push rbp / mov rbp, rsp / leave): rbp is one more callee-saved register for the
   allocator. rsp then stays where the prologue left it, frame_size below where rbp would have been, so every
   [rbp + d] of the frame becomes [rsp + frame_size + d] and the layout is unchanged. Not with alloca / VLAs or
   __builtin_frame_address (rsp moves, or rbp is asked for), nor around inline assembly. MCC_NOFPO: always rbp. */
static int fpo;
static int opt_nofpo = -1;
/* -g: line table rows (offsets in the section, locations) and frame events of the function being encoded */
static u32 *row_off, *row_loc, nrow, caprow;
static DwCfi *cfi;
static u32 ncfi, capcfi;

static u32 *moff, capmoff; /* -g: where each machine instruction starts (moff[mf.n]: where the last ends) */

static void cfi_event(int op, u32 reg, u32 n)
{
    if (!opt_debug)
        return;
    if (ncfi == capcfi) {
        capcfi = capcfi ? capcfi * 2 : 16;
        cfi = (DwCfi *)xrealloc(cfi, capcfi * sizeof(DwCfi));
    }
    cfi[ncfi].off = (u32)text->len;
    cfi[ncfi].op = (u8)op;
    cfi[ncfi].reg = (u8)reg;
    cfi[ncfi].n = (u8)n;
    cfi[ncfi].v = n;
    ncfi++;
}

static void row_add(u32 loc)
{
    if (nrow == caprow) {
        caprow = caprow ? caprow * 2 : 256;
        row_off = (u32 *)xrealloc(row_off, caprow * sizeof(u32));
        row_loc = (u32 *)xrealloc(row_loc, caprow * sizeof(u32));
    }
    row_off[nrow] = (u32)text->len;
    row_loc[nrow] = loc;
    nrow++;
}


typedef struct EA {
    u32 base, idx; /* registers or NOREG */
    u32 scale;     /* log2 */
    i64 disp;
    u32 sym;       /* NOSLOT, or the symbol this is relative to (and no registers) */
    int force32;   /* a 32-bit displacement whatever its value */
    int fs;        /* in the thread's segment */
} EA;

static void b1(u32 v)
{
    buf_u8(text, v & 0xff);
}

static void b4(u32 v)
{
    buf_u32(text, v);
}

static void op_prefix(u32 op, int w, u32 reg, u32 idx, u32 base)
{
    u32 rex = 0;
    if (op >> 16 & 0xff)
        b1(op >> 16);
    if (w)
        rex |= 8;
    if (reg != NOREG && (reg & 8))
        rex |= 4;
    if (idx != NOREG && (idx & 8))
        rex |= 2;
    if (base != NOREG && (base & 8))
        rex |= 1;
    if ((op & O_B8R) && reg >= 4 && reg < 8)
        rex |= 0x40;
    if ((op & O_B8M) && base >= 4 && base < 8)
        rex |= 0x40;
    if (rex)
        b1(0x40 | rex);
    if (op & O_0F)
        b1(0x0f);
    if (op & O_38)
        b1(0x38);
    if (op & O_3A)
        b1(0x3a);
    b1(op);
}

/* register direct; for a /digit instruction reg is the digit */
static void op_rr(u32 op, int w, u32 reg, u32 rm)
{
    op_prefix(op, w, reg, NOREG, rm);
    b1(0xc0 | (reg & 7) << 3 | (rm & 7));
}

static void mem_tail(u32 reg, const EA *ea, u32 immbytes);

/* immbytes: how many bytes of immediate follow the operand */
static void op_mem(u32 op, int w, u32 reg, const EA *ea, u32 immbytes)
{
    if (ea->fs)
        b1(0x64);
    if (ea->sym != NOSLOT)
        op_prefix(op, w, reg, NOREG, NOREG);
    else
        op_prefix(op, w, reg, ea->idx, ea->base);
    mem_tail(reg, ea, immbytes);
}

/* The VEX prefix and opcode of enc (x86i.h); vvvv NOREG: not used. reg, idx and base give the R, X and B bits. */
static void vex_prefix(u32 enc, u32 reg, u32 vvvv, u32 idx, u32 base)
{
    u32 R = reg != NOREG && (reg & 8), X = idx != NOREG && (idx & 8), B = base != NOREG && (base & 8);
    u32 vv = vvvv == NOREG ? 0 : vvvv & 15, last = (u32)XE_L(enc) << 2 | (u32)XE_PP(enc);
    if (XE_MAP(enc) == 1 && !XE_W(enc) && !X && !B) {
        b1(0xc5);
        b1((R ? 0 : 0x80) | (~vv & 15) << 3 | last);
    } else {
        b1(0xc4);
        b1((R ? 0 : 0x80) | (X ? 0 : 0x40) | (B ? 0 : 0x20) | (u32)XE_MAP(enc));
        b1((u32)XE_W(enc) << 7 | (~vv & 15) << 3 | last);
    }
    b1(XE_OPC(enc));
}

/* EVEX (enc with Z): 512 bits, no masking; registers below 16 only (R', V', the X of a register r/m stay clear) */
static void evex_prefix(u32 enc, u32 reg, u32 vvvv, u32 idx, u32 base, u32 aaa)
{
    u32 R = reg != NOREG && (reg & 8), X = idx != NOREG && (idx & 8), B = base != NOREG && (base & 8);
    u32 vv = vvvv == NOREG ? 0 : vvvv & 15;
    b1(0x62);
    b1((R ? 0 : 0x80) | (X ? 0 : 0x40) | (B ? 0 : 0x20) | 0x10 | (u32)XE_MAP(enc));
    b1((u32)XE_W(enc) << 7 | (~vv & 15) << 3 | 4 | (u32)XE_PP(enc));
    b1(2 << 5 | 8 | aaa);
    b1(XE_OPC(enc));
}

static void vex_rr(u32 enc, u32 reg, u32 vvvv, u32 rm)
{
    if (XE_Z(enc)) {
        evex_prefix(enc, reg, vvvv, NOREG, rm, (u32)XE_K1(enc));
        b1(0xc0 | (reg & 7) << 3 | (rm & 7));
        return;
    }
    vex_prefix(enc, reg, vvvv, NOREG, rm);
    b1(0xc0 | (reg & 7) << 3 | (rm & 7));
}

static void vex_mem(u32 enc, u32 reg, u32 vvvv, const EA *ea, u32 immbytes)
{
    if (ea->fs)
        b1(0x64);
    if (XE_Z(enc)) {
        /* EVEX scales an 8-bit displacement by the operand size: always 32 bits here */
        EA e = *ea;
        if (e.sym == NOSLOT && e.base != NOREG && (e.disp != 0 || (e.base & 7) == 5))
            e.force32 = 1;
        if (e.sym != NOSLOT)
            evex_prefix(enc, reg, vvvv, NOREG, NOREG, (u32)XE_K1(enc));
        else
            evex_prefix(enc, reg, vvvv, e.idx, e.base, (u32)XE_K1(enc));
        mem_tail(reg, &e, immbytes);
        return;
    }
    if (ea->sym != NOSLOT)
        vex_prefix(enc, reg, vvvv, NOREG, NOREG);
    else
        vex_prefix(enc, reg, vvvv, ea->idx, ea->base);
    mem_tail(reg, ea, immbytes);
}

/* the ModRM byte and what follows it */
static void mem_tail(u32 reg, const EA *ea, u32 immbytes)
{
    u32 r = (reg & 7) << 3, mod;
    if (ea->sym != NOSLOT) {
        b1(r | 5);
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_X64_PC32, ea->sym, ea->disp - 4 - (i64)immbytes);
        b4(0);
        return;
    }
    if (ea->base == NOREG) { /* [idx * scale + disp32] (SIB base 101 with mod 00: no base); without an index, [disp32] */
        b1(r | 4);
        b1(ea->idx == NOREG ? 0x25 : ea->scale << 6 | (ea->idx & 7) << 3 | 5);
        b4((u32)ea->disp);
        return;
    }
    mod = ea->force32 ? 2 : ea->disp == 0 && (ea->base & 7) != 5 ? 0 : ea->disp >= -128 && ea->disp <= 127 ? 1 : 2;
    if (ea->idx != NOREG || (ea->base & 7) == 4) {
        b1(mod << 6 | r | 4);
        b1(ea->scale << 6 | (ea->idx == NOREG ? 4 : ea->idx & 7) << 3 | (ea->base & 7));
    } else {
        b1(mod << 6 | r | (ea->base & 7));
    }
    if (mod == 1)
        b1((u32)ea->disp);
    else if (mod == 2)
        b4((u32)ea->disp);
}

static void ea_base(EA *ea, u32 base, i64 disp)
{
    ea->base = base;
    ea->idx = NOREG;
    ea->scale = 0;
    ea->disp = disp;
    ea->sym = NOSLOT;
    ea->force32 = ea->fs = 0;
}

static void ea_frame(EA *ea, i64 disp)
{
    if (fpo)
        ea_base(ea, RSP, disp + (i64)frame_size);
    else
        ea_base(ea, RBP, disp);
}

static u32 *spill_off; /* per spill slot: its distance below rbp */

/* [rbp + disp] of the frame, or the same place from rsp */
static void ea_frame(EA *ea, i64 disp);

static void ea_spill(EA *ea, u32 loc)
{
    ea_frame(ea, -(i64)spill_off[loc - SPILL0]);
}

/* the width of the vector in register r, 16 or 32 bytes; 0 for anything else */
static int vw_of(u32 r)
{
    u8 c;
    if (r == NOREG || r < VREG0)
        return 0;
    c = mf.vclass[r - VREG0];
    return (c & 32) ? 64 : (c & 8) ? 32 : c & VC_V128 ? 16 : 0;
}

static int is_xreg(u32 r)
{
    return r != NOREG && (r < VREG0 ? r >= XMM0 : (mf.vclass[r - VREG0] & 1));
}

static u32 loc_of(u32 r)
{
    return r == NOREG || r < VREG0 ? r : mf.loc[r - VREG0];
}

static int is_spill(u32 l)
{
    return l != NOREG && l >= SPILL0;
}

/* whole registers */
static void mov_rr(u32 d, u32 s)
{
    if (d == s)
        return;
    if (d >= XMM0)
        op_rr(O_0F | 0x28, 0, d, s); /* movaps */
    else
        op_rr(0x89, 1, s, d);
}

static void load_reg(u32 d, const EA *ea)
{
    if (d >= XMM0)
        op_mem(O_F2 | O_0F | 0x10, 0, d, ea, 0); /* movsd */
    else
        op_mem(0x8b, 1, d, ea, 0);
}

static void store_reg(u32 s, const EA *ea)
{
    if (s >= XMM0)
        op_mem(O_F2 | O_0F | 0x11, 0, s, ea, 0);
    else
        op_mem(0x89, 1, s, ea, 0);
}

/* the same for a register holding a vector of vw bytes (0: as above) */
static void load_regw(u32 d, const EA *ea, int vw)
{
    if (vw)
        vex_mem(0x110 | (vw == 64 ? VE_Z : vw == 32 ? 0x2000u : 0), d, NOREG, ea, 0); /* vmovups */
    else
        load_reg(d, ea);
}

static void store_regw(u32 s, const EA *ea, int vw)
{
    if (vw)
        vex_mem(0x111 | (vw == 64 ? VE_Z : vw == 32 ? 0x2000u : 0), s, NOREG, ea, 0);
    else
        store_reg(s, ea);
}

static void vmov_rr(u32 d, u32 s, int vw)
{
    if (d != s)
        vex_rr(0x128 | (vw == 64 ? VE_Z : vw == 32 ? 0x2000u : 0), d, NOREG, s); /* vmovaps */
}

/* The spill slot the instruction before stored last, from which register, and where the current instruction's code
   begins: a use of that slot before this instruction has emitted anything finds the value still in the register
   (an instruction whose result is spilled, then read by the next one: the store stays, the reload goes). */
static u32 sp_loc = NOREG, sp_reg, sp_end, ins_start;

static int spill_cached(u32 l, u32 scratch)
{
    return l == sp_loc && scratch == sp_reg && sp_end == ins_start && text->len == ins_start;
}

/* The register that holds r: its own, or the scratch register after a load from its spill slot. */
static u32 use(u32 r, u32 scratch)
{
    u32 l = loc_of(r);
    EA ea;
    if (!is_spill(l))
        return l;
    if (spill_cached(l, scratch))
        return scratch;
    ea_spill(&ea, l);
    load_regw(scratch, &ea, vw_of(r));
    return scratch;
}

/* use() with a scratch register of r's own kind: k = 0, 1, 2 for the first, second, third source */
static u32 xuse(u32 r, int k)
{
    if (r == NOREG)
        return NOREG;
    if (is_xreg(r))
        return use(r, k == 0 ? FSCR0 : k == 1 ? FSCR1 : FSCR2);
    if (k == 2)
        die("internal: x86-64: three general register sources");
    return use(r, k == 0 ? SCR0 : SCR1);
}

/* The register to compute r in, and what follows the computation. */
static u32 dst(u32 r, u32 scratch)
{
    u32 l = loc_of(r);
    return l == NOREG || is_spill(l) ? scratch : l;
}

static void fin(u32 r, u32 reg)
{
    u32 l = loc_of(r);
    EA ea;
    if (is_spill(l)) {
        ea_spill(&ea, l);
        store_regw(reg, &ea, vw_of(r));
        sp_loc = l;
        sp_reg = reg;
        sp_end = (u32)text->len;
    }
}

static u32 xdst(u32 r)
{
    return dst(r, is_xreg(r) ? FSCR0 : SCR0);
}

/* op reg, r/m where the second operand is the location l */
static void op_loc(u32 op, int w, u32 reg, u32 l)
{
    EA ea;
    if (is_spill(l)) {
        ea_spill(&ea, l);
        op_mem(op, w, reg, &ea, 0);
    } else {
        op_rr(op, w, reg, l);
    }
}

/* dr = the vector of vw bytes at location l */
static void mov_locw(u32 dr, u32 l, int vw)
{
    EA ea;
    if (!vw) {
        if (is_spill(l)) {
            ea_spill(&ea, l);
            load_reg(dr, &ea);
        } else {
            mov_rr(dr, l);
        }
    } else if (is_spill(l)) {
        ea_spill(&ea, l);
        load_regw(dr, &ea, vw);
    } else {
        vmov_rr(dr, l, vw);
    }
}

/* dr = the value at location l, all of it */
static void mov_loc(u32 dr, u32 l)
{
    EA ea;
    if (is_spill(l)) {
        ea_spill(&ea, l);
        load_reg(dr, &ea);
    } else {
        mov_rr(dr, l);
    }
}

static void mov_ri(u32 d, u64 v, int w)
{
    if (!w)
        v &= 0xffffffffUL;
    if (v == 0) {
        op_rr(0x31, 0, d, d); /* xor: the flags are never live where a constant is made */
    } else if (v <= 0xffffffffUL) {
        op_prefix(0xb8 | (d & 7), 0, NOREG, NOREG, d);
        b4((u32)v);
    } else if ((i64)v >= -0x7fffffffL - 1 && (i64)v < 0) {
        op_rr(0xc7, 1, 0, d);
        b4((u32)v);
    } else {
        op_prefix(0xb8 | (d & 7), 1, NOREG, NOREG, d);
        b4((u32)v);
        b4((u32)(v >> 32));
    }
}

/* op r/m, imm with the 8-bit form where it fits; alu is the /digit */
static void alu_ri(u32 alu, int w, u32 d, i64 imm)
{
    if (imm >= -128 && imm <= 127) {
        op_rr(0x83, w, alu, d);
        b1((u32)imm);
    } else {
        op_rr(0x81, w, alu, d);
        b4((u32)imm);
    }
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
    fixups[nfixups].state = 0;
    nfixups++;
    b4(0);
}

/* short forward jumps inside one instruction's code */
static u32 jf_at[4], jf_n;

static void jf(u32 opcode)
{
    b1(opcode);
    jf_at[jf_n++] = (u32)text->len;
    b1(0);
}

/* jump ahead if cc holds */
static void jf_cc(int cc)
{
    if (cc == CC_FNE) {
        jf(0x70 | CC_NE);
        jf(0x70 | CC_P);
    } else if (cc == CC_FEQ) {
        b1(0x70 | CC_P); /* over the next jump */
        b1(2);
        jf(0x70 | CC_E);
    } else {
        jf(0x70 | (u32)cc);
    }
}

static void jf_here(void)
{
    while (jf_n) {
        u32 at = jf_at[--jf_n];
        text->p[at] = (u8)(text->len - at - 1);
    }
}

static void prologue(void)
{
    u32 k, n = 0;
    EA ea;
    if (!has_frame)
        return;
    if (fpo) {
        /* the 8 bytes where rbp would have been pushed stay: the same layout and alignment */
        alu_ri(5, 1, RSP, (i64)frame_size + 8);
        cfi_event(CFI_SUBSP, 0, frame_size + 16);
    } else {
        b1(0x55);              /* push rbp */
        cfi_event(CFI_PUSHFP, 0, 0);
        op_rr(0x89, 1, RSP, RBP); /* mov rbp, rsp */
        cfi_event(CFI_SETFP, 0, 0);
        if (frame_size)
            alu_ri(5, 1, RSP, frame_size);
    }
    for (k = 0; k < 16; k++)
        if (saved_regs >> k & 1) {
            ea_frame(&ea, -(i64)(8 * ++n));
            store_reg(k, &ea);
            cfi_event(CFI_SAVE, k, 2 + n); /* rbp - 8n is the frame's address - 16 - 8n */
        }
}

static void epilogue(void)
{
    u32 k, n = 0;
    EA ea;
    if (has_frame) {
        for (k = 0; k < 16; k++)
            if (saved_regs >> k & 1) {
                ea_frame(&ea, -(i64)(8 * ++n));
                load_reg(k, &ea);
            }
        if (fpo)
            alu_ri(0, 1, RSP, (i64)frame_size + 8); /* add rsp */
        else
            b1(0xc9); /* leave */
        cfi_event(CFI_EPI, 0, 0);
    }
    b1(0xc3);
    if (has_frame)
        cfi_event(CFI_RESTORE, 0, 0); /* code after the return is in the frame again */
}

/* The memory operand of a load or store whose base and index are s1 and s2 of the instruction given
   (already in registers b and x, or NOREG). */
static void mem_ea(const MIns *i, u32 b, u32 x, i64 disp, EA *ea)
{
    ea_base(ea, b, disp);
    ea->idx = x;
    ea->scale = XF_SCALE(i->flags);
    if (i->flags & XF_SLOT) {
        ea->base = fpo ? RSP : RBP;
        if (fpo)
            ea->disp += frame_size;
        if (i->sym == ARGSLOT)
            ea->disp += 16;
        else
            ea->disp += f->slots[i->sym].off;
    } else if (i->flags & XF_SYM) {
        ea->base = NOREG;
        ea->sym = i->sym;
    }
}

/* d = a op b for a two-address instruction "op r, r/m"; sub: it is a subtraction, else the operands commute */
static void bin_int(MIns *i, u32 opc, int sub)
{
    u32 dl = loc_of(i->d), al = loc_of(i->s0), bl = loc_of(i->s1), dr = dst(i->d, SCR0);
    if (bl == dr && al != dr) {
        if (sub) {
            op_rr(0xf7, i->w, 3, dr); /* neg, then add the other */
            op_loc(0x03, i->w, dr, al);
            fin(i->d, dr);
            return;
        }
        bl = al;
        al = dr;
    }
    if (al != dr)
        mov_loc(dr, al);
    op_loc(opc, i->w, dr, bl);
    (void)dl;
    fin(i->d, dr);
}

static void bin_fp(MIns *i, u32 opc, int commut)
{
    u32 al = loc_of(i->s0), bl = loc_of(i->s1), dr = dst(i->d, FSCR0);
    u32 op = (i->w ? O_F2 : O_F3) | O_0F | opc;
    if (bl == dr && al != dr) {
        if (commut) {
            bl = al;
            al = dr;
        } else {
            mov_rr(FSCR1, bl);
            bl = FSCR1;
        }
    }
    if (al != dr)
        mov_loc(dr, al);
    op_loc(op, 0, dr, bl);
    fin(i->d, dr);
}

/* The address ea, of a base and an index (the index in SCR0), goes to SCR1: SCR0 is free again. */
static void ea_join(EA *ea)
{
    EA t;
    ea_base(&t, ea->base, 0);
    t.idx = ea->idx;
    t.scale = ea->scale;
    op_mem(0x8d, 1, SCR1, &t, 0);
    ea->base = SCR1;
    ea->idx = NOREG;
    ea->scale = 0;
}

/* d = s0 op [s1 + s2 * scale + imm] for "op r, r/m" */
static void bin_mem(MIns *i, u32 opc, int fp)
{
    u32 al = loc_of(i->s0), b, x, dr;
    EA ea;
    b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
    x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
    mem_ea(i, b, x, i->imm, &ea);
    if (fp) {
        dr = dst(i->d, FSCR0);
        if (al != dr)
            mov_loc(dr, al);
        op_mem((i->w ? O_F2 : O_F3) | O_0F | opc, 0, dr, &ea, 0);
        fin(i->d, dr);
        return;
    }
    dr = dst(i->d, SCR0);
    if (dr == SCR0 && x == SCR0)
        ea_join(&ea);
    if (al != dr && (dr == ea.base || dr == ea.idx)) {
        /* the result's register is part of the address: the memory operand goes there first */
        op_mem(0x8b, i->w, dr, &ea, 0);
        if (opc == 0x2b) {
            op_rr(0xf7, i->w, 3, dr); /* neg, then add the other */
            opc = 0x03;
        }
        op_loc(opc, i->w, dr, al);
        fin(i->d, dr);
        return;
    }
    if (al != dr)
        mov_loc(dr, al);
    op_mem(opc, i->w, dr, &ea, 0);
    fin(i->d, dr);
}

static void asm_emit(MIns *i);

/* the register of vector register r, which is never spilled */
static u32 vloc(u32 r)
{
    u32 l = loc_of(r);
    if (l == NOREG || is_spill(l))
        die("internal: x86-64: a vector value was not given a register in %s", f->name);
    return l;
}

/* op reg, rm [, imm8] for a code as X_VBIN keeps it */
static void vop_rr(i64 code, u32 reg, u32 rm)
{
    op_rr((u32)code, 0, reg, rm);
    if (code >> 40 & 1)
        b1((u32)(code >> 32) & 0xff);
}

static void vop_ri(u32 code, u32 reg, u32 rm, u32 imm8)
{
    op_rr(code, 0, reg, rm);
    b1(imm8);
}

/* the scalar d (general or xmm register, maybe spilled) = the low lane of the xmm register x; gp: a general register */
static void vsum_put(MIns *i, u32 x, int gp, int w)
{
    u32 l = loc_of(i->d), r;
    EA ea;
    if (!gp) {
        if (is_spill(l)) {
            ea_spill(&ea, l);
            store_reg(x, &ea);
        } else {
            mov_rr(l, x);
        }
        return;
    }
    r = is_spill(l) ? SCR0 : l;
    op_rr(O_66 | O_0F | 0x7e, w, x, r); /* movd / movq r, x */
    if (i->cc == M_16U && i->imm == 5)
        alu_ri(0, 0, r, 8 * 32768); /* the bias taken off each lane below */
    fin(i->d, r);
}

static void encode_vec(MIns *i)
{
    u32 dr, a, b, x;
    EA ea;
    switch (i->op) {
    case X_VBIN: {
        u32 al = vloc(i->s0), bl = vloc(i->s1);
        dr = vloc(i->d);
        if (bl == dr && al != dr) {
            if (i->w) {
                bl = al;
                al = dr;
            } else {
                mov_rr(FSCR1, bl);
                bl = FSCR1;
            }
        }
        mov_rr(dr, al);
        vop_rr(i->imm, dr, bl);
        return;
    }
    case X_VUN:
        vop_rr(i->imm, vloc(i->d), vloc(i->s0));
        return;
    case X_VSHI:
        dr = vloc(i->d);
        mov_rr(dr, vloc(i->s0));
        vop_ri((u32)i->imm, i->cc, dr, i->sym);
        return;
    case X_VSEL:
        /* (mask & s1) | (~mask & s2), put together in the scratch registers: d may be any of the three */
        a = vloc(i->s0);
        mov_rr(FSCR0, a);
        op_rr(O_66 | O_0F | 0xdb, 0, FSCR0, vloc(i->s1)); /* pand */
        mov_rr(FSCR1, a);
        op_rr(O_66 | O_0F | 0xdf, 0, FSCR1, vloc(i->s2)); /* pandn */
        op_rr(O_66 | O_0F | 0xeb, 0, FSCR0, FSCR1);       /* por */
        mov_rr(vloc(i->d), FSCR0);
        return;
    case X_VPAIR:
        dr = vloc(i->d);
        op_loc(O_66 | O_0F | 0x6e, 1, FSCR0, loc_of(i->s1)); /* movq */
        op_loc(O_66 | O_0F | 0x6e, 1, dr, loc_of(i->s0));
        op_rr(O_66 | O_0F | 0x6c, 0, dr, FSCR0); /* punpcklqdq */
        return;
    case X_VONES:
        dr = vloc(i->d);
        op_rr(O_66 | O_0F | (i->imm ? 0xef : 0x76), 0, dr, dr); /* pxor, pcmpeqd */
        return;
    case X_VSPLAT: {
        u32 sl = loc_of(i->s0);
        int k = i->cc;
        dr = vloc(i->d);
        if (i->w) {
            mov_loc(dr, sl);
            if (k == M_F32)
                vop_ri(O_0F | 0xc6, dr, dr, 0); /* shufps */
            else
                op_rr(O_66 | O_0F | 0x14, 0, dr, dr); /* unpcklpd */
            return;
        }
        op_loc(O_66 | O_0F | 0x6e, k == M_64, dr, sl); /* movd / movq */
        if (k == M_64) {
            op_rr(O_66 | O_0F | 0x6c, 0, dr, dr); /* punpcklqdq */
            return;
        }
        if (mem_bytes[k] == 1)
            op_rr(O_66 | O_0F | 0x60, 0, dr, dr); /* punpcklbw */
        if (mem_bytes[k] <= 2)
            op_rr(O_66 | O_0F | 0x61, 0, dr, dr); /* punpcklwd */
        vop_ri(O_66 | O_0F | 0x70, dr, dr, 0);     /* pshufd */
        return;
    }
    case X_VLD:
    case X_VST: {
        int ld = i->op == X_VLD;
        b = use(ld ? i->s0 : i->s1, SCR0);
        x = use(ld ? i->s1 : i->s2, SCR1);
        ea_base(&ea, b, 0);
        ea.idx = x;
        if ((x & 7) == 4 && x < 16) { /* rsp cannot be an index */
            ea.base = x;
            ea.idx = b;
        }
        if (!i->w) {
            op_mem(O_F3 | O_0F | (ld ? 0x6f : 0x7f), 0, ld ? vloc(i->d) : vloc(i->s0), &ea, 0); /* movdqu */
            return;
        }
        {
            int byte = mem_bytes[i->cc] == 1, sgn = i->cc == M_8S || i->cc == M_16S;
            if (!ld) {
                /* each lane's low bits, sign-extended so that the saturating packs keep them as they are */
                u32 t = FSCR0;
                mov_rr(t, vloc(i->s0));
                vop_ri(O_66 | O_0F | 0x72, 6, t, byte ? 24 : 16); /* pslld */
                vop_ri(O_66 | O_0F | 0x72, 4, t, byte ? 24 : 16); /* psrad */
                op_rr(O_66 | O_0F | 0x6b, 0, t, t);                /* packssdw */
                if (byte) {
                    op_rr(O_66 | O_0F | 0x63, 0, t, t); /* packsswb */
                    op_mem(O_66 | O_0F | 0x7e, 0, t, &ea, 0); /* movd */
                } else {
                    op_mem(O_66 | O_0F | 0xd6, 0, t, &ea, 0); /* movq */
                }
                return;
            }
            dr = vloc(i->d);
            if (opt_x64_sse41) {
                /* pmovzxbd pmovsxbd pmovzxwd pmovsxwd */
                op_mem(O_66 | O_0F | O_38 | (byte ? (sgn ? 0x21 : 0x31) : (sgn ? 0x23 : 0x33)), 0, dr, &ea, 0);
                return;
            }
            if (byte)
                op_mem(O_66 | O_0F | 0x6e, 0, dr, &ea, 0); /* movd */
            else
                op_mem(O_F3 | O_0F | 0x7e, 0, dr, &ea, 0); /* movq */
            if (sgn) {
                if (byte)
                    op_rr(O_66 | O_0F | 0x60, 0, dr, dr);       /* punpcklbw: each byte doubled */
                op_rr(O_66 | O_0F | 0x61, 0, dr, dr);           /* punpcklwd */
                vop_ri(O_66 | O_0F | 0x72, 4, dr, byte ? 24 : 16); /* psrad */
            } else {
                op_rr(O_66 | O_0F | 0xef, 0, FSCR0, FSCR0);
                if (byte)
                    op_rr(O_66 | O_0F | 0x60, 0, dr, FSCR0);
                op_rr(O_66 | O_0F | 0x61, 0, dr, FSCR0);
            }
        }
        return;
    }
    case X_VSUM: {
        u32 v = vloc(i->s0), T = FSCR0, U = FSCR1;
        int k = i->cc, fl = (int)i->imm;
        u32 add = k == M_F32 ? O_0F | 0x58 : k == M_F64 ? O_66 | O_0F | 0x58 : k == M_64 ? O_66 | O_0F | 0xd4 : O_66 | O_0F | 0xfe;
        if (fl >= 1 && fl <= 4 && (k == M_F32 || k == M_F64)) {
            add = (k == M_F64 ? O_66 : 0) | O_0F | (fl == 1 ? 0x5f : 0x5d); /* maxps minps (pd) */
        } else if (fl >= 1 && fl <= 4) {
            static const u32 across[5] = { 0, O_38 | 0x3d, O_38 | 0x39, O_38 | 0x3f, O_38 | 0x3b }; /* pmaxsd pminsd pmaxud pminud */
            add = O_66 | O_0F | across[fl];
        }
        if (fl >= 5) {
            if (mem_bytes[k] == 1 && fl == 5) {
                op_rr(O_66 | O_0F | 0xef, 0, T, T);      /* pxor */
                op_rr(O_66 | O_0F | 0xf6, 0, T, v);      /* psadbw: two sums of eight */
            } else {
                if (mem_bytes[k] == 1) {
                    /* signed bytes: widened to words (each byte doubled, then shifted down with its sign) */
                    mov_rr(T, v);
                    op_rr(O_66 | O_0F | 0x60, 0, T, T);  /* punpcklbw */
                    vop_ri(O_66 | O_0F | 0x71, 4, T, 8); /* psraw 8 */
                    mov_rr(U, v);
                    op_rr(O_66 | O_0F | 0x68, 0, U, U);  /* punpckhbw */
                    vop_ri(O_66 | O_0F | 0x71, 4, U, 8);
                    op_rr(O_66 | O_0F | 0xfd, 0, T, U);  /* paddw */
                    op_rr(O_66 | O_0F | 0x75, 0, U, U);  /* pcmpeqw */
                } else {
                    op_rr(O_66 | O_0F | 0x75, 0, U, U);
                    mov_rr(T, v);
                    if (fl == 5) {
                        /* without sign: each word less 32768, which pmaddwd then reads right; added back at the end */
                        vop_ri(O_66 | O_0F | 0x71, 6, U, 15); /* psllw 15 */
                        op_rr(O_66 | O_0F | 0xef, 0, T, U);
                    }
                }
                vop_ri(O_66 | O_0F | 0x71, 2, U, 15); /* psrlw 15: ones */
                op_rr(O_66 | O_0F | 0xf5, 0, T, U);   /* pmaddwd: four sums of two */
                vop_ri(O_66 | O_0F | 0x70, U, T, 0x4e);
                op_rr(O_66 | O_0F | 0xfe, 0, U, T);
                vop_ri(O_66 | O_0F | 0x70, T, U, 0xb1);
                op_rr(O_66 | O_0F | 0xfe, 0, T, U);
                vsum_put(i, T, 1, 0);
                return;
            }
            vop_ri(O_66 | O_0F | 0x70, U, T, 0x4e); /* pshufd */
            op_rr(O_66 | O_0F | 0xfe, 0, U, T);     /* paddd */
            vsum_put(i, U, 1, 0);
            return;
        }
        /* halves, then (four lanes) quarters */
        vop_ri(O_66 | O_0F | 0x70, T, v, 0x4e);
        op_rr(add, 0, T, v);
        if (k == M_64 || k == M_F64) {
            vsum_put(i, T, k == M_64, 1);
            return;
        }
        vop_ri(O_66 | O_0F | 0x70, U, T, 0xb1);
        op_rr(add, 0, U, T);
        vsum_put(i, U, k != M_F32, 0);
        return;
    }
    case X_VGATH: {
        /* lane by lane, in order: pextrd the index, sign-extend it, load the element extended, pinsrd it */
        static const u32 ld[] = { O_0F | 0xbe, O_0F | 0xb6, O_0F | 0xbf, O_0F | 0xb7, 0x8b, 0x8b, 0x8b, 0x8b };
        u32 idx = vloc(i->s1), k;
        b = use(i->s0, SCR1);
        for (k = 0; k < 4; k++) {
            vop_ri(O_66 | O_0F | O_3A | 0x16, idx, SCR0, k); /* pextrd r32, xmm, k */
            op_rr(0x63, 1, SCR0, SCR0);                        /* movsxd */
            ea_base(&ea, b, 0);
            ea.idx = SCR0;
            ea.scale = i->sym;
            op_mem(ld[i->cc], 0, SCR0, &ea, 0);
            vop_ri(O_66 | O_0F | O_3A | 0x22, FSCR0, SCR0, k); /* pinsrd xmm, r32, k */
        }
        mov_rr(vloc(i->d), FSCR0);
        return;
    }
    case X_FUN: {
        /* how 0: op d, s (sqrtsd, roundsd: d's upper half is whatever it was); 1: fabs; 2: cvtsd2si into a general register */
        u32 sl = loc_of(i->s0);
        if (i->cc == 2) {
            dr = dst(i->d, SCR0);
            op_loc((u32)i->imm, i->w, dr, sl);
            fin(i->d, dr);
            return;
        }
        dr = dst(i->d, FSCR0);
        if (i->cc == 1) {
            if (sl != dr)
                mov_loc(dr, sl);
            ea_base(&ea, NOREG, 0);
            ea.sym = fconst_sym(i->sym ? (u64)0x7fffffffu << 32 | 0x7fffffffu : 0x7fffffffffffffffUL, 16);
            op_mem(O_0F | 0x54, 0, dr, &ea, 0); /* andps with the mask in the constant section */
        } else {
            if (sl != dr)
                op_rr(O_0F | 0x57, 0, dr, dr); /* xorps: sqrtss / roundss merge into dr's upper lanes */
            op_loc((u32)i->imm, 0, dr, sl);
            if ((i->imm & 0xff) == 0x0a || (i->imm & 0xff) == 0x0b)
                b1((u32)(i->imm >> 32) & 0xff);
        }
        fin(i->d, dr);
        return;
    }
    default:
        die("internal: x86-64 vector encoding");
    }
}

static int uses_ymm; /* the function has 256-bit values: vzeroupper before calls and returns */

static void vzeroupper(void)
{
    b1(0xc5);
    b1(0xf8);
    b1(0x77);
}

/* X_VEX: one instruction on registers, the form saying where each goes */
static void enc_vex(MIns *i)
{
    u32 enc = (u32)i->imm, dr = NOREG, ra, rv, rm, dl, al, bl, cl;
    int imm8 = (i->imm >> 32 & 1) ? (int)((u64)i->imm >> 40 & 0xff) : -1;
    switch (i->w) {
    case XF_RVM:
        rv = xuse(i->s0, 0);
        rm = loc_of(i->s1);
        if (is_spill(rm) && !spill_cached(rm, FSCR1) && rm_mem_ok(enc)) {
            /* the r/m source straight from its spill slot */
            EA ea;
            ea_spill(&ea, rm);
            dr = xdst(i->d);
            vex_mem(enc, dr, rv, &ea, imm8 >= 0 ? 1 : 0);
            break;
        }
        rm = xuse(i->s1, 1);
        dr = xdst(i->d);
        vex_rr(enc, dr, rv, rm);
        break;
    case XF_RM:
        rm = loc_of(i->s0);
        if (is_spill(rm) && !spill_cached(rm, FSCR1) && rm_mem_ok(enc)) {
            EA ea;
            ea_spill(&ea, rm);
            dr = xdst(i->d);
            vex_mem(enc, dr, NOREG, &ea, imm8 >= 0 ? 1 : 0);
            break;
        }
        rm = xuse(i->s0, 1);
        dr = xdst(i->d);
        vex_rr(enc, dr, NOREG, rm);
        break;
    case XF_MR:
        ra = xuse(i->s0, 1);
        dr = xdst(i->d);
        vex_rr(enc, ra, NOREG, dr);
        break;
    case XF_VMI:
        rm = xuse(i->s0, 1);
        dr = xdst(i->d);
        vex_rr(enc, XE_DIGIT(enc), dr, rm);
        break;
    case XF_RVMR:
        rv = xuse(i->s0, 0);
        rm = xuse(i->s1, 1);
        ra = xuse(i->s2, 2);
        dr = xdst(i->d);
        vex_rr(enc, dr, rv, rm);
        imm8 = (int)((ra & 15) << 4);
        break;
    case XF_DVM:
    case XF_DVMK:
        /* d = s0, then d = s1 * d + s2 (the 213 forms); d must not be where s1 or s2 still is */
        dl = loc_of(i->d);
        al = loc_of(i->s0);
        bl = loc_of(i->s1);
        cl = loc_of(i->s2);
        dr = is_spill(dl) || ((dl == bl || dl == cl) && dl != al) ? FSCR0 : dl;
        rv = xuse(i->s1, 1);
        rm = xuse(i->s2, 2);
        if (al != dr)
            mov_locw(dr, al, vw_of(i->s0));
        vex_rr(enc, dr, rv, rm);
        if (imm8 >= 0) { /* (vpternlog) before the copy below */
            b1((u32)imm8);
            imm8 = -1;
        }
        if (dr == FSCR0 && !is_spill(dl)) {
            vmov_rr(dl, dr, vw_of(i->d));
            dr = NOREG;
        }
        break;
    case XF_TEST:
        ra = xuse(i->s0, 0);
        rm = xuse(i->s1, 1);
        vex_rr(enc, ra, NOREG, rm);
        break;
    case XF_FMA:
        dl = loc_of(i->d);
        al = loc_of(i->s0);
        bl = loc_of(i->s1);
        cl = loc_of(i->s2);
        if (!is_spill(dl) && dl == cl && dl != al && dl != bl) {
            rv = xuse(i->s0, 1);
            rm = xuse(i->s1, 2);
            vex_rr(enc + 0x10, dl, rv, rm); /* 231: d = vvvv * rm + d */
            dr = NOREG;
            break;
        }
        if (!is_spill(dl) && (dl == al || dl == bl)) {
            rv = xuse(dl == al ? i->s1 : i->s0, 1);
            rm = xuse(i->s2, 2);
            vex_rr(enc, dl, rv, rm); /* 213: d = vvvv * d + rm */
            dr = NOREG;
            break;
        }
        dr = is_spill(dl) || dl == cl ? FSCR0 : dl;
        mov_locw(dr, al, vw_of(i->s0));
        rv = xuse(i->s1, 1);
        rm = xuse(i->s2, 2);
        vex_rr(enc, dr, rv, rm);
        if (dr == FSCR0 && !is_spill(dl)) {
            vmov_rr(dl, dr, vw_of(i->d));
            dr = NOREG;
        }
        break;
    default:
        die("internal: x86-64 vector form %d", i->w);
    }
    if (imm8 >= 0)
        b1((u32)imm8);
    if (dr != NOREG && i->d != NOREG)
        fin(i->d, dr);
}

static void encode(MIns *i)
{
    u32 dr, a, b, x, n;
    EA ea;
    int w = i->w;

    if (i->op >= X_VBIN && i->op <= X_FUN) {
        encode_vec(i);
        return;
    }
    switch (i->op) {
    case X_DEFREG:
        return;
    case X_VEX:
        enc_vex(i);
        return;
    case X_VEXM: {
        u32 enc = (u32)((u64)i->imm >> 32);
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, (i32)(u32)i->imm, &ea);
        dr = NOREG;
        if (i->w & 1) {
            a = xuse(i->s0, 0);
            vex_mem(enc, a, NOREG, &ea, i->w & 2 ? 1 : 0);
        } else {
            a = i->w & 4 ? xuse(i->s0, 0) : NOREG;
            dr = xdst(i->d);
            vex_mem(enc, dr, a, &ea, i->w & 2 ? 1 : 0);
        }
        if (i->w & 2)
            b1(i->cc);
        if (!(i->w & 1))
            fin(i->d, dr);
        return;
    }
    case X_YZERO:
    case X_YONES:
        dr = xdst(i->d);
        if (i->op == X_YZERO)
            vex_rr(0x5ef, dr, dr, dr); /* vpxor xmm: the whole register */
        else
            if (i->w == 2) {
                vex_rr(VE(0x25, 3, 1, 0) | VE_Z, dr, dr, dr); /* vpternlogd 0xff */
                b1(0xff);
            } else {
                vex_rr(0x576 | (i->w ? 0x2000u : 0), dr, dr, dr); /* vpcmpeqd */
            }
        fin(i->d, dr);
        return;
    case X_YSIGN:
        dr = xdst(i->d);
        mov_ri(SCR0, i->cc == M_F64 ? (u64)1 << 63 : 0x80000000UL, i->cc == M_F64);
        vex_rr(0x56e | (i->cc == M_F64 ? 0x1000u : 0), FSCR1, NOREG, SCR0);                           /* vmovd / vmovq */
        vex_rr((i->cc == M_F64 ? (i->w == 2 ? 0x1659 : 0x659) : 0x658) | (i->w == 2 ? VE_Z : i->w ? 0x2000u : 0), dr, NOREG,
               FSCR1); /* vpbroadcastd / q */
        fin(i->d, dr);
        return;
    case X_YSPLAT: {
        int lane = i->cc, bits = lane == M_8S || lane == M_8U ? 8 : lane == M_16S || lane == M_16U ? 16
                              : lane == M_32 || lane == M_F32 ? 32 : 64;
        u32 L = i->w == 2 ? VE_Z : i->w ? 0x2000u : 0, s = xuse(i->s0, 1);
        dr = xdst(i->d);
        if (!is_xreg(i->s0)) {
            vex_rr(0x56e | (bits == 64 ? 0x1000u : 0), FSCR1, NOREG, s); /* vmovd / vmovq */
            s = FSCR1;
        }
        if (lane == M_F32)
            vex_rr(0x618 | L, dr, NOREG, s); /* vbroadcastss */
        else
            vex_rr((bits == 8 ? 0x678 : bits == 16 ? 0x679 : bits == 32 ? 0x658 : i->w == 2 ? 0x1659 : 0x659) | L, dr, NOREG,
                   s); /* vpbroadcastb w d q */
        fin(i->d, dr);
        return;
    }
    case X_YMOVLO:
        a = xuse(i->s0, 1);
        dr = xdst(i->d);
        vex_rr(0x128, dr, NOREG, a); /* vmovaps xmm: the upper part cleared */
        fin(i->d, dr);
        return;
    case X_GATHER: {
        u32 enc = (u32)i->imm, base, idx;
        if (!i->w)
            vex_rr(0x576 | (enc & 0x2000u), FSCR1, FSCR1, FSCR1); /* every lane */
        if (i->s0 != NOREG)
            mov_locw(FSCR0, loc_of(i->s0), vw_of(i->s0));
        else
            vex_rr(0x5ef, FSCR0, FSCR0, FSCR0);
        idx = xuse(i->s2, 2);
        base = use(i->s1, SCR0);
        ea_base(&ea, base, 0);
        ea.idx = idx;
        ea.scale = i->cc == 1 ? 0 : i->cc == 2 ? 1 : i->cc == 4 ? 2 : 3;
        vex_mem(enc, FSCR0, FSCR1, &ea, 0);
        if (is_spill(loc_of(i->d)))
            fin(i->d, FSCR0);
        else
            vmov_rr(loc_of(i->d), FSCR0, vw_of(i->d));
        return;
    }
    case X_KMOVK:
        a = use(i->s0, SCR0);
        vex_rr(VE(0x92, 1, 0, 0), 1, NOREG, a); /* kmovw k1, r32 */
        return;
    case X_KTEST:
        a = xuse(i->s0, 0);
        b = xuse(i->s1, 1);
        vex_rr((u32)i->imm, 1, a, b); /* vptestm k1, a, b */
        dr = dst(i->d, SCR0);
        /* kmovq / kmovd / kmovw r, k1 */
        vex_rr(i->cc == 8 ? VE(0x93, 1, 3, 1) : i->cc == 4 ? VE(0x93, 1, 3, 0) : VE(0x93, 1, 0, 0), dr, NOREG, 1);
        fin(i->d, dr);
        return;
    case X_CPUID:
        b1(0x53); /* push rbx: cpuid writes it */
        mov_ri(RAX, (u64)(u32)i->imm, 0);
        mov_ri(RCX, (u64)i->imm >> 32, 0);
        b1(0x0f);
        b1(0xa2);
        if (i->cc)
            op_rr(0x89, 0, i->cc == 1 ? RBX : i->cc == 2 ? RCX : RDX, RAX);
        b1(0x5b);
        return;
    case X_RAW:
        if ((u64)i->imm > 0xffff)
            b1((u32)((u64)i->imm >> 16));
        if ((u64)i->imm > 0xff)
            b1((u32)((u64)i->imm >> 8));
        b1((u32)i->imm);
        return;
    case X_PREFETCH:
        a = i->s0 == NOREG ? NOREG : use(i->s0, SCR0);
        x = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        mem_ea(i, a, x, i->imm, &ea);
        op_mem(O_0F | 0x18, 0, i->cc, &ea, 0);
        return;
    case X_MOV: {
        u32 dl = loc_of(i->d), sl = loc_of(i->s0);
        int vw = vw_of(i->d) > vw_of(i->s0) ? vw_of(i->d) : vw_of(i->s0);
        if (dl == NOREG || dl == sl)
            return;
        if (vw) {
            if (is_spill(dl) && is_spill(sl)) {
                ea_spill(&ea, sl);
                load_regw(FSCR0, &ea, vw);
                ea_spill(&ea, dl);
                store_regw(FSCR0, &ea, vw);
            } else if (is_spill(dl)) {
                ea_spill(&ea, dl);
                store_regw(sl, &ea, vw);
            } else {
                mov_locw(dl, sl, vw);
            }
            return;
        }
        if (is_spill(dl) && is_spill(sl)) {
            ea_spill(&ea, sl);
            load_reg(SCR0, &ea);
            ea_spill(&ea, dl);
            store_reg(SCR0, &ea);
        } else if (is_spill(dl)) {
            ea_spill(&ea, dl);
            store_reg(sl, &ea);
        } else {
            mov_loc(dl, sl);
        }
        return;
    }
    case X_MOVI:
        dr = dst(i->d, SCR0);
        mov_ri(dr, (u64)i->imm, w);
        fin(i->d, dr);
        return;
    case X_FMOVI:
        dr = dst(i->d, FSCR0);
        if (i->imm == 0) {
            op_rr(O_0F | 0x57, 0, dr, dr); /* xorps */
        } else {
            ea_base(&ea, NOREG, 0);
            ea.sym = fconst_sym((u64)i->imm & (w ? ~(u64)0 : 0xffffffffu), w ? 8 : 4);
            op_mem((w ? O_F2 : O_F3) | O_0F | 0x10, 0, dr, &ea, 0); /* movss / movsd from the constant */
        }
        fin(i->d, dr);
        return;
    case X_LEA:
        dr = dst(i->d, SCR0);
        ea_base(&ea, NOREG, i->imm);
        ea.sym = i->sym;
        op_mem(0x8d, 1, dr, &ea, 0);
        fin(i->d, dr);
        return;
    case X_SLOTADDR:
        dr = dst(i->d, SCR0);
        ea_frame(&ea, i->sym == ARGSLOT ? 16 + i->imm : f->slots[i->sym].off + i->imm);
        op_mem(0x8d, 1, dr, &ea, 0);
        fin(i->d, dr);
        return;
    case X_ADD: {
        u32 al = loc_of(i->s0), bl = loc_of(i->s1);
        dr = dst(i->d, SCR0);
        if (!is_spill(al) && !is_spill(bl) && al != dr && bl != dr) {
            ea_base(&ea, al, 0);
            ea.idx = bl;
            if ((bl & 7) == 4) { /* rsp cannot be an index */
                ea.base = bl;
                ea.idx = al;
            }
            op_mem(0x8d, w, dr, &ea, 0);
            fin(i->d, dr);
            return;
        }
        bin_int(i, 0x03, 0);
        return;
    }
    case X_SUB: bin_int(i, 0x2b, 1); return;
    case X_AND: bin_int(i, 0x23, 0); return;
    case X_OR: bin_int(i, 0x0b, 0); return;
    case X_XOR: bin_int(i, 0x33, 0); return;
    case X_IMUL: bin_int(i, O_0F | 0xaf, 0); return;
    case X_LEAX:
        a = use(i->s0, SCR0);
        x = use(i->s1, SCR1);
        dr = dst(i->d, SCR0);
        ea_base(&ea, a, 0);
        ea.idx = x;
        ea.scale = (int)i->imm;
        if ((x & 7) == 4 && x < 16 && i->imm == 0) { /* (rsp is no index; never scaled here) */
            ea.base = x;
            ea.idx = a;
        }
        op_mem(0x8d, w, dr, &ea, 0);
        fin(i->d, dr);
        return;
    case X_ADDI: {
        u32 al = loc_of(i->s0);
        dr = dst(i->d, SCR0);
        if (!is_spill(al) && al != dr) {
            ea_base(&ea, al, i->imm);
            op_mem(0x8d, w, dr, &ea, 0);
        } else {
            if (al != dr)
                mov_loc(dr, al);
            if (i->imm)
                alu_ri(0, w, dr, i->imm);
        }
        fin(i->d, dr);
        return;
    }
    case X_ANDI:
    case X_ORI:
    case X_XORI:
        dr = dst(i->d, SCR0);
        mov_loc(dr, loc_of(i->s0));
        alu_ri(i->op == X_ANDI ? 4 : i->op == X_ORI ? 1 : 6, w, dr, i->imm);
        fin(i->d, dr);
        return;
    case X_IMULI:
        dr = dst(i->d, SCR0);
        if (i->imm >= -128 && i->imm <= 127) {
            op_loc(0x6b, w, dr, loc_of(i->s0));
            b1((u32)i->imm);
        } else {
            op_loc(0x69, w, dr, loc_of(i->s0));
            b4((u32)i->imm);
        }
        fin(i->d, dr);
        return;
    case X_SHL:
    case X_SHR:
    case X_SAR:
    case X_SHLI:
    case X_SHRI:
    case X_SARI:
    case X_RORI: {
        static const u8 digit[] = { 4, 5, 7, 4, 5, 7, 1 };
        int byimm = i->op >= X_SHLI;
        dr = dst(i->d, SCR0);
        if (dr == RCX && !byimm)
            dr = SCR0; /* the count is there */
        mov_loc(dr, loc_of(i->s0));
        if (!byimm) {
            op_rr(0xd3, w, digit[i->op - X_SHL], dr);
        } else if (i->imm) {
            op_rr(0xc1, w, digit[i->op - X_SHL], dr);
            b1((u32)i->imm);
        }
        if (loc_of(i->d) == RCX && !byimm)
            mov_rr(RCX, dr);
        else
            fin(i->d, dr);
        return;
    }
    case X_NEG:
    case X_NOT:
        dr = dst(i->d, SCR0);
        mov_loc(dr, loc_of(i->s0));
        op_rr(0xf7, w, i->op == X_NEG ? 3 : 2, dr);
        fin(i->d, dr);
        return;
    case X_BSWAP:
        dr = dst(i->d, SCR0);
        mov_loc(dr, loc_of(i->s0));
        op_prefix(O_0F | 0xc8 | (dr & 7), w, NOREG, NOREG, dr);
        fin(i->d, dr);
        return;
    case X_BSF:
        dr = dst(i->d, SCR0);
        op_loc(O_0F | 0xbc, w, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_CLZ:
        dr = dst(i->d, SCR0);
        op_loc(O_0F | 0xbd, w, dr, loc_of(i->s0)); /* bsr: the index of the highest bit */
        alu_ri(6, 0, dr, w ? 63 : 31);
        fin(i->d, dr);
        return;
    case X_CQO:
        if (w)
            b1(0x48);
        b1(0x99);
        return;
    case X_IDIV: op_loc(0xf7, w, 7, loc_of(i->s0)); return;
    case X_DIV: op_loc(0xf7, w, 6, loc_of(i->s0)); return;
    case X_MULH: op_loc(0xf7, 1, 4, loc_of(i->s0)); return;
    case X_IMULH: op_loc(0xf7, 1, 5, loc_of(i->s0)); return;
    case X_MOVSX8:
    case X_MOVZX8:
        dr = dst(i->d, SCR0);
        op_loc(O_0F | O_B8M | (i->op == X_MOVSX8 ? 0xbe : 0xb6), i->op == X_MOVSX8 && w, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_MOVSX16:
    case X_MOVZX16:
        dr = dst(i->d, SCR0);
        op_loc(O_0F | (i->op == X_MOVSX16 ? 0xbf : 0xb7), i->op == X_MOVSX16 && w, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_MOVSXD:
        dr = dst(i->d, SCR0);
        op_loc(0x63, 1, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_MOVZXD:
        dr = dst(i->d, SCR0);
        op_loc(0x8b, 0, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_CMP:
        a = use(i->s0, SCR0);
        op_loc(0x3b, w, a, loc_of(i->s1));
        return;
    case X_CMPI: {
        u32 al = loc_of(i->s0);
        int small = i->imm >= -128 && i->imm <= 127;
        if (is_spill(al)) {
            ea_spill(&ea, al);
            op_mem(small ? 0x83 : 0x81, w, 7, &ea, small ? 1 : 4);
        } else {
            op_rr(small ? 0x83 : 0x81, w, 7, al);
        }
        if (small)
            b1((u32)i->imm);
        else
            b4((u32)i->imm);
        return;
    }
    case X_TESTI: {
        u32 al = loc_of(i->s0);
        int wide = w && i->imm < 0; /* else the upper half adds nothing */
        if (is_spill(al)) {
            ea_spill(&ea, al);
            op_mem(0xf7, wide, 0, &ea, 4);
        } else {
            op_rr(0xf7, wide, 0, al);
        }
        b4((u32)i->imm);
        return;
    }
    case X_TEST:
        a = use(i->s0, SCR0);
        b = i->s1 == i->s0 ? a : use(i->s1, SCR1);
        op_rr(0x85, w, b, a);
        return;
    case X_FCMP:
        a = use(i->s0, FSCR0);
        op_loc((w ? O_66 : 0) | O_0F | 0x2e, 0, a, loc_of(i->s1)); /* ucomiss / ucomisd */
        return;
    case X_SETCC:
        dr = dst(i->d, SCR0);
        if (i->cc >= CC_FEQ) {
            op_rr(O_0F | O_B8M | 0x90 | (i->cc == CC_FEQ ? CC_NP : CC_P), 0, 0, SCR1);
            op_rr(O_0F | O_B8M | 0x90 | (i->cc == CC_FEQ ? CC_E : CC_NE), 0, 0, dr);
            op_rr(O_B8R | O_B8M | (i->cc == CC_FEQ ? 0x20 : 0x08), 0, SCR1, dr); /* and / or */
        } else {
            op_rr(O_0F | O_B8M | 0x90 | i->cc, 0, 0, dr);
        }
        op_rr(O_0F | O_B8M | 0xb6, 0, dr, dr);
        fin(i->d, dr);
        return;
    case X_CMOV: {
        /* d = cc ? s0 : s1 */
        u32 tl = loc_of(i->s0), el = loc_of(i->s1);
        int cc = i->cc;
        dr = dst(i->d, SCR0);
        if (cc == CC_FEQ) { /* as "not equal" with the sides exchanged */
            u32 t = tl;
            tl = el;
            el = t;
            cc = CC_FNE;
        }
        if (cc == CC_FNE) {
            if (tl == dr) {
                if (el != dr) {
                    jf_cc(CC_FNE);
                    mov_loc(dr, el);
                    jf_here();
                }
            } else {
                if (el != dr)
                    mov_loc(dr, el);
                op_loc(O_0F | 0x40 | CC_NE, w, dr, tl);
                op_loc(O_0F | 0x40 | CC_P, w, dr, tl);
            }
        } else if (tl == dr) {
            if (el != dr)
                op_loc(O_0F | 0x40 | (u32)(cc ^ 1), w, dr, el);
        } else {
            if (el != dr)
                mov_loc(dr, el);
            op_loc(O_0F | 0x40 | (u32)cc, w, dr, tl);
        }
        fin(i->d, dr);
        return;
    }
    case X_FSEL: {
        u32 tl = loc_of(i->s0), el = loc_of(i->s1);
        int vw = vw_of(i->d);
        dr = dst(i->d, FSCR0);
        if (tl == dr) {
            if (el != dr) {
                jf_cc(i->cc);
                mov_locw(dr, el, vw);
                jf_here();
            }
        } else {
            if (el != dr)
                mov_locw(dr, el, vw);
            jf_cc(i->cc ^ 1);
            mov_locw(dr, tl, vw);
            jf_here();
        }
        fin(i->d, dr);
        return;
    }
    case X_LOAD: {
        static const u32 ops[] = { O_0F | 0xbe, O_0F | 0xb6, O_0F | 0xbf, O_0F | 0xb7, 0x8b, 0x8b, 0x8b, 0x8b, 0, 0, 0, 0x63 }; /* M_V*: none */
        int fp = i->d >= VREG0 ? mf.vclass[i->d - VREG0] & 1 : i->d >= XMM0;
        a = i->s0 == NOREG ? NOREG : use(i->s0, SCR0);
        x = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        mem_ea(i, a, x, i->imm, &ea);
        dr = dst(i->d, fp ? FSCR0 : SCR0);
        if (i->cc == M_V128 || i->cc == M_V256 || i->cc == M_V512) {
            load_regw(dr, &ea, mem_bytes[i->cc]);
            fin(i->d, dr);
            return;
        }
        if (fp)
            op_mem((mem_bytes[i->cc] == 8 ? O_F2 : O_F3) | O_0F | 0x10, 0, dr, &ea, 0);
        else
            op_mem(ops[i->cc], i->cc == M_64 || i->cc == M_F64 || i->cc == LOAD_SIGNED32, dr, &ea, 0);
        fin(i->d, dr);
        return;
    }
    case X_STORE: {
        u32 vl = loc_of(i->s0);
        int fp = i->s0 >= VREG0 ? mf.vclass[i->s0 - VREG0] & 1 : i->s0 >= XMM0;
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, i->imm, &ea);
        if (x == SCR0 && is_spill(vl) && !fp)
            ea_join(&ea); /* three spilled operands, two registers: the address is put together first (from the ea, whose base
                             is rbp for a frame slot: b was NOREG there, and the lea of b + x lost the frame) */
        a = use(i->s0, fp ? FSCR0 : SCR0);
        n = mem_bytes[i->cc];
        if (i->cc == M_V128 || i->cc == M_V256 || i->cc == M_V512)
            store_regw(a, &ea, (int)n);
        else if (fp)
            op_mem((n == 8 ? O_F2 : O_F3) | O_0F | 0x11, 0, a, &ea, 0);
        else if (n == 1)
            op_mem(O_B8R | 0x88, 0, a, &ea, 0);
        else
            op_mem((n == 2 ? O_66 : 0) | 0x89, n == 8, a, &ea, 0);
        return;
    }
    case X_STOREI: {
        i64 val = i->imm >> 32;
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, (i32)(u32)i->imm, &ea);
        n = mem_bytes[i->cc];
        if (n == 1) {
            op_mem(0xc6, 0, 0, &ea, 1);
            b1((u32)val);
        } else if (n == 2) {
            op_mem(O_66 | 0xc7, 0, 0, &ea, 2);
            b1((u32)val);
            b1((u32)val >> 8);
        } else {
            op_mem(0xc7, n == 8, 0, &ea, 4);
            b4((u32)val);
        }
        return;
    }
    case X_ADDM: bin_mem(i, 0x03, 0); return;
    case X_SUBM: bin_mem(i, 0x2b, 0); return;
    case X_ANDM: bin_mem(i, 0x23, 0); return;
    case X_ORM: bin_mem(i, 0x0b, 0); return;
    case X_XORM: bin_mem(i, 0x33, 0); return;
    case X_IMULM: bin_mem(i, O_0F | 0xaf, 0); return;
    case X_FADDM: bin_mem(i, 0x58, 1); return;
    case X_FMULM: bin_mem(i, 0x59, 1); return;
    case X_FSUBM: bin_mem(i, 0x5c, 1); return;
    case X_FDIVM: bin_mem(i, 0x5e, 1); return;
    case X_CMPM:
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, i->imm, &ea);
        if (x == SCR0 && is_spill(loc_of(i->s0)))
            ea_join(&ea);
        a = use(i->s0, SCR0);
        op_mem(i->cc ? 0x39 : 0x3b, w, a, &ea, 0);
        return;
    case X_RMW:
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, i->imm, &ea);
        if (x == SCR0 && is_spill(loc_of(i->s0)))
            ea_join(&ea);
        a = use(i->s0, SCR0);
        if (mem_bytes[i->cc] == 1)
            op_mem(O_B8R | (i->w << 3), 0, a, &ea, 0);
        else
            op_mem(i->w << 3 | 1, i->cc == M_64, a, &ea, 0);
        return;
    case X_RMWI: {
        i64 val = i->imm >> 32;
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, (i32)(u32)i->imm, &ea);
        if (mem_bytes[i->cc] == 1) {
            op_mem(0x80, 0, i->w, &ea, 1);
            b1((u32)val);
        } else if (val >= -128 && val <= 127) {
            op_mem(0x83, i->cc == M_64, i->w, &ea, 1);
            b1((u32)val);
        } else {
            op_mem(0x81, i->cc == M_64, i->w, &ea, 4);
            b4((u32)val);
        }
        return;
    }
    case X_CMPMI: {
        i64 val = i->imm >> 32;
        b = i->s1 == NOREG ? NOREG : use(i->s1, SCR1);
        x = i->s2 == NOREG ? NOREG : use(i->s2, SCR0);
        mem_ea(i, b, x, (i32)(u32)i->imm, &ea);
        if (mem_bytes[i->cc] == 1) {
            op_mem(0x80, 0, 7, &ea, 1);
            b1((u32)val);
        } else if (val >= -128 && val <= 127) {
            op_mem(0x83, w, 7, &ea, 1);
            b1((u32)val);
        } else {
            op_mem(0x81, w, 7, &ea, 4);
            b4((u32)val);
        }
        return;
    }
    case X_FMAX: bin_fp(i, 0x5f, 0); return;
    case X_FMIN: bin_fp(i, 0x5d, 0); return;
    case X_FADD: bin_fp(i, 0x58, 1); return;
    case X_FMUL: bin_fp(i, 0x59, 1); return;
    case X_FSUB: bin_fp(i, 0x5c, 0); return;
    case X_FDIV: bin_fp(i, 0x5e, 0); return;
    case X_FNEG: {
        u32 sl = loc_of(i->s0);
        dr = dst(i->d, FSCR0);
        if (sl != dr)
            mov_loc(dr, sl);
        ea_base(&ea, NOREG, 0);
        ea.sym = fconst_sym(w ? (u64)1 << 63 : (u64)0x80000000u << 32 | 0x80000000u, 16);
        op_mem(O_0F | 0x57, 0, dr, &ea, 0); /* xorps with the sign bits */
        fin(i->d, dr);
        return;
    }
    case X_SITOF:
        dr = dst(i->d, FSCR0);
        op_rr(O_0F | 0x57, 0, dr, dr); /* xorps: cvtsi2ss writes the low lane only and would wait for dr's last writer */
        op_loc((w ? O_F2 : O_F3) | O_0F | 0x2a, i->cc, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_UITOF:
        dr = dst(i->d, FSCR0);
        op_rr(O_0F | 0x57, 0, dr, dr); /* xorps (as X_SITOF) */
        if (!i->cc) {
            op_loc(0x8b, 0, SCR0, loc_of(i->s0)); /* the upper half cleared */
            op_rr((w ? O_F2 : O_F3) | O_0F | 0x2a, 1, dr, SCR0);
        } else {
            /* a value with the top bit set is halved, keeping the lowest bit for the rounding, and doubled after */
            mov_loc(SCR0, loc_of(i->s0));
            op_rr(0x85, 1, SCR0, SCR0);
            jf(0x70 | CC_S);
            op_rr((w ? O_F2 : O_F3) | O_0F | 0x2a, 1, dr, SCR0);
            b1(0xeb); /* jmp over the rest */
            n = (u32)text->len;
            b1(0);
            jf_here();
            mov_rr(SCR1, SCR0);
            op_rr(0xd1, 1, 5, SCR1); /* shr 1 */
            alu_ri(4, 0, SCR0, 1);
            op_rr(0x0b, 1, SCR1, SCR0);
            op_rr((w ? O_F2 : O_F3) | O_0F | 0x2a, 1, dr, SCR1);
            op_rr((w ? O_F2 : O_F3) | O_0F | 0x58, 0, dr, dr);
            text->p[n] = (u8)(text->len - n - 1);
        }
        fin(i->d, dr);
        return;
    case X_BITCAST:
        if (i->cc) {
            dr = dst(i->d, FSCR0);
            op_loc(O_66 | O_0F | 0x6e, w, dr, loc_of(i->s0)); /* movd / movq xmm, r/m */
        } else {
            u32 sl = loc_of(i->s0);
            dr = dst(i->d, SCR0);
            if (is_spill(sl))
                op_loc(0x8b, w, dr, sl); /* the bits straight from the spill slot */
            else
                op_rr(O_66 | O_0F | 0x7e, w, sl, dr); /* movd / movq r, xmm */
        }
        fin(i->d, dr);
        return;
    case X_FTOSI:
        dr = dst(i->d, SCR0);
        op_loc((i->cc ? O_F2 : O_F3) | O_0F | 0x2c, w, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_FTOUI: {
        u32 pre = (i->cc ? O_F2 : O_F3) | O_0F;
        dr = dst(i->d, SCR0);
        if (!w) {
            op_loc(pre | 0x2c, 1, dr, loc_of(i->s0));
            op_rr(0x8b, 0, dr, dr);
        } else {
            /* from 2^63 up: convert what is above 2^63 and put the top bit back */
            a = use(i->s0, FSCR0);
            mov_ri(SCR0, i->cc ? 0x43e0000000000000UL : 0x5f000000UL, i->cc);
            op_rr(O_66 | O_0F | 0x6e, i->cc, FSCR1, SCR0);
            op_rr((i->cc ? O_66 : 0) | O_0F | 0x2e, 0, a, FSCR1);
            jf(0x70 | CC_AE);
            op_rr(pre | 0x2c, 1, dr, a);
            b1(0xeb);
            n = (u32)text->len;
            b1(0);
            jf_here();
            mov_rr(FSCR0, a);
            op_rr(pre | 0x5c, 0, FSCR0, FSCR1);
            op_rr(pre | 0x2c, 1, dr, FSCR0);
            op_rr(O_0F | 0xba, 1, 7, dr); /* btc 63 */
            b1(63);
            text->p[n] = (u8)(text->len - n - 1);
        }
        fin(i->d, dr);
        return;
    }
    case X_FCVT:
        dr = dst(i->d, FSCR0);
        if (loc_of(i->s0) != dr)
            op_rr(O_0F | 0x57, 0, dr, dr); /* xorps: cvtss2sd / cvtsd2ss merge into dr's upper lanes */
        op_loc((w ? O_F3 : O_F2) | O_0F | 0x5a, 0, dr, loc_of(i->s0));
        fin(i->d, dr);
        return;
    case X_JMP:
        b1(0xe9);
        add_fixup((u32)i->imm, FK_JMP);
        return;
    case X_JCC:
        if (i->cc == CC_FNE) {
            b1(0x0f);
            b1(0x80 | CC_NE);
            add_fixup((u32)i->imm, FK_JCC_PAIR);
            b1(0x0f);
            b1(0x80 | CC_P);
            add_fixup((u32)i->imm, FK_JCC_PAIR);
        } else if (i->cc == CC_FEQ) {
            b1(0x70 | CC_P);
            b1(6);
            b1(0x0f);
            b1(0x80 | CC_E);
            add_fixup((u32)i->imm, FK_JCC_JP);
        } else {
            b1(0x0f);
            b1(0x80 | i->cc);
            add_fixup((u32)i->imm, FK_JCC);
        }
        return;
    case X_CALL:
        if (uses_ymm && !i->cc)
            vzeroupper();
        b1(0xe8);
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len, R_X64_PLT32, i->sym, -4);
        b4(0);
        return;
    case X_CALLR:
        if (uses_ymm && !i->cc)
            vzeroupper(); /* (the callee's address is in a general register or the frame) */
        op_loc(0xff, 0, 2, loc_of(i->s0));
        return;
    case X_RET:
        if (uses_ymm && !i->cc)
            vzeroupper();
        epilogue();
        return;
    case X_ALLOCA:
        if (i->imm == 1) {
            dr = dst(i->d, SCR0);
            mov_rr(dr, RSP);
            fin(i->d, dr);
        } else if (i->imm == 2) {
            mov_loc(RSP, loc_of(i->s0));
        } else {
            dr = dst(i->d, SCR0);
            mov_loc(SCR0, loc_of(i->s0));
            alu_ri(0, 1, SCR0, 15);
            alu_ri(4, 1, SCR0, -16);
            op_rr(0x2b, 1, RSP, SCR0);
            ea_base(&ea, RSP, (i64)align_up(outargs, 16));
            op_mem(0x8d, 1, dr, &ea, 0);
            fin(i->d, dr);
        }
        return;
    case X_FRAMEADDR:
        dr = dst(i->d, SCR0);
        if (i->imm == 0) {
            mov_rr(dr, RBP);
        } else if (i->imm == 1) {
            ea_base(&ea, RBP, 8);
            load_reg(dr, &ea);
        } else if (i->imm == 3) {
            mov_rr(dr, RSP);
        } else {
            op_prefix(0x8d, 1, dr, NOREG, NOREG); /* lea dr, [rip - 7]: this instruction */
            b1((dr & 7) << 3 | 5);
            b4(0xfffffff9U);
        }
        fin(i->d, dr);
        return;
    case X_TLS:
        /* the thread pointer is at fs:0; the thread's copy of the object lies a fixed way below it */
        dr = dst(i->d, SCR0);
        ea_base(&ea, NOREG, 0);
        ea.fs = 1;
        op_mem(0x8b, 1, dr, &ea, 0);
        ea_base(&ea, dr, 0);
        ea.force32 = 1;
        op_mem(0x8d, 1, dr, &ea, 0);
        obj_add_reloc(obj, arm64_text_sec, (u32)text->len - 4, R_X64_TPOFF32, i->sym, i->imm);
        fin(i->d, dr);
        return;
    case X_ATOMIC: {
        u32 code = (u32)i->imm, kind = code & 255, lg = code >> 8 & 3;
        u32 pre = lg == 1 ? O_66 : 0, bop = lg == 0 ? O_B8R : 0;
        int w8 = lg == 3;
        ea_base(&ea, RDI, 0);
        switch (kind) {
        case ATOM_XCHG:
            op_mem(pre | bop | (lg == 0 ? 0x86 : 0x87), w8, RSI, &ea, 0);
            mov_rr(RAX, RSI);
            break;
        case ATOM_SUB:
            op_rr(0xf7, 1, 3, RSI);
            /* fall through */
        case ATOM_ADD:
            b1(0xf0);
            op_mem(pre | bop | O_0F | (lg == 0 ? 0xc0 : 0xc1), w8, RSI, &ea, 0); /* lock xadd */
            mov_rr(RAX, RSI);
            break;
        case ATOM_CAS:
            b1(0xf0);
            op_mem(pre | bop | O_0F | (lg == 0 ? 0xb0 : 0xb1), w8, RDX, &ea, 0); /* lock cmpxchg */
            break;
        default: {
            u32 top;
            op_mem(lg == 0 ? O_0F | 0xb6 : lg == 1 ? O_0F | 0xb7 : 0x8b, w8, RAX, &ea, 0);
            top = (u32)text->len;
            mov_rr(SCR0, RAX);
            op_rr(kind == ATOM_OR ? 0x0b : kind == ATOM_XOR ? 0x33 : 0x23, 1, SCR0, RSI);
            if (kind == ATOM_NAND)
                op_rr(0xf7, 1, 2, SCR0);
            b1(0xf0);
            op_mem(pre | bop | O_0F | (lg == 0 ? 0xb0 : 0xb1), w8, SCR0, &ea, 0);
            b1(0x70 | CC_NE);
            b1(top - (u32)text->len - 1);
        }
        }
        /* the old value, zero-extended from its size */
        if (lg == 0)
            op_rr(O_0F | O_B8M | 0xb6, 0, RAX, RAX);
        else if (lg == 1)
            op_rr(O_0F | 0xb7, 0, RAX, RAX);
        else if (lg == 2)
            op_rr(0x8b, 0, RAX, RAX);
        return;
    }
    case X_MFENCE:
        b1(0x0f);
        b1(0xae);
        b1(0xf0);
        return;
    case X_UD2:
        b1(0x0f);
        b1(0x0b);
        return;
    case X_VASAVE: {
        i64 base = f->slots[i->sym].off;
        for (n = 0; n < 6; n++) {
            ea_frame(&ea, base + 8 * (i64)n);
            store_reg(arg_gr[n], &ea);
        }
        op_rr(O_B8R | O_B8M | 0x84, 0, RAX, RAX); /* test al, al: were any xmm registers used? */
        jf(0x70 | CC_E);
        for (n = 0; n < 8; n++) {
            ea_frame(&ea, base + 48 + 16 * (i64)n);
            op_mem(O_0F | 0x29, 0, XMM0 + n, &ea, 0); /* movaps */
        }
        jf_here();
        return;
    }
    case X_ASM:
        asm_emit(i);
        return;
    }
    die("internal: x86-64 encoding: op %d", i->op);
}

/* The register of each operand of an inline assembly statement. Nothing
   is shared, so an output may be written before the inputs are read. */
static void asm_assign(const AsmStmt *st, u8 *regs)
{
    static const u8 pool[] = { RDI, RSI, R8, R9, RCX, RDX, RAX, RBX, R12, R13, R14, R15 };
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
        emit(X_UD2, 0, NOREG, NOREG, NOREG);
        for (o = 1; o < f->nins; o++)
            if (f->ins[o].op == OP_ASMOUT && f->ins[o].blk && f->ins[o].a == v && uses[o]) {
                int fl = f->ins[o].ty == IT_F32 || f->ins[o].ty == IT_F64;
                m = emit(fl ? X_FMOVI : X_MOVI, f->ins[o].ty == IT_I64 || f->ins[o].ty == IT_F64, VREG0 + o, NOREG, NOREG);
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
    m = emit(X_ASM, 0, NOREG, NOREG, NOREG);
    m->imm = f->ins[i->c].a;
    m->pmask = pmask;
    has_calls = 1; /* it may push or call: a frame, and the stack aligned */
    asm_saved |= defs;
    for (k = 0; k < 48; k++)
        if ((defs >> k & 1) && ((target.allocatable[0] | target.allocatable[1]) >> k & 1))
            emit(X_DEFREG, 1, k, NOREG, NOREG);
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
            emit(X_MOV, 1, VREG0 + o, regs[f->ins[o].b], NOREG);
}

/* the name of general register r as an operand of `size` bytes (0: the high byte of a, b, c, d) */
static void asm_regname(char *to, u32 r, u32 size)
{
    static const char *const low[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
    if (r >= 8)
        sprintf(to, "%%r%u%s", r, size == 8 ? "" : size == 4 ? "d" : size == 2 ? "w" : "b");
    else if (size == 0 && r < 4)
        sprintf(to, "%%%ch", low[r][0]);
    else if (size <= 1 && r < 4)
        sprintf(to, "%%%cl", low[r][0]);
    else if (size <= 1)
        sprintf(to, "%%%sl", low[r]);
    else
        sprintf(to, "%%%s%s", size == 8 ? "r" : size == 4 ? "e" : "", low[r]);
}

/* Puts the operands into the text of an inline assembly statement and assembles it here. */
static void asm_emit(MIns *i)
{
    const AsmStmt *st = &asm_stmts[i->imm];
    u8 regs[32];
    Buf b;
    const char *t, *file;
    char tmp[24];
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
            /* %c0 and %P0: the bare number or name */
            if (mod != 'c' && mod != 'P' && mod != 'p')
                buf_u8(&b, '$');
            buf_put(&b, op->text, strlen(op->text));
        } else if (op->kind == 'm' || mod == 'a') {
            asm_regname(tmp + 1, regs[k], 8);
            tmp[0] = '(';
            strcat(tmp, ")");
            buf_put(&b, tmp, strlen(tmp));
        } else {
            u32 size = mod == 'b' ? 1 : mod == 'h' ? 0 : mod == 'w' ? 2 : mod == 'k' ? 4 : mod == 'q' ? 8 : op->cls;
            asm_regname(tmp, regs[k], size);
            buf_put(&b, tmp, strlen(tmp));
        }
    }
    if (opt_asm_text) {
        /* -S: the text is what was asked for */
        buf_put(opt_asm_text, b.p, b.len);
        buf_u8(opt_asm_text, '\n');
    } else {
        gas_text((char *)b.p, b.len, arm64_text_sec, file, line, 0);
    }
    buf_free(&b);
    text = &obj->sec[arm64_text_sec]; /* new sections may have moved the table */
}

static u32 fx_start(const Fixup *fx)
{
    return fx->off - (fx->kind == FK_JMP ? 1 : 2);
}

static u32 fx_long(const Fixup *fx)
{
    return fx->kind == FK_JMP ? 5 : 6;
}

static u32 fx_len(const Fixup *fx)
{
    return fx->state == 2 ? 0 : fx->state == 1 ? 2 : fx_long(fx);
}

/* The jumps between blocks were all written in their long forms. Here "jcc A; jmp B; A:" becomes
   "jncc B" (when the two are in one block), jumps to the next instruction go, and the rest become short where the distance allows; the
   code moves together and the relocations made since first_rel move with it. (Not with inline assembly
   in the function: the assembler keeps places in its text.) */
static void relax(u32 *blk_off, u32 first_rel, int fixed)
{
    u32 k, bi, removed = 0, src, dst, *cut, *nb;
    int changed;

    if (fixed) {
        for (k = 0; k < nfixups; k++)
            put_u32le(text->p + fixups[k].off, blk_off[fixups[k].target] - (fixups[k].off + 4));
        return;
    }
    for (k = 0, bi = 0; k + 1 < nfixups; k++) {
        Fixup *a = &fixups[k], *b = &fixups[k + 1];
        while (bi < mf.nblk && blk_off[bi] < b->off - 1)
            bi++;
        if (bi < mf.nblk && blk_off[bi] == b->off - 1)
            continue; /* the second jump starts a block: others come to it */
        if (a->kind == FK_JCC && b->kind == FK_JMP && a->off + 5 == b->off && blk_off[a->target] == b->off + 4) {
            text->p[a->off - 1] ^= 1;
            a->target = b->target;
            b->state = 2;
        }
    }
    cut = (u32 *)xmalloc((nfixups + 1) * sizeof(u32)); /* bytes taken out before each jump */
    nb = (u32 *)xmalloc((mf.nblk + 1) * sizeof(u32));  /* where the blocks start now */
    do {
        changed = 0;
        removed = 0;
        bi = 0;
        for (k = 0; k <= nfixups; k++) {
            u32 st = k < nfixups ? fx_start(&fixups[k]) : 0xffffffffU;
            while (bi < mf.nblk && blk_off[bi] <= st) {
                nb[bi] = blk_off[bi] - removed;
                bi++;
            }
            cut[k] = removed;
            if (k < nfixups)
                removed += fx_long(&fixups[k]) - fx_len(&fixups[k]);
        }
        for (k = 0; k < nfixups; k++) {
            Fixup *fx = &fixups[k];
            u32 at = fx_start(fx) - cut[k];
            i64 d;
            if (fx->state == 2)
                continue;
            d = (i64)nb[fx->target] - (i64)(at + 2);
            if (nb[fx->target] > at)
                d -= fx_len(fx) - 2; /* what follows comes nearer when this gets short */
            if (fx->kind != FK_JCC_JP && nb[fx->target] == at + fx_len(fx)) {
                fx->state = 2;
                changed = 1;
            } else if (fx->state == 0 && d >= -128 && d <= 127) {
                fx->state = 1;
                changed = 1;
            }
        }
    } while (changed);
    src = dst = nfixups ? fx_start(&fixups[0]) : (u32)text->len;
    for (k = 0; k < nfixups; k++) {
        Fixup *fx = &fixups[k];
        u32 st = fx_start(fx), op = text->p[fx->off - 1];
        memmove(text->p + dst, text->p + src, st - src);
        dst += st - src;
        src = st + fx_long(fx);
        if (fx->state == 1) {
            text->p[dst] = (u8)(fx->kind == FK_JMP ? 0xeb : 0x70 | (op & 15));
            text->p[dst + 1] = (u8)(nb[fx->target] - (dst + 2));
            if (fx->kind == FK_JCC_JP)
                text->p[dst - 1] = 2;
            dst += 2;
        } else if (fx->state == 0) {
            if (fx->kind != FK_JMP)
                text->p[dst++] = 0x0f;
            text->p[dst++] = (u8)op;
            put_u32le(text->p + dst, nb[fx->target] - (dst + 4));
            dst += 4;
        }
    }
    memmove(text->p + dst, text->p + src, text->len - src);
    text->len -= src - dst;
    for (k = first_rel, bi = 0; k < obj->nrel[arm64_text_sec]; k++) {
        ObjReloc *r = &obj->rel[arm64_text_sec][k];
        while (bi < nfixups && fx_start(&fixups[bi]) < r->off)
            bi++;
        r->off -= cut[bi];
    }
    for (k = 0, bi = 0; k < nrow; k++) {
        while (bi < nfixups && fx_start(&fixups[bi]) < row_off[k])
            bi++;
        row_off[k] -= cut[bi];
    }
    for (k = 0, bi = 0; k < ncfi; k++) {
        while (bi < nfixups && fx_start(&fixups[bi]) < cfi[k].off)
            bi++;
        cfi[k].off -= cut[bi];
    }
    if (opt_debug)
        for (k = 0, bi = 0; k <= mf.n; k++) {
            while (bi < nfixups && fx_start(&fixups[bi]) < moff[k])
                bi++;
            moff[k] -= cut[bi];
        }
    free(cut);
    free(nb);
}

/* -g: a DWARF expression for where machine location l (a register or a spill slot) is; byref: the variable
   is in memory at the address held there */
static u32 place_expr(u8 *e, u32 l, int byref)
{
    u32 n = 0, dw;
    i64 off;
    if (l >= SPILL0) {
        off = -(i64)(spill_base + 8 * (l - SPILL0 + 1)) - 16; /* rbp is the frame's address - 16 */
        e[n++] = 0x91;                                        /* DW_OP_fbreg */
        do {
            u32 byte = (u32)(off & 0x7f);
            off >>= 7;
            if ((off == 0 && !(byte & 0x40)) || (off == -1 && (byte & 0x40))) {
                e[n++] = (u8)byte;
                break;
            }
            e[n++] = (u8)(byte | 0x80);
        } while (1);
        if (byref)
            e[n++] = 0x06; /* DW_OP_deref */
        return n;
    }
    if (l < 16) {
        static const u8 dwreg[16] = {0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
        dw = dwreg[l];
    } else if (l >= XMM0 && l < XMM0 + 16) {
        dw = 17 + l - XMM0;
    } else {
        return 0;
    }
    if (byref) {
        e[n++] = (u8)(0x70 + dw); /* DW_OP_bregN 0 */
        e[n++] = 0;
    } else {
        e[n++] = (u8)(0x50 + dw); /* DW_OP_regN */
    }
    return n;
}

/* -g: where the function's variables are. Those still in a stack slot are there throughout; for the others,
   each value that was the variable's (opt_promote marked them) is where the allocator put it, wherever it is
   live. Positions of the allocator are turned into code offsets through the instructions' offsets. */
static void var_places(u32 start)
{
    u32 k, v, b, i, nslot = mf.n + mf.nblk + 1, *ss, *se, *byslot, cap = 64, *rs, *re;
    if (!dwarf_vars(f->sym))
        return;
    byslot = (u32 *)xmalloc((f->nslots + 1) * sizeof(u32));
    memset(byslot, 0, (f->nslots + 1) * sizeof(u32));
    for (k = dwarf_vars(f->sym); k; k = dwarf_var_next(k)) {
        u32 sl = dwarf_var_slot(k);
        if (sl >= f->nslots)
            continue;
        byslot[sl] = k;
        if (f->slots[sl].size && has_frame)
            dwarf_var_frame(k, f->slots[sl].off - 16);
    }
    ss = (u32 *)xmalloc(nslot * sizeof(u32));
    se = (u32 *)xmalloc(nslot * sizeof(u32));
    memset(ss, 0, nslot * sizeof(u32));
    memset(se, 0, nslot * sizeof(u32));
    for (b = 0; b < mf.nblk; b++) {
        MBlock *mb = &mf.blk[b];
        for (i = mb->first; i < mb->first + mb->n; i++) {
            ss[i + b] = moff[i] - start;
            se[i + b] = moff[i + 1] - start;
        }
        ss[mb->first + mb->n + b] = se[mb->first + mb->n + b] = moff[mb->first + mb->n] - start;
    }
    rs = (u32 *)xmalloc(cap * sizeof(u32));
    re = (u32 *)xmalloc(cap * sizeof(u32));
    for (v = 1; v < f->nins; v++) {
        Ins *in = &f->ins[v];
        u32 d, l, n, len, sl;
        u8 e[12];
        if (!in->var || !in->blk || (sl = in->var - 1) >= f->nslots || !byslot[sl])
            continue;
        d = ra_dense(v);
        if (d == NOREG || (l = mf.loc[d]) == NOREG)
            continue;
        len = place_expr(e, l, dwarf_var_byref(byslot[sl]));
        if (!len)
            continue;
        n = ra_ranges(d, rs, re, cap);
        if (n > cap) {
            cap = n;
            rs = (u32 *)xrealloc(rs, cap * sizeof(u32));
            re = (u32 *)xrealloc(re, cap * sizeof(u32));
            ra_ranges(d, rs, re, cap);
        }
        for (i = 0; i < n; i++) {
            /* a position: 2s + 2 reads in slot s, 2s + 3 writes there (so the value is there after it) */
            u32 a = rs[i] & 1 ? (rs[i] - 1) / 2 : (rs[i] - 2) / 2, z = re[i] < 2 ? 0 : (re[i] - 2) / 2;
            if (a >= nslot - 1 || z >= nslot - 1 || z < a)
                continue;
            dwarf_var_place(byslot[sl], ss[a], se[z], e, len);
        }
    }
    free(rs);
    free(re);
    free(ss);
    free(se);
    free(byslot);
}

void x64_gen(Func *fn, Obj *o)
{
    u32 k, n, off, start, nsaved, first_rel;
    u32 *blk_off, saved_loc = ir_loc, last_loc = 0, body_at = 0;
    int has_asm = 0;

    ir_loc = 0;
    opt_finish(fn);
    f = fn;
    obj = o;
    fn_serial++;
    if (getenv("MCC_DUMPIR") && strstr(intern_str(o->syms[fn->sym].name), getenv("MCC_DUMPIR")))
        ir_dump(fn); /* (debugging: the IR a function is selected from; --ir shows it before the whole-unit passes) */
    sel_loc = opt_debug ? dwarf_get_func_loc(f->sym) : 0;
    prepare();
    select_all();
    if (opt_nofpo < 0)
        opt_nofpo = getenv("MCC_NOFPO") != 0;
    fpo = !opt_nofpo && opt_level >= 1 && !uses_fp && !uses_alloca;
    for (k = 0; k < mf.n && fpo; k++)
        if (mf.ins[k].op == X_ASM || mf.ins[k].op == X_FRAMEADDR || mf.ins[k].op == X_ALLOCA)
            fpo = 0;
    if (fpo) {
        MTarget t = target;
        t.allocatable[0] |= BIT(RBP);
        ra_run(&mf, &t);
    } else {
        ra_run(&mf, &target);
    }

    /* The frame, from rbp downwards: saved registers, spills, slots; the
       outgoing arguments are at its bottom, where rsp points. */
    saved_regs = (mf.used | (asm_saved & target.allocatable[0])) & ~target.caller_saved & 0xffff;
    nsaved = 0;
    for (k = 0; k < 16; k++)
        nsaved += (u32)(saved_regs >> k & 1);
    spill_base = 8 * nsaved;
    /* spill slots: 8 bytes, or the width of a vector (16-aligned below rbp) */
    spill_off = (u32 *)xrealloc(spill_off, (mf.nspill + 1) * sizeof(u32));
    for (k = 0; k < mf.nspill; k++)
        spill_off[k] = 8;
    uses_ymm = 0;
    for (k = 0; k < mf.nvreg; k++) {
        u32 l = mf.loc[k];
        int vw = (mf.vclass[k] & 32) ? 64 : (mf.vclass[k] & 8) ? 32 : mf.vclass[k] & VC_V128 ? 16 : 0;
        if (vw >= 32)
            uses_ymm = 1;
        if (vw && l != NOREG && l >= SPILL0)
            spill_off[l - SPILL0] = (u32)vw;
    }
    off = spill_base;
    for (k = 0; k < mf.nspill; k++) {
        u32 sz = spill_off[k];
        off = (u32)align_up(off + sz, sz > 8 ? 16 : 8); /* (vmovups: no more is needed) */
        spill_off[k] = off;
    }
    for (k = 0; k < f->nslots; k++) {
        Slot *s = &f->slots[k];
        u32 al = s->align ? s->align : 1;
        if (!s->size)
            continue;
        if (al > 16)
            al = 16;
        off = (u32)align_up(off + s->size, al);
        s->off = -(i32)off;
    }
    has_frame = off || outargs || has_calls || uses_args || uses_fp || uses_alloca;
    frame_size = (u32)align_up(off + outargs, 16);

    text = &o->sec[arm64_text_sec];
    while (text->len & 15)
        b1(0x90);
    if (o->align[arm64_text_sec] < 16)
        o->align[arm64_text_sec] = 16;
    start = (u32)text->len;
    o->syms[f->sym].sec = (u16)arm64_text_sec;
    o->syms[f->sym].kind = 1;
    o->syms[f->sym].value = start;

    first_rel = o->nrel[arm64_text_sec];
    nrow = ncfi = 0;
    if (opt_debug && dwarf_get_func_loc(f->sym)) {
        last_loc = dwarf_get_func_loc(f->sym);
        row_add(last_loc);
    }
    prologue();
    body_at = (u32)text->len;
    blk_off = (u32 *)xmalloc((mf.nblk + 1) * sizeof(u32));
    nfixups = 0;
    if (opt_debug && capmoff < mf.n + 1) {
        capmoff = mf.n + 1;
        moff = (u32 *)xrealloc(moff, capmoff * sizeof(u32));
    }
    for (k = 0; k < mf.nblk; k++) {
        MBlock *mb = &mf.blk[k];
        blk_off[k] = (u32)text->len;
        sp_loc = NOREG; /* a branch may come here */
        for (n = mb->first; n < mb->first + mb->n; n++) {
            if (mf.ins[n].op == X_ASM)
                has_asm = 1;
            if (opt_debug) {
                moff[n] = (u32)text->len;
                if (mloc[n] && mloc[n] != last_loc) {
                    last_loc = mloc[n];
                    row_add(last_loc);
                }
            }
            ins_start = (u32)text->len;
            encode(&mf.ins[n]);
            text = &o->sec[arm64_text_sec];
        }
    }
    if (opt_debug)
        moff[mf.n] = (u32)text->len;
    relax(blk_off, first_rel, has_asm);
    o->syms[f->sym].size = text->len - start;
    if (opt_debug)
        var_places(start);
    if (opt_debug) {
        for (k = 0; k < nrow; k++)
            row_off[k] -= start;
        for (k = 0; k < ncfi; k++)
            cfi[k].off -= start;
        /* (the first instruction after the prologue: the prologue comes before any jump, so relax left it be) */
        dwarf_func(f->sym, (u32)(text->len - start), row_off, row_loc, nrow, cfi, ncfi, has_frame ? body_at - start : 0);
    }
    ir_loc = saved_loc;
    free(blk_off);
    free(layout);
    free(blk_index);
    free(uses);
    free(vuses);
    free(fused);
    free(cache_reg);
    free(cache_blk);
}
