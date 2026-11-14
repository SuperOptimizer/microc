/* POSIX threads for aarch64 Linux: clone for the threads, futexes for everything that waits. */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "thread.h"

#define LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define CAS(p, old, new) __sync_bool_compare_and_swap((p), (old), (new))

void __malloc_thread_exit(void); /* stdlib.c: the thread's cache of free blocks goes back */

#define DEFAULT_STACK 0x800000
#define DEFAULT_GUARD 0x1000
#define NCACHED 8

static volatile int nthreads = 1; /* threads that have not ended */

/* mappings of joined threads that had the default size, kept for the next thread */
static volatile int cache_lock;
static char *cached[NCACHED];
static int ncached;

static size_t round_up(size_t n, size_t to) { return (n + to - 1) & ~(to - 1); }

static size_t default_map_size(void)
{
    return round_up(DEFAULT_GUARD + DEFAULT_STACK + sizeof(struct __pthread) + __tls.bias + __tls.memsz + __tls.align,
                    4096);
}

static void release_map(struct __pthread *td)
{
    char *map = td->map;
    size_t size = td->map_size;
    if (size == default_map_size() && td->guard == DEFAULT_GUARD) {
        __lock(&cache_lock);
        if (ncached < NCACHED) {
            cached[ncached++] = map;
            map = 0;
        }
        __unlock(&cache_lock);
    }
    if (map)
        munmap(map, size);
}

/* ---- keys ---- */

static volatile int key_lock;
static unsigned char key_used[TSD_KEYS];
static unsigned key_gen[TSD_KEYS];
static void (*key_dtor[TSD_KEYS])(void *);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    unsigned k;
    __lock(&key_lock);
    for (k = 0; k < TSD_KEYS; k++)
        if (!key_used[k]) {
            key_used[k] = 1;
            key_gen[k]++; /* what older threads hold under this number is void */
            key_dtor[k] = destructor;
            break;
        }
    __unlock(&key_lock);
    if (k == TSD_KEYS)
        return EAGAIN;
    *key = k;
    return 0;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= TSD_KEYS || !key_used[key])
        return EINVAL;
    __lock(&key_lock);
    key_used[key] = 0;
    key_dtor[key] = 0;
    __unlock(&key_lock);
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    struct __pthread *td = __thread_self();
    if (key >= TSD_KEYS || !key_used[key])
        return EINVAL;
    td->tsd[key] = (void *)value;
    td->tsd_gen[key] = key_gen[key];
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    struct __pthread *td = __thread_self();
    if (key >= TSD_KEYS || td->tsd_gen[key] != key_gen[key])
        return 0;
    return td->tsd[key];
}

static void run_destructors(struct __pthread *td)
{
    int round, again = 1;
    unsigned k;
    for (round = 0; round < 4 && again; round++) {
        again = 0;
        for (k = 0; k < TSD_KEYS; k++) {
            void *v = td->tsd[k];
            void (*fn)(void *) = key_dtor[k];
            if (!v || td->tsd_gen[k] != key_gen[k] || !key_used[k])
                continue;
            td->tsd[k] = 0;
            if (fn) {
                fn(v);
                again = 1;
            }
        }
    }
}

/* ---- threads ---- */

void pthread_exit(void *result)
{
    struct __pthread *td = __thread_self();
    td->result = result;
    run_destructors(td);
    if (__atomic_fetch_sub(&nthreads, 1, __ATOMIC_SEQ_CST) == 1)
        exit(0); /* the last thread: the process ends as if main had returned */
    __malloc_thread_exit();
    if (td->map && !CAS(&td->state, T_JOINABLE, T_EXITED)) {
        /* detached: nobody will come for the stack, and the kernel must not write the thread id into
           what is about to be unmapped */
        __syscall(96, 0, 0, 0, 0, 0, 0);
        __unmap_exit(td->map, td->map_size);
    }
    for (;;)
        __syscall(93, 0, 0, 0, 0, 0, 0);
}

