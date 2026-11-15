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

## Round 1 changes (each with a kill switch; gate green: tests/c in all variants, -g, -march=native, thr, as, abi, self-host)

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

## Search-loop bug (fixed by mcc-scalar)

An early-exit search loop, `for (i = 1; i < n; i++) if (b[i] != b[0]) break;`, was vectorised as `vsum i32 v0`: no load and no
compare, so it summed whatever was left in xmm0. volcomp's lossless encoder has two such loops (vll_encode, vll_encode_block),
so an mcc build could write corrupt chunks, depending on the frame layout. Repro: /vesuvius/ufsm/microc/host/repro/vsum_v0.c
(main -O3: 4080 instead of 112). Fixed in microc 384e827 (vsearchfix-20261115, mcc-scalar; regression test tests/c/vsearch2).

## Round 2 (2026-11-15, drain)

All numbers below are on ufsm master a580a6c. The base is vsearchfix-20261115 ("vsf"), because main has the search-loop bug.

| branch | state | ufsm effect (min of 5 unless noted) |
|---|---|---|
| jthread3-20261115 (a398d66) = jthread2 (my threading on vsf) + mcc-scalar's thread2-20261114 | gate green; handed to microc2. It replaces jthread-20261115 and thread2-20261114 | full CPU suite vsf vs jthread2, 2 reps: pass / fail identical (the same 6 lines fail on both builds), total -1.1 %. zcache 61.3 / 55.5 / 53.8 G cycles (base / jthread2 / +thread2), 341 / 286 / 276 G instructions; volcomp_fast 38.1 / 37.0 / 37.0; crush -4.5 %, scrollaug -3.4 % |
| wip-purelicm-20261115 | gate green; WIP (no full-suite run) | a pure math call with loop-invariant arguments leaves the loop when every trip runs it. Instructions: reg -0.3 %, wind_sampler -0.1 %. The extra atan2 time in wind_sampler is the same number of calls each taking longer, not extra calls |
| wip-edgew-20261115 | gate green; WIP (mixed) | layout weights a branch edge into a block of several predecessors by flow (b - other way), and relax threads a jump to a jump. Min of 3: papyrus 2.10 -> 1.49 G cycles (-29 %), eval -1 %, votes -2 %, but zcache +3.5 % instructions (+2 % cycles). Next step: bisect with MCC_NOEDGEW / MCC_NOJJ |

Other findings:
- Binary size (477 MB per mcc test binary vs 68 MB with clang): 461 MB of it is uncompressed PTX text in .rodata (168 modules,
  159 distinct). Host .text is smaller under mcc (9.3 vs 10.3 MB in test_ink_pool). zstd -3 shrinks the rodata to 73 MB in
  0.3 s. Handed to testspeed (link / embed side).
- Small tests with the worst ratios are fixture loops: papyrus (6-deep brute-force min / max with an invariant `continue`;
  clang unswitches it), eval / votes (dilation references; the uint8 stores alias the n[] bounds, so their loads stay inside
  the loop; layout as in edgew).
