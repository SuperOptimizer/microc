/* search loops whose element test the vectoriser looks at but cannot build (2026-11-15): it must leave them scalar,
   not build a vector loop around a test it gave up on. volcomp's vll_encode_block (b[i] != b[0], b[0] read before the
   loop) summed a register nothing had set and took mixed blocks for constant ones (ufsm test_fiberraster). The
   0.0 argument puts zero in that register. Also the byte compare with a value a byte can hold (vectorised since). */
#include <stdio.h>
#include <string.h>

#define N 4096u
static unsigned char ub[N + 64];
static signed char sb[N + 64];
static int ia[N + 64];

__attribute__((noinline)) static unsigned run0(const unsigned char *b, double z)
{
    unsigned v0 = b[0], i;
    (void)z;
    for (i = 1; i < N; i++)
        if (b[i] != v0)
            break;
    return i;
}
__attribute__((noinline)) static unsigned srun0(const signed char *b, double z)
{
    int v0 = b[0];
    unsigned i;
    (void)z;
    for (i = 1; i < N; i++)
        if (b[i] != v0)
            break;
    return i;
}
__attribute__((noinline)) static int bfindv(const unsigned char *b, int n, unsigned x, double z)
{
    int i = 0;
    (void)z;
    while (i < n && b[i] != x) /* x may be above 255: then nothing matches */
        i++;
    return i;
}
__attribute__((noinline)) static int mulfind(const int *a, int n, int x, double z)
{
    int i = 0;
    (void)z;
    while (i < n && a[i] * 3 != x)
        i++;
    return i;
}

static unsigned long h = 5381;
static void put(long v) { h = h * 33 + (unsigned long)v; }

int main(void)
{
    int off, k, v;
    for (v = 0; v < 256; v += 51)
        for (off = 0; off < 20; off++)
            for (k = 0; k <= (int)N; k += k < 40 || k > 4060 ? 1 : 129) {
                memset(ub, v, sizeof ub);
                memset(sb, v - 128, sizeof sb);
                if (k < (int)N) {
                    ub[off + k] ^= 0x40;
                    sb[off + k] ^= 0x40;
                }
                put(run0(ub + off, 0.0));
                put(srun0(sb + off, 0.0));
                put(bfindv(ub + off, 300, (unsigned)(v ^ 0x40), 0.0));
                put(bfindv(ub + off, 300, (unsigned)(v | 0x100), 0.0));
            }
    for (k = 0; k < (int)N + 64; k++)
        ia[k] = k;
    for (off = 0; off < 8; off++)
        for (k = -3; k < 200; k += 7)
            put(mulfind(ia + off, 190, k * 3, 0.0));
    printf("%lx\n", h);
    return 0;
}
