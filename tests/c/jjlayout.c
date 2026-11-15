/* block layout (MCC_NOEDGEW) and jumps to a jump (MCC_NOJJ): a loop whose body is entered past a test that continues,
   chains of && bounds tests to the latch, gotos through blocks that are only a jump, a switch whose cases jump on */
#include <stdio.h>
#include <string.h>

static void dilate(const unsigned char *in, unsigned char *out, const long n[3], int tol)
{
    long z, y, x;
    int dz, dy, dx;
    memcpy(out, in, (size_t)(n[0] * n[1] * n[2]));
    for (z = 0; z < n[0]; z++)
        for (y = 0; y < n[1]; y++)
            for (x = 0; x < n[2]; x++) {
                if (!in[(z * n[1] + y) * n[2] + x])
                    continue;
                for (dz = -tol; dz <= tol; dz++)
                    for (dy = -tol; dy <= tol; dy++)
                        for (dx = -tol; dx <= tol; dx++) {
                            long zz = z + dz, yy = y + dy, xx = x + dx;
                            if (dz * dz + dy * dy + dx * dx > tol * tol)
                                continue;
                            if (zz >= 0 && zz < n[0] && yy >= 0 && yy < n[1] && xx >= 0 && xx < n[2])
                                out[(zz * n[1] + yy) * n[2] + xx] = 1;
                        }
            }
}

static int hops(int a, int b)
{
    int r = 0;
    if (a > 3)
        goto l1;
    if (b > 3)
        goto l2;
    r += 100;
    goto l3;
l1:
    goto l2;
l2:
    goto l4;
l3:
    r += a;
l4:
    r += b;
    return r;
}

static int sw(int k, int n)
{
    int s = 0, i;
    for (i = 0; i < n; i++) {
        switch ((k + i) % 5) {
        case 0: continue;
        case 1: s += 3; break;
        case 2: break;
        case 3: if (i & 1) continue; s -= 2; break;
        default: goto next;
        }
        s += i;
    next:;
    }
    return s;
}

int main(void)
{
    static unsigned char in[9 * 8 * 7], out[9 * 8 * 7];
    long n[3] = {9, 8, 7};
    unsigned st = 7, sum = 0, k;
    int i, j;
    for (k = 0; k < sizeof in; k++) {
        st = st * 1103515245u + 12345u;
        in[k] = (st >> 16 & 31) == 0;
    }
    for (i = 0; i < 4; i++) {
        dilate(in, out, n, i);
        for (k = 0, sum = 0; k < sizeof out; k++)
            sum = sum * 31 + out[k];
        printf("dilate %d %u\n", i, sum);
    }
    for (i = 0; i < 6; i++)
        for (j = 0; j < 6; j++)
            sum = sum * 7 + (unsigned)hops(i, j);
    printf("hops %u\n", sum);
    for (i = 0; i < 5; i++)
        printf("sw %d %d\n", i, sw(i, 23));
    return 0;
}
