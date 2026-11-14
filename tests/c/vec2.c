/* more loops for the vectoriser: maxima and minima of narrow elements (a select of the two values compared),
   the instructions each element width has or lacks on x86-64, and the shapes of ufsm's hot loops */
#include <stdio.h>

#define N 77
static unsigned char ua[N + 40], ub[N + 40], uc[N + 40];
static signed char sa[N + 40], sb[N + 40];
static unsigned short wa[N + 40], wb[N + 40];
static short ha[N + 40], hb[N + 40];
static int ia[N + 40], ib[N + 40];
static unsigned qa[N + 40], qb[N + 40];
static float fa[N + 40], fb[N + 40], fc[N + 40];
static double da[N + 40], db[N + 40];
static long la[N + 40];

/* papyrus.h: running maxima and minima of byte rows */
static void bmax(unsigned char *g, const unsigned char *p, const unsigned char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] > s[j] ? p[j] : s[j]; }
static void bmin(unsigned char *g, const unsigned char *p, const unsigned char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] < s[j] ? p[j] : s[j]; }
static void bmin2(unsigned char *g, const unsigned char *p, const unsigned char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = s[j] >= p[j] ? p[j] : s[j]; }
static void bclamp(unsigned char *g, const unsigned char *p, int m)
{ int j; for (j = 0; j < m; j++) { int v = p[j] > 200 ? 200 : p[j]; g[j] = (unsigned char)(v < 17 ? 17 : v); } }
static void scmax(signed char *g, const signed char *p, const signed char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] > s[j] ? p[j] : s[j]; }
static void scminu(signed char *g, const signed char *p, const signed char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = (unsigned char)p[j] < (unsigned char)s[j] ? p[j] : s[j]; }
static void wmax(unsigned short *g, const unsigned short *p, const unsigned short *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] > s[j] ? p[j] : s[j]; }
static void hmin(short *g, const short *p, const short *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] < s[j] ? p[j] : s[j]; }
static void imax2(int *g, const int *p, const int *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] > s[j] ? p[j] : s[j]; }
static void umin2(unsigned *g, const unsigned *p, const unsigned *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = p[j] < s[j] ? p[j] : s[j]; }
static void iclamp(int *g, const int *p, int lo, int hi, int m)
{ int j; for (j = 0; j < m; j++) { int v = p[j] < lo ? lo : p[j]; g[j] = v > hi ? hi : v; } }
/* near misses: the sum is wider than a byte, so its comparison is not one of bytes */
static void bsummax(unsigned char *g, const unsigned char *p, const unsigned char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = (unsigned char)(p[j] + s[j] > s[j] ? p[j] + s[j] : s[j]); }
static void bmixed(unsigned char *g, const unsigned char *p, const signed char *s, int m)
{ int j; for (j = 0; j < m; j++) g[j] = (unsigned char)(p[j] > s[j] ? p[j] : s[j]); }

/* papyrus.h: o[x] += w * q[x] */
static void axpy(float *o, const float *q, float w, int n) { int x; for (x = 0; x < n; x++) o[x] += w * q[x]; }
static void zero(float *o, int n) { int x; for (x = 0; x < n; x++) o[x] = 0.f; }
static void fclamp(float *o, const float *q, int n) { int x; for (x = 0; x < n; x++) o[x] = q[x] < 0.f ? 0.f : q[x] > 255.f ? 255.f : q[x]; }
static void thresh(int *o, const float *q, float t, int n) { int x; for (x = 0; x < n; x++) o[x] = q[x] > t; }
static void dlerp(double *o, const double *a, const double *b, double t, int n)
{ int x; for (x = 0; x < n; x++) o[x] = a[x] + (b[x] - a[x]) * t; }
static void fdiv(float *o, const float *a, const float *b, int n) { int x; for (x = 0; x < n; x++) o[x] = a[x] / (b[x] + 1.f); }
static void fneg2(float *o, const float *a, int n) { int x; for (x = 0; x < n; x++) o[x] = -a[x]; }
static void dneg(double *o, const double *a, int n) { int x; for (x = 0; x < n; x++) o[x] = -a[x] * 2.0; }

