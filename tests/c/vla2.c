#include <stdio.h>
static long f(int n, int m)
{
    long v[3][n + 1], s = 0;
    int a[n][m];
    int i, j;
    for (i = 0; i < 3; i++) for (j = 0; j <= n; j++) v[i][j] = i * 100 + j;
    for (i = 0; i < n; i++) for (j = 0; j < m; j++) a[i][j] = i * j;
    for (i = 0; i < 3; i++) for (j = 0; j <= n; j++) s += v[i][j];
    for (i = 0; i < n; i++) for (j = 0; j < m; j++) s += a[i][j];
    printf("%lu %lu %lu %ld\n", (unsigned long)sizeof v, (unsigned long)sizeof v[0], (unsigned long)sizeof a, (long)(&a[2] - &a[0]));
    return s;
}
int main(void) { printf("%ld\n", f(4, 5)); printf("%ld\n", f(7, 3)); return 0; }
