#ifndef _STRINGS_H
#define _STRINGS_H
#include <stddef.h>
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int ffs(int v);
void bzero(void *p, size_t n);
void bcopy(const void *from, void *to, size_t n);
int bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
int ffsl(long v);
int ffsll(long long v);
#endif
