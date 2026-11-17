/* mcc intermediate representation: target-independent SSA. */
#ifndef IR_H
#define IR_H

#include "mcc.h"

/* Value types. Narrow integers exist only in memory accesses. */
enum { IT_VOID, IT_I32, IT_I64, IT_F32, IT_F64,
       IT_V128, /* 128-bit vector; the lane kind is in Ins.mem (opt_vec.c; x86-64 intrinsics and vector types) */
       IT_V256, /* 256-bit vector (x86-64 AVX only) */
       IT_V512  /* 512-bit vector (x86-64 AVX-512 only) */ };
#define IT_ISVEC(t) ((t) == IT_V128 || (t) == IT_V256 || (t) == IT_V512)

/* Memory access kinds for OP_LOAD / OP_STORE. */
enum { M_8S, M_8U, M_16S, M_16U, M_32, M_64, M_F32, M_F64,
       M_V128, M_V256, M_V512 /* a whole vector (x86-64): the value is IT_V128 / IT_V256 / IT_V512; may alias anything */ };
#define M_SIZE(m) ((m) <= M_8U ? 1u : (m) <= M_16U ? 2u : (m) == M_32 || (m) == M_F32 ? 4u : (m) == M_V128 ? 16u : (m) == M_V256 ? 32u : \
                   (m) == M_V512 ? 64u : 8u)

enum {
    OP_NOP,
    /* floating values: not placed in any block, materialised at each use */
    OP_CONST,  /* integer; value = a | b << 32 (I32 kept sign-extended) */
    OP_FCONST, /* IEEE bits in a | b << 32 */
    OP_ADDR,   /* address of object symbol a, plus offset (i32) b */
    OP_SLOT,   /* address of stack slot a, plus offset (i32) b */
    /* placed values */
    OP_PARAM, /* a = parameter ordinal */
    OP_COPY,  /* a; left behind when a value is replaced */
    OP_PHI,   /* xargs[a .. a+b), one per predecessor */
    OP_ADD, OP_SUB, OP_MUL, OP_SDIV, OP_UDIV, OP_SREM, OP_UREM,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_SAR,
    OP_NEG, OP_NOT,
    OP_EQ, OP_NE, OP_SLT, OP_SLE, OP_SGT, OP_SGE, OP_ULT, OP_ULE, OP_UGT, OP_UGE,
    OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV, OP_FNEG,
    OP_FEQ, OP_FNE, OP_FLT, OP_FLE, OP_FGT, OP_FGE,
    OP_SEXT8, OP_SEXT16, OP_ZEXT8, OP_ZEXT16, /* within the value's own type */
    OP_SEXT32, OP_ZEXT32,                     /* i32 -> i64 */
    OP_TRUNC,                                 /* i64 -> i32 */
    OP_SITOF, OP_UITOF, OP_FTOSI, OP_FTOUI,
    OP_FEXT, OP_FTRUNC, /* f32 -> f64, f64 -> f32 */
    OP_UMULH, OP_SMULH, /* the high 64 bits of a 128-bit product */
    OP_ISCONST,         /* 1 if a is a constant once inlining is done, else 0 (__builtin_constant_p) */
    OP_CLZ, OP_CTZ, OP_BSWAP, OP_POPCNT, /* of a, in the width of the type; clz and ctz of 0 are not defined */
    OP_BITCAST, /* the bits of a as a value of this type: i32 <-> f32, i64 <-> f64 (device code only) */
    OP_TLS,     /* a = the address (OP_ADDR) of a thread-local object as if it were an ordinary one; the
                   value is where the running thread's copy is */
    OP_ASMOUT,  /* a = an inline assembly call; b (a number) = the operand of it whose register this reads */
    OP_LOAD,    /* a = address; mem = access kind */
    OP_STORE,   /* a = address, b = value; mem = access kind */
    OP_CALL,    /* c = callee, args xargs[a .. a+b), xargs[a-1] = fixed arg count or ~0; with IF_SRET, mem = n if
                   the n-byte result comes back in x0 and x1 (and is stored to argument 0, which has room for 16) */
    OP_MEMCPY,  /* a = dst, b = src, c = byte count */
    OP_MEMZERO, /* a = dst, c = byte count */
    OP_VASTART, /* address of the first variadic argument on the stack; a (a number) = 1, 2: the end of the
                   saved general, floating argument registers (Linux) */
    OP_SELECT,  /* a != 0 ? b : c */
    OP_FRAMEADDR, /* a (a number, not a value): 0 the frame address, 1 the return address, 2 the address of this instruction */
    OP_PIN,     /* the constant or address a, held in a register (placed before a loop) */
    OP_BYVAL,   /* x86-64: the address a, which as an argument of a call stands for the b bytes there (b and
                   the alignment c are numbers), passed on the stack; anywhere else it is just a */
    /* vectors (opt_vec.c); mem = lane kind. Arithmetic on vectors uses the
       scalar opcodes with ty = IT_V128. */
    OP_VLOAD,  /* address a + b */
    OP_VSTORE, /* b = vector, address a + c */
    OP_VSPLAT, /* the scalar a in every lane */
    OP_VSUM,   /* the lanes of a combined into a scalar; flags: 0 sum, 1-4 as the next four, 5/6 sum of unsigned/signed lanes into a wider total, 7/8/9 or / and / xor (x86-64) */
    OP_VSMAX, OP_VSMIN, OP_VUMAX, OP_VUMIN, /* lane by lane */
    OP_VIOTA, /* 0, a, 2a, ... in the lanes; a is a number, not a value */
    OP_VGATHER, /* lanes of four bytes: lane k = the element (kind in flags) at a + (b's lane k, sign-extended) << c (a number);
                   x86-64 only (opt_vec.c, with SSE4.1) */
    OP_VMATH, /* lane by lane, b (a number, VM_*) of a: x86-64 only (opt_vec.c makes it from calls of sqrtf and the like) */
    OP_XI,    /* x86-64 intrinsic: operands xargs[a .. a+b), c (a number) = its index in x86i_tab (x86i.h).
                 Reads memory when the table says so (gathers); never writes it (stores are OP_STORE) */
    OP_COUNT
};

