.text
.globl _start
.align 2
_start:
    nop
    mov x0, #42
    mov w1, #0x10000
    mov x2, #-1
    mov x3, x4
    mov x29, sp
    mov sp, x29
    movz x5, #0x1234, lsl #16
    movk x5, #0xffff, lsl #32
    add x0, x1, x2
    add x0, x1, #16
    add x0, x1, #4096
    add sp, sp, #32
    sub sp, sp, #32
    sub w3, w4, w5
    subs x0, x0, #1
    adds w1, w2, w3
    cmp x0, x1
    cmp w0, #7
    cmn x0, #1
    neg x1, x2
    mvn w1, w2
    and x0, x1, x2
    orr w0, w1, w2
    eor x0, x1, x2
    and x0, x1, #0xff
    tst x0, #1
    mul x0, x1, x2
    madd x0, x1, x2, x3
    msub w0, w1, w2, w3
    sdiv x0, x1, x2
    udiv w0, w1, w2
    lsl x0, x1, #3
    lsr w0, w1, #4
    asr x0, x1, #63
    lsl x0, x1, x2
    sxtw x0, w1
    uxtb w0, w1
    csel x0, x1, x2, eq
    cset w0, lt
    ldr x0, [x1]
    ldr w0, [x1, #4]
    ldr x0, [sp, #16]
    str x0, [x1, #-8]
    ldrb w0, [x1, #1]
    strb w0, [x1]
    ldrh w0, [x1, #2]
    ldrsw x0, [x1, #4]
    ldrsb w0, [x1]
    ldr x0, [x1, x2]
    ldr x0, [x1, #8]!
    ldr x0, [x1], #8
    ldr d0, [x1, #8]
    str s1, [sp, #4]
    stp x29, x30, [sp, #-16]!
    ldp x29, x30, [sp], #16
    stp x19, x20, [x0, #16]
    ldp d8, d9, [x0, #96]
    fadd d0, d1, d2
    fmul s0, s1, s2
    fsqrt d0, d1
    fabs d0, d1
    fcmp d0, d1
    fmov d0, x1
    fmov x1, d0
    fmov d2, d3
    scvtf d0, x1
    fcvtzs w0, d1
    fcvt s0, d1
back:
    b back
    bl back
    b.ne back
    cbz x0, fwd
    cbnz w1, fwd
    tbz x0, #63, fwd
    adr x0, fwd
fwd:
    br x1
    blr x2
    svc #0x80
    ret
