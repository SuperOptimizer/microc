#!/bin/sh
# tests/ext/libm/run.sh DIR: test the math library of the compiler in DIR (its --hosted library, so that
# the test runs here). Prints "libm: N functions ok"; the exit status is nonzero on failure.
#
# expected.txt holds, per function, arguments and results as bit patterns (see check.c). It was made by
#   cc -DHOSTGEN -w check.c -o hostcheck                   (the host's library gives the expected values ...)
#   python3 gen.py 37 2026 | ./hostcheck eval > host.txt
#   python3 verify.py --refine host.txt > expected.txt     (... and mpmath replaces them by correctly rounded ones)
# so a result may differ from it by one unit in the last place where funcs.h says 1, and not at all elsewhere.
# "check -v expected.txt" lists the largest difference per function; verify.py measures true errors.
D=${1:-build/stage0}
here=$(dirname "$0")
out=build/test/libm
mkdir -p $out || exit 1
"$D/mcc" --hosted "$here/check.c" -o $out/check || { echo "libm: check.c does not build"; exit 1; }
$out/check "$here/expected.txt"
