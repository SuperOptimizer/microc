/* What cannot be written in C, for x86-64: the system call itself and the non-local jumps. */
	.text
/* long __syscall(long number, long a, long b, long c, long d, long e, long f): the number is the
   kernel's own (thread.h translates the aarch64 numbers that the sources are written with) */
	.globl __syscall
	.type __syscall, %function
__syscall:
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsi
	movq %rcx, %rdx
	movq %r8, %r10
	movq %r9, %r8
	movq 8(%rsp), %r9
	syscall
	ret
	.size __syscall, . - __syscall

/* jmp_buf: rbx rbp r12 r13 r14 r15 rsp rip */
	.globl setjmp
	.globl _setjmp
	.type setjmp, %function
	.type _setjmp, %function
setjmp:
_setjmp:
	movq %rbx, 0(%rdi)
	movq %rbp, 8(%rdi)
	movq %r12, 16(%rdi)
	movq %r13, 24(%rdi)
	movq %r14, 32(%rdi)
	movq %r15, 40(%rdi)
	leaq 8(%rsp), %rdx
	movq %rdx, 48(%rdi)
	movq (%rsp), %rdx
	movq %rdx, 56(%rdi)
	xorl %eax, %eax
	ret
	.size setjmp, . - setjmp

	.globl longjmp
	.globl _longjmp
	.type longjmp, %function
	.type _longjmp, %function
longjmp:
_longjmp:
	movl %esi, %eax
	testl %eax, %eax
	jnz 1f
	incl %eax
1:	movq 0(%rdi), %rbx
	movq 8(%rdi), %rbp
	movq 16(%rdi), %r12
	movq 24(%rdi), %r13
	movq 32(%rdi), %r14
	movq 40(%rdi), %r15
	movq 48(%rdi), %rsp
	jmp *56(%rdi)
	.size longjmp, . - longjmp

/* sigsetjmp(env, save): the signal mask is kept after the registers */
	.globl sigsetjmp
	.type sigsetjmp, %function
sigsetjmp:
	movslq %esi, %rsi
	movq %rsi, 192(%rdi)
	testl %esi, %esi
	jz setjmp
	pushq %rdi
	leaq 200(%rdi), %rdx
	xorl %esi, %esi
	xorl %edi, %edi
	call sigprocmask
	popq %rdi
	jmp setjmp
	.size sigsetjmp, . - sigsetjmp

	.globl sqrt
	.type sqrt, %function
sqrt:
	sqrtsd %xmm0, %xmm0
	ret
	.size sqrt, . - sqrt

	.globl fabs
	.type fabs, %function
fabs:
	movq %xmm0, %rax
	btrq $63, %rax
	movq %rax, %xmm0
	ret
	.size fabs, . - fabs

/* long __clone_raw(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *fs, int *ctid):
   a new thread that runs fn(arg) on the stack given and ends when fn returns. To the caller: its id, or
   the negated error. */
	.globl __clone_raw
	.type __clone_raw, %function
__clone_raw:
	andq $-16, %rsi
	subq $16, %rsi
	movq %rdi, 0(%rsi)
	movq %rcx, 8(%rsi)
	movq %rdx, %rdi
	movq %r8, %rdx
	movq 8(%rsp), %r10
	movq %r9, %r8
	movl $56, %eax
	syscall
	testq %rax, %rax
	jz 1f
	ret
1:	xorl %ebp, %ebp
	popq %rax
	popq %rdi
	call *%rax
	movl %eax, %edi
	movl $60, %eax
	syscall
	hlt
	.size __clone_raw, . - __clone_raw

/* void __unmap_exit(void *map, size_t size): gives back the mapping the caller's stack is in, then ends
   the thread; nothing may touch the stack in between. */
	.globl __unmap_exit
	.type __unmap_exit, %function
__unmap_exit:
	movl $11, %eax
	syscall
	xorl %edi, %edi
	movl $60, %eax
	syscall
	hlt
	.size __unmap_exit, . - __unmap_exit

/* where a signal handler returns to: the kernel wants it named in every sigaction */
	.globl __restore_rt
	.type __restore_rt, %function
__restore_rt:
	movl $15, %eax
	syscall
	.size __restore_rt, . - __restore_rt