enum { VM_SQRT, VM_FLOOR, VM_CEIL, VM_TRUNC, VM_RINT, VM_NEARBYINT };

#define XI_EFFECT 1 /* OP_XI's flags: it has an effect besides its value (a fence, a prefetch): it stays where it is */
#define PARAM_GLOBAL 1 /* OP_PARAM's mem: a kernel's pointer, the address of global memory */
#define PARAM_GLOBAL_NC 2 /* ... a "const T *__restrict__" one: memory the kernel only reads (ld.global.nc) */
#define PARAM_PTR 1      /* OP_PARAM's flags, x86-64 host: the parameter is a pointer (opt.c: the root of the addresses made from it) */
#define PARAM_RESTRICT 2 /* ... declared restrict: what is written through it is reached only through it */
#define IF_NSW 1 /* OP_ADD, OP_SUB of I32, x86-64 host: where this stands, a signed int sum of the program's was worked out,
                    so it did not wrap there. Set by the parser on a sum it made new; gone as soon as the instruction
                    is taken out of its block (ir_remove: moved, perhaps to where the program did not add) */
#define IF_SRET 1 /* OP_CALL: first argument is the struct return pointer */
#define IF_ALLOCA 4 /* OP_CALL: the one argument is a byte count, the result that much new stack */
#define IF_ATOMIC 8 /* OP_CALL: an atomic operation done in line. The callee is a constant: ATOM_* kind,
                       log2 of the size in bits 8-9, bit 12 for relaxed ordering (else sequentially
                       consistent). Arguments: the address, then the operand, then (ATOM_CAS, whose
                       operand is the value expected) the new value. The result is the value that was
                       there before, zero-extended. */
enum { ATOM_LOAD, ATOM_STORE, ATOM_XCHG, ATOM_ADD, ATOM_SUB, ATOM_AND, ATOM_OR, ATOM_XOR, ATOM_NAND, ATOM_CAS,
       ATOM_FENCE };
