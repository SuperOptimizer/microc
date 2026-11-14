#ifndef _PTHREAD_H
#define _PTHREAD_H
#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

/* pthread_t (sys/types.h) is the address of the thread's descriptor. */
typedef struct {
    size_t __stacksize, __guardsize;
    void *__stackaddr;
    int __detached;
} pthread_attr_t;
typedef struct {
    volatile int __lock; /* 0 free, 1 held, 2 held with threads waiting */
    int __type;
    int __count;
    int __pad;
    void *__owner;
} pthread_mutex_t;
typedef struct {
    int __type;
} pthread_mutexattr_t;
typedef struct {
    volatile int __seq, __waiters;
    int __clock;
} pthread_cond_t;
typedef struct {
    int __clock;
} pthread_condattr_t;
typedef struct {
    pthread_mutex_t __m;
    pthread_cond_t __c;
    int __readers, __writer, __waiting_writers;
} pthread_rwlock_t;
typedef struct {
    int __unused;
} pthread_rwlockattr_t;
typedef struct {
    pthread_mutex_t __m;
    pthread_cond_t __c;
    unsigned __count, __waiting, __cycle;
} pthread_barrier_t;
typedef struct {
    int __unused;
} pthread_barrierattr_t;
typedef volatile int pthread_once_t;
typedef volatile int pthread_spinlock_t;
typedef unsigned pthread_key_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_FAST_NP PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE_NP PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_ERRORCHECK_NP PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_INITIALIZER {0, 0, 0, 0, 0}
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP {0, 1, 0, 0, 0}
#define PTHREAD_COND_INITIALIZER {0, 0, 0}
#define PTHREAD_RWLOCK_INITIALIZER {{0, 0, 0, 0, 0}, {0, 0, 0}, 0, 0, 0}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)-1)
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_SCOPE_SYSTEM 0
#define PTHREAD_INHERIT_SCHED 0
#define PTHREAD_EXPLICIT_SCHED 1
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define PTHREAD_KEYS_MAX 128
#define PTHREAD_STACK_MIN 16384

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **result);
int pthread_detach(pthread_t t);
void pthread_exit(void *result) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_kill(pthread_t t, int sig);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
int pthread_cancel(pthread_t t);
int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);
int pthread_yield(void);
int pthread_setname_np(pthread_t t, const char *name);
int pthread_getname_np(pthread_t t, char *name, size_t n);
int pthread_getattr_np(pthread_t t, pthread_attr_t *attr);
int pthread_setaffinity_np(pthread_t t, size_t size, const cpu_set_t *set);
int pthread_getaffinity_np(pthread_t t, size_t size, cpu_set_t *set);
int pthread_getconcurrency(void);
int pthread_setconcurrency(int n);
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t n);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *n);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n);
int pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *n);
int pthread_attr_setscope(pthread_attr_t *a, int scope);
int pthread_attr_setinheritsched(pthread_attr_t *a, int inherit);
int pthread_attr_setschedpolicy(pthread_attr_t *a, int policy);
int pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock);
int pthread_condattr_setpshared(pthread_condattr_t *a, int shared);

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

int pthread_once(pthread_once_t *once, void (*fn)(void));
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_spin_init(pthread_spinlock_t *s, int shared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);

/* cancellation is not provided, so the handlers only run when popped with a nonzero argument */
#define pthread_cleanup_push(fn, arg) do { void (*__cleanup_fn)(void *) = (fn); void *__cleanup_arg = (arg);
#define pthread_cleanup_pop(run) if (run) __cleanup_fn(__cleanup_arg); } while (0)
#endif
