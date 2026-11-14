#ifndef _SCHED_H
#define _SCHED_H
#include <sys/types.h>
#include <string.h>
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5
struct sched_param {
    int sched_priority;
};
int sched_yield(void);
typedef struct {
    unsigned long __bits[16];
} cpu_set_t;
#define CPU_SETSIZE 1024
#define CPU_ZERO(s) memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(i, s) ((s)->__bits[(i) / 64] |= 1UL << ((i) % 64))
#define CPU_CLR(i, s) ((s)->__bits[(i) / 64] &= ~(1UL << ((i) % 64)))
#define CPU_ISSET(i, s) (((s)->__bits[(i) / 64] >> ((i) % 64)) & 1)
#define CPU_COUNT(s) __sched_cpucount(sizeof(cpu_set_t), (s))
#define CPU_COUNT_S(n, s) __sched_cpucount((n), (s))
#define CPU_ZERO_S(n, s) memset((s), 0, (n))
#define CPU_EQUAL(a, b) (memcmp((a), (b), sizeof(cpu_set_t)) == 0)
int __sched_cpucount(size_t size, const cpu_set_t *set);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int sched_getcpu(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *p);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *p);
int sched_getparam(pid_t pid, struct sched_param *p);
#endif
