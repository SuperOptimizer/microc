#!/bin/sh
# mkmux.sh MCC DIR: for x86-64 Linux and mcc's own C library, the GPU's run time (cuda/hw: mcudahw) and the
# library that holds it and the CPU's and chooses when the program runs (cuda/mux/mux.c). mkcuda.sh calls it
# after it has made DIR/linux-x64/cuda/lib/libmcuda.a, whose objects are in DIR/linux-x64/obj/cuda. Makes, in
# DIR/linux-x64/cuda/lib:
#   libmcudahw.a   the GPU's run time alone: NVIDIA's driver is loaded by the C library's dlopen
#                  (libc/linux/src/dl.c), cublasLt is our own kernel (cuda/hw/ltk.cu)
#   libcudart.a    both run times and the choice
#   HOSTCC   a compiler for programs of this machine (default: MCC --hosted), for cuda/mux/symprefix.c
set -e
MCC=$1; D=$2
HOSTCC=${HOSTCC:-$MCC --hosted}
CC="$MCC --target=x86_64-linux-gnu -mno-glibc"
C=$D/linux-x64/obj/cuda
H=$D/linux-x64/obj/cudahw
L=$D/linux-x64/cuda/lib
rm -rf $H && mkdir -p $H/cpu $H/gpu
# the GPU's run time; its device library is PTX, made by programs of this machine (mklib.sh)
sh cuda/hw/mklib.sh "$MCC" $D $H/lib.c
hw=
for f in cuda/hw/*.c $H/lib.c; do
    o=$H/$(basename ${f%.*}).o
    $CC -DMCUDA_LT_OWN -Icuda -Icuda/include -Icuda/hw -c $f -o $o
    hw="$hw $o"
done
# cublasLt's kernel: a unit of device code like a program's, without fused multiply-add (it rounds as the CPU's does)
$CC -fmad=false -isystem $D/cuda/include -include $D/cuda/include/mcuda_device.h -c cuda/hw/ltk.cu -o $H/ltk.o
rm -f $L/libmcudahw.a
$MCC ar $L/libmcudahw.a $hw $H/ltk.o
# both in one: the global names each defines, and those names with mcpu_ or mgpu_ before them in its objects
# (which get names of their own too: an archive has one member of a name)
cpu=
for o in $C/*.o; do
    case $o in */empty.o) ;; *) cpu="$cpu $o";; esac
done
$MCC elf nm $cpu | awk 'NF == 3 && $2 ~ /^[TDBRWVC]$/ { print $2, $3 }' | sort -u > $H/cpu.sym
$MCC elf nm $hw | awk 'NF == 3 && $2 ~ /^[TDBRWVC]$/ { print $2, $3 }' | sort -u > $H/gpu.sym
awk '{ print $2 }' $H/cpu.sym > $H/cpu.names
awk '{ print $2 }' $H/gpu.sym > $H/gpu.names
$HOSTCC -o $H/symprefix cuda/mux/symprefix.c
objs=
for o in $cpu; do
    $H/symprefix mcpu_ $H/cpu.names $o $H/cpu/cpu_$(basename $o)
    objs="$objs $H/cpu/cpu_$(basename $o)"
done
for o in $hw; do
    $H/symprefix mgpu_ $H/gpu.names $o $H/gpu/gpu_$(basename $o)
    objs="$objs $H/gpu/gpu_$(basename $o)"
done
# the public functions: a stub and a slot each, and the two tables the slots are filled from
(sed 's/^/cpu /' $H/cpu.sym; sed 's/^/gpu /' $H/gpu.sym) | awk -v tab=$H/tab.c -v stubs=$H/stubs.S '
    $2 == "T" && $3 ~ /^(cuda|cublas|cu[A-Z]|__mcuda_)/ && $3 != "__mcuda_register" {
        if (!($3 in seen)) { seen[$3] = 1; name[n++] = $3 }
        has[$1, $3] = 1
    }
    END {
        print "/* made by cuda/mux/mkmux.sh */" > tab
        print "void __mcuda_mux_first(void);\nvoid __mcuda_mux_missing(const char *name);" > tab
        for (i = 0; i < n; i++) {
            if (has["cpu", name[i]]) print "void mcpu_" name[i] "(void);" > tab
            else print "static void mcpu_" name[i] "(void) { __mcuda_mux_missing(\"" name[i] "\"); }" > tab
            if (has["gpu", name[i]]) print "void mgpu_" name[i] "(void);" > tab
            else print "static void mgpu_" name[i] "(void) { __mcuda_mux_missing(\"" name[i] "\"); }" > tab
        }
        print "void *__mcuda_mux_cpu[] = {" > tab
        for (i = 0; i < n; i++) print "    (void *)mcpu_" name[i] "," > tab
        print "    0};\nvoid *__mcuda_mux_gpu[] = {" > tab
        for (i = 0; i < n; i++) print "    (void *)mgpu_" name[i] "," > tab
        print "    0};\nvoid *__mcuda_mux_slot[] = {" > tab
        for (i = 0; i < n; i++) print "    (void *)__mcuda_mux_first," > tab
        print "    0};\nconst int __mcuda_mux_n = " n ";" > tab
        print "/* made by cuda/mux/mkmux.sh */\n\t.text" > stubs
        for (i = 0; i < n; i++) {
            print "\t.globl " name[i] "\n\t.type " name[i] ", %function\n" name[i] ":" > stubs
            print "\tleaq __mcuda_mux_slot+" 8 * i "(%rip), %r11\n\tjmp *(%r11)" > stubs
            print "\t.size " name[i] ", . - " name[i] > stubs
        }
    }'
for f in cuda/mux/mux.c cuda/mux/first.S $H/tab.c $H/stubs.S; do
    o=$H/$(basename ${f%.*}).o
    $CC -c $f -o $o
    objs="$objs $o"
done
rm -f $L/libcudart.a
$MCC ar $L/libcudart.a $objs $H/ltk.o
