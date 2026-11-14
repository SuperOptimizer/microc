#!/bin/sh
# cuda.sh DIR [test...]: tests of mcuda, the CUDA runtime that runs PTX on the CPU, as mkcuda.sh installed it in
# DIR. Every tests/cuda/NAME.ptx is run by tests/cuda/ptxt.c (the kernels its "//! run" lines name) and must
# print tests/cuda/NAME.exp; rt_test.c, the test of the runtime's C interface, must print rt_test.exp, and every NAME.cu,
# a CUDA program compiled by DIR/mcc and linked with the runtime, e2e.exp. First
# on this machine (mcc --hosted), then the same programs built for aarch64 Linux, in DIR/mvm.
#   KERNEL=dir   the kernel tree (arch/arm64/boot/Image and usr/gen_init_cpio), default linux/mb10 of the main tree
#   LINUX=0      leave the Linux half out        ACCEL=hvf|emu        SECS=seconds allowed for the boot
# The g_*.ptx and their .exp were written by tests/cuda/gen.py from reference semantics, not by the engine;
# x_*.ptx are second modules that other tests load.
D=$1; shift
K=${KERNEL:-/Users/forrest/mcc/linux/mb10}
T=build/test/cuda/run
S=tests/cuda
mkdir -p $T
pass=0; fail=0
if [ $# -gt 0 ]; then tests="$*"; else
    tests="rt_test $(for f in $S/*.exp; do n=$(basename $f .exp); [ -f $S/$n.ptx ] || [ -f $S/$n.cu ] && echo $n; done)"
fi
# the tests that are programs of their own: rt_test, and every NAME.cu (a CUDA program compiled by DIR/mcc)
progs=" rt_test "
for t in $tests; do [ -f $S/$t.cu ] && progs="$progs$t "; done
isprog() { case "$progs" in *" $1 "*) return 0;; esac; return 1; }
inc="-I$D/host/cuda/include -Icuda"
CU="-isystem $D/cuda/include -include $D/cuda/include/mcuda_device.h"   # how a .cu file is compiled
$D/mcc --hosted $inc -o $T/ptxt $S/ptxt.c $D/host/cuda/lib/libmcuda.a &&
$D/mcc --hosted -DRT_TEST_ENGINE $inc -o $T/rt_test $S/rt_test.c $D/host/cuda/lib/libmcuda.a &&
for t in $tests; do
    [ -f $S/$t.cu ] || continue
    $D/mcc --hosted $CU -c $S/$t.cu -o $T/$t.o && $D/mcc --hosted -o $T/$t $T/$t.o $D/host/cuda/lib/libmcuda.a
done || { echo "cuda: cannot build the tests (host)"; exit 1; }
for t in $tests; do
    if isprog $t; then $T/$t > $T/$t.out 2> $T/$t.err; else $T/ptxt $S/$t.ptx > $T/$t.out 2> $T/$t.err; fi
    if cmp -s $T/$t.out $S/$t.exp; then pass=$((pass + 1)); else
        echo "FAIL $t (host): diff $T/$t.out $S/$t.exp"; fail=$((fail + 1))
    fi
done
if [ "${LINUX:-1}" != 0 ] && [ -f $D/linux/cuda/lib/libmcuda.a ] && [ -f $K/arch/arm64/boot/Image ] && [ -x $K/usr/gen_init_cpio ] && [ -x $D/mvm ]; then
    CC="$D/mcc --target=aarch64-linux-gnu"
    inc="-I$D/linux/cuda/include -Icuda"
    $CC -o $T/init $S/init.c &&
    $CC $inc -o $T/ptxt.lx $S/ptxt.c $D/linux/cuda/lib/libmcuda.a &&
    $CC -DRT_TEST_ENGINE $inc -o $T/rt_test.lx $S/rt_test.c $D/linux/cuda/lib/libmcuda.a &&
    for t in $tests; do
        [ -f $S/$t.cu ] || continue
        $CC $CU -c $S/$t.cu -o $T/$t.lx.o && $CC -o $T/$t.lx $T/$t.lx.o -L$D/cuda/lib64 -lcudart -lcublasLt -lstdc++
    done || { echo "cuda: cannot build the tests (linux)"; exit 1; }
    {
        echo "dir /dev 755 0 0"; echo "dir /bin 755 0 0"; echo "dir /t 755 0 0"; echo "dir /tmp 1777 0 0"
        echo "dir /dev/shm 1777 0 0"
        echo "nod /dev/console 600 0 0 c 5 1"; echo "nod /dev/null 666 0 0 c 1 3"
        echo "file /init $T/init 755 0 0"; echo "file /bin/ptxt $T/ptxt.lx 755 0 0"; echo "file /bin/rt_test $T/rt_test.lx 755 0 0"
    } > $T/spec
    : > $T/tests
    for t in $tests; do
        echo $t >> $T/tests
        if [ -f $S/$t.cu ]; then echo "file /bin/$t $T/$t.lx 755 0 0" >> $T/spec
        elif [ $t != rt_test ]; then echo "file /t/$t.ptx $S/$t.ptx 644 0 0" >> $T/spec; fi
    done
    for f in $S/x_*.ptx; do echo "file /t/$(basename $f) $f 644 0 0" >> $T/spec; done
    echo "file /tests $T/tests 644 0 0" >> $T/spec
    $K/usr/gen_init_cpio $T/spec > $T/root.cpio || exit 1
    $D/mvm -m 1536 -smp ${SMP:-4} -accel ${ACCEL:-hvf} -kernel $K/arch/arm64/boot/Image -initrd $T/root.cpio \
        -append "console=ttyAMA0 rdinit=/init panic=-1 quiet" < /dev/null > $T/log 2>&1 &
    P=$!
    ( /bin/sleep ${SECS:-600}; kill $P 2>/dev/null ) > /dev/null 2>&1 &
    W=$!
    wait $P 2>/dev/null
    kill $W 2>/dev/null; wait $W 2>/dev/null
    tr -d '\r' < $T/log > $T/out
    for t in $tests; do
        sed -n "/^== $t ==\$/,/^== $t: /p" $T/out | sed '1d' > $T/$t.lxout
        { cat $S/$t.exp; echo "== $t: status 0 =="; } > $T/$t.want
        if cmp -s $T/$t.lxout $T/$t.want; then pass=$((pass + 1)); else
            echo "FAIL $t (linux): diff $T/$t.lxout $T/$t.want"; fail=$((fail + 1))
        fi
    done
else
    echo "cuda: the Linux half is left out"
fi
echo "cuda: $pass pass${fail:+, $fail fail}" | sed 's/, 0 fail//'
[ $fail = 0 ]
