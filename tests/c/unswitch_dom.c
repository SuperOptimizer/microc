/* unswitching makes a second copy of a loop whose own loops then go (they ran on the value tested), and the next
   unswitch round copies that copy again: the copy's header (phis and a jump) must keep its phis. simplify_cfg's
   forward_phi_blocks moved them away when the dominator tree still had the numbers of blocks from before the copy
   (ufsm's l1_solve: the move loop never stopped or counted the wrong work; opt.c unswitch, 84903be) */
#include <stdio.h>
#include <stdlib.h>

static int ticks;

/* (a loop that never stops ends the test with exit 3 instead of hanging it) */
__attribute__((noinline)) static void tick(void)
{
    if (++ticks > 1000) {
        printf("runaway\n");
        exit(3);
    }
}

__attribute__((noinline)) static long solve(unsigned n, int a, int b, int vb, const long *un, unsigned long *wout)
{
    long *s = calloc(n ? n : 1, sizeof *s), e = 0;
    unsigned long work = 0;
    int iters = 0, fails = 0, sgn;
    for (sgn = 1; iters < 16 && fails < 2; sgn = -sgn, iters++) {
        long base = 0, nm = 0;
        unsigned u;
        tick();
        for (u = 0; u < n; u++)
            if (un[u] < 0)
                base += un[u];
        if (a) {
            if (b) {
                work += n + (unsigned long)iters;
                if (vb)
                    fprintf(stderr, "work %lu\n", work);
                if (base + iters >= 0) {
                    fails++;
                    continue;
                }
            }
        }
        for (u = 0; u < n; u++) {
            s[u] += (u + (unsigned)iters) % 3 == 0 ? sgn : 0;
            nm += s[u];
        }
        if (vb)
            fprintf(stderr, "nm %ld\n", nm);
        if (nm + iters >= 6 || !n) {
            fails++;
            continue;
        }
        e += nm;
    }
    *wout = work;
    free(s);
    return e + iters;
}

int main(void)
{
    long un[8] = {-1, 2, -3, 4, -5, 6, -7, 8};
    unsigned n;
    int a, b;
    for (n = 0; n <= 8; n += 4)
        for (a = 0; a < 2; a++)
            for (b = 0; b < 2; b++) {
                unsigned long w;
                long r = solve(n, a, b, 0, un, &w);
                printf("n %u a %d b %d: %ld work %lu\n", n, a, b, r, w);
            }
    return 0;
}
