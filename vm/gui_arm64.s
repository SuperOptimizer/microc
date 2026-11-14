// void gui_msg_point(id obj, SEL sel, double *xy): send a message that answers with a point (two doubles,
// which come back in d0 and d1; mcc's C does not return structures that way).
.text
.globl _gui_msg_point
.align 2
_gui_msg_point:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x2
    bl _objc_msgSend
    str d0, [x19]
    str d1, [x19, #8]
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
