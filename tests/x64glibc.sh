#!/bin/sh
# x64glibc.sh DIR: mcc as a compiler for x86-64 Linux programs that use the machine's GNU C library
# (--sysroot / -mglibc: its headers, dynamic linking; the link itself is still the machine's gcc).
# The headers come from the machine once (build/x64glibc/sysroot). DIR/mcc cross-compiles mcc's sources
# against them; there the objects are linked (stage 1), stage 1 builds stage 2 and stage 2 stage 3, which
# must be identical; then stage 2 compiles tests/c and tests/thr there, again against the GNU C library.
#   X64_SSH, X64_DIR as in tests/x64.sh
D=$1
LC_ALL=C; export LC_ALL
T=build/x64glibc
R=${X64_DIR:-/root/x64}/glibc
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/x64glibc.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
mkdir -p $T
if [ ! -d $T/sysroot/usr/include ]; then
    mkdir -p $T/sysroot
    $X64_SSH "cd / && tar czf - --exclude=usr/include/c++ usr/include" | tar xzf - -C $T/sysroot || exit 1
fi
rm -rf $T/stage1; mkdir -p $T/stage1/obj
for f in src/*.c; do
    $D/mcc --target=x86_64-linux-gnu --sysroot=$PWD/$T/sysroot -c -o $T/stage1/obj/$(basename $f .c).o $f ||
        { echo "FAIL: cross-compiling $f"; exit 1; }
done
cat > $T/run.sh <<'XEOF'
set -e
LC_ALL=C; export LC_ALL
gcc -no-pie -o stage1/mcc stage1/obj/*.o -lm -lpthread
for n in 1 2 3; do
    p=$((n - 1))
    mkdir -p stage$n/glibc
    [ $n = 1 ] || stage$p/mcc -o stage$n/mcc src/*.c
    cp -R libc/glibc/include stage$n/glibc/include
done
if cmp -s stage2/mcc stage3/mcc; then echo "bootstrap: stage2 and stage3 are identical"; else echo "FAIL: stage2 and stage3 differ"; fi
ldd stage2/mcc | grep -q libc.so.6 || echo "FAIL: stage2 does not use the shared C library"
X64_SSH=local GLIBC=/ sh tests/x64.sh stage2 | sed 's/^/native tests, C: /'
X64_SSH=local GLIBC=/ SUITE=thr sh tests/x64.sh stage2 | sed 's/^/native tests, threads: /'
XEOF
tar cf $T/src.tar src libc/glibc tests/c tests/thr tests/x64.sh 2>/dev/null
(cd $T && tar cf - src.tar run.sh stage1) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && tar xf src.tar 2>/dev/null && sh run.sh 2>&1" > $T/out
tail -20 $T/out
grep -q "stage2 and stage3 are identical" $T/out && grep -q "C: [0-9]* passed, 0 failed" $T/out &&
    grep -q "threads: [0-9]* passed, 0 failed" $T/out && ! grep -q "^FAIL" $T/out