/* each width's arithmetic */
static void wmul(unsigned short *d, const unsigned short *a, const unsigned short *b, int n)
{ int i; for (i = 0; i < n; i++) d[i] = (unsigned short)((unsigned)a[i] * b[i] + 3u); }
static void imul(int *d, const int *a, int k, int n) { int i; for (i = 0; i < n; i++) d[i] = a[i] * k - (a[i] >> 3); }
static void wshift(unsigned short *d, const unsigned short *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (unsigned short)((a[i] >> 3) | (a[i] << 9)); }
static void hsar(short *d, const short *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (short)(a[i] >> 2); }
static void ineg(int *d, const int *a, int n) { int i; for (i = 0; i < n; i++) d[i] = -a[i]; }
static void hneg(short *d, const short *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (short)-a[i]; }
static void bneg(unsigned char *d, const unsigned char *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (unsigned char)(-a[i] ^ 5); }
static void lsar(long *d, int n) { int i; for (i = 0; i < n; i++) d[i] = (d[i] >> 5) + (long)((unsigned long)d[i] << 1); }
static void bshr(unsigned char *d, const unsigned char *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (unsigned char)(a[i] >> 1); }
static void ucmp(int *d, const unsigned *a, const unsigned *b, int n)
{ int i; for (i = 0; i < n; i++) d[i] = a[i] < b[i] ? 1 : a[i] >= 3000000000u ? -1 : a[i] > b[i] ? 2 : a[i] <= 5u ? 4 : 0; }
static void scmp(int *d, const int *a, const int *b, int n)
{ int i; for (i = 0; i < n; i++) d[i] = a[i] < b[i] ? a[i] : a[i] >= b[i] + 3 ? 2 : a[i] <= -7 ? 4 : a[i] != b[i] ? 8 : a[i] == 9 ? 16 : b[i]; }
static void dcmp(long *d, const double *a, const double *b, int n)
{ int i; for (i = 0; i < n; i++) d[i] = a[i] >= b[i] ? 1 : a[i] == b[i] ? 2 : -1; }
static void tofloat(float *d, const int *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (float)a[i] * 0.125f; }
static void toint(int *d, const float *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (int)(a[i] * 3.0f); }
static void ufloat(float *d, const unsigned *a, int n) { int i; for (i = 0; i < n; i++) d[i] = (float)a[i]; }

