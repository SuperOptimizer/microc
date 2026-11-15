/* mcudahw: the modules of PTX that the compiler left in the program, on the device. A translation unit's
   module may name what another defines (a device function, a device global), so the units that share a name
   are linked together by the driver (cuLink*), once, when one of their kernels is first launched; the others
   stay modules of their own. What the driver made is kept in a file, so that its PTX compiler runs once for
   a given text and not in every process.
   A device global is, for the compiler, a host object that the PTX names (.extern .global) and the module
   record lists with its address. Here the object on the device is another one: a data module of the group
   defines it, it starts with the bytes of the host object, and the host address is only its name for
   cudaMemcpyToSymbol and the like. */
#include "hw.h"
#include <mcuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

typedef unsigned long long u64;

typedef struct {
    char *name;    /* in the PTX of the units */
    char *devname; /* in the linked module: the same unless two objects have one name */
    char *host;
    size_t size;
    unsigned align;
    CUdeviceptr dev[HW_MAXDEV]; /* on each device (each starts with the host object's bytes) */
    int mod; /* a module that names it */
} Glob;

/* a definition or a declaration in a module's text */
typedef struct {
    const char *name;
    int len, kind;       /* IT_* */
    const char *b, *e;   /* its text: a function with its body, or one line */
    const char *pb, *pe; /* a function's declaration ahead of the bodies, if there is one */
} Item;

/* a kernel that was asked for: a module of the driver's for itself, on device 0; on the others in more[device]
   (a module belongs to a context, so each device loads its own, from the same cache file) */
typedef struct Kern {
    CUhandle module, f;
    int tried, err;
    struct Kern *more;
} Kern;

typedef struct {
    char *p;
    size_t len, cap;
} Str;

typedef struct {
    struct __mcuda_module *m;
    const char *body;     /* the text after the declarations */
    char **ext;           /* the functions it declares and does not define, without their "__mcuda_" */
    int *extdef, next;    /* and the module that defines each, -1: the library */
    int *glob, nglob;     /* its device globals */
    char **globname;      /* by the names it uses */
    int indexed;          /* index_mod was here: */
    Item *items;
    int nitems;
    int *htab;            /* names to items and device globals: lookup */
    unsigned nhtab;
    Str common;           /* the declarations that every kernel's text gets */
    Kern *kern;           /* by item */
} Mod;

static Mod *mods;
static int nmods, capmods;
static Glob *globs;
static int nglobs, capglobs;
static struct __mcuda_module **pending; /* registered, not yet looked at */
static int npending, cappending;
static int section_done;

/* every device function that a module defines, to find the one another module calls */
typedef struct {
    const char *name;
    int len, mod;
} Def;
static Def *defs;
static int ndefs, capdefs;
static Def *kdefs; /* and every kernel */
static int nkdefs, capkdefs;

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "mcuda: out of memory\n");
        abort();
    }
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *d = (char *)xrealloc(0, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int isid(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

void hw_prof_start(void); /* prof.c */

void __mcuda_register(struct __mcuda_module *m)
{
    int i, known;
    if (!m)
        return;
    hw_prof_start();
    hw_lock();
    for (i = 0; i < npending && pending[i] != m; i++)
        ;
    known = i < npending;
    for (i = 0; i < nmods; i++)
        known |= mods[i].m == m;
    if (!known) {
        if (npending == cappending) {
            cappending = cappending ? 2 * cappending : 64;
            pending = (struct __mcuda_module **)xrealloc(pending, (size_t)cappending * sizeof *pending);
        }
        pending[npending++] = m;
    }
    hw_unlock();
}

extern struct __mcuda_module *__start_mcuda_modules[] __attribute__((weak));
extern struct __mcuda_module *__stop_mcuda_modules[] __attribute__((weak));

/* ---- the size of a host object from the program's symbol table, for PTX that does not say it ---- */

static struct {
    u64 value, size;
} *esyms;
static long nesyms = -1;

static u64 rd(const unsigned char *p, int n)
{
    u64 v = 0;
    while (n--)
        v = v << 8 | p[n];
    return v;
}

static void elf_read(void)
{
    FILE *f = fopen("/proc/self/exe", "rb");
    unsigned char eh[64], *sh = 0, *st = 0, *str = 0;
    u64 shoff, symoff = 0, symsize = 0, stroff = 0, strsize = 0, base = 0, k;
    unsigned shnum, i;
    nesyms = 0;
    if (!f)
        return;
    if (fread(eh, 1, 64, f) != 64 || memcmp(eh, "\177ELF\2\1", 6))
        goto out;
    shoff = rd(eh + 0x28, 8);
    shnum = (unsigned)rd(eh + 0x3c, 2);
    sh = (unsigned char *)xrealloc(0, (size_t)shnum * 64);
    if (fseek(f, (long)shoff, SEEK_SET) || fread(sh, 64, shnum, f) != shnum)
        goto out;
    for (i = 0; i < shnum; i++)
        if (rd(sh + i * 64 + 4, 4) == 2) { /* SHT_SYMTAB */
            unsigned link = (unsigned)rd(sh + i * 64 + 0x28, 4);
            symoff = rd(sh + i * 64 + 0x18, 8);
            symsize = rd(sh + i * 64 + 0x20, 8);
            if (link < shnum) {
                stroff = rd(sh + link * 64 + 0x18, 8);
                strsize = rd(sh + link * 64 + 0x20, 8);
            }
        }
    if (!symsize || !strsize)
        goto out;
    st = (unsigned char *)xrealloc(0, (size_t)symsize);
    str = (unsigned char *)xrealloc(0, (size_t)strsize + 1);
    if (fseek(f, (long)symoff, SEEK_SET) || fread(st, 1, (size_t)symsize, f) != symsize)
        goto out;
    if (fseek(f, (long)stroff, SEEK_SET) || fread(str, 1, (size_t)strsize, f) != strsize)
        goto out;
    str[strsize] = 0;
    for (k = 0; k + 24 <= symsize; k += 24) {
        u64 nm = rd(st + k, 4);
        if (nm < strsize && strcmp((char *)str + nm, "__mcuda_register") == 0)
            base = (u64)(size_t)(void *)__mcuda_register - rd(st + k + 8, 8);
    }
    esyms = xrealloc(0, (size_t)(symsize / 24 + 1) * sizeof *esyms);
    for (k = 0; k + 24 <= symsize; k += 24)
        if ((st[k + 4] & 15) == 1 && rd(st + k + 16, 8)) { /* STT_OBJECT with a size */
            esyms[nesyms].value = rd(st + k + 8, 8) + base;
            esyms[nesyms++].size = rd(st + k + 16, 8);
        }
out:
    free(sh);
    free(st);
    free(str);
    fclose(f);
}

static size_t elf_size(const char *host)
{
    long i;
    if (nesyms < 0)
        elf_read();
    for (i = 0; i < nesyms; i++)
        if (esyms[i].value == (u64)(size_t)host)
            return (size_t)esyms[i].size;
    return 0;
}

/* ---- reading a module's declarations ---- */

/* The name that a line declaring a function gives it: p is after ".func" or ".entry". */
static const char *func_name(const char *p, int *len)
{
    const char *q;
    while (*p == ' ')
        p++;
    if (*p == '(') {
        while (*p && *p != ')' && *p != '\n')
            p++;
        if (*p == ')')
            p++;
        while (*p == ' ')
            p++;
    }
    for (q = p; isid((unsigned char)*q); q++)
        ;
    *len = (int)(q - p);
    return p;
}

static int starts(const char *p, const char *s) { return strncmp(p, s, strlen(s)) == 0; }

static void *host_of(struct __mcuda_module *m, const char *name, int len)
{
    const struct __mcuda_sym *y;
    for (y = m->syms; y && y->name; y++)
        if ((int)strlen(y->name) == len && memcmp(y->name, name, (size_t)len) == 0)
            return y->addr;
    return 0;
}

static int glob_get(int mod, const char *name, int len, char *host, size_t size, unsigned align)
{
    int i, clash = 0;
    for (i = 0; i < nglobs; i++) {
        if (globs[i].host == host) {
            if (size > globs[i].size)
                globs[i].size = size;
            if (align > globs[i].align)
                globs[i].align = align;
            return i;
        }
        if ((int)strlen(globs[i].name) == len && memcmp(globs[i].name, name, (size_t)len) == 0)
            clash = 1;
    }
    if (nglobs == capglobs) {
        capglobs = capglobs ? 2 * capglobs : 64;
        globs = (Glob *)xrealloc(globs, (size_t)capglobs * sizeof *globs);
    }
    memset(&globs[nglobs], 0, sizeof *globs);
    globs[nglobs].name = xstrndup(name, (size_t)len);
    if (clash) {
        globs[nglobs].devname = (char *)xrealloc(0, (size_t)len + 32);
        sprintf(globs[nglobs].devname, "%s__g%d", globs[nglobs].name, nglobs);
    } else {
        globs[nglobs].devname = globs[nglobs].name;
    }
    globs[nglobs].host = host;
    globs[nglobs].size = size;
    globs[nglobs].align = align;
    globs[nglobs].mod = mod;
    return nglobs++;
}

/* The declarations of module i: all its lines up to the first function body. */
static void scan(int i)
{
    Mod *M = &mods[i];
    const char *p = M->m->ptx, *e, *q;
    int len, capext = 0, capglob = 0, k;
    if (!p)
        p = "";
    for (; *p; p = *e ? e + 1 : e) {
        const char *l = p;
        int ext = 0;
        e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        if (*l != '.')
            continue;
        for (;;) {
            if (starts(l, ".extern ")) {
                ext = 1;
                l += 8;
            } else if (starts(l, ".visible "))
                l += 9;
            else if (starts(l, ".weak "))
                l += 6;
            else
                break;
        }
        if (starts(l, ".func") || starts(l, ".entry")) {
            int entry = l[1] == 'e';
            if (e == p || e[-1] != ';')
                break; /* a body: the declarations are over */
            q = func_name(l + (entry ? 6 : 5), &len);
            if (ext) {
                if (len > 8 && memcmp(q, "__mcuda_", 8) == 0) {
                    if (M->next == capext) {
                        capext = capext ? 2 * capext : 16;
                        M->ext = (char **)xrealloc(M->ext, (size_t)capext * sizeof *M->ext);
                    }
                    M->ext[M->next++] = xstrndup(q + 8, (size_t)len - 8);
                }
            } else if (!entry) {
                if (ndefs == capdefs) {
                    capdefs = capdefs ? 2 * capdefs : 1024;
                    defs = (Def *)xrealloc(defs, (size_t)capdefs * sizeof *defs);
                }
                defs[ndefs].name = q;
                defs[ndefs].len = len;
                defs[ndefs++].mod = i;
            } else {
                /* a kernel: another unit may launch it by name */
                if (nkdefs == capkdefs) {
                    capkdefs = capkdefs ? 2 * capkdefs : 1024;
                    kdefs = (Def *)xrealloc(kdefs, (size_t)capkdefs * sizeof *kdefs);
                }
                kdefs[nkdefs].name = q;
                kdefs[nkdefs].len = len;
                kdefs[nkdefs++].mod = i;
            }
        } else if (ext && starts(l, ".global")) {
            /* .extern .global .align A .b8 name[size]; */
            unsigned align = 0;
            size_t size = 0;
            char *host;
            q = strstr(l, ".align ");
            if (q && q < e)
                align = (unsigned)strtoul(q + 7, 0, 10);
            for (q = e; q > l && *q != '['; q--)
                ;
            if (*q == '[')
                size = (size_t)strtoul(q + 1, 0, 10);
            for (l = q; l > p && isid((unsigned char)l[-1]); l--)
                ;
            len = (int)(q - l);
            host = (char *)host_of(M->m, l, len);
            if (!host) {
                fprintf(stderr, "mcuda: %s: device global %.*s has no host object\n", M->m->name ? M->m->name : "?", len, l);
                continue;
            }
            if (!size)
                size = elf_size(host);
            if (!size) {
                fprintf(stderr, "mcuda: %s: the size of device global %.*s is not known\n", M->m->name ? M->m->name : "?", len, l);
                continue;
            }
            if (align < 16)
                align = 16;
            if (M->nglob == capglob) {
                capglob = capglob ? 2 * capglob : 8;
                M->glob = (int *)xrealloc(M->glob, (size_t)capglob * sizeof *M->glob);
                M->globname = (char **)xrealloc(M->globname, (size_t)capglob * sizeof *M->globname);
            }
            M->globname[M->nglob] = xstrndup(l, (size_t)len);
            M->glob[M->nglob++] = glob_get(i, l, len, host, size, align);
        }
    }
    M->body = p;
}

/* Takes in the modules that were announced since the last time. Called with the lock held. */
static void take(void)
{
    int first = nmods, i, k, d;
    if (!section_done) {
        struct __mcuda_module **p = __start_mcuda_modules;
        section_done = 1;
        hw_unlock();
        for (; p && p < __stop_mcuda_modules; p++)
            __mcuda_register(*p);
        hw_lock();
    }
    if (!npending)
        return;
    for (i = 0; i < npending; i++) {
        if (nmods == capmods) {
            capmods = capmods ? 2 * capmods : 64;
            mods = (Mod *)xrealloc(mods, (size_t)capmods * sizeof *mods);
        }
        memset(&mods[nmods], 0, sizeof *mods);
        mods[nmods].m = pending[i];
        pending[i]->handle = 0;
        nmods++;
    }
    npending = 0;
    for (i = first; i < nmods; i++)
        scan(i);
    for (i = first; i < nmods; i++) {
        Mod *M = &mods[i];
        M->extdef = (int *)xrealloc(0, (size_t)(M->next + 1) * sizeof *M->extdef);
        for (k = 0; k < M->next; k++) {
            int len = (int)strlen(M->ext[k]);
            M->extdef[k] = -1;
            for (d = 0; d < ndefs; d++)
                if (defs[d].len == len && defs[d].mod != i && memcmp(defs[d].name, M->ext[k], (size_t)len) == 0) {
                    M->extdef[k] = defs[d].mod;
                    break;
                }
        }
    }
    for (i = 0; i < nmods; i++)
        mods[i].m->handle = &mods[i]; /* (the table may have moved) */
}

/* ---- the text of a module as the linker gets it ---- */

static void put(Str *s, const char *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        s->cap = 2 * s->cap + n + 256;
        s->p = (char *)xrealloc(s->p, s->cap);
    }
    memcpy(s->p + s->len, p, n);
    s->len += n;
    s->p[s->len] = 0;
}

static void puts_(Str *s, const char *p) { put(s, p, strlen(p)); }

/* The text t with other names for what other modules define: from[k] becomes to[k], and the functions
   vis[] are made visible to the other modules. 0 if nothing changes. */
static char *rewrite(const char *t, char **from, char **to, int n, char **vis, int nvis)
{
    const char *p, *q;
    Str s;
    int k, len;
    if (!n && !nvis)
        return 0;
    memset(&s, 0, sizeof s);
    for (p = t; *p;) {
        if ((p == t || p[-1] == '\n') && nvis && starts(p, ".func")) {
            q = func_name(p + 5, &len);
            for (k = 0; k < nvis; k++)
                if ((int)strlen(vis[k]) == len && memcmp(vis[k], q, (size_t)len) == 0) {
                    puts_(&s, ".visible ");
                    break;
                }
        }
        if (isid((unsigned char)*p) && (p == t || (!isid((unsigned char)p[-1]) && p[-1] != '%'))) {
            for (q = p; isid((unsigned char)*q); q++)
                ;
            len = (int)(q - p);
            for (k = 0; k < n; k++)
                if (from[k][0] == *p && (int)strlen(from[k]) == len && memcmp(from[k], p, (size_t)len) == 0)
                    break;
            if (k < n)
                puts_(&s, to[k]);
            else
                put(&s, p, (size_t)len);
            p = q;
        } else {
            for (q = p + 1; *q && !isid((unsigned char)*q) && *q != '\n'; q++)
                ;
            if (*q == '\n')
                q++;
            put(&s, p, (size_t)(q - p));
            p = q;
        }
    }
    return s.p;
}

/* ---- the cache of linked modules ---- */

static u64 fnv(u64 h, const char *p, size_t n)
{
    while (n--)
        h = (h ^ (unsigned char)*p++) * 1099511628211ULL;
    return h;
}

static int cache_path(char *out, size_t cap, u64 h1, u64 h2)
{
    const char *d = getenv("MCUDA_CACHE"), *home = getenv("HOME");
    char dir[900];
    if (d && !d[0])
        return 0; /* MCUDA_CACHE= turns it off */
    if (d) {
        if (strlen(d) > 800)
            return 0;
        strcpy(dir, d);
    } else {
        if (!home || strlen(home) > 800)
            return 0;
        sprintf(dir, "%s/.cache", home);
        mkdir(dir, 0777);
        strcat(dir, "/mcudahw");
    }
    mkdir(dir, 0777);
    if (strlen(dir) + 64 > cap)
        return 0;
    sprintf(out, "%s/%016llx%016llx.cubin", dir, h1, h2);
    return 1;
}

static void *cache_read(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *p = 0;
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        p = xrealloc(0, (size_t)n);
        if (fread(p, 1, (size_t)n, f) != (size_t)n) {
            free(p);
            p = 0;
        }
        *size = (size_t)n;
    }
    fclose(f);
    return p;
}

