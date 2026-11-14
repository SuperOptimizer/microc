/* more loops for the vectoriser: maxima and minima of narrow elements (a select of the two values compared),
   the instructions each element width has or lacks on x86-64, and the shapes of ufsm's hot loops */
#include <stdio.h>
#include <math.h>

#define N 77
static unsigned char ua[N + 40], ub[N + 40], uc[N + 40];
static signed char sa[N + 40], sb[N + 40];
static unsigned short wa[N + 40], wb[N + 40];
static short ha[N + 40], hb[N + 40];
static int ia[N + 40], ib[N + 40];
static unsigned qa[N + 40], qb[N + 40];
static float fa[N + 40], fb[N + 40], fc[N + 40];
static double da[N + 40], db[N + 40], dc[N + 40];
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
static void fdivv(float *o, const float *a, const float *b, int n) { int x; for (x = 0; x < n; x++) o[x] = a[x] / (b[x] + 1.f); }
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

/* elements of different sizes in one loop (x86-64: lanes of four bytes, the narrow ones widened and narrowed) */
static void u8tof(float *o, const unsigned char *s, float m, float k, int n) { int x; for (x = 0; x < n; x++) o[x] = ((float)s[x] - m) * k; }
static void ftou8(unsigned char *o, const float *p, int n)
{ int j; for (j = 0; j < n; j++) { const float v = p[j] + 0.5f; o[j] = v <= 0.f ? 0 : v >= 255.f ? 255 : (unsigned char)v; } }
static void gtmask(unsigned char *o, const float *p, float t, int n) { int j; for (j = 0; j < n; j++) o[j] = p[j] > t; }
static void orband(unsigned char *o, const float *p, float t, float b, int n) { int j; for (j = 0; j < n; j++) o[j] |= fabsf(p[j] - t) < b; }
static void s16toi(int *o, const short *a, const unsigned short *b, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] * 3 + b[i]; }
static void i2s(short *o, const int *a, int n) { int i; for (i = 0; i < n; i++) o[i] = (short)(a[i] >> 4); }
static void s8tou16(unsigned short *o, const signed char *a, int n) { int i; for (i = 0; i < n; i++) o[i] = (unsigned short)(a[i] * 7); }
static void zx(int *o, const int *a, int n)
{ int i; for (i = 0; i < n; i++) o[i] = (unsigned char)a[i] + (signed char)(a[i] >> 8) + (unsigned short)(a[i] * 3) + (short)a[i]; }
static void bthresh(unsigned char *o, const unsigned char *a, int t, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] > t ? 255 : a[i] / 2; }
static int cnt(const float *a, float t, int n) { int i, c = 0; for (i = 0; i < n; i++) c += a[i] < t; return c; }
static int bsumw(const unsigned char *a, const unsigned char *b, int n) { int i, s = 0; for (i = 0; i < n; i++) s += a[i] * b[i]; return s; }

/* the mathematical library lane by lane (sqrt with -fno-math-errno or -ffast-math; the roundings with SSE4.1) */
static void roots(float *o, const float *a, int n)
{ int i; for (i = 0; i < n; i++) o[i] = sqrtf(fabsf(a[i])) + floorf(a[i] * 0.3f) - ceilf(a[i]) + truncf(a[i] * 1.7f) + rintf(a[i] * 0.5f); }
static void droots(double *o, const double *a, int n) { int i; for (i = 0; i < n; i++) o[i] = sqrt(fabs(a[i])) * floor(a[i]) + nearbyint(a[i]); }
static unsigned long lr(const double *a, const float *b, int n)
{ int i; unsigned long s = 0; for (i = 0; i < n; i++) s = s * 3 + (unsigned long)lrint(a[i] * 2.5) + (unsigned long)lrintf(b[i] * 1.5f); return s; }

/* a[i + k]: offsets folded into where each array starts; overlapping ones checked when the loop is entered */
static void stencil(float *o, const float *a, int k, int n) { int i; for (i = 1; i < n; i++) o[i] = a[i - 1] * 0.25f + a[i + 1] * 0.5f + a[i + k]; }
static void shiftw(int *o, const int *a, int k, int n) { int i; for (i = 0; i < n; i++) o[i + k] = a[i] * 3 - a[k + i]; }
static void fwd(int *a, int n) { int i; for (i = 0; i < n; i++) a[i] = a[i + 1] + 1; }
static void rec(int *a, int n) { int i; for (i = 0; i < n; i++) a[i + 4] = a[i] + 1; }
static void rec2(int *a, int k, int n) { int i; for (i = 0; i < n; i++) a[i + k] = a[i] * 2 + 1; }