static int thread_start(void *p)
{
    struct __pthread *td = p;
    __thread_enter(td);
    pthread_exit(td->fn(td->arg));
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    size_t stack = attr && attr->__stacksize ? round_up(attr->__stacksize, 4096) : DEFAULT_STACK;
    size_t guard = attr ? round_up(attr->__guardsize, 4096) : DEFAULT_GUARD;
    size_t size = round_up(guard + stack + sizeof(struct __pthread) + __tls.bias + __tls.memsz + __tls.align, 4096);
    char *map = 0, *tp;
    struct __pthread *td;
    long r;

    __libc_threaded = 1;
    if (size == default_map_size() && guard == DEFAULT_GUARD) {
        __lock(&cache_lock);
        if (ncached)
            map = cached[--ncached];
        __unlock(&cache_lock);
    }
    if (!map) {
        map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | 0x20000 /* MAP_STACK */, -1, 0);
        if (map == MAP_FAILED)
            return EAGAIN;
        if (guard)
            mprotect(map, guard, PROT_NONE);
    }
    tp = (char *)((unsigned long)(map + size - __tls.bias - __tls.memsz) & ~(__tls.align - 1));
    td = (struct __pthread *)((unsigned long)(tp - sizeof(struct __pthread)) & ~(unsigned long)15);
    memset(td, 0, sizeof *td);
    memset(tp, 0, 16);
    __tls_fill(tp);
    td->map = map;
    td->map_size = size;
    td->guard = guard;
    td->stack_size = (size_t)((char *)td - map) - guard;
    td->fn = fn;
    td->arg = arg;
    td->state = attr && attr->__detached ? T_DETACHED : T_JOINABLE;
    td->tid = -1;
    __atomic_fetch_add(&nthreads, 1, __ATOMIC_SEQ_CST);
    *t = (pthread_t)td; /* before the thread runs: it may look at it */
    /* CLONE_VM | FS | FILES | SIGHAND | THREAD | SYSVSEM | SETTLS | PARENT_SETTID | CHILD_CLEARTID */
    r = __clone(thread_start, td, 0x3d0f00, td, (int *)&td->tid, tp, (int *)&td->tid);
    if (r < 0) {
        __atomic_fetch_sub(&nthreads, 1, __ATOMIC_SEQ_CST);
        release_map(td);
        return r == -ENOMEM ? EAGAIN : (int)-r;
    }
    return 0;
}

int pthread_join(pthread_t t, void **result)
{
    struct __pthread *td = (struct __pthread *)t;
    int tid;
    if (td == __thread_self())
        return EDEADLK;
    if (LOAD(&td->state) == T_DETACHED)
        return EINVAL;
    /* the kernel clears the id and wakes this futex once the thread is off its stack for good */
    while ((tid = LOAD(&td->tid)) != 0)
        __syscall(98, (long)&td->tid, FUTEX_WAIT, tid, 0, 0, 0);
    if (result)
        *result = td->result;
    if (td->map)
        release_map(td);
    return 0;
}

int pthread_detach(pthread_t t)
{
    struct __pthread *td = (struct __pthread *)t;
    if (CAS(&td->state, T_JOINABLE, T_DETACHED))
        return 0;
    if (LOAD(&td->state) == T_EXITED)
        return pthread_join(t, 0); /* already over: tidy up after it */
    return EINVAL;
}

