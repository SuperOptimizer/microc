/* The ways a large threaded program of ours spells its atomics, thread-local data and threads. */
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef struct { _Atomic int next, fail; _Atomic long long nval, hist[14]; _Atomic size_t over; _Atomic(uint8_t *) *tc, *ic;
    _Atomic uint32_t *needm; pthread_mutex_t mu; pthread_cond_t cv; _Atomic uint32_t rej[4][3][2]; atomic_uint_fast64_t f; } job;
_Thread_local int jpar_last_own = 0;
static __thread char names[4][64];
static _Atomic uint8_t *left;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static void mk(void) { pthread_key_create(&key, free); }
static void *work(void *a)
{
    job *J = a; int i;
    static _Thread_local struct { int n; double d[8]; } scratch;
    pthread_once(&once, mk);
    pthread_setspecific(key, malloc(10));
    for (;;) {
        i = atomic_fetch_add(&J->next, 1);
        if (i >= 1000) break;
        J->nval += i; J->hist[i % 14]++; J->over++;
        atomic_fetch_add_explicit(&J->rej[i % 4][i % 3][i % 2], 1, memory_order_relaxed);
        atomic_fetch_or(&J->needm[i % 8], 1u << (i % 32));
        __atomic_fetch_add(&J->f, 1, __ATOMIC_RELAXED);
        scratch.n++; scratch.d[i % 8] += 1; jpar_last_own = i; names[i % 4][0] = (char)i;
        if (J->fail) break;
    }
    return pthread_getspecific(key);
}
int main(void)
{
    job J = {0}; pthread_t t[4]; pthread_attr_t at; cpu_set_t set; int i, e = 0; long long s = 0;
    const _Atomic uint32_t *h;
    _Atomic long long *cnt = calloc(4, sizeof *cnt);
    atomic_init(&J.next, 0);
    J.needm = calloc(8, sizeof *J.needm); J.tc = calloc(4, sizeof *J.tc);
    pthread_mutex_init(&J.mu, NULL); pthread_cond_init(&J.cv, NULL);
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) printf("cpus %d\n", CPU_COUNT(&set) > 0);
    pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);
    for (i = 0; i < 4; i++) pthread_create(&t[i], &at, work, &J);
    for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
    pthread_attr_destroy(&at);
    h = J.needm;
    for (i = 0; i < 14; i++) s += J.hist[i];
    if (left == (uint8_t *)-2) e = 1;
    atomic_compare_exchange_strong(&J.tc[0], &(uint8_t *){0}, (uint8_t *)&J);
    __atomic_compare_exchange_n(&cnt[0], &(long long){0}, 5, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    printf("%lld %lld %zu %llu %x %d %lld %d\n", (long long)J.nval, s, (size_t)J.over, (unsigned long long)atomic_load(&J.f), h[0], J.tc[0] == (uint8_t *)&J, cnt[0], e);
    atomic_thread_fence(memory_order_seq_cst); sched_yield();
    return 0;
}
