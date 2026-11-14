#!/bin/sh
# mkulibs.sh MCC DIR: build the third-party libraries that ufsm links (zlib, zstd, lz4 + c-blosc, libjpeg-turbo,
# libcurl with BearSSL, a small libcrypto, the xxhash header) with MCC for aarch64 Linux and install them in
# DIR/linux/ext/include and DIR/linux/ext/lib. The sources are the release tarballs listed in
# libc/ext/SOURCES.txt, unpacked under build/ext by libc/ext/fetch.sh (EXT=dir names another place); nothing of theirs is run:
# the config headers are written by hand in libc/ext and the file lists are below. Only MCC and the tools in
# DIR/bin are used. zstd, blosc and curl's resolver use pthreads; THREADS=0 builds them without (one thread, locks compiled away).
# LIBS="z zstd ..." builds a subset. ARCH=x86_64 builds for x86-64 Linux instead, into DIR/linux-x64/ext.
set -e
MCC=$1; D=$2
[ -n "$MCC" ] && [ -d "$D/bin" ] || { echo "usage: mkulibs.sh MCC DIR" >&2; exit 2; }
case $MCC in /*) ;; *) MCC=$PWD/$MCC;; esac
case $D in /*) ;; *) D=$PWD/$D;; esac
PATH=$D/bin; export PATH
E=${EXT:-$PWD/build/ext}; P=$PWD/libc/ext
X=$D/linux/ext; O=$X/obj; I=$X/include; L=$X/lib
CC="$MCC --target=aarch64-linux-gnu"
if [ "$ARCH" = x86_64 ]; then # for x86-64 Linux, into DIR/linux-x64/ext
    X=$D/linux-x64/ext; O=$X/obj; I=$X/include; L=$X/lib
    CC="$MCC --target=x86_64-linux-gnu"
fi
if [ -n "$GLIBC" ]; then # for x86-64 Linux with the GNU C library whose headers are under $GLIBC/usr/include, into DIR/glibc/ext
    X=$D/glibc/ext; O=$X/obj; I=$X/include; L=$X/lib
    CC="$MCC --target=x86_64-linux-gnu --sysroot=$GLIBC"
fi
THREADS=${THREADS:-1}
LIBS=${LIBS:-z zstd lz4 blosc jpeg crypto xxhash bearssl curl}
mkdir -p $O $I $L

# cc_all NAME "FLAGS" DIR FILE...: compile DIR/FILE.c into $O/NAME/, collecting the objects in $objs
cc_all() {
    n=$1; fl=$2; dir=$3; shift 3
    mkdir -p $O/$n
    for f; do
        o=$O/$n/$(basename $f).o
        $CC $fl -c $dir/$f.c -o $o || { echo "mkulibs: $n: $f.c failed" >&2; exit 1; }
        objs="$objs $o"
    done
}
mklib() { rm -f $L/$1; if [ -n "$GLIBC" ]; then $MCC elf ar rc $L/$1 $objs; else $MCC ar $L/$1 $objs; fi; } # GNU ld wants GNU ar's format
want() { case " $LIBS " in *" $1 "*) return 0;; esac; return 1; }

if want z; then
    Z=$E/zlib-1.4; objs=
    cc_all z "-DHAVE_UNISTD_H -DHAVE_STDARG_H -DHAVE_VSNPRINTF -D_LARGEFILE64_SOURCE=1" $Z \
        adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil
    mklib libz.a
    cp $Z/zlib.h $Z/zconf.h $I/
fi

if want zstd; then
    Z=$E/zstd-1.5.7/lib; objs=
    fl="-DZSTD_DISABLE_ASM -DZSTD_LEGACY_SUPPORT=0 -DXXH_NAMESPACE=ZSTD_ -DDYNAMIC_BMI2=0 -I$Z -I$Z/common"
    [ $THREADS = 1 ] && fl="$fl -DZSTD_MULTITHREAD"
    cc_all zstd "$fl" $Z/common debug entropy_common error_private fse_decompress pool threading xxhash zstd_common
    cc_all zstd "$fl" $Z/compress fse_compress hist huf_compress zstd_compress_literals \
        zstd_compress_sequences zstd_compress_superblock zstd_double_fast zstd_fast zstd_ldm zstd_opt \
        zstd_preSplit zstdmt_compress
    # without threads, a request for workers (ZSTD_c_nbWorkers > 0, as ufsm makes) is taken and ignored, not refused
    mkdir -p $O/zstd/src
    if [ $THREADS = 1 ]; then cp $Z/compress/zstd_compress.c $O/zstd/src/
    else sed '/case ZSTD_c_nbWorkers :/,/return 0;/s/RETURN_ERROR_IF(value!=0, parameter_unsupported.*/(void)value;/' \
            $Z/compress/zstd_compress.c > $O/zstd/src/zstd_compress.c
    fi
    cc_all zstd "$fl -I$Z/compress" $O/zstd/src zstd_compress
    cc_all zstd "$fl" $Z/compress zstd_lazy
    cc_all zstd "$fl" $Z/decompress huf_decompress zstd_ddict zstd_decompress zstd_decompress_block
    mklib libzstd.a
    cp $Z/zstd.h $Z/zstd_errors.h $Z/zdict.h $I/
fi

if want lz4; then
    Z=$E/c-blosc-1.21.6/internal-complibs/lz4-1.9.4; objs=
    cc_all lz4 "" $Z lz4 lz4hc
    mklib liblz4.a
    cp $Z/lz4.h $Z/lz4hc.h $I/
fi

