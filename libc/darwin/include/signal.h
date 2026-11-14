/* Signals, with Darwin's numbers. Handlers are not delivered: signal() and sigaction() only keep the books. */
#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
typedef int sig_atomic_t;
typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGBUS 10
#define SIGSEGV 11
#define SIGSYS 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGURG 16
#define SIGSTOP 17
#define SIGTSTP 18
#define SIGCONT 19
#define SIGCHLD 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGIO 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGUSR1 30
#define SIGUSR2 31
#define NSIG 32
#define SIG_BLOCK 1
#define SIG_UNBLOCK 2
#define SIG_SETMASK 3
#define SA_RESTART 2
#define SA_NOCLDSTOP 8
struct sigaction {
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
};
void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);
int kill(pid_t pid, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
char *strsignal(int sig);
void psignal(int sig, const char *msg);
extern const char *const sys_siglist[NSIG];
#endif
