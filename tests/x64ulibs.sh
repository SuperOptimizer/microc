#!/bin/sh
# x64ulibs.sh DIR: test the libraries that `ARCH=x86_64 mkulibs.sh` installed in DIR/linux-x64/ext: the
# programs of tests/ulibs.sh (tests/ulibs/t_*.c), built for x86-64 Linux and run on an x86-64 machine
# instead of in mvm. The servers they talk to listen on 127.0.0.1 ports 18080 (python3's http.server in
# place of busybox httpd), 18081 and 18443 there: the sources name 8080, 8081 and 8443, which a shared
# machine may have in use, so t_curl.c is compiled from a copy with the ports changed.
#   X64_SSH, X64_DIR as in tests/x64.sh
#   GLIBC=DIR: the libraries of `GLIBC=DIR mkulibs.sh` (DIR/glibc/ext, against the GNU C library whose headers
#   are under DIR/usr/include) instead; the programs are linked on the machine by its gcc
D=$1
S=tests/ulibs; W=build/x64ulibs; X=$D/linux-x64/ext
R=${X64_DIR:-/root/x64}/ulibs
CC="$D/mcc --target=x86_64-linux-gnu"
link=
if [ -n "$GLIBC" ]; then X=$D/glibc/ext; CC="$CC --sysroot=$GLIBC -c"; fi
if [ -z "$X64_SSH" ] || [ "$X64_SSH" = local ]; then
    echo "tests/x64ulibs.sh: set X64_SSH (how to reach an x86-64 Linux machine)"; exit 1
fi
rm -rf $W; mkdir -p $W/run/out $W/src
cp -R $S/data $W/run/data
libs="-lcurl -lblosc -lzstd -ljpeg -lcrypto -lz"
if [ -n "$GLIBC" ]; then
    mkdir -p $W/run/lib; cp $X/lib/*.a $W/run/lib/
    link='for o in *.o; do gcc -no-pie -o ${o%.o} $o -Llib -lcurl -lbearssl -lblosc -lzstd -ljpeg -lcrypto -lz -lm -lpthread || echo "FAIL link $o"; done'
fi
tests=; fail=0
obj=; ldflags="-L$X/lib $libs"; ldflags2="-L$X/lib -lbearssl"
if [ -n "$GLIBC" ]; then obj=.o; ldflags=; ldflags2=; fi
for t in $S/t_*.c; do
    n=$(basename $t .c)
    [ -n "$GLIBC" ] && [ $n = t_libc ] && continue # that one asks things of mcc's own C library
    sed -e 's/:8080/:18080/g' -e 's/:8081/:18081/g' -e 's/:8443/:18443/g' $t > $W/src/$n.c
    if $CC -D_GNU_SOURCE -I$X/include -I$S -o $W/run/$n$obj $W/src/$n.c $ldflags 2>$W/$n.err; then
        tests="$tests $n"
    else
        echo "FAIL build $n (see $W/$n.err)"; fail=$((fail + 1))
    fi
done
$CC -I$X/include -o $W/run/tls_server$obj $S/tls_server.c $ldflags2 2>$W/tls_server.err ||
    { echo "FAIL build tls_server (see $W/tls_server.err)"; fail=$((fail + 1)); }
cat > $W/run/run.sh <<XEOF
$link
mkdir -p www/dir; cp data/plain.bin www/; echo index > www/dir/index.html
python3 -m http.server 18080 --bind 127.0.0.1 -d www >/dev/null 2>&1 &
PY=\$!
./tls_server 18081 - - www
./tls_server 18443 data/tls-key.der data/tls-cert.der www
sleep 1
echo "== start =="
for t in $tests; do
    timeout 300 ./\$t
    echo "exit \$t \$?"
done
echo "== done =="
kill \$PY; pkill -x tls_server
XEOF
(cd $W/run && tar cf - .) | $X64_SSH "rm -rf $R && mkdir -p $R && cd $R && tar xf - 2>/dev/null && sh run.sh 2>&1" > $W/log
sed -n '/^== start ==/,/^== done ==/p' $W/log > $W/result
pass=$(grep -c '^ok ' $W/result); fail=$((fail + $(grep -c '^FAIL' $W/result)))
grep '^FAIL' $W/result
if ! grep -q '^== done ==' $W/result; then echo "FAIL the run did not finish (see $W/log)"; fail=$((fail + 1)); fi
for n in $tests; do grep -q "^exit $n 0\$" $W/result || { echo "FAIL $n did not exit with 0"; fail=$((fail + 1)); }; done
if [ $fail = 0 ]; then echo "ulibs (x86-64): $pass pass"; else echo "ulibs (x86-64): $pass pass, $fail FAILED (see $W/result)"; exit 1; fi
