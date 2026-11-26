/* automatic arrays of constants (parse.c const_table): an object of the program's instead of a stack copy per call, and
   the ones that are not all constants (or not const) stay automatic */
#include <stdio.h>

static void dir(int d, int *dz, int *dy, int *dx)
{
    const int t[13][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, -1, 0}, {1, 0, 1}, {1, 0, -1}, {0, 1, 1}, {0, 1, -1},
                          {1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {1, -1, -1}};
    *dz = t[d][0];
    *dy = t[d][1];
    *dx = t[d][2];
}

static double wsum(int n, double x)
{
    const double w[5] = {0.5, -1.25, 2.0}; /* (the rest 0) */
    const int partial[4] = {n, 2 * n, 3, 4}; /* not all constants: stays a stack copy */
    int u[3] = {1, 2, 3};                    /* not const */
    double s = 0;
    int k;
    u[n % 3] += n;
    for (k = 0; k < 5; k++)
        s += w[k] * (x + k) + partial[k % 4] + u[k % 3];
    return s;
}

static int depth(int n, const int **seen)
{
    const short tab[4] = {3, 1, 4, 1};
    if (n == 0) {
        *seen = 0;
        return tab[0];
    }
    return tab[n & 3] + depth(n - 1, seen);
}

int main(void)
{
    int d, a, b, c, acc = 0;
    const int *seen;
    for (d = 0; d < 13; d++) {
        dir(d, &a, &b, &c);
        acc = acc * 7 + a * 9 + b * 3 + c;
        printf("%d%d%d ", a, b, c);
    }
    printf("\n%d %.4f %.4f %d\n", acc, wsum(4, 1.5), wsum(7, -2.0), depth(9, &seen));
    return 0;
}
