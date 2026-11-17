/* thread-block clusters: __cluster_dims__ on a kernel and on a template's instances, beside __launch_bounds__ */
__global__ void __cluster_dims__(2, 1, 1) __launch_bounds__(64) k2(unsigned *o) { o[threadIdx.x] = blockIdx.x; }
template <int N> __global__ void __cluster_dims__(1, 1, N) __launch_bounds__(128, 1) kn(unsigned *o) {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 900
    unsigned r;
    asm volatile("mov.u32 %0, %%cluster_ctarank;" : "=r"(r));
    o[threadIdx.x] = r;
#else
    o[threadIdx.x] = 0;
#endif
}
template __global__ void kn<4>(unsigned *);
template __global__ void kn<8>(unsigned *);
__global__ void plain(unsigned *o) { o[0] = 1; }
