#!/bin/sh
# mkhostfs.sh MCC OUTDIR: a root file system for x86-64 Linux containers in which every program is built by
# MCC from this tree, against our own C library, statically: OUTDIR/rootfs, the same as OUTDIR/hostfs.tar
# (for `docker import`, or the `FROM scratch` Dockerfile written beside it). Nothing of a distribution is in it.
# MCC's directory must hold the tools (mkbox.sh: bin/, used here instead of the system's) and the x86-64
# Linux C library (mklxlibc.sh MCC DIR x86_64). What it also holds goes in when it is there: the third-party
# libraries (ARCH=x86_64 mkulibs.sh: linux-x64/ext), the CUDA headers and run time (mkcuda.sh: cuda/include,
# linux-x64/cuda/lib).
#
#   /usr/mcc, /usr/mbox          the compiler (make, lex, yacc, linker, binutils) and the shell + tools
#   /usr/bin                     every tool by name; /bin and /sbin are links to it
#   /usr/linux-x64               the C library: include, lib/crt1.o, lib/libc.a, ext/{include,lib}
#   /usr/cuda                    include: what a .cu file includes; lib64/libcudart.a: mcuda, the CPU run time
#   /etc/passwd, group, hosts    root alone; /tmp, /root, /proc, /sys, /dev: empty
# SOURCE_DATE_EPOCH: the time every member of the archive carries (so that the same tree gives the same archive).
set -e
MCC=$1; OUT=$2
[ -n "$MCC" ] && [ -n "$OUT" ] || { echo "usage: mkhostfs.sh MCC OUTDIR" >&2; exit 2; }
case $MCC in /*) ;; *) MCC=$PWD/$MCC;; esac
case $OUT in /*) ;; *) OUT=$PWD/$OUT;; esac
D=${MCC%/*}
[ -x $D/bin/sh ] && [ -x $D/bin/tar ] || { echo "mkhostfs: no tools in $D/bin (make tools)" >&2; exit 1; }
[ -f $D/linux-x64/lib/libc.a ] || { echo "mkhostfs: no x86-64 Linux C library in $D/linux-x64 (make x64)" >&2; exit 1; }
PATH=$D/bin; LC_ALL=C; export PATH LC_ALL # (LC_ALL: src/*.c in the order the image itself will see them)
R=$OUT/rootfs; T=$OUT/obj
CC="$MCC --target=x86_64-linux-gnu -mno-glibc"
EPOCH=${SOURCE_DATE_EPOCH:-1700000000}
rm -rf $R $T $OUT/hostfs.tar
mkdir -p $T/box $R/usr/bin $R/usr/linux-x64/lib $R/etc $R/tmp $R/root $R/proc $R/sys $R/dev

# the two programs
$CC -o $R/usr/mcc src/*.c
objs=
for f in box/*.c; do
    o=$T/box/$(basename ${f%.*}).o
    $CC -c $f -o $o
    objs="$objs $o"
done
$CC -o $R/usr/mbox $objs

# every tool by name. The applet list is the host mbox's: the same source.
for a in $($D/mbox --list); do
    ln -s ../mbox $R/usr/bin/$a
done
for a in mcc make gmake lex flex yacc bison cc; do
    ln -s ../mcc $R/usr/bin/$a
done
# the binutils and the compiler under the names a Makefile written for GNU tools calls
for a in gcc ld ar nm objcopy objdump strip nvcc; do
    ln -s ../mcc $R/usr/bin/x86_64-linux-gnu-$a
    printf '#!/bin/sh\nexec /usr/mcc elf %s "$@"\n' $a > $R/usr/bin/$a
    chmod 755 $R/usr/bin/$a
done
# pkg-config: every library there is lives under one prefix
cat > $R/usr/bin/pkg-config <<'XEOF'
#!/bin/sh
# pkg-config for the libraries under /usr/linux-x64/ext (the compiler looks there by itself)
X=/usr/linux-x64/ext
lib() { case $1 in libcurl) echo curl;; libzstd) echo zstd;; zlib) echo z;; libcrypto) echo crypto;; libjpeg) echo jpeg;; liblz4) echo lz4;; *) echo ${1#lib};; esac; }
mode=; out=; st=0
for a; do
    case $a in
    --cflags|--libs|--modversion|--exists) mode=$a;;
    --*) ;;
    *)  l=$(lib $a)
        [ -f $X/lib/lib$l.a ] || { echo "pkg-config: $a: not here" >&2; st=1; continue; }
        case $mode in
        --libs) out="$out -l$l";;
        --modversion) echo 1.0;;
        esac;;
    esac
done
case $mode in
--cflags) echo "-I$X/include";;
--libs) echo "-L$X/lib$out";;
esac
exit $st
XEOF
chmod 755 $R/usr/bin/pkg-config
ln -s usr/bin $R/bin
ln -s usr/bin $R/sbin

# the C library, the third-party libraries, the CUDA headers
cp -R $D/linux-x64/include $R/usr/linux-x64/include
cp $D/linux-x64/lib/crt1.o $D/linux-x64/lib/libc.a $R/usr/linux-x64/lib/
if [ -d $D/linux-x64/ext/lib ]; then
    mkdir -p $R/usr/linux-x64/ext/lib
    cp -R $D/linux-x64/ext/include $R/usr/linux-x64/ext/include
    cp $D/linux-x64/ext/lib/*.a $R/usr/linux-x64/ext/lib/
fi
if [ -d $D/cuda/include ]; then
    mkdir -p $R/usr/cuda
    cp -R $D/cuda/include $R/usr/cuda/include
fi
# mcuda, the CUDA run time on the CPU, as the libcudart.a that a Makefile written for NVIDIA's toolkit links
if [ -f $D/linux-x64/cuda/lib/libmcuda.a ]; then
    mkdir -p $R/usr/cuda/lib64
    cp $D/linux-x64/cuda/lib/libcudart.a $D/linux-x64/cuda/lib/libcublasLt.a $D/linux-x64/cuda/lib/libstdc++.a $R/usr/cuda/lib64/
fi

printf 'root:x:0:0:root:/root:/bin/sh\n' > $R/etc/passwd
printf 'root:x:0:\n' > $R/etc/group
printf '127.0.0.1 localhost\n' > $R/etc/hosts
chmod 1777 $R/tmp
chmod 700 $R/root

tar -cf $OUT/hostfs.tar --owner=0 --group=0 --mtime=@$EPOCH -C $R .
cat > $OUT/Dockerfile <<'XEOF'
# docker build -t mcc-hostfs .        (or, without this file: docker import hostfs.tar mcc-hostfs)
FROM scratch
ADD hostfs.tar /
ENV PATH=/usr/bin HOME=/root
WORKDIR /root
ENTRYPOINT ["/usr/bin/init"]
CMD ["/bin/sh"]
XEOF
rm -rf $T
echo "hostfs: $OUT/hostfs.tar ($(wc -c < $OUT/hostfs.tar) bytes, $(tar -tf $OUT/hostfs.tar | wc -l) members)"
