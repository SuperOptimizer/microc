// error: 'ufsm_dev_phys' undeclared
/* a device function called without its header (ufsm's missing ufsm_dev.cuh): C++ / CUDA have no implicit int f() */
__global__ void k(unsigned *p) { p[0] = ufsm_dev_phys(p[1]); }