static void cache_write(const char *path, const void *p, size_t n)
{
    char tmp[1200];
    FILE *f;
    sprintf(tmp, "%s.%ld", path, (long)getpid());
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(p, 1, n, f) == n && fclose(f) == 0)
        rename(tmp, path);
    else
        remove(tmp);
}

/* ---- loading a group ---- */

static const struct HwLibPiece *lib_find(const char *name)
{
    int lo = 0, hi = hw_nlib - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(hw_lib[mid].name, name);
        if (c == 0)
            return &hw_lib[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* ---- inlining ----
   A call from compiled device code into the device library costs more than most of the functions do: the
   linker does not inline across modules. So the small ones are put into their callers here, in the text.
   The compiler writes every call as a block,
        {  .param .T param0;  st.param.T [param0+0], ARG;  ...  .param .T retval0;
           call.uni (retval0), NAME, (param0, ...);  ld.param.T DST, [retval0+0];  }
   and a function of the library reads its parameters with ld.param and returns with st.param and ret. A block
   that calls a piece which is a leaf (it uses no other piece, no stack and no thread registers) becomes a
   block holding the piece's body with its registers and labels under names of their own, the parameters and
   the result moved between registers, and each ret a jump to the block's end.
   MCUDA_INLINE=N: pieces of at most N bytes of PTX (default 8000; 0: none). */

static long inline_limit(void)
{
    static long lim = -1;
    if (lim < 0) {
        const char *v = getenv("MCUDA_INLINE");
        lim = v && v[0] ? atol(v) : 8000;
        if (lim < 0)
            lim = 0;
    }
    return lim;
}

static int movable(const char *t, size_t n)
{
    static const char *const ok[] = {"f32", "f64", "b32", "b64", "u32", "s32", "u64", "s64", 0};
    int i;
    for (i = 0; ok[i]; i++)
        if (strlen(ok[i]) == n && memcmp(ok[i], t, n) == 0)
            return 1;
    return 0;
}

/* 1: the piece can be put into its callers */
static int inlinable(const struct HwLibPiece *pc)
{
    const char *p, *e;
    if (pc->deps[0] || !starts(pc->text, ".func") || (long)strlen(pc->text) > inline_limit())
        return 0;
    if (strstr(pc->text, ".local") || strstr(pc->text, "%SP") || strstr(pc->text, "tid.") || strstr(pc->text, "call") ||
        strstr(pc->text, ".param .align"))
        return 0;
    for (p = pc->text; (p = strstr(p, ".param.")) != 0; p += 7) {
        for (e = p + 7; isid((unsigned char)*e); e++)
            ;
        if (!movable(p + 7, (size_t)(e - (p + 7))))
            return 0;
    }
    return 1;
}

/* a line of the piece with every register under the instance's name */
static void put_renamed(Str *o, const char *l, const char *e, const char *tag)
{
    const char *p;
    for (p = l; p < e; p++) {
        put(o, p, 1);
        if (*p == '%')
            puts_(o, tag);
    }
}

#define MAXARG 16
/* the body of the piece for the arguments args[0..nargs-1] and the result register dst (0: not wanted) */
static int put_inline(Str *o, const struct HwLibPiece *pc, char args[][64], int nargs, const char *dst, unsigned long id)
{
    const char *l = strstr(pc->text, "\n{\n"), *e, *q, *t;
    char tag[32], lab[32];
    if (!l)
        return 0;
    sprintf(tag, "i%lu_", id);
    sprintf(lab, "LI%lu_", id);
    puts_(o, "\t{\n");
    for (l += 3; *l && !(l[0] == '}' && l[1] == '\n'); l = e + 1) {
        e = strchr(l, '\n');
        if (!e)
            return 0;
        if (l[0] != '\t') {
            /* a label */
            puts_(o, lab);
            put(o, l, (size_t)(e - l) + 1);
        } else if (starts(l, "\tld.param.")) {
            int k;
            q = strstr(l, "_param_");
            for (t = l + 10; isid((unsigned char)*t); t++)
                ;
            while (q && q < e && strstr(q + 1, "_param_") && strstr(q + 1, "_param_") < e)
                q = strstr(q + 1, "_param_");
            if (!q || q > e)
                return 0;
            k = atoi(q + 7);
            if (k < 0 || k >= nargs)
                return 0;
            puts_(o, "\tmov.");
            put(o, l + 10, (size_t)(t - (l + 10)));
            for (q = t; q < e && *q != ','; q++)
                ;
            put_renamed(o, t, q, tag);
            puts_(o, ", ");
            puts_(o, args[k]);
            puts_(o, ";\n");
        } else if (starts(l, "\tst.param.")) {
            for (t = l + 10; isid((unsigned char)*t); t++)
                ;
            q = strstr(l, "], ");
            if (!q || q > e)
                return 0;
            if (dst) {
                puts_(o, "\tmov.");
                put(o, l + 10, (size_t)(t - (l + 10)));
                puts_(o, " ");
                puts_(o, dst);
                puts_(o, ", ");
                put_renamed(o, q + 3, e + 1, tag);
            }
        } else if (starts(l, "\tret;")) {
            puts_(o, "\tbra.uni ");
            puts_(o, lab);
            puts_(o, "end;\n");
        } else if ((q = strstr(l, "bra")) != 0 && q < e) {
            /* [@[!]%p] bra[.uni] LABEL; */
            for (t = e; t > l && t[-1] != ' '; t--)
                ;
            put_renamed(o, l, t, tag);
            puts_(o, lab);
            put(o, t, (size_t)(e - t) + 1);
        } else
            put_renamed(o, l, e + 1, tag);
    }
    puts_(o, lab);
    puts_(o, "end:\n\t}\n");
    return 1;
}

/* the text with the calls of small library functions replaced by their bodies; 0: there are none */
static char *inline_calls(const char *text)
{
    static unsigned long serial;
    Str o;
    const char *p = text, *l, *e, *blk, *copied = text;
    int did = 0;
    if (!inline_limit() || !strstr(text, "__mcuda_"))
        return 0;
    memset(&o, 0, sizeof o);
    while ((blk = strstr(p, "\n\t{\n")) != 0) {
        char args[MAXARG][64], dst[64], name[128];
        int nargs = 0, have_dst = 0, ok = 1, called = 0, n;
        const struct HwLibPiece *pc;
        blk++;
        for (l = blk + 3; ok && !starts(l, "\t}\n"); l = e + 1) {
            e = strchr(l, '\n');
            if (!e) {
                ok = 0;
                break;
            }
            if (starts(l, "\t.param .") && !starts(l, "\t.param .align"))
                continue;
            if (starts(l, "\tst.param.") && !called) {
                const char *a = strstr(l, "+0], "), *b = strstr(l, "[param");
                if (!a || a > e || !b || b > a || atoi(b + 6) != nargs || nargs == MAXARG || e - (a + 5) - 1 >= 64 || e[-1] != ';') {
                    ok = 0;
                    break;
                }
                memcpy(args[nargs], a + 5, (size_t)(e - (a + 5)) - 1);
                args[nargs++][(e - (a + 5)) - 1] = 0;
            } else if (starts(l, "\tcall.uni ") && !called) {
                const char *a = l + 10, *b;
                if (*a == '(') {
                    a = strstr(a, "), ");
                    if (!a || a > e) {
                        ok = 0;
                        break;
                    }
                    a += 3;
                }
                for (b = a; isid((unsigned char)*b); b++)
                    ;
                if (b - a >= (long)sizeof name || *b != ',') {
                    ok = 0;
                    break;
                }
                memcpy(name, a, (size_t)(b - a));
                name[b - a] = 0;
                called = 1;
            } else if (starts(l, "\tld.param.") && called && !have_dst) {
                const char *a, *b;
                for (a = l + 10; isid((unsigned char)*a); a++)
                    ;
                b = strstr(a, ", [retval0+0];");
                if (*a != ' ' || !b || b > e || b - (a + 1) >= 64) {
                    ok = 0;
                    break;
                }
                memcpy(dst, a + 1, (size_t)(b - (a + 1)));
                dst[b - (a + 1)] = 0;
                have_dst = 1;
            } else
                ok = 0;
        }
        if (!ok || !called || !starts(name, "__mcuda_") || (pc = lib_find(name)) == 0 || !inlinable(pc)) {
            p = blk + 2;
            continue;
        }
        /* (the number of parameters must be the piece's) */
        n = 0;
        for (l = strstr(pc->text, "\n{\n"); l && (l = strstr(l, "\tld.param.")) != 0; l += 10)
            n++;
        if (n != nargs) {
            p = blk + 2;
            continue;
        }
        put(&o, copied, (size_t)(blk - copied));
        e = strstr(blk, "\t}\n") + 3;
        if (!put_inline(&o, pc, args, nargs, have_dst ? dst : 0, ++serial)) {
            free(o.p);
            return 0;
        }
        copied = e;
        p = e - 1;
        did = 1;
    }
    if (!did) {
        free(o.p);
        return 0;
    }
    puts_(&o, copied);
    return o.p;
}

/* ---- printf ----
   The compiler calls __mcuda_printf with the arguments as they are after C's promotions. The driver has
   vprintf(format, arguments), the arguments in memory laid out as the members of a record are. Each call
   block becomes that. 0: the text has none. */
static char *printf_calls(const char *text)
{
    static unsigned long serial;
    static const char decl[] = ".extern .func (.param .b32 func_retval0) __mcuda_printf ";
    Str o;
    const char *p = text, *l, *e, *blk, *copied = text, *d;
    char num[32];
    if (!strstr(text, "__mcuda_printf"))
        return 0;
    memset(&o, 0, sizeof o);
    d = strstr(text, decl);
    if (d && (d == text || d[-1] == '\n') && (e = strchr(d, '\n')) != 0) {
        put(&o, text, (size_t)(d - text));
        puts_(&o, ".extern .func (.param .b32 func_retval0) vprintf (.param .b64 vprintf_param_0, .param .b64 vprintf_param_1);\n");
        copied = p = e + 1;
    }
    while ((blk = strstr(p, "\n\t{\n")) != 0) {
        const char *reg[64], *rege[64], *ty[64], *end, *call = 0, *ret = 0;
        unsigned long off = 0, offs[64];
        int n = 0, k, ok = 1;
        blk++;
        end = strstr(blk, "\n\t}\n");
        if (!end)
            break;
        for (l = blk + 3; l < end + 1; l = e + 1) {
            e = strchr(l, '\n');
            if (starts(l, "\tst.param.") && !call) {
                const char *a = strstr(l, "+0], ");
                unsigned long w;
                if (!a || a > e || n == 64 || e[-1] != ';') {
                    ok = 0;
                    break;
                }
                ty[n] = l + 10;
                w = starts(ty[n], "b64") || starts(ty[n], "f64") || starts(ty[n], "u64") || starts(ty[n], "s64") ? 8 : 4;
                reg[n] = a + 5;
                rege[n] = e - 1;
                if (n) {
                    off = (off + w - 1) & ~(w - 1);
                    offs[n] = off;
                    off += w;
                }
                n++;
            } else if (starts(l, "\tcall.uni ")) {
                const char *a = strstr(l, "__mcuda_printf, (");
                if (a && a < e)
                    call = l;
                else
                    ok = 0;
            } else if (starts(l, "\tld.param.") && call)
                ret = l;
        }
        if (!ok || !call || !n) {
            p = blk;
            continue;
        }
        sprintf(num, "%lu", ++serial);
        put(&o, copied, (size_t)(blk - copied));
        puts_(&o, "\t{\n\t.local .align 8 .b8 __mcuda_pf");
        puts_(&o, num);
        sprintf(num, "[%lu];\n", off ? (off + 7) & ~7ul : 8ul);
        puts_(&o, num);
        sprintf(num, "%lu", serial);
        puts_(&o, "\t.reg .b64 %pf");
        puts_(&o, num);
        puts_(&o, ";\n\tmov.u64 %pf");
        puts_(&o, num);
        puts_(&o, ", __mcuda_pf");
        puts_(&o, num);
        puts_(&o, ";\n\tcvta.local.u64 %pf");
        puts_(&o, num);
        puts_(&o, ", %pf");
        puts_(&o, num);
        puts_(&o, ";\n");
        for (k = 1; k < n; k++) {
            char at[48];
            puts_(&o, "\tst.");
            put(&o, ty[k], 3);
            sprintf(at, " [%%pf%s+%lu], ", num, offs[k]);
            puts_(&o, at);
            put(&o, reg[k], (size_t)(rege[k] - reg[k]));
            puts_(&o, ";\n");
        }
        puts_(&o, "\t.param .b64 param0;\n\tst.param.b64 [param0+0], ");
        put(&o, reg[0], (size_t)(rege[0] - reg[0]));
        puts_(&o, ";\n\t.param .b64 param1;\n\tst.param.b64 [param1+0], %pf");
        puts_(&o, num);
        puts_(&o, ";\n\t.param .b32 retval0;\n\tcall.uni (retval0), vprintf, (param0, param1);\n");
        if (ret)
            put(&o, ret, (size_t)(strchr(ret, '\n') + 1 - ret));
        puts_(&o, "\t}\n");
        copied = end + 4;
        p = end + 3;
    }
    puts_(&o, copied);
    return o.p;
}

/* ---- locals ----
   The compiler keeps a local whose address is taken in the function's memory (__local_depot), and a value
   seen as another type (__float_as_uint) goes through such a place. Memory is slow on the device. Two
   repairs of a function's text:
   - a store followed at once by the load of the same place and size is a move between the two registers;
   - if every address the function makes from its stack pointer is a constant offset used only to load and
     store whole 4 or 8 byte values there, each such place that no other access overlaps becomes a register.
   MCUDA_LOCALS=0: neither. 0: nothing to do. */
typedef struct {
    const char *p, *e; /* the line, without its newline */
    int kind;          /* 1: a load, 2: a store, of w bytes, the other operand x..xe, the address a..ae */
    int w;
    const char *x, *xe, *a, *ae;
    long slot;         /* the place it reads or writes, or -1 */
    int fwd;           /* a load that takes the register of the store before it */
} LLine;

typedef struct {
    long off;
    int w;    /* 4, 8; 0: stays in memory */
    int maxw; /* the widest access */
} LSlot;

static int locals_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("MCUDA_LOCALS");
        on = !(v && v[0] == '0');
    }
    return on;
}