#define IF_NC 0x40 /* OP_LOAD, device code: of global memory the kernel only reads (__ldg: ld.global.nc) */
#define IF_SSE0 0x10 /* OP_CALL with IF_SRET and mem, x86-64: the first eight bytes come back in an xmm register, */
#define IF_SSE1 0x20 /* the second eight; Func.has_sret says the same with 0x40 and 0x80 */
#define IF_FRESH 0x40 /* OP_CALL, x86-64 host: malloc, calloc, realloc, aligned_alloc: the result is the address of memory nothing
                         else points to */
#define IF_NOWRITE 0x10 /* OP_CALL without IF_SRET, x86-64 host: a C library function that writes no memory but errno
                           (getenv, strlen, strcmp, atof ...): what is known of locals and named globals stays known */
#define IF_PURE 0x80 /* OP_CALL, x86-64 host: a C library math function of its arguments alone (no memory read or written, no
                        errno under -fno-math-errno): gvn shares two alike */
#define IF_ASM 2  /* OP_CALL: inline assembly. The callee is the constant index of an AsmStmt; the
                     arguments are the values of its operands that are read or are addresses */

typedef struct AsmOp {
    u8 kind;          /* 'r' register, 'm' memory with its address in a register, 'i' text put in as it is */
    u8 out;           /* 0 read, 1 written, 2 both */
    u8 ty;            /* IT_* of a register operand */
    u8 cls;           /* device code: the constraint letter (r l f d h n) */
    signed char reg;  /* the register it must be in, or -1 */
    signed char tied; /* the operand whose register it shares, or -1 */
    u32 name;         /* interned [name], or 0 */
    const char *text;
} AsmOp;

typedef struct AsmStmt {
    char *tmpl;
    AsmOp *ops;
    u32 nops;
    u64 clobbers; /* registers */
    u32 loc;
    u8 dev;       /* CUDA: PTX, from a device function; where host code holds it, it traps */
} AsmStmt;

extern AsmStmt *asm_stmts; /* parse.c */

typedef struct Ins {
    u8 op;
    u8 ty;
    u8 mem;
    u8 flags;
    u32 a, b, c;
    u32 blk; /* 0: not placed */
    u32 prev, next;
    u32 aux; /* scratch for passes */
    u32 loc; /* source location it was made for (-g: line tables), 0 if none */
    u32 var; /* -g: 1 + the stack slot of the variable whose value this is (opt_promote), else 0 */
} Ins;

enum { TERM_NONE, TERM_JMP, TERM_BR, TERM_RET };

typedef struct Block {
    u32 first, last; /* instruction list */
    u32 phis;        /* phi list, linked through next */
    u32 *preds;
    u32 npreds, cappreds;
    u8 term;
    u8 flag;
    u8 lhdr;     /* the body of a for or while loop: where its phis are (see rank_all in ptx.c) */
    u8 nounroll; /* device code: the head of a loop the GPU's assembler is not to unroll (#pragma unroll 1, or the loop
                    left of #pragma unroll N: PTX .pragma "nounroll", as nvcc writes) */
    u32 arg;     /* TERM_BR: condition; TERM_RET: value or 0 */
    u32 succ[2]; /* TERM_BR: [0] taken when arg != 0 */
    u32 aux, aux2; /* scratch for passes */
    u32 freq;      /* from a profile: 0 = unknown, else 1 + execution count (scaled) */
} Block;

typedef struct Slot {
    u32 size;
    u32 align;
    i32 off; /* assigned by the backend */
} Slot;

typedef struct Func {
    Ins *ins; /* ins[0] unused: value 0 means "none" */
    u32 nins, capins;
    Block *blk; /* blk[0] unused; blk[1] is the entry */
    u32 nblk, capblk;
    u32 *xargs;
    u32 nxargs, capxargs;
    Slot *slots;
    u32 nslots, capslots;
    u32 *ctab; /* hash-consing table for floating values */
    u32 ctab_size, ctab_used;
    u32 copies; /* nonzero while OP_COPY instructions may still be referenced */
    u32 *dv;    /* -g: (value, 1 + slot) pairs beyond Ins.var: a value that is the value of several variables */
    u32 ndv, capdv;
    u32 cur;   /* block being filled; 0 = unreachable code is dropped */
    u32 sym;   /* object symbol */
    u8 *param_ty;
    u32 nparams, capparams;
    u8 ret_ty;
    u8 has_sret; /* parameter 0 is the struct return pointer; 1 + n: the caller wants the n bytes in x0 and x1 */
    u8 variadic;
    u8 avx; /* x86-64: compiled for AVX (-mavx2 ..., or target("avx2")): 128-bit vector work in VEX encoding */
    const char *name;
    Arena arena; /* predecessor lists and other per-function data */
} Func;

