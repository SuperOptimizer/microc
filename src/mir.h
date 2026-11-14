/* Machine IR shared by instruction selection, register allocation and emission.
   Register ids: 0..63 are physical (target-defined), >= VREG0 are virtual. */
#ifndef MIR_H
#define MIR_H

#include "ir.h"

#define NOREG 0xffffffffU
#define VREG0 64
#define SPILL0 0x40000000U /* location of a spilled virtual register: SPILL0 + index */

#define MF_CALL 1 /* clobbers the target's caller-saved registers */
#define MF_SCALE 2 /* indexed memory access: index is scaled by the access size */
#define MF_SXTW 4  /* indexed memory access: index is a sign-extended 32-bit value */
#define MF_UXTW 8  /* indexed memory access: index is a zero-extended 32-bit value */
#define MF_LO12 16 /* memory access: base holds the page of symbol sym + imm; add the offset within it */

typedef struct MIns {
    u16 op;
    u8 w;  /* 64-bit integer or double precision */
    u8 cc; /* condition, access kind, or other small operand */
    u8 flags;
    u32 d;          /* register defined, or NOREG */
    u32 s0, s1, s2; /* registers used, or NOREG */
    u32 sym;
    i64 imm;
    u64 pmask; /* physical registers used implicitly (call arguments, return value) */
} MIns;

typedef struct MBlock {
    u32 first, n;
    u32 succ[2];
    u32 nsucc;
    u32 freq; /* from a profile: 0 = unknown, else 1 + execution count */
} MBlock;

typedef struct MFunc {
    MIns *ins;
    u32 n, cap;
    MBlock *blk;
    u32 nblk, capblk;
    u32 nvreg;  /* virtual registers are VREG0 .. VREG0 + nvreg - 1 */
    u8 *vclass; /* register class of each virtual register */
    u32 capvreg;
    /* register allocation results */
    u32 *loc; /* per virtual register: physical register or SPILL0 + n */
    u32 nspill;
    u64 used; /* physical registers assigned */
} MFunc;

typedef struct MTarget {
    u64 allocatable[2]; /* per register class */
    u64 caller_saved;
    int move_op; /* register-to-register copy: gives allocation hints */
} MTarget;

void ra_run(MFunc *m, const MTarget *t);

#endif
