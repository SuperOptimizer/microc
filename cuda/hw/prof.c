/* mcudahw: MCUDA_PROF=FILE samples where the program's processor time goes (the instruction interrupted
   by a timer, 1000 times a second of processor time, all threads) and writes the addresses to FILE at
   exit, for `nm -n` to name. For finding out why a program built here is slower than another build. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* the interrupted registers, as the x86-64 kernel lays a signal's context out (uc_flags, uc_link, uc_stack,
   then the general registers: the same in every C library, so no <ucontext.h> is asked of this one) */
#define CTX_REG(ctx, i) (((const unsigned long *)(const void *)((const char *)(ctx) + 40))[i])

#define PROF_MAX (1L << 20)
static unsigned long *prof_pc;
static volatile long prof_n;
static const char *prof_path;
extern char __executable_start[], etext[];

static void prof_tick(int sig, siginfo_t *si, void *ctx)
{
    long k = __sync_fetch_and_add(&prof_n, 1);
    (void)sig;
    (void)si;
#if defined(__x86_64__)
    if (k < PROF_MAX) {
        unsigned long pc = CTX_REG(ctx, 16) /* rip */;
        unsigned long lo = (unsigned long)__executable_start, hi = (unsigned long)etext;
        prof_pc[2 * k] = pc;
        prof_pc[2 * k + 1] = 0;
        if (pc < lo || pc >= hi) {
            /* in a shared library: the first word up the stack that points into the program's own code is
               (nearly always) where the program called out */
            const unsigned long *sp = (const unsigned long *)CTX_REG(ctx, 15) /* rsp */;
            int j;
            for (j = 0; j < 600; j++)
                if (sp[j] >= lo && sp[j] < hi) {
                    prof_pc[2 * k + 1] = sp[j];
                    break;
                }
        }
    }
#else
    (void)ctx;
    (void)k;
#endif
}

static void prof_dump(void)
{
    FILE *f;
    long k, n = prof_n < PROF_MAX ? prof_n : PROF_MAX;
    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_PROF, &off, 0);
    f = fopen(prof_path, "w");
    if (!f)
        return;
    for (k = 0; k < n; k++)
        fprintf(f, "%lx %lx\n", prof_pc[2 * k], prof_pc[2 * k + 1]);
    fclose(f);
}

/* MCUDA_SEGV=1: a fault prints where it happened and the addresses up the stack that point into the
   program's code (the callers, more or less), then the program dies as it would have. */
static void segv_report(int sig, siginfo_t *si, void *ctx)
{
#if defined(__x86_64__)
    unsigned long pc = CTX_REG(ctx, 16);
    const unsigned long *sp = (const unsigned long *)CTX_REG(ctx, 15);
    unsigned long lo = (unsigned long)__executable_start, hi = (unsigned long)etext;
    int j, n = 0;
    fprintf(stderr, "mcuda: signal %d at pc %lx, address %lx; stack:", sig, pc, (unsigned long)si->si_addr);
    for (j = 0; j < 400 && n < 12; j++)
        if (sp[j] >= lo && sp[j] < hi) {
            fprintf(stderr, " %lx", sp[j]);
            n++;
        }
    fprintf(stderr, "\n");
#else
    (void)si;
    (void)ctx;
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

/* (called when the first unit of device code registers itself) */
void hw_prof_start(void)
{
    static int done;
    struct sigaction sa;
    struct itimerval it;
    if (done)
        return;
    done = 1;
    if (getenv("MCUDA_SEGV")) {
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = segv_report;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, 0);
        sigaction(SIGBUS, &sa, 0);
        sigaction(SIGFPE, &sa, 0);
    }
    prof_path = getenv("MCUDA_PROF");
    if (!prof_path || !prof_path[0])
        return;
    prof_pc = (unsigned long *)calloc((size_t)PROF_MAX, 2 * sizeof *prof_pc);
    if (!prof_pc)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_tick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, 0);
    atexit(prof_dump);
}
