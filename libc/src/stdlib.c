#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "lock.h"

void *__map(size_t n);
void __unmap(void *p, size_t n);
void _exit(int status);
int __getpid(void);
int __kill(int pid, int sig);
void __stdio_exit(void);
extern char **__environ;

/* ---- memory allocator ----
   Small blocks come from size classes (powers of two and midpoints) carved
   out of large chunks and are recycled through per-class free lists.
   Large blocks are mapped and unmapped directly. Every block has a 16-byte
   header holding its capacity.

   With threads, each thread keeps free blocks of its own per class and goes
   to the shared lists, under a lock, a batch at a time: to refill when it has
   none of a class, to give back when it has gathered too many, and with
   everything it has when it ends. */

#define HDR 16
#define NCLASS 48
#define BIG 0x20000 /* above this, map directly */
#define CHUNK 0x100000

typedef struct Free {
    struct Free *next;
} Free;

#define NKEPT 8
#define KEEP_MAX 0x1000000 /* mappings above 16MB are always given back */
static size_t *kept[NKEPT]; /* freed mappings waiting to be reused */

static Free *free_lists[NCLASS];
static char *chunk_p, *chunk_end;
LOCK_DECL(heap_lock);

#ifdef __MCC_THREADS
static __thread Free *cache[NCLASS];
static __thread unsigned short ncache[NCLASS];
#endif

static size_t class_size(int c)
{
    size_t base = (size_t)16 << (c / 2);
    return (c & 1) ? base + base / 2 : base;
}

static int size_class(size_t n)
{
    int c = 0;
    size_t base = 16;
    while (base < n) {
        if (base + base / 2 >= n)
            return c + 1;
        base <<= 1;
        c += 2;
    }
    return c;
}

#ifdef __MCC_THREADS
/* How many blocks of a class a thread holds on to: about 64KB worth, 64 at most, two at least. */
static unsigned cache_max(int c)
{
    size_t n = 0x10000 / class_size(c);
    return n > 64 ? 64 : n < 2 ? 2 : (unsigned)n;
}
#endif

void *malloc(size_t n)
{
    size_t *h;
    size_t size;
    int c;
    if (n == 0)
        n = 1;
    if (n > BIG) {
        int best = -1;
        size = (n + HDR + 0x3fff) & ~(size_t)0x3fff;
        /* a recently freed mapping that fits without wasting much */
        LOCK(heap_lock);
        for (c = 0; c < NKEPT; c++)
            if (kept[c] && kept[c][0] + HDR >= size && kept[c][0] + HDR <= size * 4 &&
                (best < 0 || kept[c][0] < kept[best][0]))
                best = c;
        if (best >= 0) {
            h = kept[best];
            kept[best] = 0;
            UNLOCK(heap_lock);
            return (char *)h + HDR;
        }
        UNLOCK(heap_lock);
        h = __map(size);
        if (!h) {
            errno = ENOMEM;
            return 0;
        }
        h[0] = size - HDR;
        h[1] = 1; /* mapped */
        return (char *)h + HDR;
    }
    c = size_class(n);
#ifdef __MCC_THREADS
    if (cache[c]) {
        Free *f = cache[c];
        cache[c] = f->next;
        ncache[c]--;
        return f;
    }
    if (__libc_threaded) {
        /* a few at once from the shared list, so that the lock is not taken for every block */
        int want = (int)cache_max(c) / 4;
        Free *f;
        __lock(&heap_lock);
        f = free_lists[c];
        if (f) {
            Free *last = f;
            int got = 1;
            while (got <= want && last->next) {
                last = last->next;
                got++;
            }
            free_lists[c] = last->next;
            last->next = 0;
            cache[c] = f->next;
            ncache[c] = (unsigned short)(got - 1);
            __unlock(&heap_lock);
            return f;
        }
        __unlock(&heap_lock);
    }
#endif
    LOCK(heap_lock);
    if (free_lists[c]) {
        Free *f = free_lists[c];
        free_lists[c] = f->next;
        UNLOCK(heap_lock);
        return f;
    }
    size = class_size(c) + HDR;
    if ((size_t)(chunk_end - chunk_p) < size) {
        chunk_p = __map(CHUNK);
        if (!chunk_p) {
            chunk_end = 0;
            UNLOCK(heap_lock);
            errno = ENOMEM;
            return 0;
        }
        chunk_end = chunk_p + CHUNK;
    }
    h = (size_t *)chunk_p;
    chunk_p += size;
    UNLOCK(heap_lock);
    h[0] = size - HDR;
    h[1] = 0;
    return (char *)h + HDR;
}

