/* nvcc: -arch=sm_120 */
/* Loads of neighbouring bytes put together (b[k] | b[k + 1] << 8 as one 16-bit load, four bytes as one 32-bit load) only
   where the address is known to be a multiple of the width: an even index into an aligned shared array, an index 4 i
   into an aligned global array (cudaMalloc), an odd one (stays two loads), a store between the two loads (stays two),
   the pair in the other order, and a sum instead of an or. Every result is checked against the host. */
#include <cuda_runtime.h>
#include <stdio.h>

__global__ void __launch_bounds__(256) kc(const unsigned char *g, unsigned *o)
{
    __shared__ __align__(16) unsigned char s[1024], s2[1024];   /* (s2: the one written between two loads, by its own thread only) */
    int t = threadIdx.x;
    for (int i = t; i < 1024; i += 256)
        s[i] = s2[i] = g[i];
    __syncthreads();
    int e = 2 * t, d = 4 * t, odd = 2 * t + 1;
    unsigned a = (unsigned)s[e] | ((unsigned)s[e + 1] << 8);                                          /* 16, even */
    unsigned b = (unsigned)s[d] | ((unsigned)s[d + 1] << 8) | ((unsigned)s[d + 2] << 16) | ((unsigned)s[d + 3] << 24); /* 32 */
    unsigned c = (unsigned)s[odd] | ((unsigned)s[odd + 1] << 8);                                      /* odd: two loads */
    unsigned h = ((unsigned)s[e + 513] << 8) + (unsigned)s[e + 512];                                  /* the other order, a sum */
    unsigned q = (unsigned)g[d] | ((unsigned)g[d + 1] << 8) | ((unsigned)g[d + 2] << 16) | ((unsigned)g[d + 3] << 24); /* global */
    unsigned x0 = s2[e];
    s2[e + 1] = (unsigned char)(t * 3);                                                               /* a store between */
    unsigned x = x0 | ((unsigned)s2[e + 1] << 8);
    o[6 * t] = a; o[6 * t + 1] = b; o[6 * t + 2] = c; o[6 * t + 3] = h; o[6 * t + 4] = q; o[6 * t + 5] = x;
}

int main()
{
    unsigned char hb[1024];
    unsigned ho[6 * 256], *d;
    unsigned char *g;
    for (int i = 0; i < 1024; i++)
        hb[i] = (unsigned char)(i * 73 + 11);
    cudaMalloc(&g, 1024);
    cudaMalloc(&d, sizeof ho);
    cudaMemcpy(g, hb, 1024, cudaMemcpyHostToDevice);
    kc<<<1, 256>>>(g, d);
    cudaMemcpy(ho, d, sizeof ho, cudaMemcpyDeviceToHost);
    int bad = 0;
    for (int t = 0; t < 256; t++) {
        int e = 2 * t, dd = 4 * t, odd = 2 * t + 1;
        unsigned w[6];
        unsigned char sc[1024];
        for (int i = 0; i < 1024; i++) sc[i] = hb[i];
        w[0] = sc[e] | (sc[e + 1] << 8);
        w[1] = sc[dd] | (sc[dd + 1] << 8) | (sc[dd + 2] << 16) | ((unsigned)sc[dd + 3] << 24);
        w[2] = sc[odd] | (sc[odd + 1] << 8);
        w[3] = sc[e + 512] | (sc[e + 513] << 8);
        w[4] = w[1];
        w[5] = sc[e] | ((unsigned)(unsigned char)(t * 3) << 8);
        for (int k = 0; k < 6; k++)
            if (ho[6 * t + k] != w[k] && bad++ < 6)
                printf("t %d k %d: %08x, host %08x\n", t, k, ho[6 * t + k], w[k]);
    }
    printf("%08x %08x %08x %08x %08x %08x\n", ho[6 * 77], ho[6 * 77 + 1], ho[6 * 77 + 2], ho[6 * 77 + 3], ho[6 * 77 + 4], ho[6 * 77 + 5]);
    printf("ldcomb: %s\n", bad ? "FAIL" : "ok");
    return 0;
}
