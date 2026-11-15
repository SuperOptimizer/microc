/* nvcc: -arch=sm_120 */
/* A kernel's struct parameter read at places worked out at run time (p.t[i], p.d[i][j], a pointer stepped through it,
   one of two members chosen by a select) is read where the parameter is (ld.param), not from a copy in local memory;
   a kernel that hands the parameter's address on (a device function, a copy into another struct, a write into the copy)
   keeps the copy. Every kernel's result is checked against the same arithmetic on the host. */
#include <cuda_runtime.h>
#include <stdio.h>

struct Tab { signed char t[27], d[27][3]; short w[9]; int n; long long big; };

__global__ void kread(Tab p, int *o)
{
    int i = threadIdx.x, s = 0;
    for (int k = 0; k < p.n; k++)
        s += p.t[(i + k) % 27] * p.d[(i * 7 + k) % 27][k % 3];
    const signed char *q = (i & 1) ? p.t : &p.d[0][0];
    for (int k = 0; k < 5; k++)
        s += q[(i + 3 * k) % 27];
    s += p.w[i % 9] + (int)(p.big >> (i % 40));
    o[i] = s;
}

__device__ __noinline__ int sum9(const short *w) { int s = 0; for (int k = 0; k < 9; k++) s += w[k] * (k + 1); return s; }

__global__ void kesc(Tab p, int *o)
{
    int i = threadIdx.x;
    Tab c = p;               /* a copy of the block */
    c.t[i % 27] += 1;        /* a write into it */
    p.t[(i + 1) % 27] -= 2;  /* a write into the parameter's own copy */
    o[i] = sum9(p.w) + c.t[i % 27] + p.t[(i + 1) % 27] + p.t[i % 27];
}

static int hread(const Tab *p, int i)
{
    int s = 0;
    for (int k = 0; k < p->n; k++)
        s += p->t[(i + k) % 27] * p->d[(i * 7 + k) % 27][k % 3];
    const signed char *q = (i & 1) ? p->t : &p->d[0][0];
    for (int k = 0; k < 5; k++)
        s += q[(i + 3 * k) % 27];
    return s + p->w[i % 9] + (int)(p->big >> (i % 40));
}

static int hesc(const Tab *p, int i)
{
    int s = 0;
    for (int k = 0; k < 9; k++)
        s += p->w[k] * (k + 1);
    Tab c = *p, q = *p;
    c.t[i % 27] += 1;
    q.t[(i + 1) % 27] -= 2;
    return s + c.t[i % 27] + q.t[(i + 1) % 27] + q.t[i % 27];
}

int main()
{
    Tab p;
    for (int k = 0; k < 27; k++) {
        p.t[k] = (signed char)(k * 37 - 100);
        for (int j = 0; j < 3; j++)
            p.d[k][j] = (signed char)(k * 11 - j * 50 + 3);
    }
    for (int k = 0; k < 9; k++)
        p.w[k] = (short)(k * 1000 - 4000);
    p.n = 13;
    p.big = 0x123456789abcdefLL;
    int *d, h[64], bad = 0;
    cudaMalloc(&d, sizeof h);
    kread<<<1, 64>>>(p, d);
    cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
    for (int i = 0; i < 64; i++)
        if (h[i] != hread(&p, i)) {
            if (bad++ < 4) printf("kread %d: %d, host %d\n", i, h[i], hread(&p, i));
        }
    printf("kread: %d %d %d\n", h[0], h[17], h[63]);
    kesc<<<1, 64>>>(p, d);
    cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
    for (int i = 0; i < 64; i++)
        if (h[i] != hesc(&p, i)) {
            if (bad++ < 8) printf("kesc %d: %d, host %d\n", i, h[i], hesc(&p, i));
        }
    printf("kesc: %d %d %d\n", h[0], h[17], h[63]);
    printf("parmblk: %s\n", bad ? "FAIL" : "ok");
    return 0;
}
