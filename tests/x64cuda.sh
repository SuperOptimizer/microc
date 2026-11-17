#!/bin/sh
# x64cuda.sh DIR [test...]: the tests of tests/cuda.sh (mcuda, the CUDA run time that runs PTX on the CPU) built
# for x86-64 Linux against the library `mkcuda.sh` installed in DIR/linux-x64/cuda, and run on an x86-64
# machine: every tests/cuda/NAME.ptx by ptxt, rt_test, and every NAME.cu as a program; each must print NAME.exp.
#   X64_SSH, X64_DIR as in tests/x64.sh         SECS=seconds allowed for one test (default 300)
# The programs link libcudart.a, which holds the CPU's run time and the GPU's (cuda/mux): they are told to take
# the CPU's (MCUDA_BACKEND=cpu), whatever the machine has.
#   GPU=1   the machine has NVIDIA's driver and a GPU: the NAME.cu programs alone, on the GPU's run time
#           (MCUDA_BACKEND=gpu: the driver loaded by our C library's dlopen, cublasLt our own kernel); e2e's
#           lines are compared sorted (a device prints its threads' lines in the order it runs them)
#   GPU_SKIP="mx ldg"   programs left out with GPU=1. By default mx, which is for sm_120a (a Blackwell GPU), and
#           ldg, which reads a uint2 at an address that is a multiple of 4 and not of 8: ld.global.nc.v2.u32
#           faults there on a real device ("misaligned address"), the CPU's run time does not mind.
D=$1; shift
S=tests/cuda; T=build/x64cuda
R=${X64_DIR:-/root/x64}/cuda
CC="$D/mcc --target=x86_64-linux-gnu -mno-glibc"
L=$D/linux-x64/cuda
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/x64cuda.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
[ -f $L/lib/libmcuda.a ] || { echo "tests/x64cuda.sh: no $L/lib/libmcuda.a (make x64 cuda)"; exit 1; }
rm -rf $T; mkdir -p $T/run/t $T/out
backend=cpu; label="cuda (x86-64)"
if [ -n "$GPU" ]; then backend=gpu; label="cuda (x86-64, GPU)"; fi
if [ $# -gt 0 ]; then tests="$*"; elif [ -n "$GPU" ]; then
    tests=$(for f in $S/*.cu; do
        n=$(basename $f .cu)
        case " ${GPU_SKIP-mx ldg} " in (*" $n "*) continue;; esac
        [ -f $S/$n.exp ] && echo $n
    done)
else
    tests="rt_test $(for f in $S/*.exp; do n=$(basename $f .exp); [ -f $S/$n.ptx ] || [ -f $S/$n.cu ] && echo $n; done)"
fi
inc="-I$L/include -Icuda"
CU="-isystem $D/cuda/include -include $D/cuda/include/mcuda_device.h"   # how a .cu file is compiled
fail=0
$CC $inc -o $T/run/ptxt $S/ptxt.c $L/lib/libmcuda.a &&
$CC -DRT_TEST_ENGINE $inc -o $T/run/rt_test $S/rt_test.c $L/lib/libmcuda.a || { echo "cuda (x86-64): cannot build the tests"; exit 1; }
: > $T/run/tests
for t in $tests; do
    if [ -f $S/$t.cu ]; then
        if $CC $CU -c $S/$t.cu -o $T/$t.o 2> $T/$t.err && $CC -o $T/run/$t $T/$t.o -L$L/lib -lcudart -lcublasLt -lstdc++ 2>> $T/$t.err; then
            echo "$t prog" >> $T/run/tests
        else
            echo "FAIL $t (build): see $T/$t.err"; fail=$((fail + 1))
        fi
    elif [ $t = rt_test ]; then
        echo "$t prog" >> $T/run/tests
    else
        cp $S/$t.ptx $T/run/t/
        echo "$t ptx" >> $T/run/tests
    fi
done
cp $S/x_*.ptx $T/run/t/ 2>/dev/null
cat > $T/run/run.sh <<XEOF
mkdir -p out cache
export MCUDA_BACKEND=$backend MCUDA_CACHE=\$PWD/cache
while read t kind; do
    if [ \$kind = prog ]; then timeout ${SECS:-300} ./\$t > out/\$t.out 2> out/\$t.err
    else (cd t && timeout ${SECS:-300} ../ptxt \$t.ptx) > out/\$t.out 2> out/\$t.err; fi
    echo "\$?" > out/\$t.status
done < tests
XEOF
(cd $T/run && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && sh run.sh && tar cf - out; cd .. && rm -rf $R" | (cd $T && tar xf -)
pass=0
while read t kind; do
    same=0
    if cmp -s $T/out/$t.out $S/$t.exp; then same=1
    elif [ -n "$GPU" ] && [ $t = e2e ]; then
        sort $T/out/$t.out > $T/out/$t.sorted; sort $S/$t.exp | cmp -s - $T/out/$t.sorted && same=1
    fi
    if [ $same = 1 ] && [ "$(cat $T/out/$t.status 2>/dev/null)" = 0 ]; then pass=$((pass + 1)); else
        echo "FAIL $t: diff $T/out/$t.out $S/$t.exp (status $(cat $T/out/$t.status 2>/dev/null))"; fail=$((fail + 1))
    fi
done < $T/run/tests
echo "$label: $pass pass${fail:+, $fail fail}" | sed 's/, 0 fail//'
[ $fail = 0 ]
