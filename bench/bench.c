/* Small compute kernels for comparing generated code. Prints checksums. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static clock_t t0;
static void lap(const char *name)
{
    clock_t t1 = clock();
    fprintf(stderr, "  %-8s %6ld ms\n", name, (long)(t1 - t0) / (CLOCKS_PER_SEC / 1000));
    t0 = t1;
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int sieve(int n)
{
    char *comp = calloc((size_t)n + 1, 1);
    int i, j, count = 0;
    for (i = 2; i <= n; i++) {
        if (comp[i])
            continue;
        count++;
        for (j = i * 2; j <= n; j += i)
            comp[j] = 1;
    }
    free(comp);
    return count;
}

#define N 120
static double a[N][N], b[N][N], c[N][N];
static double matmul(void)
{
    int i, j, k;
    double s = 0;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            a[i][j] = i + j;
            b[i][j] = i - j;
        }
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            double t = 0;
            for (k = 0; k < N; k++)
                t += a[i][k] * b[k][j];
            c[i][j] = t;
        }
    for (i = 0; i < N; i++)
        s += c[i][i];
    return s;
}

static unsigned hash(const char *s, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static int cmp_int(const void *x, const void *y)
{
    int p = *(const int *)x, q = *(const int *)y;
    return p < q ? -1 : p > q;
}

static long sort_test(int n)
{
    int *v = malloc((size_t)n * sizeof(int)), i;
    unsigned seed = 12345;
    long s = 0;
    for (i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        v[i] = (int)(seed >> 8) % 100000;
    }
    qsort(v, (size_t)n, sizeof(int), cmp_int);
    for (i = 0; i < n; i += 97)
        s += v[i];
    free(v);
    return s;
}

struct Node { struct Node *next; int v; };
static long list_test(int n, int rounds)
{
    struct Node *nodes = malloc((size_t)n * sizeof(struct Node)), *p;
    long s = 0;
    int i, r;
    for (i = 0; i < n; i++) {
        nodes[i].v = i;
        nodes[i].next = i + 1 < n ? &nodes[i + 1] : 0;
    }
    for (r = 0; r < rounds; r++)
        for (p = nodes; p; p = p->next)
            s += p->v ^ r;
    free(nodes);
    return s;
}

int main(void)
{
    char buf[4096];
    unsigned h = 0;
    int i;
    memset(buf, 'x', sizeof buf);
    t0 = clock();
    printf("fib %d\n", fib(35));
    lap("fib");
    printf("sieve %d\n", sieve(20000000));
    lap("sieve");
    for (i = 0; i < 20; i++)
        h += (unsigned)matmul();
    printf("matmul %u\n", h);
    lap("matmul");
    h = 0;
    for (i = 0; i < 100000; i++)
        h ^= hash(buf, sizeof buf) + (unsigned)i;
    printf("hash %u\n", h);
    lap("hash");
    printf("sort %ld\n", sort_test(2000000));
    lap("sort");
    printf("list %ld\n", list_test(100000, 2000));
    lap("list");
    return 0;
}
