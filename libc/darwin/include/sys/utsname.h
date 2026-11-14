#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
struct utsname {
    char sysname[256], nodename[256], release[256], version[256], machine[256];
};
int uname(struct utsname *u);
#endif