static int isdig(int c) { return c >= '0' && c <= '9'; }
static unsigned long locals_slots, locals_fwds; /* what was done, for MCUDA_VERBOSE */

/* the bytes of a register of the compiler's by its name: %r1 %f1 are 4, %rd1 %fd1 are 8; 0: something else */
static int reg_bytes(const char *r, const char *e)
{
    const char *q;
    if (e - r < 3 || r[0] != '%' || (r[1] != 'r' && r[1] != 'f'))
        return 0;
    q = r + 2;
    if (*q == 'd')
        q++;
    if (q == e)
        return 0;
    for (; q < e; q++)
        if (!isdig(*q))
            return 0;
    return r[2] == 'd' ? 8 : 4;
}

/* the line as a plain load or store: "ld.T X, [A];" or "st.T [A], X;" */
static void laccess(LLine *L)
{
    const char *l = L->p, *e = L->e, *t, *q;
    L->kind = 0;
    L->slot = -1;
    L->fwd = 0;
    if (e - l < 12 || l[0] != '\t' || e[-1] != ';' || l[3] != '.' || !((l[1] == 'l' && l[2] == 'd') || (l[1] == 's' && l[2] == 't')))
        return;
    t = l + 4;
    if (!(t[0] == 'f' || t[0] == 'u' || t[0] == 's' || t[0] == 'b') || t[3] != ' ')
        return;
    if (t[1] == '3' && t[2] == '2')
        L->w = 4;
    else if (t[1] == '6' && t[2] == '4')
        L->w = 8;
    else
        return;
    q = t + 4;
    if (l[1] == 'l') {
        L->x = q;
        while (q < e && *q != ',')
            q++;
        L->xe = q;
        if (e - q < 6 || q[1] != ' ' || q[2] != '[' || e[-2] != ']')
            return;
        L->a = q + 3;
        L->ae = e - 2;
        L->kind = 1;
    } else {
        if (*q != '[')
            return;
        L->a = ++q;
        while (q < e && *q != ']')
            q++;
        L->ae = q;
        if (e - q < 5 || q[1] != ',' || q[2] != ' ')
            return;
        L->x = q + 3;
        L->xe = e - 1;
        L->kind = 2;
    }
    for (q = L->a; q < L->ae; q++)
        if (*q == '+' || *q == ' ' || *q == ',')
            L->kind = 0;
    for (q = L->x; q < L->xe; q++)
        if (*q == ' ' || *q == ',' || *q == '[')
            L->kind = 0;
}

