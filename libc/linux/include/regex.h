#ifndef _REGEX_H
#define _REGEX_H
#include <sys/types.h>
typedef long regoff_t;
typedef struct {
    size_t re_nsub;
    void *__prog;
} regex_t;
typedef struct {
    regoff_t rm_so, rm_eo;
} regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8
#define REG_NOTBOL 1
#define REG_NOTEOL 2
#define REG_STARTEND 4
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13
int regcomp(regex_t *re, const char *pattern, int flags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *m, int flags);
size_t regerror(int err, const regex_t *re, char *buf, size_t n);
void regfree(regex_t *re);
#endif
