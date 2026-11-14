#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lock.h"

int __rename(const char *from, const char *to);
int __getpid(void);
typedef void (*OutFn)(void *ctx, const char *s, size_t n);
int __vformat(OutFn fn, void *ctx, const char *fmt, va_list ap);

#define F_READ 1
#define F_WRITE 2
#define F_EOF 4
#define F_ERR 8
#define F_LINE 16  /* flush at each newline */
#define F_UNBUF 32
#define F_OWNBUF 64 /* buffer was allocated by us */
#define F_READING 128 /* buffer holds input */
#define F_WRITING 256 /* buffer holds pending output */
#define F_APPEND 512
#define F_OPEN 1024

struct __file {
    int fd;
    int flags;
    char *buf;
    size_t cap;
    size_t pos; /* next byte to read, or number of bytes pending output */
    size_t len; /* bytes of input in the buffer */
    int unget;  /* pushed-back character or -1 */
    char small[1];
};

static struct __file files[FOPEN_MAX] = {
    { 0, F_READ | F_OPEN, 0, 0, 0, 0, -1, { 0 } },
    { 1, F_WRITE | F_OPEN, 0, 0, 0, 0, -1, { 0 } },
    { 2, F_WRITE | F_UNBUF | F_OPEN, 0, 0, 0, 0, -1, { 0 } }
};
static int std_ready;

/* With threads, every stream has a lock that the functions below hold from
   start to finish, so that the output of one call stays in one piece; a
   thread may take the lock of a stream again while it holds it. */
#ifdef __MCC_THREADS
static struct __rlock stream_locks[FOPEN_MAX];
LOCK_DECL(table_lock);
#define FLOCK(f) RLOCK(stream_locks[(f) - files])
#define FUNLOCK(f) RUNLOCK(stream_locks[(f) - files])
#else
#define FLOCK(f) ((void)0)
#define FUNLOCK(f) ((void)0)
#endif

/* A free entry of the table, marked as taken. */
static FILE *new_stream(void)
{
    int i;
    FILE *f = 0;
    LOCK(table_lock);
    for (i = 3; i < FOPEN_MAX && !f; i++)
        if (!(files[i].flags & F_OPEN)) {
            f = &files[i];
            f->flags = F_OPEN;
        }
    UNLOCK(table_lock);
    if (!f)
        errno = ENOMEM;
    return f;
}

FILE *__stdin = &files[0];
FILE *__stdout = &files[1];
FILE *__stderr = &files[2];

static void prepare(FILE *f)
{
    if (f->buf)
        return;
    if (f == __stdout && !std_ready) {
        std_ready = 1;
        if (isatty(1))
            f->flags |= F_LINE;
    }
    if (!(f->flags & F_UNBUF)) {
        f->buf = malloc(BUFSIZ);
        if (f->buf) {
            f->cap = BUFSIZ;
            f->flags |= F_OWNBUF;
            return;
        }
    }
    f->buf = f->small;
    f->cap = 1;
}

static int flush_out(FILE *f)
{
    size_t done = 0;
    if (!(f->flags & F_WRITING))
        return 0;
    while (done < f->pos) {
        long n = write(f->fd, f->buf + done, f->pos - done);
        if (n <= 0) {
            f->flags |= F_ERR;
            f->pos = 0;
            return EOF;
        }
        done += (size_t)n;
    }
    f->pos = 0;
    f->flags &= ~F_WRITING;
    return 0;
}

/* Drops buffered input, moving the file position back to the logical one. */
static void drop_in(FILE *f)
{
    if (f->flags & F_READING) {
        long back = (long)(f->len - f->pos) + (f->unget >= 0);
        if (back)
            lseek(f->fd, -back, SEEK_CUR);
        f->pos = f->len = 0;
        f->flags &= ~F_READING;
    }
    f->unget = -1;
}

/* set by open_memstream: called when a stream has been flushed (closing: and is about to be closed) */
void (*__stdio_sync_hook)(FILE *f, int closing);