pthread_t pthread_self(void) { return (pthread_t)__thread_self(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_kill(pthread_t t, int sig)
{
    struct __pthread *td = (struct __pthread *)t;
    int tid = LOAD(&td->tid);
    long r;
    if (tid <= 0)
        return ESRCH;
    r = __syscall(131, getpid(), tid, sig, 0, 0, 0); /* tgkill */
    return r < 0 ? (int)-r : 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    long r = __syscall(135, how, (long)set, (long)old, 8, 0, 0);
    return r < 0 ? (int)-r : 0;
}

int pthread_cancel(pthread_t t)
{
    (void)t;
    return ENOSYS;
}
int pthread_setcancelstate(int state, int *old)
{
    (void)state;
    if (old)
        *old = PTHREAD_CANCEL_ENABLE;
    return 0;
}
int pthread_setcanceltype(int type, int *old)
{
    (void)type;
    if (old)
        *old = PTHREAD_CANCEL_DEFERRED;
    return 0;
}
void pthread_testcancel(void) {}
int pthread_yield(void) { return sched_yield(); }
int pthread_getconcurrency(void) { return 0; }
int pthread_setconcurrency(int n)
{
    (void)n;
    return 0;
}
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
    (void)prepare;
    (void)parent;
    (void)child;
    return 0;
}

int pthread_setname_np(pthread_t t, const char *name)
{
    if (t != pthread_self())
        return ENOSYS;
    return __syscall(167, 15 /* PR_SET_NAME */, (long)name, 0, 0, 0, 0) < 0 ? EINVAL : 0;
}

int pthread_getname_np(pthread_t t, char *name, size_t n)
{
    char buf[16];
    if (t != pthread_self())
        return ENOSYS;
    if (__syscall(167, 16 /* PR_GET_NAME */, (long)buf, 0, 0, 0, 0) < 0)
        return EINVAL;
    if (strlen(buf) >= n)
        return ERANGE;
    strcpy(name, buf);
    return 0;
}

int pthread_setaffinity_np(pthread_t t, size_t size, const cpu_set_t *set)
{
    long r = __syscall(122, LOAD(&((struct __pthread *)t)->tid), (long)size, (long)set, 0, 0, 0);
    return r < 0 ? (int)-r : 0;
}

int pthread_getaffinity_np(pthread_t t, size_t size, cpu_set_t *set)
{
    long r = __syscall(123, LOAD(&((struct __pthread *)t)->tid), (long)size, (long)set, 0, 0, 0);
    if (r < 0)
        return (int)-r;
    if ((size_t)r < size)
        memset((char *)set + r, 0, size - (size_t)r);
    return 0;
}

/* ---- attributes ---- */

int pthread_attr_init(pthread_attr_t *a)
{
    memset(a, 0, sizeof *a);
    a->__stacksize = DEFAULT_STACK;
    a->__guardsize = DEFAULT_GUARD;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a)
{
    (void)a;
    return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n)
{
    if (n < PTHREAD_STACK_MIN)
        return EINVAL;
    a->__stacksize = n;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n)
{
    *n = a->__stacksize;
    return 0;
}
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n)
{
    a->__guardsize = n;
    return 0;
}
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n)
{
    *n = a->__guardsize;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    a->__detached = state;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    *state = a->__detached;
    return 0;
}
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n)
{
    /* the library places stacks itself; the size is honoured */
    a->__stackaddr = addr;
    return pthread_attr_setstacksize(a, n);
}
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n)
{
    *addr = a->__stackaddr;
    *n = a->__stacksize;
    return 0;
}
int pthread_attr_setscope(pthread_attr_t *a, int scope)
{
    (void)a;
    return scope == PTHREAD_SCOPE_SYSTEM ? 0 : ENOTSUP;
}
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit)
{
    (void)a;
    (void)inherit;
    return 0;
}
int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy)
{
    (void)a;
    return policy == SCHED_OTHER ? 0 : ENOTSUP;
}
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p)
{
    (void)a;
    (void)p;
    return 0;
}
int pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
    struct __pthread *td = (struct __pthread *)t;
    pthread_attr_init(a);
    if (td->map) {
        a->__stackaddr = td->map + td->guard;
        a->__stacksize = td->stack_size;
        a->__guardsize = td->guard;
    }
    a->__detached = LOAD(&td->state) == T_DETACHED;
    return 0;
}

