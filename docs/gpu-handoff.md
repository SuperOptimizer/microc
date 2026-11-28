# Handoff: mcc's PTX on a real GPU

Goal: find out how the PTX that mcc writes for device code behaves on real hardware (correctness first, then speed
against nvcc), and improve it where it is slower. Nothing on this branch has been run through ptxas or on a GPU: every
PTX change so far was checked only against mcuda, the CPU run time.

Branch: `claude/loving-bardeen-a7emn9`. Notes of the work before this: `docs/x64-host-round-20261128.md`.

## 1. Build (on the pod: x86-64 Linux with NVIDIA's driver)

    make && make x64
    sh mkcuda.sh build/stage0/mcc build/stage0                                       # mcuda (CPU) and the headers
    HWCC="build/stage0/mcc -mglibc" sh mkcuda.sh build/stage0/mcc build/stage0 hw    # mcudahw: the run time on the driver

The first two lines were run in this session; the `hw` line was not (no driver here). Its form is the one the Makefile's
comment gives. tests/x64cuda.sh links `build/stage0/linux-x64/cuda/lib/libcudart.a`, which holds both run times and
picks by MCUDA_BACKEND; read mkcuda.sh and cuda/mux/mkmux.sh if the GPU side does not load.

## 2. Correctness on the device (item 8)

    X64_SSH="sh -c" X64_DIR=$HOME/x64 sh tests/x64cuda.sh build/stage0          # CPU: must print "cuda (x86-64): 73 pass"
    GPU=1 X64_SSH="sh -c" X64_DIR=$HOME/x64 sh tests/x64cuda.sh build/stage0    # the .cu programs on the GPU

- `X64_SSH="sh -c"` runs the tests on the machine itself. The CPU line is the sanity check that the build is whole.
- With GPU=1 the programs `mx` (wants sm_120a) and `ldg` (a misaligned vector load the device faults on) are left out
  by default (GPU_SKIP).
- Outputs land in build/x64cuda/out; each must equal tests/cuda/NAME.exp.
- tests/cuda/jitedge.exp is the CPU interpreter's output, not a device's. If the GPU differs there, the device is the
  authority: report which line (toint / tofloat / divide / calls / atoms) and fix mcuda, not the PTX. NaN results of
  float to 64-bit integer conversions and lround / lrint of values no long holds are the likely places.
- A kernel the driver rejects stops the program with the driver's message: that is a PTX bug in src/ptx.c. Keep the
  PTX (mcc -S or the embedded text; MCUDA_KLOG=FILE lists the kernels a process loads) and the message.

## 3. Speed against nvcc (item 9 and beyond)

No GPU timing harness exists in the repo. What is needed:
- a handful of kernels built twice, by mcc and by nvcc (if the pod has the CUDA toolkit), launched from the same host
  program and timed with events or cudaDeviceSynchronize around many launches; sizes large enough that launch overhead
  does not dominate. Candidate kernels used on the CPU side (not in the repo, easy to rewrite): a histogram with
  atomicAdd and lrintf, a map with expf / division / floorf, a polynomial map, a branchy map, an integer hash, a
  5-point stencil, a kernel that calls a __noinline__ device function;
- for any kernel where mcc is slower: both PTX texts side by side (nvcc -ptx or --keep), and if needed the SASS
  (nvdisasm or cuobjdump -sass) of both.

Known suspects in mcc's PTX, none measured on a device:
- float <-> int bit copies that remain (mov.b32 between .f32 and .b32 registers);
- loop-invariant work left inside loops (about 320 instructions over the tests): constants loaded in the loop body are
  probably free after ptxas, invariant arithmetic may not be;
- device functions that are not inlined (calls are cheap on a device, but check);
- anything ptxas warns about.

Switches in src/ptx.c that turn single PTX changes off, for bisecting a slowdown or a rejection: MCC_NOSHMASK,
MCC_NOADDRSPLIT, and the others `grep -o 'getenv("MCC_[A-Z0-9_]*")' src/ptx.c` lists.

## 4. Gates before any commit to src/

    X64_SSH=local sh tests/x64.sh build/stage0                                  # 415 passed
    X64_SSH="sh -c" X64_DIR=$HOME/x64 sh tests/x64cuda.sh build/stage0          # 73 pass (CPU)
    X64_SSH="sh -c" X64_DIR=$HOME/x64 sh tests/x64self.sh build/stage0          # stage 2 and stage 3 identical

- x64self.sh also prints "FAIL: the cross-built stage1 differs from stage2" on a glibc host: it did before any of this
  work and is not a regression.
- tests/thr/once fails about once in several hundred runs, before this work as after.
- Each change in this tree has a kill switch (MCC_NO...) and a line in the docs; keep to that.

## 5. What to report back

- GPU model, driver version, whether nvcc / ptxas are on the pod;
- pass / fail of every .cu program with GPU=1, with the driver's message for each failure;
- a table of kernel times, mcc against nvcc, and for each gap the PTX difference that explains it.
