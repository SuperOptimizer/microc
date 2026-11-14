/* Every lock and every host thread of the library is here. On Linux the blocks of a launch run on several
   host threads (pthreads); elsewhere there is one thread and nothing here does anything.
   MCUDA_THREADS=n in the environment sets the number of threads (1: none besides the caller). */
#include <stdlib.h>
#include "mc.h"

#ifdef __linux__
#include <pthread.h>
#include <unistd.h>

#define NLOCK 8
#define NSTRIPE 64
#define MAXPOOL 63

static pthread_mutex_t locks[NLOCK] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
                                       PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
                                       PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static pthread_t owner[NLOCK];
static int held[NLOCK], depth[NLOCK];
static pthread_mutex_t stripes[NSTRIPE];
static pthread_mutex_t setup = PTHREAD_MUTEX_INITIALIZER;
static int stripes_ready;

/* The locks may be taken again by the thread that holds them: the library was written with one thread in mind. */
void mcuda_lock(int which)
{
    pthread_t me = pthread_self();
    if (held[which] && pthread_equal(owner[which], me)) {
        depth[which]++;
        return;
    }
    pthread_mutex_lock(&locks[which]);
    owner[which] = me;
    depth[which] = 1;
    held[which] = 1;
}

void mcuda_unlock(int which)
{
    if (--depth[which] == 0) {
        held[which] = 0;
        pthread_mutex_unlock(&locks[which]);
    }
}

static void stripes_init(void)
{
    int i;
    pthread_mutex_lock(&setup);
    if (!stripes_ready) {
        for (i = 0; i < NSTRIPE; i++)
            pthread_mutex_init(&stripes[i], 0);
        stripes_ready = 1;
    }
    pthread_mutex_unlock(&setup);
}

static pthread_mutex_t *stripe(const void *p)
{
    unsigned long a = (unsigned long)p >> 3;
    if (!stripes_ready)
        stripes_init();
    return &stripes[(a ^ a >> 6 ^ a >> 12) & (NSTRIPE - 1)];
}

/* for an atom or red instruction on the word at p */
void mcuda_lock_addr(const void *p) { pthread_mutex_lock(stripe(p)); }
void mcuda_unlock_addr(const void *p) { pthread_mutex_unlock(stripe(p)); }

/* ---- the threads ---- */

static pthread_mutex_t launch = PTHREAD_MUTEX_INITIALIZER; /* one mcuda_par at a time */
static pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start = PTHREAD_COND_INITIALIZER, done = PTHREAD_COND_INITIALIZER;
static pthread_t pool[MAXPOOL];
static int npool, nwork;
static unsigned long gen;
static int want, active;
static void (*job_fn)(void *, int);
static void *job_arg;

int mcuda_nworkers(void)
{
    if (!nwork) {
        const char *e = getenv("MCUDA_THREADS");
        long n = e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN);
        nwork = n < 1 ? 1 : n > MAXPOOL + 1 ? MAXPOOL + 1 : (int)n;
    }
    return nwork;
}

static void *pool_main(void *arg)
{
    int me = (int)(long)arg;
    unsigned long seen = 0;
    pthread_mutex_lock(&pm);
    for (;;) {
        while (gen == seen || me > want) {
            if (gen != seen)
                seen = gen; /* a job this thread has no part in */
            pthread_cond_wait(&start, &pm);
        }
        seen = gen;
        pthread_mutex_unlock(&pm);
        job_fn(job_arg, me);
        pthread_mutex_lock(&pm);
        if (--active == 0)
            pthread_cond_signal(&done);
    }
    return 0;
}

/* Run fn(arg, w) on n threads at once, w = 0 (the caller) .. n - 1; returns when all have returned. */
void mcuda_par(int n, void (*fn)(void *arg, int worker), void *arg)
{
    int max = mcuda_nworkers();
    if (n > max)
        n = max;
    pthread_mutex_lock(&launch);
    if (n > 1) {
        pthread_mutex_lock(&pm);
        while (npool < n - 1) {
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 1UL << 20);
            if (pthread_create(&pool[npool], &at, pool_main, (void *)(long)(npool + 1)) != 0) {
                pthread_attr_destroy(&at);
                break;
            }
            pthread_attr_destroy(&at);
            npool++;
        }
        if (n > npool + 1)
            n = npool + 1;
        job_fn = fn;
        job_arg = arg;
        want = n - 1;
        active = n - 1;
        gen++;
        pthread_cond_broadcast(&start);
        pthread_mutex_unlock(&pm);
    }
    fn(arg, 0);
    if (n > 1) {
        pthread_mutex_lock(&pm);
        while (active)
            pthread_cond_wait(&done, &pm);
        pthread_mutex_unlock(&pm);
    }
    pthread_mutex_unlock(&launch);
}

#else

void mcuda_lock(int which) { (void)which; }
void mcuda_unlock(int which) { (void)which; }
void mcuda_lock_addr(const void *p) { (void)p; }
void mcuda_unlock_addr(const void *p) { (void)p; }
int mcuda_nworkers(void) { return 1; }
void mcuda_par(int n, void (*fn)(void *arg, int worker), void *arg)
{
    (void)n;
    fn(arg, 0);
}

#endif
