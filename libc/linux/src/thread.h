/* Threads inside the Linux library: what sys.c (always linked) and pthread.c share. */
#ifndef MCC_THREAD_H
#define MCC_THREAD_H
#include <stddef.h>

#define TSD_KEYS 128

/* One per thread. A made thread has it in the mapping that holds its stack:
       guard | stack ... | this | 16 bytes the thread pointer points at | the thread's copy of the TLS image */
struct __pthread {
    volatile int tid;   /* set by clone, cleared (and its waiters woken) by the kernel when the thread is gone */
    volatile int state; /* T_JOINABLE, T_DETACHED, T_EXITED */
    void *result;
    void *(*fn)(void *);
    void *arg;
    char *map; /* 0 for the first thread */
    size_t map_size, guard, stack_size;
    void *tsd[TSD_KEYS];
    unsigned tsd_gen[TSD_KEYS];
};
enum { T_JOINABLE, T_DETACHED, T_EXITED };

/* The image every thread gets a copy of, from the PT_TLS header. */
struct __tls_image {
    const char *data;
    size_t filesz, memsz, align;
    size_t bias; /* from the thread pointer to the copy */
};
extern struct __tls_image __tls;

extern int __libc_threaded; /* a second thread has been made */
struct __pthread *__thread_self(void);
void __thread_enter(struct __pthread *td); /* first thing a new thread does */
void __tls_fill(char *tp);                 /* a fresh copy of the image at thread pointer tp */

long __syscall(long number, long a, long b, long c, long d, long e, long f);
#ifdef __x86_64__
/* The sources name system calls by their aarch64 numbers; this turns each into the x86-64 one. */
#include "../sys/x86_64/sysnum.h"
#define __syscall(n, a, b, c, d, e, f) __syscall(__X64_SYS_##n, a, b, c, d, e, f)
#endif
void __set_tp(void *tp);
long __clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid);
void __unmap_exit(void *map, size_t size); /* unmaps (the caller's stack) and ends the thread */

/* locks of the library itself */
void __lock(volatile int *l);
void __unlock(volatile int *l);
int __trylock(volatile int *l);
long __futex_wait(volatile int *addr, int val, const void *timeout);
long __futex_wake(volatile int *addr, int n);

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 128
#endif
