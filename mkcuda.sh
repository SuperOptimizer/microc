#!/bin/sh
# mkcuda.sh MCC DIR: build mcuda, the CUDA runtime that runs PTX on the CPU (cuda/), with MCC, for this machine
# and for Linux, and install it: the headers under DIR/cuda/include, where the compiler looks for them when it
# reads a .cu file, and again beside each library, DIR/host/cuda/{include,lib} and DIR/linux/cuda/{include,lib};
# the library is libmcuda.a. A program links it after its own objects: mcc --hosted ... DIR/host/cuda/lib/libmcuda.a
# For Linux it is also DIR/cuda/lib64/libcudart.a, with a libcublasLt.a and a libstdc++.a that hold nothing.
#
# mkcuda.sh MCC DIR hw: build mcudahw instead, the same run time on NVIDIA's driver (cuda/hw), for the machine
# this runs on, an x86-64 Linux with the driver: the kernels then run on its GPU. MCC must compile for that
# machine and its C library as it stands (HWCC: the command, default MCC). Installed like NVIDIA's toolkit
# under DIR/cudahw: include, and lib64 with libcudart.a (all of it), libcublasLt.a and libstdc++.a (nothing).
set -e
MCC=$1; D=$2
if [ "$3" = hw ]; then
    CC=${HWCC:-$MCC}
    T=$D/cudahw/obj
    rm -rf $D/cudahw && mkdir -p $T $D/cudahw/lib64 $D/cudahw/include $T/inc/cuda
    cp -R cuda/include/. $D/cudahw/include/
    if [ -d cuda/devinc ]; then cp -R cuda/devinc/. $D/cudahw/include/; fi
    # (the device library is made of programs of this machine: mklib.sh wants the headers as DIR/cuda/include)
    cp -R $D/cudahw/include $T/inc/cuda/include
    HOSTED="${HWHOSTED-}" sh cuda/hw/mklib.sh "$CC" $T/inc $T/lib.c
    objs=
    for f in cuda/hw/*.c $T/lib.c libc/src/f16.c; do
        o=$T/$(basename ${f%.*}).o
        $CC -Icuda -Icuda/include -Icuda/hw -c $f -o $o
        objs="$objs $o"
    done
    ar rcs $D/cudahw/lib64/libcudart.a $objs
    echo 'int __mcuda_nothing_here;' > $T/empty.c
    $CC -c $T/empty.c -o $T/empty.o
    ar rcs $D/cudahw/lib64/libcublasLt.a $T/empty.o
    ar rcs $D/cudahw/lib64/libstdc++.a $T/empty.o
    exit 0
fi
rm -rf $D/cuda/include && mkdir -p $D/cuda/include
cp -R cuda/include/. $D/cuda/include/
if [ -d cuda/devinc ]; then cp -R cuda/devinc/. $D/cuda/include/; fi
for flavour in host linux; do
    T=$D/$flavour/obj/cuda
    L=$D/$flavour/cuda
    if [ $flavour = host ]; then CC="$MCC --hosted"; AS="$MCC as"; asm=cuda/fp_darwin.s
    else CC="$MCC --target=aarch64-linux-gnu"; AS="$MCC --target=aarch64-linux-gnu -c"; asm=cuda/fp_linux.s; fi
    [ -d $D/$flavour/lib ] || continue
    mkdir -p $T $L/lib
    rm -rf $L/include && cp -R $D/cuda/include $L/include
    objs=
    for f in cuda/*.c; do
        o=$T/$(basename ${f%.*}).o
        $CC -fno-strict-aliasing -Icuda -Icuda/include -c $f -o $o
        objs="$objs $o"
    done
    $AS -o $T/fp.o $asm
    objs="$objs $T/fp.o"
    rm -f $L/lib/libmcuda.a
    $MCC ar $L/lib/libmcuda.a $objs 2>/dev/null || ${AR:-llvm-ar} rcs $L/lib/libmcuda.a $objs
    if [ $flavour = linux ]; then
        # what a Makefile written for NVIDIA's toolkit links: -L$CUDA/lib64 -lcudart -lcublasLt -lstdc++.
        # libcudart.a is all of mcuda; the other two hold one empty object each
        mkdir -p $D/cuda/lib64
        echo 'int __mcuda_nothing_here;' > $T/empty.c
        $CC -c $T/empty.c -o $T/empty.o
        for l in cublasLt stdc++; do
            rm -f $D/cuda/lib64/lib$l.a
            $MCC ar $D/cuda/lib64/lib$l.a $T/empty.o 2>/dev/null || ${AR:-llvm-ar} rcs $D/cuda/lib64/lib$l.a $T/empty.o
        done
        cp $L/lib/libmcuda.a $D/cuda/lib64/libcudart.a
    fi
done
