#!/bin/sh
# cqemu.sh DIR [test...]: the C execute tests (tests/c/NAME.c, NAME.expect) compiled for aarch64 Linux by DIR/mcc
# against the C library `mklxlibc.sh DIR/mcc DIR` installed, in the variants of tests/x64.sh, and run under
# qemu-aarch64 on a machine without mvm (an x86-64 Linux; no binfmt_misc needed).
#   QEMU=qemu-aarch64      the emulator (default qemu-aarch64)
#   CFLAGS=...             more compiler flags
D=$1; shift
T=build/cqemu
Q=${QEMU:-qemu-aarch64}
CC="$D/mcc --target=aarch64-linux-gnu"
command -v $Q > /dev/null || { echo "tests/cqemu.sh: no $Q"; exit 1; }
[ -d $D/linux/lib ] || { echo "tests/cqemu.sh: no $D/linux/lib (sh mklxlibc.sh $D/mcc $D)"; exit 1; }
rm -rf $T; mkdir -p $T
if [ $# -gt 0 ]; then tests="$*"; else
    tests=$(for f in tests/c/*.expect; do basename $f .expect; done)
fi
pass=0; fail=0
for t in $tests; do
    n=0
    for flags in "" "-O0" "-Os" "-O3" "-O3 -ffast-math"; do
        n=$((n + 1))
        src=tests/c/$t.c
        [ -f tests/c/${t}_2.c ] && src="$src tests/c/${t}_2.c"
        if ! $CC $CFLAGS $flags -o $T/$t.$n $src 2> $T/$t.$n.err; then
            echo "FAIL: $t ($flags compile)"; head -3 $T/$t.$n.err
            fail=$((fail + 1)); continue
        fi
        { timeout 120 $Q $T/$t.$n; echo "exit $?"; } > $T/$t.$n.out 2> /dev/null
        if cmp -s $T/$t.$n.out tests/c/$t.expect; then pass=$((pass + 1)); else
            echo "FAIL: $t.$n ($flags)"; fail=$((fail + 1))
        fi
    done
done
echo "c (aarch64 Linux under qemu): $pass passed, $fail failed"
[ "$fail" = 0 ]
