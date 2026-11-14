/* mcuda: a CPU executor for NVIDIA PTX and a replacement for the CUDA run-time libraries.
   This header is the interface between the PTX engine (ptx*.c) and the libraries built on it
   (the cuda* run time, the cu* driver API, cublasLt). Nothing here is seen by user programs. */
#ifndef MC_H
#define MC_H
#include <stddef.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long u64; /* LP64 only */
typedef long s64;
typedef signed char s8;
typedef short s16;

typedef struct McModule McModule; /* one PTX translation unit, parsed */
typedef struct McFunc McFunc;     /* an .entry or .func of a module */

/* Parse PTX text (NUL-terminated; it must stay valid while the module lives: nothing is copied).
   Returns 0 on error, with the message (naming the PTX line) in mc_error(). Function bodies are
   decoded on first launch. `name` is used in diagnostics only and may be 0. */
McModule *mc_module_load(const char *ptx, const char *name);
void mc_module_free(McModule *m);
/* An .entry (or .func) by its PTX name; 0 if the module has none. */
McFunc *mc_module_func(McModule *m, const char *name);
McFunc *mc_find_kernel(const char *name);
int mc_func_defined(McFunc *f);
extern int (*mc_more_modules)(void);
/* Host address of a .global or .const variable of the module; 0 if there is none. *size gets its size in bytes. */
void *mc_module_global(McModule *m, const char *name, u64 *size);
/* Give a module-level variable that the PTX declares `.extern .global` (or any .global, before the first
   launch) its storage. Returns 0, or -1 if there is no such variable. */
int mc_module_bind(McModule *m, const char *name, void *addr);

/* The parameters of a kernel in declaration order. mc_func_param returns 0, or -1 when i is out of range;
   offset is the place of the parameter in the parameter buffer that ld.param reads. */
int mc_func_nparams(McFunc *f);
int mc_func_param(McFunc *f, int i, u64 *size, u64 *align, u64 *offset);
u64 mc_func_param_bytes(McFunc *f);
const char *mc_func_name(McFunc *f);
/* The .maxntid / .reqntid of an entry (0, 0, 0 if absent) and its static shared memory in bytes. */
void mc_func_limits(McFunc *f, u32 maxntid[3], u32 reqntid[3], u64 *static_shared);

/* Run a kernel over a grid: every block, every thread, on the calling host thread, to completion.
   args[i] points at the value of parameter i. Returns 0, or a cudaError_t / CUresult value (both
   enumerations use the same numbers): 1 invalid value, 218 invalid PTX (a body that does not decode),
   719 launch failure (a fault, a trap or a deadlock; the diagnostic goes to stderr and into mc_error()). */
int mc_launch(McFunc *f, const u32 grid[3], const u32 block[3], u64 shared_bytes, void **args);
/* The same with the parameter buffer already laid out as mc_func_param describes. */
int mc_launch_buf(McFunc *f, const u32 grid[3], const u32 block[3], u64 shared_bytes, const void *params);

/* The message of the last error on this host thread ("" if none). */
const char *mc_error(void);
/* PTX instructions executed so far by all launches (for measurements). */
u64 mc_instructions(void);
/* the sums of a floating-point mma as the hardware makes them (1) or rounded once (0); MCUDA_MMA_HW=1 is the
   same said from outside */
void mc_mma_hw(int on);

/* The fake device. */
#define MC_DEV_NAME "NVIDIA GeForce RTX 3090"
#define MC_DEV_SM_MAJOR 8
#define MC_DEV_SM_MINOR 6
#define MC_DEV_NSM 82
#define MC_DEV_MAX_SHARED 102400UL /* bytes of shared memory a block may ask for: more than this fails the launch */
#define MC_DEV_MAX_THREADS 1024

/* lock.c: every lock of the library. No-ops until there are host threads. */
void mcuda_lock(int which);
void mcuda_unlock(int which);
#define MC_LOCK_MODULE 0 /* loading and decoding modules */
#define MC_LOCK_ATOM 1   /* atom / red instructions on memory that several blocks may touch */
#define MC_LOCK_RT 2     /* the run-time library's tables (allocations, streams, events) */
#define MC_LOCK_ERR 3
#define MC_LOCK_JOB 4    /* the blocks of the launch that is running */
void mcuda_lock_addr(const void *p); /* for an atom / red on the word at p */
void mcuda_unlock_addr(const void *p);
int mcuda_nworkers(void); /* host threads a launch may use */
void mcuda_par(int n, void (*fn)(void *arg, int worker), void *arg); /* fn on n threads at once, the caller among them */
#define MC_MAX_WORKERS 64

/* Conversions that the run time shares with the engine (fpconv.c): bit-exact, round to nearest even. */
u32 mc_f32_to_f16(u32 f32bits);
u32 mc_f16_to_f32(u32 f16bits);
u32 mc_f32_to_bf16(u32 f32bits);
u32 mc_bf16_to_f32(u32 bf16bits);
/* a * b + c with one rounding */
float mc_fmaf(float a, float b, float c);
double mc_fma(double a, double b, double c);
#endif
