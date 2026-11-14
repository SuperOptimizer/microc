// Program entry, raw system calls and non-local jumps for arm64 Darwin.
// This is the only operating-system specific file of the C library.
.text
.globl start
.globl ___syscall
.globl ___fork
.globl ___pipe
.globl _setjmp
.globl _longjmp
.globl _sqrt
.globl _fabs

// dyld enters here like main(argc, argv, envp, apple)
.align 2
start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    adrp x8, ___environ@PAGE
    add x8, x8, ___environ@PAGEOFF
    str x2, [x8]
    bl _main
    bl _exit

// long __syscall(long number, long a, long b, long c, long d, long e, long f)
// Returns the result, or the negated errno value on failure.
.align 2
___syscall:
    mov x16, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0x80
    b.cc syscall_ok
    neg x0, x0
syscall_ok:
    ret

// int __fork(void): 0 in the child, the child's pid in the parent,
// or the negated errno value. The kernel sets x1 to 1 in the child.
.align 2
___fork:
    mov x16, #2
    svc #0x80
    b.cc fork_ok
    neg x0, x0
    ret
fork_ok:
    cbz x1, fork_parent
    mov x0, #0
fork_parent:
    ret

// int __pipe(int fd[2]): the kernel returns the descriptors in x0 and x1.
.align 2
___pipe:
    mov x9, x0
    mov x16, #42
    svc #0x80
    b.cc pipe_ok
    neg x0, x0
    ret
pipe_ok:
    str w0, [x9]
    str w1, [x9, #4]
    mov x0, #0
    ret

// int setjmp(jmp_buf env)
.align 2
_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov x0, #0
    ret

// void longjmp(jmp_buf env, int val)
.align 2
_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    mov sp, x9
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret

.align 2
_sqrt:
    fsqrt d0, d0
    ret

.align 2
_fabs:
    fabs d0, d0
    ret
