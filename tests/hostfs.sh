#!/bin/sh
# hostfs.sh OUTDIR: test the root file system of `mkhostfs.sh MCC OUTDIR` on an x86-64 Linux machine, without
# Docker: OUTDIR/hostfs.tar is unpacked there and
#   - every file in it is examined (readelf of that machine): an ELF file must be a static executable or a
#     relocatable object of ours - nothing with an interpreter or a dynamic section;
#   - tests/hostfs/smoke.sh runs inside it as root of a user namespace, through bubblewrap (`bwrap --bind
#     rootfs / --unshare-all`: no network, nothing of the machine visible but /proc and a /dev of its own),
#     started by the image's own init as the Dockerfile's ENTRYPOINT would;
#   - when the image has ssh, tests/hostfs/ssh.sh runs in a second such container: server and client over the
#     loopback of its empty network, as a user that is not root (see below).
#   X64_SSH="ssh user@host"   how to reach the machine (it must have bwrap; no root needed)
#   X64_DIR=dir               a directory there that this may use (default /root/x64); it works in DIR/hostfs
#   UFSM=dir                  a clone of ufsm: its tests that need no GPU are built and run in the image too
#                             (tests/hostfs/ufsm.sh; UFSM_TESTS="test_x ..." names others)
#   GPU=1                     the machine has NVIDIA's driver: its libcuda and libnvidia-ptxjitcompiler
#                             (read-only) and the /dev/nvidia* nodes are bound into the image, where and under
#                             the names NVIDIA's container run time puts them, and tests/hostfs/gpu.sh runs too
#                             (UFSM_GPU_TESTS="test_nn ...": tests of ufsm to run on the GPU, timed)
#   DOCKER=1                  the machine has Docker and this user may use it: the image is built from its
#                             Dockerfile and the same scripts run in real containers too (with GPU: under
#                             --gpus all, with what NVIDIA's container run time injects and nothing bound by
#                             hand); ssh as root there, with --network none. Images and containers are named
#                             mcc-hostfs-test* and removed.
O=$1
R=${X64_DIR:-/root/x64}/hostfs
W=build/hostfstest
[ -f "$O/hostfs.tar" ] || { echo "usage: tests/hostfs.sh OUTDIR (of mkhostfs.sh)"; exit 1; }
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/hostfs.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
rm -rf $W; mkdir -p $W/in/work
cp $O/hostfs.tar $O/Dockerfile $W/in/
cp tests/hostfs/smoke.sh tests/hostfs/ssh.sh $W/in/work/
if [ -n "$GPU" ]; then cp tests/hostfs/gpu.sh tests/hostfs/ltcmp.cu $W/in/work/; fi
if [ -n "$UFSM" ]; then
    cp tests/hostfs/ufsm.sh $W/in/work/
    (cd $UFSM && LC_ALL=C tar cf - Makefile src tests third_party 2>/dev/null) > $W/in/work/ufsm.tar
fi
LC_ALL=C tar cf $W/in/work/src.tar src 2>/dev/null
cat > $W/in/run.sh <<'REOF'
rm -rf rootfs; mkdir rootfs
tar -xf hostfs.tar -C rootfs
echo "== files =="
find rootfs -type f | while read f; do
    if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
        t=$(readelf -hW "$f" | sed -n 's/^ *Type: *\([A-Z]*\).*/\1/p')
        dyn=$(readelf -lW "$f" 2>/dev/null | grep -c -E 'INTERP|DYNAMIC')
        echo "elf $t dyn=$dyn $f"
    elif [ "$(head -c 7 "$f")" = '!<arch>' ]; then
        echo "archive $f"
    elif [ "$(head -c 2 "$f")" = '#!' ]; then
        echo "script $f"
    else
        echo "data $f"
    fi
done > files.txt
grep -c '^elf EXEC dyn=0' files.txt | sed 's/^/static executables: /'
grep -c '^elf REL' files.txt | sed 's/^/relocatable objects: /'
grep -c '^archive' files.txt | sed 's/^/archives: /'
grep -c '^script' files.txt | sed 's/^/scripts: /'
grep -c '^data' files.txt | sed 's/^/other files (headers, text): /'
grep '^elf' files.txt | grep -v -e '^elf EXEC dyn=0' -e '^elf REL dyn=0' | sed 's/^/FAIL not ours: /'
if [ -n "$DOCKER" ]; then cp -R work dwork; fi
gpu=
if [ -f work/gpu.sh ]; then
    for l in libcuda.so.1 libnvidia-ptxjitcompiler.so.1; do
        p=
        for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
            if [ -z "$p" ] && [ -e $d/$l ]; then p=$(readlink -f $d/$l); fi
        done
        # under its full name alone (libcuda.so.615.71.10), as the run time's CDI specification mounts it: the
        # short name is a link its ldconfig hook makes, which the image must not need
        if [ -n "$p" ]; then gpu="$gpu --ro-bind $p /usr/lib/x86_64-linux-gnu/$(basename $p)"; else echo "FAIL this machine has no $l"; fi
    done
    for d in /dev/nvidiactl /dev/nvidia-uvm /dev/nvidia-uvm-tools /dev/nvidia[0-9]*; do
        if [ -e $d ]; then gpu="$gpu --dev-bind $d $d"; fi
    done
