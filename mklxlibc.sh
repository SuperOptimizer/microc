#!/bin/sh
# mklxlibc.sh MCC DIR [ARCH]: build the Linux C library with MCC and install it under DIR/linux
# (ARCH aarch64, the default) or DIR/linux-x64 (ARCH x86_64)
set -e
MCC=$1; D=$2/linux; T=$D/obj
CC="$MCC --target=aarch64-linux-gnu"
SYS=libc/linux/sys
ARCHSRC=
if [ "$3" = x86_64 ]; then
    D=$2/linux-x64; T=$D/obj
    CC="$MCC --target=x86_64-linux-gnu -mno-glibc" # (an mcc built on glibc defaults to its headers)
    SYS=libc/linux/sys/x86_64
    ARCHSRC="$SYS/arch_x64.c $SYS/math_x64.c $SYS/math99_x64.c"
    # the compiler's own headers for use with the GNU C library (--sysroot, -mglibc)
    rm -rf $2/glibc && mkdir -p $2/glibc && cp -R libc/glibc/include $2/glibc/include
    # glibc/lib/libmcc.a: what compiled code calls unasked in glibc mode (the 16-bit float conversions, 128-bit division);
    # the driver adds it to every glibc link, and a program with _Float16 does not link without it
    # (against the machine's headers on an x86-64 Linux, or GLIBC_SYSROOT's elsewhere; skipped when there are neither)
    GS=; [ -n "$GLIBC_SYSROOT" ] && GS=--sysroot=$GLIBC_SYSROOT
    if [ -n "$GS" ] || { [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; }; then
        mkdir -p $2/glibc/lib $2/glibc/obj
        for f in f16 int128; do $MCC --target=x86_64-linux-gnu -mglibc $GS -c libc/src/$f.c -o $2/glibc/obj/$f.o; done
        rm -f $2/glibc/lib/libmcc.a
        $MCC ar $2/glibc/lib/libmcc.a $2/glibc/obj/f16.o $2/glibc/obj/int128.o 2>/dev/null ||
            ${AR:-llvm-ar} rcs $2/glibc/lib/libmcc.a $2/glibc/obj/f16.o $2/glibc/obj/int128.o
    fi
fi
mkdir -p $D/lib $T
rm -rf $D/include && cp -R libc/linux/include $D/include
objs=
for f in $SYS/syscall.S $ARCHSRC libc/linux/src/*.c libc/src/string.c libc/src/ctype.c libc/src/fmt.c \
         libc/src/stdlib.c libc/src/strtod.c libc/src/math.c libc/src/math99.c libc/src/mathgnu.c libc/src/mathd.c libc/src/mathx.c libc/src/mathf.c libc/src/stdio.c libc/src/misc.c libc/src/f16.c libc/src/int128.c libc/src/prof.c; do
    o=$T/$(basename ${f%.*}).o
    [ $f = libc/linux/src/sys.c ] && o=$T/lxsys.o
    $CC -D__environ=environ -c $f -o $o
    objs="$objs $o"
done
$CC -c $SYS/crt1.S -o $D/lib/crt1.o
rm -f $D/lib/libc.a
$MCC ar $D/lib/libc.a $objs 2>/dev/null || ${AR:-llvm-ar} rcs $D/lib/libc.a $objs