static void plane(float *o, const float *a, unsigned long z, unsigned long m, unsigned long n)
{ unsigned long j; for (j = 0; j < n; j++) o[z * m + j] += 0.5f * a[(z + 1) * m + j] - a[j + 3]; }
static void bplane(unsigned char *o, const unsigned char *a, long z, long n) { long j; for (j = 0; j < n; j++) o[z + j] = (unsigned char)(a[j + z * 2] + 1); }

/* gathers through an index computed lane by lane (x86-64 with SSE4.1), the range read checked against what is written */
static void clamprow(float *row, const unsigned char *src, int P, int r)
{ int x; for (x = -r; x < P + r; x++) row[x + r] = src[x < 0 ? 0 : x >= P ? P - 1 : x]; }
static void half(unsigned char *dst, const unsigned char *row, int o, int lo, int P)
{ int x; for (x = 0; x < P; x++) dst[x] = row[((o + x + 1) >> 1) - lo]; }
static void fgat(float *d, const float *a, int k, int n) { int i; for (i = 0; i < n; i++) d[i] = a[(i * 3 + k) >> 1] * 2.f + 1.f; }
static void sgat(int *d, const short *a, int n) { int i; for (i = 0; i < n; i++) d[i] = a[n - 1 - i > i ? i : n - 1 - i] + d[i]; }
static void selfgat(int *a, int n) { int i; for (i = 0; i < n; i++) a[i + 8] = a[(i >> 1) + 5] + 1; }

/* -ffast-math: floating-point maxima and minima, and sums, reordered (exact here: small multiples of a quarter) */
static float fmx(const float *a, int n) { int i; float m = -1e30f; for (i = 0; i < n; i++) m = a[i] > m ? a[i] : m; return m; }
static double dmn(const double *a, int n) { int i; double m = 1e300; for (i = 0; i < n; i++) if (a[i] < m) m = a[i]; return m; }
static float fsum2(const float *a, const float *b, int n) { int i; float s = 0.f; for (i = 0; i < n; i++) s += a[i] * b[i]; return s; }

/* i <= n */
static void taps(float *w, const float *e, float s, int r) { int k; for (k = 0; k <= 2 * r; k++) w[k] = e[k] / s; }
static void ule(unsigned *d, const unsigned *a, unsigned lo, unsigned hi) { unsigned i; for (i = lo; i <= hi; i++) d[i] = a[i] * 5u + i; }

/* two bounds: i < a && i < b is i < min (a, b) */
static void tb1(unsigned char *o, const float *p, int s, int a, int b) { int i; for (i = s; i < a && i < b; i++) o[i] = p[i] > 0.5f; }
static void tb2(int *o, const int *p, int s, int a, int b) { int i; for (i = s; i <= a && i <= b; i++) o[i] = p[i] * 3 + i; }
static void tb3(unsigned *o, const unsigned *p, unsigned s, unsigned a, unsigned b) { unsigned i; for (i = s; i < a && i < b; i++) o[i] = p[i] ^ 0x5au; }
static void tb4(float *o, const float *p, long s, long a, long b) { long i; for (i = s; i < a && i < b; i++) o[i] = p[i] * 2.f - 1.f; }

/* a choice fixed for the loop; the same read in both arms; reads of fixed places; a short inner loop unrolled */
static void isel(float *w, const float *q, int cn, int n) { int i; const int bz = cn >> 2, bx = cn & 1; for (i = 0; i < n; i++) w[i] = (bz ? q[i] : 1 - q[i]) * (bx ? q[i + 1] : 2 - q[i + 1]); }
static void isel64(double *w, const double *q, long c, int n) { int i; for (i = 0; i < n; i++) w[i] = c ? q[i] * 2 : q[i] - 1; }
static void linv8(float *o, const float *m, const float *a, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] * m[0] + m[1] - a[i] * m[2] + m[3] * m[4] - m[5] + m[6] * a[i] + m[7]; }
static void u3(float *U, const float *A, const float *w, int n, float c) { int i, d; for (i = 0; i < n; i++) for (d = 0; d < 3; d++) U[d * 39 + i] = c + A[d * 3] * w[i] + A[d * 3 + 1]; }

