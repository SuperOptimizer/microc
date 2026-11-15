/* A float constant that only the second comparison of an && / || chain reads (go < 0.3) is loaded after the chain's
   first branch out; a block reached by that branch, entered only from there, must not take it as loaded (x86-64
   kept it per block: an uninitialised xmm register, so f(NaN, 0, 1) was 2). */
#include <stdio.h>
static volatile double zero = 0.0, big = 1e300;
__attribute__((noinline)) int f(double ro, double ri, double rr, double nan)
{
    int me = 0, sk = 0;
    const double I = __builtin_inf();
    const double go = (ro < I) & (ro > -I) ? (ro - rr) : nan, gi = (ri < I) & (ri > -I) ? (rr - ri) : nan;
    if (((go < I) & (go > -I) & (go < 0.3)) || ((gi < I) & (gi > -I) & (gi < 0.3)))
        me = 1;
    else if (((go < I) & (go > -I) & (go > 1.8)) || ((gi < I) & (gi > -I) & (gi > 1.8)))
        sk = 1;
    return me * 2 + sk;
}
int main(void)
{
    double inf = big * big, nan = zero / zero, v[10];
    int i, j, k, n = 0;
    v[0] = nan, v[1] = inf, v[2] = -inf, v[3] = 0, v[4] = 0.1, v[5] = 1.0, v[6] = 5.0, v[7] = -3.0, v[8] = 2.0, v[9] = 0.25;
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++)
            for (k = 3; k < 10; k++)
                n = n * 3 + f(v[i], v[j], v[k], nan);
        printf("%d\n", n);
    }
    return 0;
}
