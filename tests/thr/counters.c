/* Counters shared by eight threads: with atomics and with locks they must come out exact. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>

#define NT 8
#define N 40000

static atomic_int a_int;
static _Atomic long a_op;         /* ++ and += */
static _Atomic uint64_t a_relaxed;
static _Atomic unsigned char a_byte; /* wraps */
static _Atomic unsigned short a_short;
static atomic_long a_max;
static _Atomic int a_sub = NT * N;
static atomic_uint a_bits;
static _Atomic double a_fp;
static long sync_count;
static long m_count, s_count, rec_count;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rec;
static pthread_spinlock_t spin;
static struct { _Atomic int next; int done[NT * 64]; } job;
static atomic_flag flag = ATOMIC_FLAG_INIT;
static long flag_count;

static void *run(void *arg)
{
    long id = (long)arg, i, v;
    for (i = 0; i < N; i++) {
        atomic_fetch_add(&a_int, 1);
        a_op++;
        a_op += 2;
        atomic_fetch_add_explicit(&a_relaxed, 3, memory_order_relaxed);
        a_byte++;
        a_short += 3;
        a_sub--;
        __sync_fetch_and_add(&sync_count, 1);
        /* a running maximum */
        v = atomic_load(&a_max);
        while (v < id * N + i && !atomic_compare_exchange_weak(&a_max, &v, id * N + i)) {}
        pthread_mutex_lock(&mu);
        m_count++;
        pthread_mutex_unlock(&mu);
        if (i % 4 == 0) {
            pthread_spin_lock(&spin);
            s_count++;
            pthread_spin_unlock(&spin);
            pthread_mutex_lock(&rec);
            pthread_mutex_lock(&rec);
            rec_count++;
            pthread_mutex_unlock(&rec);
            pthread_mutex_unlock(&rec);
            while (atomic_flag_test_and_set(&flag)) {}
            flag_count++;
            atomic_flag_clear(&flag);
            a_fp += 0.5;
        }
    }
    atomic_fetch_or(&a_bits, 1u << id);
    /* work handed out by a shared index, as a parallel loop does */
    for (;;) {
        int k = atomic_fetch_add(&job.next, 1);
        if (k >= NT * 64)
            break;
        job.done[k]++;
    }
    return 0;
}

int main(void)
{
    pthread_t t[NT];
    pthread_mutexattr_t ma;
    long i, bad = 0;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec, &ma);
    pthread_spin_init(&spin, 0);
    for (i = 0; i < NT; i++)
        pthread_create(&t[i], 0, run, (void *)i);
    for (i = 0; i < NT; i++)
        pthread_join(t[i], 0);
    for (i = 0; i < NT * 64; i++)
        bad += job.done[i] != 1;
    printf("int %d op %ld relaxed %lu\n", a_int, a_op, (unsigned long)a_relaxed);
    printf("byte %d short %d sub %d sync %ld\n", a_byte, a_short, a_sub, sync_count);
    printf("max %ld bits %x fp %g\n", atomic_load(&a_max), atomic_load(&a_bits), a_fp);
    printf("mutex %ld spin %ld recursive %ld flag %ld\n", m_count, s_count, rec_count, flag_count);
    printf("jobs %d bad %ld\n", job.next >= NT * 64, bad);
    return 0;
}
