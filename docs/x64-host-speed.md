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

## Memory: which object an address is in (2026-11-16, memalias-20261116)

Before: gvn's memory table and licm told two addresses apart only when both were a local or a global named directly, when they
had the same base value and constant offsets, or by the kind of access (float against int ...). Every call, block copy and
vector store forgot everything, and a loop head started from nothing.

Now (src/opt.c, `roots_apart`; MCC_NOALIASROOT turns all of it off):
- The root of an address is what it is made from by adding and subtracting: a local, a global, a pointer parameter
  (`PARAM_PTR`, set by parse.c), an allocation (`IF_FRESH`: malloc, calloc, realloc, aligned_alloc not defined in the file).
  Different known roots are different objects; a parameter may still point into a global or where another parameter points,
  unless it is `restrict` or a record passed by value in memory (`PARAM_RESTRICT`; MCC_NORESTRICT: off).
- An address of unknown root (read from memory, a phi, a call's result) can be in a local, an allocation or a restrict
  parameter's object only if that one's address escaped (stored, passed to a call, merged in a phi or select, returned, used in
  other arithmetic). The scan is per function and does not follow the order of events.
- gvn: a call keeps what is known of objects it cannot reach; memcpy / memset / a vector store kill their own object only.
- gvn at a loop head (`mem_enter_loop`; MCC_NOMEMLOOP: off): what the ways in agree on, less what the loop's stores and calls
  may write, and only values the loop uses anyway or constants (MCC_MEMLOOPALL: all of them).
- licm: up to 48 stores per loop (16 before), block writes by object. Calls still stop every load (MCC_LICMCALLS: they spare
  what they cannot reach). MCC_NOLICMROOT: licm as before.

Instructions (G, one run; ufsm perf2 harness, tc built from commit 43b8c9f = "base"):

| test | base | roots (gvn + licm) | + by-value records, licm over calls | + loop head, all values | + loop head, used values (final) |
|---|---|---|---|---|---|
| test_spatial_augment | 844.64 | 841.99 | 844.10 | 849.92 | 841.82 |
| test_repair | 323.04 | 319.40 | 319.40 | 318.81 | 318.81 |
| test_fit_emb | 89.71 | 88.94 | 88.94 | 88.94 | 88.94 |
| test_strip | 91.47 | 91.23 | 91.23 | 91.39 | 91.17 |
| test_wind_sampler | 206.47 | 206.15 | 206.37 | 206.63 | 206.50 |
| test_volcomp_fast, test_reg | 93.95, 122.13 | same | same | +0.3, same | same |

test_zcache, test_scrollaug and test_crush vary by 3, 0.4 and 0.3 G between runs of one binary (190.85 - 194.16, 103.58 -
103.99, 97.29 - 97.59 for base), more than the changes move them; test_wind_sampler by 0.15, the other six by 0.02. The seven
steady ones go from 1771.42 to 1763.34 (-0.46 %); all ten 2163.84 / 2166.44 (base, two runs) to 2155.54 / 2157.12. Gate green
(tag ma-6), the ten tests pass.

What this showed: **a load removed is not an instruction removed.** A value kept from before a loop, or moved out of one,
needs a register through the loop. Where there is none it is read from a stack slot instead of from where it was (no gain), and
where the read was the memory operand of a compare (`cmpl $0, 0x10(%rbx)`) it becomes a load and a test (a loss). Two steps were
net losses for this reason and are off or narrowed: licm over calls (warp_w4_ref's `sym sy` fields: +2 G) and keeping every
known value through a loop (+8 G in test_spatial_augment). The allocator could take these back by reading a spilled value from
the memory it was loaded from when nothing in between can write there (roots_apart answers that), and by using a stack slot as
the operand of a compare.

Where the remaining non-stack loads and stores are (perf annotate, memory operands that are neither stack nor constants):
fit_vox2 11 % of its instructions, nearly all fields of `f` read once; fit_grad_terms: `dg[d] +=` and `lp[k] +=` through
pointers to the caller's locals (clang has them in registers because it inlines fit_grad_terms into the loop); coh_run:
`acc[ri * 64 + si] +=`, a histogram; filt_axis, emb_at, vll_rebuild: loops clang vectorises 16-64 wide and mcc 8 wide or not
at all. None of these is an aliasing question.

Not done: pointers against integers of 64 bits by type (both are M_64; fit_vox2 reads f->delta again after each `ic[c] =`),
a load made available on the one way into a join that lacks it, dead stores, pointers that step through a phi (the root is
lost there, and the base counts as escaped).

## Round 3 (2026-11-16 / 17, hostperf-20261116): instructions on ten tests

Yardstick: user instructions (`perf stat -e instructions:u`, UFSM_NCPU=1) of ten CPU tests on forlindesk2 (Zen 5):
test_volcomp_fast, test_zcache, test_wind_sampler, test_spatial_augment, test_reg, test_scrollaug, test_strip, test_fit_emb,
test_crush, test_repair (UFSM_TEST_QUICK=1). Cycles were not usable (the desk was loaded). Repeatable to ~0.1 % except
test_zcache (+-2 G).

| | G instructions | vs clang |
|---|---|---|
| clang | 1547 | 1.00 |
| mcc main 16cd2b7 | 2292 | 1.48 |
| + loop splits of written registers, vex3, float blend, bzhi, select fold, reload splits, gather hint | 2145 | 1.39 |
| + memalias, veccover, branchfold branches | 2064 | 1.33 |
| + spill weights apart from layout weights | 2056 | 1.33 |
| + host inline limit 20, no-write library calls | 2029 | 1.31 |
| + xmm16-31 on AVX-512 targets (xmm16-20261117) | 2008 | 1.30 |
| + inlining: 280 where a local's address is passed, 500 for a function called once | 1975 | 1.28 |
| + vectors chosen by ?: as values, unasked unrolling up to 3000 instructions | 1958 | 1.27 |
| + 32-bit loop counters widened (iv_widen) | 1941 | 1.25 |

Per test now (mcc / clang): volcomp_fast 80.6 / 68.0, zcache 177 / 199, wind_sampler 184 / 130, spatial_augment 808 / 560,
reg 119 / 78.8, scrollaug 99 / 67.8, strip 87 / 56.9, fit_emb 84.8 / 67.0, crush 91.6 / 70.7, repair 298 / 249.

What went in (each with its switch):

- x64 `split_loops`: a spilled register a loop writes gets the loop's own register (copy in at the preheader, copy back
  on each way out where it is alive; outer loops first). `MCC_NOWSPLIT`, `MCC_SPLITROUNDS=N`.
- x64 `split_reloads`: a spilled register read twice or more in a call-free stretch of a block is loaded once.
  `MCC_NORSPLIT`, `MCC_RSPLIT=1|2`.
- AVX scalar float arithmetic with three operands (`MCC_NOVEX3`); a float chosen by a float comparison as vcmpss +
  vblendvps (`MCC_NOFBLEND`); `x & ((1 << n) - 1)` as bzhi (`MCC_NOBZHI`); `x == k ? c : e` where e is c at x == k
  (`MCC_NOSELSAME`); a gather writes its result register and the allocator keeps that apart from the index
  (`MCC_NOGATHERD`); uitof of a non-negative 32-bit value as the signed conversion.
- Spill weights come from the block frequency guess without the if-body rule; the layout keeps the rule
  (`MCC_ONEFREQ`). With one guess for both, test_zcache lost 14 G.
- Host inline limit 20 instructions (was 12; `-finline-limit=N`, `MCC_INLINELIMIT=N`): -27 G, no larger binaries; 32 and
  48 are no better.
- Library calls that write nothing but errno (getenv, strlen, strcmp, atof ...) keep what gvn knows of locals and
  named globals (`MCC_NONOWRITE`).
- xmm16-xmm31 on targets with AVX-512 F, VL, BW, DQ (`MCC_NOXMM16`; `MCC_XMM16=2` takes them first, for testing;
  `MCC_NODSPLIT`, `MCC_NOVSPLIT`): the allocator runs with xmm0-12 first and again with the high registers only if xmm
  values spilled; comparisons into a mask, blends, gathers, hadd and the 128-bit legacy path keep their operands low.
  -20 G: the stack traffic left in fit_vox2 / write_spatial_tm is values alive across calls, which no xmm register
  survives.
- Inlining (`opt_inline.c`): a callee passed the address of a local up to 280 instructions (was 160:
  `-finline-slotarg=N`, `MCC_INLINESLOT`; spatial_aug_pull into its three callers, -26 G); a function called from one
  place up to 500 (was 200, `MCC_INLINEONCE`; -6 G). No gain from: the `inline` keyword's limit above 48
  (`MCC_INLINEHINT`), a larger limit at call sites inside loops, called-once functions of more than 48 blocks (zcache
  +10 G: the big function's registers).
- A vector chosen by `?:` is copied into one temporary in each arm and becomes a value (`MCC_NOCONDVEC`); it was
  chosen by its address, so both arms were stored and the result loaded through a pointer (write_spatial_tm's gathers).
- Unasked unrolling up to 3000 instructions (was 1200; `MCC_UNROLLINS=N`): -16 G, 4 % more code. (With the inlining
  above the earlier loss on test_zcache turned into a gain of 10 G.)
- `iv_widen` (opt.c, last before selection, `MCC_NOIVWIDEN`, `MCC_IVWLOG`): a 32-bit counter read widened becomes a
  64-bit counter where i + step cannot wrap (the way back is taken only while next < n and the first value is a
  constant, known small or was compared before the loop; or the header leaves unless i < n); `(long)(i - k)` of a
  counter that goes up from a known first value too. movslq went from 5 % to 1-2 % of the tests' own instructions;
  -15 G.
- From the branches: see "Memory" above; opt_vec.c narrowing / narrow lanes / exact counts / bit reductions
  (`MCC_NOVECNARROW`, `MCC_NOVECZX`, `MCC_NOVECEXT`, `MCC_NOVECEXACT`, `MCC_NOVECBITRED`, `MCC_VECEPI=1`); jump threading
  on ranges, selects value-numbered, loop exits and if bodies in the frequency guess, address sums reshaped
  (`MCC_NOJTR`, `MCC_NOSELCSE`, `MCC_NOLOOPX`, `MCC_NOIFIN`, `MCC_NOADDRSHAPE`, `MCC_NOIFCSH`).

Measuring, learned the hard way:

- `perf record -e instructions:u -c 1000003` lost two thirds of its samples on the loaded desk, and not evenly: the
  per-function tables it gave were wrong. At `-c 10000019` the sample total equals perf stat's count. Check that sum.
- The profile's branch class is skid-biased; `perf record -b` gives exact branch counts.
- `worker` in test_scrollaug / test_wind_sampler is a spin wait: ~20 G of mcc's count there is waiting, not code.
- Much of test_spatial_augment's excess (~120 G of 250) is the test's reference code (write_spatial_ref, warp_*_ref,
  ws_cmp), not production paths.

Where the rest is (10M-instruction samples over the ten tests, clang 153 k, mcc 204 k): stack loads +126 G, arithmetic
+134 G, library code +65 G (more calls: helpers not inlined into loops, so their hypot / atan2 are not merged with
the caller's), other loads +60 G, stack stores +41 G, branches +32 G, copies +31 G, other stores +18 G.

- Float / vector values alive across calls (fit_vox2, write_spatial_tm): every xmm register is caller-saved, so they
  live in memory; splitting the range around the call (reload once after it) is what is left to do.
- By mnemonic (mn.sh), against clang, per test: compare results made into 0 / 1 and tested again (setcc, movzbl,
  test), `xor r, r` for a zero inside loops (a cmov's operand), xorps before cvtsi2sd, twice the register copies.
- Small leaf functions use callee-saved registers and pay six saves and restores (rblk_sample); r10 / r11 are emission
  scratch and not allocatable.
- Vectoriser: filt_axis (clamped gather, strict reductions), 16-trip loops the parser unrolls first (needs SLP),
  f32 / f64 mixed lanes.
- Layout: unconditional jumps are still several times clang's.

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
