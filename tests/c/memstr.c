/* memcpy, memmove, memset, memcmp, strlen, strcmp at every small size
   and alignment, checked against byte-at-a-time references. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char src[256], dst[256], ref[256];
static unsigned seed = 12345;

static unsigned rnd(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 16 & 0x7fff;
}

static void fill(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        src[i] = (unsigned char)(rnd() | 1);
        dst[i] = ref[i] = (unsigned char)i;
    }
}

static int cmpint(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

struct rec {
    char pad[3];
    int key;
    char tail[5];
};

static int cmprec(const void *a, const void *b)
{
    return ((const struct rec *)a)->key - ((const struct rec *)b)->key;
}

int main(void)
{
    int n, a, b, i, bad = 0;
    long sum = 0;
    for (n = 0; n < 100; n++)
        for (a = 0; a < 9; a++)
            for (b = 0; b < 9; b += 2) {
                fill();
                memcpy(dst + a, src + b, (size_t)n);
                for (i = 0; i < n; i++)
                    ref[a + i] = src[b + i];
                if (memcmp(dst, ref, 256))
                    bad++;
                fill();
                memset(dst + a, n + b, (size_t)n);
                for (i = 0; i < n; i++)
                    ref[a + i] = (unsigned char)(n + b);
                for (i = 0; i < 256; i++)
                    if (dst[i] != ref[i])
                        bad++;
                /* overlapping moves in both directions */
                fill();
                memcpy(dst, src, 256);
                memcpy(ref, src, 256);
                memmove(dst + a, dst + b, (size_t)n);
                for (i = 0; i < n; i++)
                    ref[a + i] = src[b + i];
                for (i = 0; i < 256; i++)
                    if (dst[i] != ref[i])
                        bad++;
            }
    printf("copy/set/move mismatches: %d\n", bad);
    bad = 0;
    for (n = 0; n < 60; n++)
        for (a = 0; a < 9; a++) {
            char *p = (char *)dst + a, *q = (char *)ref + (a & 8);
            fill();
            memcpy(p, src, (size_t)n);
            p[n] = 0;
            if (strlen(p) != (size_t)n)
                bad++;
            memcpy(q, p, (size_t)n + 1);
            if (strcmp(p, q) != 0 || memcmp(p, q, (size_t)n + 1) != 0)
                bad++;
            if (n) {
                i = (int)(rnd() % (unsigned)n);
                q[i] = (char)((unsigned char)q[i] + 1);
                if ((strcmp(p, q) < 0) != ((unsigned char)p[i] < (unsigned char)q[i]))
                    bad++;
                if ((memcmp(p, q, (size_t)n) < 0) != ((unsigned char)p[i] < (unsigned char)q[i]))
                    bad++;
                q[i] = 0;
                if (strcmp(p, q) <= 0 || strcmp(q, p) >= 0)
                    bad++;
            }
        }
    printf("strlen/strcmp/memcmp mismatches: %d\n", bad);
    {
        static int v[500];
        static struct rec r[200];
        for (i = 0; i < 500; i++)
            v[i] = (int)rnd() - 16000;
        qsort(v, 500, sizeof v[0], cmpint);
        bad = 0;
        for (i = 1; i < 500; i++) {
            if (v[i - 1] > v[i])
                bad++;
            sum += (long)v[i] * i;
        }
        for (i = 0; i < 200; i++) {
            r[i].key = (int)rnd();
            r[i].pad[0] = (char)r[i].key;
            r[i].tail[4] = (char)(r[i].key >> 3);
        }
        qsort(r, 200, sizeof r[0], cmprec);
        for (i = 0; i < 200; i++) {
            if (i && r[i - 1].key > r[i].key)
                bad++;
            if (r[i].pad[0] != (char)r[i].key || r[i].tail[4] != (char)(r[i].key >> 3))
                bad++;
        }
        printf("qsort errors: %d sum %ld\n", bad, sum);
    }
    return 0;
}
