/* The names a shared object loaded by dl.c may ask this program's C library for (see glibcabi.c), each with
   the function or object that answers to it: F(name) is this library's function of that name, A(name, fn)
   another function, D(name, object) an object. Declared here without their types, so that no header is
   needed: only the addresses are taken. The list is what NVIDIA's libcuda.so.1 and
   libnvidia-ptxjitcompiler.so.1 (driver 615) import; a name that is missing shows as "undefined symbol" in
   dlerror, and is added here. __glibc_lookup sorts the table at its first use. */
#ifdef __x86_64__
#define GLIBC_LIST(F, A, D) \
    A(_IO_getc, __glibc_IO_getc) \
    A(_IO_putc, __glibc_IO_putc) \
    F(__assert_fail) \
    A(__cxa_atexit, __glibc_cxa_atexit) \
    A(__cxa_finalize, __glibc_cxa_finalize) \
    F(__errno_location) \
    A(__fxstat, __glibc_fxstat) \
    A(__fxstat64, __glibc_fxstat) \
    A(__getdelim, __glibc_getdelim) \
    A(__isoc99_fscanf, __glibc_isoc99_fscanf) \
    A(__isoc99_sscanf, __glibc_isoc99_sscanf) \
    A(__lxstat, __glibc_lxstat) \
    A(__lxstat64, __glibc_lxstat) \
    A(__register_atfork, __glibc_register_atfork) \
    A(__xmknod, __glibc_xmknod) \
    A(__xstat, __glibc_xstat) \
    A(__xstat64, __glibc_xstat) \
    F(_setjmp) \
    F(abort) \
    F(access) \
    A(alphasort64, alphasort) \
    F(bind) \
    F(bsearch) \
    F(calloc) \
    F(ceil) \
    F(chmod) \
    F(chown) \
    F(clock) \
    F(clock_gettime) \
    F(close) \
    F(closedir) \
    F(connect) \
    F(cos) \
    F(dlclose) \
    F(dlerror) \
    F(dlmopen) \
    F(dlopen) \
    F(dlsym) \
    F(dlvsym) \
    F(dup) \
    F(dup2) \
    F(dup3) \
    A(eventfd, __glibc_eventfd) \
    F(execle) \
    F(execvp) \
    F(exit) \
    F(expf) \
    F(faccessat) \
    F(fchmod) \
    F(fclose) \
    F(fcntl) \
    F(fdopen) \
    F(fegetenv) \
    F(feof) \
    F(ferror) \
    F(fesetenv) \
    F(fflush) \
    F(fgetc) \
    F(fgets) \
    F(fileno) \
    F(flock) \
    F(floor) \
    F(fopen) \
    A(fopen64, fopen) \
    F(fork) \
    F(fprintf) \
    F(fputc) \
    F(fputs) \
    F(fread) \
    F(free) \
    F(fscanf) \
    F(fseek) \
    F(ftell) \
    F(ftruncate) \
    F(fwrite) \
    F(get_nprocs) \
    F(getegid) \
    F(getenv) \
    F(geteuid) \
    F(gethostname) \
    F(getopt_long) \
    F(getpagesize) \
    F(getpid) \
    F(getrlimit) \
    F(getsockopt) \
    F(gettimeofday) \
    F(getuid) \
    F(ioctl) \
    F(isalnum) \
    F(isalpha) \
    F(isatty) \
    F(isspace) \
    F(listen) \
    F(localtime_r) \
    F(log) \
    F(log10) \
    F(longjmp) \
    F(lround) \
    F(lseek) \
    F(madvise) \
    F(malloc) \
    F(memalign) \
    F(memchr) \
    F(memcmp) \
    F(memcpy) \
    F(memmove) \
    F(memset) \
    F(mkdir) \
    F(mkfifo) \
    A(mlock, __glibc_mlock) \
    F(mmap) \
    A(mmap64, mmap) \
    F(mprotect) \
    F(mremap) \
    F(msync) \
    F(munlock) \
    F(munmap) \
    F(nanosleep) \
    A(nftw, __glibc_nftw) \
    F(open) \
    A(open64, __glibc_open) \
    A(openat, __glibc_openat) \
    A(openat64, __glibc_openat) \
    F(opendir) \
    D(optarg, optarg) \
    D(optind, optind) \
    F(pipe) \
    F(poll) \
    A(posix_spawn, __glibc_posix_spawn) \
    A(posix_spawn_file_actions_adddup2, __glibc_spawn_adddup2) \
    A(posix_spawn_file_actions_addopen, __glibc_spawn_addopen) \
    A(posix_spawn_file_actions_destroy, __glibc_spawn_destroy) \
    A(posix_spawn_file_actions_init, __glibc_spawn_init) \
    F(pow) \
    F(printf) \
    F(pthread_attr_init) \
    F(pthread_attr_setstack) \
    F(pthread_cond_broadcast) \
    F(pthread_cond_destroy) \
    F(pthread_cond_init) \
    F(pthread_cond_signal) \
    F(pthread_cond_timedwait) \
    F(pthread_cond_wait) \
    F(pthread_condattr_init) \
    F(pthread_condattr_setpshared) \
    F(pthread_create) \
    F(pthread_detach) \
    F(pthread_getspecific) \
    F(pthread_join) \
    F(pthread_key_create) \
    F(pthread_key_delete) \
    F(pthread_kill) \
    F(pthread_mutex_destroy) \
    F(pthread_mutex_init) \
    F(pthread_mutex_lock) \
    F(pthread_mutex_trylock) \
    F(pthread_mutex_unlock) \
    F(pthread_mutexattr_destroy) \
    F(pthread_mutexattr_init) \
    A(pthread_mutexattr_setprotocol, __glibc_pthread_mutexattr_setprotocol) \
    F(pthread_mutexattr_setpshared) \
    F(pthread_mutexattr_settype) \
    F(pthread_once) \
    F(pthread_rwlock_destroy) \
    F(pthread_rwlock_init) \
    F(pthread_rwlock_rdlock) \
    F(pthread_rwlock_trywrlock) \
    F(pthread_rwlock_unlock) \
    F(pthread_rwlock_wrlock) \
    F(pthread_rwlockattr_destroy) \
    F(pthread_rwlockattr_init) \
    A(pthread_rwlockattr_setpshared, __glibc_pthread_rwlockattr_setpshared) \
    F(pthread_self) \
    F(pthread_setspecific) \
    F(pthread_spin_destroy) \
    F(pthread_spin_init) \
    F(pthread_spin_lock) \
    F(pthread_spin_trylock) \
    F(pthread_spin_unlock) \
    F(putchar) \
    F(puts) \
    F(qsort) \
    F(rand_r) \
    F(read) \
    F(readdir) \
    F(readlink) \
    F(realloc) \
    F(realpath) \
    F(recvmsg) \
    F(remove) \
    F(rewind) \
    F(rmdir) \
    A(scandir64, scandir) \
    F(sched_get_priority_max) \
    F(sched_get_priority_min) \
    F(sched_yield) \
    F(sem_destroy) \
    F(sem_init) \
    F(sem_post) \
    F(sem_timedwait) \
    F(sem_trywait) \
    F(sem_wait) \
    F(sendmsg) \
    F(setenv) \
    F(setrlimit) \
    F(setsockopt) \
    A(shm_open, __glibc_shm_open) \
    A(shm_unlink, __glibc_shm_unlink) \
    F(shutdown) \
    A(sigaction, __glibc_sigaction) \
    F(sin) \
    F(snprintf) \
    F(socket) \
    F(sprintf) \
    F(sqrt) \
    F(sscanf) \
    D(stderr, __stderr) \
    D(stdin, __stdin) \
    D(stdout, __stdout) \
    F(stpcpy) \
    F(strcasecmp) \
    F(strcasestr) \
    F(strcat) \
    F(strchr) \
    F(strcmp) \
    F(strcoll) \
    F(strcpy) \
    F(strcspn) \
    F(strdup) \
    F(strerror) \
    A(strerror_r, __glibc_strerror_r) \
    F(strlen) \
    F(strncasecmp) \
    F(strncat) \
    F(strncmp) \
    F(strncpy) \
    F(strnlen) \
    F(strpbrk) \
    F(strrchr) \
    F(strstr) \
    F(strtod) \
    F(strtok) \
    F(strtok_r) \
    F(strtol) \
    F(strtoll) \
    F(strtoul) \
    F(strtoull) \
    F(symlink) \
    F(syscall) \
    F(sysconf) \
    F(sysinfo) \
    F(time) \
    A(timerfd_create, __glibc_timerfd_create) \
    A(timerfd_settime, __glibc_timerfd_settime) \
    F(tolower) \
    F(toupper) \
    F(uname) \
    F(unlink) \
    F(unsetenv) \
    F(usleep) \
    F(vfprintf) \
    F(vsnprintf) \
    F(vsprintf) \
    F(waitpid) \
    F(write) \
    /* asked for with dlsym */ \
    A(accept4, __glibc_accept4) \
    F(pipe2) \
    F(pthread_getaffinity_np) \
    F(pthread_setaffinity_np) \
    F(sched_getcpu) \
    /* more that the driver's 620 libraries import (615 did not) */ \
    F(dladdr) \
    F(rename) \
    F(wcslen) \
    /* end */

#define DECL_F(n) extern void n(void);
#define DECL_A(n, fn) extern void fn(void);
#define DECL_D(n, obj) extern char obj[];
GLIBC_LIST(DECL_F, DECL_A, DECL_D)

struct glibc_entry {
    const char *name;
    void *addr;
};
#define ROW_F(n) {#n, (void *)n},
#define ROW_A(n, fn) {#n, (void *)fn},
#define ROW_D(n, obj) {#n, (void *)obj},
struct glibc_entry __glibc_table[] = {GLIBC_LIST(ROW_F, ROW_A, ROW_D) {0, 0}};
const int __glibc_table_n = (int)(sizeof __glibc_table / sizeof __glibc_table[0]) - 1;
#else
extern int __glibctab_nothing_here;
#endif