/* a constant operand of a store as a move takes it: 0f and 0d are floats by their bits */
static int lconst(const char *x, const char *xe, int w, char *out)
{
    const char *q = x;
    size_t n = (size_t)(xe - x);
    if (n >= 60 || !n)
        return 0;
    if (n == (size_t)(2 + 2 * w) && x[0] == '0' && (x[1] == (w == 4 ? 'f' : 'd') || x[1] == (w == 4 ? 'F' : 'D'))) {
        memcpy(out, x, n);
        out[1] = 'x';
        out[n] = 0;
        return 1;
    }
    if (*q == '-')
        q++;
    if (q == xe)
        return 0;
    for (; q < xe; q++)
        if (!isdig(*q))
            return 0;
    memcpy(out, x, n);
    out[n] = 0;
    return 1;
}

/* one function: its lines b..e (after the "{" line, before the "}" line) written to o; 1 if changed */
static int locals_fn(Str *o, const char *b, const char *e)
{
    LLine *L = 0;
    LSlot *S = 0;
    long *regslot = 0; /* by the number of an %rd register: its place + 1; 0: not an address of the stack */
    long nl = 0, capl = 0, ns = 0, caps = 0, nreg = 0, i, k;
    int escape = 0, depot = 0, changed = 0;
    const char *p, *q;
    char buf[160];
    for (p = b; p < e; p = q + 1) {
        q = memchr(p, '\n', (size_t)(e - p));
        if (!q)
            q = e;
        if (nl == capl) {
            capl = capl ? 2 * capl : 256;
            L = (LLine *)xrealloc(L, (size_t)capl * sizeof *L);
        }
        L[nl].p = p;
        L[nl].e = q;
        laccess(&L[nl]);
        nl++;
    }
    /* the addresses made from the stack pointer */
    for (i = 0; i < nl; i++) {
        const char *l = L[i].p, *le = L[i].e;
        long n, off;
        if (starts(l, "\t.local ") && le - l > 21 && strstr(l, "__local_depot") && strstr(l, "__local_depot") < le)
            depot = 1;
        if (!(starts(l, "\tadd.u64 %rd") || starts(l, "\tadd.s64 %rd")))
            continue;
        p = l + 12;
        if (!isdig(*p))
            continue;
        n = strtol(p, (char **)&p, 10);
        if (!starts(p, ", %SP, "))
            continue;
        p += 7;
        if (!isdig(*p)) {
            escape = 1;
            continue;
        }
        off = strtol(p, (char **)&p, 10);
        if (p != le - 1 || *p != ';' || n < 0 || n > 1000000) {
            escape = 1;
            continue;
        }
        if (n >= nreg) {
            regslot = (long *)xrealloc(regslot, (size_t)(n + 64) * sizeof *regslot);
            memset(regslot + nreg, 0, (size_t)(n + 64 - nreg) * sizeof *regslot);
            nreg = n + 64;
        }
        if (regslot[n])
            escape = 1;
        for (k = 0; k < ns && S[k].off != off; k++)
            ;
        if (k == ns) {
            if (ns == caps) {
                caps = caps ? 2 * caps : 64;
                S = (LSlot *)xrealloc(S, (size_t)caps * sizeof *S);
            }
            S[ns].off = off;
            S[ns].maxw = 1;
            S[ns++].w = -1;
        }
        regslot[n] = k + 1;
        L[i].kind = 3; /* the definition */
    }
    /* every other use of the stack pointer or of such an address */
    for (i = 0; depot && !escape && i < nl; i++) {
        const char *l = L[i].p, *le = L[i].e;
        if (L[i].kind == 3)
            continue;
        for (p = l; p < le && (p = memchr(p, '%', (size_t)(le - p))) != 0; p++) {
            if (p[1] == 'S' && p[2] == 'P') {
                if (!(starts(l, "\tcvta.local.u64 %SP, %SPL;") || starts(l, "\tmov.u64 %SPL, __local_depot;") || starts(l, "\t.reg .b64 %SP")))
                    escape = 1;
            } else if (p[1] == 'r' && p[2] == 'd' && isdig(p[3])) {
                long n = strtol(p + 3, (char **)&q, 10);
                if (n < nreg && regslot[n]) {
                    LSlot *s = &S[regslot[n] - 1];
                    int ok = L[i].kind && p == L[i].a && q == L[i].ae;
                    if (!ok) {
                        escape = 1;
                        break;
                    }
                    L[i].slot = regslot[n] - 1;
                    if (L[i].w > s->maxw)
                        s->maxw = L[i].w;
                    if (reg_bytes(L[i].x, L[i].xe) != L[i].w && !(L[i].kind == 2 && lconst(L[i].x, L[i].xe, L[i].w, buf)))
                        s->w = 0;
                    else if (s->w < 0)
                        s->w = L[i].w;
                    else if (s->w != L[i].w)
                        s->w = 0;
                }
            }
        }
    }
    /* (a line that is not a plain 4 or 8 byte access may still use such an address: other sizes) */
    for (i = 0; depot && !escape && i < nl; i++) {
        const char *l = L[i].p, *le = L[i].e;
        if (L[i].kind)
            continue;
        for (p = l; p < le && (p = memchr(p, '%', (size_t)(le - p))) != 0; p++)
            if (p[1] == 'r' && p[2] == 'd' && isdig(p[3])) {
                long n = strtol(p + 3, (char **)&q, 10);
                if (n < nreg && regslot[n])
                    escape = 1;
            }
    }
    if (!depot || escape)
        for (i = 0; i < nl; i++)
            L[i].slot = -1;
    else {
        /* places that overlap stay */
        for (i = 0; i < ns; i++)
            for (k = 0; k < ns; k++)
                if (k != i && S[k].off > S[i].off && S[k].off < S[i].off + S[i].maxw) {
                    S[i].w = S[i].w > 0 ? -2 : S[i].w;
                    S[k].w = S[k].w > 0 ? -2 : S[k].w;
                }
        for (i = 0; i < nl; i++)
            if (L[i].slot >= 0 && S[L[i].slot].w <= 0)
                L[i].slot = -1;
    }
    /* a store and the load of it (with only line numbers, .loc, between them: code made with -g is the same) */
    for (i = 0; i + 1 < nl; i++) {
        long j = i + 1;
        while (j < nl && starts(L[j].p, "\t.loc "))
            j++;
        if (j < nl && L[i].kind == 2 && L[j].kind == 1 && L[i].slot < 0 && L[j].slot < 0 && L[i].w == L[j].w &&
            L[i].ae - L[i].a == L[j].ae - L[j].a && !memcmp(L[i].a, L[j].a, (size_t)(L[i].ae - L[i].a)) &&
            reg_bytes(L[i].x, L[i].xe) == L[i].w && reg_bytes(L[j].x, L[j].xe) == L[i].w)
            L[j].fwd = 1;
    }
    for (i = 0; i < nl && !changed; i++)
        changed = L[i].slot >= 0 || L[i].fwd;
    if (changed) {
        for (k = 0; k < ns; k++)
            if (!escape && depot && S[k].w > 0) {
                sprintf(buf, "\t.reg .b%d %%mL%ld;\n", 8 * S[k].w, k);
                puts_(o, buf);
                locals_slots++;
            }
        for (i = 0; i < nl; i++) {
            LLine *l = &L[i];
            if (l->slot >= 0) {
                sprintf(buf, "\tmov.b%d ", 8 * l->w);
                puts_(o, buf);
                if (l->kind == 1) {
                    put(o, l->x, (size_t)(l->xe - l->x));
                    sprintf(buf, ", %%mL%ld;\n", l->slot);
                    puts_(o, buf);
                } else {
                    char c[64];
                    sprintf(buf, "%%mL%ld, ", l->slot);
                    puts_(o, buf);
                    if (reg_bytes(l->x, l->xe) == l->w)
                        put(o, l->x, (size_t)(l->xe - l->x));
                    else {
                        lconst(l->x, l->xe, l->w, c);
                        puts_(o, c);
                    }
                    puts_(o, ";\n");
                }
            } else if (l->fwd) {
                locals_fwds++;
                sprintf(buf, "\tmov.b%d ", 8 * l->w);
                puts_(o, buf);
                put(o, l->x, (size_t)(l->xe - l->x));
                puts_(o, ", ");
                put(o, l[-1].x, (size_t)(l[-1].xe - l[-1].x));
                puts_(o, ";\n");
            } else {
                put(o, l->p, (size_t)(l->e - l->p));
                puts_(o, "\n");
            }
        }
    }
    free(L);
    free(S);
    free(regslot);
    return changed;
}