/* ---- mutexes ---- */

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    a->__type = PTHREAD_MUTEX_NORMAL;
    return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    (void)a;
    return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (type < 0 || type > PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    a->__type = type;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    *type = a->__type;
    return 0;
}
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared)
{
    (void)a;
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    memset(m, 0, sizeof *m);
    m->__type = a ? a->__type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    void *me;
    if (m->__type == PTHREAD_MUTEX_NORMAL) {
        __lock(&m->__lock);
        return 0;
    }
    me = __thread_self();
    if (__atomic_load_n(&m->__owner, __ATOMIC_RELAXED) == me) {
        if (m->__type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
        if (m->__count == INT_MAX)
            return EAGAIN;
        m->__count++;
        return 0;
    }
    __lock(&m->__lock);
    __atomic_store_n(&m->__owner, me, __ATOMIC_RELAXED);
    m->__count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    void *me;
    if (m->__type == PTHREAD_MUTEX_NORMAL)
        return __trylock(&m->__lock) ? 0 : EBUSY;
    me = __thread_self();
    if (__atomic_load_n(&m->__owner, __ATOMIC_RELAXED) == me) {
        if (m->__type == PTHREAD_MUTEX_ERRORCHECK)
            return EBUSY;
        m->__count++;
        return 0;
    }
    if (!__trylock(&m->__lock))
        return EBUSY;
    __atomic_store_n(&m->__owner, me, __ATOMIC_RELAXED);
    m->__count = 1;
    return 0;
}

/* How long from now until the moment given on the clock; 0 if it has passed. */
static int time_left(clockid_t clock, const struct timespec *until, struct timespec *left)
{
    struct timespec now;
    clock_gettime(clock, &now);
    left->tv_sec = until->tv_sec - now.tv_sec;
    left->tv_nsec = until->tv_nsec - now.tv_nsec;
    if (left->tv_nsec < 0) {
        left->tv_nsec += 1000000000L;
        left->tv_sec--;
    }
    return left->tv_sec >= 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until)
{
    struct timespec left;
    int r;
    while ((r = pthread_mutex_trylock(m)) == EBUSY) {
        if (until->tv_nsec < 0 || until->tv_nsec >= 1000000000L)
            return EINVAL;
        if (!time_left(CLOCK_REALTIME, until, &left))
            return ETIMEDOUT;
        /* wait as a thread that lock would put to sleep does */
        if (__atomic_exchange_n(&m->__lock, 2, __ATOMIC_SEQ_CST) == 0) {
            if (m->__type != PTHREAD_MUTEX_NORMAL) {
                __atomic_store_n(&m->__owner, (void *)__thread_self(), __ATOMIC_RELAXED);
                m->__count = 1;
            }
            return 0;
        }
        __futex_wait(&m->__lock, 2, &left);
    }
    return r;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->__type != PTHREAD_MUTEX_NORMAL) {
        if (__atomic_load_n(&m->__owner, __ATOMIC_RELAXED) != (void *)__thread_self())
            return EPERM;
        if (--m->__count)
            return 0;
        __atomic_store_n(&m->__owner, (void *)0, __ATOMIC_RELAXED);
    }
    __unlock(&m->__lock);
    return 0;
}

/* ---- condition variables ----
   A waiter notes the sequence number, lets go of the mutex and sleeps while the
   number is still the same; a signal changes the number and wakes sleepers. */