/* running maxima and minima of floats with NaNs among them (exact even with -ffast-math -fno-finite-math-only) */
static float fmxi(const float *a, float m, int n) { int i; for (i = 0; i < n; i++) m = a[i] > m ? a[i] : m; return m; }
static float fmni(const float *a, float m, int n) { int i; for (i = 0; i < n; i++) if (a[i] <= m) m = a[i]; return m; }
static double dmxl(const double *a, double m, int n) { int i; for (i = 0; i < n; i++) m = m > a[i] ? m : a[i]; return m; }
/* lane by lane maxima / minima of floats as selects, NaNs and zeros of both signs among them: maxps / minps exactly */
static void lmaxf(float *o, const float *a, const float *b, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] > b[i] ? a[i] : b[i]; }
static void fclamp4(float *o, const float *a, int n) { int i; for (i = 0; i < n; i++) { float v = a[i] < 0.f ? 0.f : a[i]; o[i] = 4.f < v ? 4.f : v; } }
static void lmind(double *o, const double *a, const double *b, int n) { int i; for (i = 0; i < n; i++) o[i] = b[i] < a[i] ? b[i] : a[i]; }
/* division by a value fixed for the loop: by a power of two it is a multiplication, exactly (others only with -freciprocal-math) */
static void divc(float *o, const float *a, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] / 0.25f + a[i] / 4.f; }
static void divv(float *o, const float *a, float c, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] / c; }
static void ddivc(double *o, const double *a, int n) { int i; for (i = 0; i < n; i++) o[i] = a[i] / 1024.0 - a[i] / 0x1p-1000; }
static float qnanf(void) { union { unsigned u; float f; } x; x.u = 0x7fc00001u; return x.f; } /* (one NaN, whatever makes it) */
static double qnan(void) { union { unsigned long u; double d; } x; x.u = 0x7ff8000000000001ul; return x.d; }
static double nn(double x) { return x != x ? 12345.0 : x; } /* (printf writes a NaN's sign) */

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
        dlerp(da, da, db, 0.375, n); fdivv(fa, fa, fb, n); fneg2(fb + 3, fb, n); dneg(db, da, n);
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
        u8tof(fc, ua, 100.f, 0.125f, n); ftou8(ub, fc, n); ftou8(uc + 1, fb, n); gtmask(ua + 2, fa, 1.5f, n); orband(ua, fb, 3.f, 1.25f, n);
        s16toi(ib, ha, wa, n); i2s(hb, ia, n); s8tou16(wb, sa, n); zx(ia, ib, n); bthresh(sb, (unsigned char *)sa, 77, n);
        printf("   %lx %lx %lx %lx %lx %lx %d %d\n", check(fc, sizeof fc), check(ua, sizeof ua) ^ check(ub, sizeof ub), check(uc, sizeof uc),
               check(ib, sizeof ib) ^ check(hb, sizeof hb), check(wb, sizeof wb) ^ check(ia, sizeof ia), check(sb, sizeof sb), cnt(fb, 9.f, n),
               bsumw(ua, ub, n));
        for (i = 0; i < N + 40; i++) { fa[i] = (float)((i * 37) % 41) * 0.7f - 13.f; da[i] = (double)((i * 11) % 23) * 0.9 - 9.0; }
        roots(fc, fa, n); droots(db, da, n);
        stencil(fb, fa, 3, n); shiftw(ib, ia, 5, n); fwd(ia, n); rec(ia + 2, n); rec2(ib, 2, n / 2); rec2(ib + 40, 9, n / 2);
        clamprow(fc, ua, n / 2, 3); half(ub + 2, ua, n, 1, n / 2); fgat(fb, fc, 2, n / 2); sgat(ib, ha, n); selfgat(ia, n / 2);
        selfgat(ia + 20, n / 3); half(ua, ua + 30, 3, 0, n / 3);
        printf("   %lx %lx %lx %lx %lx\n", check(fc, sizeof fc), check(ub, sizeof ub), check(fb, sizeof fb), check(ib, sizeof ib),
               check(ia, sizeof ia) ^ check(ua, sizeof ua));
        for (i = 0; i < N + 40; i++) { fa[i] = (float)((i * 29) % 37) * 0.25f - 4.f; fb[i] = (float)(i % 3); da[i] = (double)((i * 13) % 31) * 0.5 - 7.0; }
        taps(fc, fa, 4.f, n / 3); ule(qb, qa, 2, (unsigned)n / 2); ule(qa, qb, 5, (unsigned)n / 4);
        printf("   %lx %lx %lx\n", check(fc, sizeof fc), check(qa, sizeof qa), check(qb, sizeof qb));
        printf("   %g %g %g\n", (double)fmx(fa, n), dmn(da, n), (double)fsum2(fa, fb, n));
        plane(fb, fa, 1, 20, (unsigned long)n / 3); plane(fc, fb, 0, 9, (unsigned long)n / 2); bplane(ua, ub, 3, n / 2); bplane(ub, ub, 1, n / 3);
        printf("   %lx %lx %lx %lx %lx\n", check(fb, sizeof fb), check(ia, sizeof ia), check(ib, sizeof ib), check(fc, sizeof fc),
               check(ua, sizeof ua) ^ check(ub, sizeof ub));
        printf("   %lx %lx %lu\n", check(fc, sizeof fc), check(db, sizeof db), lr(da, fa, n));
        printf("   %d %u %d %d %d %d %ld %g %g %lx\n", imx(ia, n), umn(qa, n), busum(ua, n), bssum(sa, n), wusum(wa, n), hssum(ha, n),
               lsum2(la, n), (double)fsum(fa, n), dsum(da, n), check(fb, sizeof fb));
        tb1(ua, fa, 3, n, n / 2 + 5); tb1(ub, fb, 0, n - 7, -3); tb2(ia, ib, 1, n / 2, n - 2); tb2(ib, ia, -1 < n ? 0 : 1, n - 1, n / 3);
        tb3(qa, qb, 2, (unsigned)n, (unsigned)n / 2 + 1); tb3(qb, qa, 0, 0xffffffffu, (unsigned)n);
        tb4(fc, fa, 1, (long)n, (long)n * 2 / 3); tb4(fb, fc, -0L + 2, -5L, (long)n);
        printf("   %lx %lx %lx %lx\n", check(ua, sizeof ua) ^ check(ub, sizeof ub), check(ia, sizeof ia) ^ check(ib, sizeof ib),
               check(qa, sizeof qa) ^ check(qb, sizeof qb), check(fb, sizeof fb) ^ check(fc, sizeof fc));
        isel(fc, fa, 5, n); isel(fb, fa, 2, n); isel64(db, da, n & 1, n); isel64(da, db, (long)n << 33, n);
        linv8(fc, fb, fa, n); linv8(fb + 3, fb, fa, n / 2); linv8(fa, fa + n / 2, fb, n);
        u3(fb, fa, fc, n / 2, 0.5f); u3(fa, fa + 1, fb, n / 3, -1.f);
        printf("   %lx %lx %lx %lx\n", check(fa, sizeof fa), check(fb, sizeof fb), check(fc, sizeof fc), check(da, sizeof da) ^ check(db, sizeof db));
        for (i = 0; i < N + 40; i++) { fa[i] = (float)((i * 37) % 53) - 20.5f; da[i] = db[i] = (double)((i * 11) % 29) - 9.25; }
        for (i = 3; i < N + 40; i += 7) { fa[i] = __builtin_nanf(""); da[i] = __builtin_nan(""); }
        printf("   %g %g %g %g %g %g\n", nn(fmxi(fa, -1e30f, n)), nn(fmxi(fa, __builtin_nanf(""), n)), nn(fmni(fa, 1e30f, n)),
               nn(fmni(fa + 1, 3.5f, n - 1)), nn(dmxl(db, -1e300, n)), nn(dmxl(db, 2.0, n / 2))); /* (no NaN: -ffinite-math-only may vectorise it) */
        for (i = 0; i < N + 40; i++) {
            fb[i] = i % 5 == 0 ? -0.f : i % 5 == 1 ? 0.f : (float)((i * 7) % 11) - 5.f; db[i] = i % 4 == 0 ? -0.0 : i % 4 == 1 ? 0.0 : (double)((i * 5) % 9) - 4;
            fa[i] = i % 6 == 0 ? 0.f : i % 6 == 1 ? -0.f : (float)((i * 3) % 13) - 6.f; da[i] = i % 3 == 0 ? 0.0 : (double)((i * 7) % 5) - 2;
        }
        for (i = 4; i < N + 40; i += 9) { fa[i] = qnanf(); db[i] = qnan(); }
        for (i = 2; i < N + 40; i += 13) fb[i] = qnanf();
        lmaxf(fc, fa, fb, n); lmind(dc, da, db, n);
        printf("   %lx %lx", check(fc, sizeof fc), check(dc, sizeof dc));
        fclamp4(fc, fa, n); fclamp4(fb, fb, n);
        printf(" %lx %lx\n", check(fc, sizeof fc), check(fb, sizeof fb));
        for (i = 0; i < N + 40; i++) { fa[i] = (float)((i * 37) % 53) * 1.37e-38f + (float)(i % 7) * 3.1f; da[i] = (double)((i * 7) % 31) * 1.1e-300 - i; }
        divc(fb, fa, n); divv(fc, fa, 8.f, n); ddivc(db, da, n);
        printf("   %lx %lx %lx\n", check(fb, sizeof fb), check(fc, sizeof fc), check(db, sizeof db));
    }
    return 0;
}
