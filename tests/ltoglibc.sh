#!/bin/sh
# ltoglibc.sh DIR: -flto objects linked through the system's linker (glibc mode, x86-64 Linux): mcc reads them back, optimises them as one
# and hands ld the result (main.c sys_lto). tests/ltoglibc/a.c + b.c (+ c.c, always ordinary: a strong definition of b.c's weak function), as plain objects and as -flto objects (also mixed, and
# -fprofile-generate / -fprofile-use with -flto), must print what the plain build prints.
D=${1:?DIR}; T=build/test/ltoglibc; S=tests/ltoglibc
mkdir -p $T
M="$D/mcc -mglibc -O3"
fail=0
$M -c $S/a.c -o $T/a.o && $M -c $S/b.c -o $T/b.o && $M -c $S/c.c -o $T/c.o && $M $T/a.o $T/b.o $T/c.o -o $T/plain || { echo "FAIL lto: plain build"; exit 1; }
$T/plain > $T/plain.out
$M -flto -c $S/a.c -o $T/al.o && $M -flto -c $S/b.c -o $T/bl.o || { echo "FAIL lto: -flto compile"; exit 1; }
for v in "al bl" "al b" "a bl"; do
    set -- $v
    n=lto_$1_$2
    if $M $T/$1.o $T/$2.o $T/c.o -o $T/$n && $T/$n > $T/$n.out && cmp -s $T/$n.out $T/plain.out; then :; else
        echo "FAIL lto: $1.o + $2.o"; fail=1
    fi
done
# ld -r of -flto objects keeps their IR (SHF_EXCLUDE stays in a relocatable link), and the final link through mcc optimises it
if $M -r $T/al.o $T/bl.o -o $T/r.o && $M $T/r.o $T/c.o -o $T/lto_r && $T/lto_r > $T/lto_r.out && cmp -s $T/lto_r.out $T/plain.out; then :; else
    echo "FAIL lto: ld -r of -flto objects"; fail=1
fi
# the system's linker alone must refuse -flto objects (their functions are defined in the discarded IR section), not link
# placeholders
if ${MCC_CC:-cc} -no-pie $T/al.o $T/bl.o $T/c.o -o $T/sysonly > $T/sysonly.log 2>&1 || ! grep -q "discarded section .\.mcc\.ir" $T/sysonly.log; then
    echo "FAIL lto: the system's linker alone did not refuse the IR objects"; fail=1
fi
rm -f $T/mcc.prof
if $M -flto -fprofile-generate $S/a.c $S/b.c $T/c.o -o $T/gen && (cd $T && MCC_PROFILE=mcc.prof ./gen > gen.out) && cmp -s $T/gen.out $T/plain.out &&
   $M -flto -fprofile-use=$T/mcc.prof $S/a.c $S/b.c $T/c.o -o $T/use && $T/use > $T/use.out && cmp -s $T/use.out $T/plain.out; then :; else
    echo "FAIL lto: -fprofile-generate / -fprofile-use"; fail=1
fi
[ $fail = 0 ] && echo "lto: 6 links as the plain build, the system linker alone refused: $(cat $T/plain.out)"
exit $fail
