/* Not ISO C: reading directories. The entry layout is Darwin's. */
#ifndef _DIRENT_H
#define _DIRENT_H
struct dirent {
    unsigned long d_ino, d_seekoff;
    unsigned short d_reclen, d_namlen;
    unsigned char d_type;
    char d_name[1024];
};
typedef struct __dir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
