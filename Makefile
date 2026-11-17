# Stage 0: build mcc with the host C compiler, in strict C89.
CC = cc
CFLAGS = -std=c89 -pedantic -Wall -Wextra -Werror -O2
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)
MCC = build/stage0/mcc
LIBC = build/stage0/lib/libc.a
LIBC_SRC = $(wildcard libc/src/*.c) $(wildcard libc/sys/*.s) $(wildcard libc/include/*.h) $(wildcard libc/include/sys/*.h)

all: $(MCC) $(LIBC)

$(MCC): $(SRC) $(HDR)
	@mkdir -p build/stage0
	$(CC) $(CFLAGS) -o $@ $(SRC)

# The C library, built by mcc itself, next to the compiler that built it.
libc: $(LIBC)

$(LIBC): $(MCC) $(LIBC_SRC) mklibc.sh
	@sh mklibc.sh $(MCC) build/stage0

test: $(MCC) $(LIBC)
	@sh tests/run.sh build/stage$(STAGE)/mcc

# The rest of the toolchain, all built by mcc: the C libraries for Linux programs and for programs of this
# machine, and mbox (the shell and the Unix tools a build needs). build/stage$(STAGE)/bin then holds every
# tool by name, and a build run with only that directory on PATH uses nothing of the system's.
tools: $(MCC) $(LIBC)
	@sh mklxlibc.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
	@sh mkhostlibc.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
	@sh mkbox.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
	@sh mkvm.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
	@sh mkdesk.sh build/stage$(STAGE)/mcc build/stage$(STAGE)

test-tools: tools
	@sh tests/box.sh build/stage$(STAGE)
	@sh tests/vm.sh build/stage$(STAGE)

# Atomics, thread-local storage and threads, in Linux programs run in mvm (needs a built kernel, see tests/thr.sh).
test-threads: tools
	@sh tests/thr.sh build/stage$(STAGE)

# x86-64 Linux: the C library for it, and the tests on an x86-64 machine (X64_SSH says how to reach one, see
# tests/x64.sh): the C tests, the thread tests, and the calling convention against that machine's C compiler.
x64: $(MCC) $(LIBC)
	@sh mklxlibc.sh build/stage$(STAGE)/mcc build/stage$(STAGE) x86_64
test-x64: x64
	@sh tests/x64.sh build/stage$(STAGE)
	@SUITE=thr sh tests/x64.sh build/stage$(STAGE)
	@SUITE=x64c sh tests/x64.sh build/stage$(STAGE)
	@sh tests/x64as.sh build/stage$(STAGE)
	@sh tests/x64abi.sh build/stage$(STAGE)
# The third-party libraries (see ulibs) for x86-64 Linux, and their tests on the x86-64 machine
ulibs-x64: tools x64
	@ARCH=x86_64 THREADS=$(THREADS) build/stage$(STAGE)/bin/sh mkulibs.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
test-ulibs-x64: ulibs-x64
	@sh tests/x64ulibs.sh build/stage$(STAGE)
# hostfs: a root file system for x86-64 Linux containers (build/hostfs/rootfs, hostfs.tar and a Dockerfile, see
# mkhostfs.sh) in which every program is built by mcc from this tree: the shell and tools, the compiler, the C
# library, the CUDA headers and, when their sources are under build/ext (libc/ext/fetch.sh), the third-party
# libraries. test-hostfs runs tests/hostfs/smoke.sh inside it on the x86-64 machine (X64_SSH, X64_DIR; it
# needs bubblewrap there, not Docker and not root); with UFSM=dir also ufsm's tests that need no GPU.
hostfs: tools x64 cuda
	@[ ! -d $(EXT)/zlib-1.4 ] || EXT=$(EXT) ARCH=x86_64 THREADS=$(THREADS) build/stage$(STAGE)/bin/sh mkulibs.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
	@build/stage$(STAGE)/bin/sh mkhostfs.sh build/stage$(STAGE)/mcc build/hostfs
test-hostfs: hostfs
	@UFSM=$(UFSM) GPU=$(GPU) UFSM_GPU_TESTS="$(UFSM_GPU_TESTS)" sh tests/hostfs.sh build/hostfs
EXT = $(CURDIR)/build/ext
UFSM =
GPU =
UFSM_GPU_TESTS =
# mcc cross-built for x86-64 Linux, then building itself and running the C tests there
test-x64-self: x64
	@sh tests/x64self.sh build/stage$(STAGE)
test-x64-pgo: x64
	@PGO=1 sh tests/x64self.sh build/stage$(STAGE)
# x86-64 Linux with the machine's GNU C library (--sysroot / -mglibc): mcc self-hosting and the C and thread
# tests there; the third-party libraries built that way and their tests. GLIBC_SYSROOT: the machine's
# /usr/include mirrored here (tests/x64glibc.sh fetches it).
GLIBC_SYSROOT = $(CURDIR)/build/x64glibc/sysroot
test-x64-glibc: x64
	@sh tests/x64glibc.sh build/stage$(STAGE)
ulibs-x64-glibc: tools x64
	@ARCH=x86_64 GLIBC=$(GLIBC_SYSROOT) THREADS=$(THREADS) build/stage$(STAGE)/bin/sh mkulibs.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
test-ulibs-x64-glibc: ulibs-x64-glibc
	@GLIBC=$(GLIBC_SYSROOT) sh tests/x64ulibs.sh build/stage$(STAGE)
# Random programs compared with the C compiler of the x86-64 machine (FUZZ_FIRST, FUZZ_COUNT: seeds)
fuzz-x64: x64
	@sh tests/x64fuzz.sh build/stage$(STAGE) $(FUZZ_FIRST) $(FUZZ_COUNT)

# The third-party libraries that ufsm links (zlib, zstd, blosc, libjpeg-turbo, libcurl + BearSSL, ...) for aarch64
# Linux, from the sources under build/ext (libc/ext/fetch.sh gets them), and their tests in mvm. THREADS=0: without pthreads.
ulibs: tools
	@THREADS=$(THREADS) build/stage$(STAGE)/bin/sh mkulibs.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
test-ulibs: ulibs
	@build/stage$(STAGE)/bin/sh tests/ulibs.sh build/stage$(STAGE)

# mcuda: the CUDA runtime that runs the PTX mcc writes for device code on the CPU (cuda/)
cuda: tools
	@build/stage$(STAGE)/bin/sh mkcuda.sh build/stage$(STAGE)/mcc build/stage$(STAGE)
test-cuda: cuda
	@build/stage$(STAGE)/bin/sh tests/cuda.sh build/stage$(STAGE)
# the same run time and tests for x86-64 Linux, on the x86-64 machine (X64_SSH, X64_DIR)
test-x64-cuda: x64 cuda
	@GPU=$(GPU) sh tests/x64cuda.sh build/stage$(STAGE)

# -flto through the system's linker (glibc mode, x86-64 Linux): tests/ltoglibc.sh
test-lto: $(MCC) $(LIBC)
	@sh tests/ltoglibc.sh build/stage$(STAGE)

# CUDA / C++ diagnostics and C++17 inline variables (tests/cuerr.sh)
test-cuerr: $(MCC) $(LIBC)
	@sh tests/cuerr.sh build/stage$(STAGE)

# kernel names (tests/mangle.sh): the Itanium C++ ABI's, as nvcc writes them, so profiles of mcc and nvcc builds match
test-mangle: cuda
	@sh tests/mangle.sh build/stage$(STAGE)
THREADS = 1

# Self-hosting check: each stage is built by the previous one, against the
# C library the previous stage built. Stages 2 and 3 must be identical.
bootstrap: $(MCC) $(LIBC)
	@for n in 1 2 3; do \
		prev=build/stage$$((n - 1)); cur=build/stage$$n; \
		mkdir -p $$cur && $$prev/mcc -o $$cur/mcc $(SRC) && sh mklibc.sh $$cur/mcc $$cur || exit 1; \
	done
	@cmp build/stage2/mcc build/stage3/mcc && cmp build/stage2/lib/libc.a build/stage3/lib/libc.a \
		&& echo "bootstrap: stage2 and stage3 are identical"

STAGE = 0
clean:
	rm -rf build

.PHONY: hostfs test-hostfs test-x64-cuda all libc test tools test-tools test-threads ulibs test-ulibs cuda test-cuda test-mangle test-lto test-cuerr bootstrap clean
