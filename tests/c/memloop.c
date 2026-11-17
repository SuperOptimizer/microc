/* What is known of memory before a loop is known inside it and after it unless the loop writes there (opt.c,
   mem_enter_loop): loads that licm may not move (not run in every trip), and the cases where the loop does write. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NI __attribute__((noinline))

typedef struct { int n, k; float h; float *out; long cnt, cnt2; } rec;

static int gcount;
static rec *grec;

NI static void touch(void)
{
    gcount++;
    if (grec)
        grec->h += 1;
}

/* a field read before the loop, read again under a condition inside it and after it; the loop writes a local array */
NI static float kept(const rec *r, const int *sel, int n)
{
    float t[16], s = r->h;
    int i;
    for (i = 0; i < 16; i++)
        t[i] = (float)i;
    for (i = 0; i < n; i++) {
        if (sel[i])
            s += r->h * t[i & 15];
        t[(i + 1) & 15] += 1;
    }
    return s + r->h;
}

/* the loop writes the field itself, late in its body: the read at the top of the next trip sees it */
NI static float written(rec *r, int n)
{
    float s = r->h;
    int i;
    for (i = 0; i < n; i++) {
        if (i & 1)
            s += r->h;
        r->h = s * 0.5f;
    }
    return s + r->h;
}

/* ... or writes it through another pointer that may be the same */
NI static float written_through(rec *r, float *o, int n)
{
    float s = r->h;
    int i;
    for (i = 0; i < n; i++) {
        if (i % 3)
            s += r->h;
        o[i & 1] += 1; /* o == &r->h */
    }
    return s + r->h;
}

/* a call in the loop: a local it cannot reach is kept, the record it can reach is read again */
NI static float with_call(rec *r, int n)
{
    float loc[4], s = r->h;
    int i;
    for (i = 0; i < 4; i++)
        loc[i] = r->h + (float)i;
    for (i = 0; i < n; i++) {
        touch();
        if (i & 1)
            s += r->h + loc[i & 3];
    }
    return s + r->h + loc[1];
}

/* nested loops, the inner one writes */
NI static long nested(rec *r, long *v, int n)
{
    long s = r->cnt;
    int i, j;
    for (i = 0; i < n; i++) {
        if (i != 2)
            s += r->cnt;
        for (j = 0; j < 2; j++)
            if (v[j] > 1)
                v[j] -= 1; /* v == &r->cnt */
    }
    return s + r->cnt;
}

/* a loop entered in the middle */
NI static float middle(rec *r, int n, int skip)
{
    float s = r->h;
    int i = 0;
    if (skip)
        goto in;
    for (; i < n; i++) {
        r->h += 1;
    in:
        s += r->h;
    }
    return s;
}

/* a block cleared inside the loop */
NI static float cleared(rec *r, rec *other, int n)
{
    float s = r->h;
    int i;
    for (i = 0; i < n; i++) {
        s += r->h + (float)r->n;
        if (i == 2)
            memset(other, 0, sizeof *other);
    }
    return s;
}

/* an allocation made before the loop, another made inside it */
NI static float allocs(const rec *r, int n)
{
    float *a = malloc(8 * sizeof *a), s = 0;
    int i;
    for (i = 0; i < 8; i++)
        a[i] = r->h * (float)i;
    for (i = 0; i < n; i++) {
        float *b = malloc(4 * sizeof *b);
        b[0] = a[i & 7] + r->h;
        b[1] = b[0] * 2;
        a[(i + 3) & 7] += b[1];
        s += b[0] + a[0];
        free(b);
    }
    s += a[3];
    free(a);
    return s;
}

int main(void)
{
    rec r, o;
    int sel[12] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0};
    long v[4] = {5, 6, 7, 8};
    float out[4] = {0, 0, 0, 0};
    memset(&r, 0, sizeof r);
    r.n = 3; r.k = 4; r.h = 0.5f; r.out = out; r.cnt = 9; r.cnt2 = 4;
    printf("kept %g\n", kept(&r, sel, 12));
    printf("written %g\n", written(&r, 6));
    r.h = 0.5f;
    printf("written_through %g", written_through(&r, &r.h, 7));
    printf(" %g\n", written_through(&r, out, 7));
    r.h = 0.5f;
    printf("with_call %g\n", with_call(&r, 6));
    grec = &r;
    printf("with_call %g %d\n", with_call(&r, 6), gcount);
    grec = 0;
    printf("nested %ld", nested(&r, v, 5));
    printf(" %ld\n", nested(&r, &r.cnt, 5));
    r.h = 0.5f;
    printf("middle %g", middle(&r, 4, 0));
    printf(" %g\n", middle(&r, 4, 1));
    r.h = 0.5f;
    o = r;
    printf("cleared %g", cleared(&r, &o, 5));
    printf(" %g\n", cleared(&r, &r, 5));
    r.h = 0.5f;
    printf("allocs %g\n", allocs(&r, 9));
    return 0;
}
