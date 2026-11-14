#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define MB_CUR_MAX 1
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
void exit(int status);
void _Exit(int status);
void abort(void);
int atexit(void (*fn)(void));
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *s);
int clearenv(void);
int system(const char *cmd);
int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
int rand(void);
void srand(unsigned seed);
long random(void);
void srandom(unsigned seed);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *realpath(const char *path, char *out);
int mkstemp(char *tmpl);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);
int posix_memalign(void **out, size_t align, size_t n);
void *memalign(size_t align, size_t n);
int getloadavg(double *avg, int n);
char *ptsname(int fd);
int grantpt(int fd);
int unlockpt(int fd);
int ptsname_r(int fd, char *buf, size_t n);
int posix_openpt(int flags);
void *aligned_alloc(size_t align, size_t n);
void *valloc(size_t n);
void *reallocarray(void *p, size_t count, size_t size);
lldiv_t lldiv(long long a, long long b);
long double strtold(const char *s, char **end);
void qsort_r(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *arg);
int rand_r(unsigned *seed);
double drand48(void);
double erand48(unsigned short s[3]);
long lrand48(void);
long nrand48(unsigned short s[3]);
long mrand48(void);
long jrand48(unsigned short s[3]);
void srand48(long seed);
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *out, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
#include <alloca.h>
#endif
