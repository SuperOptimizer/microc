/* loops the vectoriser handles at -O3, and near misses it must leave alone */
#include <stdio.h>
#include <string.h>

#define N 103
static float fa[N + 8], fb[N + 8];
static double da[N + 8], db[N + 8];
static int ia[N + 8], ib[N + 8], ic[N + 8];
static unsigned char ca[N + 8], cb[N + 8];
static short sa[N + 8], sb[N + 8];
static long la[N + 8], lb[N + 8];

static void saxpy(float *y, const float *x, float a, int n) { int i; for (i = 0; i < n; i++) y[i] = y[i] + a * x[i]; }
static void dscale(double *y, const double *x, double a, long n) { long i; for (i = 0; i < n; i++) y[i] = x[i] * a - y[i] / 4.0; }
static int isum(const int *p, int n) { int i, s = 7; for (i = 0; i < n; i++) s += p[i]; return s; }
static long lsum(const long *p, const long *q, int n) { int i; long s = 0; for (i = 0; i < n; i++) s += p[i] ^ q[i]; return s; }
static void imix(int *d, const int *a, const int *b, int k, int n) { int i; for (i = 0; i < n; i++) d[i] = (a[i] * b[i] + k) ^ (a[i] - 3); }
static void addb(unsigned char *d, const unsigned char *s, long n) { long i; for (i = 0; i < n; i++) d[i] = (unsigned char)(d[i] + s[i] * 3); }
static void adds(short *d, const short *s, int n) { int i; for (i = 0; i < n; i++) d[i] = (short)(d[i] - s[i]); }
static void fill(int *d, int v, int n) { int i; for (i = 0; i < n; i++) d[i] = v; }
static void shifted(int *d, int n) { int i; for (i = 1; i < n; i++) d[i] = d[i - 1] + 1; } /* carries a dependence */
static void from(int *d, const int *s, int lo, int hi) { int i; for (i = lo; i < hi; i++) d[i] = s[i] + 1; }

static void ushift(unsigned *d, const unsigned *s, unsigned long n) { unsigned long i; for (i = 0; i < n; i++) d[i] = (s[i] >> 3) + (s[i] << 5); }
static void sshift(int *d, const int *s, unsigned n) { unsigned i; for (i = 0; i < n; i++) d[i] = (s[i] >> 2) - d[i] * 8; }
static void fneg(float *d, const float *s, int n) { int i; for (i = 0; i != n; i++) d[i] = -s[i] - d[i]; }
static void lneg(long *d, const long *s, int n) { int i; for (i = 0; i != n; i++) d[i] = -(s[i] << 7) | 1; }
static void bshl(unsigned char *d, const unsigned char *s, long n) { long i; for (i = 0; i < n; i++) d[i] = (unsigned char)(d[i] + s[i] * 4); }

static void pcopy(int *d, const int *s, int n) { while (n--) *d++ = *s++ + 1; }
static void ptriple(int *d, const int *e) { for (; d < e; d++) *d = *d * 3; }
static int psum(const int *p, const int *e) { int s = 0; while (p != e) s += *p++; return s; }
static void pbytes(unsigned char *d, const unsigned char *s, unsigned long n) { do { *d++ ^= *s++; } while (--n); }
static void pshort(short *d, const short *s, const short *e) { while (s < e) { *d = (short)(*d + *s); d++; s++; } }
static void ppre(double *d, const double *s, int n) { d--; s--; while (n-- > 0) *++d = *++s * 0.5; }
static void twoidx(int *d, const int *s, int n) { int i, j = 5; for (i = 0; i < n; i++, j += 3) d[i] = s[i] - 2; ic[0] += j; }