int fflush(FILE *f)
{
    int i, r = 0;
    if (f) {
        FLOCK(f);
        r = flush_out(f);
        if (__stdio_sync_hook)
            __stdio_sync_hook(f, 0);
        FUNLOCK(f);
        return r;
    }
    for (i = 0; i < FOPEN_MAX; i++)
        if (files[i].flags & F_OPEN) {
            FLOCK(&files[i]);
            r |= flush_out(&files[i]);
            FUNLOCK(&files[i]);
        }
    return r;
}

void __stdio_exit(void)
{
    fflush(0);
}

static int parse_mode(const char *mode, int *oflags)
{
    int flags, plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
    case 'r':
        flags = F_READ | (plus ? F_WRITE : 0);
        *oflags = plus ? O_RDWR : O_RDONLY;
        break;
    case 'w':
        flags = F_WRITE | (plus ? F_READ : 0);
        *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        break;
    case 'a':
        flags = F_WRITE | F_APPEND | (plus ? F_READ : 0);
        *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        break;
    default:
        return 0;
    }
    return flags;
}

static FILE *open_into(FILE *f, const char *path, const char *mode)
{
    int oflags, flags = parse_mode(mode, &oflags), fd;
    if (!flags) {
        errno = EINVAL;
        return 0;
    }
    fd = open(path, oflags, 0666);
    if (fd < 0)
        return 0;
    f->fd = fd;
    f->flags = flags | F_OPEN;
    f->buf = 0;
    f->cap = f->pos = f->len = 0;
    f->unget = -1;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *f = new_stream();
    if (f && !open_into(f, path, mode)) {
        f->flags = 0;
        f = 0;
    }
    return f;
}

static int fclose_nolock(FILE *f)
{
    int r = flush_out(f);
    if (close(f->fd) < 0)
        r = EOF;
    if (f->flags & F_OWNBUF)
        free(f->buf);
    f->flags = 0;
    f->buf = 0;
    return r;
}

int fclose(FILE *f)
{
    int r;
    FLOCK(f);
    if (__stdio_sync_hook) {
        flush_out(f);
        __stdio_sync_hook(f, 1);
    }
    r = fclose_nolock(f);
    FUNLOCK(f);
    return r;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    fclose(f);
    return open_into(f, path, mode);
}

static int fill(FILE *f)
{
    long n;
    if (!(f->flags & F_READ) || (f->flags & F_EOF))
        return EOF;
    if (f->flags & F_WRITING)
        flush_out(f);
    prepare(f);
    if (f == __stdin)
        flush_out(__stdout);
    n = read(f->fd, f->buf, f->cap);
    if (n <= 0) {
        f->flags |= n == 0 ? F_EOF : F_ERR;
        f->pos = f->len = 0;
        return EOF;
    }
    f->flags |= F_READING;
    f->pos = 0;
    f->len = (size_t)n;
    return 0;
}

static int fgetc_nolock(FILE *f)
{
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        return c;
    }
    if (f->pos >= f->len || !(f->flags & F_READING))
        if (fill(f))
            return EOF;
    return (unsigned char)f->buf[f->pos++];
}

int fgetc(FILE *f)
{
    int r;
    FLOCK(f);
    r = fgetc_nolock(f);
    FUNLOCK(f);
    return r;
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(__stdin); }

