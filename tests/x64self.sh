#!/bin/sh
# x64self.sh DIR: mcc as a compiler that runs on x86-64 Linux. DIR/mcc cross-builds mcc (stage 1) and its
# sources go to the x86-64 machine; there stage 1 builds the C library and stage 2, stage 2 builds the
# library again and stage 3, and stages 2 and 3 must be identical (stage 1, built here, should be too: the
# same compiler run on another machine). Then stage 2 compiles and runs the C tests there (tests/x64.sh).
# With PGO=1, instead of those tests: stage 2 is built again instrumented, compiles mcc once for a profile,
# and mcc built with that profile (the block layout of profile_layout in src/x64.c) must build the same
# stage 2 and pass the C tests.
#   X64_SSH, X64_DIR as in tests/x64.sh
D=$1
LC_ALL=C; export LC_ALL # src/*.c in the same order on both machines
T=build/x64self
R=${X64_DIR:-/root/x64}/self
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/x64self.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
rm -rf $T; mkdir -p $T/stage1
$D/mcc --target=x86_64-linux-gnu -o $T/stage1/mcc src/*.c || { echo "FAIL: cross-building mcc"; exit 1; }
cat > $T/run.sh <<'XEOF'
set -e
LC_ALL=C; export LC_ALL
sh mklxlibc.sh stage1/mcc stage1 x86_64
for n in 2 3; do
    p=$((n - 1))
    mkdir -p stage$n
    stage$p/mcc -o stage$n/mcc src/*.c
    sh mklxlibc.sh stage$n/mcc stage$n x86_64
done
if cmp -s stage2/mcc stage3/mcc; then echo "bootstrap: stage2 and stage3 are identical"; else echo "FAIL: stage2 and stage3 differ"; fi
if cmp -s stage1/mcc stage2/mcc; then echo "the cross-built stage1 is identical too"; else echo "FAIL: the cross-built stage1 differs from stage2"; fi
if [ -z "$PGO" ]; then
    X64_SSH=local sh tests/x64.sh stage2 | sed 's/^/native tests: /'
else
    mkdir -p pg pgo/again
    ln -s ../stage2/linux-x64 pg/linux-x64
    stage2/mcc -fprofile-generate -o pg/mcc src/*.c
    MCC_PROFILE=$PWD/pg/mcc.prof pg/mcc -j1 -o pg/out.bin src/*.c
    stage2/mcc -fprofile-use=pg/mcc.prof -o pgo/mcc src/*.c
    sh mklxlibc.sh pgo/mcc pgo x86_64
    pgo/mcc -o pgo/again/mcc src/*.c
    if cmp -s pgo/mcc stage2/mcc; then echo "FAIL: the profile changed nothing"; fi
    if cmp -s pgo/again/mcc stage2/mcc; then echo "profile: mcc built with it builds the same stage2"; else echo "FAIL: mcc built with a profile builds another stage2"; fi
    X64_SSH=local sh tests/x64.sh pgo | sed 's/^/native tests: /'
fi
XEOF
tar cf $T/src.tar src libc/src libc/linux libc/glibc mklxlibc.sh tests/c tests/x64.sh 2>/dev/null
(cd $T && tar cf - src.tar run.sh stage1/mcc) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && tar xf src.tar 2>/dev/null && PGO=$PGO sh run.sh 2>&1" > $T/out
cat $T/out | tail -20
grep -q "stage2 and stage3 are identical" $T/out && grep -q "stage1 is identical too" $T/out && grep -q "native tests: [0-9]* passed, 0 failed" $T/out && ! grep -q "^FAIL" $T/out
