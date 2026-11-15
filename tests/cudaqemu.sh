#!/bin/bash
# cudaqemu.sh DIR [test...]: tests/cuda.sh's Linux half on an x86-64 Linux without mvm: mcuda and the tests built for
# aarch64 Linux by DIR/mcc and run under qemu-aarch64 (binfmt_misc). DIR is made by
#   sh mklxlibc.sh DIR/mcc DIR && sh mkcuda.sh DIR/mcc DIR        (DIR/mcc: a copy of the compiler under test)
D=$1; shift
T=$D/run; S=tests/cuda; mkdir -p $T
if [ $# -gt 0 ]; then tests="$*"; else
    tests="rt_test $(for f in $S/*.exp; do n=$(basename $f .exp); [ -f $S/$n.ptx ] || [ -f $S/$n.cu ] && echo $n; done)"
fi
CC="$D/mcc --target=aarch64-linux-gnu"; inc="-I$D/linux/cuda/include -Icuda"
CU="-isystem $D/cuda/include -include $D/cuda/include/mcuda_device.h"
$CC $inc -o $T/ptxt $S/ptxt.c $D/linux/cuda/lib/libmcuda.a || exit 1
$CC -DRT_TEST_ENGINE $inc -o $T/rt_test $S/rt_test.c $D/linux/cuda/lib/libmcuda.a || exit 1
pass=0; fail=0
for t in $tests; do
    if [ -f $S/$t.cu ]; then
        $CC $CU $(sed -n '1s|^// mcc: ||p' $S/$t.cu) -c $S/$t.cu -o $T/$t.o 2> $T/$t.cerr && $CC -o $T/$t $T/$t.o -L$D/cuda/lib64 -lcudart -lcublasLt -lstdc++ 2>> $T/$t.cerr ||
            { echo "FAIL $t (build): $T/$t.cerr"; fail=$((fail + 1)); continue; }
        (cd $T && timeout 600 ./$t > $t.out 2> $t.err)
    elif [ $t = rt_test ]; then (cd $T && timeout 600 ./rt_test > rt_test.out 2> rt_test.err)
    else timeout 600 $T/ptxt $S/$t.ptx > $T/$t.out 2> $T/$t.err; fi
    if cmp -s $T/$t.out $S/$t.exp; then pass=$((pass + 1)); else echo "FAIL $t: diff $T/$t.out $S/$t.exp"; fail=$((fail + 1)); fi
done
echo "cuda (aarch64 Linux under qemu): $pass passed, $fail failed"
[ $fail = 0 ]
