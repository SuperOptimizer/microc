/* dlopen for x86-64 Linux: a loader of shared objects inside a statically linked program. There is no ld.so
   and no shared C library here: what a loaded object imports from "libc.so.6" and its companions (libm,
   libdl, libpthread, librt) is this library itself, looked up by name in the table of glibcabi.c, which holds
   the functions under the names and with the calling conventions the GNU C library gives them. That is
   enough for objects written in C that keep to the library's functions, such as NVIDIA's libcuda.so.1; it
   is not a general dynamic linker:
     - no thread-local storage in a loaded object (PT_TLS, the TLS relocations): refused with a message;
     - symbol versions are ignored (a name has one meaning);
     - nothing is ever unmapped, and destructors are not run;
     - objects are bound when loaded (RTLD_LAZY is RTLD_NOW).
   MCC_DLDEBUG=1 in the environment reports every request, where an object was found and what it lacked. */
#ifdef __x86_64__
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef unsigned char u8;

typedef struct {
    u8 ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;
typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} Phdr;
typedef struct {
    int64_t tag;
    u64 val;
} Dyn;
typedef struct {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
} Sym;
typedef struct {
    u64 offset, info;
    int64_t addend;
} Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7 };
enum {
    DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
    DT_INIT = 12, DT_SONAME = 14, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27,
    DT_GNU_HASH = 0x6ffffef5
};
enum { R_64 = 1, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8, R_DTPMOD64 = 16, R_DTPOFF64 = 17, R_TPOFF64 = 18, R_IRELATIVE = 37 };

typedef struct Lib {
    struct Lib *next;
    char *path, *soname;
    u8 *base;      /* what a virtual address of the object is added to */
    u8 *lo;
    size_t span;
    Dyn *dyn;
    Sym *symtab;
    const char *strtab;
    u32 *hash;     /* DT_HASH, or 0 */
    u32 *gnu;      /* DT_GNU_HASH, or 0 */
    struct Lib **needs;
    int nneeds;
    int libc;      /* the handle that stands for this program and the C library in it */
    int ready;
} Lib;

void *__glibc_lookup(const char *name); /* glibcabi.c */
extern char **environ;

static Lib *libs;
static Lib self_lib;
static pthread_mutex_t dl_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static char dl_msg[512];
static int dl_failed;

static int dl_debug(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MCC_DLDEBUG");
        v = e && e[0] && e[0] != '0';
    }
    return v;
}

