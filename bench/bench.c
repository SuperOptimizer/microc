/* Small compute kernels for comparing generated code. Prints checksums. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static clock_t t0;
static void lap(const char *name)
{
    clock_t t1 = clock();
    long us = (long)((t1 - t0) / (CLOCKS_PER_SEC / 1000000));
    fprintf(stderr, "  %-8s %6ld.%03ld ms\n", name, us / 1000, us % 1000);
    t0 = clock();
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

/* Byte loops: classify, case-fold and copy a text buffer, then search it. */
static long str_test(int rounds)
{
    static char text[8192], out[8192];
    static const char words[] = "the quick Brown fox, jumps over 42 lazy dogs; and THEN sleeps.";
    long s = 0;
    int i, r, n = (int)sizeof text - 1;
    for (i = 0; i < n; i++)
        text[i] = words[i % (int)(sizeof words - 1)];
    text[n] = 0;
    for (r = 0; r < rounds; r++) {
        const char *p = text;
        char *q = out;
        int nw = 0, inw = 0, c;
        while ((c = (unsigned char)*p++) != 0) {
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                if (!inw)
                    nw++;
                inw = 1;
                *q++ = (char)c;
            } else {
                inw = 0;
                if (c != ' ')
                    *q++ = '_';
            }
        }
        *q = 0;
        s += nw + (q - out);
        for (p = out; *p; p++)
            if (*p == '_' && p[1] == 'a')
                s++;
        text[r % n] ^= 1;
        text[r % n] |= 32;
    }
    return s;
}

/* Struct-heavy: step an array of particles with integer state. */
struct Part { int x, y, vx, vy; short life; unsigned char kind, flag; long acc; };
static long struct_test(int n, int rounds)
{
    struct Part *ps = malloc((size_t)n * sizeof(struct Part)), *p;
    long s = 0;
    int i, r;
    for (i = 0; i < n; i++) {
        p = &ps[i];
        p->x = i * 7 % 1000;
        p->y = i * 13 % 1000;
        p->vx = i % 5 - 2;
        p->vy = i % 7 - 3;
        p->life = (short)(i % 100);
        p->kind = (unsigned char)(i & 3);
        p->flag = 0;
        p->acc = 0;
    }
    for (r = 0; r < rounds; r++)
        for (i = 0; i < n; i++) {
            p = &ps[i];
            p->x += p->vx;
            p->y += p->vy;
            if (p->x < 0 || p->x >= 1000) {
                p->vx = -p->vx;
                p->flag ^= 1;
            }
            if (p->y < 0 || p->y >= 1000)
                p->vy = -p->vy;
            if (p->kind == 2)
                p->acc += p->x ^ p->y;
            else
                p->acc += p->life;
            if (--p->life < 0)
                p->life = 99;
        }
    for (i = 0; i < n; i++)
        s += ps[i].acc + ps[i].flag;
    free(ps);
    return s;
}

/* Open-addressing hash table of integer keys: insert, look up, delete. */
#define HT_SIZE (1 << 16)
struct Slot { unsigned key; int val; };
static struct Slot ht[HT_SIZE];
static struct Slot *ht_find(unsigned key)
{
    unsigned h = (key * 2654435761u) >> 16 & (HT_SIZE - 1);
    while (ht[h].key != 0 && ht[h].key != key)
        h = (h + 1) & (HT_SIZE - 1);
    return &ht[h];
}
static long ht_test(int rounds)
{
    long s = 0;
    unsigned seed = 1;
    int i, r;
    for (r = 0; r < rounds; r++) {
        memset(ht, 0, sizeof ht);
        for (i = 0; i < HT_SIZE / 2; i++) {
            struct Slot *e;
            seed = seed * 1664525u + 1013904223u;
            e = ht_find((seed >> 12) | 1);
            e->key = (seed >> 12) | 1;
            e->val += i;
        }
        for (i = 1; i < HT_SIZE; i++)
            s += ht_find((unsigned)i)->val;
    }
    return s;
}