static char *locals(const char *text)
{
    Str o;
    const char *p = text, *b, *e, *copied = text;
    int did = 0;
    if (!locals_on())
        return 0;
    memset(&o, 0, sizeof o);
    while ((b = strstr(p, "\n{\n")) != 0 && (e = strstr(b, "\n}\n")) != 0) {
        b += 3;
        e += 1;
        put(&o, copied, (size_t)(b - copied));
        if (locals_fn(&o, b, e)) {
            did = 1;
            copied = e;
        } else
            copied = b;
        p = e;
    }
    if (!did) {
        free(o.p);
        return 0;
    }
    puts_(&o, copied);
    return o.p;
}

static int verbose(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MCUDA_VERBOSE");
        v = e && e[0] && e[0] != '0';
    }
    return v;
}

/* The header of a module's text: its lines up to .address_size. */
static void header(Str *s, const char *t)
{
    const char *q = strstr(t, ".address_size");
    if (q && (q = strchr(q, '\n')) != 0)
        put(s, t, (size_t)(q + 1 - t));
    else
        puts_(s, ".version 8.5\n.target sm_86\n.address_size 64\n");
}

static double now(void);

/* ---- loading a kernel ----
   A kernel is loaded when it is first launched (or asked about), by itself: the text the driver gets is the
   kernel, the functions it reaches in its unit and in other units, the shared variables they name and the
   pieces of the device library they call, one text per unit so that each unit's own names stay its own. A
   unit may hold thousands of kernels and megabytes of text for each, of which a program runs a few.
   A device global is one block of device memory for all kernels, made when the first of them is loaded and
   filled from the host object then. Kernels reach it through a table of addresses that each kernel's module
   has for itself (__mcuda_gt, filled when the module is loaded): where the compiler wrote
        mov.u64 %r, NAME;       the text gets       ld.global.u64 %r, [__mcuda_gt+8 k]; */

enum { IT_FUNC, IT_ENTRY, IT_DATA, IT_EXT, IT_PROTO };

static unsigned name_hash(const char *p, int len)
{
    unsigned h = 2166136261U;
    while (len-- > 0)
        h = (h ^ (unsigned char)*p++) * 16777619U;
    return h;
}

/* what the name means in module M: item + 1, -(its device global + 1), 0: nothing */
static int lookup(const Mod *M, const char *p, int len)
{
    unsigned h;
    if (!M->nhtab)
        return 0;
    for (h = name_hash(p, len) & (M->nhtab - 1); M->htab[h]; h = (h + 1) & (M->nhtab - 1)) {
        int v = M->htab[h];
        if (v > 0 ? M->items[v - 1].len == len && memcmp(M->items[v - 1].name, p, (size_t)len) == 0
                  : (int)strlen(M->globname[-v - 1]) == len && memcmp(M->globname[-v - 1], p, (size_t)len) == 0)
            return v;
    }
    return 0;
}

static void hash_put(Mod *M, const char *p, int len, int v)
{
    unsigned h;
    if (lookup(M, p, len))
        return;
    for (h = name_hash(p, len) & (M->nhtab - 1); M->htab[h]; h = (h + 1) & (M->nhtab - 1))
        ;
    M->htab[h] = v;
}

static void item_add(Mod *M, int *cap, int kind, const char *name, int len, const char *b, const char *e)
{
    Item *it;
    if (M->nitems == *cap) {
        *cap = *cap ? 2 * *cap : 64;
        M->items = (Item *)xrealloc(M->items, (size_t)*cap * sizeof *M->items);
    }
    it = &M->items[M->nitems++];
    it->name = name;
    it->len = len;
    it->kind = kind;
    it->b = b;
    it->e = e;
    it->pb = it->pe = 0;
}

/* The parts of module i's text: what it defines and declares, by name. */
static void index_mod(int i)
{
    Mod *M = &mods[i];
    const char *t = M->m->ptx ? M->m->ptx : "", *p, *e, *q;
    int cap = 0, k, len;
    if (M->indexed)
        return;
    M->indexed = 1;
    for (p = t; *p; p = e) {
        const char *l = p;
        int ext = 0;
        e = strchr(p, '\n');
        e = e ? e + 1 : p + strlen(p);
        if (*l != '.') {
            if (p < M->body)
                put(&M->common, p, (size_t)(e - p));
            continue;
        }
        for (;;) {
            if (starts(l, ".extern ")) {
                ext = 1;
                l += 8;
            } else if (starts(l, ".visible "))
                l += 9;
            else if (starts(l, ".weak "))
                l += 6;
            else
                break;
        }
        if (starts(l, ".func") || starts(l, ".entry")) {
            int entry = l[1] == 'e';
            q = func_name(l + (entry ? 6 : 5), &len);
            if (p < M->body)
                item_add(M, &cap, ext ? IT_EXT : IT_PROTO, q, len, p, e);
            else {
                const char *x = strstr(p, "\n}\n");
                e = x ? x + 3 : p + strlen(p);
                item_add(M, &cap, entry ? IT_ENTRY : IT_FUNC, q, len, p, e);
            }
        } else if (starts(l, ".global") || starts(l, ".shared") || starts(l, ".const")) {
            if (ext && l[1] == 'g')
                continue; /* a device global: through the table */
            for (q = l; q < e && *q != '[' && *q != ';' && *q != '=' && *q != '\n'; q++)
                ;
            while (q > l && q[-1] == ' ')
                q--;
            for (l = q; l > p && isid((unsigned char)l[-1]); l--)
                ;
            item_add(M, &cap, IT_DATA, l, (int)(q - l), p, e);
        } else if (p < M->body)
            put(&M->common, p, (size_t)(e - p));
    }
    for (M->nhtab = 64; M->nhtab < 2U * (unsigned)(M->nitems + M->nglob) + 2; M->nhtab *= 2)
        ;
    M->htab = (int *)xrealloc(0, M->nhtab * sizeof *M->htab);
    memset(M->htab, 0, M->nhtab * sizeof *M->htab);
    for (k = 0; k < M->nitems; k++)
        if (M->items[k].kind != IT_PROTO)
            hash_put(M, M->items[k].name, M->items[k].len, k + 1);
    for (k = 0; k < M->nglob; k++)
        hash_put(M, M->globname[k], (int)strlen(M->globname[k]), -(k + 1));
    for (k = 0; k < M->nitems; k++)
        if (M->items[k].kind == IT_PROTO) {
            int v = lookup(M, M->items[k].name, M->items[k].len);
            if (v > 0 && M->items[v - 1].kind == IT_FUNC) {
                M->items[v - 1].pb = M->items[k].b;
                M->items[v - 1].pe = M->items[k].e;
            }
        }
    M->kern = (Kern *)xrealloc(0, (size_t)(M->nitems + 1) * sizeof *M->kern);
    memset(M->kern, 0, (size_t)(M->nitems + 1) * sizeof *M->kern);
}

/* what one kernel takes of a module */
typedef struct {
    int mod;
    unsigned char *sel; /* by item */
    Str body;           /* its functions, as they were met */
    char **vis;         /* those that another module calls */
    int nvis;
} Part;

typedef struct {
    Part *part;
    int npart;
    int *stack; /* (part, item) to be read */
    int nstack, capstack;
    int *kg; /* the device globals, in the order of the table */
    int nkg;
    char **need; /* of the library */
    int nneed, capneed;
    int bad;
} Build;

static int part_of(Build *B, int mod)
{
    int i;
    for (i = 0; i < B->npart; i++)
        if (B->part[i].mod == mod)
            return i;
    index_mod(mod);
    B->part = (Part *)xrealloc(B->part, (size_t)(B->npart + 1) * sizeof *B->part);
    memset(&B->part[B->npart], 0, sizeof *B->part);
    B->part[B->npart].mod = mod;
    B->part[B->npart].sel = (unsigned char *)xrealloc(0, (size_t)mods[mod].nitems + 1);
    memset(B->part[B->npart].sel, 0, (size_t)mods[mod].nitems + 1);
    return B->npart++;
}

static void want(Build *B, int pi, int item)
{
    int kind = mods[B->part[pi].mod].items[item].kind;
    if (B->part[pi].sel[item])
        return;
    B->part[pi].sel[item] = 1;
    if (kind != IT_FUNC && kind != IT_ENTRY)
        return;
    if (B->nstack + 2 > B->capstack) {
        B->capstack = B->capstack ? 2 * B->capstack : 64;
        B->stack = (int *)xrealloc(B->stack, (size_t)B->capstack * sizeof *B->stack);
    }
    B->stack[B->nstack++] = pi;
    B->stack[B->nstack++] = item;
}

