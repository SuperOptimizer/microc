#ifndef _STRING_H
#define _STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *mempcpy(void *dst, const void *src, size_t n);
void *memmem(const void *h, size_t hn, const void *n, size_t nn);
void *memccpy(void *dst, const void *src, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *stpcpy(char *dst, const char *src);
char *stpncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strchrnul(const char *s, int c);
char *strstr(const char *s, const char *sub);
char *strcasestr(const char *s, const char *sub);
size_t strspn(const char *s, const char *set);
size_t strcspn(const char *s, const char *set);
char *strpbrk(const char *s, const char *set);
char *strtok(char *s, const char *sep);
char *strtok_r(char *s, const char *sep, char **save);
char *strsep(char **s, const char *sep);
char *strerror(int err);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strsignal(int sig);
int strverscmp(const char *a, const char *b);
size_t strlcpy(char *dst, const char *src, size_t n);
#include <strings.h>
int strerror_r(int err, char *buf, size_t n);
#endif
