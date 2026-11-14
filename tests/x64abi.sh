#!/bin/sh
# x64abi.sh DIR: the x86-64 calling convention against the C compiler of an x86-64 Linux machine:
# tests/x64abi/abi_a.c calls abi_b.c; one is compiled by DIR/mcc and the other there, both ways round, and
# the two are linked and run there. X64_SSH and X64_DIR as in tests/x64.sh; X64_CC (default gcc).
D=$1
T=build/x64abi
R=${X64_DIR:-/root/x64}/abi
CC="$D/mcc --target=x86_64-linux-gnu"
if [ -z "$X64_SSH" ]; then
    echo "tests/x64abi.sh: set X64_SSH (how to reach an x86-64 Linux machine, or \"local\")"; exit 1
fi
rm -rf $T; mkdir -p $T
cp tests/x64abi/abi.h tests/x64abi/abi_a.c tests/x64abi/abi_b.c $T/
fail=0
for o in O0 O1 Os; do
    for f in a b; do
        $CC -$o -c -o $T/mcc_${f}_$o.o tests/x64abi/abi_$f.c || fail=1
    done
done
[ $fail = 0 ] || { echo "FAIL: compile"; exit 1; }
cat > $T/run.sh <<'XEOF'
CC=${1:-gcc}
for o in O0 O1 Os; do
    for g in O0 O2; do
        $CC -$g -w -c -o cc_a.o abi_a.c && $CC -$g -w -c -o cc_b.o abi_b.c || exit 1
        $CC -no-pie -o t1 mcc_a_$o.o cc_b.o && ./t1 | sed "s/^/mcc -$o calls cc -$g: /"
        $CC -no-pie -o t2 cc_a.o mcc_b_$o.o && ./t2 | sed "s/^/cc -$g calls mcc -$o: /"
    done
done
$CC -no-pie -o t3 mcc_a_O1.o mcc_b_O1.o && ./t3 | sed "s/^/mcc calls mcc: /"
XEOF
if [ "$X64_SSH" = local ]; then # (this is an x86-64 Linux machine: run them in place)
    (cd $T && sh run.sh ${X64_CC:-gcc} 2>&1) > $T/out.tmp; mv $T/out.tmp $T/out
else
    (cd $T && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && sh run.sh ${X64_CC:-gcc} 2>&1" > $T/out
fi
grep -v ": abi: 0 mismatches\|/ld: " $T/out | head -40
n=$(grep -c ": abi: 0 mismatches" $T/out)
echo "$n of 13 combinations agree"
[ "$n" = 13 ]