int pthread_condattr_init(pthread_condattr_t *a)
{
    a->__clock = CLOCK_REALTIME;
    return 0;
}
int pthread_condattr_destroy(pthread_condattr_t *a)
{
    (void)a;
    return 0;
}
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
        return EINVAL;
    a->__clock = (int)clock;
    return 0;
}
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock)
{
    *clock = a->__clock;
    return 0;
}
int pthread_condattr_setpshared(pthread_condattr_t *a, int shared)
{
    (void)a;
    return shared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    memset(c, 0, sizeof *c);
    c->__clock = a ? a->__clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until)
{
    struct timespec left;
    int seq, r = 0, count = 0;
    void *owner = 0;
    if (until && (until->tv_nsec < 0 || until->tv_nsec >= 1000000000L))
        return EINVAL;
    __atomic_fetch_add(&c->__waiters, 1, __ATOMIC_SEQ_CST);
    seq = LOAD(&c->__seq);
    /* the mutex is let go of entirely, however often this thread holds it */
    if (m->__type != PTHREAD_MUTEX_NORMAL) {
        owner = m->__owner;
        count = m->__count;
        __atomic_store_n(&m->__owner, (void *)0, __ATOMIC_RELAXED);
    }
    __unlock(&m->__lock);
    if (!until) {
        __futex_wait(&c->__seq, seq, 0);
    } else if (!time_left(c->__clock, until, &left)) {
        r = ETIMEDOUT;
    } else if (__futex_wait(&c->__seq, seq, &left) == -ETIMEDOUT) {
        r = ETIMEDOUT;
    }
    __lock(&m->__lock);
    if (m->__type != PTHREAD_MUTEX_NORMAL) {
        __atomic_store_n(&m->__owner, owner, __ATOMIC_RELAXED);
        m->__count = count;
    }
    __atomic_fetch_sub(&c->__waiters, 1, __ATOMIC_SEQ_CST);
    return r;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return pthread_cond_timedwait(c, m, 0);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (LOAD(&c->__waiters)) {
        __atomic_fetch_add(&c->__seq, 1, __ATOMIC_SEQ_CST);
        __futex_wake(&c->__seq, 1);
    }
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (LOAD(&c->__waiters)) {
        __atomic_fetch_add(&c->__seq, 1, __ATOMIC_SEQ_CST);
        __futex_wake(&c->__seq, INT_MAX);
    }
    return 0;
}

/* ---- read-write locks, barriers: a mutex and a condition each ---- */

