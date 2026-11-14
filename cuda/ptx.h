/* The PTX engine: what ptxparse.c (text to pre-decoded instructions), ptxexec.c (the interpreter and the
   scheduler of a block's threads), ptxops.c (the instructions that are not one line of C) and fpconv.c share. */
#ifndef PTX_H
#define PTX_H
#include "mc.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MC_RN, MC_RZ, MC_RM, MC_RP, MC_RNA };

/* fpconv.c and fp_*.s */
typedef struct McFmt {
    u8 ebits, mbits;
    s16 bias;
    u8 ieee;              /* the top exponent is infinity and NaN */
    u32 maxcode, nancode; /* largest finite value, and the NaN of the format (0: it has none) */
} McFmt;
extern const McFmt mc_fmt_f16, mc_fmt_bf16, mc_fmt_tf32, mc_fmt_f32, mc_fmt_e4m3, mc_fmt_e5m2, mc_fmt_e2m1, mc_fmt_e2m3,
    mc_fmt_e3m2;
u32 mc_enc(const McFmt *f, double v, int rmode, int sat);
double mc_dec(const McFmt *f, u32 code);
u32 mc_enc_ue8m0(double v, int rmode, int sat);
double mc_dec_ue8m0(u32 code);
extern float mc_h2f_tab[65536];
void mc_fp_init(void);
double mc_rint(double d, int rmode);
u64 mc_round_begin(int rmode);
u32 mc_fbits(float f);
float mc_bitsf(u32 u);
u64 mc_dbits(double d);
double mc_bitsd(u64 u);
/* S[i][j] += A[i][k] * B[k][j] for 16 rows i (64 doubles apart in A), k < K, 8 columns j */
void mc_mma_rows(const double *A, const double *B, double *S, long K);
/* D = A * B + C of mma m16n8k16: A 16 x 16 and B 16 x 8 of f16, C 16 x 8 of f32, which becomes D; W is room
   for 384 doubles. 1, and C is not to be used, if some sum is a NaN. */
int mc_mma_f16(const void *A, const void *B, void *C, double *W);
/* the same with the sums cut as the hardware cuts them (ptxops.c, mma_hw_sum); W is room for 640 doubles. 1,
   and C is not to be used, if something in A, B or C is not finite. */
int mc_mma_f16hw(const void *A, const void *B, void *C, double *W);
/* the same for m16n8k32 of bytes: A 16 x 32, B 32 x 8, C 16 x 8 of s32; W is room for 3072 bytes; bit 0 of
   flags: A is unsigned, bit 1: B is */
void mc_mma_s8(const void *A, const void *B, void *C, void *W, long flags);
float mc_sqrtf(float x);
double mc_sqrt(double x);
u64 mc_getfpcr(void);
void mc_setfpcr(u64 v);
double mc_rintn(double x);
double mc_rintp(double x);
double mc_rintm(double x);
double mc_rintz(double x);
double mc_rinta(double x);
float mc_rintnf(float x);
float mc_rintpf(float x);
float mc_rintmf(float x);
float mc_rintzf(float x);

/* A register, a constant or a temporary of a function: 64 bits, of which an instruction uses as many as its
   type says. Integer results are stored zero-extended. */
typedef union Slot {
    u64 u;
    s64 s;
    u32 w;
    s32 i;
    float f;
    double d;
} Slot;

/* the operands of such an mma from the frames R[0..31] of a warp's threads, whose registers are the slots
   xs[0..9] (four of A, two of B, four of C), laid out as mc_mma_f16 (get16) and mc_mma_s8 (get8) want them;
   and the result C into the slots xd[0..3] */
void mc_mma_get16(Slot **R, const s32 *xs, void *A, void *B, void *C);
void mc_mma_get8(Slot **R, const s32 *xs, void *A, void *B, void *C);
void mc_mma_put(Slot **R, const s32 *xd, const void *C);

/* One decoded instruction. d, a, b, c, e index the frame: registers from 0 up, constants from -1 down.
   Some instructions use the fields for other things (branch distances, an index into McFunc.xtra). */
typedef struct Ins {
    u16 op, aux;
    s32 d, a, b, c, e;
} Ins;