static int ungetc_nolock(int c, FILE *f)
{
    if (c == EOF || f->unget >= 0)
        return EOF;
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

int ungetc(int c, FILE *f)
{
    int r;
    FLOCK(f);
    r = ungetc_nolock(c, f);
    FUNLOCK(f);
    return r;
}

static size_t fread_nolock(void *buf, size_t size, size_t count, FILE *f)
{
    size_t total = size * count, done = 0;
    char *p = buf;
    if (total == 0)
        return 0;
    while (done < total) {
        size_t avail;
        if (f->unget >= 0) {
            p[done++] = (char)f->unget;
            f->unget = -1;
            continue;
        }
        if (!(f->flags & F_READING) || f->pos >= f->len) {
            /* large reads bypass the buffer */
            if (total - done >= BUFSIZ && (f->flags & F_READ) && !(f->flags & F_EOF)) {
                long n;
                if (f->flags & F_WRITING)
                    flush_out(f);
                n = read(f->fd, p + done, total - done);
                if (n <= 0) {
                    f->flags |= n == 0 ? F_EOF : F_ERR;
                    break;
                }
                done += (size_t)n;
                continue;
            }
            if (fill(f))
                break;
        }
        avail = f->len - f->pos;
        if (avail > total - done)
            avail = total - done;
        memcpy(p + done, f->buf + f->pos, avail);
        f->pos += avail;
        done += avail;
    }
    return done / size;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f)
{
    size_t r;
    FLOCK(f);
    r = fread_nolock(buf, size, count, f);
    FUNLOCK(f);
    return r;
}

static void put_bytes(FILE *f, const char *s, size_t n)
{
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERR;
        return;
    }
    if (f->flags & F_READING)
        drop_in(f);
    prepare(f);
    f->flags |= F_WRITING;
    if (f->flags & F_UNBUF) {
        f->pos = 0;
        while (n) {
            long w = write(f->fd, s, n);
            if (w <= 0) {
                f->flags |= F_ERR;
                break;
            }
            s += w;
            n -= (size_t)w;
        }
        f->flags &= ~F_WRITING;
        return;
    }
    while (n) {
        size_t room = f->cap - f->pos;
        if (room == 0) {
            if (flush_out(f))
                return;
            f->flags |= F_WRITING;
            continue;
        }
        if (room > n)
            room = n;
        memcpy(f->buf + f->pos, s, room);
        f->pos += room;
        s += room;
        n -= room;
    }
}

static size_t fwrite_nolock(const void *buf, size_t size, size_t count, FILE *f)
{
    size_t total = size * count;
    if (total == 0)
        return 0;
    put_bytes(f, buf, total);
    if ((f->flags & F_LINE) && memchr(buf, '\n', total))
        flush_out(f);
    return (f->flags & F_ERR) ? 0 : count;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f)
{
    size_t r;
    FLOCK(f);
    r = fwrite_nolock(buf, size, count, f);
    FUNLOCK(f);
    return r;
}

static int fputc_nolock(int c, FILE *f)
{
    char ch = (char)c;
    if ((f->flags & (F_WRITE | F_WRITING | F_UNBUF | F_LINE)) == (F_WRITE | F_WRITING) && f->pos < f->cap) {
        f->buf[f->pos++] = ch;
        return (unsigned char)c;
    }
    put_bytes(f, &ch, 1);
    if (c == '\n' && (f->flags & F_LINE))
        flush_out(f);
    return (f->flags & F_ERR) ? EOF : (unsigned char)c;
}

int fputc(int c, FILE *f)
{
    int r;
    FLOCK(f);
    r = fputc_nolock(c, f);
    FUNLOCK(f);
    return r;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, __stdout); }

static int fputs_nolock(const char *s, FILE *f)
{
    size_t n = strlen(s);
    if (n && fwrite(s, 1, n, f) != n)
        return EOF;
    return 0;
}

int fputs(const char *s, FILE *f)
{
    int r;
    FLOCK(f);
    r = fputs_nolock(s, f);
    FUNLOCK(f);
    return r;
}

int puts(const char *s)
{
    int r;
    FLOCK(__stdout);
    r = fputs(s, __stdout) == EOF || fputc('\n', __stdout) == EOF ? EOF : 0;
    FUNLOCK(__stdout);
    return r;
}

