#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>
#define MAXPATHLEN PATH_MAX
#define MAXHOSTNAMELEN 64
#define MAXSYMLINKS 20
#define NOFILE 256
#define HZ 100
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define NBBY 8
#endif