/* types of instructions and registers */
enum {
    T_NONE, T_B8, T_B16, T_B32, T_B64, T_U8, T_U16, T_U32, T_U64, T_S8, T_S16, T_S32, T_S64,
    T_F16, T_F32, T_F64, T_BF16, T_F16X2, T_BF16X2, T_TF32, T_PRED, T_B128,
    T_E4M3X2, T_E5M2X2, T_E2M1X2, T_E2M3X2, T_E3M2X2, T_UE8M0X2
};

/* state spaces of memory instructions */
enum { SP_GENERIC, SP_GLOBAL, SP_SHARED, SP_LOCAL, SP_PARAM, SP_CONST };

/* values that differ between threads, blocks or launches, written into constant slots when a frame is made */
enum {
    FX_FRAME = 1, /* address of frame memory: frame + off */
    FX_PARAM,     /* address in the launch's parameter buffer */
    FX_TID_X, FX_TID_Y, FX_TID_Z, FX_NTID_X, FX_NTID_Y, FX_NTID_Z, FX_CTAID_X, FX_CTAID_Y, FX_CTAID_Z,
    FX_NCTAID_X, FX_NCTAID_Y, FX_NCTAID_Z, FX_LANEID, FX_WARPID, FX_NWARPID, FX_GRIDID, FX_SMID, FX_NSMID,
    FX_LANEMASK_EQ, FX_LANEMASK_LE, FX_LANEMASK_LT, FX_LANEMASK_GE, FX_LANEMASK_GT, FX_DYN_SMEM, FX_TOTAL_SMEM,
    FX_ZERO, FX_ONE
};

typedef struct Fix {
    s32 slot;
    u32 kind;
    u64 off;
} Fix;

typedef struct Param {
    u8 isreg, type;
    s32 slot;         /* a .reg parameter */
    u32 off;          /* a .param parameter: offset in the frame (of a .func) or in the parameter buffer (of an .entry) */
    u32 size, align;
    const char *name;
    u32 nlen;
} Param;

/* jit.c: machine code of a function. code starts with the way in, called as mc_jit_enter says; off has, for
   each instruction, where its code is (in words), with MC_JIT_NOENTER if the interpreter may not enter there. */
typedef struct McJit {
    u32 *code;
    u32 *off;
    u64 size;
} McJit;
typedef struct McJitCtx {
    u8 *sh;     /* the block's shared memory */
    u64 shsz;
    u64 n;      /* out: instructions executed */
    float *h2f; /* mc_h2f_tab */
    void *blk;  /* the Blk */
    void *thr;  /* the Thr */
    /* A warp's threads in a row. The code starts with the frame it is given, which is R[0], and when it comes
       to a warp instruction (OP_WSYNC) it notes its index in ex[lane], adds one to lane, and if that is still
       below nl goes on from where it started, with the frame R[lane] and thr = thrs[lane]. When it leaves for
       another reason, lane says for which thread. A single thread: lane = 0, nl = 1. */
    u64 lane, nl;
    Slot *R[32];
    void *thrs[32];
    u32 ex[32];
} McJitCtx;
#define MC_JIT_NOENTER 0x80000000U
#define MC_JIT_FAULT 0x40000000U /* in the result of the code: the instruction of that index has failed, mc_fault() is done */
/* run from the code at word `at` until an instruction that the interpreter has to do; its index is the result
   (ctx->lane and ctx->nl have to be set) */
#define mc_jit_enter(jt, R, ctx, at) ((*(u32 (*)(Slot *, McJitCtx *, u32 *))(jt)->code)(R, ctx, (jt)->code + (at)))

typedef struct Gvar {
    struct Gvar *next;
    char *name;
    u8 space;   /* SP_GLOBAL, SP_CONST, SP_SHARED */
    u8 is_extern, owned, type;
    u64 size, align, nelem;
    u8 *addr;   /* host storage of a global */
    u64 off;    /* place of a shared variable in a block's shared memory */
    u32 stamp;
    const char *init; /* text of the initialiser, read when every variable has its address */
    u32 init_line;
} Gvar;

