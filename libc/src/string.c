#include <string.h>

typedef unsigned long W;
typedef unsigned int H;
#define ONES 0x0101010101010101ul
#define HIGHS 0x8080808080808080ul
#define HASZERO(x) (((x) - ONES) & ~(x) & HIGHS)

/* Unaligned word access is fine on every target so far. Short and
   trailing pieces are done with two overlapping moves, all loads first. */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n >= 16) {
        W t0 = *(const W *)(s + n - 16), t1 = *(const W *)(s + n - 8);
        unsigned char *e = d + n;
        while (n >= 32) {
            W a = ((const W *)s)[0], b = ((const W *)s)[1];
            W c = ((const W *)s)[2], f = ((const W *)s)[3];
            ((W *)d)[0] = a;
            ((W *)d)[1] = b;
            ((W *)d)[2] = c;
            ((W *)d)[3] = f;
            d += 32;
            s += 32;
            n -= 32;
        }
        if (n > 16) {
            W a = ((const W *)s)[0], b = ((const W *)s)[1];
            ((W *)d)[0] = a;
            ((W *)d)[1] = b;
        }
        if (n) {
            *(W *)(e - 16) = t0;
            *(W *)(e - 8) = t1;
        }
    } else if (n >= 8) {
        W a = *(const W *)s, b = *(const W *)(s + n - 8);
        *(W *)d = a;
        *(W *)(d + n - 8) = b;
    } else if (n >= 4) {
        H a = *(const H *)s, b = *(const H *)(s + n - 4);
        *(H *)d = a;
        *(H *)(d + n - 4) = b;
    } else if (n) {
        unsigned char a = s[0], b = s[n >> 1], c = s[n - 1];
        d[0] = a;
        d[n >> 1] = b;
        d[n - 1] = c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s)
        return dst;
    if (d < s) {
        if (d + n <= s)
            return memcpy(dst, src, n);
        while (n >= 8) {
            W a = *(const W *)s;
            *(W *)d = a;
            d += 8;
            s += 8;
            n -= 8;
        }
        while (n--)
            *d++ = *s++;
        return dst;
    }
    if (s + n <= d)
        return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n >= 8) {
        W a;
        d -= 8;
        s -= 8;
        a = *(const W *)s;
        *(W *)d = a;
        n -= 8;
    }
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    W w = (unsigned char)c;
    w |= w << 8;
    w |= w << 16;
    if (n >= 16) {
        unsigned char *e = d + n;
        W *p;
        w |= w << 32;
        *(W *)d = w;
        *(W *)(e - 8) = w;
        p = (W *)(((W)d + 8) & ~7ul);
        n = (size_t)(e - (unsigned char *)p) >> 3;
        while (n >= 8) {
            p[0] = w;
            p[1] = w;
            p[2] = w;
            p[3] = w;
            p[4] = w;
            p[5] = w;
            p[6] = w;
            p[7] = w;
            p += 8;
            n -= 8;
        }
        while (n--)
            *p++ = w;
    } else if (n >= 8) {
        w |= w << 32;
        *(W *)d = w;
        *(W *)(d + n - 8) = w;
    } else if (n >= 4) {
        *(H *)d = (H)w;
        *(H *)(d + n - 4) = (H)w;
    } else if (n) {
        d[0] = (unsigned char)c;
        d[n >> 1] = (unsigned char)c;
        d[n - 1] = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n >= 8 && *(const W *)x == *(const W *)y) {
        x += 8;
        y += 8;
        n -= 8;
    }
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c)
            return (void *)p;
    return 0;
}

/* Whole aligned words never cross a page, so reading past the
   terminator inside one is harmless. */
size_t strlen(const char *s)
{
    const char *p = s;
    const W *w;
    for (; (W)p & 7; p++)
        if (!*p)
            return (size_t)(p - s);
    for (w = (const W *)p; !HASZERO(*w); w++)
        ;
    for (p = (const char *)w; *p; p++)
        ;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    if ((((W)a | (W)b) & 7) == 0) {
        const W *x = (const W *)a, *y = (const W *)b;
        while (*x == *y && !HASZERO(*x)) {
            x++;
            y++;
        }
        a = (const char *)x;
        b = (const char *)y;
    }
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            break;
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) {
        strncpy(dst, src, n - 1);
        dst[n - 1] = 0;
    }
    return len;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    if (n == 0)
        return (char *)s;
    for (; (s = strchr(s, sub[0])) != 0; s++)
        if (strncmp(s, sub, n) == 0)
            return (char *)s;
    return 0;
}

size_t strspn(const char *s, const char *set)
{
    const char *p = s;
    while (*p && strchr(set, *p))
        p++;
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *set)
{
    const char *p = s;
    while (*p && !strchr(set, *p))
        p++;
    return (size_t)(p - s);
}

char *strpbrk(const char *s, const char *set)
{
    s += strcspn(s, set);
    return *s ? (char *)s : 0;
}

char *strtok(char *s, const char *sep)
{
    static char *next;
    char *tok;
    if (!s)
        s = next;
    if (!s)
        return 0;
    s += strspn(s, sep);
    if (!*s) {
        next = 0;
        return 0;
    }
    tok = s;
    s += strcspn(s, sep);
    if (*s) {
        *s = 0;
        next = s + 1;
    } else {
        next = 0;
    }
    return tok;
}

#if !defined(__linux__) && !defined(__MCC_POSIX__)
char *strerror(int err)
{
    switch (err) {
    case 0: return "no error";
    case 2: return "no such file or directory";
    case 5: return "input/output error";
    case 9: return "bad file descriptor";
    case 12: return "out of memory";
    case 13: return "permission denied";
    case 17: return "file exists";
    case 22: return "invalid argument";
    case 33: return "argument out of domain";
    case 34: return "result out of range";
    }
    return "unknown error";
}
#endif
