#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>
struct group {
    char *gr_name, *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};
struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int initgroups(const char *user, gid_t group);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *n);
int setgroups(size_t n, const gid_t *list);
#endif