if want blosc; then
    Z=$E/c-blosc-1.21.6/blosc; objs=
    fl="-DHAVE_LZ4 -DHAVE_ZLIB -DHAVE_ZSTD -I$I -I$Z"
    cc_all blosc "$fl" $Z blosclz fastcopy shuffle shuffle-generic bitshuffle-generic
    mkdir -p $O/blosc/src
    if [ $THREADS = 1 ]; then
        cp $Z/blosc.c $O/blosc/src/
    else
        # no threads: locks compile to nothing (libc/ext/nothreads/pthread.h) and a call that asks for
        # several threads is served by the serial code
        fl="$fl -I$P/nothreads"
        sed 's/if (context->numthreads == 1 || (context->sourcesize/if (1 || (context->sourcesize/' $Z/blosc.c > $O/blosc/src/blosc.c
        grep -q 'if (1 || (context->sourcesize' $O/blosc/src/blosc.c || { echo "mkulibs: blosc: blosc.c patch did not apply" >&2; exit 1; }
    fi
    cc_all blosc "$fl" $O/blosc/src blosc
    # lz4 rides in libblosc.a so that -lblosc -lzstd -lz is all a program needs
    objs="$objs $O/lz4/lz4.o $O/lz4/lz4hc.o"
    mklib libblosc.a
    cp $Z/blosc.h $Z/blosc-export.h $I/
fi

if want jpeg; then
    Z=$E/libjpeg-turbo-2.1.5.1; objs=
    cc_all jpeg "-I$P/jpeg -I$Z" $Z jcapimin jcapistd jccoefct jccolor jcdctmgr jchuff jcicc jcinit jcmainct jcmarker \
        jcmaster jcomapi jcparam jcphuff jcprepct jcsample jctrans jdapimin jdapistd jdatadst jdatasrc jdcoefct \
        jdcolor jddctmgr jdhuff jdicc jdinput jdmainct jdmarker jdmaster jdmerge jdphuff jdpostct jdsample jdtrans \
        jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst jidctint jidctred jquant1 jquant2 jutils jmemmgr \
        jmemnobs jaricom jcarith jdarith jsimd_none
    mklib libjpeg.a
    cp $Z/jpeglib.h $Z/jmorecfg.h $Z/jerror.h $P/jpeg/jconfig.h $I/
fi

if want crypto; then
    objs=
    cc_all crypto "-I$P/crypto" $P/crypto evp
    mklib libcrypto.a
    mkdir -p $I/openssl
    cp $P/crypto/openssl/evp.h $I/openssl/
fi

if want bearssl; then
    # BearSSL 0.6, curl's TLS: every file of src/ (the x86 and POWER ones compile to nothing here)
    Z=$E/bearssl-0.6; objs=
    brx=; [ "$ARCH" = x86_64 ] && brx="-DBR_i386=0 -DBR_amd64=0" # the code for x86 is all intrinsics, which this compiler has not
    for d in $Z/src $Z/src/*; do
        [ -d $d ] || continue
        set --
        for f in $d/*.c; do [ -f $f ] && set -- "$@" $(basename $f .c); done
        [ $# = 0 ] || cc_all bearssl "-I$Z/inc -I$Z/src -DBR_USE_URANDOM=1 -DBR_USE_UNIX_TIME=1 $brx" $d "$@"
    done
    mklib libbearssl.a
    cp $Z/inc/*.h $I/
fi

if want curl; then
    # libcurl 8.11.0 with libc/ext/curl/curl_config.h: HTTP/1.1 and HTTPS through BearSSL. Every file of
    # lib/ is compiled; what the config turns off compiles to nothing.
    Z=$E/curl-8.11.0; objs=
    fl="-D_GNU_SOURCE -DHAVE_CONFIG_H -DBUILDING_LIBCURL -DCURL_STATICLIB -I$P/curl -I$Z/include -I$Z/lib -I$I"
    [ $THREADS = 1 ] && fl="$fl -DULIBS_THREADS"
    # there is no HTTP/2 here: a request for it (CURL_HTTP_VERSION_2TLS, as ufsm makes) is taken as "no preference"
    mkdir -p $O/curl/src
    sed '/case CURLOPT_HTTP_VERSION:/,/data->set.httpwant/s|/\* not accepted \*/|if(arg == CURL_HTTP_VERSION_2_0 \|\| arg == CURL_HTTP_VERSION_2TLS \|\| arg == CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE) { arg = CURL_HTTP_VERSION_NONE; break; }|' \
        $Z/lib/setopt.c > $O/curl/src/setopt.c
    grep -q 'arg = CURL_HTTP_VERSION_NONE; break;' $O/curl/src/setopt.c || { echo "mkulibs: curl: setopt.c patch did not apply" >&2; exit 1; }
    for d in $Z/lib $Z/lib/vauth $Z/lib/vtls $Z/lib/vquic $Z/lib/vssh; do
        set --
        for f in $d/*.c; do [ $f = $Z/lib/setopt.c ] || set -- "$@" $(basename $f .c); done
        cc_all curl "$fl" $d "$@"
    done
    cc_all curl "$fl" $O/curl/src setopt
    # libcurl.a carries its TLS: BearSSL's objects go in too, as br_NAME.o (both have an md5.o and an hmac.o),
    # so that -lcurl alone links. libbearssl.a stays for programs that use BearSSL themselves.
    ls $O/bearssl/*.o >/dev/null 2>&1 || { echo "mkulibs: curl: build bearssl first (LIBS=\"bearssl curl\")" >&2; exit 1; }
    rm -rf $O/curl_tls; mkdir -p $O/curl_tls
    for f in $O/bearssl/*.o; do
        cp $f $O/curl_tls/br_$(basename $f)
        objs="$objs $O/curl_tls/br_$(basename $f)"
    done
    mklib libcurl.a
    mkdir -p $I/curl
    cp $Z/include/curl/*.h $I/curl/
fi

if want xxhash; then
    cp $E/xxHash-0.8.3/xxhash.h $I/
fi
