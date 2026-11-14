/* Reading directories. The entry layout is Darwin's. */
#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>
struct dirent {
    unsigned long d_ino, d_seekoff;
    unsigned short d_reclen, d_namlen;
    unsigned char d_type;
    char d_name[1024];
};
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
typedef struct __dir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
