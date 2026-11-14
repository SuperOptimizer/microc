#!/bin/sh
# mkhostlibc.sh MCC DIR: build the POSIX flavour of the C library for this machine (what `mcc --hosted` uses)
# and install it under DIR/host. Its headers are the neutral ones of the Linux library plus Darwin's layouts.
set -e
MCC=$1; D=$2/host; T=$D/obj
mkdir -p $D/lib $T
rm -rf $D/include && mkdir -p $D/include/sys
for h in alloca assert ctype float getopt inttypes libgen limits locale malloc math fenv regex fnmatch stdarg stdbool \
         stddef stdint stdio stdlib string strings time unistd byteswap endian features; do
    cp libc/linux/include/$h.h $D/include/
done
cp libc/include/setjmp.h $D/include/
cp -R libc/darwin/include/. $D/include/
objs=
for f in libc/src/*.c libc/linux/src/posix.c libc/linux/src/regex.c libc/darwin/src/*.c; do
    o=$T/$(basename ${f%.*}).o
    $MCC --hosted -D__MCC_POSIX__ -c $f -o $o
    objs="$objs $o"
done
for f in libc/sys/*.s; do
    o=$T/$(basename ${f%.*}).o
    $MCC as -o $o $f
    objs="$objs $o"
done
rm -f $D/lib/libc.a
$MCC ar $D/lib/libc.a $objs
