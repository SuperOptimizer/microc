/* ext.c: the parts of the Linux C library that larger programs (ufsm, libcurl, zstd, ...) need beyond what
   busybox and the kernel build asked for: aligned allocation, the malloc.h extras, file and memory advice,
   fmemopen, dlopen (which always fails: programs are static), qsort_r, and other small pieces. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

void *__memalign(size_t align, size_t n);
size_t __malloc_size(void *p);

/* ---- allocation ---- */

void *memalign(size_t align, size_t n)
{
    if (align & (align - 1)) {
        errno = EINVAL;
        return 0;
    }
    return __memalign(align, n);
}

int posix_memalign(void **out, size_t align, size_t n)
{
    void *p;
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    p = __memalign(align, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }
void *valloc(size_t n) { return __memalign(4096, n); }
void *pvalloc(size_t n) { return __memalign(4096, (n + 4095) & ~(size_t)4095); }
size_t malloc_usable_size(void *p) { return __malloc_size(p); }
int malloc_trim(size_t pad) { (void)pad; return 0; }
int mallopt(int param, int value) { (void)param; (void)value; return 1; } /* nothing to tune: taken and ignored */
struct mallinfo2 mallinfo2(void)
{
    struct mallinfo2 m;
    memset(&m, 0, sizeof m);
    return m;
}

void *reallocarray(void *p, size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return 0;
    }
    return realloc(p, count * size);
}

/* ---- memory and files ---- */

/* syscall() sets errno and returns -1 on failure. The posix_ functions return the error instead and leave
   errno as it was. */

int mincore(void *addr, size_t n, unsigned char *vec) { return (int)syscall(SYS_mincore, addr, n, vec); }
int munlock(const void *addr, size_t n) { return (int)syscall(SYS_munlock, addr, n); }
int memfd_create(const char *name, unsigned flags) { return (int)syscall(SYS_memfd_create, name, flags); }

int posix_madvise(void *addr, size_t n, int advice)
{
    int saved = errno, e;
    if (advice == POSIX_MADV_DONTNEED) /* the kernel's MADV_DONTNEED throws the pages away: not what POSIX means */
        return 0;
    e = syscall(SYS_madvise, addr, n, advice) < 0 ? errno : 0;
    errno = saved;
    return e;
}

int posix_fadvise(int fd, off_t off, off_t len, int advice)
{
    int saved = errno, e = syscall(SYS_fadvise64, fd, off, len, advice) < 0 ? errno : 0;
    errno = saved;
    return e;
}

int fallocate(int fd, int mode, off_t off, off_t len) { return (int)syscall(SYS_fallocate, fd, mode, off, len); }

int posix_fallocate(int fd, off_t off, off_t len)
{
    int saved = errno, e = syscall(SYS_fallocate, fd, 0, off, len) < 0 ? errno : 0;
    errno = saved;
    return e;
}

ssize_t preadv(int fd, const struct iovec *v, int n, off_t off) { return syscall(SYS_preadv, fd, v, n, off, 0L); }
ssize_t pwritev(int fd, const struct iovec *v, int n, off_t off) { return syscall(SYS_pwritev, fd, v, n, off, 0L); }
int clock_getres(clockid_t id, struct timespec *ts) { return (int)syscall(SYS_clock_getres, id, ts); }
pid_t gettid(void) { return (pid_t)syscall(SYS_gettid); }

