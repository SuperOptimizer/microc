#!/bin/sh
# hwflags.sh DIR: mcudahw on the card: cudaSetDeviceFlags (cudaDeviceScheduleBlockingSync) reaches the primary context and a wait
# for a long kernel then sleeps (tests/hwflags/flags.cu). Needs a GPU (SKIP without); run it under the machine's GPU lock.
D=${1:?DIR}; T=build/test/hwflags; mkdir -p $T
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h -I$D/cudahw/include"
$D/mcc -mglibc $CU --cuda-arch=${CUDA_ARCH:-860} tests/hwflags/flags.cu -o $T/flags $D/cudahw/lib64/libcudart.a -ldl -lpthread -lm > $T/build.log 2>&1 \
    || { echo "FAIL hwflags: build: $(tail -2 $T/build.log)"; exit 1; }
out=$($T/flags 2>&1); echo "$out"
case "$out" in *SKIP*) exit 0;; *"
ok"*|ok) exit 0;; *) exit 1;; esac