#ifdef __MCC_THREADS
/* Moves blocks of class c from the calling thread's cache to the shared list until keep are left. */
static void cache_release(int c, unsigned keep)
{
    Free *first = cache[c], *last = first;
    unsigned n = ncache[c] - keep, k;
    if (!n)
        return;
    for (k = 1; k < n; k++)
        last = last->next;
    cache[c] = last->next;
    ncache[c] = (unsigned short)keep;
    __lock(&heap_lock);
    last->next = free_lists[c];
    free_lists[c] = first;
    __unlock(&heap_lock);
}

/* A thread is ending: what it holds is for the others. */
void __malloc_thread_exit(void)
{
    int c;
    for (c = 0; c < NCLASS; c++)
        if (ncache[c])
            cache_release(c, 0);
}
#endif

void free(void *p)
{
    size_t *h;
    Free *f;
    int c;
    if (!p)
        return;
    h = (size_t *)((char *)p - HDR);
    if (h[1] > 1) { /* cut out of a larger block by __memalign: the header names that block */
        p = (void *)h[1];
        h = (size_t *)((char *)p - HDR);
    }
    if (h[1]) {
        /* keep a few mappings for reuse: mapping and unmapping costs two
           system calls and a page fault for every page touched again */
        size_t *out = h;
        if (h[0] <= KEEP_MAX) {
            LOCK(heap_lock);
            for (c = 0; c < NKEPT; c++)
                if (!kept[c]) {
                    kept[c] = h;
                    UNLOCK(heap_lock);
                    return;
                }
            /* full: let go of the smallest */
            for (c = 0; c < NKEPT; c++)
                if (kept[c][0] < out[0]) {
                    size_t *t = kept[c];
                    kept[c] = out;
                    out = t;
                }
            UNLOCK(heap_lock);
        }
        __unmap(out, out[0] + HDR);
        return;
    }
    c = size_class(h[0]);
    f = p;
#ifdef __MCC_THREADS
    if (__libc_threaded) {
        unsigned max = cache_max(c);
        f->next = cache[c];
        cache[c] = f;
        if (++ncache[c] > max)
            cache_release(c, max / 2);
        return;
    }
#endif
    f->next = free_lists[c];
    free_lists[c] = f;
}

void *calloc(size_t count, size_t size)
{
    size_t n = count * size;
    void *p;
    if (size && n / size != count)
        return 0;
    p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    size_t *h;
    void *q;
    if (!p)
        return malloc(n);
    h = (size_t *)((char *)p - HDR);
    if (n <= h[0] && (!h[1] || n > h[0] / 2))
        return p;
    q = malloc(n);
    if (!q)
        return 0;
    memcpy(q, p, n < h[0] ? n : h[0]);
    free(p);
    return q;
}

/* A block of n bytes at a multiple of align (a power of two). Above the allocator's own 16 it is cut out
   of a larger block, with a header in front that says so: free and realloc go by that. */
void *__memalign(size_t align, size_t n)
{
    char *raw, *p;
    size_t *h;
    if (align <= 16)
        return malloc(n);
    if (n + align + HDR < n) {
        errno = ENOMEM;
        return 0;
    }
    raw = malloc(n + align + HDR);
    if (!raw)
        return 0;
    p = (char *)(((size_t)raw + HDR + align - 1) & ~(align - 1));
    h = (size_t *)(p - HDR);
    h[0] = ((size_t *)(raw - HDR))[0] - (size_t)(p - raw);
    h[1] = (size_t)raw;
    return p;
}

