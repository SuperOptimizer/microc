/* Not ISO C: file status, as much as the mcc tools need. The layout is Darwin's. */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H
struct timespec {
    long tv_sec;
    long tv_nsec;
};
struct stat {
    int st_dev;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned long st_ino;
    unsigned st_uid, st_gid;
    int st_rdev;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    long st_size, st_blocks;
    int st_blksize;
    unsigned st_flags, st_gen;
    int st_lspare;
    long st_qspare[2];
};
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
int stat(const char *path, struct stat *st);
int mkfifo(const char *path, int mode);
#endif
