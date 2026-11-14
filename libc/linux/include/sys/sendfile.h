#ifndef _SYS_SENDFILE_H
#define _SYS_SENDFILE_H
#include <sys/types.h>
ssize_t sendfile(int out, int in, off_t *off, size_t n);
#endif
