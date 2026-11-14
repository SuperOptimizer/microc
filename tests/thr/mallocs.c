/* The allocator under eight threads: blocks of every size made, filled, checked, grown and
   freed, some of them by a thread other than the one that made them. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT 8
#define ROUNDS 6000
#define HELD 64

static atomic_long errors, handed;
/* blocks passed from thread to thread */
static void *_Atomic box[32];

static unsigned next_rand(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 8;
}

static void fill(unsigned char *p, size_t n, unsigned tag)
{
    size_t k;
    for (k = 0; k < n; k++)
        p[k] = (unsigned char)(tag + k);
}

static int check(const unsigned char *p, size_t n, unsigned tag)
{
    size_t k;
    for (k = 0; k < n; k++)
        if (p[k] != (unsigned char)(tag + k))
            return 0;
    return 1;
}

static void *run(void *arg)
{
    long id = (long)arg;
    unsigned seed = (unsigned)id * 7919u, r;
    struct { unsigned char *p; size_t n; unsigned tag; } held[HELD];
    int i, k;
    memset(held, 0, sizeof held);
    for (i = 0; i < ROUNDS; i++) {
        size_t n;
        k = (int)(next_rand(&seed) % HELD);
        r = next_rand(&seed) % 100;
        n = r < 70 ? next_rand(&seed) % 200 + 1 : r < 95 ? next_rand(&seed) % 5000 + 1 : next_rand(&seed) % 300000 + 1;
        if (held[k].p) {
            if (!check(held[k].p, held[k].n, held[k].tag))
                atomic_fetch_add(&errors, 1);
            if (r % 4 == 0) {
                /* grow or shrink it: the part that stays must be kept */
                size_t keep = n < held[k].n ? n : held[k].n;
                held[k].p = realloc(held[k].p, n);
                if (!held[k].p || !check(held[k].p, keep, held[k].tag))
                    atomic_fetch_add(&errors, 1);
                fill(held[k].p, n, held[k].tag);
                held[k].n = n;
                continue;
            }
            if (r % 4 == 1) {
                /* leave it for whoever comes by, and free what was left there */
                unsigned char *mine = held[k].p, *theirs;
                mine[0] = 0xa5;
                theirs = atomic_exchange(&box[next_rand(&seed) % 32], mine);
                if (theirs) {
                    if (theirs[0] != 0xa5)
                        atomic_fetch_add(&errors, 1);
                    atomic_fetch_add(&handed, 1);
                    free(theirs);
                }
            } else {
                free(held[k].p);
            }
            held[k].p = 0;
        }
        if (r % 8 == 7) {
            unsigned char *z = calloc(n, 1);
            size_t j;
            for (j = 0; z && j < n; j++)
                if (z[j])
                    break;
            if (!z || j < n)
                atomic_fetch_add(&errors, 1);
            held[k].p = z;
        } else {
            held[k].p = malloc(n);
            if (!held[k].p)
                atomic_fetch_add(&errors, 1);
        }
        held[k].n = n;
        held[k].tag = seed;
        fill(held[k].p, n, held[k].tag);
    }
    for (k = 0; k < HELD; k++) {
        if (held[k].p && !check(held[k].p, held[k].n, held[k].tag))
            atomic_fetch_add(&errors, 1);
        free(held[k].p);
    }
    return 0;
}

int main(void)
{
    pthread_t t[NT];
    long i, round;
    for (round = 0; round < 3; round++) {
        for (i = 0; i < NT; i++)
            pthread_create(&t[i], 0, run, (void *)(round * NT + i + 1));
        for (i = 0; i < NT; i++)
            pthread_join(t[i], 0);
    }
    for (i = 0; i < 32; i++)
        free(box[i]);
    printf("errors %ld, handed over %s\n", atomic_load(&errors), atomic_load(&handed) > 1000 ? "many" : "FEW");
    return 0;
}
