#!/bin/sh
# mkbox.sh MCC DIR: build mbox (the shell and the Unix tools a build needs) with `mcc --hosted` and install
# DIR/bin, a directory that holds the whole toolchain by name: the shell, the tools, make, lex, yacc, the
# compiler and the cross binutils. A build run with PATH=DIR/bin uses nothing else.
set -e
MCC=$1; D=$2; T=$D/host/obj/box
mkdir -p $T $D/bin
objs=
for f in box/*.c; do
    o=$T/$(basename ${f%.*}).o
    $MCC --hosted -c $f -o $o
    objs="$objs $o"
done
$MCC --hosted -o $D/mbox $objs
for a in $($D/mbox --list); do
    ln -sf ../mbox "$D/bin/$a"
done
for a in mcc make gmake lex flex yacc bison cc; do
    ln -sf ../mcc $D/bin/$a
done
for a in gcc ld ar nm objcopy objdump strip; do
    ln -sf ../mcc $D/bin/aarch64-linux-gnu-$a
done