size_t __malloc_size(void *p)
{
    return p ? ((size_t *)((char *)p - HDR))[0] : 0;
}

/* ---- process ---- */

static void (*exit_fns[32])(void);
static int nexit_fns;
LOCK_DECL(exit_lock);
LOCK_DECL(exiting);

int atexit(void (*fn)(void))
{
    int r = -1;
    LOCK(exit_lock);
    if (nexit_fns < 32) {
        exit_fns[nexit_fns++] = fn;
        r = 0;
    }
    UNLOCK(exit_lock);
    return r;
}

void exit(int status)
{
    LOCK(exiting); /* one thread does it; any other that tries waits here for the end */
    for (;;) {
        void (*fn)(void);
        LOCK(exit_lock);
        fn = nexit_fns ? exit_fns[--nexit_fns] : 0;
        UNLOCK(exit_lock);
        if (!fn)
            break;
        fn();
    }
    __stdio_exit();
    _exit(status);
}

void abort(void)
{
    __kill(__getpid(), SIGABRT);
    _exit(134);
}

int raise(int sig)
{
    return __kill(__getpid(), sig);
}

#ifndef __linux__
void (*signal(int sig, void (*handler)(int)))(int)
{
    (void)sig;
    (void)handler;
    return SIG_DFL; /* handlers are not supported */
}
#endif

#ifdef __MCC_THREADS
volatile int __env_lock; /* shared with the functions that change the environment */
#endif

char *getenv(const char *name)
{
    size_t n = strlen(name);
    char **e, *r = 0;
    LOCK(__env_lock);
    for (e = __environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') {
            r = *e + n + 1;
            break;
        }
    UNLOCK(__env_lock);
    return r;
}

#if !defined(__linux__) && !defined(__MCC_POSIX__)
int system(const char *cmd)
{
    return cmd ? -1 : 0; /* no command processor */
}
#endif

/* ---- numbers ---- */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

div_t div(int a, int b)
{
    div_t r;
    r.quot = a / b;
    r.rem = a % b;
    return r;
}

ldiv_t ldiv(long a, long b)
{
    ldiv_t r;
    r.quot = a / b;
    r.rem = a % b;
    return r;
}

LOCK_DECL(rand_lock);

#ifdef __linux__
/* On Linux the numbers are those of the GNU library, which programs' tests have come to expect: an
   additive generator, r[i] = r[i-31] + r[i-3], over a table that a multiplicative one fills from the seed. */
static unsigned rand_tab[34];
static int rand_at = -1; /* -1: not seeded yet; else the place of the oldest of the 34 */

static unsigned rand_step(void)
{
    unsigned v = rand_tab[(rand_at + 3) % 34] + rand_tab[(rand_at + 31) % 34];
    rand_tab[rand_at] = v;
    rand_at = (rand_at + 1) % 34;
    return v;
}

static void rand_seed(unsigned seed)
{
    int i;
    long w;
    rand_tab[0] = seed ? seed : 1;
    for (i = 1; i < 31; i++) {
        long x = (long)(int)rand_tab[i - 1];
        w = 16807 * (x % 127773) - 2836 * (x / 127773);
        if (w < 0)
            w += 2147483647;
        rand_tab[i] = (unsigned)w;
    }
    for (i = 31; i < 34; i++)
        rand_tab[i] = rand_tab[i - 31];
    rand_at = 0;
    for (i = 34; i < 344; i++)
        rand_step();
}

int rand(void)
{
    unsigned v;
    LOCK(rand_lock);
    if (rand_at < 0)
        rand_seed(1);
    v = rand_step();
    UNLOCK(rand_lock);
    return (int)(v >> 1);
}

void srand(unsigned seed)
{
    LOCK(rand_lock);
    rand_seed(seed);
    UNLOCK(rand_lock);
}
#else
static unsigned long rand_state = 1;

int rand(void)
{
    unsigned long s;
    LOCK(rand_lock);
    s = rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    UNLOCK(rand_lock);
    return (int)((s >> 33) & 0x7fffffff);
}

