/* Condition variables: producers and consumers around a small buffer, a broadcast that
   releases every waiter, and a wait that times out. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define CAP 4
#define NPROD 3
#define NCONS 3
#define ITEMS 5000

static int buf[CAP], head, count, producers_left = NPROD;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER, not_empty = PTHREAD_COND_INITIALIZER;
static long consumed_sum, consumed_n;

static void *producer(void *arg)
{
    long id = (long)arg;
    int i;
    for (i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&mu);
        while (count == CAP)
            pthread_cond_wait(&not_full, &mu);
        buf[(head + count++) % CAP] = (int)(id * 100000 + i);
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    producers_left--;
    pthread_cond_broadcast(&not_empty);
    pthread_mutex_unlock(&mu);
    return 0;
}

static void *consumer(void *arg)
{
    long sum = 0, n = 0;
    (void)arg;
    for (;;) {
        int v;
        pthread_mutex_lock(&mu);
        while (count == 0 && producers_left)
            pthread_cond_wait(&not_empty, &mu);
        if (count == 0) {
            pthread_mutex_unlock(&mu);
            break;
        }
        v = buf[head];
        head = (head + 1) % CAP;
        count--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&mu);
        sum += v;
        n++;
    }
    pthread_mutex_lock(&mu);
    consumed_sum += sum;
    consumed_n += n;
    pthread_mutex_unlock(&mu);
    return 0;
}

/* a gate that eight threads wait at */
static pthread_mutex_t gate_mu;
static pthread_cond_t gate;
static int gate_open, waiting, through;

static void *waiter(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gate_mu);
    waiting++;
    while (!gate_open)
        pthread_cond_wait(&gate, &gate_mu);
    through++;
    pthread_mutex_unlock(&gate_mu);
    return 0;
}

int main(void)
{
    pthread_t p[NPROD], c[NCONS], w[8];
    struct timespec ts, t0, t1;
    long i, want = 0, ms;
    int r;
    for (i = 0; i < NCONS; i++)
        pthread_create(&c[i], 0, consumer, 0);
    for (i = 0; i < NPROD; i++)
        pthread_create(&p[i], 0, producer, (void *)(i + 1));
    for (i = 0; i < NPROD; i++)
        pthread_join(p[i], 0);
    for (i = 0; i < NCONS; i++)
        pthread_join(c[i], 0);
    for (i = 1; i <= NPROD; i++)
        want += i * 100000L * ITEMS + (long)ITEMS * (ITEMS + 1) / 2;
    printf("consumed %ld items, sum %s\n", consumed_n, consumed_sum == want ? "right" : "WRONG");

    pthread_mutex_init(&gate_mu, 0);
    pthread_cond_init(&gate, 0);
    for (i = 0; i < 8; i++)
        pthread_create(&w[i], 0, waiter, 0);
    for (;;) {
        pthread_mutex_lock(&gate_mu);
        r = waiting;
        pthread_mutex_unlock(&gate_mu);
        if (r == 8)
            break;
        sched_yield();
    }
    pthread_mutex_lock(&gate_mu);
    printf("before the broadcast: %d through\n", through);
    gate_open = 1;
    pthread_cond_broadcast(&gate);
    pthread_mutex_unlock(&gate_mu);
    for (i = 0; i < 8; i++)
        pthread_join(w[i], 0);
    printf("after: %d through\n", through);

    /* nobody signals this one */
    clock_gettime(CLOCK_REALTIME, &ts);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ts.tv_nsec += 50 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    pthread_mutex_lock(&gate_mu);
    gate_open = 0;
    do
        r = pthread_cond_timedwait(&gate, &gate_mu, &ts);
    while (r == 0 && !gate_open);
    pthread_mutex_unlock(&gate_mu);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("timed wait: %s, %s\n", r == ETIMEDOUT ? "timed out" : "WRONG", ms >= 45 && ms < 5000 ? "after 50 ms" : "LATE OR EARLY");
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    pthread_mutex_lock(&gate_mu);
    r = pthread_cond_timedwait(&gate, &gate_mu, &ts);
    pthread_mutex_unlock(&gate_mu);
    printf("a time in the past: %s\n", r == ETIMEDOUT ? "timed out" : "WRONG");
    pthread_cond_destroy(&gate);
    pthread_mutex_destroy(&gate_mu);
    return 0;
}