/* A small stack-machine interpreter running a counting loop. */
enum { I_PUSH, I_ADD, I_SUB, I_MUL, I_DUP, I_SWAP, I_LOAD, I_STORE, I_JNZ, I_LT, I_XOR, I_HALT };
static long interp(const int *code, long *vars)
{
    long stack[64], *sp = stack, t;
    const int *pc = code;
    for (;;) {
        switch (*pc++) {
        case I_PUSH: *sp++ = *pc++; break;
        case I_ADD: sp--; sp[-1] += sp[0]; break;
        case I_SUB: sp--; sp[-1] -= sp[0]; break;
        case I_MUL: sp--; sp[-1] *= sp[0]; break;
        case I_XOR: sp--; sp[-1] ^= sp[0]; break;
        case I_LT: sp--; sp[-1] = sp[-1] < sp[0]; break;
        case I_DUP: sp[0] = sp[-1]; sp++; break;
        case I_SWAP: t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; break;
        case I_LOAD: *sp++ = vars[*pc++]; break;
        case I_STORE: vars[*pc++] = *--sp; break;
        case I_JNZ: t = *pc++; if (*--sp) pc = code + t; break;
        case I_HALT: return sp[-1];
        }
    }
}
static long interp_test(int n)
{
    static const int code[] = {
        /* 0 */ I_LOAD, 1, I_LOAD, 0, I_MUL, I_PUSH, 7, I_ADD, I_LOAD, 0, I_XOR, I_STORE, 1,
        /* 13 */ I_LOAD, 0, I_PUSH, 1, I_SUB, I_DUP, I_STORE, 0,
        /* 21 */ I_PUSH, 0, I_SWAP, I_LT, I_JNZ, 0,
        /* 27 */ I_LOAD, 1, I_HALT
    };
    long vars[2];
    vars[0] = n;
    vars[1] = 1;
    return interp(code, vars);
}

/* Floating point: an n-body style step (square roots and divisions) and a polynomial. */
static double fp_test(int steps)
{
    double px[8], py[8], vx[8], vy[8], m[8], e = 0;
    int i, j, s;
    for (i = 0; i < 8; i++) {
        px[i] = i * 1.5;
        py[i] = (i * i) % 5 - 2.0;
        vx[i] = 0.01 * i;
        vy[i] = -0.02 * i;
        m[i] = 1.0 + i * 0.25;
    }
    for (s = 0; s < steps; s++) {
        for (i = 0; i < 8; i++)
            for (j = i + 1; j < 8; j++) {
                double dx = px[i] - px[j], dy = py[i] - py[j];
                double d2 = dx * dx + dy * dy + 1.0;
                double mag = 0.001 / (d2 * d2);
                vx[i] -= dx * m[j] * mag;
                vy[i] -= dy * m[j] * mag;
                vx[j] += dx * m[i] * mag;
                vy[j] += dy * m[i] * mag;
            }
        for (i = 0; i < 8; i++) {
            px[i] += 1e-6 * vx[i];
            py[i] += 1e-6 * vy[i];
        }
    }
    for (i = 0; i < 8; i++)
        e += m[i] * (vx[i] * vx[i] + vy[i] * vy[i]) + px[i] * 1e-3;
    return e;
}

static int mandel(int w, int h, int maxit)
{
    int x, y, it, total = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double cr = -2.0 + 3.0 * x / w, ci = -1.0 + 2.0 * y / h, zr = 0, zi = 0;
            for (it = 0; it < maxit; it++) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                if (zr * zr + zi * zi > 4.0)
                    break;
            }
            total += it;
        }
    return total;
}

