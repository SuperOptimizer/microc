/* search loops (the counter test and a test of elements; the first element that stops it): every shape against the
   scalar result, at every start alignment, length and stopping place */
#include <stdio.h>
#include <string.h>

#define M 300
static int ia[M + 8];
static float fa[M + 8];
static double da[M + 8];
static long la[M + 8];
static unsigned char ua[M + 32];
static signed char sa[M + 32];
static unsigned qa[M + 8];

static int find(const int *a, int n, int x) { int i = 0; while (i < n && a[i] != x) i++; return i; }
static int ffirst(const float *a, int n, float t) { int i; for (i = 0; i < n; i++) if (a[i] > t) break; return i; }
static int fnle(const float *a, int n, float t) { int i; for (i = 0; i < n; i++) if (!(a[i] <= t)) break; return i; }
static long dfind(const double *a, long n, double t) { long i = 0; while (i < n && a[i] < t) i++; return i; }
static unsigned long lfind(const long *a, unsigned long n, long x) { unsigned long i; for (i = 0; i < n; i++) if (a[i] == x) break; return i; }
static int bfind(const unsigned char *a, int n) { int i = 0; while (i < n && a[i] != 0) i++; return i; }
static int bfind2(const unsigned char *a, int n) { int i; for (i = 0; i < n; i++) if (a[i] == 10 || a[i] == 13) break; return i; }
static int sfind(const signed char *a, int n) { int i = 0; while (i < n && a[i] != -5) i++; return i; }
static unsigned ufind(const unsigned *a, unsigned n, unsigned x) { unsigned i = 0; while (i < n && a[i] != x) i++; return i; }
static int nfind(const int *a, int n, int x) { int i = 0; while (i + 1 < n && a[i + 1] != x) i++; return i; }
static int pair(const int *a, const int *b, int n) { int i; for (i = 0; i < n; i++) if (a[i] > b[i] + 3) break; return i; }
static int fwin(const float *a, int s, int n, float lo, float hi) { int i; for (i = s; i < n; i++) if (a[i] < lo || a[i] > hi) break; return i; }
static int sumcmp(const int *a, int n, int x) { int i = 0; while (n > i && (a[i] ^ x) != 7) i++; return i; }

static unsigned long h = 5381;
static void put(long v) { h = h * 33 + (unsigned long)v; }

int main(void)
{
    int n, s, k, i;
    float nanv;
    unsigned u = 0x7fc00000u;
    memcpy(&nanv, &u, 4);
    for (i = 0; i < M + 8; i++) {
        ia[i] = i * 7 % 101; fa[i] = (float)(i % 37) * 0.5f; da[i] = (double)(i % 53) * 0.25; la[i] = (long)i * 1000003L % 997;
        qa[i] = (unsigned)i * 2654435761u % 1009u;
    }
    for (i = 0; i < M + 32; i++) { ua[i] = (unsigned char)(i * 13 % 251 + 1); sa[i] = (signed char)(i * 11 % 200 - 100); if (sa[i] == -5) sa[i] = 6; }
    for (n = 0; n <= 70; n += n < 40 ? 1 : 7)
        for (s = 0; s < 8; s++)
            for (k = -1; k < n; k += k < 20 ? 1 : 5) {
                /* the stopping element at k (none if k < 0) */
                int save_i = 0, *pi = ia + s;
                float save_f = 0, *pf = fa + s;
                double save_d = 0, *pd = da + s;
                long save_l = 0, *pl = la + s;
                unsigned char save_u = 0, *pu = ua + s;
                signed char save_s = 0, *ps = sa + s;
                unsigned save_q = 0, *pq = qa + s;
                if (k >= 0) {
                    save_i = pi[k]; pi[k] = 5000; save_f = pf[k]; pf[k] = k % 3 ? 99.f : nanv; save_d = pd[k]; pd[k] = 1e9;
                    save_l = pl[k]; pl[k] = -77; save_u = pu[k]; pu[k] = k % 2 ? 0 : 13; save_s = ps[k]; ps[k] = -5; save_q = pq[k]; pq[k] = 4000000000u;
                }
                put(find(pi, n, 5000)); put(ffirst(pf, n, 50.f)); put(fnle(pf, n, 50.f)); put(dfind(pd, n, 1e8)); put((long)lfind(pl, (unsigned long)n, -77));
                put(bfind(pu, n)); put(bfind2(pu, n)); put(sfind(ps, n)); put(ufind(pq, (unsigned)n, 4000000000u)); put(nfind(pi, n, 5000));
                put(pair(pi, ia + 3, n)); put(fwin(pf, s & 3, n, -1.f, 60.f)); put(sumcmp(pi, n, 5000 ^ 7));
                if (k >= 0) { pi[k] = save_i; pf[k] = save_f; pd[k] = save_d; pl[k] = save_l; pu[k] = save_u; ps[k] = save_s; pq[k] = save_q; }
            }
    printf("%lx\n", h);
    return 0;
}
