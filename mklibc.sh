#!/bin/sh
# Builds the C library with the given compiler. usage: mklibc.sh path/to/mcc stage-dir
set -e
MCC=$1
DIR=$2
mkdir -p "$DIR/include" "$DIR/lib" "$DIR/libc"
mkdir -p "$DIR/include/sys"
cp libc/include/*.h "$DIR/include/"
cp libc/include/sys/*.h "$DIR/include/sys/"
objs=""
for src in libc/src/*.c; do
    obj="$DIR/libc/$(basename "$src" .c).o"
    "$MCC" -c -o "$obj" "$src"
    objs="$objs $obj"
done
for src in libc/sys/*.s; do
    obj="$DIR/libc/$(basename "$src" .s).o"
    "$MCC" as -o "$obj" "$src"
    objs="$objs $obj"
done
"$MCC" ar "$DIR/lib/libc.a" $objs
