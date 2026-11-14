/* Thread-local objects: initialised and zero, static and extern, arrays, records, alignment,
   and a copy for every thread. */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tls.h"

_Thread_local int shared_counter = 100;         /* defined here, used in tls_2.c */
__thread long zero_long;                        /* zeroed, defined here */
static _Thread_local int init_int = 42;
static thread_local char name[16] = "initial";
static __thread double dbl = 2.5;
static _Thread_local struct pt { int x, y; char tag[8]; } point = {3, 4, "pt"};
static thread_local int zarr[100];
static _Thread_local __attribute__((aligned(64))) char wide[64];
static __thread int after_wide = 7;
static _Thread_local __attribute__((aligned(16))) struct { long a, b; } pair = {11, 22};
static thread_local int *self_ptr;
static _Thread_local const char *str = "text";

static int *addrs[5];

static int bump(void)
{
    static _Thread_local int calls;
    static __thread int from_ten = 10;
    calls++;
    return calls + from_ten++;
}

static void *run(void *arg)
{
    long id = (long)arg, k, sum = 0;
    int ok = 1;
    /* a fresh copy: the initial values, whatever the others did */
    ok &= init_int == 42 && strcmp(name, "initial") == 0 && dbl == 2.5 && point.x == 3 && point.y == 4;
    ok &= strcmp(point.tag, "pt") == 0 && zero_long == 0 && shared_counter == 100 && after_wide == 7;
    ok &= pair.a == 11 && pair.b == 22 && self_ptr == 0 && strcmp(str, "text") == 0 && other_value == 1000;
    for (k = 0; k < 100; k++)
        ok &= zarr[k] == 0;
    for (k = 0; k < 64; k++)
        ok &= wide[k] == 0;
    ok &= ((uintptr_t)wide & 63) == 0 && ((uintptr_t)&pair & 15) == 0 && ((uintptr_t)&other_aligned & 63) == 0;
    init_int += (int)id;
    sprintf(name, "thread%ld", id);
    dbl *= (double)id;
    point.x = (int)id;
    zero_long = id * 1000;
    self_ptr = &init_int;
    addrs[id] = &init_int;
    for (k = 0; k < 100; k++)
        zarr[k] = (int)(id * k);
    memset(wide, (int)id, sizeof wide);
    for (k = 0; k < 1000; k++) {
        shared_counter++;
        other_add(2);
        sum += bump();
    }
    for (k = 0; k < 100; k++)
        ok &= zarr[k] == id * k;
    for (k = 0; k < 64; k++)
        ok &= wide[k] == id;
    ok &= init_int == 42 + id && *self_ptr == 42 + id && dbl == 2.5 * (double)id && zero_long == id * 1000;
    ok &= shared_counter == 1100 && other_get() == 3000 && *other_addr() == 3000 && other_counter() == 1100;
    ok &= sum == 1000 * 1001 / 2 + 1000 * 10 + 999 * 1000 / 2 && after_wide == 7;
    return (void *)(long)(ok ? (long)strlen(name) : -1);
}

int main(void)
{
    pthread_t t[4];
    void *r;
    long i, j;
    int distinct = 1;
    for (i = 0; i < 4; i++)
        pthread_create(&t[i], 0, run, (void *)(i + 1));
    for (i = 0; i < 4; i++) {
        pthread_join(t[i], &r);
        printf("thread %ld: %ld\n", i + 1, (long)r);
    }
    addrs[0] = &init_int;
    for (i = 0; i < 5; i++)
        for (j = 0; j < i; j++)
            distinct &= addrs[i] != addrs[j];
    printf("main: %d %s %g %d %ld %d %d\n", init_int, name, dbl, point.x, zero_long, shared_counter, other_get());
    printf("distinct %d aligned %d %d\n", distinct, (int)((uintptr_t)wide & 63), (int)((uintptr_t)&other_aligned & 63));
    printf("bump %d %d\n", bump(), bump());
    return 0;
}