Func *ir_func_new(void);
void ir_rehash(Func *f);
int ir_is_bool(Func *f, u32 v, int depth); /* is v known to be 0 or 1? */
int ir_nonneg(Func *f, u32 v);              /* is the I32 v known not to be negative? */
void ir_func_reset(Func *f);
void ir_dbg_var(Func *f, u32 v, u32 var); /* -g: value v is (also) variable var's (1 + its slot) */

u32 ir_const(Func *f, int ty, u64 v);
u32 ir_fconst(Func *f, int ty, double v);
u32 ir_fconst_bits(Func *f, int ty, u64 bits);
u32 ir_addr(Func *f, u32 sym, i64 off);
u32 ir_slot_addr(Func *f, u32 slot, i64 off);
u32 ir_new_slot(Func *f, u64 size, u32 align);
u32 ir_add_param(Func *f, int ty); /* returns the OP_PARAM value */
int ptx_single_trip(Func *f); /* device code: loops that run at most once lose their way back */
extern int opt_nostrip, opt_noldcomb;

int ir_is_const(Func *f, u32 v, u64 *val); /* integer constant */
int ir_eval_with(Func *f, u32 v, u32 x, u64 k, int depth, u64 *out); /* v if x were the constant k */
u64 ir_umax32(Func *f, u32 v);             /* a bound of the 32-bit v read as unsigned (0xffffffff: none known) */
int ir_is_fconst(Func *f, u32 v, double *val);

extern u32 ir_loc; /* the source location new instructions are given: the parser's token; 0 while optimising */
u32 ir_raw(Func *f, int op, int ty, u32 a, u32 b); /* create, do not place */
void ir_append(Func *f, u32 blk, u32 v);
void ir_insert_before(Func *f, u32 pos, u32 v);
void ir_remove(Func *f, u32 v);
u32 ir_ins(Func *f, int op, int ty, u32 a, u32 b); /* fold, else create and append to f->cur */
u32 ir_load(Func *f, int ty, int mem, u32 addr);
void ir_store(Func *f, int mem, u32 addr, u32 val);
u32 ir_call(Func *f, int ty, u32 callee, const u32 *args, u32 nargs, u32 nfixed, int flags);
u32 ir_phi(Func *f, u32 blk, int ty); /* arguments filled in later */
u32 ir_xi(Func *f, int ty, u32 id, const u32 *args, u32 nargs); /* OP_XI, appended to f->cur */
u32 ir_resolve(Func *f, u32 v);       /* follow OP_COPY chains */
u32 ir_fold_at(Func *f, u32 v);       /* simplify placed instruction v; returns its replacement or 0 */
void ir_resolve_all(Func *f);         /* rewrite every operand through OP_COPY */
void ir_phi_add_arg(Func *f, u32 phi, u32 val);
void ir_replace(Func *f, u32 v, u32 with);

extern u8 ir_opcount_tab[];
/* Fast operand walk: p[0..n-1] are the operands of *i, except that a
   call also has its callee in i->c. */
#define IR_OPS(f, i, p, n) \
    do { \
        (n) = ir_opcount_tab[(i)->op]; \
        (p) = &(i)->a; \
        if ((n) == 255) { \
            (p) = (f)->xargs + (i)->a; \
            (n) = (i)->b; \
        } \
    } while (0)
u32 ir_opcount(Func *f, u32 v);         /* number of value operands */
u32 *ir_opref(Func *f, u32 v, u32 k);   /* pointer to operand k */

