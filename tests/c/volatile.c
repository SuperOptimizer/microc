#include <stdio.h>
#include <signal.h>
static volatile sig_atomic_t g;
volatile double d = 1.5; volatile signed char sc = -3; volatile short sh = -7;
static void h(int s) { (void)s; g = 1; }
int main(void) { volatile int n = 0; int i; h(2);
 while (!g) ; for (i = 0; i < 10; i++) n += i; d += 1; sc--; sh *= 2;
 printf("%d %d %g %d %d\n", (int)g, n, d, sc, sh); return 0; }