/* a function that module mod declares outside itself: another module's, or the library's */
static void want_ext(Build *B, int mod, const Item *x)
{
    Mod *M = &mods[mod];
    int k, j, len = x->len - 8;
    if (len <= 0 || memcmp(x->name, "__mcuda_", 8))
        return;
    for (k = 0; k < M->next; k++)
        if ((int)strlen(M->ext[k]) == len && memcmp(M->ext[k], x->name + 8, (size_t)len) == 0)
            break;
    if (k == M->next)
        return;
    if (M->extdef[k] >= 0) {
        int pi = part_of(B, M->extdef[k]), v = lookup(&mods[M->extdef[k]], M->ext[k], len);
        Part *P = &B->part[pi];
        if (v > 0 && mods[M->extdef[k]].items[v - 1].kind == IT_FUNC) {
            want(B, pi, v - 1);
            P = &B->part[pi];
            for (j = 0; j < P->nvis && strcmp(P->vis[j], M->ext[k]); j++)
                ;
            if (j == P->nvis) {
                P->vis = (char **)xrealloc(P->vis, (size_t)(P->nvis + 1) * sizeof *P->vis);
                P->vis[P->nvis++] = M->ext[k];
            }
        }
        return;
    }
    if (len == 6 && memcmp(x->name + 8, "printf", 6) == 0)
        return; /* the driver's vprintf: printf_calls */
    for (j = 0; j < B->nneed && ((int)strlen(B->need[j]) != x->len || memcmp(B->need[j], x->name, (size_t)x->len)); j++)
        ;
    if (j == B->nneed) {
        if (B->nneed == B->capneed) {
            B->capneed = B->capneed ? 2 * B->capneed : 32;
            B->need = (char **)xrealloc(B->need, (size_t)B->capneed * sizeof *B->need);
        }
        B->need[B->nneed++] = xstrndup(x->name, (size_t)x->len);
    }
}

/* One function of a part: its text goes to the part's, and what it names is wanted. */
static void take_fn(Build *B, int pi, int item)
{
    int mod = B->part[pi].mod;
    Mod *M = &mods[mod];
    const Item *it = &M->items[item];
    const char *e = it->e, *copied = it->b, *l, *le, *q, *t;
    for (l = it->b; l < e; l = le) {
        const char *g = 0;
        int gk = -1, glen = 0;
        le = (const char *)memchr(l, '\n', (size_t)(e - l));
        le = le ? le + 1 : e;
        for (q = l; q < le;) {
            int c = (unsigned char)*q;
            if (!isid(c)) {
                q++;
                continue;
            }
            for (t = q + 1; t < le && isid((unsigned char)*t); t++)
                ;
            if (!(c >= '0' && c <= '9') && (q == l || (q[-1] != '%' && q[-1] != '.')) && t - q > 1) {
                int v = lookup(M, q, (int)(t - q));
                if (v > 0) {
                    want(B, pi, v - 1);
                    if (M->items[v - 1].kind == IT_EXT)
                        want_ext(B, mod, &M->items[v - 1]);
                } else if (v < 0) {
                    g = q;
                    gk = -v - 1;
                    glen = (int)(t - q);
                }
            }
            q = t;
        }
        if (g) {
            if (starts(l, "\tmov.u64 %") && g[glen] == ';' && g > l + 11 && g[-1] == ' ' && g[-2] == ',') {
                char at[64];
                int k;
                for (k = 0; k < B->nkg && B->kg[k] != M->glob[gk]; k++)
                    ;
                if (k == B->nkg) {
                    B->kg = (int *)xrealloc(B->kg, (size_t)(B->nkg + 1) * sizeof *B->kg);
                    B->kg[B->nkg++] = M->glob[gk];
                }
                put(&B->part[pi].body, copied, (size_t)(l - copied));
                puts_(&B->part[pi].body, "\tld.global.u64 ");
                put(&B->part[pi].body, l + 9, (size_t)(g - 2 - (l + 9)));
                sprintf(at, ", [__mcuda_gt+%d];\n", 8 * k);
                puts_(&B->part[pi].body, at);
                copied = le;
            } else {
                fprintf(stderr, "mcuda: %s: a use of device global %s that the run time does not know: %.*s", M->m->name ? M->m->name : "?",
                        M->globname[gk], (int)(le - l), l);
                B->bad = 1;
            }
        }
    }
    put(&B->part[pi].body, copied, (size_t)(e - copied));
}

/* the block of device memory of device global j on the thread's device */
static int glob_dev(int j)
{
    int r, d = hw_cur();
    if (globs[j].dev[d])
        return 0;
    r = hw.MemAlloc(&globs[j].dev[d], globs[j].size ? globs[j].size : 1);
    if (!r)
        r = hw.MemcpyHtoD(globs[j].dev[d], globs[j].host, globs[j].size);
    if (r) {
        fprintf(stderr, "mcuda: device global %s: error %d\n", globs[j].name, r);
        globs[j].dev[d] = 0;
    }
    return r;
}

static char *pass(char *text, char *(*f)(const char *), const char *keep)
{
    char *in = f(text);
    if (!in)
        return text;
    if (text != keep)
        free(text);
    return in;
}

/* the texts of one kernel, ready for the driver */
typedef struct {
    Str *texts, lib, data;
    const char **tname;
    int ntext;
    int *kg, nkg; /* its device globals, in the order of its table */
    char path[1100], kname[300];
    int have_path, r;
    size_t bytes;
} Job;

static void job_free(Job *J)
{
    int i;
    for (i = 0; i < J->ntext; i++)
        if (J->texts[i].cap)
            free(J->texts[i].p);
    free(J->lib.p);
    free(J->data.p);
    free(J->texts);
    free((void *)J->tname);
    free(J->kg);
}

/* Kernel `item` of module mod as a job; J->r if its text cannot be made. Lock held. */
static void job_prep(Job *J, int mod, int item)
{
    Build B;
    int i, k, j, r = 0;
    u64 h1 = 1469598103934665603ULL, h2 = 88172645463325252ULL;
    memset(J, 0, sizeof *J);
    memset(&B, 0, sizeof B);
    memset(&B, 0, sizeof B);
    sprintf(J->kname, "%.290s", mods[mod].items[item].name);
    J->kname[mods[mod].items[item].len < 290 ? mods[mod].items[item].len : 290] = 0;
    want(&B, part_of(&B, mod), item);
    while (B.nstack) {
        B.nstack -= 2;
        take_fn(&B, B.stack[B.nstack], B.stack[B.nstack + 1]);
    }
    if (B.bad)
        r = 218;
    J->texts = (Str *)xrealloc(0, (size_t)(B.npart + 2) * sizeof *J->texts);
    J->tname = (const char **)xrealloc(0, (size_t)(B.npart + 2) * sizeof *J->tname);
    memset(J->texts, 0, (size_t)(B.npart + 2) * sizeof *J->texts);
    for (i = 0; !r && i < B.npart; i++) {
        Part *P = &B.part[i];
        Mod *M = &mods[P->mod];
        Str s;
        char **from, **to, *t;
        int n = 0;
        memset(&s, 0, sizeof s);
        put(&s, M->common.p ? M->common.p : "", M->common.len);
        if (B.nkg) {
            char line[80];
            sprintf(line, ".extern .global .align 8 .u64 __mcuda_gt[%d];\n", B.nkg);
            puts_(&s, line);
        }
        for (k = 0; k < M->nitems; k++)
            if (P->sel[k]) {
                const Item *it = &M->items[k];
                if (it->kind == IT_EXT || it->kind == IT_DATA)
                    put(&s, it->b, (size_t)(it->e - it->b));
                else if (it->pb)
                    put(&s, it->pb, (size_t)(it->pe - it->pb));
            }
        put(&s, P->body.p ? P->body.p : "", P->body.len);
        from = (char **)xrealloc(0, (size_t)(M->next + 1) * sizeof *from);
        to = (char **)xrealloc(0, (size_t)(M->next + 1) * sizeof *to);
        for (k = 0; k < M->next; k++)
            if (M->extdef[k] >= 0) {
                from[n] = (char *)xrealloc(0, strlen(M->ext[k]) + 9);
                sprintf(from[n], "__mcuda_%s", M->ext[k]);
                to[n++] = M->ext[k];
            }
        t = rewrite(s.p, from, to, n, P->vis, P->nvis);
        if (t) {
            free(s.p);
            s.p = t;
        }
        for (k = 0; k < n; k++)
            free(from[k]);
        free(from);
        free(to);
        s.p = pass(s.p, inline_calls, 0);
        s.p = pass(s.p, locals, 0);
        s.p = pass(s.p, printf_calls, 0);
        J->texts[J->ntext].p = s.p;
        J->texts[J->ntext].len = strlen(s.p);
        J->texts[J->ntext].cap = 1; /* to be freed */
        J->tname[J->ntext++] = M->m->name ? M->m->name : "module";
    }
    /* the pieces of the device library that are called, and what those call */
    header(&J->lib, mods[mod].m->ptx ? mods[mod].m->ptx : "");
    for (j = 0; !r && j < B.nneed; j++) {
        const struct HwLibPiece *pc = lib_find(B.need[j]);
        const char *d, *de;
        if (!pc) {
            fprintf(stderr, "mcuda: %s: device code calls %s, which nothing defines\n", J->kname, B.need[j] + 8);
            r = 218;
            continue;
        }
        for (d = pc->deps; d && *d; d = *de ? de + 1 : de) {
            de = strchr(d, ' ');
            if (!de)
                de = d + strlen(d);
            for (k = 0; k < B.nneed && (strlen(B.need[k]) != (size_t)(de - d) || memcmp(B.need[k], d, (size_t)(de - d))); k++)
                ;
            if (k == B.nneed && de > d) {
                if (B.nneed == B.capneed) {
                    B.capneed = B.capneed ? 2 * B.capneed : 32;
                    B.need = (char **)xrealloc(B.need, (size_t)B.capneed * sizeof *B.need);
                }
                B.need[B.nneed++] = xstrndup(d, (size_t)(de - d));
            }
        }
    }
    if (r)
        goto out;
    /* (its tables first, then declarations of all its functions: a piece may call one that comes later) */
    for (j = 0; j < B.nneed; j++) {
        const struct HwLibPiece *pc = lib_find(B.need[j]);
        if (pc->text[0] == '.' && pc->text[1] == 'g') {
            puts_(&J->lib, ".visible ");
            puts_(&J->lib, pc->text);
        }
    }
    for (j = 0; j < B.nneed; j++) {
        const struct HwLibPiece *pc = lib_find(B.need[j]);
        const char *b = strstr(pc->text, "\n{");
        if (pc->text[0] == '.' && pc->text[1] == 'g')
            continue;
        puts_(&J->lib, ".visible ");
        put(&J->lib, pc->text, b ? (size_t)(b - pc->text) : 0);
        puts_(&J->lib, ";\n");
    }
    for (j = 0; j < B.nneed; j++) {
        const struct HwLibPiece *pc = lib_find(B.need[j]);
        if (pc->text[0] == '.' && pc->text[1] == 'g')
            continue;
        puts_(&J->lib, ".visible ");
        puts_(&J->lib, pc->text);
    }
    if (B.nneed) {
        J->lib.p = pass(J->lib.p, locals, 0);
        J->lib.len = strlen(J->lib.p);
        J->texts[J->ntext] = J->lib;
        J->texts[J->ntext].cap = 0;
        J->tname[J->ntext++] = "mcuda device library";
    }
    /* the table of its device globals */
    if (B.nkg) {
        char line[80];
        header(&J->data, mods[mod].m->ptx ? mods[mod].m->ptx : "");
        sprintf(line, ".visible .global .align 8 .u64 __mcuda_gt[%d];\n", B.nkg);
        puts_(&J->data, line);
        J->texts[J->ntext] = J->data;
        J->texts[J->ntext].cap = 0;
        J->tname[J->ntext++] = "mcuda device globals";
    }

    for (i = 0; i < J->ntext; i++) {
        h1 = fnv(h1, J->texts[i].p, J->texts[i].len + 1);
        h2 = fnv(h2 * 31 + 7, J->texts[i].p, J->texts[i].len + 1);
        J->bytes += J->texts[i].len;
    }
    {
        char dn[256];
        int ver = 0;
        dn[0] = 0;
        hw.DeviceGetName(dn, 255, hw_dev);
        hw.DriverGetVersion(&ver);
        h1 = fnv(h1, dn, strlen(dn));
        h2 = fnv(h2, (const char *)&ver, sizeof ver);
    }
    J->have_path = cache_path(J->path, sizeof J->path, h1, h2);
out:
    for (j = 0; j < B.nneed; j++)
        free(B.need[j]);
    free(B.need);
    for (i = 0; i < B.npart; i++) {
        free(B.part[i].sel);
        free(B.part[i].body.p);
        free(B.part[i].vis);
    }
    free(B.part);
    free(B.stack);
    J->kg = B.kg;
    J->nkg = B.nkg;
    J->r = r;
}