fi
echo "== smoke =="
bwrap --bind rootfs / --proc /proc --dev /dev --tmpfs /tmp --bind work /work $gpu --unshare-all --as-pid-1 --uid 0 --gid 0 \
    --clearenv --setenv PATH /usr/bin --setenv HOME /root --setenv UFSM_TESTS "$UFSM_TESTS" \
    --setenv UFSM_GPU_TESTS "$UFSM_GPU_TESTS" --chdir /work \
    /usr/bin/init /bin/sh -c 'sh /work/smoke.sh; if [ -f /work/ufsm.sh ]; then sh /work/ufsm.sh; fi; if [ -f /work/gpu.sh ]; then sh /work/gpu.sh; fi' 2>&1
echo "exit $?"
# ssh, when the image has it: server and client over the loopback of a network that is the container's alone.
# As a user that is not root: in a user namespace made without privilege nobody may set groups, which the
# server does for root before it starts root's shell (under Docker it can).
if [ -e rootfs/usr/dropbearmulti ]; then
    echo "== ssh =="
    printf 'tester:x:1000:1000::/work/home:/bin/sh\n' >> rootfs/etc/passwd
    mkdir -p work/home && chmod 700 work/home
    bwrap --bind rootfs / --proc /proc --dev /dev --tmpfs /tmp --bind work /work --unshare-all --as-pid-1 --uid 1000 --gid 1000 \
        --clearenv --setenv PATH /usr/bin --setenv HOME /work/home --chdir /work /usr/bin/init /bin/sh /work/ssh.sh 2>&1
    echo "ssh exit $?"
fi
# Docker: the image built from the Dockerfile as it is, then the same scripts in real containers (the work
# files go in as a layer on top, so that nothing of root's is left on this machine). With GPU, --gpus all:
# whatever NVIDIA's run time puts into the container is all the GPU checks get. ssh as root, with no network.
if [ -n "$DOCKER" ]; then
    echo "== docker =="
    docker build -q -t mcc-hostfs-test . > docker.out 2>&1 || { echo "FAIL docker-build"; cat docker.out; }
    printf 'FROM mcc-hostfs-test\nCOPY dwork /work\n' > Dockerfile.work
    docker build -q -t mcc-hostfs-test-work -f Dockerfile.work . > docker.out 2>&1 || { echo "FAIL docker-build-work"; cat docker.out; }
    g=; if [ -f work/gpu.sh ]; then g="--gpus all"; fi
    is() { if [ "$2" = "$3" ]; then echo "ok $1"; else echo "FAIL $1: got '$2', want '$3'"; fi; }
    is docker-pid1 "$(docker run --rm --name mcc-hostfs-test-0 mcc-hostfs-test /bin/sh -c 'cat /proc/1/comm; pwd; echo $PATH $HOME' 2>&1 | tr '\n' ' ')" "init /root /usr/bin /root "
    is docker-status "$(docker run --rm --name mcc-hostfs-test-0 mcc-hostfs-test /bin/sh -c 'exit 7' 2>&1; echo $?)" 7
    docker run --rm --name mcc-hostfs-test-1 $g -w /work -e UFSM_TESTS="$UFSM_TESTS" -e UFSM_GPU_TESTS="$UFSM_GPU_TESTS" mcc-hostfs-test-work \
        /bin/sh -c 'sh /work/smoke.sh; if [ -f /work/ufsm.sh ]; then sh /work/ufsm.sh; fi; if [ -f /work/gpu.sh ]; then sh /work/gpu.sh; fi' 2>&1 |
        sed -e 's/^ok /ok docker-/' -e 's/^FAIL /FAIL docker-/' -e 's/^== /== docker: /'
    if [ -e rootfs/usr/dropbearmulti ]; then
        docker run --rm --name mcc-hostfs-test-2 --network none -w /work mcc-hostfs-test-work /bin/sh /work/ssh.sh 2>&1 |
            sed -e 's/^ok /ok docker-/' -e 's/^FAIL /FAIL docker-/' -e 's/^== /== docker: /'
    fi
    docker rmi mcc-hostfs-test-work mcc-hostfs-test > /dev/null 2>&1
    echo "== docker over =="
fi
REOF
(cd $W/in && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && DOCKER='$DOCKER' UFSM_TESTS='$UFSM_TESTS' UFSM_GPU_TESTS='$UFSM_GPU_TESTS' sh run.sh 2>&1; cd .. && rm -rf $R" > $W/log
grep -v '^ok ' $W/log
pass=$(grep -c '^ok ' $W/log); fail=$(grep -c '^FAIL' $W/log)
grep -q '^== done ==' $W/log && grep -q '^exit 0' $W/log || { echo "FAIL the run did not finish (see $W/log)"; fail=$((fail + 1)); }
if [ -n "$DOCKER" ]; then
    grep -q '^== docker: done ==' $W/log && grep -q '^== docker over ==' $W/log || { echo "FAIL the run in Docker did not finish (see $W/log)"; fail=$((fail + 1)); }
fi
echo "hostfs: $pass passed, $fail failed"
[ "$fail" = 0 ]