struct McFunc {
    struct McFunc *next;
    McModule *mod;
    char *name;
    u8 is_entry, has_body, decoded, decoding, builtin, noreturn, addr_taken, is_extern;
    u32 nformal; /* registers that are parameters or return values */
    struct McFunc *alias; /* an extern function's definition in another module */
    u64 st_launches, st_blocks, st_instr, st_usec, st_wsync, st_wusec; /* MCUDA_STATS */
    const char *body; /* just after the opening brace */
    u32 body_line;
    Param *params, *rets;
    int nparams, nrets;
    u64 param_bytes;  /* an entry's parameter buffer */
    u32 nreg, nconst; /* slots */
    Slot *ctemplate;  /* constants in frame order: ctemplate[nconst - 1 - i] is slot -1 - i */
    Fix *fix;
    u32 nfix;
    u32 mem_bytes, formal_mem; /* frame memory behind the registers; the part of it that holds .param formals */
    u32 frame_bytes;
    Ins *code;
    u32 *lines;
    u32 ncode;
    s32 *xtra;
    u32 nxtra;
    u32 maxntid[3], reqntid[3];
    int uses_sync;
    Gvar **srefs; /* the .shared variables it names */
    struct McFunc **callees;
    u32 nsrefs, ncallees, stamp;
    u64 shared_used;
    struct McJit *jit; /* its translation to machine code, if there is one */
};

struct McModule {
    const char *ptx;
    char *name;
    McFunc *funcs, *lastfunc;
    Gvar *gvars, *lastgvar;
    u64 shared_static; /* bytes of the .shared variables; the dynamic part of a block's shared memory follows */
    void *symtab;      /* module-level names */
    void *arena;
    int ver, sm, inited;
    struct McModule *next;
};

/* thread states */
enum { TS_RUN, TS_BAR, TS_WSYNC, TS_EXIT };

typedef struct CallRec {
    Ins *ip; /* the call instruction */
    Slot *R;
    McFunc *f;
    u8 *sp;
} CallRec;

typedef struct Thr {
    Ins *ip;
    Slot *R;
    McFunc *f;
    u32 state, tid, lane, warp, cf, bar;
    u32 xyz[3];
    CallRec *cs;
    u32 ncs, capcs;
    u8 *stk, *sp, *stk_end;
    Slot *R0; /* the entry frame */
} Thr;

typedef struct Bar {
    u32 arrived, need, nwait;
} Bar;

typedef struct Launch {
    McFunc *f;
    u32 grid[3], block[3];
    u64 dyn_shared, shared_bytes;
    u8 *params;
} Launch;

/* Everything a host thread needs to run blocks of a launch, one at a time. */
typedef struct Blk {
    Launch *L;
    Thr *thr;
    u32 nthr, nwarp, live;
    u32 cta[3];
    u8 *frames;
    u8 *shared;
    u64 shared_size; /* static + dynamic, the range that addresses of the shared space are taken to lie in */
    Bar bars[16];
    u64 icount;
    u64 wsync_n, wsync_usec; /* MCUDA_STATS: warp-synchronous operations and the time in them, in mc_ticks() */
    int failed;
    Thr *cur;
    char msg[900];
} Blk;

/* run results */
enum { RUN_PARK, RUN_EXIT, RUN_FAULT };

/* ptxparse.c */
int mc_func_decode(McFunc *f); /* 0, or -1 with mc_error() set */
int mc_module_init_data(McModule *m);
void mc_set_error(const char *msg);
int mc_tbits(int t);

/* what translated code calls (ptxexec.c) */
u64 mc_jh_prmt(u64 a, u64 b, u64 c, u64 mode);
u64 mc_jh_fgen(u64 sub, u64 flags, u64 a, u64 b, u64 c);

/* jit.c */
/* the processor's counter and how much it counts in a second (fp_*.s) */
u64 mc_ticks(void);
u64 mc_tickhz(void);
void mc_jit_compile(McFunc *f);
void mc_jit_free(McFunc *f);

/* ptxexec.c */
void mc_fault(Blk *b, Thr *t, const char *fmt, const char *arg, u64 val);
u8 *mc_stack_alloc(Blk *b, Thr *t, u64 n, u64 align);

/* ptxops.c */
int mc_wsync(Blk *b, u32 warp, Thr **grp, u32 mask, u32 n); /* run a warp-synchronous instruction for the lanes that wait at it */
u64 mc_cvt(u32 spec, u64 a, u64 b, u64 c);
int mc_cvt_ok(u32 spec);
u64 mc_fgen(u32 sub, u32 flags, Slot a, Slot b, Slot c);
u64 mc_atom(Blk *b, u32 spec, u8 *p, u64 x, u64 y);
McModule *mc_modules(void);
extern int mc_stats_on;
int mc_builtin_id(const char *name);
int mc_builtin_call(Blk *b, Thr *t, int id, Ins *ip);
void mc_prmt_gen(void);

