#!/bin/sh
# mklib.sh MCC DIR OUT.c: makes the device library of mcudahw (the C file with the table hw_lib) from its
# sources in cuda/hw/dev. MCC is a compiler for this machine whose CUDA headers are in DIR/cuda/include; the
# sources are compiled as programs of this machine, and dev/mklib.c, linked with them, writes out their PTX.
#   HOSTED=--hosted   what makes MCC compile and link for this machine (on macOS; nothing where it is the default)
MCC=$1; D=$2; OUT=$3
HOSTED=${HOSTED---hosted}
H=$(dirname $0)
W=$(dirname $OUT)/mklib.d
mkdir -p $W || exit 1
# The double-precision functions and the 128-bit division are the C library's: its sources, with every function definition marked as
# code for both sides (a definition starts in the first column with its type and ends with a parenthesis, a comma or its body).
L=${LIBCSRC:-$H/../../libc/src}
for f in mathimpl.h mathtab.h math.c mathd.c mathx.c int128.c; do
    sed -E '/^(static )?(inline )?(double|float|int|long|void|u64|u32|i64) [A-Za-z_0-9]+\(.*(\)|,|\{.*)$/s/^/__host__ __device__ /' $L/$f > $W/$f || exit 1
done
objs=
for u in mathf mathd mem; do
    $MCC $HOSTED -isystem $D/cuda/include -include $D/cuda/include/mcuda_device.h -I$W -c $H/dev/$u.cu -o $W/$u.o --ptx-out=$W/$u.ptx || exit 1
    objs="$objs $W/$u.o"
done
$MCC $HOSTED -I$D/cuda/include -I$H -c $H/dev/mklib.c -o $W/mklib.o || exit 1
$MCC $HOSTED -o $W/mklib $W/mklib.o $objs || exit 1
$W/mklib $OUT
