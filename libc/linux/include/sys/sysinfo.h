#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H
struct sysinfo {
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char __reserved[4];
};
int sysinfo(struct sysinfo *info);
int get_nprocs(void);
#endif
