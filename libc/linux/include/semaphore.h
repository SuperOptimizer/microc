#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
#include <time.h>
typedef struct {
    volatile int __value, __waiters;
} sem_t;
#define SEM_FAILED ((sem_t *)0)
#define SEM_VALUE_MAX 0x7fffffff
int sem_init(sem_t *s, int shared, unsigned value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *until);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);
#endif
