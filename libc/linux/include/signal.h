#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <stddef.h>
typedef int sig_atomic_t;
typedef struct {
    unsigned long __bits[1];
} sigset_t;
typedef int __pid_t_sig;
union sigval {
    int sival_int;
    void *sival_ptr;
};
typedef struct {
    int si_signo, si_errno, si_code;
    int __pad0;
    union {
        struct { int si_pid; unsigned si_uid; int si_status; } __kill;
        struct { void *si_addr; } __fault;
        long __fill[14];
    } __u;
} siginfo_t;
#define si_pid __u.__kill.si_pid
#define si_uid __u.__kill.si_uid
#define si_status __u.__kill.si_status
#define si_addr __u.__fault.si_addr
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    sigset_t sa_mask;
};
#define sa_handler __sa.sa_handler
#define sa_sigaction __sa.sa_sigaction
typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
typedef void (*sighandler_t)(int);
typedef void (*__sighandler_t)(int);
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL 29
#define SIGPWR 30
#define SIGSYS 31
#define SIGRTMIN 34
#define SIGRTMAX 64
#define NSIG 65
#define _NSIG 65
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOMASK SA_NODEFER
#define SA_ONESHOT SA_RESETHAND
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);
int kill(int pid, int sig);
int killpg(int pgrp, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigsuspend(const sigset_t *mask);
int sigpending(sigset_t *set);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int sigwait(const sigset_t *set, int *sig);
struct timespec;
int sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout);
int sigwaitinfo(const sigset_t *set, siginfo_t *info);
int siginterrupt(int sig, int flag);
char *strsignal(int sig);
void psignal(int sig, const char *msg);
extern const char *const sys_siglist[];
#endif
