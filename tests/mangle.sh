#!/bin/sh
# mangle.sh DIR: the kernel names mcc (DIR/mcc, as mklxlibc.sh / mkcuda.sh installed it) writes for tests/mangle/mangle.cu must be
# the Itanium C++ ABI's, which are nvcc's: tests/mangle/mangle.exp (written by clang++ from the same declarations). On a
# difference the names that differ are printed. MCC_FLAGS: how the headers are found (default -mglibc).
D=${1:?DIR}; T=build/test/mangle
mkdir -p $T
$D/mcc ${MCC_FLAGS:--mglibc} -isystem cuda/devinc -include cuda/devinc/mcuda_device.h --cuda-arch=1200 -c tests/mangle/mangle.cu \
    -o $T/mangle.o --ptx-out=$T/mangle.ptx || { echo "FAIL mangle: does not compile"; exit 1; }
grep -oE '\.entry [A-Za-z0-9_]+' $T/mangle.ptx | cut -d' ' -f2 | LC_ALL=C sort -u > $T/mangle.out
if cmp -s $T/mangle.out tests/mangle/mangle.exp; then echo "mangle: $(wc -l < $T/mangle.out) kernel names as nvcc's"; exit 0; fi
echo "FAIL mangle: (< mcc, > expected)"; diff $T/mangle.out tests/mangle/mangle.exp; exit 1
