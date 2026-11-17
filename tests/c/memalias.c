/* Which object an address is in (opt.c, roots_apart): loads kept across stores into other objects (a local, a
   global, what a parameter points to, an allocation, a restrict parameter) and across calls that cannot reach them,
   and the cases where the objects are the same after all and the load must be done again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NI __attribute__((noinline))

typedef struct { int lz, ly, lx; float h; const float *delta; float *out; float w; long n; } dev;

static float tab[64];
static int gn = 5;
static float gs = 1.5f;
static float garr[4] = {1, 2, 3, 4};
static float *gp;
static long *glp;
static dev *gdev;

/* stores to a local array between loads through a parameter: the loads may leave the loop */
NI static float local_vs_param(const dev *f, int n, int k)
{
    float wc[64], s = 0;
    int c;
    for (c = 0; c < n; c++)
        wc[c] = f->h * (float)c + f->w;
    for (c = 0; c < n; c++)
        s += wc[c] * f->h;
    return s + wc[k];
}

/* ... but not when the "parameter" structure lies in that array (reached through an escaped address) */
NI static float local_escaped(int n)
{
    float wc[16], s = 0;
    int c;
    gp = wc;
    for (c = 0; c < 16; c++)
        wc[c] = 1;
    for (c = 0; c < n; c++) {
        wc[c] = gp[c + 1] * 2 + (float)c; /* gp[c + 1] is wc[c + 1] */
        gp[c + 2] += 1;
        s += wc[c + 2];
    }
    return s;
}

/* a pointer read from memory against a local whose address was never given away */
NI static float local_vs_loaded(const dev *f, int n)
{
    float t[8], s = 0;
    int c;
    for (c = 0; c < 8; c++)
        t[c] = 0;
    for (c = 0; c < n; c++) {
        t[c & 7] += f->delta[c]; /* f->delta is read once */
        s += f->delta[c + 1] * t[(c + 3) & 7];
    }
    return s;
}

/* a local whose address was stored in a structure, written through that structure */
NI static float local_through_struct(dev *f, int n)
{
    float t[8], s = 0;
    int c;
    for (c = 0; c < 8; c++)
        t[c] = (float)c;
    f->out = t;
    for (c = 0; c < n; c++) {
        f->out[c & 7] += 1;
        s += t[c & 7];
    }
    return s;
}

/* globals: stores into one array, loads of other variables */
NI static void global_vs_global(void)
{
    int i;
    for (i = 0; i < gn; i++)
        tab[i] = gs * (float)i;
}

/* a parameter may point at a global */
NI static float param_vs_global(float *p, int n)
{
    float s = 0;
    int i;
    for (i = 0; i < n; i++) {
        p[i] = garr[1] + (float)i; /* p is garr */
        s += garr[1];
    }
    return s;
}

/* two parameters may point at the same thing ... */
NI static float param_vs_param(float *d, const float *s, int n)
{
    float a = 0;
    int i;
    for (i = 0; i < n; i++) {
        d[i] = s[0] + 1;
        a += s[0];
    }
    return a;
}

/* ... unless one is restrict */
NI static float restrict_param(float *__restrict d, const float *s, const dev *f, int n)
{
    float a = 0;
    int i;
    for (i = 0; i < n; i++) {
        d[i] = s[i] * f->h + s[0];
        a += d[i];
    }
    return a;
}

/* a restrict parameter whose value is given away is like any other */
NI static float restrict_escaped(float *__restrict d, int n)
{
    float a = 0;
    int i;
    gp = d;
    for (i = 0; i < n; i++) {
        d[0] = (float)i;
        gp[0] += 1;
        a += d[0];
    }
    return a;
}

/* stores of size_t through one parameter, loads of a pointer member through another: they may be the same memory */
NI static float long_vs_pointer(const dev *f, long *ic, int a)
{
    float s = 0;
    ic[0] = (long)a;
    s += f->delta[ic[0]];
    ic[1] = (long)a + 1;
    s += f->delta[ic[1]];
    return s + (float)f->n;
}

