/* The C library pieces added for ufsm and the libraries (libc/linux/src/ext.c, printf %a, scanf lengths,
   strtod's hexadecimal and special values). */
#define _GNU_SOURCE
#include "t.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
static int by_key(const void *a, const void *b, void *arg)
{
    const int *k = arg;
    return k[*(const int *)a] - k[*(const int *)b];
}
int main(void)
{
    char b[200]; void *p[40]; int i, ok, fd; size_t z = 0; long l = 0; long long ll = 0; unsigned char hh = 0; short h = 0; double d; float f;
    intmax_t j = 0; ptrdiff_t t = 0; struct stat st; FILE *fp; struct timespec ts; unsigned char vec[4]; void *m;
    /* aligned allocation: every alignment, freed and reallocated like any other block */
    ok = 1;
    for (i = 0; i < 40; i++) {
        size_t al = (size_t)16 << (i % 10), n = 1 + (size_t)i * 977;
        if (i & 1) { if (posix_memalign(&p[i], al, n)) ok = 0; } else p[i] = i & 2 ? memalign(al, n) : aligned_alloc(al, n);
        if (!p[i] || ((uintptr_t)p[i] & (al - 1)) || malloc_usable_size(p[i]) < n) ok = 0;
        else memset(p[i], i, n);
    }
    for (i = 0; i < 40; i++) {
        size_t n = 1 + (size_t)i * 977;
        if (((unsigned char *)p[i])[0] != i || ((unsigned char *)p[i])[n - 1] != i) ok = 0;
        if (i % 3 == 0) { p[i] = realloc(p[i], n * 3); if (!p[i] || ((unsigned char *)p[i])[n - 1] != i) ok = 0; }
        free(p[i]);
    }
    CHECK("libc-memalign", ok && posix_memalign(&p[0], 24, 10) == EINVAL);
    m = malloc(100);
    CHECK("libc-malloc-extras", malloc_usable_size(m) >= 100 && malloc_usable_size(NULL) == 0 && mallopt(M_MMAP_THRESHOLD, 1 << 20) == 1 && malloc_trim(0) == 0);
    free(m);
    snprintf(b, sizeof b, "%a|%A|%.2a|%a|%.0a|%a|%10.1a|%a", 1.0, 255.5, 3.14159, 0.0, 1.5, -0x1.fffffffffffffp+1023, 1.0, 0x1p-1074);
    CHECK("libc-printf-%a", !strcmp(b, "0x1p+0|0X1.FFP+7|0x1.92p+1|0x0p+0|0x2p+0|-0x1.fffffffffffffp+1023|  0x1.0p+0|0x0.0000000000001p-1022"));
    snprintf(b, sizeof b, "%zu %td %jd %lld %hhu %hhd %hd %'d %F %5.1Lf", (size_t)-1, (ptrdiff_t)-5, (intmax_t)INT64_MIN, 1LL << 62, 300, -129, 70000, 1234567, 1.5, (long double)2.25);
    CHECK("libc-printf-lengths", !strcmp(b, "18446744073709551615 -5 -9223372036854775808 4611686018427387904 44 127 4464 1234567 1.500000   2.2"));
    snprintf(b, sizeof b, "%" PRIu64 " %" PRIx32 " %" PRId8 " %" PRIuPTR " %" PRIiMAX " %" PRIdLEAST16 " %" PRIuFAST32, (uint64_t)1 << 63, (uint32_t)0xdeadbeef, (int8_t)-3, (uintptr_t)99, (intmax_t)-1, (int_least16_t)-7, (uint_fast32_t)8);
    CHECK("libc-inttypes", !strcmp(b, "9223372036854775808 deadbeef -3 99 -1 -7 8"));
    i = sscanf("77 -5000000000 9 200 -9 33 44 1.5 2.5e3", "%zu %lld %td %hhu %hd %jd %ld %f %lf", &z, &ll, &t, &hh, &h, &j, &l, &f, &d);
    CHECK("libc-scanf-lengths", i == 9 && z == 77 && ll == -5000000000LL && t == 9 && hh == 200 && h == -9 && j == 33 && l == 44 && f == 1.5f && d == 2500.0);
    CHECK("libc-strtod-hex", strtod("0x1.8p1", NULL) == 3.0 && strtod("-0X.8P-1", NULL) == -0.25 && strtod("0x1p-1074", NULL) == 0x1p-1074 &&
          strtod("0x1.fffffffffffff8p0", NULL) == 2.0 && strtod("0x1.00000000000008p0", NULL) == 1.0 && strtod("0x1.00000000000018p0", NULL) == 0x1.0000000000002p0 &&
          strtod("0x1p1024", NULL) == INFINITY && strtod("0x123456789abcdef01p-4", NULL) == 0x123456789abcdfp8);
    { char *e; d = strtod("infinityx", &e); CHECK("libc-strtod-special", isinf(d) && *e == 'x' && isnan(strtod("NaN(12)", &e)) && !*e && strtod("-INF", NULL) == -INFINITY && strtold("2.5", NULL) == 2.5L && strtod("0x", &e) == 0 && *e == 'x'); }
    { int key[8] = { 5, 3, 9, 1, 7, 2, 8, 0 }, idx[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }, want[8] = { 7, 3, 5, 1, 0, 4, 6, 2 };
      static int big[5000], kk[5000];
      qsort_r(idx, 8, sizeof idx[0], by_key, key);
      for (i = 0; i < 5000; i++) { big[i] = i; kk[i] = (int)((unsigned)i * 2654435761u >> 12); }
      qsort_r(big, 5000, sizeof big[0], by_key, kk);
      for (ok = 1, i = 1; i < 5000; i++) if (kk[big[i - 1]] > kk[big[i]]) ok = 0;
      CHECK("libc-qsort_r", !memcmp(idx, want, sizeof want) && ok); }
    /* fmemopen as ufsm uses it (a scratch stream), and over a buffer */
    fp = fmemopen(NULL, 64, "w+");
    ok = fp && fputs("line one\nline two\n", fp) >= 0; if (fp) rewind(fp);
    CHECK("libc-fmemopen-scratch", ok && fgets(b, sizeof b, fp) && !strcmp(b, "line one\n") && fgets(b, sizeof b, fp) && !strcmp(b, "line two\n") && !fgets(b, sizeof b, fp) && !fclose(fp));
    fp = fmemopen("abc def", 7, "r");
    CHECK("libc-fmemopen-read", fp && fread(b, 1, 100, fp) == 7 && !memcmp(b, "abc def", 7) && feof(fp) && !fclose(fp));
    CHECK("libc-dlopen-static", dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL) == NULL && dlerror() != NULL && dlerror() == NULL && dlsym(RTLD_DEFAULT, "x") == NULL);
    CHECK("libc-strerror_r", strerror_r(ENOENT, b, sizeof b) == 0 && !strcmp(b, strerror(ENOENT)) && strerror_r(ENOENT, b, 3) == ERANGE);
    fd = open("out/t.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    ok = fd >= 0 && pwrite(fd, "0123456789", 10, 0) == 10;
    { struct iovec v[2]; char x[4], y[4]; v[0].iov_base = x; v[0].iov_len = 3; v[1].iov_base = y; v[1].iov_len = 3;
      CHECK("libc-preadv", ok && preadv(fd, v, 2, 2) == 6 && !memcmp(x, "234", 3) && !memcmp(y, "567", 3)); }
    CHECK("libc-posix_fadvise", posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0 && posix_fadvise(-1, 0, 0, POSIX_FADV_SEQUENTIAL) == EBADF);
    i = posix_fallocate(fd, 0, 5000);      /* the shared directory (9p) may not allocate: then the error must be an honest one */
    CHECK("libc-fallocate", (i == 0 && !fstat(fd, &st) && st.st_size == 5000) || i == EOPNOTSUPP || i == ENOSYS);
    close(fd);
    fd = memfd_create("t", 0);
    CHECK("libc-memfd", fd >= 0 && write(fd, "xyz", 3) == 3 && fallocate(fd, 0, 0, 10000) == 0 && !fstat(fd, &st) && st.st_size == 10000);
    close(fd);
    m = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ok = m != MAP_FAILED; if (ok) ((char *)m)[4096] = 1;
    CHECK("libc-mincore-madvise", ok && mincore(m, 4 * 4096, vec) == 0 && (vec[1] & 1) && !(vec[3] & 1) && (madvise(m, 4 * 4096, MADV_HUGEPAGE) == 0 || errno == EINVAL) && madvise(m, 4096, MADV_DONTNEED) == 0 &&
          ((i = posix_madvise(m, 4096, POSIX_MADV_WILLNEED)) == 0 || i == EBADF) && posix_madvise(m, 4096, 99) == EINVAL && munmap(m, 4 * 4096) == 0);
    CHECK("libc-clocks", clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 && clock_getres(CLOCK_MONOTONIC, &ts) == 0 && ts.tv_sec == 0 && ts.tv_nsec > 0 && gettid() == getpid());
    srand48(0); d = drand48();
    { unsigned s = 1; unsigned char e[16] = { 0 }; lldiv_t q = lldiv(-7, 2);
      CHECK("libc-random", fabs(d - 0.17082803610628972) < 1e-15 && lrand48() == 1610402240 && rand_r(&s) >= 0 && getentropy(e, 16) == 0 && (e[0] | e[1] | e[2] | e[3] | e[4] | e[5] | e[6] | e[7]));
      CHECK("libc-div", q.quot == -3 && q.rem == -1 && imaxabs(-5) == 5 && ffsl(0x100000000L) == 33 && ffsll(0) == 0); }
    { wchar_t w[8]; char u[16];
      CHECK("libc-wchar", mbstowcs(w, "a\xc3\xa9\xe2\x82\xac", 8) == 3 && w[1] == 0xe9 && w[2] == 0x20ac && wcslen(w) == 3 && wcstombs(u, w, 16) == 6 && !strcmp(u, "a\xc3\xa9\xe2\x82\xac") && mbtowc(w, "\xff", 1) == -1); }
    CHECK("libc-stdint", INT_FAST16_MAX == INT64_MAX && UINT_LEAST8_MAX == 255 && SIZE_MAX == UINT64_MAX && INTMAX_C(5) == 5 && sizeof(intmax_t) == 8);
    return t_fail != 0;
}