- Left to others: register allocation (live-range splitting, remat, slot sharing: mcc-scalar's rasplit / slotco) and scalar
  loops clang vectorises (mcc-vec). These are the main remaining gap.

## Appendix: every CPU-suite line, clang vs mcc main 475ac78 (round 1, G cycles, min of 3)

| test | clang | mcc | mcc / clang | note |
|---|---|---|---|---|
| test_afv | 0.012 | 0.005 | 0.45 | |
| test_annot | 0.075 | 0.112 | 1.49 | |
| test_aug_preset | 0.322 | 0.523 | 1.62 | |
| test_checkpoint_runtime | 0.005 | 0.004 | 0.99 | |
| test_ckring | 0.005 | 0.005 | 0.99 | |
| test_codes_grid | 0.140 | 0.096 | 0.68 | |
| test_coreline | 0.763 | 1.117 | 1.46 | |
| test_cover | 0.006 | 0.006 | 0.99 | |
| test_ct_augment | 0.202 | 0.269 | 1.33 | |
| test_dcache_batch | 0.177 | 0.269 | 1.52 | |
| test_dcache_cap | 0.043 | 0.043 | 1.00 | |
| test_dcache_phase | 11.171 | 11.700 | 1.05 | |
| test_dino_pts | 0.067 | 0.104 | 1.54 | |
| test_edt | 2.192 | 2.532 | 1.15 | |
| test_eval | 1.618 | 4.057 | 2.51 | |
| test_faces | 0.169 | 0.193 | 1.14 | |
| test_fblend | 0.158 | 0.128 | 0.81 | mcc fails |
| test_fiberjudge | 0.549 | 0.950 | 1.73 | |
| test_fiber_link | 0.010 | 0.004 | 0.36 | |
| test_fibertrace | 0.802 | 0.907 | 1.13 | |
| test_fibertrace_dyad | 3.308 | 3.686 | 1.11 | |
| test_fields_pack | 1.743 | 3.254 | 1.87 | |
| test_fields_phase | 0.212 | 0.255 | 1.20 | mcc fails |
| test_fit_dir | 20.572 | 25.138 | 1.22 | |
| test_fit_emb | 21.525 | 31.482 | 1.46 | |
| test_flatten | 12.079 | 19.328 | 1.60 | |
| test_formats | 0.006 | 0.006 | 1.03 | clang fails mcc fails |
| test_fuse | 0.174 | 0.275 | 1.59 | |
| test_geom_check | 2.442 | 4.186 | 1.71 | |
| test_gn_ema_avg | 0.075 | 0.014 | 0.19 | |
| test_gns | 9.460 | 11.384 | 1.20 | |
| test_gpuq | 0.857 | 0.844 | 0.98 | |
| test_gt_gate | 0.323 | 0.462 | 1.43 | |
| test_http_reader | 0.228 | 0.220 | 0.96 | clang fails mcc fails |
| test_imgops | 0.043 | 0.037 | 0.86 | |
| test_ingest_lut | 0.126 | 0.126 | 1.00 | clang fails mcc fails |
| test_ink_blend | 0.096 | 0.097 | 1.01 | clang fails mcc fails |
| test_ink_gate | 0.512 | 0.858 | 1.67 | mcc fails |
| test_ink_pool | 1.295 | 2.123 | 1.64 | |
| test_jpegpar | 2.077 | 3.135 | 1.51 | |
| test_json | 0.069 | 0.100 | 1.45 | |
| test_label_versions | 1.008 | 1.456 | 1.44 | |
| test_mcc_compile | 0.005 | 0.005 | 1.02 | |
| test_mgpu_lint | 10.261 | 10.028 | 0.98 | |
| test_optcheck | 0.003 | 0.003 | 1.01 | |
| test_pairacc | 1.029 | 1.168 | 1.13 | |
| test_patchid | 19.541 | 25.669 | 1.31 | |
| test_patchsolve | 1.069 | 1.146 | 1.07 | |
| test_phase_check | 0.050 | 0.071 | 1.41 | |
| test_phaselab | 0.619 | 0.928 | 1.50 | |
| test_phasepack | 1.967 | 3.267 | 1.66 | |
| test_podscripts | 0.016 | 0.016 | 1.00 | |
| test_pyports | 0.089 | 0.031 | 0.35 | |
| test_raster | 25.032 | 32.771 | 1.31 | |
| test_recto | 1.875 | 2.462 | 1.31 | |
| test_reg | 32.778 | 45.122 | 1.38 | |
| test_reg_render | 2.687 | 3.934 | 1.46 | |
| test_regz2 | 0.011 | 0.012 | 1.08 | |
| test_render_absent | 0.021 | 0.024 | 1.15 | |
| test_render_fill | 0.092 | 0.154 | 1.67 | mcc fails |
| test_render_known | 0.877 | 0.682 | 0.78 | mcc fails |
| test_render_stream | 11.330 | 16.305 | 1.44 | |
| test_repair | 72.618 | 105.022 | 1.45 | |
| test_replay_lean | 4.598 | 6.586 | 1.43 | |
| test_retain | 0.322 | 0.323 | 1.01 | |
| test_run_suite | 0.808 | 0.807 | 1.00 | |
| test_sample_ops | 1.127 | 2.906 | 2.58 | |
| test_sampler_safety | 0.091 | 0.088 | 0.98 | clang fails mcc fails |
| test_scan_augment | 1.800 | 4.069 | 2.26 | |
| test_score_par | 55.967 | 62.919 | 1.12 | |
| test_seamless | 0.656 | 0.638 | 0.97 | clang fails mcc fails |
| test_sheetcut | 0.048 | 0.042 | 0.89 | |
| test_sheetpbr | 5.641 | 6.975 | 1.24 | |
| test_sheetscore | 0.366 | 0.534 | 1.46 | |
| test_side | 0.585 | 0.715 | 1.22 | |
| test_sources_json | 0.010 | 0.010 | 1.04 | |
| test_spatial_augment~part0of3 | 19.524 | 30.053 | 1.54 | |
| test_spatial_augment~part1of3 | 30.892 | 57.841 | 1.87 | |
| test_spatial_augment~part2of3 | 23.361 | 30.212 | 1.29 | |
| test_sslstat | 0.005 | 0.015 | 3.11 | |
| test_stitch | 2.243 | 2.909 | 1.30 | |
| test_strip | 18.641 | 28.843 | 1.55 | |
| test_stripdistort | 1.065 | 1.129 | 1.06 | |
| test_surf | 1.419 | 1.599 | 1.13 | |
| test_surflabels | 120.680 | 158.815 | 1.32 | mcc fails |
| test_sysinfo | 0.006 | 0.006 | 1.00 | |
| test_tfx | 2.933 | 6.657 | 2.27 | |
| test_tiffw | 0.044 | 0.037 | 0.84 | |
| test_typed_zarr | 3.114 | 5.577 | 1.79 | |
| test_unroll_qa | 3.689 | 4.345 | 1.18 | |
| test_uw_fixed_point | 0.079 | 0.015 | 0.19 | |
| test_volcomp | 0.006 | 0.010 | 1.57 | |
| test_volcomp_fast | 22.127 | 39.014 | 1.76 | |
| test_votes | 1.546 | 4.405 | 2.85 | |
| test_vwt_sample | 0.075 | 0.014 | 0.19 | |
| test_wbench_stats | 0.388 | 0.062 | 0.16 | |
| test_winauto | 0.003 | 0.003 | 1.02 | |
| test_wind | 0.520 | 0.742 | 1.43 | |
| test_wrap_check | 0.463 | 0.535 | 1.16 | |
| test_wraprepair | 4.787 | 7.345 | 1.53 | |
| test_wraps_core | 5.063 | 6.556 | 1.29 | |
| test_wrapsnap | 5.512 | 9.281 | 1.68 | |
| test_z3_autosub | 1.817 | 2.482 | 1.37 | |
| test_z3_sink | 4.117 | 7.082 | 1.72 | |
| test_zarr | 0.005 | 0.005 | 0.90 | |