u32 ir_new_block(Func *f);
void ir_add_pred(Func *f, u32 blk, u32 pred);
void ir_jmp(Func *f, u32 target);
void ir_br(Func *f, u32 cond, u32 t, u32 e);
void ir_ret(Func *f, u32 val);

void ir_dump(Func *f);

/* opt_ssa.c */
void opt_promote(Func *f); /* stack slots -> SSA values */
extern u32 opt_promoted;
int opt_split(Func *f);    /* local arrays and records read and written only at constant places: one slot for each place */
void opt_dce(Func *f);
void opt_cfg_clean(Func *f);
int opt_param_fwd(Func *f);
void opt_dom(Func *fn);
int opt_dominates(u32 a, u32 b); /* device code: fields of a record parameter read straight from its doublewords */
int opt_vectorise(Func *f); /* opt_vec.c; returns the number of loops rewritten */

/* opt_inline.c */
void inline_reset(void);
extern int opt_wrapv;
extern u8 *opt_sym_private;    /* by object symbol, while the unit's functions are generated: a static object no pointer reaches */
extern u32 opt_sym_private_n; /* -fwrapv, -fno-strict-overflow */
extern int opt_inline_once;
extern int opt_inline_oncelim;
extern int opt_inline_limit, opt_inline_host;
extern int opt_inline_hint; /* -finline-hint=: the limit for functions declared inline */
extern int opt_inline_slotarg; /* -finline-slotarg=: the limit at host call sites passing the address of a local */
extern int opt_level;
extern int opt_fast_math;
extern int opt_reciprocal_math; /* -freciprocal-math (in -ffast-math) */
extern int opt_no_signed_zeros; /* -fno-signed-zeros (in -ffast-math) */
extern int opt_finite_math; /* -ffinite-math-only (in -ffast-math): no NaN or infinity reaches a comparison, so a < b ? a : b may be a min */
extern int opt_vec_report;
extern int opt_vec_gather; /* opt_vec.c: -fvec-gather */
extern int opt_glibc;      /* opt.c: x86-64 programs for the GNU C library (-mglibc / --sysroot): its functions beyond C's may be called */
extern int opt_x64_fma;   /* x64.c: FMA3 may be used (-mfma, -march=native / x86-64-v3) */
extern int opt_x64_xmm16; /* x64.c: xmm16-xmm31 may be allocated (AVX-512 F, VL, BW, DQ; MCC_NOXMM16: off) */
extern int opt_x64_bmi2;  /* x64.c: shlx / shrx / sarx for shifts by a register (BMI2; MCC_NOSHLX: off) */
extern int opt_x64_avx2;  /* opt_vec.c: 32-byte vectors in AVX functions */
extern int opt_x64_sse41; /* opt_vec.c: x86-64 vector code may use SSE4.1 */
extern Obj *opt_obj;    /* opt_vec.c: the object the functions being optimised go to (for the names of what they call) */
extern int opt_math_errno; /* x64.c: sqrt is a call, for errno (-fno-math-errno, -ffast-math: in line) */
int x64_cpu_has(const char *flag); /* main.c: the machine mcc runs on has it (/proc/cpuinfo) */
extern int opt_size; /* prefer smaller code where it costs speed */
extern int opt_fp_contract; /* -ffp-contract=fast: a * b + c is rounded once (arm64.c: fmadd) */
extern int opt_no_jump_tables; /* -fno-jump-tables: a switch stays a tree of comparisons (arm64.c) */
int inline_saved(u32 sym);
void inline_restore(Func *f, u32 sym);
void inline_save(Func *f);
int inline_calls(Func *f);
extern int opt_inline_mode;
extern int opt_novec;
extern int opt_no_fp; /* -mgeneral-regs-only: no floating point registers, not even to save them */
void func_stash(Func *f, int mode);
int func_stashed(u32 sym);
void func_unstash(Func *f, u32 sym);
void func_realise(Func *f, u32 sym);
void func_refs(u32 *calls, u8 *esc, u32 nsym); /* calls of each function put aside; esc: its address used otherwise */
void func_once(u32 sym);                         /* called from one place only: inlined there even when large */
void opt_finish(Func *f); /* last changes before code generation */

