/* Run-time side of -fprofile-generate: collects the block counters of every
   translation unit that ran and merges them into the profile file at exit. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned long word;

/* descriptor: { registered, next, number of words, table }
   table: records { key, n, count[n] } */
static word *head;

static void dump(void)
{
    const char *path = getenv("MCC_PROFILE");
    word *old = 0, *d, **slot = 0;
    size_t len = 0, cap = 0, nslot = 0, k, mask;
    int fd;
    if (!path)
        path = "mcc.prof";
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        for (;;) {
            long n;
            if (cap - len < 0x10000) {
                cap = cap ? cap * 2 : 0x40000;
                old = (word *)realloc(old, cap);
                if (!old)
                    return;
            }
            n = read(fd, (char *)old + len, cap - len);
            if (n <= 0)
                break;
            len += (size_t)n;
        }
        close(fd);
        if (len < 8 || memcmp(old, "MCCPROF1", 8) != 0)
            len = 0;
    }
    len /= sizeof(word);
    /* index what the file has */
    for (k = 1; k + 2 <= len; k += 2 + old[k + 1]) {
        if (old[k + 1] > len - k - 2) {
            len = k; /* damaged: drop the rest */
            break;
        }
        nslot++;
    }
    for (mask = 63; mask < nslot * 2; mask = mask * 2 + 1)
        ;
    slot = (word **)calloc(mask + 1, sizeof(word *));
    if (!slot)
        return;
    for (k = 1; k + 2 <= len; k += 2 + old[k + 1]) {
        size_t h;
        if (!old[k])
            continue;
        for (h = old[k] & mask; slot[h]; h = (h + 1) & mask)
            ;
        slot[h] = old + k;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    write(fd, "MCCPROF1", 8);
    /* add to the records the file already has; the rest are appended */
    for (d = head; d; d = (word *)d[1]) {
        word *rec = (word *)d[3];
        size_t n = d[2];
        for (k = 0; k + 2 <= n; k += 2 + rec[k + 1]) {
            size_t h, j;
            for (h = rec[k] & mask; slot[h] && slot[h][0] != rec[k]; h = (h + 1) & mask)
                ;
            if (!slot[h])
                continue;
            if (slot[h][1] == rec[k + 1]) {
                for (j = 0; j < rec[k + 1]; j++)
                    slot[h][2 + j] += rec[k + 2 + j];
                rec[k] = 0; /* merged */
            } else {
                slot[h][0] = 0; /* from another build of the program */
            }
        }
    }
    if (len > 1)
        write(fd, old + 1, (len - 1) * sizeof(word));
    for (d = head; d; d = (word *)d[1]) {
        word *rec = (word *)d[3];
        size_t n = d[2];
        for (k = 0; k + 2 <= n; k += 2 + rec[k + 1])
            if (rec[k])
                write(fd, rec + k, (2 + rec[k + 1]) * sizeof(word));
    }
    close(fd);
}

/* (the orderings as numbers: only the Linux targets predefine the names) */
#ifndef __ATOMIC_ACQUIRE
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_ACQ_REL 4
#endif

/* The first call of any function of a unit registers the unit's table. Threads may race here (ufsm's samplers start
   many at once): the table is claimed by one of them, pushed onto the list without a lock, and dump() is registered
   once. (The counters themselves are plain increments: under threads a few counts can be lost, which a profile can
   afford.) */
static int dump_set;

void __mcc_prof_enter(word *d)
{
    word *h;
    if (__atomic_load_n(&d[0], __ATOMIC_ACQUIRE) || __atomic_exchange_n(&d[0], (word)1, __ATOMIC_ACQ_REL))
        return;
    h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    do
        d[1] = (word)h;
    while (!__atomic_compare_exchange_n(&head, &h, d, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    if (!__atomic_exchange_n(&dump_set, 1, __ATOMIC_ACQ_REL))
        atexit(dump);
}
