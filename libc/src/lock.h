/* Locks around the library's own shared state. Only the Linux library has
   threads; elsewhere, and in a program that never made a second thread,
   they cost nothing or one test. */
#ifndef MCC_LIBC_LOCK_H
#define MCC_LIBC_LOCK_H
#if defined(__linux__) && !defined(__MCC_POSIX__)
#define __MCC_THREADS 1
extern int __libc_threaded;
void __lock(volatile int *l);
void __unlock(volatile int *l);
/* a lock its holder may take again */
struct __rlock {
    volatile int lock;
    int count;
    void *owner;
};
void __rlock_take(struct __rlock *l);
void __rlock_drop(struct __rlock *l);
#define LOCK(l) do { if (__libc_threaded) __lock(&(l)); } while (0)
#define UNLOCK(l) do { if (__libc_threaded) __unlock(&(l)); } while (0)
#define RLOCK(l) do { if (__libc_threaded) __rlock_take(&(l)); } while (0)
#define RUNLOCK(l) do { if (__libc_threaded) __rlock_drop(&(l)); } while (0)
#define LOCK_DECL(name) static volatile int name
extern volatile int __env_lock;
#else
#define LOCK(l) ((void)0)
#define UNLOCK(l) ((void)0)
#define RLOCK(l) ((void)0)
#define RUNLOCK(l) ((void)0)
#define LOCK_DECL(name) typedef int name##_unused
#endif
#endif
