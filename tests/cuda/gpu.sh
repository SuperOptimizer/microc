#!/bin/sh
# gpu.sh [test...]: the CUDA programs of tests/cuda that are plain CUDA, built by NVIDIA's nvcc and run on a real
# GPU: what they print must be the same tests/cuda/NAME.exp that mcc's build of them must print on our own run
# time (tests/cuda.sh). So the expectations are the hardware's, not ours. A program whose first line says
# "mcc only" uses what nvcc does not have and is left out; a line "nvcc: FLAGS" in a program adds flags
# (-arch=sm_86 is given unless they name another).
#   CUDA_SSH="ssh -p PORT user@host"  the machine with the GPU (its nvcc: NVCC, default /usr/local/cuda/bin/nvcc)
#   CUDA_DIR  a directory there to work in (default /root/tcu)     UPDATE=1  write the .exp files from what the GPU prints
S=tests/cuda; R=${CUDA_DIR:-/root/tcu}; N=${NVCC:-/usr/local/cuda/bin/nvcc}
[ -n "$CUDA_SSH" ] || { echo "gpu: set CUDA_SSH"; exit 2; }
if [ $# -gt 0 ]; then tests="$*"; else tests=$(for f in $S/*.cu; do basename $f .cu; done); fi
pass=0; fail=0; list=
for t in $tests; do
    head -1 $S/$t.cu | grep -q 'mcc only' && continue
    list="$list $t"
done
mkdir -p build/test/cuda/gpu
( cd $S && tar cf - $(for t in $list; do echo $t.cu; done) ) | $CUDA_SSH "mkdir -p $R && cd $R && tar xf -" || exit 1
for t in $list; do
    fl=$(sed -n 's/^.*nvcc: \(.*\)$/\1/p' $S/$t.cu | head -1 | sed 's/ *\*\/ *$//')
    case "$fl" in *arch=*) ar=;; *) ar=-arch=sm_86;; esac
    $CUDA_SSH "cd $R && $N -O3 $ar $fl -o $t $t.cu 2> $t.err && timeout 300 ./$t" > build/test/cuda/gpu/$t.out 2>&1
    if [ "$UPDATE" = 1 ]; then cp build/test/cuda/gpu/$t.out $S/$t.exp; fi
    if cmp -s build/test/cuda/gpu/$t.out $S/$t.exp; then pass=$((pass + 1)); else
        echo "FAIL $t: diff build/test/cuda/gpu/$t.out $S/$t.exp"; fail=$((fail + 1))
        $CUDA_SSH "head -3 $R/$t.err" 2>/dev/null
    fi
done
echo "gpu: $pass pass${fail:+, $fail fail}" | sed 's/, 0 fail//'
[ $fail = 0 ]
