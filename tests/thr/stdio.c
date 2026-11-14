/* Formatted output from eight threads to one stream: every line must arrive whole. Also the
   conversions that once shared a buffer, the time functions, errno and rand. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NT 8
#define LINES 400

static FILE *out;
static atomic_int errors;

static void *writer(void *arg)
{
    long id = (long)arg;
    int i;
    char num[64], want[64];
    for (i = 0; i < LINES; i++) {
        struct tm tm;
        time_t t = (time_t)(id * 86400 * 365 + i * 3600);
        fprintf(out, "thread %ld line %d: %s %.2f %x|%s\n", id, i, "some words in the middle", (double)id + i / 100.0,
                (unsigned)(id * 1000 + i), "end");
        if (i % 16 == 0)
            printf("");
        /* floating conversions */
        snprintf(num, sizeof num, "%.6f %g", (double)id * 1.5 + i, (double)i / 8);
        snprintf(want, sizeof want, "%ld.%s00000 %g", (id * 3 / 2) + i, id % 2 ? "5" : "0", (double)i / 8);
        if (strcmp(num, want))
            atomic_fetch_add(&errors, 1);
        /* the calendar */
        gmtime_r(&t, &tm);
        if (tm.tm_hour != i % 24 || mktime(&tm) != t)
            atomic_fetch_add(&errors, 1);
        /* each thread has its own errno */
        if (id % 2) {
            if (close(-1) != -1 || errno != EBADF)
                atomic_fetch_add(&errors, 1);
        } else {
            if (open("/no/such/file", O_RDONLY) != -1 || errno != ENOENT)
                atomic_fetch_add(&errors, 1);
        }
        errno = (int)id;
        sched_yield();
        if (errno != id)
            atomic_fetch_add(&errors, 1);
        (void)rand();
#ifndef __GLIBC__ /* whose getenv (and mktime, through it) may read the table while setenv moves it */
        if (!getenv("PATH_TO_NOWHERE") && i % 64 == 0) {
            char name[32];
            sprintf(name, "VAR_%ld_%d", id, i);
            setenv(name, "x", 1);
        }
#endif
    }
    return 0;
}

int main(void)
{
    pthread_t t[NT];
    char line[256];
    long i, id, seen[NT + 1], bad = 0, total = 0;
    int n, hex;
    double d;
    out = fopen("/tmp/lines", "w");
    if (!out) {
        printf("cannot write /tmp/lines\n");
        return 1;
    }
#ifdef __GLIBC__
    setenv("VAR_3_64", "x", 1);
    setenv("VAR_8_384", "x", 1);
#endif
    errno = 99;
    for (i = 0; i < NT; i++)
        pthread_create(&t[i], 0, writer, (void *)(i + 1));
    for (i = 0; i < NT; i++)
        pthread_join(t[i], 0);
    fclose(out);
    printf("main's errno %d\n", errno);
    memset(seen, 0, sizeof seen);
    out = fopen("/tmp/lines", "r");
    while (fgets(line, sizeof line, out)) {
        char words[64], end[8];
        total++;
        if (sscanf(line, "thread %ld line %d: %63[a-z ] %lf %x|%7s", &id, &n, words, &d, &hex, end) != 6 || id < 1 ||
            id > NT || n != seen[id] || strcmp(words, "some words in the middle ") || strcmp(end, "end") ||
            hex != id * 1000 + n) {
            bad++;
            continue;
        }
        seen[id]++; /* a thread's own lines stay in order */
    }
    fclose(out);
    printf("%ld lines, %ld damaged, errors %d\n", total, bad, atomic_load(&errors));
    printf("environment %s\n", getenv("VAR_3_64") && getenv("VAR_8_384") ? "kept" : "LOST");
    return 0;
}
