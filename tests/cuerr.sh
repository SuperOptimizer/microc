#!/bin/sh
# cuerr.sh DIR: CUDA / C++ diagnostics. tests/cuerr/NAME.cu's first line is "// error: TEXT" (DIR/mcc must fail with TEXT in its
# message) or "// ok" (must compile).
D=${1:?DIR}; S=tests/cuerr; T=build/test/cuerr; mkdir -p $T
pass=0; fail=0
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h"
[ -f $D/cudahw/include/mcuda_device.h ] || CU="-isystem cuda/devinc -include cuda/devinc/mcuda_device.h"
for f in $S/*.cu; do
    n=$(basename $f .cu); want=$(head -1 $f)
    $D/mcc -mglibc $CU -c $f -o $T/$n.o > $T/$n.log 2>&1; rc=$?
    case "$want" in
    "// ok") [ $rc = 0 ] && pass=$((pass + 1)) || { echo "FAIL cuerr $n: did not compile: $(head -2 $T/$n.log)"; fail=$((fail + 1)); } ;;
    "// error: "*) msg=${want#// error: }
        if [ $rc != 0 ] && grep -qF "$msg" $T/$n.log; then pass=$((pass + 1)); else echo "FAIL cuerr $n: wanted an error with: $msg (rc $rc)"; fail=$((fail + 1)); fi ;;
    esac
done
# C++17 inline variables in two units: one object (weak definitions), so the program links and both units see the same one
I=$S/inl
if $D/mcc -mglibc $CU -c $I/a.cu -o $T/inl_a.o && $D/mcc -mglibc $CU -c $I/b.cu -o $T/inl_b.o && $D/mcc -mglibc $T/inl_a.o $T/inl_b.o -o $T/inl > $T/inl.log 2>&1 &&
   [ "$($T/inl)" = "5 6 4 4" ]; then pass=$((pass + 1)); else echo "FAIL cuerr inline variables: $(head -2 $T/inl.log) / $($T/inl 2>&1)"; fail=$((fail + 1)); fi
echo "cuerr: $pass passed, $fail failed"
[ $fail = 0 ]
