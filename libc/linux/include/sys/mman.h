#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/types.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)
#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4
#define MREMAP_MAYMOVE 1
void *mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off);
#define mmap64 mmap
int munmap(void *addr, size_t n);
int mprotect(void *addr, size_t n, int prot);
void *mremap(void *addr, size_t oldn, size_t newn, int flags, ...);
int msync(void *addr, size_t n, int flags);
int mlock(const void *addr, size_t n);
int madvise(void *addr, size_t n, int advice);
#define MAP_GROWSDOWN 0x100
#define MAP_LOCKED 0x2000
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000
#define MAP_HUGETLB 0x40000
#define MAP_FIXED_NOREPLACE 0x100000
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
#define MADV_REMOVE 9
#define MADV_DONTFORK 10
#define MADV_DOFORK 11
#define MADV_MERGEABLE 12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15
#define MADV_DONTDUMP 16
#define MADV_DODUMP 17
#define POSIX_MADV_NORMAL 0
#define POSIX_MADV_RANDOM 1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED 3
#define POSIX_MADV_DONTNEED 4
#define MFD_CLOEXEC 1
#define MFD_ALLOW_SEALING 2
int munlock(const void *addr, size_t n);
int mincore(void *addr, size_t n, unsigned char *vec);
int posix_madvise(void *addr, size_t n, int advice);
int memfd_create(const char *name, unsigned flags);
#endif