int getentropy(void *buf, size_t n)
{
    size_t done = 0;
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    while (done < n) {
        ssize_t r = getrandom((char *)buf + done, n - done, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

/* open_memstream: a stream to write to, whose contents are handed over as an allocated string each time it is
   flushed and when it is closed. It writes to an unnamed file in memory, like fmemopen below; the stream
   library calls memstream_sync after a flush. */
extern void (*__stdio_sync_hook)(FILE *f, int closing);
static struct memstream {
    FILE *f;
    char **ptr;
    size_t *size;
} memstreams[16];

static void memstream_sync(FILE *f, int closing)
{
    int i;
    for (i = 0; i < 16; i++)
        if (memstreams[i].f == f) {
            int fd = fileno(f);
            off_t n = lseek(fd, 0, SEEK_END);
            char *p;
            size_t done = 0;
            if (n < 0)
                n = 0;
            p = realloc(*memstreams[i].ptr, (size_t)n + 1);
            if (!p)
                return;
            while (done < (size_t)n) {
                ssize_t r = pread(fd, p + done, (size_t)n - done, (off_t)done);
                if (r <= 0)
                    break;
                done += (size_t)r;
            }
            p[done] = 0;
            *memstreams[i].ptr = p;
            *memstreams[i].size = done;
            if (closing)
                memstreams[i].f = 0;
            return;
        }
}

FILE *open_memstream(char **ptr, size_t *sizeloc)
{
    int i;
    FILE *f;
    for (i = 0; i < 16 && memstreams[i].f; i++)
        ;
    if (i == 16) {
        errno = EMFILE;
        return 0;
    }
    f = fmemopen(0, 0, "w+");
    if (!f)
        return 0;
    *ptr = 0;
    *sizeloc = 0;
    memstreams[i].ptr = ptr;
    memstreams[i].size = sizeloc;
    memstreams[i].f = f;
    __stdio_sync_hook = memstream_sync;
    return f;
}

/* A stream over memory. It is a stream over an unnamed file in memory (memfd, or an unlinked file in /tmp
   where the kernel has none): what is in buf can be read, and with a null buf the stream is a scratch file,
   which is how programs use it; writes are not copied back into a buf the caller supplied. */
FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    int fd = memfd_create("fmemopen", 0);
    FILE *f;
    if (fd < 0) {
        char path[] = "/tmp/fmemXXXXXX";
        fd = mkstemp(path);
        if (fd < 0)
            return 0;
        unlink(path);
    }
    if (buf && mode[0] != 'w') {
        size_t n = mode[0] == 'a' ? strnlen(buf, size) : size, done = 0;
        while (done < n) {
            ssize_t r = write(fd, (char *)buf + done, n - done);
            if (r <= 0) {
                close(fd);
                return 0;
            }
            done += (size_t)r;
        }
        if (mode[0] == 'r')
            lseek(fd, 0, SEEK_SET);
    }
    f = fdopen(fd, mode[0] == 'r' && !strchr(mode, '+') ? "r" : mode[0] == 'a' ? "a+" : "r+");
    if (!f)
        close(fd);
    return f;
}

/* ---- dlopen: there is no dynamic loader in a static program ---- */

static const char *dl_msg;

void *dlopen(const char *path, int flags)
{
    (void)path;
    (void)flags;
    dl_msg = "dlopen: this program is statically linked; shared objects cannot be loaded";
    return 0;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    dl_msg = "dlsym: no shared objects are loaded";
    return 0;
}

int dlclose(void *handle)
{
    (void)handle;
    dl_msg = "dlclose: not a handle";
    return -1;
}

char *dlerror(void)
{
    const char *m = dl_msg;
    dl_msg = 0;
    return (char *)m;
}

int dladdr(const void *addr, Dl_info *info)
{
    (void)addr;
    (void)info;
    return 0;
}

/* ---- strings and numbers ---- */

int strerror_r(int err, char *buf, size_t n) /* the POSIX one: 0 or an error number */
{
    const char *s = strerror(err);
    size_t len = strlen(s);
    if (len >= n) {
        if (n) {
            memcpy(buf, s, n - 1);
            buf[n - 1] = 0;
        }
        return ERANGE;
    }
    memcpy(buf, s, len + 1);
    return 0;
}

long double strtold(const char *s, char **end) { return strtod(s, end); }
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }
imaxdiv_t imaxdiv(intmax_t a, intmax_t b) { imaxdiv_t r; r.quot = a / b; r.rem = a % b; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r; r.quot = a / b; r.rem = a % b; return r; }

int ffsl(long v)
{
    int n = 1;
    if (!v)
        return 0;
    for (; !(v & 1); v = (long)((unsigned long)v >> 1))
        n++;
    return n;
}
int ffsll(long long v) { return ffsl((long)v); }

/* heap sort: in place, no recursion, n log n whatever the input */
static void sift(char *a, size_t size, size_t i, size_t n, int (*cmp)(const void *, const void *, void *), void *arg)
{
    for (;;) {
        size_t c = 2 * i + 1, k;
        if (c >= n)
            return;
        if (c + 1 < n && cmp(a + c * size, a + (c + 1) * size, arg) < 0)
            c++;
        if (cmp(a + i * size, a + c * size, arg) >= 0)
            return;
        for (k = 0; k < size; k++) {
            char t = a[i * size + k];
            a[i * size + k] = a[c * size + k];
            a[c * size + k] = t;
        }
        i = c;
    }
}

void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg)
{
    char *a = base;
    size_t i, k;
    if (n < 2 || !size)
        return;
    for (i = n / 2; i-- > 0;)
        sift(a, size, i, n, cmp, arg);
    for (i = n - 1; i > 0; i--) {
        for (k = 0; k < size; k++) {
            char t = a[k];
            a[k] = a[i * size + k];
            a[i * size + k] = t;
        }
        sift(a, size, 0, i, cmp, arg);
    }
}

