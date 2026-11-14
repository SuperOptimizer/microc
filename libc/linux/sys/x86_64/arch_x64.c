/* x86-64 Linux: the thread pointer. The compiler and the linker follow the ABI of the processor: %fs
   points at a word that holds its own address, and the thread's copy of the TLS image ends there.
   The rest of the library keeps its own picture (struct __tls_image in thread.h: the copy starts
   __tls.bias after "the thread pointer"); sys.c makes __tls.memsz cover the image, padded to its
   alignment, and 16 bytes more, and %fs points at these 16 bytes. */
#include "../../src/thread.h"

long __clone_raw(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *fs, int *ctid);

static char *fs_of(void *tp)
{
    char *fs = (char *)tp + __tls.bias + __tls.memsz - 16;
    *(char **)fs = fs;
    return fs;
}

void __set_tp(void *tp)
{
    (__syscall)(158, 0x1002, (long)fs_of(tp), 0, 0, 0, 0); /* arch_prctl(ARCH_SET_FS) */
}

long __clone(int (*fn)(void *), void *stack, long flags, void *arg, int *ptid, void *tls, int *ctid)
{
    return __clone_raw(fn, stack, flags, arg, ptid, fs_of(tls), ctid);
}
