/* What every slot of cuda/mux holds until a run time is chosen (mux.c): a public function was called, its
   stub jumped here with the slot's address in r11 and everything else as the caller left it. The argument
   registers are kept over the choice, and then the jump is made again, through the slot as it now is. */
	.text
	.globl __mcuda_mux_first
	.type __mcuda_mux_first, %function
__mcuda_mux_first:
	pushq %rbp
	movq %rsp, %rbp
	subq $208, %rsp
	movq %rdi, 0(%rsp)
	movq %rsi, 8(%rsp)
	movq %rdx, 16(%rsp)
	movq %rcx, 24(%rsp)
	movq %r8, 32(%rsp)
	movq %r9, 40(%rsp)
	movq %rax, 48(%rsp)
	movq %r10, 56(%rsp)
	movq %r11, 64(%rsp)
	movups %xmm0, 80(%rsp)
	movups %xmm1, 96(%rsp)
	movups %xmm2, 112(%rsp)
	movups %xmm3, 128(%rsp)
	movups %xmm4, 144(%rsp)
	movups %xmm5, 160(%rsp)
	movups %xmm6, 176(%rsp)
	movups %xmm7, 192(%rsp)
	call __mcuda_mux_init
	movups 192(%rsp), %xmm7
	movups 176(%rsp), %xmm6
	movups 160(%rsp), %xmm5
	movups 144(%rsp), %xmm4
	movups 128(%rsp), %xmm3
	movups 112(%rsp), %xmm2
	movups 96(%rsp), %xmm1
	movups 80(%rsp), %xmm0
	movq 64(%rsp), %r11
	movq 56(%rsp), %r10
	movq 48(%rsp), %rax
	movq 40(%rsp), %r9
	movq 32(%rsp), %r8
	movq 24(%rsp), %rcx
	movq 16(%rsp), %rdx
	movq 8(%rsp), %rsi
	movq 0(%rsp), %rdi
	movq %rbp, %rsp
	popq %rbp
	jmp *(%r11)
	.size __mcuda_mux_first, . - __mcuda_mux_first