/* internal opcodes */
enum {
    OP_NOP,
    /* control */
    OP_BRA, OP_BRX, OP_BRT, OP_BRF, OP_SKIPF, OP_SKIPT, OP_CALL, OP_CALLI, OP_CALLB, OP_RET, OP_EXIT, OP_TRAP,
    OP_BAR, OP_WSYNC, OP_CLOCK, OP_ACTIVEMASK, OP_ALLOCA, OP_STACKSAVE, OP_STACKRESTORE, OP_BADFUNC,
    /* moves and extensions */
    OP_MOV, OP_MOV32, OP_ZX8, OP_ZX16, OP_SX8_32, OP_SX16_32, OP_SX8_64, OP_SX16_64, OP_SX32_64, OP_PNOT,
    /* 32-bit integers */
    OP_ADD32, OP_SUB32, OP_MUL32, OP_MULHIU32, OP_MULHIS32, OP_MULWU32, OP_MULWS32, OP_MAD32, OP_MADHIU32, OP_MADHIS32,
    OP_MADWU32, OP_MADWS32, OP_MUL24, OP_MUL24HIU, OP_MUL24HIS, OP_DIVU32, OP_DIVS32, OP_REMU32, OP_REMS32, OP_ABS32,
    OP_NEG32, OP_MINU32, OP_MINS32, OP_MAXU32, OP_MAXS32, OP_POPC32, OP_CLZ32, OP_BFINDU32, OP_BFINDS32, OP_BREV32,
    OP_BFEU32, OP_BFES32, OP_BFI32, OP_SHL32, OP_SHRU32, OP_SHRS32, OP_AND32, OP_OR32, OP_XOR32, OP_NOT32, OP_CNOT32,
    OP_SHFL, OP_SHFR, OP_PRMT, OP_ADDCC32, OP_ADDC32, OP_SUBCC32, OP_SUBC32, OP_SADU32, OP_SADS32, OP_DP4A, OP_DP2A,
    OP_FNS, OP_SZEXT, OP_BMSK, OP_LOP3, OP_ADDSAT32, OP_SUBSAT32,
    /* 64-bit integers */
    OP_ADD64, OP_SUB64, OP_MUL64, OP_MULHIU64, OP_MULHIS64, OP_MAD64, OP_MADHIU64, OP_MADHIS64, OP_DIVU64, OP_DIVS64,
    OP_REMU64, OP_REMS64, OP_ABS64, OP_NEG64, OP_MINU64, OP_MINS64, OP_MAXU64, OP_MAXS64, OP_POPC64, OP_CLZ64,
    OP_BFINDU64, OP_BFINDS64, OP_BREV64, OP_BFEU64, OP_BFES64, OP_BFI64, OP_SHL64, OP_SHRU64, OP_SHRS64, OP_AND64,
    OP_OR64, OP_XOR64, OP_NOT64, OP_CNOT64, OP_ADDCC64, OP_ADDC64, OP_SUBCC64, OP_SUBC64, OP_SADU64, OP_SADS64,
    /* comparisons: the order within each group is that of the CMP_ codes */
    OP_SETP_U32, OP_SETP_S32 = OP_SETP_U32 + 6, OP_SETP_U64 = OP_SETP_S32 + 6, OP_SETP_S64 = OP_SETP_U64 + 6,
    OP_SETP_F32 = OP_SETP_S64 + 6, OP_SETP_F64 = OP_SETP_F32 + 14, OP_SETP_END = OP_SETP_F64 + 14,
    OP_SELP, OP_SELP32, OP_SLCT_S32, OP_SLCT_F32, OP_SLCT_F32FTZ, OP_SET, OP_PAND, OP_POR, OP_PXOR,
    /* floating point */
    OP_FADD32, OP_FSUB32, OP_FMUL32, OP_FDIV32, OP_FMA32, OP_FADD32Z, OP_FSUB32Z, OP_FMUL32Z, OP_FDIV32Z, OP_FMA32Z,
    OP_FMIN32, OP_FMAX32, OP_FABS32, OP_FNEG32, OP_FSQRT32, OP_FRSQRT32, OP_FRCP32,
    OP_FADD64, OP_FSUB64, OP_FMUL64, OP_FDIV64, OP_FMA64, OP_FMIN64, OP_FMAX64, OP_FABS64, OP_FNEG64, OP_FSQRT64,
    OP_FGEN, OP_TESTP,
    /* conversions */
    OP_CVT_F32_S32, OP_CVT_F32_U32, OP_CVT_F64_S32, OP_CVT_F64_U32, OP_CVT_F32_S64, OP_CVT_F32_U64, OP_CVT_F64_S64,
    OP_CVT_F64_U64, OP_CVT_S32_F32Z, OP_CVT_U32_F32Z, OP_CVT_S32_F64Z, OP_CVT_S64_F32Z, OP_CVT_S64_F64Z,
    OP_CVT_F64_F32, OP_CVT_F32_F64, OP_CVT_F32_F16, OP_CVT_F16_F32, OP_CVT,
    /* memory: generic addresses, then the same for addresses of the shared space */
    OP_LD8, OP_LD8S32, OP_LD8S64, OP_LD16, OP_LD16S32, OP_LD16S64, OP_LD32, OP_LD32S64, OP_LD64, OP_LDV,
    OP_ST8, OP_ST16, OP_ST32, OP_ST64, OP_STV,
    OP_LD8_S, OP_LD8S32_S, OP_LD8S64_S, OP_LD16_S, OP_LD16S32_S, OP_LD16S64_S, OP_LD32_S, OP_LD32S64_S, OP_LD64_S,
    OP_LDV_S, OP_ST8_S, OP_ST16_S, OP_ST32_S, OP_ST64_S, OP_STV_S,
    OP_ATOM, OP_ATOM_S, OP_CPASYNC, OP_CVTA_TO_SHARED, OP_CVTA_SHARED, OP_ISSPACEP,
    OP_COUNT
};