/* The job through the driver's compiler and linker: the code (the link's, until it is destroyed) or an error,
   which is printed. Needs no lock. */
static int job_compile(Job *J, CUhandle *link, void **cubin, size_t *size)
{
    char log[8192];
    int opt[3], i, r, nopt = 2;
    void *val[3];
    opt[0] = 5; /* CU_JIT_ERROR_LOG_BUFFER */
    val[0] = log;
    opt[1] = 6; /* CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES */
    val[1] = (void *)(size_t)sizeof log;
    for (i = 0; i < J->ntext; i++)
        if (J->texts[i].p && strstr(J->texts[i].p, "\n\t.loc ")) {
            /* made with -g: the code keeps its source lines (nvcc's -lineinfo), for compute-sanitizer and cuda-gdb */
            opt[nopt] = 13; /* CU_JIT_GENERATE_LINE_INFO */
            val[nopt++] = (void *)(size_t)1;
            break;
        }
    log[0] = 0;
    *link = 0;
    r = hw.LinkCreate((unsigned)nopt, opt, val, link);
    if (r)
        fprintf(stderr, "mcuda: %s: the driver's linker does not start (error %d)\n", J->kname, r);
    for (i = 0; !r && i < J->ntext; i++) {
        r = hw.LinkAddData(*link, 1 /* CU_JIT_INPUT_PTX */, J->texts[i].p, J->texts[i].len + 1, J->tname[i], 0, 0, 0);
        if (r)
            fprintf(stderr, "mcuda: %s: the driver does not take the PTX of %s (error %d)\n%s\n", J->kname, J->tname[i], r, log);
    }
    if (!r) {
        r = hw.LinkComplete(*link, cubin, size);
        if (r)
            fprintf(stderr, "mcuda: %s: linking failed (error %d)\n%s\n", J->kname, r, log);
    }
    return r;
}

/* MCUDA_DUMP=DIR: the texts of every kernel that is loaded (with MCUDA_PRECOMPILE: of every kernel, and nothing is
   compiled), DIR/KERNEL.N.ptx. Returns whether it is set. */
static int job_dump(Job *J)
{
    const char *d = getenv("MCUDA_DUMP");
    int j;

    if (!d || !d[0])
        return 0;
    for (j = 0; j < J->ntext; j++) {
        char fn[1400];
        FILE *f;
        sprintf(fn, "%.900s/%.250s.%d.ptx", d, J->kname, j);
        f = fopen(fn, "w");
        if (f) {
            fwrite(J->texts[j].p, 1, J->texts[j].len, f);
            fclose(f);
        }
    }
    return 1;
}

/* Kernel `item` of module mod, loaded: K->module and K->f, or K->err. Lock held. */
static void kload(Kern *K, int mod, int item)
{
    Job J;
    int j, r;
    void *image = 0, *cubin = 0;
    size_t size = 0;
    CUhandle link = 0;
    double t0;

    K->tried = 1;
    job_prep(&J, mod, item);
    r = J.r;
    job_dump(&J);
    if (!r && getenv("MCUDA_SUBST")) { /* MCUDA_SUBST=DIR: DIR/KERNEL.N.ptx, where there is one, in place of text N */
        for (j = 0; j < J.ntext; j++) {
            char fn[1400];
            FILE *f;
            long n;
            char *t;
            sprintf(fn, "%.900s/%.250s.%d.ptx", getenv("MCUDA_SUBST"), J.kname, j);
            f = fopen(fn, "r");
            if (!f)
                continue;
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            t = (char *)xrealloc(0, (size_t)n + 1);
            n = (long)fread(t, 1, (size_t)n, f);
            t[n] = 0;
            fclose(f);
            if (J.texts[j].cap)
                free(J.texts[j].p);
            J.texts[j].p = t;
            J.texts[j].len = (size_t)n;
            J.texts[j].cap = 1;
            J.have_path = 0;
            fprintf(stderr, "mcuda: %s: text %d from %s\n", J.kname, j, fn);
        }
    }
    if (!r && J.have_path)
        image = cache_read(J.path, &size);
    if (image) {
        r = hw.ModuleLoadData(&K->module, image);
        if (r) { /* a file that is not what it should be */
            free(image);
            image = 0;
            remove(J.path);
            r = 0;
        } else if (verbose())
            fprintf(stderr, "mcuda: %s: from %s\n", J.kname, J.path);
    }
    if (!r && !image) {
        t0 = now();
        r = job_compile(&J, &link, &cubin, &size);
        if (!r) {
            r = hw.ModuleLoadData(&K->module, cubin);
            if (r)
                fprintf(stderr, "mcuda: %s: the driver does not load what it compiled (error %d)\n", J.kname, r);
            if (!r && J.have_path)
                cache_write(J.path, cubin, size);
            if (!r && verbose())
                fprintf(stderr, "mcuda: %s: compiled in %.2f s, %lu bytes of PTX in %d texts, %lu of code\n", J.kname, now() - t0,
                        (unsigned long)J.bytes, J.ntext, (unsigned long)size);
        }
        if (link)
            hw.LinkDestroy(link);
    }
    free(image);
    if (verbose() && (locals_slots || locals_fwds)) {
        fprintf(stderr, "mcuda: %s: %lu locals in registers, %lu loads taken from the store before\n", J.kname, locals_slots, locals_fwds);
        locals_slots = locals_fwds = 0;
    }
    if (!r) {
        r = hw.ModuleGetFunction(&K->f, K->module, J.kname);
        if (r)
            fprintf(stderr, "mcuda: %s: not in its own module (error %d)\n", J.kname, r);
    }
    if (!r && J.nkg) {
        CUdeviceptr gt = 0, *addr = (CUdeviceptr *)xrealloc(0, (size_t)J.nkg * sizeof *addr);
        size_t gtb = 0;
        for (j = 0; !r && j < J.nkg; j++) {
            r = glob_dev(J.kg[j]);
            addr[j] = globs[J.kg[j]].dev[hw_cur()];
        }
        if (!r)
            r = hw.ModuleGetGlobal(&gt, &gtb, K->module, "__mcuda_gt");
        if (!r)
            r = hw.MemcpyHtoD(gt, addr, (size_t)J.nkg * sizeof *addr);
        if (r)
            fprintf(stderr, "mcuda: %s: the table of its device globals: error %d\n", J.kname, r);
        free(addr);
    }
    if (r && K->module) {
        hw.ModuleUnload(K->module);
        K->module = 0;
        K->f = 0;
    }
    job_free(&J);
    K->err = r;
}

/* ---- MCUDA_PRECOMPILE=N ----
   The program's first use of the device becomes this instead: every kernel of the program (MCUDA_PRECOMPILE_ONLY=
   TEXT: those with TEXT in their names) that the cache does not hold goes through the driver's compiler, N at
   a time, and into the cache; then the program ends. For a build to pay for what the first run would. */
static int pre_mod, pre_item;
static unsigned long pre_seen, pre_made, pre_bad;

static void *pre_worker(void *arg)
{
    const char *only = getenv("MCUDA_PRECOMPILE_ONLY");
    (void)arg;
    if (hw_enter())
        return 0;
    for (;;) {
        Job J;
        int have = 0;
        hw_lock();
        while (!have && pre_mod < nmods) {
            const Item *it;
            index_mod(pre_mod);
            if (pre_item >= mods[pre_mod].nitems) {
                pre_mod++;
                pre_item = 0;
                continue;
            }
            it = &mods[pre_mod].items[pre_item++];
            if (it->kind != IT_ENTRY)
                continue;
            if (only && only[0]) {
                char nm[300];
                sprintf(nm, "%.*s", it->len < 290 ? it->len : 290, it->name);
                if (!strstr(nm, only))
                    continue;
            }
            job_prep(&J, pre_mod, pre_item - 1);
            pre_seen++;
            have = 1;
        }
        hw_unlock();
        if (!have)
            break;
        if (J.r)
            pre_bad++;
        else if (job_dump(&J))
            ;
        else if (J.have_path && access(J.path, R_OK) != 0) {
            CUhandle link = 0;
            void *cubin = 0;
            size_t size = 0;
            double t0 = now();
            int r = job_compile(&J, &link, &cubin, &size);
            if (!r)
                cache_write(J.path, cubin, size);
            if (link)
                hw.LinkDestroy(link);
            hw_lock();
            if (r)
                pre_bad++;
            else
                pre_made++;
            hw_unlock();
            if (!r)
                fprintf(stderr, "mcuda: %s: compiled in %.2f s, %lu bytes of PTX, %lu of code\n", J.kname, now() - t0, (unsigned long)J.bytes,
                        (unsigned long)size);
        }
        job_free(&J);
    }
    return 0;
}

