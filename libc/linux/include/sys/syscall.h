#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
#ifdef __x86_64__
#define SYS_getcwd 79
#define SYS_ioctl 16
#define SYS_openat 257
#define SYS_close 3
#define SYS_getdents64 217
#define SYS_read 0
#define SYS_write 1
#define SYS_exit 60
#define SYS_exit_group 231
#define SYS_reboot 169
#define SYS_gettid 186
#define SYS_getpid 39
#define SYS_kill 62
#define SYS_tkill 200
#define SYS_getrandom 318
#define SYS_fallocate 285
#define SYS_preadv 295
#define SYS_pwritev 296
#define SYS_futex 202
#define SYS_clock_getres 229
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204
#define SYS_sched_yield 24
#define SYS_fadvise64 221
#define SYS_mlock 149
#define SYS_munlock 150
#define SYS_mincore 27
#define SYS_madvise 28
#define SYS_memfd_create 319
#else
#define SYS_getcwd 17
#define SYS_ioctl 29
#define SYS_openat 56
#define SYS_close 57
#define SYS_getdents64 61
#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_reboot 142
#define SYS_gettid 178
#define SYS_getpid 172
#define SYS_kill 129
#define SYS_tkill 130
#define SYS_getrandom 278
#define SYS_fallocate 47
#define SYS_preadv 69
#define SYS_pwritev 70
#define SYS_futex 98
#define SYS_clock_getres 114
#define SYS_sched_setaffinity 122
#define SYS_sched_getaffinity 123
#define SYS_sched_yield 124
#define SYS_fadvise64 223
#define SYS_mlock 228
#define SYS_munlock 229
#define SYS_mincore 232
#define SYS_madvise 233
#define SYS_memfd_create 279
#endif
long syscall(long n, ...);
#endif
