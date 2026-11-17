#!/bin/sh
# mkssh.sh MCC DIR: build Dropbear (an ssh server and client that are not ours: matt.ucc.asn.au/dropbear, MIT
# licence) from its source with MCC for x86-64 Linux against mcc's own C library, statically, as one program:
# DIR/linux-x64/ssh/dropbearmulti, which is dropbear (the server), dbclient (the client), dropbearkey and scp by
# the name it is called under. mkhostfs.sh puts it into the image when it is there.
#   EXT=dir   where the unpacked release is: EXT/dropbear-$VERSION (default: build/ext)
# The release is fetched and checked by hand, once (README: "ssh"); nothing is downloaded here. Its configure
# script and makefiles are not run: libc/ext/dropbear/config.h says what configure would find, localoptions.h
# what the image wants (keys only, no passwords), and the lists of sources are read here from its Makefile.in
# files, as text. The tools of DIR/bin are used.
set -e
MCC=$1; D=$2
VERSION=${DROPBEAR_VERSION:-2026.90}
E=${EXT:-build/ext}
Z=$E/dropbear-$VERSION
[ -n "$MCC" ] && [ -n "$D" ] || { echo "usage: mkssh.sh MCC DIR" >&2; exit 2; }
[ -f $Z/src/dbmulti.c ] || { echo "mkssh: no $Z (the unpacked release of Dropbear $VERSION)" >&2; exit 1; }
PATH=$D/bin; LC_ALL=C; export PATH LC_ALL
P=libc/ext/dropbear
O=$D/linux-x64/obj/ssh
rm -rf $O $D/linux-x64/ssh
mkdir -p $O/inc $O/ltc $O/ltm $O/db $D/linux-x64/ssh
CC="$MCC --target=x86_64-linux-gnu -mno-glibc -O2"
cp $P/config.h $P/localoptions.h $O/inc/
# default_options.h with every #define inside an #ifndef, so that localoptions.h comes first (what the
# package's ifndef_wrapper.sh does)
sed 's/^\( *#define \([^ ][^ ]*\) .*\)/#ifndef \2\
\1\
#endif/' $Z/src/default_options.h > $O/inc/default_options_guard.h
inc="-I$O/inc -I$Z/src -I$Z/libtomcrypt/src/headers -I$Z/libtommath"
def="-DLOCALOPTIONS_H_EXISTS -DMP_32BIT -DDROPBEAR_SERVER -DDROPBEAR_CLIENT -DDROPBEAR_MULTI -DDBMULTI_dropbear -DDBMULTI_dbclient -DDBMULTI_dropbearkey -DDBMULTI_scp"
objs=
# libtomcrypt: the objects its Makefile.in names (OBJECTS= to the first empty line); aes_enc.o is aes.c again
for o in $(sed -n '/^OBJECTS=/,/^$/p' $Z/libtomcrypt/Makefile.in | tr -d '\\' | sed 's/^OBJECTS=//'); do
    n=$(echo $o | tr / _)
    case $o in
    src/ciphers/aes/aes_enc.o) $CC $inc $def -DLTC_SOURCE -DENCRYPT_ONLY -c $Z/libtomcrypt/src/ciphers/aes/aes.c -o $O/ltc/$n;;
    *) $CC $inc $def -DLTC_SOURCE -c $Z/libtomcrypt/${o%.o}.c -o $O/ltc/$n;;
    esac
    objs="$objs $O/ltc/$n"
done
for f in $Z/libtommath/bn_*.c; do
    o=$O/ltm/$(basename ${f%.c}).o
    $CC $inc $def -c $f -o $o
    objs="$objs $o"
done
# (MP_32BIT: libtommath on 32-bit digits; its 64-bit ones want a 128-bit integer type, which mcc has not)
# Dropbear itself: what its Makefile.in lists for the four programs, and dbmulti.c, which chooses by name
for n in dbutil buffer dbhelpers dss bignum signkey rsa dbrandom queue atomicio compat fake-rfc2553 ltc_prng ecc ecdsa sk-ecdsa \
    crypto_desc curve25519 ed25519 sk-ed25519 dbmalloc gensignkey gendss genrsa gened25519 \
    common-session packet common-algo common-kex common-channel common-chansession termcodes loginrec tcp-accept listener \
    process-packet dh_groups common-runopts circbuffer list netio chachapoly gcm kex-x25519 kex-dh kex-ecdh kex-pqhybrid \
    sntrup761 mlkem768 \
    svr-kex svr-auth sshpty svr-authpasswd svr-authpubkey svr-authpubkeyoptions svr-session svr-service svr-chansession \
    svr-runopts svr-agentfwd svr-main svr-x11fwd svr-forward svr-tcpfwd svr-streamfwd svr-authpam \
    cli-main cli-auth cli-authpasswd cli-kex cli-session cli-runopts cli-chansession cli-authpubkey cli-tcpfwd cli-channel \
    cli-authinteract cli-agentfwd cli-readconf \
    dropbearkey scp progressmeter scpmisc dbmulti; do
    $CC $inc $def -c $Z/src/$n.c -o $O/db/$n.o
    objs="$objs $O/db/$n.o"
done
$CC -o $D/linux-x64/ssh/dropbearmulti $objs
echo "ssh: $D/linux-x64/ssh/dropbearmulti (Dropbear $VERSION, $(wc -c < $D/linux-x64/ssh/dropbearmulti) bytes)"
