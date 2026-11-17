#!/bin/sh
# hostfs.sh OUTDIR: test the root file system of `mkhostfs.sh MCC OUTDIR` on an x86-64 Linux machine, without
# Docker: OUTDIR/hostfs.tar is unpacked there and
#   - every file in it is examined (readelf of that machine): an ELF file must be a static executable or a
#     relocatable object of ours - nothing with an interpreter or a dynamic section;
#   - tests/hostfs/smoke.sh runs inside it as root of a user namespace, through bubblewrap (`bwrap --bind
#     rootfs / --unshare-all`: no network, nothing of the machine visible but /proc and a /dev of its own),
#     started by the image's own init as the Dockerfile's ENTRYPOINT would.
#   X64_SSH="ssh user@host"   how to reach the machine (it must have bwrap; no root needed)
#   X64_DIR=dir               a directory there that this may use (default /root/x64); it works in DIR/hostfs
#   UFSM=dir                  a clone of ufsm: its tests that need no GPU are built and run in the image too
#                             (tests/hostfs/ufsm.sh; UFSM_TESTS="test_x ..." names others)
O=$1
R=${X64_DIR:-/root/x64}/hostfs
W=build/hostfstest
[ -f "$O/hostfs.tar" ] || { echo "usage: tests/hostfs.sh OUTDIR (of mkhostfs.sh)"; exit 1; }
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/hostfs.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
rm -rf $W; mkdir -p $W/in/work
cp $O/hostfs.tar $W/in/
cp tests/hostfs/smoke.sh $W/in/work/
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
echo "== smoke =="
bwrap --bind rootfs / --proc /proc --dev /dev --tmpfs /tmp --bind work /work --unshare-all --as-pid-1 --uid 0 --gid 0 \
    --clearenv --setenv PATH /usr/bin --setenv HOME /root --setenv UFSM_TESTS "$UFSM_TESTS" --chdir /work \
    /usr/bin/init /bin/sh -c 'sh /work/smoke.sh; if [ -f /work/ufsm.sh ]; then sh /work/ufsm.sh; fi' 2>&1
echo "exit $?"
REOF
(cd $W/in && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && UFSM_TESTS='$UFSM_TESTS' sh run.sh 2>&1; cd .. && rm -rf $R" > $W/log
grep -v '^ok ' $W/log
pass=$(grep -c '^ok ' $W/log); fail=$(grep -c '^FAIL' $W/log)
grep -q '^== done ==' $W/log && grep -q '^exit 0' $W/log || { echo "FAIL the run did not finish (see $W/log)"; fail=$((fail + 1)); }
echo "hostfs: $pass passed, $fail failed"
[ "$fail" = 0 ]
