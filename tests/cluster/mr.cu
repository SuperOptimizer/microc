/* __maxnreg__(n) on a kernel (and on a template instance, beside __launch_bounds__): PTX .maxnreg n, the fixed entry budget that
   setmaxnreg.inc / .dec need (without it ptxas drops them: C7508 "unable to determine register count at entry") */
template <int R> __global__ void __maxnreg__(R) __launch_bounds__(640, 1) mr_k(unsigned *out) {
    unsigned v = threadIdx.x;
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 1200
    if (threadIdx.x < 128) { asm volatile("setmaxnreg.inc.sync.aligned.u32 120;"); v += 1000; }
    else { asm volatile("setmaxnreg.dec.sync.aligned.u32 80;"); v += 2000; }
#endif
    out[blockIdx.x * 640 + threadIdx.x] = v;
}
template __global__ void mr_k<96>(unsigned *);
__global__ void __maxnreg__(64) plain_k(float *o) { o[threadIdx.x] *= 2.f; }
__global__ void none_k(float *o) { o[threadIdx.x] = 1.f; }
