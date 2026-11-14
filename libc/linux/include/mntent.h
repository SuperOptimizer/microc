#ifndef _MNTENT_H
#define _MNTENT_H
#include <stdio.h>
#define MOUNTED "/proc/mounts"
#define MNTTAB "/etc/fstab"
#define MNTTYPE_SWAP "swap"
#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO "ro"
#define MNTOPT_RW "rw"
#define MNTOPT_NOAUTO "noauto"
struct mntent {
    char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts;
    int mnt_freq, mnt_passno;
};
FILE *setmntent(const char *path, const char *mode);
struct mntent *getmntent(FILE *f);
struct mntent *getmntent_r(FILE *f, struct mntent *m, char *buf, int n);
int addmntent(FILE *f, const struct mntent *m);
int endmntent(FILE *f);
char *hasmntopt(const struct mntent *m, const char *opt);
#endif
