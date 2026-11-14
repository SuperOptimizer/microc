/* A word read from bytes at any address with memcpy stays a copy of bytes (the GPU faults on a wide load there),
   also where the local it is copied into is otherwise a register. */
#include <stdio.h>
#include <string.h>
#include <cuda_runtime.h>
struct pr { unsigned a, b; };
__global__ void run(const unsigned char *p, unsigned long long *o) {
    const int i = threadIdx.x;
    unsigned long long w; unsigned u; pr q;
    memcpy(&w, p + i, 8); memcpy(&u, p + i + 3, 4); memcpy(&q, p + i + 1, 8);
    o[i] = w ^ ((unsigned long long)u << 7) ^ q.a ^ ((unsigned long long)q.b << 32);
}
int main(void) {
    unsigned char b[64]; unsigned long long o[32], x = 0, *dout; unsigned char *db; int i;
    for (i = 0; i < 64; i++) b[i] = (unsigned char)(i * 37 + 11);
    cudaMalloc(&db, 64); cudaMalloc(&dout, sizeof o); cudaMemcpy(db, b, 64, cudaMemcpyHostToDevice);
    run<<<1, 32>>>(db, dout);
    printf("%d\n", (int)cudaDeviceSynchronize());
    cudaMemcpy(o, dout, sizeof o, cudaMemcpyDeviceToHost);
    for (i = 0; i < 32; i++) x = x * 1099511628211ull ^ o[i];
    printf("%016llx\n", x);
    return 0;
}
