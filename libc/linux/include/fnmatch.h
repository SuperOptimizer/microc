#ifndef _FNMATCH_H
#define _FNMATCH_H
#define FNM_NOMATCH 1
#define FNM_PATHNAME 1
#define FNM_NOESCAPE 2
#define FNM_PERIOD 4
#define FNM_LEADING_DIR 8
#define FNM_CASEFOLD 16
int fnmatch(const char *pattern, const char *s, int flags);
#endif
