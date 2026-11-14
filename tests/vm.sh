#!/bin/sh
# vm.sh DIR: test mvm, the virtual machine in DIR. The guest in tests/vm is built with DIR's own cross tools,
# then run by the interpreter and, where the host allows it, on the real CPU; both must print tests/vm/expect.
D=$1; T=$D/test-vm; S=tests/vm
mkdir -p $T
fail=0
$D/mcc --target=aarch64-linux-gnu -ffreestanding -c $S/guest.S -o $T/guest.o &&
$D/mcc --target=aarch64-linux-gnu -ffreestanding -c $S/main.c -o $T/main.o &&
$D/mcc elf ld -T $S/link.ld -o $T/guest.elf $T/guest.o $T/main.o &&
$D/mcc elf objcopy -O binary $T/guest.elf $T/guest.img || { echo "vm: cannot build the guest"; exit 1; }
for a in emu hvf; do
    $D/mvm -accel $a -m 64 -kernel $T/guest.img </dev/null >$T/$a.out 2>$T/$a.err
    rc=$?
    if [ $a = hvf ] && grep -q "hypervisor is not available" $T/$a.err; then
        echo "vm: hvf skipped (no hypervisor here)"
    elif [ $rc = 0 ] && cmp -s $T/$a.out $S/expect; then
        echo "vm: $a ok"
    else
        echo "vm: $a FAILED (see $T/$a.out, $T/$a.err)"
        fail=1
    fi
done
exit $fail
