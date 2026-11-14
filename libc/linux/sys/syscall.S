/* What cannot be written in C: the system call itself and the non-local jumps. */
	.text
	.globl __syscall
	.type __syscall, %function
__syscall:
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0
	ret
	.size __syscall, . - __syscall

	.globl setjmp
	.globl _setjmp
	.type setjmp, %function
	.type _setjmp, %function
setjmp:
_setjmp:
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x2, sp
	str x2, [x0, #96]
	stp d8, d9, [x0, #112]
	stp d10, d11, [x0, #128]
	stp d12, d13, [x0, #144]
	stp d14, d15, [x0, #160]
	mov x0, #0
	ret
	.size setjmp, . - setjmp

	.globl longjmp
	.globl _longjmp
	.type longjmp, %function
	.type _longjmp, %function
longjmp:
_longjmp:
	ldp x19, x20, [x0, #0]
	ldp x21, x22, [x0, #16]
	ldp x23, x24, [x0, #32]
	ldp x25, x26, [x0, #48]
	ldp x27, x28, [x0, #64]
	ldp x29, x30, [x0, #80]
	ldr x2, [x0, #96]
	mov sp, x2
	ldp d8, d9, [x0, #112]
	ldp d10, d11, [x0, #128]
	ldp d12, d13, [x0, #144]
	ldp d14, d15, [x0, #160]
	cmp w1, #0
	csinc w0, w1, wzr, ne
	br x30
	.size longjmp, . - longjmp

/* sigsetjmp(env, save): the signal mask is kept after the registers */
	.globl sigsetjmp
	.type sigsetjmp, %function
sigsetjmp:
	str x1, [x0, #192]
	cbz w1, setjmp
	stp x0, x30, [sp, #-16]!
	add x2, x0, #200
	mov x1, #0
	mov x0, #0
	bl sigprocmask
	ldp x0, x30, [sp], #16
	b setjmp
	.size sigsetjmp, . - sigsetjmp

	.globl sqrt
	.type sqrt, %function
sqrt:
	fsqrt d0, d0
	ret
	.size sqrt, . - sqrt

	.globl fabs
	.type fabs, %function
fabs:
	fabs d0, d0
	ret
	.size fabs, . - fabs

/* void __set_tp(void *tp): the thread pointer of the calling thread */
	.globl __set_tp
	.type __set_tp, %function
__set_tp:
	msr tpidr_el0, x0
	ret
	.size __set_tp, . - __set_tp

/* long __clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid):
   a new thread that runs fn(arg) on the stack given and ends when fn returns. To the caller: its id, or
   the negated error. */
	.globl __clone
	.type __clone, %function
__clone:
	and x1, x1, #-16
	stp x0, x3, [x1, #-16]!
	mov x0, x2
	mov x2, x4
	mov x3, x5
	mov x4, x6
	mov x8, #220
	svc #0
	cbz x0, 1f
	ret
1:	mov x29, #0
	mov x30, #0
	ldp x1, x0, [sp], #16
	blr x1
	mov x8, #93
	svc #0
	.size __clone, . - __clone

/* void __unmap_exit(void *map, size_t size): gives back the mapping the caller's stack is in, then ends
   the thread; nothing may touch the stack in between. */
	.globl __unmap_exit
	.type __unmap_exit, %function
__unmap_exit:
	mov x8, #215
	svc #0
	mov x0, #0
	mov x8, #93
	svc #0
	.size __unmap_exit, . - __unmap_exit
