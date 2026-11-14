/* Not ISO C: what the mcc driver needs to run compile jobs in parallel. */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WTERMSIG(s) ((s) & 0x7f)
#define WNOHANG 1
int waitpid(int pid, int *status, int options);
#endif
