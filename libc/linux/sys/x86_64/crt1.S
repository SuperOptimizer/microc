/* Program entry for x86-64 Linux: the kernel leaves argc, argv and the
   environment on the stack. */
	.text
	.globl _start
	.type _start, %function
_start:
	xorl %ebp, %ebp
	movq %rsp, %rdi
	andq $-16, %rsp
	call __libc_start
	hlt
	.size _start, . - _start
