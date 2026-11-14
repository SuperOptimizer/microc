#!/bin/sh
# gpumcc.sh [test...]: the CUDA programs of tests/cuda (NAME.cu with NAME.exp, which came from nvcc on a GPU)
# compiled by mcc for x86-64 Linux, the device half as mcc's PTX, linked with mcudahw (cuda/hw: the run time on
# NVIDIA's driver) and run on a machine with a GPU; what each prints must be NAME.exp.
#   CUDA_SSH="ssh -p PORT user@host"   the machine (its directory /root/ptxrun is used)
#   MCC=build/stage0/mcc               the compiler, run here
#   POD_MCC=/path/to/mcc               a native mcc on that machine: then everything is its work there (the run
#                                      time by mkcuda.sh's hw mode, the tests compiled and linked by it)
# Without POD_MCC the objects are made here and linked there by the machine's gcc against its C library; the
# run time is compiled there by gcc too (with libc/src/f16.c, the half-float helpers that mcc's objects call);
# its device library (cuda/hw/mklib.sh) is made here.
: ${CUDA_SSH:?set CUDA_SSH to the ssh command of a machine with a GPU}
MCC=${MCC:-build/stage0/mcc}
S=tests/cuda
T=build/test/cuda/gpumcc
R=/root/ptxrun
mkdir -p $T
if [ $# -gt 0 ]; then tests="$*"; else
    tests=$(for f in $S/*.cu; do n=$(basename $f .cu); [ -f $S/$n.exp ] && echo $n; done)
fi
if [ -n "$POD_MCC" ]; then
    COPYFILE_DISABLE=1 tar --no-xattrs -cf - mkcuda.sh cuda/hw cuda/include cuda/devinc cuda/errtab.h cuda/algo.c cuda/mc.h libc/src/f16.c \
        libc/src/math.c libc/src/mathd.c libc/src/mathx.c libc/src/mathimpl.h libc/src/mathtab.h libc/src/int128.c \
        $(for t in $tests; do echo $S/$t.cu; done) | \
        $CUDA_SSH "rm -rf $R/tree && mkdir -p $R/tree $R/inst $R/t && cd $R/tree && tar xf - 2>/dev/null; sh mkcuda.sh $POD_MCC $R/inst hw" || { echo "the run time does not build"; exit 1; }
else
    sh cuda/hw/mklib.sh $MCC $(dirname $MCC) $T/lib.c || { echo "the device library does not build"; exit 1; }
    COPYFILE_DISABLE=1 tar --no-xattrs -cf - cuda/hw cuda/include cuda/errtab.h cuda/algo.c cuda/mc.h libc/src/f16.c -C $T lib.c | $CUDA_SSH "rm -rf $R/src && mkdir -p $R/src $R/t && cd $R/src && tar xf - && \
        mkdir -p obj && for f in cuda/hw/*.c lib.c libc/src/f16.c; do gcc -std=gnu89 -O2 -Wall -Wno-unused -Icuda -Icuda/include -Icuda/hw -c \$f -o obj/\$(basename \$f .c).o || exit 1; done && \
        rm -f libmcudahw.a && ar rcs libmcudahw.a obj/*.o" || { echo "the run time does not build"; exit 1; }
fi
pass=0; fail=0
for t in $tests; do
    flags=$(sed -n 's/^.*nvcc: *\(.*\)$/\1/p' $S/$t.cu | head -1)
    arch=
    case "$flags" in *compute_120a*) arch="--cuda-arch=1200a";; esac
    if [ -n "$POD_MCC" ]; then
        $CUDA_SSH "cd $R/t && $POD_MCC $arch -I$R/inst/cudahw/include -c ../tree/$S/$t.cu -o $t.o 2>&1 && $POD_MCC -o $t $t.o -L$R/inst/cudahw/lib64 -lcudart 2>&1 && ./$t 2>$t.stderr" > $T/$t.out 2>&1
    else
        if ! $MCC --target=x86_64-linux-gnu $arch -c $S/$t.cu -o $T/$t.o --ptx-out=$T/$t.ptx 2> $T/$t.err; then
            echo "FAIL $t (compile)"; head -3 $T/$t.err; fail=$((fail + 1)); continue
        fi
        cat $T/$t.o | $CUDA_SSH "cd $R/t && cat > $t.o && gcc -no-pie -o $t $t.o ../src/libmcudahw.a -lm 2>&1 && ./$t 2>$t.stderr" > $T/$t.out 2>&1
    fi
    same=0
    if cmp -s $T/$t.out $S/$t.exp; then same=1
    elif [ $t = e2e ]; then   # a real device prints its threads' lines in the order it runs them
        sort $T/$t.out > $T/$t.out.s; sort $S/$t.exp | cmp -s - $T/$t.out.s && same=1
    fi
    if [ $same = 1 ]; then pass=$((pass + 1)); else
        echo "FAIL $t: diff $T/$t.out $S/$t.exp"; $CUDA_SSH "head -5 $R/t/$t.stderr" 2>/dev/null; fail=$((fail + 1))
    fi
done
echo "gpumcc: $pass pass${fail:+, $fail fail}" | sed 's/, 0 fail//'
[ $fail = 0 ]
