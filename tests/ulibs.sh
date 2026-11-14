#!/bin/sh
# ulibs.sh DIR: test the libraries that mkulibs.sh installed in DIR/linux/ext. Each tests/ulibs/t_*.c is built
# with DIR/mcc for aarch64 Linux, and all of them run inside one boot of mvm (kernel linux/mb10, busybox
# linux/bb10) with the test directory shared; every check prints "ok NAME" or "FAIL NAME". The reference
# data in tests/ulibs/data was made on the Mac by tests/ulibs/gen.py. Prints "ulibs: N pass".
D=$(cd "$1" && pwd) || exit 2
M=$(pwd); S=$M/tests/ulibs; W=$M/build/test/ulibs/run; X=$D/linux/ext
PATH=$D/bin; export PATH
rm -rf $W; mkdir -p $W/out $W/www
cp -R $S/data $W/data
libs="-lcurl -lblosc -lzstd -ljpeg -lcrypto -lz"
tests=
for t in $S/t_*.c; do
    n=$(basename $t .c)
    if $D/mcc --target=aarch64-linux-gnu -D_GNU_SOURCE -I$X/include -I$S -o $W/$n $t -L$X/lib $libs 2>$W/$n.err; then
        tests="$tests $n"
    else
        echo "FAIL build $n (see $W/$n.err)"; echo "FAIL build $n" >> $W/build.fail
    fi
done
$D/mcc --target=aarch64-linux-gnu -I$X/include -o $W/tls_server $S/tls_server.c -L$X/lib -lbearssl 2>$W/tls_server.err ||
    { echo "FAIL build tls_server (see $W/tls_server.err)"; echo "FAIL build tls_server" >> $W/build.fail; }
{
    echo "export TESTS='$tests' NOW=$(date +%s)"
    cat $S/guest.sh
} > $W/run.sh
cat > $W/init <<'INIT'
#!/bin/busybox sh
/bin/busybox --install -s /bin
export PATH=/bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
echo 0 > /proc/sys/kernel/printk
mkdir /mnt; mount -t 9p -o trans=virtio,version=9p2000.L,msize=262144 host /mnt || echo "FAIL mount"
ifconfig lo 127.0.0.1 up; ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up && route add default gw 10.0.2.2
echo "nameserver 10.0.2.3" > /etc/resolv.conf
echo "== start =="
cd /mnt && sh ./run.sh
echo "== done =="
poweroff -f
INIT
ROOTNAME=rootulibs KERNEL=mb10 BUSYBOX=bb10 ACCEL=${ACCEL:-hvf} CONSOLE=hvc0 MCCDIR=$D \
    MVMFLAGS="-smp 2 -net -rng -vcon -share $W" \
    sh $M/linux/tools/boot7.sh $W/init ${SECS:-300} 2>&1 | tr -d '\r' > $W/log
sed -n '/^== start ==/,/^== done ==/p' $W/log > $W/result
pass=$(grep -c '^ok ' $W/result); fail=$(grep -c '^FAIL' $W/result)
[ -f $W/build.fail ] && fail=$((fail + $(wc -l < $W/build.fail)))
grep '^FAIL' $W/result
if ! grep -q '^== done ==' $W/result; then echo "FAIL the guest did not finish (see $W/log)"; fail=$((fail + 1)); fi
for n in $tests; do grep -q "^exit $n 0\$" $W/result || { echo "FAIL $n did not exit with 0"; fail=$((fail + 1)); }; done
if [ $fail = 0 ]; then echo "ulibs: $pass pass"; else echo "ulibs: $pass pass, $fail FAILED (see $W/result)"; exit 1; fi