int pthread_rwlockattr_init(pthread_rwlockattr_t *a)
{
    a->__unused = 0;
    return 0;
}
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
    (void)a;
    memset(l, 0, sizeof *l);
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    (void)l;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->__m);
    while (l->__writer || l->__waiting_writers) /* writers first, or readers could starve them */
        pthread_cond_wait(&l->__c, &l->__m);
    l->__readers++;
    pthread_mutex_unlock(&l->__m);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    int r = EBUSY;
    pthread_mutex_lock(&l->__m);
    if (!l->__writer && !l->__waiting_writers) {
        l->__readers++;
        r = 0;
    }
    pthread_mutex_unlock(&l->__m);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->__m);
    l->__waiting_writers++;
    while (l->__writer || l->__readers)
        pthread_cond_wait(&l->__c, &l->__m);
    l->__waiting_writers--;
    l->__writer = 1;
    pthread_mutex_unlock(&l->__m);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    int r = EBUSY;
    pthread_mutex_lock(&l->__m);
    if (!l->__writer && !l->__readers) {
        l->__writer = 1;
        r = 0;
    }
    pthread_mutex_unlock(&l->__m);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->__m);
    if (l->__writer)
        l->__writer = 0;
    else if (l->__readers)
        l->__readers--;
    pthread_cond_broadcast(&l->__c);
    pthread_mutex_unlock(&l->__m);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
    (void)a;
    if (!count)
        return EINVAL;
    memset(b, 0, sizeof *b);
    b->__count = count;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b)
{
    (void)b;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    unsigned cycle;
    pthread_mutex_lock(&b->__m);
    if (++b->__waiting == b->__count) {
        b->__waiting = 0;
        b->__cycle++;
        pthread_cond_broadcast(&b->__c);
        pthread_mutex_unlock(&b->__m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    cycle = b->__cycle;
    while (cycle == b->__cycle)
        pthread_cond_wait(&b->__c, &b->__m);
    pthread_mutex_unlock(&b->__m);
    return 0;
}

/* ---- once: 0 not begun, 1 running, 3 running with threads waiting, 2 done ---- */

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    for (;;) {
        int c = LOAD(once);
        if (c == 2)
            return 0;
        if (c == 0) {
            if (!CAS(once, 0, 1))
                continue;
            fn();
            if (__atomic_exchange_n(once, 2, __ATOMIC_SEQ_CST) == 3)
                __futex_wake(once, INT_MAX);
            return 0;
        }
        if (c == 1 && !CAS(once, 1, 3))
            continue;
        __futex_wait(once, 3, 0);
    }
}

/* ---- spin locks ---- */

int pthread_spin_init(pthread_spinlock_t *s, int shared)
{
    (void)shared;
    STORE(s, 0);
    return 0;
}
int pthread_spin_destroy(pthread_spinlock_t *s)
{
    (void)s;
    return 0;
}
int pthread_spin_lock(pthread_spinlock_t *s)
{
    int k = 0;
    while (__atomic_exchange_n(s, 1, __ATOMIC_SEQ_CST)) {
        while (__atomic_load_n(s, __ATOMIC_RELAXED))
            if (++k % 1000 == 0)
                sched_yield(); /* the holder may have lost its processor */
    }
    return 0;
}
int pthread_spin_trylock(pthread_spinlock_t *s)
{
    return __atomic_exchange_n(s, 1, __ATOMIC_SEQ_CST) ? EBUSY : 0;
}
int pthread_spin_unlock(pthread_spinlock_t *s)
{
    STORE(s, 0);
    return 0;
}

/* ---- semaphores ---- */

int sem_init(sem_t *s, int shared, unsigned value)
{
    (void)shared;
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    s->__value = (int)value;
    s->__waiters = 0;
    return 0;
}

int sem_destroy(sem_t *s)
{
    (void)s;
    return 0;
}

int sem_trywait(sem_t *s)
{
    int v;
    while ((v = LOAD(&s->__value)) > 0)
        if (CAS(&s->__value, v, v - 1))
            return 0;
    errno = EAGAIN;
    return -1;
}

int sem_timedwait(sem_t *s, const struct timespec *until)
{
    struct timespec left;
    for (;;) {
        int v = LOAD(&s->__value);
        if (v > 0) {
            if (CAS(&s->__value, v, v - 1))
                return 0;
            continue;
        }
        if (until && (until->tv_nsec < 0 || until->tv_nsec >= 1000000000L)) {
            errno = EINVAL;
            return -1;
        }
        if (until && !time_left(CLOCK_REALTIME, until, &left)) {
            errno = ETIMEDOUT;
            return -1;
        }
        __atomic_fetch_add(&s->__waiters, 1, __ATOMIC_SEQ_CST);
        __futex_wait(&s->__value, 0, until ? &left : 0);
        __atomic_fetch_sub(&s->__waiters, 1, __ATOMIC_SEQ_CST);
    }
}

int sem_wait(sem_t *s) { return sem_timedwait(s, 0); }

int sem_post(sem_t *s)
{
    int v;
    do {
        v = LOAD(&s->__value);
        if (v == SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!CAS(&s->__value, v, v + 1));
    if (LOAD(&s->__waiters))
        __futex_wake(&s->__value, 1);
    return 0;
}

int sem_getvalue(sem_t *s, int *value)
{
    *value = LOAD(&s->__value);
    return 0;
}

/* ---- scheduling ---- */

int __sched_cpucount(size_t size, const cpu_set_t *set)
{
    const unsigned char *p = (const unsigned char *)set;
    size_t k;
    int n = 0;
    for (k = 0; k < size; k++) {
        unsigned b = p[k];
        for (; b; b &= b - 1)
            n++;
    }
    return n;
}

int sched_getcpu(void)
{
    unsigned cpu = 0;
    long r = __syscall(168, (long)&cpu, 0, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)cpu;
}

int sched_get_priority_max(int policy) { return policy == SCHED_FIFO || policy == SCHED_RR ? 99 : 0; }
int sched_get_priority_min(int policy) { return policy == SCHED_FIFO || policy == SCHED_RR ? 1 : 0; }
