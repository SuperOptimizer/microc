/* strength reduction (opt.c iv_reduce) with two roots in one loop, the second made from the first: libc's fflush(0) walks its FILE
   table (48-byte records) by &files[i], and &files[i] - files was the second root. Its first value was worked out in front of the loop
   from the first root's new counter, a phi of the loop header, so the pointers were garbage (11-27: every tests/cuda program of the
   CPU's run time that flushed at exit segfaulted). Also two counters in one loop, each with its roots. */
#include <stdio.h>

typedef struct { int flags; char pad[44]; } F; /* 48 bytes, as FILE */
static F files[20];
static int calls;

__attribute__((noinline)) static void touch(int *l) { (*l)++; }

__attribute__((noinline)) static F *new_stream(void)
{
    int i;
    F *f = 0;
    touch(&calls);
    for (i = 3; i < 20 && !f; i++)
        if (!(files[i].flags & 1)) {
            f = &files[i];
            f->flags = 1;
        }
    return f;
}

__attribute__((noinline)) static long flush_all(void)
{
    int i;
    long r = 0;
    for (i = 0; i < 20; i++)
        if (files[i].flags & 1) {
            touch(&calls);
            r += (long)(&files[i] - files) * 1000 + files[i].flags;
        }
    return r;
}

typedef struct { short a; char b[22]; } G; /* 24 bytes */
static G g[64];

__attribute__((noinline)) static long two(int n)
{
    long s = 0;
    int i, j;
    for (i = 0, j = n - 1; i < n; i++, j--) {
        s += (long)(&files[i % 20] - files) * 7 + (long)(&g[j] - g) * 3;
        g[j].a = (short)i;
        touch(&calls);
    }
    return s + g[0].a + g[n - 1].a;
}

int main(void)
{
    F *a, *b;
    files[3].flags = files[4].flags = 1;
    a = new_stream();
    b = new_stream();
    printf("%d %d %ld\n", (int)(a - files), (int)(b - files), flush_all());
    {
        long t = two(40);
        printf("%ld %d\n", t, calls);
    }
    return 0;
}
