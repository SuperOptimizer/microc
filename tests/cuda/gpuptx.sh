#!/bin/sh
# gpuptx.sh [test...]: the PTX test modules of tests/cuda (NAME.ptx with a NAME.exp) run on a real GPU by
# gpuptx.c, and what the device prints compared with NAME.exp, which is what our engine must print (ptxt.c,
# tests/cuda.sh). A difference is a line "DIFF NAME: n lines" and the device's output stays in
# build/test/cuda/gpuptx/NAME.out; tests/cuda/gpuptx.txt says which differences are known and why.
#   CUDA_TESTS  where the tests are (default tests/cuda)
#   CUDA_SSH="ssh -p PORT user@host"  the machine with the GPU     CUDA_DIR  a directory there (default /root/ptxrun)
S=${CUDA_TESTS:-tests/cuda}; R=${CUDA_DIR:-/root/ptxrun}; O=build/test/cuda/gpuptx
[ -n "$CUDA_SSH" ] || { echo "gpuptx: set CUDA_SSH"; exit 2; }
if [ $# -gt 0 ]; then tests="$*"; else tests=$(for f in $S/*.exp; do n=$(basename $f .exp); [ -f $S/$n.ptx ] && echo $n; done); fi
mkdir -p $O
export COPYFILE_DISABLE=1
( cd tests/cuda && tar --no-xattrs -cf - gpuptx.c ) | $CUDA_SSH "mkdir -p $R && cd $R && tar xf -" || exit 1
( cd $S && tar --no-xattrs -cf - *.ptx ) | $CUDA_SSH "cd $R && tar xf - 2>/dev/null && gcc -O1 -I/usr/local/cuda/include -o gpuptx gpuptx.c -L/usr/local/cuda/lib64 -lcuda" || exit 1
same=0; diff=0
for t in $tests; do
    $CUDA_SSH "cd $R && ./gpuptx $t.ptx" > $O/$t.out 2>&1
    if cmp -s $O/$t.out $S/$t.exp; then same=$((same + 1)); else
        diff=$((diff + 1)); echo "DIFF $t: $(diff $O/$t.out $S/$t.exp | grep -c '^[<>]') lines"
    fi
done
echo "gpuptx: $same the same${diff:+, $diff differ}" | sed 's/, 0 differ//'
