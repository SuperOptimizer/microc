#!/bin/sh
# cerr.sh DIR: C diagnostics. tests/cerr/NAME.c's first line is "// error: TEXT" (DIR/mcc must fail with TEXT in its message),
# "// warning: TEXT" (must compile, with TEXT on stderr) or "// ok" (must compile, with nothing on stderr); a second line
# "// flags: ..." gives the options (default -std=c23).
D=${1:?DIR}; S=tests/cerr; T=build/test/cerr; mkdir -p $T
pass=0; fail=0
for f in $S/*.c; do
    n=$(basename $f .c); want=$(head -1 $f); fl=$(sed -n '2s|^// flags: ||p' $f)
    [ -n "$fl" ] || fl=-std=c23
    $D/mcc $fl -c $f -o $T/$n.o > $T/$n.log 2>&1; rc=$?
    case "$want" in
    "// ok") [ $rc = 0 ] && [ ! -s $T/$n.log ] && pass=$((pass + 1)) || { echo "FAIL cerr $n: rc $rc: $(head -2 $T/$n.log)"; fail=$((fail + 1)); } ;;
    "// warning: "*) msg=${want#// warning: }
        if [ $rc = 0 ] && grep -qF "$msg" $T/$n.log; then pass=$((pass + 1)); else echo "FAIL cerr $n: wanted a warning with: $msg (rc $rc)"; fail=$((fail + 1)); fi ;;
    "// error: "*) msg=${want#// error: }
        if [ $rc != 0 ] && grep -qF "$msg" $T/$n.log; then pass=$((pass + 1)); else echo "FAIL cerr $n: wanted an error with: $msg (rc $rc)"; fail=$((fail + 1)); fi ;;
    *) echo "FAIL cerr $n: no '// error:', '// warning:' or '// ok' line"; fail=$((fail + 1)) ;;
    esac
done
echo "cerr: $pass passed, $fail failed"
[ $fail = 0 ]
