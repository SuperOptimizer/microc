#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(int argc, char **argv)
{
    int a[10], i;
    char buf[64];
    char *p = malloc(100);
    double d;
    printf("hello, %s! argc=%d %c %5d|%-5d|%05d %x %lu\n", "world", argc, 'x', 42, 42, 42, 255, 1234567890123UL);
    printf("%f %.3f %e %g %g %g %10.2f|\n", 3.14159, 2.0 / 3, 12345.678, 0.0001, 1e20, 100.0, -1.005);
    printf("%.17g %.0f %g %g\n", 0.1, 2.5, 1.0 / 3, 5e-324);
    for (i = 0; i < 10; i++)
        a[i] = (i * 7919) % 13;
    qsort(a, 10, sizeof a[0], cmp);
    for (i = 0; i < 10; i++)
        printf("%d ", a[i]);
    printf("\n");
    strcpy(p, "dynamic");
    sprintf(buf, "%s-%d", p, (int)strlen(p));
    puts(buf);
    d = strtod("6.02214076e23", 0);
    printf("%g %.15g %g %g\n", d, strtod("0.1", 0) + strtod("0.2", 0), sqrt(2.0), pow(2.0, 10));
    printf("%.6f %.6f %.6f %.6f\n", sin(1.0), cos(1.0), exp(1.0), log(10.0));
    sscanf("12 abc 3.5", "%d %s %lf", &i, buf, &d);
    printf("%d %s %g\n", i, buf, d);
    free(p);
    return argv[0] ? 7 : 0; /* exit status is checked too */
}
