#!/bin/sh
# x64as.sh DIR: the x86-64 assembler against the assembler of an x86-64 Linux machine: every
# tests/x64as/NAME.s is assembled by DIR/mcc and there, and the bytes of the sections must be the same.
# X64_SSH and X64_DIR as in tests/x64.sh; X64_CC (default gcc).
D=$1
T=build/x64as
R=${X64_DIR:-/root/x64}/as
if [ -z "$X64_SSH" ]; then
    echo "tests/x64as.sh: set X64_SSH (how to reach an x86-64 Linux machine, or \"local\")"; exit 1
fi
rm -rf $T; mkdir -p $T
pass=0; fail=0
for f in tests/x64as/*.s; do
    n=$(basename $f .s)
    cp $f $T/
    $D/mcc --target=x86_64-linux-gnu -c -o $T/$n.o $f || { echo "FAIL: $n (assemble)"; fail=$((fail + 1)); rm -f $T/$n.s; }
done
cat > $T/run.sh <<'XEOF'
for f in *.s; do
    n=${f%.s}
    ${1:-gcc} -c -o $n.ref.o $f || continue
    for s in .text .data; do
        objcopy -O binary -j $s $n.o $n.bin; objcopy -O binary -j $s $n.ref.o $n.ref.bin
        cmp -s $n.bin $n.ref.bin || { echo "DIFF $n $s"; objdump -d $n.o | cut -f2- > $n.txt; objdump -d $n.ref.o | cut -f2- > $n.ref.txt; diff $n.txt $n.ref.txt | head -20; }
    done
    echo "DONE $n"
done
XEOF
if [ "$X64_SSH" = local ]; then # (this is an x86-64 Linux machine: run them in place)
    (cd $T && sh run.sh ${X64_CC:-gcc} 2>&1) > $T/out.tmp; mv $T/out.tmp $T/out
else
    (cd $T && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && sh run.sh ${X64_CC:-gcc} 2>&1" > $T/out
fi
for f in $T/*.s; do
    n=$(basename $f .s)
    if grep -q "^DONE $n\$" $T/out && ! grep -q "^DIFF $n " $T/out; then pass=$((pass + 1)); else echo "FAIL: $n"; fail=$((fail + 1)); fi
done
grep -v "^DONE" $T/out | head -40
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