/* ---- random numbers: the 48-bit generator of POSIX, and rand_r ---- */

static unsigned short seed48_state[3] = { 0x330e, 0xabcd, 0x1234 };

static unsigned long step48(unsigned short *s)
{
    unsigned long x = (unsigned long)s[0] | (unsigned long)s[1] << 16 | (unsigned long)s[2] << 32;
    x = (x * 0x5deece66dUL + 0xb) & 0xffffffffffffUL;
    s[0] = (unsigned short)x;
    s[1] = (unsigned short)(x >> 16);
    s[2] = (unsigned short)(x >> 32);
    return x;
}

void srand48(long seed)
{
    seed48_state[0] = 0x330e;
    seed48_state[1] = (unsigned short)seed;
    seed48_state[2] = (unsigned short)(seed >> 16);
}
double erand48(unsigned short s[3]) { return (double)step48(s) / 281474976710656.0; }
double drand48(void) { return erand48(seed48_state); }
long nrand48(unsigned short s[3]) { return (long)(step48(s) >> 17); }
long lrand48(void) { return nrand48(seed48_state); }
long jrand48(unsigned short s[3]) { return (long)(int)(step48(s) >> 16); }
long mrand48(void) { return jrand48(seed48_state); }

int rand_r(unsigned *seed)
{
    unsigned x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (!x)
        x = 0x9e3779b9U;
    *seed = x;
    return (int)(x & 0x7fffffff);
}

/* ---- wide characters: wchar_t holds a code point, the multibyte encoding is UTF-8 ---- */

size_t wcslen(const wchar_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int wctomb(char *s, wchar_t wc)
{
    unsigned c = (unsigned)wc;
    if (!s)
        return 0;
    if (c < 0x80) {
        s[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        s[0] = (char)(0xc0 | c >> 6);
        s[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        if (c >= 0xd800 && c < 0xe000)
            goto bad;
        s[0] = (char)(0xe0 | c >> 12);
        s[1] = (char)(0x80 | (c >> 6 & 63));
        s[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    if (c < 0x110000) {
        s[0] = (char)(0xf0 | c >> 18);
        s[1] = (char)(0x80 | (c >> 12 & 63));
        s[2] = (char)(0x80 | (c >> 6 & 63));
        s[3] = (char)(0x80 | (c & 63));
        return 4;
    }
bad:
    errno = EILSEQ;
    return -1;
}

int mbtowc(wchar_t *out, const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned c, min;
    int len, i;
    if (!s)
        return 0;
    if (!n)
        goto bad;
    c = p[0];
    if (c < 0x80) {
        if (out)
            *out = (wchar_t)c;
        return c != 0;
    }
    if (c >= 0xc2 && c < 0xe0) {
        len = 2; c &= 0x1f; min = 0x80;
    } else if (c >= 0xe0 && c < 0xf0) {
        len = 3; c &= 0x0f; min = 0x800;
    } else if (c >= 0xf0 && c < 0xf5) {
        len = 4; c &= 0x07; min = 0x10000;
    } else {
        goto bad;
    }
    if ((size_t)len > n)
        goto bad;
    for (i = 1; i < len; i++) {
        if ((p[i] & 0xc0) != 0x80)
            goto bad;
        c = c << 6 | (p[i] & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000))
        goto bad;
    if (out)
        *out = (wchar_t)c;
    return len;
bad:
    errno = EILSEQ;
    return -1;
}

int mblen(const char *s, size_t n) { return mbtowc(0, s, n); }

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
    size_t k = 0;
    while (!dst || k < n) {
        wchar_t wc;
        int len = mbtowc(&wc, src, 4);
        if (len < 0)
            return (size_t)-1;
        if (dst)
            dst[k] = len ? wc : 0;
        if (!len)
            break;
        src += len;
        k++;
    }
    return k;
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
    size_t k = 0;
    for (;; src++) {
        char tmp[4];
        int len;
        if (!*src) {
            if (dst && k < n)
                dst[k] = 0;
            return k;
        }
        len = wctomb(tmp, *src);
        if (len < 0)
            return (size_t)-1;
        if (dst) {
            if (k + (size_t)len > n)
                return k;
            memcpy(dst + k, tmp, (size_t)len);
        }
        k += (size_t)len;
    }
}
