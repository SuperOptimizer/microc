/* Program entry for aarch64 Linux: the kernel leaves argc, argv and the
   environment on the stack. */
	.text
	.globl _start
	.type _start, %function
_start:
	mov x29, #0
	mov x30, #0
	mov x0, sp
	bl __libc_start
	.size _start, . - _start
