/* A pthread.h for building third-party libraries for a libc without threads (mkulibs.sh, THREADS=0):
   locks do nothing, threads cannot be created. The names are private to each object file. */
#ifndef _ULIBS_NOTHREADS_PTHREAD_H
#define _ULIBS_NOTHREADS_PTHREAD_H
#include <errno.h>
typedef int pthread_t, pthread_attr_t, pthread_mutex_t, pthread_mutexattr_t, pthread_cond_t, pthread_condattr_t,
    pthread_once_t, pthread_barrier_t, pthread_barrierattr_t, pthread_key_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_COND_INITIALIZER 0
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define __NT static __inline__ __attribute__((unused)) int
__NT pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { *m = 0; return 0; }
__NT pthread_mutex_destroy(pthread_mutex_t *m) { return 0; }
__NT pthread_mutex_lock(pthread_mutex_t *m) { return 0; }
__NT pthread_mutex_trylock(pthread_mutex_t *m) { return 0; }
__NT pthread_mutex_unlock(pthread_mutex_t *m) { return 0; }
__NT pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { *c = 0; return 0; }
__NT pthread_cond_destroy(pthread_cond_t *c) { return 0; }
__NT pthread_cond_signal(pthread_cond_t *c) { return 0; }
__NT pthread_cond_broadcast(pthread_cond_t *c) { return 0; }
__NT pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return EDEADLK; }
__NT pthread_attr_init(pthread_attr_t *a) { *a = 0; return 0; }
__NT pthread_attr_destroy(pthread_attr_t *a) { return 0; }
__NT pthread_attr_setdetachstate(pthread_attr_t *a, int s) { return 0; }
__NT pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) { return EAGAIN; }
__NT pthread_join(pthread_t t, void **ret) { return ESRCH; }
__NT pthread_once(pthread_once_t *o, void (*fn)(void)) { if (!*o) { *o = 1; fn(); } return 0; }
__NT pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void)) { return 0; }
__NT pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned n) { *b = 0; return 0; }
__NT pthread_barrier_destroy(pthread_barrier_t *b) { return 0; }
__NT pthread_barrier_wait(pthread_barrier_t *b) { return PTHREAD_BARRIER_SERIAL_THREAD; }
#undef __NT
#endif
