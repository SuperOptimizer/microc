/* Many threads made and joined one after another and in groups: the stacks must be given
   back, whatever their size, and each must be as large as asked for. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long vm_pages(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long n = -1;
    if (f) {
        if (fscanf(f, "%ld", &n) != 1)
            n = -1;
        fclose(f);
    }
    return n;
}

static void *quick(void *arg) { return (void *)((long)arg + 1); }

static long depth(char *base, long want)
{
    char page[4096];
    memset(page, 1, sizeof page);
    if (base - page >= want)
        return page[100];
    return depth(base, want) + page[200] - 1;
}

static void *deep(void *arg)
{
    char here;
    return (void *)depth(&here, (long)arg);
}

int main(void)
{
    pthread_t t[16];
    pthread_attr_t attr;
    void *r;
    long i, k, sum = 0, before, after, distinct = 0;
    unsigned long seen[64];

    for (i = 0; i < 16; i++) {
        pthread_create(&t[0], 0, quick, (void *)i);
        pthread_join(t[0], &r);
    }
    before = vm_pages();
    for (i = 0; i < 3000; i++) {
        if (pthread_create(&t[0], 0, quick, (void *)i)) {
            printf("create failed at %ld\n", i);
            return 1;
        }
        for (k = 0; k < distinct; k++)
            if (seen[k] == (unsigned long)t[0])
                break;
        if (k == distinct && distinct < 64)
            seen[distinct++] = (unsigned long)t[0];
        pthread_join(t[0], &r);
        sum += (long)r;
    }
    printf("3000 in turn: sum %ld, %s\n", sum, distinct <= 2 ? "stack reused" : "MANY STACKS");
    sum = 0;
    for (i = 0; i < 200; i++) {
        for (k = 0; k < 16; k++)
            pthread_create(&t[k], 0, quick, (void *)(i * 16 + k));
        for (k = 0; k < 16; k++) {
            pthread_join(t[k], &r);
            sum += (long)r;
        }
    }
    printf("200 groups of 16: sum %ld\n", sum);
    /* stacks of other sizes are mapped and unmapped each time */
    pthread_attr_init(&attr);
    sum = 0;
    for (i = 0; i < 400; i++) {
        pthread_attr_setstacksize(&attr, i % 2 ? 65536 : 1 << 20);
        pthread_create(&t[0], &attr, deep, (void *)(i % 2 ? 40000L : 1000000L));
        pthread_join(t[0], &r);
        sum += (long)r;
    }
    printf("400 with their own stack sizes: %ld\n", sum);
    pthread_attr_setstacksize(&attr, 32 << 20);
    pthread_create(&t[0], &attr, deep, (void *)(31L << 20));
    pthread_join(t[0], &r);
    printf("a 32 MB stack: %ld\n", (long)r);
    pthread_create(&t[0], 0, deep, (void *)(7L << 20));
    pthread_join(t[0], &r);
    printf("the default stack holds 7 MB: %ld\n", (long)r);
    after = vm_pages();
    if (before < 0)
        printf("no /proc\n");
    else
        printf("address space %s\n", after - before < 16 * 2100 ? "steady" : "GREW");
    return 0;
}
