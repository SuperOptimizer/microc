/* pthread_once from many threads at once; keys with destructors; what threads return. */
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_once_t once = PTHREAD_ONCE_INIT, once2 = PTHREAD_ONCE_INIT;
static atomic_int inits, seen_before_done;
static int ready;
static pthread_key_t key, key2;
static atomic_int destroyed, destroyed_sum, chained;

static void init(void)
{
    int k;
    volatile int spin = 0;
    atomic_fetch_add(&inits, 1);
    for (k = 0; k < 200000; k++)
        spin += k; /* long enough for the others to arrive */
    ready = 1;
}

static void init2(void) { atomic_fetch_add(&inits, 100); }

static void destroy(void *p)
{
    atomic_fetch_add(&destroyed, 1);
    atomic_fetch_add(&destroyed_sum, *(int *)p);
    free(p);
}

static void destroy2(void *p)
{
    /* a destructor that sets a value again is called again */
    if ((long)p < 3) {
        pthread_setspecific(key2, (void *)((long)p + 1));
        atomic_fetch_add(&chained, 1);
    }
}

static void *run(void *arg)
{
    long id = (long)arg;
    int *v = malloc(sizeof *v), k;
    pthread_once(&once, init);
    if (!ready)
        atomic_fetch_add(&seen_before_done, 1);
    pthread_once(&once2, init2);
    if (pthread_getspecific(key) != 0)
        return (void *)-1L;
    *v = (int)id;
    pthread_setspecific(key, v);
    if (id == 1)
        pthread_setspecific(key2, (void *)1L);
    for (k = 0; k < 1000; k++)
        if (*(int *)pthread_getspecific(key) != id)
            return (void *)-2L;
    if (id % 2)
        pthread_exit((void *)(id * 10));
    return (void *)(id * 10);
}

static sem_t done;
static void *detached(void *arg)
{
    int *v = malloc(sizeof *v);
    *v = 1000;
    pthread_setspecific(key, v);
    sem_post((sem_t *)arg);
    return 0;
}

int main(void)
{
    pthread_t t[8], d;
    pthread_attr_t attr;
    void *r;
    long i, sum = 0;
    pthread_key_t k3;
    pthread_key_create(&key, destroy);
    pthread_key_create(&key2, destroy2);
    for (i = 0; i < 8; i++)
        pthread_create(&t[i], 0, run, (void *)(i + 1));
    for (i = 0; i < 8; i++) {
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    printf("once: %d, seen unfinished %d\n", atomic_load(&inits), atomic_load(&seen_before_done));
    printf("returned %ld\n", sum);
    printf("destructors %d, sum %d, chained %d\n", atomic_load(&destroyed), atomic_load(&destroyed_sum), atomic_load(&chained));
    printf("main's value %s\n", pthread_getspecific(key) ? "SET" : "unset");

    /* detached threads tidy up after themselves */
    sem_init(&done, 0, 0);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (i = 0; i < 4; i++)
        pthread_create(&d, &attr, detached, &done);
    pthread_attr_destroy(&attr);
    pthread_create(&d, 0, detached, &done);
    pthread_detach(d);
    for (i = 0; i < 5; i++)
        sem_wait(&done);
    while (atomic_load(&destroyed) < 13)
        sched_yield();
    printf("detached: destructors %d, sum %d\n", atomic_load(&destroyed), atomic_load(&destroyed_sum));

    /* a key number used again starts empty */
    pthread_setspecific(key2, (void *)7L);
    pthread_key_delete(key2);
    pthread_key_create(&k3, 0);
    printf("key reused %d, value %s\n", k3 == key2, pthread_getspecific(k3) ? "STALE" : "unset");
    printf("self %d %d\n", pthread_equal(pthread_self(), pthread_self()), pthread_equal(pthread_self(), t[0]));
    return 0;
}
