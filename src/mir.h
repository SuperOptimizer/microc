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
#define MF_TIED 0x40  /* two-address: d is written over s0 (x86), so s0's register is the place for d when s0 dies here */
#define MF_COMMUT 0x08 /* with MF_TIED: s0 and s1 commute, so s1's register will do as well */
#define MF_SPLIT 0x20 /* a copy x64.c's loop splitting made (the loop's own register for a spilled one) */
#define VC_LOW 0x40 /* register class bit: the register may not be one of MTarget.ext (an instruction that reads or writes it cannot name them) */

#define MF_NOTS1 0x80 /* two-address, not commutative: d had better not get s1's register (it would take a copy of s1 first) */

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
    u64 ext;       /* registers that only some instructions can name: never for a register of a VC_LOW class */
    int ext_first; /* the other registers take those first, and leave the rest to the ones that need them */
} MTarget;

void ra_run(MFunc *m, const MTarget *t);
void ra_rerun(MFunc *m, const MTarget *t); /* again, after the function changed (keeps ra_dense's numbering) */
u32 ra_dense(u32 r);                                    /* -g: after ra_run */
u32 ra_ranges(u32 d, u32 *start, u32 *end, u32 max);

#endif
