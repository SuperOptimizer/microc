// ok
/* intrinsics ufsm calls that must be declared by mcuda_device.h (no implicit declarations in CUDA) */
__global__ void k(float *p, unsigned *u) {
    p[0] = __fdividef(p[1], p[2]) + __expf(p[3]) + __logf(p[4]) + __saturatef(p[5]) + __fdiv_rn(p[6], p[7]) + __frcp_rn(p[8]);
    __threadfence_block(); __threadfence(); __threadfence_system();
    u[0] = __reduce_max_sync(0xffffffffu, u[1]) + __popc(u[2]) + __ffs((int)u[3]) + __ballot_sync(0xffffffffu, u[4] != 0);
}