/* sums and extremes */
static int imx(const int *p, int n) { int i, m = -5; for (i = 0; i < n; i++) m = p[i] > m ? p[i] : m; return m; }
static unsigned umn(const unsigned *p, int n) { int i; unsigned m = 4000000000u; for (i = 0; i < n; i++) m = p[i] < m ? p[i] : m; return m; }
static int busum(const unsigned char *p, int n) { int i, s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static int bssum(const signed char *p, int n) { int i, s = 3; for (i = 0; i < n; i++) s += p[i]; return s; }
static int wusum(const unsigned short *p, int n) { int i, s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static int hssum(const short *p, int n) { int i, s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static long lsum2(const long *p, int n) { int i; long s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static float fsum(const float *p, int n) { int i; float s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }
static double dsum(const double *p, int n) { int i; double s = 0; for (i = 0; i < n; i++) s += p[i]; return s; }

/* many values at once: more than x86-64 has vector registers for */
static void wide(float *o, const float *a, const float *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        o[i] = (a[i] * 1.5f + b[i] * 2.5f) * (a[i] - 3.5f) + (b[i] + 4.5f) * (a[i] * 5.5f - b[i] * 6.5f) + (a[i] + 7.5f) * (b[i] - 8.5f) +
               (a[i] * 9.5f) * (b[i] * 10.5f) + 11.5f * (a[i] + b[i] + 12.5f);
}

static unsigned long check(const void *p, unsigned long n)
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
    for (n = 0; n <= N; n += n < 20 ? 1 : 19) {
        for (i = 0; i < N + 40; i++) {
            ua[i] = (unsigned char)(i * 37 + n); ub[i] = (unsigned char)(i * 101 + 7); uc[i] = 0;
            sa[i] = (signed char)(i * 53 - n); sb[i] = (signed char)(i * 29 + 100);
            wa[i] = (unsigned short)(i * 4111 + n); wb[i] = (unsigned short)(i * 30011);
            ha[i] = (short)(i * 3001 - 20000); hb[i] = (short)(i * 1237 + n);
            ia[i] = i * 100003 - 3000000 + n; ib[i] = (i * 7919) % 2001 - 1000;
            qa[i] = (unsigned)i * 123456789u; qb[i] = (unsigned)i * 987654321u + (unsigned)n;
            fa[i] = (float)(i % 17) * 0.75f - 4.0f; fb[i] = (float)(i % 5) * 0.5f - 1.0f; fc[i] = 1.0f;
            da[i] = (i * 13) % 7 - 2.5; db[i] = (i * 3) % 7 - 1.5;
            la[i] = (long)i * 1000000007L - 40000000000L;
        }
        if (n > 9) { qa[9] = qb[9]; ia[9] = 9; ib[9] = 9; da[9] = db[9]; }
        bmax(uc, ua, ub, n); bmin(uc + 1, uc, ub, n); bmin2(ua, ub, uc, n); bclamp(ub, ua + 2, n);
        scmax(sa, sa, sb, n); scminu(sb, sa + 3, sb, n);
        wmax(wa, wa, wb, n); hmin(hb, ha, hb, n);
        imax2(ib, ia, ib, n); umin2(qb, qa, qb, n); iclamp(ia, ib, -300, 700, n);
        bsummax(ub, ua, uc, n); bmixed(uc, ub, sa, n);
        printf("%d: %lx %lx %lx %lx %lx %lx\n", n, check(ua, sizeof ua), check(ub, sizeof ub), check(uc, sizeof uc),
               check(sa, sizeof sa) ^ check(sb, sizeof sb), check(wa, sizeof wa) ^ check(hb, sizeof hb),
               check(ia, sizeof ia) ^ check(ib, sizeof ib) ^ check(qb, sizeof qb));
        zero(fc, n / 2); axpy(fc, fa, 0.25f, n); axpy(fc + 1, fc, -0.5f, n); fclamp(fb, fc, n); thresh(ia, fa, 0.5f, n);
        dlerp(da, da, db, 0.375, n); fdiv(fa, fa, fb, n); fneg2(fb + 3, fb, n); dneg(db, da, n);
        printf("   %lx %lx %lx %lx %lx\n", check(fa, sizeof fa), check(fb, sizeof fb), check(fc, sizeof fc), check(ia, sizeof ia),
               check(da, sizeof da) ^ check(db, sizeof db));
        wmul(wb, wa, wb, n); imul(ib, ia, -77, n); wshift(wa, wb, n); hsar(ha, hb, n); ineg(ia + 1, ia, n); hneg(hb, ha, n);
        bneg(ua, ub, n); lsar(la, n); bshr(ub, ua, n); ucmp(ib, qa, qb, n); scmp(ia, ia, ib, n); dcmp(la + 1, da, db, n);
        tofloat(fa, ia, n); toint(ib, fb, n); ufloat(fc, qa, n);
        printf("   %lx %lx %lx %lx %lx %lx %lx\n", check(wa, sizeof wa) ^ check(wb, sizeof wb), check(ha, sizeof ha) ^ check(hb, sizeof hb),
               check(ia, sizeof ia), check(ib, sizeof ib), check(ua, sizeof ua) ^ check(ub, sizeof ub), check(la, sizeof la),
               check(fa, sizeof fa) ^ check(fc, sizeof fc));
        for (i = 0; i < N + 40; i++) {
            ua[i] = (unsigned char)(i * 97 + 13 + n); sa[i] = (signed char)(i * 89 - n);
            wa[i] = (unsigned short)(i * 4099 + 60000); ha[i] = (short)(i * 4099 - 30000);
            fa[i] = (float)(i % 8) * 0.5f; da[i] = (double)(i % 6) * 0.25;
        }
        wide(fb, fa, fc, n);
        printf("   %d %u %d %d %d %d %ld %g %g %lx\n", imx(ia, n), umn(qa, n), busum(ua, n), bssum(sa, n), wusum(wa, n), hssum(ha, n),
               lsum2(la, n), (double)fsum(fa, n), dsum(da, n), check(fb, sizeof fb));
    }
    return 0;
}
