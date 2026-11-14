/* Memory mapping, with Darwin's numbers. */
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
#define MAP_ANON 0x1000
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t n, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t n);
int mprotect(void *addr, size_t n, int prot);
#endif