static char *fgets_nolock(char *s, int n, FILE *f)
{
    int i = 0, c = 0;
    if (n <= 0)
        return 0;
    while (i < n - 1) {
        c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return 0;
    s[i] = 0;
    return s;
}

char *fgets(char *s, int n, FILE *f)
{
    char *r;
    FLOCK(f);
    r = fgets_nolock(s, n, f);
    FUNLOCK(f);
    return r;
}

char *gets(char *s)
{
    int i = 0, c;
    while ((c = getchar()) != EOF && c != '\n')
        s[i++] = (char)c;
    if (c == EOF && i == 0)
        return 0;
    s[i] = 0;
    return s;
}

/* ---- positioning ---- */

static int fseek_nolock(FILE *f, long off, int whence)
{
    if (flush_out(f))
        return -1;
    drop_in(f);
    f->flags &= ~F_EOF;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

int fseek(FILE *f, long off, int whence)
{
    int r;
    FLOCK(f);
    r = fseek_nolock(f, off, whence);
    FUNLOCK(f);
    return r;
}

static long ftell_nolock(FILE *f)
{
    long at = lseek(f->fd, 0, SEEK_CUR);
    if (at < 0)
        return -1;
    if (f->flags & F_WRITING)
        return at + (long)f->pos;
    if (f->flags & F_READING)
        at -= (long)(f->len - f->pos);
    if (f->unget >= 0)
        at--;
    return at;
}

long ftell(FILE *f)
{
    long r;
    FLOCK(f);
    r = ftell_nolock(f);
    FUNLOCK(f);
    return r;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int fgetpos(FILE *f, fpos_t *pos)
{
    *pos = ftell(f);
    return *pos < 0 ? -1 : 0;
}

int fsetpos(FILE *f, const fpos_t *pos)
{
    return fseek(f, *pos, SEEK_SET);
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }

static int setvbuf_nolock(FILE *f, char *buf, int mode, size_t size)
{
    if (f->buf)
        return -1;
    f->flags &= ~(F_LINE | F_UNBUF);
    if (mode == _IONBF) {
        f->flags |= F_UNBUF;
    } else {
        if (mode == _IOLBF)
            f->flags |= F_LINE;
        if (buf && size) {
            f->buf = buf;
            f->cap = size;
        }
    }
    if (f == __stdout)
        std_ready = 1;
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    int r;
    FLOCK(f);
    r = setvbuf_nolock(f, buf, mode, size);
    FUNLOCK(f);
    return r;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

int remove(const char *path) { return unlink(path); }
int rename(const char *from, const char *to) { return __rename(from, to); }

char *tmpnam(char *s)
{
    static char buf[L_tmpnam];
    static int counter;
    if (!s)
        s = buf;
    sprintf(s, "/tmp/mcc%d_%d", __getpid(), counter++);
    return s;
}

FILE *tmpfile(void)
{
    char name[L_tmpnam];
    FILE *f;
    tmpnam(name);
    f = fopen(name, "w+");
    if (f)
        unlink(name);
    return f;
}

void perror(const char *s)
{
    int e = errno;
    FLOCK(__stderr);
    if (s && *s) {
        fputs(s, __stderr);
        fputs(": ", __stderr);
    }
    fputs(strerror(e), __stderr);
    fputc('\n', __stderr);
    FUNLOCK(__stderr);
}

/* ---- formatted output ---- */

static void file_out(void *ctx, const char *s, size_t n)
{
    put_bytes(ctx, s, n);
}

static int vfprintf_nolock(FILE *f, const char *fmt, va_list ap)
{
    int n = __vformat(file_out, f, fmt, ap);
    if ((f->flags & F_LINE) && f->pos && memchr(f->buf, '\n', f->pos))
        flush_out(f);
    return (f->flags & F_ERR) ? -1 : n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    int r;
    FLOCK(f);
    r = vfprintf_nolock(f, fmt, ap);
    FUNLOCK(f);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(__stdout, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(__stdout, fmt, ap);
    va_end(ap);
    return n;
}

static void str_out(void *ctx, const char *s, size_t n)
{
    char **p = ctx;
    memcpy(*p, s, n);
    *p += n;
}

int vsprintf(char *s, const char *fmt, va_list ap)
{
    char *p = s;
    int n = __vformat(str_out, &p, fmt, ap);
    *p = 0;
    return n;
}

int sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsprintf(s, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- formatted input ---- */

typedef struct In {
    FILE *f;
    const char *s;
    int count; /* characters consumed */
} In;

static int in_get(In *in)
{
    int c;
    if (in->f) {
        c = fgetc(in->f);
    } else {
        c = *in->s ? (unsigned char)*in->s++ : EOF;
    }
    if (c != EOF)
        in->count++;
    return c;
}

static void in_unget(In *in, int c)
{
    if (c == EOF)
        return;
    in->count--;
    if (in->f)
        ungetc(c, in->f);
    else
        in->s--;
}

static int is_space(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static int vscan(In *in, const char *fmt, va_list ap)
{
    int assigned = 0, c;
    for (; *fmt; fmt++) {
        int suppress = 0, width = 0, length = 0, conv;
        if (is_space((unsigned char)*fmt)) {
            while (is_space(c = in_get(in)))
                ;
            in_unget(in, c);
            continue;
        }
        if (*fmt != '%') {
            c = in_get(in);
            if (c != (unsigned char)*fmt) {
                in_unget(in, c);
                return assigned ? assigned : (c == EOF ? EOF : 0);
            }
            continue;
        }
        fmt++;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        for (;; fmt++) { /* hh is 'H'; l, ll, z, j, t and q all mean 64 bits */
            if (*fmt == 'h')
                length = length == 'h' ? 'H' : 'h';
            else if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q')
                length = 'l';
            else if (*fmt == 'L')
                length = 'L';
            else
                break;
        }
        conv = *fmt;
        if (!conv)
            break;
        if (conv == '%') {
            while (is_space(c = in_get(in)))
                ;
            if (c != '%') {
                in_unget(in, c);
                return assigned;
            }
            continue;
        }
        if (conv == 'n') {
            if (suppress)
                ;
            else if (length == 'l')
                *va_arg(ap, long *) = in->count;
            else if (length == 'h')
                *va_arg(ap, short *) = (short)in->count;
            else if (length == 'H')
                *va_arg(ap, signed char *) = (signed char)in->count;
            else
                *va_arg(ap, int *) = in->count;
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while (is_space(c = in_get(in)))
                ;
            in_unget(in, c);
        }
        switch (conv) {
        case 'c': {
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int n = width ? width : 1, i;
            for (i = 0; i < n; i++) {
                c = in_get(in);
                if (c == EOF)
                    return assigned ? assigned : EOF;
                if (dst)
                    dst[i] = (char)c;
            }
            if (dst)
                assigned++;
            break;
        }
        case 's': {
            char *dst = suppress ? 0 : va_arg(ap, char *);
            int i = 0;
            for (;;) {
                c = in_get(in);
                if (c == EOF || is_space(c) || (width && i >= width)) {
                    in_unget(in, c);
                    break;
                }
                if (dst)
                    dst[i] = (char)c;
                i++;
            }
            if (i == 0)
                return assigned ? assigned : EOF;
            if (dst) {
                dst[i] = 0;
                assigned++;
            }
            break;
        }
        case '[': {
            char *dst = suppress ? 0 : va_arg(ap, char *);
            char set[256];
            int negate = 0, i = 0;
            memset(set, 0, sizeof set);
            fmt++;
            if (*fmt == '^') {
                negate = 1;
                fmt++;
            }
            if (*fmt == ']') {
                set[']'] = 1;
                fmt++;
            }
            for (; *fmt && *fmt != ']'; fmt++) {
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    int k;
                    for (k = (unsigned char)fmt[0]; k <= (unsigned char)fmt[2]; k++)
                        set[k] = 1;
                    fmt += 2;
                } else {
                    set[(unsigned char)*fmt] = 1;
                }
            }
            for (;;) {
                c = in_get(in);
                if (c == EOF || set[c] == negate || (width && i >= width)) {
                    in_unget(in, c);
                    break;
                }
                if (dst)
                    dst[i] = (char)c;
                i++;
            }
            if (i == 0)
                return assigned;
            if (dst) {
                dst[i] = 0;
                assigned++;
            }
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            unsigned long v = 0;
            int neg = 0, base = conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 10, any = 0;
            int left = width ? width : 1 << 30;
            c = in_get(in);
            if ((c == '-' || c == '+') && left > 0) {
                neg = c == '-';
                c = in_get(in);
                left--;
            }
            if (c == '0' && left > 0 && (conv == 'i' || base == 16)) {
                any = 1;
                c = in_get(in);
                left--;
                if ((c == 'x' || c == 'X') && left > 0) {
                    base = 16;
                    c = in_get(in);
                    left--;
                } else if (conv == 'i') {
                    base = 8;
                }
            }
            for (; left > 0; left--) {
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                if (d >= base)
                    break;
                v = v * (unsigned long)base + (unsigned long)d;
                any = 1;
                c = in_get(in);
            }
            in_unget(in, c);
            if (!any)
                return assigned ? assigned : (c == EOF ? EOF : 0);
            if (neg)
                v = 0 - v;
            if (!suppress) {
                if (conv == 'p')
                    *va_arg(ap, void **) = (void *)v;
                else if (length == 'l')
                    *va_arg(ap, long *) = (long)v;
                else if (length == 'h')
                    *va_arg(ap, short *) = (short)v;
                else if (length == 'H')
                    *va_arg(ap, signed char *) = (signed char)v;
                else
                    *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
        case 'F':
        case 'a':
        case 'A': {
            char buf[128];
            int i = 0, seen_digit = 0, seen_dot = 0, seen_e = 0;
            int left = width && width < 127 ? width : 127;
            for (; i < left; i++) {
                c = in_get(in);
                if (c >= '0' && c <= '9')
                    seen_digit = 1;
                else if ((c == '+' || c == '-') && (i == 0 || buf[i - 1] == 'e' || buf[i - 1] == 'E'))
                    ;
                else if (c == '.' && !seen_dot && !seen_e)
                    seen_dot = 1;
                else if ((c == 'e' || c == 'E') && seen_digit && !seen_e)
                    seen_e = 1;
                else {
                    in_unget(in, c);
                    break;
                }
                buf[i] = (char)c;
            }
            buf[i] = 0;
            if (!seen_digit)
                return assigned ? assigned : (c == EOF ? EOF : 0);
            if (!suppress) {
                double d = strtod(buf, 0);
                if (length == 'l' || length == 'L')
                    *va_arg(ap, double *) = d;
                else
                    *va_arg(ap, float *) = (float)d;
                assigned++;
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

int sscanf(const char *s, const char *fmt, ...)
{
    In in;
    va_list ap;
    int n;
    in.f = 0;
    in.s = s;
    in.count = 0;
    va_start(ap, fmt);
    n = vscan(&in, fmt, ap);
    va_end(ap);
    return n;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    In in;
    va_list ap;
    int n;
    in.f = f;
    in.s = 0;
    in.count = 0;
    va_start(ap, fmt);
    FLOCK(f);
    n = vscan(&in, fmt, ap);
    FUNLOCK(f);
    va_end(ap);
    return n;
}

int scanf(const char *fmt, ...)
{
    In in;
    va_list ap;
    int n;
    in.f = __stdin;
    in.s = 0;
    in.count = 0;
    va_start(ap, fmt);
    FLOCK(__stdin);
    n = vscan(&in, fmt, ap);
    FUNLOCK(__stdin);
    va_end(ap);
    return n;
}

#if defined(__linux__) || defined(__MCC_POSIX__)
/* ---- what POSIX and common practice add ---- */

#include <sys/wait.h>

static int child_of[FOPEN_MAX]; /* for popen */

int fileno(FILE *f) { return f->fd; }

FILE *fdopen(int fd, const char *mode)
{
    int oflags, flags = parse_mode(mode, &oflags);
    if (!flags) {
        errno = EINVAL;
        return 0;
    }
    {
        FILE *f = new_stream();
        if (f) {
            f->fd = fd;
            f->buf = 0;
            f->cap = f->pos = f->len = 0;
            f->unget = -1;
            f->flags = flags | F_OPEN;
        }
        return f;
    }
}

typedef struct Bounded {
    char *p;
    size_t left;
} Bounded;

static void bounded_out(void *ctx, const char *s, size_t n)
{
    Bounded *b = ctx;
    if (n > b->left)
        n = b->left;
    memcpy(b->p, s, n);
    b->p += n;
    b->left -= n;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    Bounded b;
    int r;
    b.p = s;
    b.left = n ? n - 1 : 0;
    r = __vformat(bounded_out, &b, fmt, ap);
    if (n)
        *b.p = 0;
    return r;
}

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list again;
    int n;
    va_copy(again, ap);
    n = vsnprintf(0, 0, fmt, again);
    va_end(again);
    *out = malloc((size_t)n + 1);
    if (!*out)
        return -1;
    return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char *s;
    int n = vasprintf(&s, fmt, ap), done = 0;
    if (n < 0)
        return -1;
    while (done < n) {
        long w = write(fd, s + done, (size_t)(n - done));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            n = -1;
            break;
        }
        done += (int)w;
    }
    free(s);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    In in;
    in.f = 0;
    in.s = s;
    in.count = 0;
    return vscan(&in, fmt, ap);
}

static int vfscanf_nolock(FILE *f, const char *fmt, va_list ap)
{
    In in;
    in.f = f;
    in.s = 0;
    in.count = 0;
    return vscan(&in, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    int r;
    FLOCK(f);
    r = vfscanf_nolock(f, fmt, ap);
    FUNLOCK(f);
    return r;
}

static ssize_t getdelim_nolock(char **line, size_t *cap, int delim, FILE *f)
{
    size_t n = 0;
    int c;
    if (!*line)
        *cap = 0;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            size_t want = *cap ? *cap * 2 : 120;
            char *p = realloc(*line, want);
            if (!p)
                return -1;
            *line = p;
            *cap = want;
        }
        (*line)[n++] = (char)c;
        if (c == delim)
            break;
    }
    if (!n)
        return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}

ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f)
{
    ssize_t r;
    FLOCK(f);
    r = getdelim_nolock(line, cap, delim, f);
    FUNLOCK(f);
    return r;
}

ssize_t getline(char **line, size_t *cap, FILE *f) { return getdelim(line, cap, '\n', f); }

int fseeko(FILE *f, off_t off, int whence) { return fseek(f, off, whence); }
off_t ftello(FILE *f) { return ftell(f); }
void setlinebuf(FILE *f) { setvbuf(f, 0, _IOLBF, 0); }
void setbuffer(FILE *f, char *buf, size_t size) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, size); }

int getc_unlocked(FILE *f) { return fgetc(f); }
int fgetc_unlocked(FILE *f) { return fgetc(f); }
int putc_unlocked(int c, FILE *f) { return fputc(c, f); }
int fputc_unlocked(int c, FILE *f) { return fputc(c, f); }
int getchar_unlocked(void) { return fgetc(__stdin); }
int putchar_unlocked(int c) { return fputc(c, __stdout); }
int fputs_unlocked(const char *s, FILE *f) { return fputs(s, f); }
char *fgets_unlocked(char *s, int n, FILE *f) { return fgets(s, n, f); }
size_t fwrite_unlocked(const void *buf, size_t size, size_t count, FILE *f) { return fwrite(buf, size, count, f); }
size_t fread_unlocked(void *buf, size_t size, size_t count, FILE *f) { return fread(buf, size, count, f); }
int fflush_unlocked(FILE *f) { return fflush(f); }
int feof_unlocked(FILE *f) { return feof(f); }
int ferror_unlocked(FILE *f) { return ferror(f); }
int fileno_unlocked(FILE *f) { return f->fd; }
void clearerr_unlocked(FILE *f) { clearerr(f); }
void flockfile(FILE *f)
{
    (void)f;
    FLOCK(f);
}
void funlockfile(FILE *f)
{
    (void)f;
    FUNLOCK(f);
}
char *ctermid(char *s)
{
    static char buf[16];
    return strcpy(s ? s : buf, "/dev/tty");
}

FILE *popen(const char *cmd, const char *mode)
{
    int p[2], reading = mode[0] == 'r', pid;
    FILE *f;
    if (pipe(p) < 0)
        return 0;
    f = fdopen(p[reading ? 0 : 1], mode);
    if (!f) {
        close(p[0]);
        close(p[1]);
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        close(p[reading ? 1 : 0]);
        fclose(f);
        return 0;
    }
    if (pid == 0) {
        dup2(p[reading ? 1 : 0], reading ? 1 : 0);
        close(p[0]);
        close(p[1]);
#ifdef __MCC_POSIX__
        execlp("sh", "sh", "-c", cmd, (char *)0); /* the shell on PATH: the toolchain's own when it leads */
#endif
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    close(p[reading ? 1 : 0]);
    child_of[f - files] = pid;
    return f;
}

int pclose(FILE *f)
{
    int pid = child_of[f - files], status;
    fclose(f);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return status;
}
#endif
