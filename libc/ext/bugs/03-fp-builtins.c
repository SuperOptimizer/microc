/* Links fail with undefined ___builtin_* / __builtin_*: mcc has no floating-point builtins (found while
   writing libm, and ufsm calls __builtin_rintf and the C23 __builtin_clzg):
     mcc --target=aarch64-linux-gnu 03-fp-builtins.c    -> undefined symbol: __builtin_sqrt, ...
   Also: inline asm rejects FP register operands, so libm cannot reach fsqrt/fmadd/frint* that way:
     double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));   -> "unsupported asm output operand"
   libm works around both with assembly stubs (libc/sys/math_arm64.s, the __asm__ block in libc/src/math.c). */
double a(double x) { return __builtin_sqrt(x) + __builtin_fabs(x) + __builtin_inf(); }
float b(float x) { return __builtin_rintf(x); }
int c(unsigned long x) { return __builtin_clzg(x); }
int main(void) { return (int)(a(4.0) + b(1.5f)) + c(1); }
