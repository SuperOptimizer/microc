/* pure math calls of loop-invariant arguments (licm, MCC_NOPURELICM): hoisted from a block every trip runs, kept in a
   conditional one, a loop that runs no trip, loads hoisted past a pure call, a non-pure call keeps the loop's memory */
#include <stdio.h>
#include <math.h>

static int calls;
static double tick(double x) { calls++; return x; }

static double rot(const double *x, const double *y, int n, double ang)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += x[i] * cos(ang) - y[i] * sin(ang);
    return s;
}

static double cond(const double *x, int n, double a, double b)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++)
        if (x[i] > 0.5)
            s += atan2(a, b); /* a conditional pure call */
        else
            s -= hypot(a, b) * x[i];
    return s;
}

static double nested(int n, int m, const double *k)
{
    double s = 0;
    int i, j;
    for (i = 0; i < n; i++)
        for (j = 0; j < m; j++)
            s += sqrt(k[0] + i) * exp(k[1]) + j; /* exp(k[1]) leaves both loops, sqrt the inner one; k[*] loads too */
    return s;
}

static double withcall(double *g, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s += log(g[0] + 2.0); /* g[0] changes in the call: neither the load nor the log may leave */
        g[0] = tick(g[0] + 1.0);
    }
    return s;
}

int main(void)
{
    double x[8] = {0.1, 0.9, 0.3, 0.7, 0.2, 0.8, 0.6, 0.4}, y[8] = {1, 2, 3, 4, 5, 6, 7, 8}, k[2] = {2.0, 0.5}, g[1] = {1.0};
    volatile int zero = 0, eight = 8;
    printf("rot %.6f\n", rot(x, y, eight, 0.3));
    printf("rot0 %.6f\n", rot(x, y, zero, 0.3));
    printf("cond %.6f\n", cond(x, eight, 1.0, 2.0));
    printf("nested %.6f\n", nested(3, 4, k));
    printf("nested0 %.6f\n", nested(zero, 4, k));
    {
        double w = withcall(g, eight);
        printf("withcall %.6f calls %d g %.1f\n", w, calls, g[0]);
    }
    return 0;
}