/* an allocation against a parameter and against a global; another against it once its address is stored */
NI static float fresh(const dev *f, int n)
{
    float *t = malloc((size_t)n * sizeof *t), *u = malloc((size_t)n * sizeof *u), s = 0;
    int i;
    for (i = 0; i < n; i++) {
        t[i] = f->h * (float)i + gs;
        u[i] = t[i] + f->w;
    }
    gp = u;
    for (i = 0; i < n; i++) {
        gp[i] += 1;
        s += u[i] + t[i];
    }
    free(t);
    free(u);
    return s;
}

NI static void bump(void)
{
    gs += 1;
    if (gp)
        gp[0] += 10;
    if (glp)
        glp[1] += 100;
    if (gdev)
        gdev->h += 1;
}

/* calls: a local they cannot reach, a global and an escaped local they can */
NI static float calls(int n)
{
    float keep[8], seen[8], s = 0;
    int i;
    for (i = 0; i < 8; i++)
        keep[i] = seen[i] = (float)i;
    gp = seen;
    for (i = 0; i < n; i++) {
        bump();
        s += keep[i & 7] + seen[0] + gs;
        keep[(i + 1) & 7] += 1;
    }
    gp = 0;
    return s;
}

/* an address made by arithmetic on integers */
NI static long through_integer(unsigned long a, int n)
{
    long loc[4], s = 0;
    int i;
    for (i = 0; i < 4; i++)
        loc[i] = i;
    glp = loc;
    a += (unsigned long)glp;
    for (i = 0; i < n; i++) {
        *(long *)(a + 8) += 2; /* loc[1] when a was 0 */
        s += loc[1];
        bump();
        s += loc[1];
    }
    glp = 0;
    return s;
}

/* a pointer chosen between a local and a parameter */
NI static int chosen(int *p, int which, int n)
{
    int loc[4] = {1, 2, 3, 4}, *q = which ? loc : p, s = 0, i;
    for (i = 0; i < n; i++) {
        q[i & 3] += 5;
        s += loc[i & 3] + p[i & 3];
    }
    return s;
}

/* a block copied or cleared into one object leaves what is known of another */
NI static float blocks(dev *f, const float *src)
{
    float t[8], h0 = f->h, s;
    memcpy(t, src, sizeof t);
    s = f->h + t[3];
    memset(f, 0, sizeof *f); /* f->h is 0 now */
    s += f->h + h0;
    memcpy(t, src + 1, sizeof t);
    return s + t[0];
}

/* the structure itself is the target of the stores */
NI static float self_store(dev *f, int n)
{
    float *o = (float *)&f->h, s = 0;
    int i;
    for (i = 0; i < n; i++) {
        o[0] += 1;
        s += f->h;
    }
    return s;
}

int main(void)
{
    float d[32], src[32], out[8];
    long ic[4];
    int ia[4] = {10, 20, 30, 40};
    dev f;
    int i;
    for (i = 0; i < 32; i++) {
        d[i] = (float)(i * i % 7);
        src[i] = (float)i * 0.5f;
    }
    memset(&f, 0, sizeof f);
    f.lz = 2; f.ly = 3; f.lx = 4; f.h = 0.25f; f.w = 2; f.delta = src; f.out = out; f.n = 7;
    printf("local_vs_param %g\n", local_vs_param(&f, 20, 5));
    printf("local_escaped %g\n", local_escaped(12));
    printf("local_vs_loaded %g\n", local_vs_loaded(&f, 20));
    printf("local_through_struct %g\n", local_through_struct(&f, 20));
    f.out = out;
    global_vs_global();
    printf("global_vs_global %g %g\n", tab[1], tab[4]);
    printf("param_vs_global %g\n", param_vs_global(garr, 3));
    printf("param_vs_param %g %g\n", param_vs_param(d, d, 4), param_vs_param(d + 8, d, 4));
    printf("restrict_param %g\n", restrict_param(d, src, &f, 16));
    printf("restrict_escaped %g\n", restrict_escaped(d, 5));
    printf("long_vs_pointer %g\n", long_vs_pointer(&f, ic, 3));
    printf("fresh %g\n", fresh(&f, 10));
    printf("calls %g\n", calls(10));
    printf("through_integer %ld\n", through_integer(0, 3));
    printf("chosen %d %d\n", chosen(ia, 0, 6), chosen(ia, 1, 6));
    printf("blocks %g\n", blocks(&f, src));
    f.h = 0.25f;
    gdev = &f;
    bump();
    printf("self_store %g %g\n", self_store(&f, 4), f.h);
    return 0;
}
