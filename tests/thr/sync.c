/* The other ways to wait: read-write locks, semaphores, barriers, the kinds of mutex,
   and what the scheduler reports. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define NT 6

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static long table[16], rw_errors;
static atomic_int readers_in, max_readers;
static sem_t slots, turn;
static atomic_int in_slots, max_in_slots;
static pthread_barrier_t barrier;
static atomic_int phase[3], serial, barrier_errors;
static long sem_count;

static void *run(void *arg)
{
    long id = (long)arg;
    int i, k, r;
    for (i = 0; i < 2000; i++) {
        if (i % 8 == (int)id) {
            pthread_rwlock_wrlock(&rw);
            if (atomic_load(&readers_in))
                rw_errors++;
            for (k = 0; k < 16; k++)
                table[k]++;
            pthread_rwlock_unlock(&rw);
        } else {
            pthread_rwlock_rdlock(&rw);
            r = atomic_fetch_add(&readers_in, 1) + 1;
            k = atomic_load(&max_readers);
            while (k < r && !atomic_compare_exchange_weak(&max_readers, &k, r)) {}
            for (k = 1; k < 16; k++)
                if (table[k] != table[0])
                    atomic_fetch_add(&barrier_errors, 1);
            atomic_fetch_sub(&readers_in, 1);
            pthread_rwlock_unlock(&rw);
        }
        /* at most two at a time past this point */
        sem_wait(&slots);
        r = atomic_fetch_add(&in_slots, 1) + 1;
        k = atomic_load(&max_in_slots);
        while (k < r && !atomic_compare_exchange_weak(&max_in_slots, &k, r)) {}
        atomic_fetch_sub(&in_slots, 1);
        sem_post(&slots);
        /* a semaphore of one is a lock */
        sem_wait(&turn);
        sem_count++;
        sem_post(&turn);
    }
    /* everybody finishes a phase before anybody starts the next */
    for (i = 0; i < 3; i++) {
        atomic_fetch_add(&phase[i], 1);
        r = pthread_barrier_wait(&barrier);
        if (r == PTHREAD_BARRIER_SERIAL_THREAD)
            atomic_fetch_add(&serial, 1);
        if (atomic_load(&phase[i]) != NT)
            atomic_fetch_add(&barrier_errors, 1);
    }
    return 0;
}

static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static void *try_it(void *arg)
{
    struct timespec ts;
    int r1 = pthread_mutex_trylock(&held), r2;
    (void)arg;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 20 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    r2 = pthread_mutex_timedlock(&held, &ts);
    return (void *)(long)(r1 == EBUSY && r2 == ETIMEDOUT);
}

int main(void)
{
    pthread_t t[NT];
    pthread_mutexattr_t ma;
    pthread_mutex_t m;
    cpu_set_t set;
    sigset_t mask, old;
    void *r;
    long i, n;
    int v;
    sem_init(&slots, 0, 2);
    sem_init(&turn, 0, 1);
    pthread_barrier_init(&barrier, 0, NT);
    for (i = 0; i < NT; i++)
        pthread_create(&t[i], 0, run, (void *)i);
    for (i = 0; i < NT; i++)
        pthread_join(t[i], 0);
    printf("rwlock: table %ld, errors %ld, readers together %s\n", table[15], rw_errors,
           atomic_load(&max_readers) >= 1 ? "yes" : "NO");
    sem_getvalue(&slots, &v);
    printf("semaphore: at most %d inside, value %d, count %ld\n", atomic_load(&max_in_slots) <= 2 ? 2 : 99, v, sem_count);
    printf("barrier: serial %d, errors %d\n", atomic_load(&serial), atomic_load(&barrier_errors));
    printf("trywait %d", sem_trywait(&turn));
    printf(" %d %d\n", sem_trywait(&turn), errno == EAGAIN);

    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m, &ma);
    printf("errorcheck: %d", pthread_mutex_lock(&m));
    printf(" %d", pthread_mutex_lock(&m) == EDEADLK);
    printf(" %d", pthread_mutex_unlock(&m));
    printf(" %d\n", pthread_mutex_unlock(&m) == EPERM);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &ma);
    printf("recursive: %d %d %d", pthread_mutex_lock(&m), pthread_mutex_trylock(&m), pthread_mutex_lock(&m));
    printf(" %d %d %d", pthread_mutex_unlock(&m), pthread_mutex_unlock(&m), pthread_mutex_unlock(&m));
    printf(" %d\n", pthread_mutex_unlock(&m) == EPERM);
    pthread_mutex_lock(&held);
    pthread_create(&t[0], 0, try_it, 0);
    pthread_join(t[0], &r);
    pthread_mutex_unlock(&held);
    printf("trylock and timedlock on a held mutex: %ld, then %d\n", (long)r, pthread_mutex_trylock(&held));

    n = sysconf(_SC_NPROCESSORS_ONLN);
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    printf("processors: %s, %s\n", n >= 2 ? "several" : "ONE", CPU_COUNT(&set) == n && CPU_ISSET(0, &set) ? "as the mask says" : "MASK DIFFERS");
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, 0);
    pthread_sigmask(SIG_BLOCK, 0, &old);
    printf("signal mask: %d %d\n", sigismember(&old, SIGUSR1), sigismember(&old, SIGUSR2));
    printf("yield %d\n", sched_yield());
    return 0;
}
