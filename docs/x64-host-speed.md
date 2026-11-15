# x86-64 host code: mcc against clang on ufsm (2026-11-15, mcc-host)

Goal: ufsm's CPU paths (sampler, label builders, rasters, fit, the volcomp codec) as fast under mcc as under
clang -O3 -march=native -ffast-math -fno-finite-math-only -ffp-contract=fast.

## Measure

- ufsm tree: origin/mccround-20261115 (the toolchain-mcc presets), `verify` (mcc) and `llvm-verify` (clang + nvcc) build dirs of
  the same commit; the 120 non-GPU, non-slow lines of tests/suite.txt.
- Each line under `perf stat -e cycles:u,instructions:u`, UFSM_NCPU=4, the two builds alternated, min of 3 (2 for the
  mcc-vs-mcc rounds). Scripts: /vesuvius/ufsm/microc/host/{buildu,runab,summ,bench,profab}.sh; per-symbol A/B: profab.sh.
- The desk is shared, so cycles:u varies up to ~15 % between runs. Single changes are judged on min-of-5 with the builds
  interleaved (bench.sh, UFSM_NCPU=1), and on instructions:u, which is deterministic.

## Baseline (microc main 475ac78)

- 73 lines where both builds pass and clang takes more than 0.2 G cycles: mcc 950.8 vs clang 667.4 G cycles, **1.425x** total,
  1.40x geometric mean.
- Largest absolute gaps (G cycles, clang / mcc):

| test | clang | mcc | ratio |
|---|---|---|---|
| test_repair | 72.6 | 105.0 | 1.45 |
| test_spatial_augment part 1 | 30.9 | 57.8 | 1.87 |
| test_wind_sampler | 34.9 | 54.2 | 1.55 |
| test_zcache | 42.2 | 59.8 | 1.42 |
| test_volcomp_fast | 22.1 | 39.0 | 1.76 |
| test_reg | 32.8 | 45.1 | 1.38 |
| test_scrollaug | 19.7 | 30.7 | 1.56 |
| test_strip | 18.6 | 28.8 | 1.55 |
| test_fit_emb | 21.5 | 31.5 | 1.46 |
| test_crush | 24.4 | 33.3 | 1.37 |

- Worst ratios (small tests): papyrus 4.6, votes 2.9, sample_ops 2.6, eval 2.5, tfx 2.3, scan_augment 2.3.

## Causes found (perf annotate, both binaries)

1. **Register allocation.** Linear scan spills a whole interval to the stack: it does not split live ranges. It has 12
   general registers (r10 / r11 are scratch, rbp is the frame pointer) and 13 xmm registers (xmm13-15 are scratch).
   - In the hot loops, stack loads and stores carry 22 % of the samples in volcomp's lossless decoder
     (vllx_decode_sub_sink), 42 % in the lossy one (vfx_decode_subs_sink) and 39 % in fit_vox2.
   - In loop latches, phi copies between two spilled values go memory to memory.
   - Spill-slot sharing across copies is mcc-scalar's slotco-20261115.
2. **Missed vectorisation of scalar loops** (mcc-vec's area). write_spatial_tm (sample.c scalar path) is 2.97x clang,
   filt_axis (reg) 2.33x, and papyrus, votes and sample_ops are near the top of the ratio list.
3. **Calls and branches clang does not have.**
   - isnan / isinf / isfinite / signbit were PLT calls in every guard: glibc's <math.h> calls __isnan etc. because mcc
     defines __SUPPORT_SNAN__.
   - An inlined function that returns a truth value is tested again after its returns meet in a phi of constants
     (`mov $1; movzbl; test; je`): there is no jump threading.
   - Pure libm calls (sin / cos / atan2 / hypot) are not hoisted out of loops, nor kept across calls to non-pure
     functions, nor fused into sincos. They cost +109 M atan2 and hypot calls in test_wind_sampler's fixture, and
     +33 M sin and +17 M cos in test_reg's.
4. Smaller items: no shlx / shrx (variable shifts pin rcx), no induction-variable widening (movslq per iteration), jumps to
   jumps (relax() does not thread them), and the callee-saved registers are saved even on early exits.

## Changes (each with a kill switch; gate green: tests/c in all variants, -g, -march=native, thr, as, abi, self-host)

| branch | what | kill switch | ufsm effect |
|---|---|---|---|
| br2const-20261115 | **miscompile fix**: after an and / or chain of comparisons (FUSE_BR2), a constant loaded after the chain's first branch was reused by the next block | (bug fix) | test_geom_check passes again with inline isfinite |
| fpclass-20261115 (on br2const) | isnan / isinf / isfinite / signbit in line (the __isnan / __isinf / __finite / __signbit calls) | MCC_NOFPCLASS | spatial_augment part 1: -4 % cycles, 217.9 -> 213.4 G instructions |
| fpo-20261115 | frame addressed from rsp, rbp allocatable (not with -O0 / alloca / VLA / frame_address / asm); CFI def_cfa_offset | MCC_NOFPO | about -1 % instructions; volcomp_fast -2.5 % cycles |
| shlx-20261115 | BMI2 shlx / shrx / sarx for shifts by a register | MCC_NOSHLX | -1..-4 % instructions in the decoders; cycles level on Zen 5 |
| jthread-20261115 | jump threading through a block whose branch its predecessors decide (a phi of constants) | MCC_NOJT | zcache 350 -> 288 G instructions, -9 % cycles; volcomp_fast -3 %; held: it exposes the opt_vec bug below |

Whole CPU suite against main, 2 reps, outputs compared (pass / fail of every line):
- br2const + fpclass + fpo + shlx (r4): no line changes its status; total -1.3 % cycles, geometric mean -1.1 % over 74 lines
  (spatial_augment part 1 -3.9 %, crush -4.8 %, score_par -2.6 %, render_stream -3.4 %).
- with jthread as well (r3): total -2.9 %, geometric mean -1.9 %. test_label_versions and test_gt_gate fail: the opt_vec bug
  below shows up in vll_encode once the frame moves. At -O2 (no vectoriser) jthread passes them.

## Open bug found on the way (mcc-vec)

An early-exit search loop, `for (i = 1; i < n; i++) if (b[i] != b[0]) break;`, is vectorised as `vsum i32 v0`: no load, no
compare, so it sums leftover xmm0 contents. volcomp's lossless encoder has two such loops (vll_encode, vll_encode_block), so
an mcc build can write corrupt chunks. Whether it does depends on the frame layout. Repro: /vesuvius/ufsm/microc/host/repro/vsum_v0.c
(main -O3: 4080 instead of 112).
