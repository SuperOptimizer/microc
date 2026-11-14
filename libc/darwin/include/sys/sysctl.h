/* Not ISO C: just enough to ask for the number of processors. */
#ifndef _SYS_SYSCTL_H
#define _SYS_SYSCTL_H
#include <stddef.h>
#define CTL_HW 6
#define HW_NCPU 3
int sysctl(int *name, unsigned namelen, void *old, size_t *oldlen, void *newp, size_t newlen);
#endif
