#ifndef _DLFCN_H
#define _DLFCN_H
/* Programs built with this library are static. On x86-64 dlopen loads a shared object into one all the same
   (libc/linux/src/dl.c: for objects that keep to the C library and have no thread-local storage); elsewhere
   it always fails, and dlerror says why. */
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_NOLOAD 4
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1)
typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
int dladdr(const void *addr, Dl_info *info);
void *dlvsym(void *handle, const char *name, const char *version);
void *dlmopen(long ns, const char *path, int flags);
#endif