/* comparison codes */
enum {
    CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE, CMP_EQU, CMP_NEU, CMP_LTU, CMP_LEU, CMP_GTU, CMP_GEU, CMP_NUM,
    CMP_NAN
};

/* OP_FGEN: the function in the low byte of aux; flags in c */
enum {
    FG_ADD, FG_SUB, FG_MUL, FG_DIV, FG_FMA, FG_MIN, FG_MAX, FG_ABS, FG_NEG, FG_RCP, FG_SQRT, FG_RSQRT, FG_SIN, FG_COS,
    FG_LG2, FG_EX2, FG_TANH, FG_COPYSIGN, FG_RINT
};
#define FF_RMODE 7      /* MC_R* */
#define FF_FTZ 8
#define FF_SAT 16
#define FF_NAN 32       /* min, max: a NaN operand gives NaN */
#define FF_ABS 64       /* min, max: of the magnitudes */
#define FF_XORSIGN 128  /* min, max: with the product's sign */
#define FF_TYPE_SHIFT 8 /* T_F32, T_F64, T_F16, T_BF16, T_F16X2, T_BF16X2 */
#define FF_3SRC 0x10000

/* OP_WSYNC kinds, in aux */
enum {
    WS_BARWARP, WS_SHFL_UP, WS_SHFL_DOWN, WS_SHFL_BFLY, WS_SHFL_IDX, WS_VOTE_ALL, WS_VOTE_ANY, WS_VOTE_EQ,
    WS_VOTE_BALLOT, WS_MATCH_ANY32, WS_MATCH_ANY64, WS_MATCH_ALL32, WS_MATCH_ALL64, WS_REDUX, WS_MMA, WS_LDMATRIX, WS_ACTIVEMASK
};

/* OP_CVT: the conversion is described by three words in xtra */
/* OP_ATOM spec: op | type << 8 */
enum { AT_AND, AT_OR, AT_XOR, AT_CAS, AT_EXCH, AT_ADD, AT_INC, AT_DEC, AT_MIN, AT_MAX };

/* mma descriptors, in xtra: kind, then the slots */
enum { MMA_F16_F32, MMA_F16_F16, MMA_BF16_F32, MMA_TF32_K8, MMA_S8_K32, MMA_U8_K32, MMA_F16_K8, MMA_BF16_K8, MMA_TF32_K4, MMA_S8_K16, MMA_FP8_K32 };
#endif