static void dl_err(const char *what, const char *arg)
{
    snprintf(dl_msg, sizeof dl_msg, "%s%s%s", what, arg ? ": " : "", arg ? arg : "");
    dl_failed = 1;
    if (dl_debug())
        fprintf(stderr, "dl: %s\n", dl_msg);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* the names under which the GNU C library is asked for: all of them are this program */
static int is_libc(const char *name)
{
    static const char *const names[] = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
                                        "ld-linux-x86-64.so.2", "libutil.so.1", 0};
    int i;
    name = base_name(name);
    for (i = 0; names[i]; i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

static u32 sysv_hash(const char *s)
{
    u32 h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (u8)*s;
        g = h & 0xf0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static u32 gnu_hash(const char *s)
{
    u32 h = 5381;
    for (; *s; s++)
        h = h * 33 + (u8)*s;
    return h;
}

/* a symbol the object defines (global or weak), by name */
static Sym *lib_sym(Lib *l, const char *name)
{
    Sym *s;
    if (l->gnu) {
        u32 nb = l->gnu[0], symoff = l->gnu[1], bloom = l->gnu[2];
        u32 *buckets = l->gnu + 4 + bloom * 2, *chain = buckets + nb;
        u32 h = gnu_hash(name), i;
        if (!nb)
            return 0;
        i = buckets[h % nb];
        if (i < symoff)
            return 0;
        for (;; i++) {
            u32 c = chain[i - symoff];
            s = &l->symtab[i];
            if ((c | 1) == (h | 1) && s->shndx && !strcmp(l->strtab + s->name, name))
                return s;
            if (c & 1)
                return 0;
        }
    }
    if (l->hash) {
        u32 nb = l->hash[0], *buckets = l->hash + 2, *chain = buckets + nb, i;
        if (!nb)
            return 0;
        for (i = buckets[sysv_hash(name) % nb]; i; i = chain[i]) {
            s = &l->symtab[i];
            if (s->shndx && !strcmp(l->strtab + s->name, name))
                return s;
        }
    }
    return 0;
}

static void *sym_addr(Lib *l, Sym *s)
{
    void *p = l->base + s->value;
    if ((s->info & 15) == 10) /* STT_GNU_IFUNC: the address is what its resolver returns */
        p = ((void *(*)(void))p)();
    return p;
}

/* NAME as an object L refers to it: this program's C library first, then L, then what L needs, then every
   object loaded before */
static void *resolve(Lib *l, const char *name, int depth)
{
    void *p = __glibc_lookup(name);
    Sym *s;
    Lib *o;
    int i;
    if (p)
        return p;
    if (l && !l->libc) {
        s = lib_sym(l, name);
        if (s)
            return sym_addr(l, s);
        for (i = 0; i < l->nneeds && depth < 8; i++)
            if (!l->needs[i]->libc && (p = resolve(l->needs[i], name, depth + 1)) != 0)
                return p;
    }
    if (depth == 0)
        for (o = libs; o; o = o->next)
            if (o != l && (s = lib_sym(o, name)) != 0)
                return sym_addr(o, s);
    return 0;
}

static int relocate(Lib *l, Rela *r, size_t n)
{
    size_t i;
    int bad = 0;
    for (i = 0; i < n; i++, r++) {
        u32 type = (u32)r->info, si = (u32)(r->info >> 32);
        u64 *where = (u64 *)(l->base + r->offset);
        if (type == R_RELATIVE)
            *where = (u64)(l->base + r->addend);
        else if (type == R_IRELATIVE)
            *where = (u64)((void *(*)(void))(l->base + r->addend))();
        else if (type == R_64 || type == R_GLOB_DAT || type == R_JUMP_SLOT) {
            Sym *s = &l->symtab[si];
            const char *name = l->strtab + s->name;
            void *p = resolve(l, name, 0);
            if (!p && (s->info >> 4) != 2) { /* (a weak one that nothing defines is 0) */
                if (!bad) { /* (the first is the message; the trace has them all) */
                    char buf[400];
                    snprintf(buf, sizeof buf, "%s (%s wants it)", name, l->path);
                    dl_err("undefined symbol", buf);
                } else if (dl_debug())
                    fprintf(stderr, "dl: undefined symbol: %s\n", name);
                bad++;
                continue;
            }
            *where = (u64)p + (type == R_64 && p ? (u64)r->addend : 0);
        } else if (type == R_DTPMOD64 || type == R_DTPOFF64 || type == R_TPOFF64) {
            dl_err("the object uses thread-local storage, which this loader has not", l->path);
            return -1;
        } else if (type) {
            char num[32];
            sprintf(num, "%u", type);
            dl_err("relocation of a kind this loader has not", num);
            return -1;
        }
    }
    return bad ? -1 : 0;
}

static Lib *open_lib(const char *name, int depth);

/* NAME in the directory DIR (DN characters of it). With VER, NAME is "libx.so.1" and any "libx.so.*" there
   will do, the last in order of name: a container run time mounts a driver's library under its full version
   (libcuda.so.615.71.10) and leaves the short name to a link that ldconfig makes, which may not have run. */
static int try_path(const char *dir, size_t dn, const char *name, int ver, char *out, size_t cap)
{
    DIR *d;
    struct dirent *de;
    const char *so;
    size_t pn;
    int found = 0;
    if (dn + strlen(name) + 2 > cap)
        return 0;
    memcpy(out, dir, dn);
    out[dn] = '/';
    if (!ver) {
        strcpy(out + dn + 1, name);
        return access(out, R_OK) == 0;
    }
    so = strstr(name, ".so.");
    if (!so)
        return 0;
    pn = (size_t)(so - name) + 4;
    out[dn] = 0;
    d = opendir(out);
    if (!d)
        return 0;
    out[dn] = '/';
    out[dn + 1] = 0;
    while ((de = readdir(d)) != 0) {
        if (strncmp(de->d_name, name, pn) != 0 || dn + strlen(de->d_name) + 2 > cap)
            continue;
        if (found && strcmp(de->d_name, out + dn + 1) <= 0)
            continue;
        strcpy(out + dn + 1, de->d_name);
        found = 1;
    }
    closedir(d);
    return found && access(out, R_OK) == 0;
}

/* where NAME is: as given when it has a slash; else LD_LIBRARY_PATH, then the directories where distributions
   and NVIDIA's container run time put libraries (there is no ld.so.cache to ask); first under its own name
   everywhere, then under a longer version of it */
static int find_lib(const char *name, char *out, size_t cap)
{
    static const char *const dirs[] = {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/lib/x86_64-linux-gnu", "/lib64",
                                       "/usr/lib", "/lib", "/usr/local/lib", "/usr/local/nvidia/lib64",
                                       "/usr/local/nvidia/lib", 0};
    const char *p, *e;
    int i, ver;
    if (strchr(name, '/')) {
        if (strlen(name) + 1 > cap || access(name, R_OK) != 0)
            return 0;
        strcpy(out, name);
        return 1;
    }
    for (ver = 0; ver < 2; ver++) {
        for (p = getenv("LD_LIBRARY_PATH"); p && *p; p = *e ? e + 1 : e) {
            e = p + strcspn(p, ":");
            if (e > p && try_path(p, (size_t)(e - p), name, ver, out, cap))
                return 1;
        }
        for (i = 0; dirs[i]; i++)
            if (try_path(dirs[i], strlen(dirs[i]), name, ver, out, cap))
                return 1;
    }
    return 0;
}

static Lib *find_loaded(const char *name)
{
    Lib *l;
    const char *b = base_name(name);
    for (l = libs; l; l = l->next)
        if (!strcmp(l->path, name) || !strcmp(base_name(l->path), b) || (l->soname && !strcmp(l->soname, b)))
            return l;
    return 0;
}

static Lib *load(const char *path, int depth)
{
    Ehdr eh;
    Phdr *ph = 0;
    Lib *l = 0;
    Dyn *d;
    u64 lo = ~(u64)0, hi = 0;
    u8 *map = (u8 *)MAP_FAILED;
    Rela *rela = 0, *jmprel = 0;
    size_t relasz = 0, pltsz = 0, ninit = 0, k;
    u64 init = 0, *init_array = 0;
    int fd, i, n;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dl_err("cannot open", path);
        return 0;
    }
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.ident, "\177ELF\2\1", 6) != 0 || eh.type != 3 ||
        eh.machine != 62 || eh.phentsize != sizeof(Phdr)) {
        dl_err("not a shared object for x86-64", path);
        goto fail;
    }
    ph = (Phdr *)malloc(eh.phnum * sizeof(Phdr));
    if (!ph || pread(fd, ph, eh.phnum * sizeof(Phdr), (off_t)eh.phoff) != (ssize_t)(eh.phnum * sizeof(Phdr))) {
        dl_err("cannot read", path);
        goto fail;
    }
    for (i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_TLS && ph[i].memsz) {
            dl_err("the object has thread-local storage, which this loader has not", path);
            goto fail;
        }
        if (ph[i].type != PT_LOAD)
            continue;
        if (ph[i].vaddr < lo)
            lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi)
            hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo) {
        dl_err("nothing to load in", path);
        goto fail;
    }
    lo &= ~(u64)4095;
    hi = (hi + 4095) & ~(u64)4095;
    map = (u8 *)mmap(0, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == (u8 *)MAP_FAILED) {
        dl_err("no address space for", path);
        goto fail;
    }
    l = (Lib *)calloc(1, sizeof *l);
    if (!l)
        goto fail;
    l->lo = map;
    l->span = hi - lo;
    l->base = map - lo;
    l->path = strdup(path);
    for (i = 0; i < eh.phnum; i++) {
        u64 start, end, fend, off;
        int prot;
        if (ph[i].type == PT_DYNAMIC)
            l->dyn = (Dyn *)(l->base + ph[i].vaddr);
        if (ph[i].type != PT_LOAD)
            continue;
        prot = (ph[i].flags & 4 ? PROT_READ : 0) | (ph[i].flags & 2 ? PROT_WRITE : 0) | (ph[i].flags & 1 ? PROT_EXEC : 0);
        start = ph[i].vaddr & ~(u64)4095;
        off = ph[i].offset - (ph[i].vaddr - start);
        fend = ph[i].vaddr + ph[i].filesz;                  /* where what the file holds ends */
        end = (ph[i].vaddr + ph[i].memsz + 4095) & ~(u64)4095;
        if (ph[i].filesz) {
            u64 fpages = (fend + 4095) & ~(u64)4095;
            if (mmap(l->base + start, fpages - start, prot, MAP_PRIVATE | MAP_FIXED, fd, (off_t)off) == MAP_FAILED) {
                dl_err("cannot map", path);
                goto fail;
            }
            if (ph[i].memsz > ph[i].filesz && (prot & PROT_WRITE))
                memset(l->base + fend, 0, fpages - fend); /* the rest of the last page is not the file's */
            if (end > fpages && mmap(l->base + fpages, end - fpages, prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
                dl_err("cannot map", path);
                goto fail;
            }
        } else if (mmap(l->base + start, end - start, prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
            dl_err("cannot map", path);
            goto fail;
        }
    }
    close(fd);
    fd = -1;
    if (!l->dyn) {
        dl_err("no dynamic section in", path);
        goto fail;
    }
    n = 0;
    for (d = l->dyn; d->tag; d++) {
        u8 *p = l->base + d->val;
        switch ((u64)d->tag) {
        case DT_NEEDED: n++; break;
        case DT_STRTAB: l->strtab = (const char *)p; break;
        case DT_SYMTAB: l->symtab = (Sym *)p; break;
        case DT_HASH: l->hash = (u32 *)p; break;
        case DT_GNU_HASH: l->gnu = (u32 *)p; break;
        case DT_RELA: rela = (Rela *)p; break;
        case DT_RELASZ: relasz = d->val; break;
        case DT_JMPREL: jmprel = (Rela *)p; break;
        case DT_PLTRELSZ: pltsz = d->val; break;
        case DT_INIT: init = d->val; break;
        case DT_INIT_ARRAY: init_array = (u64 *)p; break;
        case DT_INIT_ARRAYSZ: ninit = d->val / 8; break;
        }
    }
    if (!l->strtab || !l->symtab || (!l->hash && !l->gnu)) {
        dl_err("no symbol table in", path);
        goto fail;
    }
    for (d = l->dyn; d->tag; d++)
        if (d->tag == DT_SONAME)
            l->soname = strdup(l->strtab + d->val);
    /* on the list before what it needs is loaded, so that a circle of objects ends */
    l->next = libs;
    libs = l;
    l->needs = (Lib **)calloc((size_t)n + 1, sizeof(Lib *));
    for (d = l->dyn; d->tag; d++)
        if (d->tag == DT_NEEDED) {
            Lib *o = open_lib(l->strtab + d->val, depth + 1);
            if (!o) {
                libs = l->next; /* (it stays mapped: nothing is ever taken down) */
                return 0;
            }
            l->needs[l->nneeds++] = o;
        }
    if (relocate(l, rela, relasz / sizeof(Rela)) || relocate(l, jmprel, pltsz / sizeof(Rela))) {
        Lib **pp;
        for (pp = &libs; *pp; pp = &(*pp)->next)
            if (*pp == l) {
                *pp = l->next;
                break;
            }
        return 0;
    }
    l->ready = 1;
    if (dl_debug())
        fprintf(stderr, "dl: %s at %p, %lu bytes\n", path, (void *)l->lo, (unsigned long)l->span);
    free(ph);
    /* its initialisers, as ld.so calls them */
    if (init)
        ((void (*)(int, char **, char **))(l->base + init))(0, 0, environ);
    for (k = 0; k < ninit; k++)
        if (init_array[k] && init_array[k] != ~(u64)0)
            ((void (*)(int, char **, char **))init_array[k])(0, 0, environ);
    return l;
fail:
    if (fd >= 0)
        close(fd);
    free(ph);
    if (!l && map != (u8 *)MAP_FAILED)
        munmap(map, hi - lo);
    return 0;
}

static Lib *open_lib(const char *name, int depth)
{
    char path[1024];
    Lib *l;
    if (!name || is_libc(name)) {
        self_lib.libc = 1;
        self_lib.ready = 1;
        self_lib.path = (char *)"";
        return &self_lib;
    }
    if (depth > 16) {
        dl_err("objects that need each other too deeply", name);
        return 0;
    }
    l = find_loaded(name);
    if (l)
        return l;
    if (!find_lib(name, path, sizeof path)) {
        dl_err("no such shared object", name);
        return 0;
    }
    l = find_loaded(path);
    return l ? l : load(path, depth);
}

void *dlopen(const char *path, int flags)
{
    Lib *l;
    (void)flags;
    pthread_mutex_lock(&dl_lock);
    if (dl_debug())
        fprintf(stderr, "dl: dlopen %s\n", path ? path : "(this program)");
    l = open_lib(path, 0);
    pthread_mutex_unlock(&dl_lock);
    return l;
}

void *dlsym(void *handle, const char *name)
{
    Lib *l = (Lib *)handle;
    void *p;
    pthread_mutex_lock(&dl_lock);
    if (handle == RTLD_DEFAULT || handle == RTLD_NEXT || l->libc)
        p = resolve(0, name, 0);
    else {
        Sym *s = lib_sym(l, name);
        int i;
        p = s ? sym_addr(l, s) : 0;
        for (i = 0; !p && i < l->nneeds; i++)
            p = l->needs[i]->libc ? __glibc_lookup(name) : resolve(l->needs[i], name, 1);
    }
    if (!p)
        dl_err("undefined symbol", name);
    pthread_mutex_unlock(&dl_lock);
    return p;
}

void *dlvsym(void *handle, const char *name, const char *version)
{
    (void)version;
    return dlsym(handle, name);
}

void *dlmopen(long ns, const char *path, int flags)
{
    (void)ns;
    return dlopen(path, flags);
}

int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

char *dlerror(void)
{
    if (!dl_failed)
        return 0;
    dl_failed = 0;
    return dl_msg;
}

int dladdr(const void *addr, Dl_info *info)
{
    Lib *l;
    for (l = libs; l; l = l->next)
        if ((const u8 *)addr >= l->lo && (const u8 *)addr < l->lo + l->span) {
            info->dli_fname = l->path;
            info->dli_fbase = l->lo;
            info->dli_sname = 0;
            info->dli_saddr = 0;
            return 1;
        }
    return 0;
}
#else
extern int __dl_nothing_here;
#endif