void srand(unsigned seed)
{
    LOCK(rand_lock);
    rand_state = seed;
    UNLOCK(rand_lock);
}
#endif

static int digit_of(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

/* Shared by strtol and strtoul; *neg and *overflow report what happened. */
static unsigned long parse_ulong(const char *s, char **end, int base, int *neg, int *overflow)
{
    const char *p = s, *start;
    unsigned long v = 0, limit;
    *neg = *overflow = 0;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '-') {
        *neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_of(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    start = p;
    limit = ULONG_MAX / (unsigned long)base;
    while (digit_of(*p) < base) {
        unsigned long d = (unsigned long)digit_of(*p);
        if (v > limit || v * (unsigned long)base > ULONG_MAX - d)
            *overflow = 1;
        v = v * (unsigned long)base + d;
        p++;
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    int neg, overflow;
    unsigned long v = parse_ulong(s, end, base, &neg, &overflow);
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    return neg ? 0 - v : v;
}

long strtol(const char *s, char **end, int base)
{
    int neg, overflow;
    unsigned long v = parse_ulong(s, end, base, &neg, &overflow);
    if (overflow || v > (neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX)) {
        errno = ERANGE;
        return neg ? LONG_MIN : LONG_MAX;
    }
    return neg ? (long)(0 - v) : (long)v;
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
double atof(const char *s) { return strtod(s, 0); }

/* ---- sorting and searching ---- */

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n >= 8) {
        unsigned long t = *(unsigned long *)a;
        *(unsigned long *)a = *(unsigned long *)b;
        *(unsigned long *)b = t;
        a += 8;
        b += 8;
        n -= 8;
    }
    if (n >= 4) {
        unsigned int t = *(unsigned int *)a;
        *(unsigned int *)a = *(unsigned int *)b;
        *(unsigned int *)b = t;
        a += 4;
        b += 4;
        n -= 4;
    }
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* elements of 4 and 8 bytes are swapped in place, others through swap_bytes */
#define SWAP(x, y, n) \
    do { \
        if ((n) == 4) { \
            unsigned int t_ = *(unsigned int *)(x); \
            *(unsigned int *)(x) = *(unsigned int *)(y); \
            *(unsigned int *)(y) = t_; \
        } else if ((n) == 8) { \
            unsigned long t_ = *(unsigned long *)(x); \
            *(unsigned long *)(x) = *(unsigned long *)(y); \
            *(unsigned long *)(y) = t_; \
        } else { \
            swap_bytes((x), (y), (n)); \
        } \
    } while (0)

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *a = base;
    while (n > 8) {
        /* median of three to the front, then partition */
        char *lo = a, *hi = a + (n - 1) * size, *mid = a + (n / 2) * size, *i, *j;
        if (cmp(mid, lo) < 0)
            SWAP(mid, lo, size);
        if (cmp(hi, mid) < 0) {
            SWAP(hi, mid, size);
            if (cmp(mid, lo) < 0)
                SWAP(mid, lo, size);
        }
        SWAP(lo, mid, size);
        i = lo;
        j = hi + size;
        for (;;) {
            do
                i += size;
            while (i <= hi && cmp(i, lo) < 0);
            do
                j -= size;
            while (cmp(lo, j) < 0);
            if (i >= j)
                break;
            SWAP(i, j, size);
        }
        SWAP(lo, j, size);
        /* recurse into the smaller part, loop on the larger */
        {
            size_t left = (size_t)(j - a) / size, right = n - left - 1;
            if (left < right) {
                qsort(a, left, size, cmp);
                a = j + size;
                n = right;
            } else {
                qsort(j + size, right, size, cmp);
                n = left;
            }
        }
    }
    {
        size_t i, j;
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && cmp(a + j * size, a + (j - 1) * size) < 0; j--)
                SWAP(a + j * size, a + (j - 1) * size, size);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    const char *a = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, a + mid * size);
        if (c == 0)
            return (void *)(a + mid * size);
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}
