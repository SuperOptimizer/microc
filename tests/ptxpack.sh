#!/bin/sh
# ptxpack.sh DIR: the packed module text (cuda/include/mcuda_pack.h). Every tests/cuda/NAME.cu is compiled by DIR/mcc at each
# --cuda-ptx-pack level (1 MPZ, 2 MPY greedy = the default, 3 MPY optimal) with --ptx-out and its main renamed, linked with
# tests/ptxpack/check.c and mcudahw, and must unpack to exactly the --ptx-out text and refuse damaged copies; --cuda-ptx-raw
# and --cuda-ptx-pack=0 must leave the text plain. No GPU: nothing is launched.
D=${1:?DIR}; S=tests/cuda; T=build/test/ptxpack; mkdir -p $T
pass=0; fail=0
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h"
INC="-I$D/cudahw/include"
$D/mcc -mglibc $INC -c tests/ptxpack/check.c -o $T/check.o > $T/check.log 2>&1 || { echo "FAIL ptxpack check.c: $(tail -2 $T/check.log)"; exit 1; }
for L in 1 2 3; do
  case $L in 1) magic=MPZ;; *) magic=MPY;; esac
  for f in $S/*.cu; do
    n=$(basename $f .cu)
    rm -f $T/$n.ptx
    if ! $D/mcc -mglibc $CU --cuda-ptx-pack=$L -Dmain=unit_main -c $f -o $T/$n.o --ptx-out=$T/$n.ptx > $T/$n.log 2>&1; then continue; fi   # (cuda.sh judges compiling)
    [ -f $T/$n.ptx ] || continue   # (no device code: no module)
    if grep -qa "$magic" $T/$n.o &&
       $D/mcc -mglibc $T/check.o $T/$n.o -o $T/$n $D/cudahw/lib64/libcudart.a -ldl -lpthread -lm >> $T/$n.log 2>&1 && MALLOC_CHECK_=3 MALLOC_PERTURB_=165 $T/$n $T/$n.ptx >> $T/$n.log 2>&1; then
        pass=$((pass + 1))
    else
        echo "FAIL ptxpack $n (pack $L): $(tail -2 $T/$n.log)"; fail=$((fail + 1))
    fi
  done
done
# --cuda-ptx-file (MPF): the packed record in OUT.ptxz; refused when that file is damaged; found through MCUDA_PTXDIR when moved
mkdir -p $T/moved
for f in $S/*.cu; do
    n=$(basename $f .cu)
    rm -f $T/$n.ptx $T/$n.o.ptxz $T/moved/$n.o.ptxz
    if ! $D/mcc -mglibc $CU --cuda-ptx-file -Dmain=unit_main -c $f -o $T/$n.o --ptx-out=$T/$n.ptx > $T/$n.log 2>&1; then continue; fi
    [ -f $T/$n.ptx ] || continue
    ok=0
    if [ -f $T/$n.o.ptxz ] && grep -qa MPF $T/$n.o && ! grep -qa MPY $T/$n.o &&
       $D/mcc -mglibc $T/check.o $T/$n.o -o $T/$n $D/cudahw/lib64/libcudart.a -ldl -lpthread -lm >> $T/$n.log 2>&1 &&
       MALLOC_CHECK_=3 MALLOC_PERTURB_=165 $T/$n $T/$n.ptx >> $T/$n.log 2>&1; then
        cp $T/$n.o.ptxz $T/$n.good
        printf '\377' | dd of=$T/$n.o.ptxz bs=1 seek=40 conv=notrunc 2> /dev/null   # (a damaged file: refused)
        if ! $T/$n $T/$n.ptx >> $T/$n.log 2>&1; then
            mv $T/$n.good $T/moved/$n.o.ptxz; rm -f $T/$n.o.ptxz
            MCUDA_PTXDIR=$T/moved $T/$n $T/$n.ptx >> $T/$n.log 2>&1 && ok=1
        fi
    fi
    if [ $ok = 1 ]; then pass=$((pass + 1)); else echo "FAIL ptxpack $n (--cuda-ptx-file): $(tail -2 $T/$n.log)"; fail=$((fail + 1)); fi
done
# --cuda-ptx-raw: the record holds the plain text (the first bytes of the --ptx-out file)
f=$S/$(ls $S | grep '\.cu$' | head -1)
for raw in --cuda-ptx-raw --cuda-ptx-pack=0; do
  if $D/mcc -mglibc $CU $raw -c $f -o $T/raw.o --ptx-out=$T/raw.ptx && head -c 64 $T/raw.ptx > $T/raw.head &&
     grep -qF "$(head -1 $T/raw.head)" $T/raw.o && ! grep -qaE 'MP[ZY]' $T/raw.o; then pass=$((pass + 1)); else echo "FAIL ptxpack $raw"; fail=$((fail + 1)); fi
done
echo "ptxpack: $pass passed, $fail failed"
[ $fail = 0 ] && [ $pass -gt 1 ]
