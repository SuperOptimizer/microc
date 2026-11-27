#include <stdio.h>
static double m[40][37];
static int g[50][13];
static long col(int j, int lo, int hi, int step)
{
    long s = 0;
    int k;
    for (k = lo; k < hi; k += step)
        s += (long)(m[k][j] * 3);
    return s;
}
static long down(int j, int hi)
{
    long s = 0;
    int k;
    for (k = hi; k >= 0; k--)
        if (g[k][j] & 1)
            s += g[k][j] * 7;
        else
            s -= g[k][j];
    return s;
}
static long rows(long n, long w)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += i * w * 3 + (i * 12 - i) * 5 + (i << 4) - i;
    return s;
}
static int *last;
static long brk(int j)
{
    int k;
    long s = 0;
    for (k = 1; k < 50; k++) {
        last = &g[k][j];
        if (g[k][j] == 77)
            break;
        s += *last;
    }
    return s + (last - &g[0][0]);
}
int main(void)
{
    int i, j;
    long t = 0;
    for (i = 0; i < 40; i++)
        for (j = 0; j < 37; j++)
            m[i][j] = i * 37 + j * 0.5;
    for (i = 0; i < 50; i++)
        for (j = 0; j < 13; j++)
            g[i][j] = (i * 13 + j * 7) % 101;
    for (j = 0; j < 37; j++)
        t += col(j, 0, 40, 1) + col(j, 3, 40, 3) + col(j, j, 39, 2);
    for (j = 0; j < 13; j++)
        t += down(j, 49) + down(j, j) + brk(j);
    t += rows(1000, 17) + rows(0, 5) + rows(3, -9);
    printf("%ld\n", t);
    return 0;
}
