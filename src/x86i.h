/* x86-64 intrinsics (<immintrin.h>): what each one takes and gives, and how it is made.
   The header defines _mm256_add_ps and the rest as __builtin_x86_mm256_add_ps ...; the parser looks the
   name up here (x86i_find) and builds IR: plain loads, stores, splats and constants where those say it,
   else one OP_XI whose c is the entry's index, which x64.c turns into the instruction described. */
#ifndef X86I_H
#define X86I_H

#include "mcc.h"

/* operand and result kinds */
enum {
    XT_VOID,
    XT_M512, XT_M512D, XT_M512I, /* 512-bit vectors: IT_V512 */
    XT_M256, XT_M256D, XT_M256I, /* 256-bit vectors: IT_V256 */
    XT_M128, XT_M128D, XT_M128I, /* 128-bit vectors: IT_V128 */
    XT_I8, XT_I16, XT_U16, XT_I32, XT_U32, XT_I64, XT_U64, XT_F32, XT_F64,
    XT_PTR, /* an address */
    XT_IMM  /* an integer constant expression: the instruction's immediate byte */
};

/* how an entry is made */
enum {
    XK_VEX,      /* one VEX-encoded instruction, operands placed by the form */
    XK_LOAD,     /* a whole vector from the address (OP_LOAD M_V128 / M_V256) */
    XK_STORE,    /* the vector to the address (OP_STORE) */
    XK_VLDM,     /* one VEX instruction whose r/m operand is the memory at the address (XF_RM) */
    XK_VSTM,     /* one VEX instruction that writes the memory at the address (XF_MR) */
    XK_ZERO,     /* the zero vector */
    XK_SPLAT,    /* the scalar in every lane (OP_VSPLAT, lane kind in extra) */
    XK_SET,      /* the arguments as lanes, the last one first */
    XK_SETR,     /* the arguments as lanes, the first one first */
    XK_CAST,     /* the same bits as another vector type */
    XK_LO,       /* the low part, or a wider register with this in its low part */
    XK_BCAST128, /* both halves the 128-bit argument */
    XK_EXTRACT,  /* lane imm of a 256-bit vector, lane kind in extra */
    XK_TESTZ,    /* vptest / vtestps: 1 if ZF */
    XK_TESTC,    /* ... 1 if CF */
    XK_GATHER,   /* base, index, scale */
    XK_MGATHER,  /* source, base, index, mask, scale */
    XK_CVTSH,    /* half (in the low 16 bits of an integer) to float */
    XK_CVTSS_SH, /* float to half, rounded as imm says */
    XK_CPUID,    /* a register of cpuid(leaf, subleaf) */
    XK_RAW,      /* fixed bytes (enc, most significant first) */
    XK_PREFETCH, /* prefetch hint imm of the address */
    XK_KTEST,    /* vptestm[bwdq] into a mask register, whose bits are the result (kmov) */
    XK_MASKOP    /* (src, mask, a, b): the operation on a and b where the mask has a 1, src elsewhere (k1, XF_DVMK) */
};

/* where the operands of an XK_VEX instruction go: d is the result, a0 a1 a2 the arguments that are not
   immediates; an XT_IMM argument (or extra & 0x100) is the immediate byte at the end */
enum {
    XF_NONE,
    XF_RVM,  /* reg = d, vvvv = a0, rm = a1 */
    XF_RVMS, /* reg = d, vvvv = a1, rm = a0 (vpermd: the index is the first source) */
    XF_RM,   /* reg = d, rm = a0 */
    XF_MR,   /* reg = a0, rm = d (vextracti128, vcvtps2ph, vmovd to a general register) */
    XF_VMI,  /* vvvv = d, rm = a0, reg = the digit in enc (shifts by an immediate) */
    XF_RVMR, /* reg = d, vvvv = a0, rm = a1, imm[7:4] = a2 (vblendvps) */
    XF_DVM,  /* d = a0 first, then reg = d, vvvv = a1, rm = a2 (FMA 213: d = a0 * a1 + a2) */
    XF_DVMK  /* the same, the operands in that order whatever they are (vpternlog; a masked operation merged into a0) */
};

/* enc: the opcode byte, then map (1 0f, 2 0f38, 3 0f3a), pp (0 none, 1 66, 2 f3, 3 f2), W, L, /digit, Z (512 bits, EVEX;
   W then as EVEX wants it) */
#define XE_OPC(e) ((e) & 0xff)
#define XE_MAP(e) ((e) >> 8 & 3)
#define XE_PP(e) ((e) >> 10 & 3)
#define XE_W(e) ((e) >> 12 & 1)
#define XE_L(e) ((e) >> 13 & 1)
#define XE_DIGIT(e) ((e) >> 14 & 7)
#define XE_Z(e) ((e) >> 17 & 1) /* 512 bits: EVEX encoded */
#define XE_K1(e) ((e) >> 18 & 1) /* EVEX: masked by k1 (merging) */

typedef struct X86I {
    const char *name; /* without the leading underscore */
    u8 ret;
    u8 nargs;
    u8 arg[5]; /* XK_SET / XK_SETR: every argument is of the kind of arg[0] */
    u8 kind;
    u8 form;
    u32 enc;
    u32 extra; /* XK_SPLAT, XK_EXTRACT: lane kind (M_*); XK_VEX: 0x100 | a fixed immediate byte */
} X86I;

extern const X86I x86i_tab[];
extern const u32 x86i_count;
int x86i_find(const char *name);

#endif
