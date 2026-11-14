/* Every form of instruction the assembler knows; tests/x64as.sh compares the bytes with those of the
   assembler of an x86-64 machine. (No forward jumps: this assembler does not shorten them.) */
	.text
f:
	addq $1, %rax
	addl $1000, %ecx
	addb $3, (%rdi)
	addw $300, 4(%rsi,%rdx,2)
	subq %rax, %r12
	subq 8(%rsp), %r13
	xorl %eax, %eax
	cmpq $-1, -8(%rbp)
	cmpb %al, %sil
	orl %r8d, (%r9)
	andq $-16, %rsp
	adcq $0, %rdx
	sbbl %ebx, %ebx
	movq %rsp, %rdi
	movl $5, %eax
	movq $-1, %rax
	movq $0x123456789a, %r10
	movabsq $0x1122334455667788, %rcx
	movb $1, %ah
	movb %ah, %bl
	movw $7, %ax
	movq %fs:0, %rax
	movq %rax, %fs:16
	movl %edx, 0x12345(%r13)
	movq (%r12), %rax
	movq (%r13), %rax
	movq (%rsp), %rax
	movq (%rbp), %rax
	movq (,%rax,8), %rdx
	movq 16, %rax
	movq f(%rip), %rax
	leaq f(%rip), %rax
	leaq 8(%rax,%rax,4), %rdx
	leal (%rdi,%rsi), %eax
	testq %rax, %rax
	testb $1, %dil
	testl $0x100, 8(%rdi)
	xchgq %rax, (%rdi)
	notq %rax
	negl %ecx
	mulq %rcx
	divq (%rdi)
	idivl %esi
	imulq %rsi, %rdi
	imulq $10, %rsi, %rdi
	imull $1000, (%rdi), %eax
	imulq $3, %rax
	incq %rax
	decl (%rdi)
	shlq $3, %rax
	shrq %rax
	sarl %cl, %edx
	sarq $1, %rax
	rolw $4, %ax
	pushq %rbp
	pushq %r15
	pushq $1
	pushq $1000
	pushq 8(%rax)
	popq %r12
	popq %rbp
	call f
	call g@PLT
	call *%rax
	call *8(%rax)
	jmp f
	jmp *%r11
	je f
1:	ja 1b
	jmp 1b
	sete %al
	setne (%rdi)
	setl %r9b
	cmovgeq %rax, %rdx
	cmovel (%rdi), %eax
	movzbl %al, %eax
	movzbl (%rdi), %eax
	movzwl %ax, %eax
	movzbq %sil, %rax
	movsbl %al, %eax
	movswq (%rdi), %rax
	movslq %edi, %rax
	movslq 4(%rdi), %r9
	btq $5, %rax
	btsl %ecx, (%rdi)
	bsfq %rax, %rcx
	bsrl %eax, %ecx
	popcntq %rax, %rax
	bswapl %eax
	bswapq %r9
	lock xaddq %rax, (%rdi)
	lock cmpxchgl %esi, (%rdi)
	lock; cmpxchgq %rsi, (%rdi)
	shldq $3, %rax, %rdx
	shrdl %cl, %eax, %edx
	rep movsb
	rep stosq
	cltq
	cqto
	cltd
	syscall
	ret
	ret $8
	leave
	nop
	nopw 0(%rax,%rax,1)
	int $3
	ud2
	pause
	mfence
	addsd %xmm1, %xmm0
	mulss (%rdi), %xmm9
	divsd 8(%rsp), %xmm15
	sqrtsd %xmm0, %xmm0
	sqrtss %xmm1, %xmm2
	ucomisd %xmm1, %xmm0
	comiss (%rax), %xmm3
	cvtsi2sdq %rax, %xmm0
	cvtsi2sdl %eax, %xmm0
	cvtsi2ss %r9, %xmm1
	cvtsi2ssl (%rdi), %xmm1
	cvttsd2si %xmm0, %rax
	cvttss2si %xmm0, %eax
	cvtsd2si (%rdi), %r10
	cvtss2sd %xmm0, %xmm0
	cvtsd2ss %xmm0, %xmm0
	movsd %xmm0, (%rdi)
	movsd (%rdi), %xmm0
	movsd %xmm1, %xmm2
	movss %xmm0, 4(%rsp)
	movaps %xmm0, %xmm8
	movups %xmm0, (%rdi)
	movdqa (%rsi), %xmm1
	movdqu %xmm1, (%rdi,%rcx)
	movd %eax, %xmm0
	movd %xmm0, %eax
	movq %rax, %xmm0
	movq %xmm0, %rax
	movq %xmm0, (%rdi)
	movq (%rdi), %xmm0
	movq %xmm1, %xmm0
	xorps %xmm0, %xmm0
	andpd f(%rip), %xmm0
	pxor %xmm1, %xmm1
	pcmpeqb (%rdi), %xmm0
	pmovmskb %xmm0, %eax
	pshufd $0x1b, %xmm0, %xmm1
	shufps $0, %xmm0, %xmm0
	psllq $4, %xmm0
	psrldq $8, %xmm11
	psrlq %xmm1, %xmm0
	punpcklqdq %xmm1, %xmm0
	ldmxcsr 4(%rsp)
	stmxcsr -4(%rsp)
	movmskpd %xmm0, %eax
	movl $f, %eax
	movq $f, %rax
	movq tv@tpoff, %rax
	leaq tv@tpoff(%rax), %rdx
	cmpl $f, %edi
	addq $f+4, %rax
	movw %ax, %ds
	movw %fs, %ax
	cmpxchg16b (%rdi)
	.data
	.quad f
	.long f - .
