#!/bin/sh
# x64.sh DIR [test...]: the C execute tests (tests/c/NAME.c, NAME.expect) compiled for x86-64 Linux by
# DIR/mcc, in the variants of tests/run.sh (but not -flto, which the ELF linker does not do, and not with
# profiles), and run on an x86-64 Linux machine.
#   X64_SSH="ssh -p PORT user@host"   how to reach the machine ("local": this is one; run them here)
#   X64_DIR=/root/x64                 a directory there that this may use (default /root/x64)
#   CFLAGS=...                        more compiler flags
#   SUITE=thr                         tests/thr instead (threads, atomics, thread-local storage: NAME.c with
#                                     NAME_2.c if there is one; NAME.expect is the output, the status must be 0)
#   GLIBC=DIR                         against the GNU C library instead of mcc's: headers under DIR/usr/include
#                                     (--sysroot=DIR), objects linked on the machine by its gcc
D=$1; shift
S=tests/${SUITE:-c}
T=build/x64test
R=${X64_DIR:-/root/x64}/t
CC="$D/mcc --target=x86_64-linux-gnu"
# mcc's own C library unless GLIBC says otherwise (an mcc built on a glibc machine defaults to glibc)
[ -n "$GLIBC" ] || CC="$CC -mno-glibc"
if [ -z "$X64_SSH" ]; then
    echo "tests/x64.sh: set X64_SSH (how to reach an x86-64 Linux machine, or \"local\")"; exit 1
fi
rm -rf $T; mkdir -p $T/bin $T/out
pass=0; fail=0
if [ $# -gt 0 ]; then tests="$*"; else
    tests=$(for f in $S/*.expect; do basename $f .expect; done | grep -v '^fpemu$') # fpemu is aarch64 assembly
fi
: > $T/list
for t in $tests; do
    n=0
    for flags in "" "-O0" "-Os" "-O3" "-O3 -ffast-math"; do
        n=$((n + 1))
        src=$S/$t.c
        [ -f $S/${t}_2.c ] && src="$src $S/${t}_2.c"
        if [ -n "$GLIBC" ]; then
            k=0; ok=1
            for one in $src; do
                k=$((k + 1))
                $CC --sysroot=$GLIBC -D_GNU_SOURCE $CFLAGS $flags -c -o $T/bin/$t.$n.$k.o $one 2>> $T/$t.$n.err || ok=0
            done
            [ $ok = 1 ]
        else
            $CC $CFLAGS $flags -o $T/bin/$t.$n $src 2> $T/$t.$n.err
        fi
        if [ $? = 0 ]; then
            echo "$t.$n" >> $T/list
        else
            echo "FAIL: $t ($flags compile)"; head -3 $T/$t.$n.err
            fail=$((fail + 1))
        fi
    done
done
cat > $T/bin/run.sh <<'XEOF'
for b in $(cat list); do
    [ -f $b.1.o ] && gcc -no-pie -o $b $b.*.o -lm -lpthread 2> $b.lderr
    { timeout 60 ./$b; echo "exit $?"; } > $b.out 2>/dev/null
    # Rented machines have been seen to kill (signal 9) an executable whatever it does once the same file
    # contents have been run some twenty times: such a one gets more chances as a copy with bytes
    # appended that no earlier run had.
    for again in 1 2 3 4; do
        [ "$(tail -1 $b.out)" = "exit 137" ] || break
        cp $b $b.again; echo "$$ $again $(date +%s%N)" >> $b.again
        { timeout 60 ./$b.again; echo "exit $?"; } > $b.out 2>/dev/null
    done
done
XEOF
cp $T/list $T/bin/list
if [ "$X64_SSH" = local ]; then
    (cd $T/bin && sh run.sh && mv *.out ../out/)
else
    (cd $T/bin && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && sh run.sh && tar cf - *.out" | (cd $T/out && tar xf -)
fi
for b in $(cat $T/list); do
    t=${b%.*}
    cp $S/$t.expect $T/want
    [ $S = tests/c ] || echo "exit 0" >> $T/want
    if cmp -s $T/out/$b.out $T/want; then pass=$((pass + 1)); else
        echo "FAIL: $b (output)"; fail=$((fail + 1))
    fi
done
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