static void relu(float *d, const float *s, int n) { int i; for (i = 0; i < n; i++) d[i] = s[i] > 3.0f ? s[i] : 0; }
static int imax(const int *p, int n) { int i, m = p[0]; for (i = 1; i < n; i++) if (p[i] > m) m = p[i]; return m; }
static int imin(const int *p, int n) { int i, m = 12345; for (i = 0; i < n; i++) if (m >= p[i]) m = p[i]; return m; }
static unsigned umax(const unsigned *p, const unsigned *e) { unsigned m = 0; for (; p != e; p++) m = *p > m ? *p : m; return m; }
static void conv(float *d, const int *s, int n) { int i; for (i = 0; i < n; i++) d[i] = (float)s[i] * 0.5f; }
static void back(int *d, const float *s, int n) { int i; for (i = 0; i < n; i++) d[i] = (int)(s[i] * 1.5f) + 1; }
static void dconv(double *d, const long *s, int n) { int i; for (i = 0; i < n; i++) d[i] = (double)s[i]; }
static void absd(int *d, const int *a, const int *b, int n) { int i; for (i = 0; i < n; i++) d[i] = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]; }
static void pick(long *d, const long *a, long k, int n) { int i; for (i = 0; i < n; i++) d[i] = a[i] != k ? (a[i] <= 9 ? 9 : a[i]) : -1; }
static void fsel(double *d, const double *a, const double *b, int n) { int i; for (i = 0; i < n; i++) d[i] = a[i] != b[i] ? (a[i] <= b[i] ? a[i] : b[i]) : 0.25; }
static void usel(unsigned *d, const unsigned *a, unsigned k, int n) { int i; for (i = 0; i < n; i++) d[i] = a[i] < k ? k - a[i] : a[i] >= 4000000000u ? 1 : a[i]; }

static void iota(int *d, int k, int n) { int i; for (i = 0; i < n; i++) d[i] = i * k + 3; }
static void ramp(float *d, int lo, int hi) { int i; for (i = lo; i < hi; i++) d[i] = (float)i * 0.25f; }
static void lramp(long *d, const long *s, long n) { long i, j = 100; for (i = 0; i < n; i++, j -= 7) d[i] = s[i] > 40 ? j : -j; }
static void mark(int *d, int n) { int i; for (i = 0; i < n; i++) d[i] = d[i] == i ? -1 : d[i] ^ i; }

