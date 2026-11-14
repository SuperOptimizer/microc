#!/bin/sh
# Builds the C library with the given compiler. usage: mklibc.sh path/to/mcc stage-dir
set -e
# (the library for arm64 macOS: named, so that an mcc built on another machine cross-builds it rather than
# compiling it against that machine's headers)
MCC="$1 --target=arm64-apple-darwin -mno-glibc"
DIR=$2
mkdir -p "$DIR/include" "$DIR/lib" "$DIR/libc"
mkdir -p "$DIR/include/sys"
cp libc/include/*.h "$DIR/include/"
cp libc/include/sys/*.h "$DIR/include/sys/"
objs=""
for src in libc/src/*.c; do
    obj="$DIR/libc/$(basename "$src" .c).o"
    $MCC -c -o "$obj" "$src"
    objs="$objs $obj"
done
for src in libc/sys/*.s; do
    obj="$DIR/libc/$(basename "$src" .s).o"
    $1 as -o "$obj" "$src"
    objs="$objs $obj"
done
$1 ar "$DIR/lib/libc.a" $objs
