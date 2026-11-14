#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H
#define PR_SET_PDEATHSIG 1
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_SET_CHILD_SUBREAPER 36
int prctl(int op, ...);
#endif
