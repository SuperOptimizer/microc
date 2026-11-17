# gpu.sh: run inside the root file system when tests/hostfs.sh was given GPU=1, on a machine with NVIDIA's
# driver: its libcuda.so.1 and libnvidia-ptxjitcompiler.so.1 and the device nodes are bound in, as NVIDIA's
# container run time does it. Nothing else of the machine is there: no glibc, no ld.so. Prints ok / FAIL lines.
#   - a C program opens the driver with the C library's dlopen and asks it for its devices;
#   - a CUDA program built here runs its kernel on the GPU (libcudart.a chooses it), and on the CPU when told;
#   - cublasLtMatmul by our kernel gives, bit for bit, what the CPU run time gives (ltcmp.cu);
#   - with UFSM_GPU_TESTS="test_nn ..." and ufsm unpacked by ufsm.sh: those tests of ufsm, timed.
mkdir -p /work/gpu && cd /work/gpu || exit 1
ok() { echo "ok $1"; }
bad() { echo "FAIL $1"; }
cat > cuinit.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    void *lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    int (*init)(unsigned), (*count)(int *), (*get)(int *, int), (*name)(char *, int, int);
    int (*mem)(size_t *, int);
    int r, n = 0, i;
    if (!lib) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    init = (int (*)(unsigned))dlsym(lib, "cuInit");
    count = (int (*)(int *))dlsym(lib, "cuDeviceGetCount");
    get = (int (*)(int *, int))dlsym(lib, "cuDeviceGet");
    name = (int (*)(char *, int, int))dlsym(lib, "cuDeviceGetName");
    mem = (int (*)(size_t *, int))dlsym(lib, "cuDeviceTotalMem_v2");
    if (!init || !count || !get || !name || !mem) {
        printf("dlsym: %s\n", dlerror());
        return 1;
    }
    r = init(0);
    printf("cuInit: %d\n", r);
    if (r)
        return 1;
    count(&n);
    for (i = 0; i < n; i++) {
        char buf[256];
        size_t bytes = 0;
        int dev = -1;
        memset(buf, 0, sizeof buf);
        get(&dev, i);
        name(buf, sizeof buf, dev);
        mem(&bytes, dev);
        printf("device %d: %s, %lu MiB\n", i, buf, (unsigned long)(bytes >> 20));
    }
    return n < 1;
}
EOF
if cc -o cuinit cuinit.c 2> cuinit.err && ./cuinit > cuinit.out 2>> cuinit.err && grep -q '^cuInit: 0' cuinit.out && grep -q '^device 0: ' cuinit.out; then
    ok "gpu-cuinit ($(grep -c '^device' cuinit.out) devices; $(sed -n 's/^device 0: //p' cuinit.out))"
else bad gpu-cuinit; cat cuinit.out cuinit.err | head -5; fi
# the driver's two libraries import nothing that our C library has not (dlcheck finds them where the loader does)
if dlcheck > dlcheck.out 2>&1; then ok "gpu-dlcheck ($(tail -1 dlcheck.out | sed 's/^dlcheck: //'))"; else bad gpu-dlcheck; cat dlcheck.out; fi

cat > k.cu <<'EOF'
#include <stdio.h>
#include <cuda_runtime.h>
__global__ void scale(float *x, float a, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) x[i] = a * x[i] + sqrtf((float)i);
}
int main(void)
{
    enum { N = 100000 };
    static float h[N];
    float *d;
    double s = 0;
    int i;
    for (i = 0; i < N; i++) h[i] = (float)(i % 17);
    if (cudaMalloc(&d, sizeof h) != cudaSuccess) { printf("cudaMalloc: %s\n", cudaGetErrorString(cudaGetLastError())); return 1; }
    cudaMemcpy(d, h, sizeof h, cudaMemcpyHostToDevice);
    scale<<<(N + 255) / 256, 256>>>(d, 0.5f, N);
    cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
    for (i = 0; i < N; i++) s += h[i];
    printf("sum %.1f %s\n", s, cudaGetErrorString(cudaGetLastError()));
    return 0;
}
EOF
L="-L/usr/cuda/lib64 -lcudart -lcublasLt -lstdc++ -lm -lpthread"
if nvcc -O3 -arch=sm_86 -c k.cu -o k.o 2> k.err && gcc -o k k.o $L 2>> k.err; then
    MCUDA_VERBOSE=1 ./k > k.gpu 2> k.gpu.err
    MCUDA_BACKEND=cpu MCUDA_VERBOSE=1 ./k > k.cpu 2> k.cpu.err
    if grep -q 'the GPU run time' k.gpu.err && grep -q '^sum .* no error' k.gpu; then ok "gpu-kernel ($(cat k.gpu))"; else bad gpu-kernel; head -3 k.gpu k.gpu.err; fi
    if grep -q 'the CPU run time' k.cpu.err && grep -q '^sum .* no error' k.cpu; then ok "gpu-kernel-on-cpu ($(cat k.cpu))"; else bad gpu-kernel-on-cpu; head -3 k.cpu k.cpu.err; fi
else bad gpu-kernel-build; head -5 k.err; fi

if nvcc -O3 -arch=sm_86 -c /work/ltcmp.cu -o ltcmp.o 2> ltcmp.err && gcc -o ltcmp ltcmp.o $L 2>> ltcmp.err; then
    MCUDA_BACKEND=cpu ./ltcmp > lt.cpu 2> lt.cpu.err; c=$?
    MCUDA_BACKEND=gpu ./ltcmp > lt.gpu 2> lt.gpu.err; g=$?
    n=$(grep -c 'no error$' lt.gpu)
    if [ $c = 0 ] && [ $g = 0 ] && [ "$n" -ge 20 ] && cmp -s lt.cpu lt.gpu; then ok "gpu-cublaslt ($n products, the same bits as on the CPU)"; else
        bad "gpu-cublaslt (status $c and $g)"; diff lt.cpu lt.gpu | head -6; head -3 lt.gpu.err
    fi
else bad gpu-cublaslt-build; head -5 ltcmp.err; fi

for t in $UFSM_GPU_TESTS; do
    if [ ! -x /work/out/$t ]; then bad "ufsm-gpu-$t (not built)"; continue; fi
    t0=$(date +%s)
    if (cd /work/ufsm && TMPDIR=/tmp MCUDA_VERBOSE=1 /work/out/$t > /work/gpu/$t.log 2>&1); then
        if grep -q 'the GPU run time' /work/gpu/$t.log; then ok "ufsm-gpu-$t ($(($(date +%s) - t0)) s; it ends: $(grep -v '^mcuda: ' /work/gpu/$t.log | tail -1 | cut -c1-80))"; else bad "ufsm-gpu-$t (it did not run on the GPU)"; fi
    else
        bad "ufsm-gpu-$t (status $?)"; tail -4 /work/gpu/$t.log | cut -c1-300
    fi
done
echo "== gpu done =="
