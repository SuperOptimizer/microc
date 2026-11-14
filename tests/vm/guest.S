/* A bare-metal guest for mvm: it runs batches of instructions on pseudo-random inputs and folds the
   results into one number per batch, takes exceptions and interrupts, and visits EL0 behind an MMU.
   The real CPU (mvm -accel hvf) and the interpreter (mvm -accel emu) must print the same. */

    .section .head, "ax"
    .globl _start
_start:
    b start
    .long 0
    .quad 0
    .quad _end - 0x40000000
    .quad 0xa
    .quad 0, 0, 0
    .ascii "ARM\x64"
    .long 0

    .text
start:
    ldr x0, =stack_top
    mov sp, x0
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    ldr x0, =vectors
    msr vbar_el1, x0
    isb
    bl main
    ldr x0, =0x84000008
    hvc #0
1:  b 1b
    .ltorg

/* ---- vectors ---- */

.macro SAVE
    sub sp, sp, #176
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x30, [sp, #144]
    stp x29, x29, [sp, #160]
.endm

.macro RESTORE
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x30, [sp, #144]
    ldr x29, [sp, #160]
    add sp, sp, #176
.endm

    .balign 0x800
vectors:
    .rept 4
    b .
    .balign 0x80
    .endr
    b sync_el1
    .balign 0x80
    b irq_el1
    .balign 0x80
    b .
    .balign 0x80
    b .
    .balign 0x80
    b sync_el0
    .balign 0x80
    .rept 7
    b .
    .balign 0x80
    .endr

sync_el1:
    SAVE
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    mov x3, #1
    bl sync_handler
    msr elr_el1, x0
    RESTORE
    eret

sync_el0:
    SAVE
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    mov x3, #0
    bl sync_handler
    cbz x0, user_return
    msr elr_el1, x0
    RESTORE
    eret

irq_el1:
    SAVE
    bl irq_handler
    RESTORE
    eret

/* run_user(entry, stack): enter EL0; comes back when the user code does svc #2 */
    .globl run_user
run_user:
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    ldr x2, =saved_sp
    mov x3, sp
    str x3, [x2]
    msr elr_el1, x0
    msr sp_el0, x1
    mov x2, #0x3c0
    msr spsr_el1, x2
    mov x0, #41
    eret
user_return:
    ldr x0, [sp] /* what the user had in x0 */
    ldr x2, =saved_sp
    ldr x3, [x2]
    mov sp, x3
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
    .ltorg

/* what EL0 runs; it is entered through another mapping, so it must not care where it is */
    .globl user_code
user_code:
    add x0, x0, #1
    svc #1
    mov x1, #0xc0000000
    str x0, [x1]
    ldr x2, [x1, #8]
    mov x3, #0x40000000
    ldr x4, [x3]
    mrs x5, ttbr0_el1
    str x0, [sp, #-16]!
    ldr x6, [sp], #16
    add x0, x6, #100
    svc #2
    b .

/* ---- small services for the C side ---- */

    .globl do_svc, do_brk, do_udf, get_cntfrq, set_tval, set_ctl, irq_on, irq_off, do_wfi, mmu_on, tpidr_test, do_ldtr, do_sttr
do_svc:
    svc #7
    ret
do_brk:
    brk #3
    ret
do_udf:
    .inst 0
    ret
get_cntfrq:
    mrs x0, cntfrq_el0
    ret
set_tval:
    msr cntv_tval_el0, x0
    ret
set_ctl:
    msr cntv_ctl_el0, x0
    ret
irq_on:
    msr daifclr, #2
    ret
irq_off:
    msr daifset, #2
    ret
do_wfi:
    wfi
    ret
mmu_on: /* x0 = table, x1 = tcr, x2 = mair */
    msr ttbr0_el1, x0
    msr tcr_el1, x1
    msr mair_el1, x2
    isb
    tlbi vmalle1
    dsb sy
    isb
    mrs x0, sctlr_el1
    orr x0, x0, #1          /* MMU */
    orr x0, x0, #(1 << 2)   /* data cache */
    orr x0, x0, #(1 << 12)  /* instruction cache */
    msr sctlr_el1, x0
    isb
    ret
tpidr_test:
    msr tpidr_el1, x0
    msr tpidr_el0, x1
    mrs x2, tpidr_el1
    mrs x3, tpidr_el0
    eor x0, x2, x3, ror #7
    mrs x4, currentel
    add x0, x0, x4
    mrs x4, spsel
    add x0, x0, x4, lsl #8
    mrs x4, daif
    add x0, x0, x4, lsl #16
    ret
do_ldtr:
    ldtr x0, [x0]
    ret
do_sttr:
    sttr x1, [x0]
    ret

/* ---- instruction batches ---- */

load_inputs:
    .irp r, 0,1,2,3,4,5,6,7
    eor x19, x19, x19, lsl #13
    eor x19, x19, x19, lsr #7
    eor x19, x19, x19, lsl #17
    mov x\r, x19
    .endr
    and x5, x5, #63
    sxth x6, w6
    and x7, x7, #3
    lsl x8, x4, #28
    msr nzcv, x8
    ret

load_fp: /* d0, d1: moderate integers; d2: a large value; d3: a fraction */
    asr x8, x0, #40
    scvtf d0, x8
    asr x8, x1, #44
    scvtf d1, x8
    asr x8, x2, #3
    scvtf d2, x8
    fdiv d3, d0, d1
    ret

mix:
    mrs x21, nzcv
    eor x20, x20, x21, lsr #28
    .irp r, 0,1,2,3,4,5,6,7
    eor x20, x20, x\r
    ror x20, x20, #51
    add x20, x20, x20, lsl #5
    .endr
    ret

mixf:
    .irp r, 0,1,2,3
    fmov x10, d\r
    eor x20, x20, x10
    ror x20, x20, #47
    add x20, x20, x20, lsl #3
    .endr
    b mix

.macro GROUP name
    .globl \name
\name:
    stp x29, x30, [sp, #-64]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    ldr x19, =0x9e3779b97f4a7c15
    mov x20, #0
    mov x23, #200
    ldr x9, =buffer + 256
.Lloop_\name:
.endm

.macro ENDGROUP name
    subs x23, x23, #1
    b.ne .Lloop_\name
    mov x0, x20
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x29, x30, [sp], #64
    ret
    .ltorg
.endm

.macro T ops:vararg
    bl load_inputs
    \ops
    bl mix
.endm

.macro TF ops:vararg
    bl load_inputs
    bl load_fp
    \ops
    bl mixf
.endm

GROUP grp_arith
    T add x0, x1, #0x123
    T add w0, w1, #0xfff, lsl #12
    T adds x0, x1, #1
    T adds w0, w1, #0x800, lsl #12
    T sub x0, x1, #77
    T subs x0, x1, #0
    T subs w0, w1, #0xabc
    T add x0, x1, x2
    T add w0, w1, w2, lsl #7
    T adds x0, x1, x2, lsr #13
    T adds w0, w1, w2, asr #3
    T sub x0, x1, x2, asr #60
    T subs x0, x1, x2
    T subs w0, w1, w2, lsl #31
    T add x0, x1, w2, uxtb
    T add x0, x1, w2, sxth #3
    T adds x0, x1, w2, sxtw #2
    T sub w0, w1, w2, uxth #1
    T subs x0, x1, x2, uxtx #4
    T subs x0, x1, w2, sxtb
    T adc x0, x1, x2
    T adcs w0, w1, w2
    T sbc w0, w1, w2
    T sbcs x0, x1, x2
    T ngc x0, x1
    T ngcs w0, w1
    T neg x0, x1, lsl #3
    T negs w0, w1
    T cmp x1, x2
    T cmp w1, w2
    T cmn x1, x2
    T cmn w1, #5
    T cmp x7, #2
    T cmp x1, x1
    T ccmp x1, x2, #5, ne
    T ccmp w7, #2, #10, cs
    T ccmn x6, #1, #3, mi
    T ccmn w1, w2, #12, vc
    T ccmp x6, x7, #0, hi
    T ccmp x1, #31, #15, lt
    T ccmp x1, x2, #6, gt
    T ccmp x1, x2, #9, le
    T csel x0, x1, x2, eq
    T csel w0, w1, w2, ls
    T csinc x0, x1, x2, ge
    T csinv w0, w1, w2, vs
    T csneg x0, x1, x2, pl
    T csneg w0, w1, w2, cc
    T cset x0, hi
    T csetm w0, lt
    T cinc x0, x1, ne
    T cneg w0, w1, mi
ENDGROUP grp_arith

GROUP grp_logic
    T and x0, x1, #0xff
    T and w0, w1, #0x80000001
    T orr x0, x1, #0x5555555555555555
    T orr w0, w1, #0xf0f0f0f0
    T eor x0, x1, #0x00ffff0000ffff00
    T eor w0, w1, #0x3ffc
    T ands x0, x1, #0xfffffffffffffffe
    T ands w0, w1, #0x7
    T tst x1, #0x8000000000000000
    T and x0, x1, x2
    T and w0, w1, w2, lsl #9
    T bic x0, x1, x2, lsr #5
    T bics w0, w1, w2
    T orr x0, x1, x2, asr #17
    T orn w0, w1, w2, ror #3
    T eor x0, x1, x2, ror #63
    T eon x0, x1, x2, lsl #1
    T ands x0, x1, x2, lsr #63
    T tst w1, w2
    T mvn x0, x1
    T mvn w0, w1, lsl #4
    T mov x0, #0x1234
    T mov w0, #0xffff0000
    T movz x0, #0xbeef, lsl #32
    T movn x0, #0x1234, lsl #16
    T movn w0, #0
    T movk x0, #0xabcd, lsl #48
    T movk w0, #0x1111, lsl #16
    T ubfx x0, x1, #5, #17
    T ubfx w0, w1, #31, #1
    T sbfx x0, x1, #60, #4
    T sbfx w0, w1, #3, #9
    T bfi x0, x1, #12, #20
    T bfi w0, w1, #31, #1
    T bfxil x0, x1, #40, #7
    T bfxil w0, w1, #0, #32
    T ubfiz x0, x1, #33, #9
    T sbfiz x0, x1, #7, #50
    T sbfiz w0, w1, #1, #3
    T lsl x0, x1, #63
    T lsl w0, w1, #1
    T lsr x0, x1, #1
    T lsr w0, w1, #31
    T asr x0, x1, #17
    T asr w0, w1, #5
    T ror x0, x1, #13
    T ror w0, w1, #31
    T sxtb x0, w1
    T sxth w0, w1
    T sxtw x0, w1
    T uxtb w0, w1
    T uxth w0, w1
    T extr x0, x1, x2, #7
    T extr w0, w1, w2, #31
    T extr x0, x1, x2, #0
    T lsl x0, x1, x5
    T lsl w0, w1, w5
    T lsr x0, x1, x2
    T lsr w0, w1, w2
    T asr x0, x1, x5
    T asr w0, w1, w2
    T ror x0, x1, x5
    T ror w0, w1, w2
    T clz x0, x1
    T clz w0, w6
    T clz x0, x7
    T cls x0, x6
    T cls w0, w1
    T cls x0, x7
    T rbit x0, x1
    T rbit w0, w1
    T rev x0, x1
    T rev w0, w1
    T rev16 x0, x1
    T rev16 w0, w1
    T rev32 x0, x1
ENDGROUP grp_logic

GROUP grp_mul
    T mul x0, x1, x2
    T mul w0, w1, w2
    T madd x0, x1, x2, x3
    T madd w0, w1, w2, w3
    T msub x0, x1, x2, x3
    T msub w0, w1, w2, w3
    T mneg x0, x1, x2
    T smull x0, w1, w2
    T umull x0, w1, w2
    T smaddl x0, w1, w2, x3
    T umaddl x0, w1, w2, x3
    T smsubl x0, w1, w2, x3
    T umsubl x0, w1, w2, x3
    T smulh x0, x1, x2
    T smulh x0, x6, x2
    T smulh x0, x6, x6
    T umulh x0, x1, x2
    T udiv x0, x1, x2
    T udiv x0, x1, x5
    T udiv w0, w1, w5
    T udiv x0, x1, x7
    T sdiv x0, x1, x6
    T sdiv w0, w1, w6
    T sdiv x0, x1, x7
    T sdiv w0, w6, w7
    T sdiv x0, x6, x5
ENDGROUP grp_mul

GROUP grp_mem
    bl load_inputs
    stp x0, x1, [x9, #-64]
    stp x2, x3, [x9, #-48]
    stp w4, w6, [x9, #-32]
    str w0, [x9, x5]
    strh w1, [x9, x5, lsl #1]
    strb w2, [x9, #100]
    stur x3, [x9, #-3]
    ldrsb x0, [x9, #-64]
    ldrsh w1, [x9, #-62]
    ldrsw x2, [x9, #-60]
    ldrb w3, [x9, x5]
    ldrh w4, [x9, w5, uxtw #1]
    ldur x6, [x9, #-3]
    bl mix
    bl load_inputs
    mov x10, x9
    str x0, [x10, #8]!
    str w1, [x10], #-4
    strb w2, [x10, #-1]!
    strh w3, [x10], #2
    ldr x0, [x10, #4]
    ldp w1, w2, [x10], #8
    ldpsw x3, x4, [x10, #-8]!
    ldrsb w5, [x10, #1]
    ldrsh x6, [x10, #2]!
    sub x7, x10, x9
    bl mix
    bl load_inputs
    str x0, [x9, #128]
    str x1, [x9, #136]
2:  ldxr x2, [x9, #128]
    add x2, x2, x1
    stxr w3, x2, [x9, #128]
    cbnz w3, 2b
    add x10, x9, #128
3:  ldaxp x4, x5, [x10]
    eor x4, x4, x5
    stlxp w3, x4, x0, [x10]
    cbnz w3, 3b
4:  ldxrb w6, [x10]
    add w6, w6, #1
    stxrb w3, w6, [x10]
    cbnz w3, 4b
    ldar x0, [x10]
    stlr w0, [x10, #0]
    ldr x1, [x10]
    ldr x2, [x10, #8]
    bl mix
    bl load_inputs
    stp x0, x1, [x9, #-64]
    stp x2, x3, [x9, #-48]
    ldr q0, [x9, #-64]
    str q0, [x9, #32]
    ldp q1, q2, [x9, #-64]
    stp q2, q1, [x9, #64]
    ldp d3, d4, [x9, #72]
    str d3, [x9, #160]
    str s4, [x9, #168]
    ldr s5, [x9, #-60]
    stur q1, [x9, #-7]
    ldur d6, [x9, #1]
    ldr x0, [x9, #40]
    ldr x1, [x9, #64]
    ldr x2, [x9, #88]
    fmov x3, d3
    fmov x4, d5
    fmov x5, d6
    ldr x6, [x9, #160]
    ldr x7, [x9, #168]
    bl mix
    bl load_inputs
    ldr x0, .Llit
    ldr w1, .Llit
    ldrsw x2, .Llit + 4
    bl mix
    b 5f
    .balign 8
.Llit:
    .quad 0x89abcdef01234567
5:
ENDGROUP grp_mem

GROUP grp_branch
    bl load_inputs
    mov x8, #0
    cbz x7, 2f
    add x8, x8, #1
2:  cbnz w7, 2f
    add x8, x8, #2
2:  tbz x0, #37, 2f
    add x8, x8, #4
2:  tbnz w1, #5, 2f
    add x8, x8, #8
2:  tbnz x2, #63, 2f
    add x8, x8, #16
2:  cmp x0, x1
    b.hi 2f
    add x8, x8, #32
2:  b.lt 2f
    add x8, x8, #64
2:  cmn w2, w3
    b.vs 2f
    add x8, x8, #128
2:  b.mi 2f
    add x8, x8, #256
2:  b.ls 2f
    add x8, x8, #512
2:  b.ge 2f
    add x8, x8, #1024
2:  b.cc 2f
    add x8, x8, #2048
2:  b.le 2f
    add x8, x8, #1, lsl #12
2:  b.al 2f
    add x8, x8, #2, lsl #12
2:  adr x10, 3f
    br x10
    add x8, x8, #4, lsl #12
3:  adr x10, 4f
    mov x22, x30
    blr x10
    b 5f
4:  add x8, x8, #8, lsl #12
    ret
5:  bl 4b
    mov x30, x22
    mov x0, x8
    bl mix
ENDGROUP grp_branch

GROUP grp_fp
    TF fadd d0, d1, d2
    TF fsub d0, d1, d3
    TF fmul d0, d2, d3
    TF fdiv d0, d2, d1
    TF fnmul d0, d1, d3
    TF fabs d1, d2 ; fsqrt d0, d1
    TF fneg d0, d3
    TF fmax d0, d1, d2 ; fmin d1, d2, d3
    TF fmaxnm d0, d1, d3 ; fminnm d1, d0, d2
    TF fcvtzs x0, d3 ; fcvtzu x1, d3 ; fcvtzs w2, d2 ; fcvtzu w3, d2 ; fcvtzs x4, d2 ; fcvtzu x5, d0
    TF fcvtns x0, d3 ; fcvtnu x1, d3 ; fcvtas x2, d3 ; fcvtau w3, d3 ; fcvtms x4, d3 ; fcvtmu w5, d3 ; fcvtps x6, d3 ; fcvtpu x7, d3
    TF scvtf d0, x1 ; ucvtf d1, x2 ; scvtf d2, w3 ; ucvtf d3, w4
    TF scvtf s0, x1 ; ucvtf s1, x2 ; scvtf s2, w3 ; ucvtf s3, w4
    TF frintm d0, d3 ; frintp d1, d3 ; frintz d2, d3
    TF frinta d0, d3 ; frintn d1, d3 ; frintx d2, d3
    TF fcvt s0, d3 ; fcvt d1, s0 ; fcvt s2, d2
    TF fcmp d0, d1
    TF fcmp d3, #0.0
    TF fcmpe d1, d0
    TF fcmp d0, d0
    TF fcmp d0, d1 ; fcsel d2, d0, d1, gt ; fcsel d3, d0, d1, mi
    TF fccmp d0, d1, #4, ne
    TF fccmp d3, d2, #11, eq
    TF fmov x0, d3 ; fmov d1, x1 ; fmov w2, s3 ; fmov s2, w3
    TF fcvt s0, d0 ; fcvt s1, d1 ; fadd s2, s0, s1 ; fmul s3, s0, s1
    TF fcvt s0, d3 ; fcvt s1, d1 ; fdiv s2, s0, s1 ; fsub s3, s0, s1 ; fabs s0, s0 ; fsqrt s0, s0
    TF fcvt s0, d3 ; fcvtzs w0, s0 ; fcvtzu x1, s0 ; fcvtas w2, s0 ; frintm s1, s0 ; fcmp s0, s1
    TF fcvt s1, d2 ; fcvtzs w0, s1 ; fcvtzu w1, s1 ; fcvtzs x2, s1 ; fneg s2, s1 ; fcvtzu x3, s2
ENDGROUP grp_fp

/* vector instructions, as the compiler's vectorizer writes them */
GROUP grp_simd
    bl load_inputs
    .inst 0x4e040c20 /* dup v0.4s, w1 */
    .inst 0x4e080c41 /* dup v1.2d, x2 */
    .inst 0x4e181c61 /* mov v1.d[1], x3 */
    .inst 0x4e020c82 /* dup v2.8h, w4 */
    .inst 0x4e010cc3 /* dup v3.16b, w6 */
    .inst 0x4ea18404 /* add v4.4s, v0.4s, v1.4s */
    .inst 0x6e628425 /* sub v5.8h, v1.8h, v2.8h */
    .inst 0x4ea19c06 /* mul v6.4s, v0.4s, v1.4s */
    .inst 0x4e239c27 /* mul v7.16b, v1.16b, v3.16b */
    .inst 0x6ea18c90 /* cmeq v16.4s, v4.4s, v1.4s */
    .inst 0x6e2334b1 /* cmhi v17.16b, v5.16b, v3.16b */
    .inst 0x4ea134d2 /* cmgt v18.4s, v6.4s, v1.4s */
    .inst 0x4e623cb3 /* cmge v19.8h, v5.8h, v2.8h */
    .inst 0x6ea13cd3 /* cmhs v19.4s, v6.4s, v1.4s */
    .inst 0x4e211c94 /* and v20.16b, v4.16b, v1.16b */
    .inst 0x4ea51cd5 /* orr v21.16b, v6.16b, v5.16b */
    .inst 0x6e271c96 /* eor v22.16b, v4.16b, v7.16b */
    .inst 0x6e641cb1 /* bsl v17.16b, v5.16b, v4.16b */
    .inst 0x6ea61c92 /* bit v18.16b, v4.16b, v6.16b */
    .inst 0x6ee71cb3 /* bif v19.16b, v5.16b, v7.16b */
    .inst 0x4ea164d7 /* smax v23.4s, v6.4s, v1.4s */
    .inst 0x6e236cf8 /* umin v24.16b, v7.16b, v3.16b */
    .inst 0x4e626cb9 /* smin v25.8h, v5.8h, v2.8h */
    .inst 0x6ea164da /* umax v26.4s, v6.4s, v1.4s */
    .inst 0x4f2354db /* shl v27.4s, v6.4s, #3 */
    .inst 0x6f3b04dc /* ushr v28.4s, v6.4s, #5 */
    .inst 0x4f1d04bd /* sshr v29.8h, v5.8h, #3 */
    .inst 0x6ea0b8de /* neg v30.4s, v6.4s */
    .inst 0x6e2058ff /* not v31.16b, v7.16b */
    bl 9f
    .inst 0x4eb1b89f /* addv s31, v4.4s */
    .inst 0x1e2603e0 /* fmov w0, s31 */
    .inst 0x6eb0a8df /* umaxv s31, v6.4s */
    .inst 0x1e2603e1 /* fmov w1, s31 */
    .inst 0x4eb1a8df /* sminv s31, v6.4s */
    .inst 0x1e2603e2 /* fmov w2, s31 */
    .inst 0x6e3038ff /* uaddlv h31, v7.16b */
    .inst 0x1e2603e3 /* fmov w3, s31 */
    .inst 0x4e3038ff /* saddlv h31, v7.16b */
    .inst 0x1e2603e4 /* fmov w4, s31 */
    .inst 0x5ef1b83f /* addp d31, v1.2d */
    .inst 0x9e6603e5 /* fmov x5, d31 */
    .inst 0x4e21d886 /* scvtf v6.4s, v4.4s */
    .inst 0x6e21d827 /* ucvtf v7.4s, v1.4s */
    .inst 0x4e27d4c8 /* fadd v8.4s, v6.4s, v7.4s */
    .inst 0x6e27dcc9 /* fmul v9.4s, v6.4s, v7.4s */
    .inst 0x4ea7d4ca /* fsub v10.4s, v6.4s, v7.4s */
    .inst 0x6e26e4eb /* fcmge v11.4s, v7.4s, v6.4s */
    .inst 0x4ea1b90c /* fcvtzs v12.4s, v8.4s */
    .inst 0x6ea0f94d /* fneg v13.4s, v10.4s */
    .inst 0x6e28d52e /* faddp v14.4s, v9.4s, v8.4s */
    .inst 0x7e30d9cf /* faddp s15, v14.2s */
    .inst 0x4f03e5f0 /* movi v16.16b, #0x6f */
    .inst 0x4e183d86 /* umov x6, v12.d[1] */
    .inst 0x0e0c3da7 /* umov w7, v13.s[1] */
    bl mix
    .inst 0x4ea81d00 /* v0 = v8 */
    .inst 0x4ea91d21 /* v1 = v9 */
    .inst 0x4eaf1de2 /* v2 = v15 */
    .inst 0x4eb01e03 /* v3 = v16 */
    bl mixf
    b 8f
9:  /* fold the sixteen results so far */
    str x30, [sp, #-16]!
    .irp r, 4,5,6,7,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    fmov x10, d\r
    eor x20, x20, x10
    ror x20, x20, #47
    add x20, x20, x20, lsl #3
    .inst 0x4e183c0a | (\r << 5) /* umov x10, v.d[1] */
    eor x20, x20, x10
    ror x20, x20, #45
    .endr
    ldr x30, [sp], #16
    ret
8:
ENDGROUP grp_simd

    .bss
    .balign 16
buffer:
    .space 512
    .globl saved_sp
saved_sp:
    .quad 0
    .space 16384
stack_top:
