#!/bin/sh
# mkvm.sh MCC DIR: build mvm, the virtual machine that boots an arm64 Linux kernel (interpreter, or the real
# CPU through Hypervisor.framework), with `mcc --hosted`; it lands in DIR and by name in DIR/bin.
set -e
MCC=$1; D=$2; T=$D/host/obj/vm
mkdir -p $T $D/bin
objs=
for f in vm/*.c; do
    o=$T/$(basename ${f%.*}).o
    $MCC --hosted -c $f -o $o
    objs="$objs $o"
done
for f in vm/*.s; do
    o=$T/$(basename ${f%.*}).o
    $MCC as -o $o $f
    objs="$objs $o"
done
$MCC --hosted -framework Cocoa -framework Hypervisor --entitlement=com.apple.security.hypervisor -o $D/mvm $objs
ln -sf ../mvm $D/bin/mvm
