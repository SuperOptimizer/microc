/* loops with several branches on a value they do not change (x86-64 makes two loops and tests once: unswitch in
   opt.c): values read after the loop, ways out in the middle, loops in loops, calls */
#include <stdio.h>

typedef struct { int mode; float lut[256]; } plan;

__attribute__((noinline)) static float corners(const unsigned char *c, const plan *ap, int hard, int n, float mean, float *t)
{
    float s = 0;
    int last = -1, k, j;
    for (k = 0; k < n; k++) {
        float cv = 0;
        for (j = 0; j < 4; j++) {
            float val = ap && ap->mode ? 1.f : ap ? ap->lut[c[k + j]] : (c[k + j] - mean) * .5f;
            cv += val * (j + 1);
            if (!hard)
                t[j] += val;
        }
        s += cv;
        if (cv > 100)
            last = k;
    }
    return s + last;
}

/* ways out of the middle, and what is read after them */
__attribute__((noinline)) static int find(const int *a, int n, int up, int lim, int *where)
{
    int k, acc = 0, hit = -1;
    for (k = 0; k < n; k++) {
        if (up)
            acc += a[k];
        else
            acc -= a[k];
        if (acc > lim || acc < -lim) {
            hit = k;
            break;
        }
        if (up)
            acc += 1;
        if (a[k] == 99)
            return -5 - k;
    }
    *where = hit;
    return acc * 2 + k;
}

__attribute__((noinline)) static int note(int x) { static int n; n += x; return n; }

/* a call in the loop, a loop in the loop, the value an unsigned char that is not 0 or 1 */
__attribute__((noinline)) static long nest(const unsigned char *m, int rows, int cols, unsigned char flag, int twice)
{
    long s = 0;
    int r, c;
    for (r = 0; r < rows; r++) {
        if (flag)
            s += note(r);
        for (c = 0; c < cols; c++) {
            if (twice)
                s += m[r * cols + c] * 2;
            else
                s += m[r * cols + c];
            if (flag && m[r * cols + c] > 200)
                s -= 7;
            if (twice && c == 2)
                s ^= 1;
        }
        if (!flag)
            s += 3;
    }
    return s;
}

/* a loop that does not run, and one whose leaving depends on the value */
__attribute__((noinline)) static int until(int n, int stop, int step)
{
    int k = 0, s = 0;
    while (k < n) {
        if (stop)
            s += k;
        if (stop && s > 20)
            break;
        if (!stop)
            s -= 1;
        k += step;
    }
    return s * 100 + k;
}

int main(void)
{
    static plan pl, pm;
    static int a[10] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3}, b[4] = {1, 99, 2, 3};
    unsigned char c[64];
    float t[4] = {0};
    volatile int z = 0;
    int i, w = 7;
    for (i = 0; i < 256; i++)
        pl.lut[i] = i * .25f;
    pm.mode = 1;
    for (i = 0; i < 64; i++)
        c[i] = (unsigned char)(i * 37);
    printf("%g %g %g %g %g\n", corners(c, &pl, z, 60, 3, t), corners(c, 0, z, 60, 3, t), corners(c, &pl, z + 1, 60, 3, t),
           corners(c, 0, z + 1, z, 3, t), corners(c, &pm, z, 9, 3, t));
    printf("%g %g\n", t[0], t[3]);
    printf("%d", find(a, 10, z + 1, 20, &w));
    printf(" %d", w);
    printf(" %d", find(a, 10, z, 20, &w));
    printf(" %d", w);
    printf(" %d", find(a, 10, z, 1000, &w));
    printf(" %d", w);
    printf(" %d %d %d\n", find(b, 4, z + 1, 1000, &w), find(b, 4, z, 1000, &w), find(b, z, z, 1000, &w));
    printf("%ld %ld %ld %ld %ld\n", nest(c, 4, 9, (unsigned char)(z + 4), z), nest(c, 4, 9, (unsigned char)z, z + 1),
           nest(c, 4, 9, (unsigned char)(z + 255), z + 2), nest(c, z, 9, 1, 1), nest(c, 3, z, 0, 0));
    printf("%d %d %d %d\n", until(10, z, 1), until(10, z + 1, 1), until(z, z + 1, 1), until(40, z + 1, 3));
    return 0;
}
