#ifndef _FENV_H
#define _FENV_H

/* the bits of the AArch64 status (FPSR) and control (FPCR) registers */
#define FE_INVALID 1
#define FE_DIVBYZERO 2
#define FE_OVERFLOW 4
#define FE_UNDERFLOW 8
#define FE_INEXACT 16
#define FE_ALL_EXCEPT 31
#define FE_TONEAREST 0
#define FE_UPWARD 0x400000
#define FE_DOWNWARD 0x800000
#define FE_TOWARDZERO 0xc00000

typedef unsigned int fexcept_t;
typedef struct { unsigned long __fpcr, __fpsr; } fenv_t;
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int), feraiseexcept(int), fetestexcept(int);
int fegetexceptflag(fexcept_t *, int), fesetexceptflag(const fexcept_t *, int);
int fegetround(void), fesetround(int);
int fegetenv(fenv_t *), fesetenv(const fenv_t *), feholdexcept(fenv_t *), feupdateenv(const fenv_t *);

#endif
