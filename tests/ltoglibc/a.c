/* -flto on x86-64 glibc (tests/ltoglibc.sh): what the link-time optimiser must carry through the system's linker */
#include <stdio.h>
#include <string.h>
extern int b_twice(int);
extern const char *b_name(void);
extern _Thread_local int b_tls;
extern int b_shared;
extern int b_picked(void);
int weak_missing(void) __attribute__((weak));
static int ctor_ran;
__attribute__((constructor)) static void ctor(void) { ctor_ran = 41; }
static int square(int x) { return x * x; }
static int (*const table[2])(int) = {square, b_twice};
static unsigned mxcsr(void) { unsigned c; __asm__ volatile("stmxcsr %0" : "=m"(c)); return c; }
int main(void)
{
    int s = 0, k;
    for (k = 0; k < 10; k++)
        s += table[k & 1](k);
    b_tls = 5;
    printf("pick %d ctor %d sum %d name %s tls %d shared %d weak %s mxcsr %s %zu\n", b_picked(), ctor_ran + 1, s, b_name(), b_tls, b_shared,
           weak_missing ? "set" : "null", (mxcsr() & 0x1f80) == 0x1f80 ? "default" : "changed", strlen("seven77"));
    return 0;
}
