#!/bin/sh
# sinpi.sh DIR: the C library's sinpi / cospi (libc/src/mathx.c, also the device library's: mcuda_device.h's sincospi) against a long
# double reference (tests/sinpi/p.c -DREF, host cc): at most 1 ulp apart, and exactly equal on the special values (integers, halves,
# quarters, signed zeros).
D=${1:?DIR}; T=build/test/sinpi; mkdir -p $T
$D/mcc --target=x86_64-linux-gnu -mno-glibc -O2 -o $T/mcc tests/sinpi/p.c || { echo "FAIL sinpi: mcc build"; exit 1; }
cc -O2 -DREF -o $T/ref tests/sinpi/p.c -lm || { echo "FAIL sinpi: host reference build"; exit 1; }
$T/mcc > $T/mcc.out && $T/ref > $T/ref.out || { echo "FAIL sinpi: run"; exit 1; }
cat > $T/cmp.c <<'XEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long long key(double d) { long long i; memcpy(&i, &d, 8); return i < 0 ? (long long)0x8000000000000000ULL - i : i; }
int main(int argc, char **argv) {
    FILE *a = fopen(argv[1], "r"), *b = fopen(argv[2], "r"); char la[256], lb[256]; long n = 0, bad = 0, special = 28; long long worst = 0;
    while (fgets(la, sizeof la, a) && fgets(lb, sizeof lb, b)) {
        double x, s1, c1, x2, s2, c2; long long d1, d2;
        sscanf(la, "%la %la %la", &x, &s1, &c1); sscanf(lb, "%la %la %la", &x2, &s2, &c2); n++;
        d1 = llabs(key(s1) - key(s2)); d2 = llabs(key(c1) - key(c2)); if (d1 > worst) worst = d1; if (d2 > worst) worst = d2;
        if ((n <= special && (d1 || d2)) || d1 > 1 || d2 > 1) { bad++; if (bad < 10) printf("x %a: sinpi %a vs %a, cospi %a vs %a\n", x, s1, s2, c1, c2); }
    }
    printf("sinpi: %ld arguments, worst %lld ulp, %ld bad\n", n, worst, bad); return bad != 0 || n < 60000;
}
XEOF
cc -O2 -o $T/cmp $T/cmp.c && $T/cmp $T/mcc.out $T/ref.out
