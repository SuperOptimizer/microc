#!/bin/sh
# devlib.sh DIR: every C library function mcuda_device.h declares (its extern "C" blocks) is there on the device. A kernel calls each
# once; each __mcuda_NAME its PTX calls must be a piece of DIR's device library (DIR/cudahw/lib64/libcudart.a), the rest are inline
# instructions. (11-27: the double sinpi / cospi were declared, used by sincospi, and missing: mcudahw refused the launch with "device
# code calls sinpi, which nothing defines", and ufsm's FFT twiddle table stayed unwritten.)
D=${1:?DIR}; T=build/test/devlib; mkdir -p $T; LC_ALL=C; export LC_ALL
H=cuda/devinc/mcuda_device.h; L=$D/cudahw/lib64/libcudart.a
[ -f $L ] || { echo "FAIL devlib: no $L (mkcuda.sh MCC DIR hw)"; exit 1; }
awk '/^extern "C" \{/ { f = 1; next } f && /^\}/ { f = 0 } f' $H | tr ';' '\n' | awk '
    BEGIN { print "__global__ void devlib_k(double *o, int *io, const double *in) {"; print "    double d = in[0], d2 = 0, acc = 0; float f = (float)in[1], f2 = 0; int i = io[0], i2 = 0;" }
    /\(/ { s = $0; sub(/^ +/, "", s); p = index(s, "("); head = substr(s, 1, p - 1); args = substr(s, p + 1); sub(/\).*/, "", args)
           n = split(head, w, " "); name = w[n]; ret = head; sub(" *" name "$", "", ret)
           na = split(args, a, ","); call = name "("
           for (k = 1; k <= na; k++) { t = a[k]; gsub(/^ +| +$/, "", t)
               v = t == "float" ? "f" : t == "double" ? "d" : t == "int" ? "i" : t == "long" ? "(long)i" : t == "long long" ? "(long long)i" :
                   t == "float *" ? "&f2" : t == "double *" ? "&d2" : t == "int *" ? "&i2" : t == "const char *" ? "\"\"" : "?" t "?"
               call = call (k > 1 ? ", " : "") v }
           call = call ")"; print (ret == "void" ? "    " call ";" : "    acc += (double)" call ";"); names = names " " name }
    END { print "    o[0] = acc + d2 + f2 + i2;"; print "}"; print "/* names:" names " */" }' > $T/all.cu
grep -q '?' $T/all.cu && { echo "FAIL devlib: a parameter type the generator does not know: $(grep -m1 '?' $T/all.cu)"; exit 1; }
CU="-isystem $D/cudahw/include -include $D/cudahw/include/mcuda_device.h"
$D/mcc -mglibc $CU --cuda-arch=1200a -O2 -c $T/all.cu -o $T/all.o --ptx-out=$T/all.ptx > $T/log 2>&1 || { echo "FAIL devlib: compile: $(head -3 $T/log)"; exit 1; }
strings -a $L | grep -o '__mcuda_[A-Za-z0-9_]*' | sort -u > $T/pieces
grep '^\.extern \.func' $T/all.ptx | grep -o '__mcuda_[A-Za-z0-9_]* *(' | tr -d ' (' | sort -u > $T/calls
miss=$(comm -23 $T/calls $T/pieces | tr '\n' ' ')
n=$(sed -n 's|^/\* names:||p' $T/all.cu | wc -w); nc=$(wc -l < $T/calls)
if [ -n "$miss" ]; then echo "FAIL devlib: device code calls what the device library does not define: $miss"; exit 1; fi
echo "devlib: $n declared functions called, $nc of them from the device library, none missing"