/* opt.c */
extern int arm64_text_sec;
void opt_run(Func *f); /* the whole pipeline, from freshly parsed IR */
int ptx_pure_call(Func *f, u32 v); /* ptx.c: a call that is an instruction of arithmetic in device code */
extern Obj *ptx_obj;
extern int opt_strict_aliasing;
int opt_alias_roots(void); /* opt.c: addresses are told apart by the object they are in (x86-64 host; MCC_NOALIASROOT: off) */

/* backend */
void arm64_gen(Func *f, Obj *o);
void x64_gen(Func *f, Obj *o); /* x64.c; arm64_gen hands over to it when opt_arch says so */

/* ptx.c: the backend for device code */
extern int opt_fmad; /* CUDA device code: a product and a sum become one fused operation (-fmad=false: never) */
extern int opt_noparamfwd;
extern int opt_norange, opt_nofastexp, opt_noifasm, opt_shsym, opt_nocondrec, opt_nodivcse, opt_nohoist, opt_nofastcall, opt_nolocal, opt_nored, opt_nonc, opt_nolatch, opt_nounroll, opt_unrollpeel, opt_nosplit, opt_noauto, opt_nolicm, opt_autolog;
extern u32 opt_automax, opt_aumax;
extern int opt_slotcheck, opt_norounds, opt_nosh32, opt_nochain, opt_nowide, opt_dev_parse, opt_plaindiv, opt_nominmax, opt_noasmco, opt_nopred, opt_nodist, opt_noifcall, opt_nolocal, opt_noxrem, opt_nowiden, opt_noselk, opt_noglobal, opt_premat, opt_nospecial1, opt_ifc, opt_noparm;
extern int opt_dev_code, opt_dolicm; /* the function being read is device code only; MCC_LICM: its loops are hoisted from all the same */ /* debugging: MCC_NOUNROLL, MCC_NOSPLIT in the environment */
extern int opt_dev_fn; /* the function being optimised is device code */
extern int opt_fastmath; /* CUDA device code, nvcc's -use_fast_math: single precision flushes denormal numbers to zero
                            and divides and takes square roots approximately, as the GPU's quick instructions do */
extern const char *opt_ptx_out; /* also write the module's text to this file */
extern int opt_ptx_raw;          /* --cuda-ptx-raw: the module's text plain in the object, not packed (mcuda_pack.h) */
extern const char *opt_ptx_file; /* --cuda-ptx-file: the packed text in this file (OUT.ptxz), the object names it (MPF) */
extern int opt_ptx_pack;         /* --cuda-ptx-pack=N: 0 plain, 1 MPZ, 2 MPY greedy (the default), 3 MPY optimal (mcuda_pack.h) */
void ptx_reset(void);
int ptx_active(void);
u32 ptx_module(Obj *o);
void ptx_shared(u32 sym, u64 size, u32 align, int ext);
void ptx_constant(u32 sym); /* CUDA: sym is a __constant__ object (constant memory: .const, ld.const) */
void ptx_gen(Func *f, Obj *o, int entry);
extern u32 ptx_maxntid; /* of the kernel ptx_gen is about to write: the threads of a block at most, 0: not stated */
extern u32 ptx_maxnreg; /* and its register budget a thread (.maxnreg), 0: none */
extern u32 ptx_cluster; /* and its thread-block cluster (x | y << 8 | z << 16), 0: none */
u32 ptx_next_needed(void);
void ptx_extern(u32 sym);
void ptx_finish(Obj *o);

/* lto.c */
extern int opt_prof_gen; /* count block executions */
extern int opt_prof_use; /* a profile is loaded */
extern u32 prof_max;     /* the largest block count in it */
extern int opt_inline_hot, opt_hot_shift;
void prof_begin(Obj *o);
void prof_instrument(Func *f);
void prof_end(void);
void prof_load(const char *path);
int prof_apply(Func *f, const char *path);
extern int opt_lto;             /* compile to IR, to be optimised at link time */
void lto_save(Func *f, Obj *o); /* appends f, as parsed, to o's IR section */

#endif