static int bsum(const unsigned char *p, int n) { int i, s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static int scsum(const signed char *p, const signed char *e) { int s = 1; while (p < e) s += *p++; return s; }
static int ssum(const short *p, int n) { int s = -5; while (n-- > 0) s += *p++; return s; }
static unsigned ussum(const unsigned short *p, unsigned n) { unsigned i, s = 0; for (i = 0; i != n; i++) s += p[i]; return s; }

struct vecs { int n; int k; int *p; float scale; float *f; };
static void field(struct vecs *v) { int i; for (i = 0; i < v->n; i++) v->p[i] += v->k; }
static void ffield(struct vecs *v, const float *s) { int i; for (i = 0; i < v->n; i++) v->f[i] = s[i] * v->scale; }
static int gn = 50, gk = 3;
static void glob(int *d) { int i; for (i = 0; i < gn; i++) d[i] = d[i] * gk + gn; }
static void self(int *d) { int i; for (i = 0; i < d[40]; i++) d[i] = d[i] + 1; } /* the bound is one of the elements written */

static unsigned long check(const void *p, size_t n)
{
    const unsigned char *b = p;
    unsigned long h = 5381;
    while (n--)
        h = h * 33 + *b++;
    return h;
}

int main(void)
{
    int i, n;
    for (i = 0; i < N + 8; i++) {
        fa[i] = (float)i * 0.5f; fb[i] = (float)(N - i);
        da[i] = i * 1.25; db[i] = 100.0 - i;
        ia[i] = i * 7 - 300; ib[i] = 1000 - i * 3; ic[i] = 0;
        ca[i] = (unsigned char)(i * 5); cb[i] = (unsigned char)(250 - i);
        sa[i] = (short)(i * 300); sb[i] = (short)(i * 77 - 4000);
        la[i] = (long)i << 33; lb[i] = -(long)i * 12345;
    }
    for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
        saxpy(fa, fb, 1.5f, n);
        dscale(da, db, 0.75, n);
        imix(ic, ia, ib, n, n);
        addb(ca, cb, n);
        adds(sa, sb, n);
        printf("%d: %d %ld\n", n, isum(ia, n), lsum(la, lb, n));
    }
    printf("%lx %lx %lx %lx %lx\n", check(fa, sizeof fa), check(da, sizeof da), check(ic, sizeof ic), check(ca, sizeof ca),
           check(sa, sizeof sa));
    /* overlapping arrays: must behave as the scalar loop does */
    saxpy(fa + 1, fa, 2.0f, 60);
    imix(ia + 2, ia, ib, 5, 70);
    addb(ca, ca + 3, 80);
    imix(ib, ib, ib, 1, N);
    printf("%lx %lx %lx %lx\n", check(fa, sizeof fa), check(ia, sizeof ia), check(ca, sizeof ca), check(ib, sizeof ib));
    fill(ic, 9, N);
    shifted(ic, 50);
    from(ic, ia, 40, 90);
    from(ic, ia, 95, 96);
    from(ic, ia, 99, 3);
    printf("%lx %d %d\n", check(ic, sizeof ic), ic[49], ic[102]);
    for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
        ushift((unsigned *)ic, (unsigned *)ia, (unsigned long)n);
        sshift(ib, ia, (unsigned)n);
        fneg(fa, fb, n);
        lneg(la, lb, n);
        bshl(ca, cb, n);
    }
    printf("%lx %lx %lx %lx %lx\n", check(ic, sizeof ic), check(ib, sizeof ib), check(fa, sizeof fa), check(la, sizeof la),
           check(ca, sizeof ca));
    for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
        pcopy(ic, ia, n);
        ptriple(ib, ib + n);
        printf("%d ", n ? psum(ic + 1, ic + n) : 0);
        if (n)
            pbytes(ca, cb, (unsigned long)n);
        pshort(sa, sb, sb + n);
        ppre(da, db, n);
        twoidx(ia, ic, n);
    }
    for (i = 0; i < N + 8; i++) {
        fa[i] = (float)((i * 37) % 23) - 7.5f; fb[i] = (float)(i % 5);
        da[i] = (i * 7) % 11; db[i] = (i * 3) % 11;
        ia[i] = (i * 7919) % 1000 - 500; ib[i] = (i * 104729) % 1000 - 500;
        la[i] = (i % 3) ? i - 20 : 77; lb[i] = (long)i * 1000003 - 50000000;
    }
    ia[61] = 2147483647; ia[5] = -2147483647 - 1; ib[3] = -7;
    for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
        relu(fb, fa, n);
        conv(fa, ib, n);
        back(ic, fa, n);
        dconv(da, lb, n);
        absd(ib, ia, ic, n);
        pick(lb, la, 77, n);
        fsel(db, da, db, n);
        usel((unsigned *)ic, (unsigned *)ia, 300u, n);
        printf("%d %d %u ", n ? imax(ia, n) : 0, imin(ia, n), umax((unsigned *)ia, (unsigned *)ia + n));
    }
    printf("\n%lx %lx %lx %lx %lx %lx %lx\n", check(fa, sizeof fa), check(fb, sizeof fb), check(ic, sizeof ic), check(da, sizeof da),
           check(ib, sizeof ib), check(lb, sizeof lb), check(db, sizeof db));
    for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
        iota(ia, n + 2, n);
        ramp(fb, n / 2, n);
        lramp(lb, la, n);
        mark(ia, n);
    }
    printf("%lx %lx %lx\n", check(ia, sizeof ia), check(fb, sizeof fb), check(lb, sizeof lb));
    for (i = 0; i < N + 8; i++) { ca[i] = (unsigned char)(i * 97 + 13); sa[i] = (short)(i * 4099 - 30000); }
    for (n = 0; n <= N; n += n < 20 ? 1 : 41)
        printf("%d %d %d %u ", bsum(ca, n), scsum((signed char *)ca, (signed char *)ca + n), ssum(sa, n),
               ussum((unsigned short *)sa, (unsigned)n));
    printf("\n");
    {
        struct vecs v;
        v.k = 7; v.p = ib; v.scale = 0.5f; v.f = fa;
        for (n = 0; n <= N; n += n < 20 ? 1 : 41) {
            v.n = n;
            field(&v);
            ffield(&v, fb);
            gn = n;
            glob(ic);
        }
        {
            /* the loop writes over its own bound: the structure lies among the elements */
            static long room[40];
            struct vecs *w = (struct vecs *)(room + 10);
            w->n = 60; w->k = -40; w->p = (int *)room;
            field(w);
            printf("%d %d %ld ", w->n, w->k, room[3]);
        }
        for (i = 0; i < 64; i++) ia[i] = 30 + (i % 9);
        self(ia);
        printf("%lx %lx %lx %lx\n", check(ib, sizeof ib), check(fa, sizeof fa), check(ic, sizeof ic), check(ia, sizeof ia));
    }
    pcopy(ic + 1, ic, 90); /* overlapping */
    pbytes(ca + 2, ca, 70);
    printf("\n%lx %lx %lx %lx %lx %lx\n", check(ic, sizeof ic), check(ib, sizeof ib), check(ca, sizeof ca), check(sa, sizeof sa),
           check(da, sizeof da), check(ia, sizeof ia));
    return 0;
}
