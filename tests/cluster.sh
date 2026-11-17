#!/bin/sh
# cluster.sh DIR: __cluster_dims__(x, y, z) on a kernel is PTX .reqnctapercluster x, y, z on its .entry for sm_90 on (template instances
# too), and nothing for an older target; a kernel without it has none. __maxnreg__(n) is .maxnreg n (every target); with a ptxas on PATH
# (or PTXAS=) the sm_120a module assembles and its setmaxnreg are kept. No GPU (ufsm's test_cluster launches them on the card).
D=${1:?DIR}; T=build/test/cluster; mkdir -p $T
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h"
pass=0; fail=0
ok() { pass=$((pass + 1)); }
bad() { echo "FAIL cluster: $1"; fail=$((fail + 1)); }
if $D/mcc -mglibc $CU --cuda-arch=1200a -c tests/cluster/clu.cu -o $T/a.o --ptx-out=$T/a.ptx > $T/a.log 2>&1; then
    [ "$(grep -c '^\.reqnctapercluster' $T/a.ptx)" = 3 ] && ok || bad "sm_120a: $(grep -c '^\.reqnctapercluster' $T/a.ptx) directives, not 3"
    grep -q '^\.reqnctapercluster 2, 1, 1' $T/a.ptx && grep -q '^\.reqnctapercluster 1, 1, 4' $T/a.ptx && grep -q '^\.reqnctapercluster 1, 1, 8' $T/a.ptx && ok || bad "sm_120a: shapes"
    grep -q 'cluster_ctarank' $T/a.ptx && ok || bad "sm_120a: the asm"
else bad "sm_120a compile: $(tail -2 $T/a.log)"; fi
if $D/mcc -mglibc $CU --cuda-arch=860 -c tests/cluster/clu.cu -o $T/b.o --ptx-out=$T/b.ptx > $T/b.log 2>&1; then
    grep -q 'reqnctapercluster' $T/b.ptx && bad "sm_86: a cluster directive" || ok
else bad "sm_86 compile: $(tail -2 $T/b.log)"; fi
if $D/mcc -mglibc $CU --cuda-arch=1200a -c tests/cluster/mr.cu -o $T/m.o --ptx-out=$T/m.ptx > $T/m.log 2>&1; then
    [ "$(grep -c '^\.maxnreg' $T/m.ptx)" = 2 ] && grep -q '^\.maxnreg 96' $T/m.ptx && grep -q '^\.maxnreg 64' $T/m.ptx && ok || bad "maxnreg: directives"
    P=${PTXAS:-$(command -v ptxas || echo /usr/local/cuda/bin/ptxas)}
    if [ -x "$P" ]; then
        "$P" -arch=sm_120a -v $T/m.ptx -o $T/m.cubin > $T/m.pas 2>&1 && ! grep -q C7508 $T/m.pas && grep -q 'Used 96 registers' $T/m.pas && ok \
            || bad "maxnreg: ptxas: $(grep -E 'error|C75|Used' $T/m.pas | head -3)"
    fi
else bad "maxnreg compile: $(tail -2 $T/m.log)"; fi
if $D/mcc -mglibc $CU --cuda-arch=860 -c tests/cluster/mr.cu -o $T/n.o --ptx-out=$T/n.ptx > $T/n.log 2>&1; then
    [ "$(grep -c '^\.maxnreg' $T/n.ptx)" = 2 ] && ok || bad "maxnreg sm_86: directives"
else bad "maxnreg sm_86 compile: $(tail -2 $T/n.log)"; fi
echo "cluster: $pass passed, $fail failed"
[ $fail = 0 ]
