/* -g: shadowed locals each resolve to their own block (DW_TAG_lexical_block); &x keeps them in the frame */
#include <stdio.h>
__attribute__((noinline)) static void use(int *p) { /* (kept: the test is about lexical blocks, not optimised locations) */ if (*p == -1) printf("."); }
static int sq(int v) { int t = v * v; return t + 1; }
int f(int n)
{
    int x = n + 1, s = 0;
    use(&x);
    {
        int x = n * 10;
        use(&x);
        {
            int x = n * 100;
            use(&x);
            s += sq(x); /* BP1 */
        }
        s += x; /* BP2 */
    }
    for (int x = 7; x < 8; x++) {
        use(&x);
        s += x; /* BP3 */
    }
    s += x; /* BP4 */
    return s;
}
int main(int argc, char **argv) { (void)argv; printf("%d\n", f(argc + 1)); return 0; }
