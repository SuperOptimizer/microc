# smoke.sh: run inside the root file system of mkhostfs.sh, as process 1's command (tests/hostfs.sh does that),
# in a writable directory that holds src.tar (mcc's sources). Each check prints "ok NAME" or "FAIL NAME".
ok() { echo "ok $1"; }
bad() { echo "FAIL $1"; }
check() { n=$1; shift; if "$@" >$n.log 2>&1; then ok $n; else bad $n; sed 's/^/    /' $n.log | head -5; fi; }
is() { if [ "$2" = "$3" ]; then ok $1; else bad "$1: got '$2', want '$3'"; fi; }

# the shell is the child of init, which is process 1
is init-pid1 "$(tr '\0' ' ' < /proc/1/cmdline | cut -d' ' -f1)" /usr/bin/init
is uid "$(id -u)" 0
is whoami "$(whoami)" root
is uname "$(uname -sm)" "Linux x86_64"

# every name in /usr/bin leads somewhere
n=0; broken=
for f in /usr/bin/*; do
    n=$((n + 1))
    [ -x $f ] || broken="$broken $f"
done
is bin-links "$broken" ""
[ $n -gt 70 ] && ok bin-count || bad "bin-count: $n"

# the tools
mkdir -p t/sub && cd t
seq 1 1000 > nums
is seq-wc "$(wc -l < nums | tr -d ' ')" 1000
is awk-sum "$(awk '{s += $1} END {print s}' nums)" 500500
is sed-grep "$(sed -n 's/^9\(9\)9$/x\1/p' nums | grep -c x9)" 1
is sort-uniq "$(printf 'b\na\nb\n' | sort | uniq -c | tr -s ' ' | tr '\n' ,)" " 1 a, 2 b,"
is find "$(find . -name nums -type f | wc -l | tr -d ' ')" 1
is sha1 "$(printf abc | sha1sum | cut -d' ' -f1)" a9993e364706816aba3e25717850c26c9cd0d89d
is sha256 "$(printf abc | sha256sum | cut -d' ' -f1)" ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
gzip -c nums > nums.gz && gunzip -c nums.gz | cmp -s - nums && ok gzip || bad gzip
ln -s ../nums sub/link && cp nums sub/copy && chmod 640 sub/copy
tar -cf a.tar nums sub && mkdir x && tar -xf a.tar -C x && cmp -s x/sub/copy nums && [ "$(readlink x/sub/link)" = ../nums ] &&
    [ "$(tar -tf a.tar | wc -l | tr -d ' ')" = 4 ] && ok tar || bad tar
is expr-bc "$(expr 6 \* 7) $(echo '2^10' | bc)" "42 1024"
cd ..

# the compiler: a program, one with threads and the math library, the binutils and make
cat > hello.c <<'EOF'
#include <stdio.h>
int main(int argc, char **argv) { printf("hello from %s, %d args\n", argv[0], argc - 1); return 0; }
EOF
check cc-hello cc -o hello hello.c
is run-hello "$(./hello a b)" "hello from ./hello, 2 args"
check gcc-c23 gcc -std=c23 -O3 -march=native -g -Wall -o hello23 hello.c
is run-hello23 "$(./hello23)" "hello from ./hello23, 0 args"
# by the names in /bin too (a link to usr/bin: the compiler still finds its headers and library)
check bin-mcc /bin/mcc -o hellob hello.c
check bin-cc /bin/cc -o hellob hello.c
is run-hellob "$(./hellob)" "hello from ./hellob, 0 args"
# dlopen: a real loader in a static program; a library that is not there fails with a reason
cat > dl.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    void *h = dlopen("libnothere.so.9", RTLD_NOW), *self = dlopen("libc.so.6", RTLD_NOW);
    const char *e = dlerror();
    size_t (*len)(const char *) = self ? (size_t (*)(const char *))dlsym(self, "strlen") : 0;
    printf("%s %s %d\n", h ? "opened" : "refused", e && strstr(e, "libnothere.so.9") ? "named" : "silent", len ? (int)len("four") : -1);
    return 0;
}
EOF
check cc-dl cc -o dl dl.c
is run-dl "$(./dl)" "refused named 4"
cat > thr.c <<'EOF'
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
static _Atomic long sum;
static void *work(void *p) { long i, n = (long)p; for (i = 1; i <= n; i++) sum += i; return 0; }
int main(void)
{
    pthread_t t[8];
    int i;
    for (i = 0; i < 8; i++) pthread_create(&t[i], 0, work, (void *)100000L);
    for (i = 0; i < 8; i++) pthread_join(t[i], 0);
    printf("%ld %.6f\n", (long)sum, sqrt(2.0) * exp(1.0));
    return 0;
}
EOF
check cc-threads cc -O2 -pthread -o thr thr.c -lm -lpthread
is run-threads "$(./thr)" "40000400000 3.844231"
mkdir mk && cat > mk/Makefile <<'EOF'
OBJ = a.o b.o
all: prog
%.o: %.c
	$(CC) -c $< -o $@
lib.a: $(OBJ)
	ar rcs $@ $^
both.o: $(OBJ)
	ld -r -o $@ $^
prog: main.c lib.a both.o
	$(CC) -o $@ main.c lib.a
	nm both.o | grep -c ' T ' > syms
EOF
echo 'int a(void) { return 40; }' > mk/a.c
echo 'int b(void) { return 2; }' > mk/b.c
echo 'int a(void); int b(void); int main(void) { return a() + b(); }' > mk/main.c
check make make -C mk -j4
(cd mk && ./prog; is make-prog "$? $(cat syms)" "42 2")

# the libraries ufsm links, through pkg-config as its Makefile asks for them
if [ -d /usr/linux-x64/ext/lib ]; then
    cat > libs.c <<'EOF'
#include <blosc.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <zstd.h>
int main(void)
{
    static char in[100000], z[120000], out[100000];
    unsigned long zn = sizeof z, on = sizeof out;
    size_t n;
    int i, good = 0;
    for (i = 0; i < (int)sizeof in; i++) in[i] = (char)(i * i >> 3);
    compress((unsigned char *)z, &zn, (unsigned char *)in, sizeof in);
    uncompress((unsigned char *)out, &on, (unsigned char *)z, zn);
    good += on == sizeof in && !memcmp(in, out, sizeof in);
    n = ZSTD_compress(z, sizeof z, in, sizeof in, 3);
    good += ZSTD_decompress(out, sizeof out, z, n) == sizeof in && !memcmp(in, out, sizeof in);
    blosc_init();
    i = blosc_compress(5, 1, 4, sizeof in, in, z, sizeof z);
    good += blosc_decompress(z, out, sizeof out) == (int)sizeof in && !memcmp(in, out, sizeof in) && i > 0;
    good += curl_version_info(CURLVERSION_NOW)->version_num >= 0x080000;
    good += EVP_sha256() != 0;
    { struct jpeg_error_mgr e; good += jpeg_std_error(&e) == &e; }
    printf("libs %d\n", good);
    return 0;
}
EOF
    check cc-libs cc -D_GNU_SOURCE $(pkg-config --cflags libcurl libzstd blosc zlib libcrypto) -o libs libs.c \
        $(pkg-config --libs libcurl libzstd blosc zlib libcrypto) -ljpeg -lm -lpthread
    is run-libs "$(./libs)" "libs 6"
fi

# a CUDA unit compiles (device code to PTX inside the object)
if [ -d /usr/cuda/include ]; then
    cat > k.cu <<'EOF'
__global__ void scale(float *p, int n, float f) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) p[i] *= f; }
extern "C" void run(float *p, int n) { scale<<<(n + 255) / 256, 256>>>(p, n, 2.0f); }
EOF
    check nvcc nvcc -O3 -arch=sm_86 -c k.cu -o k.o
    nm k.o | grep -q ' T ' && ok nvcc-object || bad nvcc-object
    # and runs, on the CPU run time (mcuda), linked as a Makefile written for NVIDIA's toolkit links
    if [ -f /usr/cuda/lib64/libcudart.a ]; then
        cat > kmain.c <<'EOF'
#include <cuda_runtime.h>
#include <stdio.h>
void run(float *p, int n);
int main(void)
{
    static float h[1000];
    float *d;
    int i, good = 0;
    for (i = 0; i < 1000; i++) h[i] = (float)i;
    if (cudaMalloc((void **)&d, sizeof h) != cudaSuccess) return 1;
    cudaMemcpy(d, h, sizeof h, cudaMemcpyHostToDevice);
    run(d, 1000);
    if (cudaDeviceSynchronize() != cudaSuccess) return 2;
    cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
    for (i = 0; i < 1000; i++) good += h[i] == (float)(2 * i);
    printf("kernel %d\n", good);
    return 0;
}
EOF
        check cuda-link gcc -I/usr/cuda/include -o kmain kmain.c k.o -L/usr/cuda/lib64 -lcudart -lcublasLt -lstdc++ -lm -lpthread
        is cuda-run "$(./kmain)" "kernel 1000"
    fi
fi

# the compiler builds itself here, to the bytes of the one that is installed
if [ -f src.tar ]; then
    tar -xf src.tar
    check self-build mcc -o mcc2 src/*.c
    cmp -s mcc2 /usr/mcc && ok self-identical || bad self-identical
fi
echo "== done =="
