#!/bin/sh
# x64fuzz.sh DIR [FIRST [COUNT]]: random C programs (tests/x64fuzz/gen.py SEED, seeds FIRST.. , 200 of them
# from 1 by default) compiled for x86-64 Linux by DIR/mcc at -O0, the default level, -Os and -O3, and by
# the C compiler of the x86-64 machine: all five must print the same. X64_SSH and X64_DIR as in
# tests/x64.sh; X64_CC (default gcc). Needs python3 here.
D=$1; first=${2:-1}; count=${3:-200}
T=build/x64fuzz
R=${X64_DIR:-/root/x64}/fuzz
CC="$D/mcc --target=x86_64-linux-gnu"
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/x64fuzz.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
rm -rf $T; mkdir -p $T/run
fail=0; k=$first; last=$((first + count))
: > $T/run/list
while [ $k -lt $last ]; do
    python3 tests/x64fuzz/gen.py $k > $T/run/p$k.c
    n=0
    for flags in -O0 -O1 -Os -O3; do
        n=$((n + 1))
        if ! $CC $flags -o $T/run/p$k.$n $T/run/p$k.c 2> $T/p$k.$n.err; then
            echo "FAIL: seed $k ($flags compile): $(head -1 $T/p$k.$n.err)"; fail=$((fail + 1))
        fi
    done
    echo $k >> $T/run/list
    k=$((k + 1))
done
cat > $T/run/run.sh <<'XEOF'
CC=${1:-gcc}
one() {
    k=$1
    $CC -O0 -w -ffp-contract=off -o p$k.ref p$k.c 2>/dev/null || { echo "SKIP $k (reference compiler)"; return; }
    timeout 10 ./p$k.ref > p$k.want 2>/dev/null; [ $? = 0 ] || { echo "SKIP $k (reference run)"; return; }
    for n in 1 2 3 4; do
        [ -f p$k.$n ] || continue
        timeout 20 ./p$k.$n > p$k.$n.out 2>/dev/null
        cmp -s p$k.$n.out p$k.want || echo "DIFF $k $n"
    done
    echo "DONE $k"
}
for k in $(cat list); do one $k & [ $(jobs -r | wc -l) -ge 8 ] && wait -n; done
wait
XEOF
(cd $T/run && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && bash run.sh ${X64_CC:-gcc} 2>&1" > $T/out
grep "^DIFF\|^SKIP" $T/out | sort -k2 -n | head -30
fail=$((fail + $(grep -c "^DIFF" $T/out)))
echo "$(grep -c '^DONE' $T/out) programs compared, $(grep -c '^SKIP' $T/out) skipped, $fail failures"
[ "$fail" = 0 ]
