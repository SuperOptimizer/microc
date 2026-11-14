#!/bin/sh
# thr.sh DIR [test...]: tests of atomics, thread-local storage and threads. Every tests/thr/NAME.c (with
# NAME_2.c, if there is one, as a second source file) is compiled for aarch64 Linux by DIR/mcc, and all of
# them run under tests/thr/init.c as the first process of a Linux kernel in DIR/mvm, on the real CPU with
# four processors and on the interpreter with two. What each prints must be tests/thr/NAME.expect.
#   KERNEL=dir  the kernel tree (arch/arm64/boot/Image and usr/gen_init_cpio), default linux/mb10 of the main tree
#   ACCELS="hvf emu"   SECS=seconds allowed for each boot
D=$1; shift
K=${KERNEL:-/Users/forrest/mcc/linux/mb10}
T=build/thr
CC="$D/mcc --target=aarch64-linux-gnu"
mkdir -p $T
pass=0; fail=0
if [ $# -gt 0 ]; then tests="$*"; else
    tests=$(for f in tests/thr/*.expect; do basename $f .expect; done)
fi
: > $T/tests
{
    echo "dir /dev 755 0 0"
    echo "dir /bin 755 0 0"
    echo "dir /tmp 1777 0 0"
    echo "dir /proc 755 0 0"
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "nod /dev/null 666 0 0 c 1 3"
} > $T/spec
$CC -o $T/init tests/thr/init.c || exit 1
echo "file /init $T/init 755 0 0" >> $T/spec
for t in $tests; do
    src=tests/thr/$t.c
    [ -f tests/thr/${t}_2.c ] && src="$src tests/thr/${t}_2.c"
    if $CC $CFLAGS -o $T/$t $src 2> $T/$t.err; then
        echo "file /bin/$t $T/$t 755 0 0" >> $T/spec
        echo $t >> $T/tests
    else
        echo "FAIL $t (compile)"; head -5 $T/$t.err
        fail=$((fail + 1))
    fi
done
echo "file /tests $T/tests 644 0 0" >> $T/spec
$K/usr/gen_init_cpio $T/spec > $T/root.cpio || exit 1
for a in ${ACCELS:-hvf emu}; do
    smp=4; [ $a = emu ] && smp=2
    $D/mvm -m 512 -smp ${SMP:-$smp} -accel $a -kernel $K/arch/arm64/boot/Image -initrd $T/root.cpio \
        -append "console=ttyAMA0 rdinit=/init panic=-1 quiet" < /dev/null > $T/log-$a 2>&1 &
    P=$!
    ( /bin/sleep ${SECS:-300}; kill $P 2>/dev/null ) > /dev/null 2>&1 &
    W=$!
    wait $P 2>/dev/null
    kill $W 2>/dev/null; wait $W 2>/dev/null
    tr -d '\r' < $T/log-$a > $T/out-$a
    for t in $(cat $T/tests); do
        sed -n "/^== $t ==\$/,/^== $t: /p" $T/out-$a | sed '1d' > $T/$t.out-$a
        { cat tests/thr/$t.expect; echo "== $t: status 0 =="; } > $T/$t.want
        if cmp -s $T/$t.out-$a $T/$t.want; then
            pass=$((pass + 1))
        else
            echo "FAIL $t ($a): diff $T/$t.out-$a $T/$t.want"
            fail=$((fail + 1))
        fi
    done
done
echo "threads: $pass pass${fail:+, $fail fail}" | sed 's/, 0 fail//'
[ $fail = 0 ]
