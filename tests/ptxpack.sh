#!/bin/sh
# ptxpack.sh DIR: the packed module text (cuda/include/mcuda_pack.h). Every tests/cuda/NAME.cu is compiled by DIR/mcc (packed, the
# default) with --ptx-out and its main renamed, linked with tests/ptxpack/check.c and mcudahw, and must unpack to exactly the
# --ptx-out text and refuse damaged copies; --cuda-ptx-raw must leave the text plain. No GPU: nothing is launched.
D=${1:?DIR}; S=tests/cuda; T=build/test/ptxpack; mkdir -p $T
pass=0; fail=0
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h"
INC="-I$D/cudahw/include"
for f in $S/*.cu; do
    n=$(basename $f .cu)
    rm -f $T/$n.ptx
    if ! $D/mcc -mglibc $CU -Dmain=unit_main -c $f -o $T/$n.o --ptx-out=$T/$n.ptx > $T/$n.log 2>&1; then continue; fi   # (cuda.sh judges compiling)
    [ -f $T/$n.ptx ] || continue   # (no device code: no module)
    if $D/mcc -mglibc $INC -c tests/ptxpack/check.c -o $T/check.o >> $T/$n.log 2>&1 &&
       $D/mcc -mglibc $T/check.o $T/$n.o -o $T/$n $D/cudahw/lib64/libcudart.a -ldl -lpthread -lm >> $T/$n.log 2>&1 && MALLOC_CHECK_=3 MALLOC_PERTURB_=165 $T/$n $T/$n.ptx >> $T/$n.log 2>&1; then
        pass=$((pass + 1))
    else
        echo "FAIL ptxpack $n: $(tail -2 $T/$n.log)"; fail=$((fail + 1))
    fi
done
# --cuda-ptx-raw: the record holds the plain text (the first bytes of the --ptx-out file)
f=$S/$(ls $S | grep '\.cu$' | head -1)
if $D/mcc -mglibc $CU --cuda-ptx-raw -c $f -o $T/raw.o --ptx-out=$T/raw.ptx && head -c 64 $T/raw.ptx > $T/raw.head &&
   grep -qF "$(head -1 $T/raw.head)" $T/raw.o && ! grep -qa 'MPZ' $T/raw.o; then pass=$((pass + 1)); else echo "FAIL ptxpack raw"; fail=$((fail + 1)); fi
echo "ptxpack: $pass passed, $fail failed"
[ $fail = 0 ] && [ $pass -gt 1 ]