void hw_precompile(int n)
{
    pthread_t th[64];
    int i, made = 0;
    double t0 = now();
    if (n > 64)
        n = 64;
    hw_lock();
    take();
    hw_unlock();
    for (i = 0; i < n - 1; i++)
        if (pthread_create(&th[made], 0, pre_worker, 0) == 0)
            made++;
    pre_worker(0);
    for (i = 0; i < made; i++)
        pthread_join(th[i], 0);
    fprintf(stderr, "mcuda: %lu kernels, %lu compiled, %lu failed, %.1f s\n", pre_seen, pre_made, pre_bad, now() - t0);
    fflush(stderr);
    _exit(pre_bad ? 1 : 0);
}

/* The module record m as a module here. Lock held. */
static int mod_of(struct __mcuda_module *m)
{
    Mod *M;
    take();
    M = (Mod *)m->handle;
    if (!M || M < mods || M >= mods + nmods || M->m != m) {
        hw_unlock();
        __mcuda_register(m);
        hw_lock();
        take();
        M = (Mod *)m->handle;
    }
    return (int)(M - mods);
}

/* Kernel `kernel` as module m launches it, loaded; 0 and an error number if there is none or it fails. Lock held. */
static Kern *kern_get(struct __mcuda_module *m, const char *kernel, int *err)
{
    int mod = mod_of(m), len = (int)strlen(kernel), v, j;
    Kern *K;
    index_mod(mod);
    v = lookup(&mods[mod], kernel, len);
    if (v <= 0 || mods[mod].items[v - 1].kind != IT_ENTRY) {
        /* not a kernel of the unit that launches it: of the first other unit that defines one of this name */
        v = 0;
        for (j = 0; j < nkdefs && !v; j++)
            if (kdefs[j].len == len && memcmp(kdefs[j].name, kernel, (size_t)len) == 0 && kdefs[j].mod != mod) {
                index_mod(kdefs[j].mod);
                v = lookup(&mods[kdefs[j].mod], kernel, len);
                if (v > 0 && mods[kdefs[j].mod].items[v - 1].kind == IT_ENTRY)
                    mod = kdefs[j].mod;
                else
                    v = 0;
            }
        if (!v) {
            *err = 98; /* cudaErrorInvalidDeviceFunction */
            return 0;
        }
    }
    K = &mods[mod].kern[v - 1];
    if (hw_cur()) { /* (the thread's device is not 0) */
        if (!K->more)
            K->more = (Kern *)calloc(HW_MAXDEV, sizeof *K->more);
        if (!K->more) {
            *err = 2; /* cudaErrorMemoryAllocation */
            return 0;
        }
        K = &K->more[hw_cur()];
    }
    if (!K->tried)
        kload(K, mod, v - 1);
    *err = K->err;
    return K->err ? 0 : K;
}

/* ---- launching ---- */

typedef struct {
    const char *kernel; /* the address of the name is the key: the compiler passes a constant */
    struct __mcuda_module *m;
    int dev;            /* the device whose module f is in */
    CUhandle f;
    unsigned maxdyn;
    unsigned long calls; /* MCUDA_PROFILE: launches, and the seconds they ran */
    double secs;
    double first;        /* the first launch, which loads the code: not in secs */
} Fn;
static Fn *fns;
static unsigned nfns, capfns;

static Fn *fn_get(struct __mcuda_module *m, const char *kernel, int *err)
{
    unsigned h, i;
    Kern *g;
    Fn *f;
    int v = 0, dev = hw_cur();
    if (capfns) {
        h = (unsigned)(((size_t)kernel >> 3) * 2654435761U) & (capfns - 1);
        for (i = h; fns[i].kernel; i = (i + 1) & (capfns - 1))
            if (fns[i].kernel == kernel && fns[i].m == m && fns[i].dev == dev)
                return &fns[i];
    }
    g = kern_get(m, kernel, err);
    if (!g)
        return 0;
    if (2 * (nfns + 1) > capfns) {
        Fn *old = fns;
        unsigned oc = capfns;
        capfns = capfns ? 2 * capfns : 256;
        fns = (Fn *)xrealloc(0, capfns * sizeof *fns);
        memset(fns, 0, capfns * sizeof *fns);
        for (i = 0; i < oc; i++)
            if (old[i].kernel) {
                h = (unsigned)(((size_t)old[i].kernel >> 3) * 2654435761U) & (capfns - 1);
                while (fns[h].kernel)
                    h = (h + 1) & (capfns - 1);
                fns[h] = old[i];
            }
        free(old);
    }
    h = (unsigned)(((size_t)kernel >> 3) * 2654435761U) & (capfns - 1);
    while (fns[h].kernel)
        h = (h + 1) & (capfns - 1);
    f = &fns[h];
    f->f = g->f;
    f->kernel = kernel;
    f->m = m;
    f->dev = dev;
    hw.FuncGetAttribute(&v, 8 /* MAX_DYNAMIC_SHARED_SIZE_BYTES */, f->f);
    f->maxdyn = (unsigned)v;
    nfns++;
    return f;
}

/* MCUDA_PROFILE=1: every launch waits for the device before and after and its time is added up by kernel; the
   sums are printed when the program ends, the costliest first. (The waiting changes the program's own timing.) */
static int profiling(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("MCUDA_PROFILE");
        on = v && v[0] && v[0] != '0';
    }
    return on;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static int by_secs(const void *a, const void *b)
{
    double x = ((const Fn *)a)->secs, y = ((const Fn *)b)->secs;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void profile_print(void)
{
    Fn *v;
    unsigned i, n = 0;
    double all = 0, first = 0;
    hw_lock();
    v = (Fn *)xrealloc(0, (nfns + 1) * sizeof *v);
    for (i = 0; i < capfns; i++)
        if (fns[i].kernel && fns[i].calls) {
            v[n++] = fns[i];
            all += fns[i].secs;
            first += fns[i].first;
        }
    hw_unlock();
    qsort(v, n, sizeof *v, by_secs);
    fprintf(stderr, "mcuda: %.3f s in %u kernels, and %.3f s in the first launch of each\n", all, n, first);
    for (i = 0; i < n && i < 60; i++)
        fprintf(stderr, "mcuda: %9.3f ms %5.1f%% %8lu x %8.1f us first %7.2f ms  %s\n", v[i].secs * 1e3, all > 0 ? 100.0 * v[i].secs / all : 0.0,
                v[i].calls, v[i].calls > 1 ? v[i].secs * 1e6 / (double)(v[i].calls - 1) : 0.0, v[i].first * 1e3, v[i].kernel);
    free(v);
}

int __mcuda_launch(struct __mcuda_module *m, const char *kernel, const unsigned grid[3], const unsigned block[3],
                   unsigned long shared_bytes, void *stream, void **args)
{
    Fn *f;
    CUhandle fh = 0;
    int r = hw_enter();
    if (!r && (!m || !kernel))
        r = 1;
    if (!r) {
        hw_lock();
        f = fn_get(m, kernel, &r);
        if (f) {
            fh = f->f;
            if (shared_bytes > f->maxdyn && shared_bytes < 0x7fffffffUL) {
                /* (NVIDIA's run time wants cudaFuncSetAttribute for this; here the launch asks for it) */
                if (hw.FuncSetAttribute(fh, 8, (int)shared_bytes) == 0)
                    f->maxdyn = (unsigned)shared_bytes;
            }
        }
        hw_unlock();
    }
    if (!r && profiling()) {
        static int registered;
        double t0;
        hw.CtxSynchronize();
        t0 = now();
        r = hw.LaunchKernel(fh, grid[0], grid[1], grid[2], block[0], block[1], block[2], (unsigned)shared_bytes, stream, args, 0);
        hw.CtxSynchronize();
        t0 = now() - t0;
        hw_lock();
        f = fn_get(m, kernel, &r);
        if (f) {
            if (f->calls++)
                f->secs += t0;
            else
                f->first = t0;
        }
        if (!registered) {
            registered = 1;
            atexit(profile_print);
        }
        hw_unlock();
    } else if (!r) {
        static int blocking = -1;
        if (blocking < 0) {
            const char *v = getenv("CUDA_LAUNCH_BLOCKING");
            blocking = v && v[0] && v[0] != '0';
        }
        r = hw.LaunchKernel(fh, grid[0], grid[1], grid[2], block[0], block[1], block[2], (unsigned)shared_bytes, stream, args, 0);
        if (!r && blocking) /* (as NVIDIA's run time: the launch returns when the kernel has run, with its error) */
            r = hw.CtxSynchronize();
    }
    if (verbose() && kernel && (r || getenv("MCUDA_TRACE")))
        fprintf(stderr, "mcuda: launch %s: %d\n", kernel, r);
    if (r)
        hw_err_set(r);
    return r;
}

CUdeviceptr hw_symbol(const void *p, size_t *left)
{
    const char *c = (const char *)p;
    CUdeviceptr d = 0;
    int j, err;
    hw_lock();
    take();
    for (j = 0; j < nglobs; j++)
        if (c >= globs[j].host && c < globs[j].host + globs[j].size) {
            err = glob_dev(j);
            if (!err) {
                d = globs[j].dev[hw_cur()] + (CUdeviceptr)(c - globs[j].host);
                if (left)
                    *left = globs[j].size - (size_t)(c - globs[j].host);
            }
            break;
        }
    hw_unlock();
    return d;
}

void *__mcuda_symbol(struct __mcuda_module *m, const char *name)
{
    void *h;
    if (!m || !name)
        return 0;
    h = host_of(m, name, (int)strlen(name));
    return h;
}

CUhandle hw_kernel_of(const void *p)
{
    CUhandle fh = 0;
    int i, err;
    hw_lock();
    take();
    for (i = 0; i < nmods && !fh; i++) {
        const struct __mcuda_sym *y;
        for (y = mods[i].m->syms; y && y->name; y++)
            if (y->addr == p) {
                Fn *f = fn_get(mods[i].m, y->name, &err);
                if (f)
                    fh = f->f;
                break;
            }
    }
    hw_unlock();
    return fh;
}
