#!/bin/sh
# box.sh DIR: test the shell and tools installed in DIR/bin (see mkbox.sh). Each tests/box/*.sh is run by that
# shell with nothing else on PATH, in an empty directory $W; its output must match the .expect file.
D=$(cd "$1" && pwd); S=$(cd tests/box && pwd); OUT=$(pwd)/build/test/boxrun
pass=0; fail=0
for t in $S/*.sh; do
    name=$(basename $t .sh)
    rm -rf $OUT/$name; mkdir -p $OUT/$name
    (cd $OUT/$name && T=$S W=$OUT/$name PATH=$D/bin $D/bin/sh ../../../../tests/box/$name.sh) >$OUT/$name.out 2>&1
    echo "exit $?" >>$OUT/$name.out
    if cmp -s $OUT/$name.out $S/$name.expect; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); echo "FAIL: box/$name (diff $OUT/$name.out $S/$name.expect)"
    fi
done
echo "box: $pass passed, $fail failed"
[ $fail = 0 ]
