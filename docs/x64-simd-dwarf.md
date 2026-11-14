# x86-64: vector lowering and DWARF line tables (design, 2026-11-14)

Why: on ufsm's 59 CPU tests, mcc's host code is 3.6x slower than clang -O3 -march=native. clang vectorises the hot loops
(augmentation, the sampler, volcomp) and runs the AVX2 intrinsic paths. mcc has neither on x86-64: `opt_vectorise` is skipped
when `opt_arch == ARCH_X64` (opt.c), x64.c dies on `IT_V128`, and GNU vector operations are split into scalar element
operations by the parser (`vec_binop`, `vec_put`). Separately, `-g` emits nothing, so perf, gdb and crash backtraces show no
source lines.

## 1. v128 on x86-64 (SSE4.1 baseline, the existing opt_vec loops)

Mirror arm64's design: seven machine ops; the lane kind is carried in `MIns.cc` and decoded in the encoder.

| IR (opt_vec.c) | x64 MIR | encoding (SSE / SSE4.1, legacy 2-operand; d tied to s0 the way the scalar float ops are) |
|---|---|---|
| OP_VLOAD a+b | X_VLDR | movdqu xmm, [a+b] (F3 0F 6F); movups for float lanes (0F 10) |
| OP_VSTORE | X_VSTR | movdqu [a+c], xmm (F3 0F 7F) |
| OP_VSPLAT | X_VDUP | int: movd/movq + pshufd 0 (32) / punpcklqdq (64) / pshufb with a zero mask (8) or pshuflw+pshufd (16); float: shufps 0 |
| arithmetic, IT_V128 | X_VBIN imm = IR op | per lane kind: padd{b,w,d,q} psub* pmull{w,d} (SSE4.1) pand por pxor; addps subps mulps divps (and pd); psll/psrl/psra by imm; pcmpeq/pcmpgt (+ pxor for !=, swap for <); cvtdq2ps / cvttps2dq |
| OP_VSMAX/MIN, VUMAX/MIN | X_VBIN | pmaxs{b,w,d} pmins* pmaxu* pminu* (SSE4.1; 64-bit lanes: compare + blend) |
| OP_SELECT (lanes) | X_VSEL | pblendvb with the mask in xmm0, or pand/pandn/por without the fixed register |
| OP_VSUM | X_VSUM | pshufd / paddd folding to one lane, then movd; the widening sums: pmaddwd / psadbw |
| OP_VIOTA | X_VIOTA | a 16-byte constant in .rodata, then movdqu |

- Register class: the xmm registers already exist for scalar floats (XMM0..XMM15, FSCR0/1). ra.c needs the "all 128 bits
  matter" class bit that arm64 uses (`cls_of`) so that copies of whole registers are movdqa. Spills are already avoided:
  opt_vec bounds the number of live vectors.
- Turn it on: drop `opt_arch != ARCH_X64` from the `opt_vectorise` condition in opt.c, and the die in x64.c.
- Test: tests/c in the -O3 variant on x86-64 (`make test-x64` already has variants), plus `fuzz-x64` against gcc. The
  vectoriser's existing NEON tests give the loop shapes.

## 2. Vector types as values (makes immintrin.h fast)

- `vector_size(16 / 32)` types become IR values (IT_V128 now, IT_V256 next) instead of element-wise memory temporaries:
  - the `vec_binop` / `vec_unary` / compare / cast paths emit one vector op;
  - `[i]` becomes an extract or insert op;
  - `__builtin_shufflevector` becomes a shuffle op with a constant pattern, lowered to pshufd / shufps / pshufb / vpermd.
- Then immintrin.h's element loops get rewritten as vector expressions or builtins (`__builtin_ia32_*`-style, one per
  instruction), each mapping to one MIR op.
- v256: VEX-encoded AVX2 (3-operand, no tie), ymm registers in the allocator (256-bit spill slots), and `vzeroupper` before
  calls and returns.
- Test: a parity file per intrinsic family, comparing the element-wise header (which is exact, see immintrin.h) with the
  lowered version on random and special inputs.

## 3. DWARF line tables (`-g`)

- Ins gets a `u32 loc` (set in `ir_ins` from a parser global holding the current statement's location; the passes copy it
  when they make instructions from others); MIns gets it too (isel copies it from the IR instruction).
- The x64 encoder records (code offset, loc) at every change of loc. `loc_physical` (lex.c) turns loc into file and line.
- elf.c writes `.debug_line` (DWARF 5: header, directory and file tables, one sequence per function: DW_LNE_set_address with an
  R_X86_64_64 relocation to the function's symbol, then DW_LNS_advance_line / advance_pc / copy rows), plus a minimal
  `.debug_info` (one DW_TAG_compile_unit with DW_AT_stmt_list, low_pc / high_pc per function as DW_TAG_subprogram) and
  `.debug_abbrev` / `.debug_str`. The ELF linker (eld.c) concatenates the debug sections and applies their relocations; with
  the glibc link done by `cc`, the system linker does it.
- That is enough for perf annotate, gdb `break file:line` and backtraces with lines. Variables (DW_TAG_variable with
  locations) are a later step.
- Test: `readelf --debug-dump=line` and `addr2line` on tests/c programs: every line that has code maps back to itself.