/* Bit twiddling: table-less CRC-32 and population counts. */
static unsigned crc_test(const unsigned char *p, int n, int rounds)
{
    unsigned crc = 0xffffffffu, pop = 0;
    int i, k, r;
    for (r = 0; r < rounds; r++)
        for (i = 0; i < n; i++) {
            unsigned x;
            crc ^= p[i] + (unsigned)r;
            for (k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
            x = crc;
            x = x - ((x >> 1) & 0x55555555u);
            x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
            pop += (((x + (x >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
        }
    return crc ^ pop;
}

/* Binary search tree: build from a pool, then look keys up through child pointers. */
struct Tree { struct Tree *kid[2]; int key, count; };
static long tree_test(int n, int lookups)
{
    struct Tree *pool = malloc((size_t)n * sizeof(struct Tree)), *root = 0, *t, **link;
    unsigned seed = 7;
    long s = 0;
    int i, used = 0;
    for (i = 0; i < n; i++) {
        int key;
        seed = seed * 1103515245u + 12345u;
        key = (int)(seed >> 10) % (n * 4);
        link = &root;
        while ((t = *link) != 0 && t->key != key)
            link = &t->kid[key > t->key];
        if (!t) {
            t = &pool[used++];
            t->kid[0] = t->kid[1] = 0;
            t->key = key;
            t->count = 0;
            *link = t;
        }
        t->count++;
    }
    for (i = 0; i < lookups; i++) {
        int key;
        seed = seed * 1103515245u + 12345u;
        key = (int)(seed >> 10) % (n * 4);
        for (t = root; t && t->key != key; t = t->kid[key > t->key])
            s++;
        if (t)
            s += t->count;
    }
    free(pool);
    return s + used;
}

/* Integer array work: insertion sort of short runs, prefix sums, binary search. */
static long array_test(int n, int rounds)
{
    int *v = malloc((size_t)n * sizeof(int)), i, j, r;
    unsigned seed = 99;
    long s = 0;
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            v[i] = (int)(seed >> 16) & 1023;
        }
        for (i = 0; i + 32 <= n; i += 32)
            for (j = i + 1; j < i + 32; j++) {
                int x = v[j], k = j - 1;
                while (k >= i && v[k] > x) {
                    v[k + 1] = v[k];
                    k--;
                }
                v[k + 1] = x;
            }
        for (i = 1; i < n; i++)
            v[i] += v[i - 1];
        for (i = 0; i < n; i++) {
            int lo = 0, hi = n, key = i * 37;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (v[mid] < key)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            s += lo;
        }
    }
    free(v);
    return s;
}

/* Integer stencil over a 2D grid held in a flat array. */
static long stencil_test(int w, int h, int rounds)
{
    int *g = malloc((size_t)(w * h) * sizeof(int)), *t = malloc((size_t)(w * h) * sizeof(int)), *sw;
    long s = 0;
    int x, y, r;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            g[y * w + x] = (x * 31 + y * 17) & 255;
            t[y * w + x] = 0;
        }
    for (r = 0; r < rounds; r++) {
        for (y = 1; y < h - 1; y++)
            for (x = 1; x < w - 1; x++)
                t[y * w + x] = (g[y * w + x] * 4 + g[y * w + x - 1] + g[y * w + x + 1]
                                + g[(y - 1) * w + x] + g[(y + 1) * w + x]) >> 3;
        sw = g;
        g = t;
        t = sw;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            s += g[y * w + x];
    free(g);
    free(t);
    return s;
}

/* Recursion with small arrays: count the solutions of the n queens problem. */
static int queens_cols[16], queens_d1[32], queens_d2[32];
static int queens(int row, int n)
{
    int c, count = 0;
    if (row == n)
        return 1;
    for (c = 0; c < n; c++) {
        if (queens_cols[c] || queens_d1[row + c] || queens_d2[row - c + n])
            continue;
        queens_cols[c] = queens_d1[row + c] = queens_d2[row - c + n] = 1;
        count += queens(row + 1, n);
        queens_cols[c] = queens_d1[row + c] = queens_d2[row - c + n] = 0;
    }
    return count;
}

/* A scanner in the style of a compiler's lexer: table-driven character classes and number/identifier
   hashing over a source-like text. */
static long lex_test(int rounds)
{
    static const char src[] =
        "static int f(int a, long b) { while (a < 100 && b != 0x7f) { a += b * 3; b = b >> 1 | a; } "
        "return a + b - 12345; } /* comment */ char *name = \"str\"; x1 = y2 + z_3 * 42;\n";
    static unsigned char cls[256];
    long s = 0;
    int r, i;
    for (i = 0; i < 256; i++)
        cls[i] = (unsigned char)((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_' ? 1
                                 : i >= '0' && i <= '9' ? 2 : i == ' ' || i == '\n' ? 3 : 0);
    for (r = 0; r < rounds; r++) {
        const unsigned char *p = (const unsigned char *)src;
        int ntok = 0;
        while (*p) {
            switch (cls[*p]) {
            case 1: {
                unsigned h = 0;
                do
                    h = h * 31 + *p++;
                while (cls[*p] == 1 || cls[*p] == 2);
                s += h & 0xff;
                break;
            }
            case 2: {
                long v = 0;
                while (cls[*p] == 2)
                    v = v * 10 + (*p++ - '0');
                s += v;
                break;
            }
            case 3:
                p++;
                continue;
            default:
                if (*p == '"') {
                    p++;
                    while (*p != '"')
                        p++;
                }
                p++;
                break;
            }
            ntok++;
        }
        s += ntok + r;
    }
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
    printf("str %ld\n", str_test(20000));
    lap("str");
    printf("struct %ld\n", struct_test(10000, 3000));
    lap("struct");
    printf("hashtab %ld\n", ht_test(100));
    lap("hashtab");
    printf("interp %ld\n", interp_test(3000000));
    lap("interp");
    printf("nbody %ld\n", (long)(fp_test(1000000) * 1e4));
    lap("nbody");
    printf("mandel %d\n", mandel(600, 400, 200));
    lap("mandel");
    printf("crc %u\n", crc_test((unsigned char *)buf, sizeof buf, 3000));
    lap("crc");
    printf("tree %ld\n", tree_test(200000, 2000000));
    lap("tree");
    printf("array %ld\n", array_test(100000, 20));
    lap("array");
    printf("stencil %ld\n", stencil_test(512, 512, 200));
    lap("stencil");
    printf("queens %d\n", queens(0, 11));
    lap("queens");
    printf("lex %ld\n", lex_test(300000));
    lap("lex");
    return 0;
}
