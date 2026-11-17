# ufsm.sh: run inside the root file system after smoke.sh when tests/hostfs.sh was given UFSM=dir (a clone of
# ufsm): ufsm's tests that need no GPU are built there by ufsm's own Makefile, with the image's make, shell,
# compiler, linker and libraries, and run. /work/ufsm.tar is the clone (Makefile, src, tests, third_party).
mkdir -p /work/ufsm && tar -xf /work/ufsm.tar -C /work/ufsm && cd /work/ufsm || exit 1
[ -n "$UFSM_TESTS" ] || UFSM_TESTS="test_fuse test_target_erode test_papyrus test_cover test_zcache test_crush test_votes test_wind test_phase_check"
T=$UFSM_TESTS
targets=; for t in $T; do targets="$targets /work/out/$t"; done
if make -k -j8 ARCH=86 BDIR=/work/out CCACHE= HOSTCC=gcc NVCC=nvcc CUDA=/usr/cuda $targets > /work/ufsm-build.log 2>&1; then
    echo "ok ufsm-build"
else
    echo "FAIL ufsm-build"; grep -i -m 12 'error\|\*\*\*' /work/ufsm-build.log | cut -c1-300
fi
for t in $T; do
    if [ -x /work/out/$t ]; then
        if (TMPDIR=/tmp /work/out/$t > /work/$t.log 2>&1); then echo "ok ufsm-$t"; else
            echo "FAIL ufsm-$t (status $?)"; tail -3 /work/$t.log | cut -c1-300
        fi
    else
        echo "FAIL ufsm-$t (not built)"
    fi
done
echo "== ufsm done =="
