/* dlcheck [FILE.so ...]: will this C library's dlopen (src/dl.c) take these shared objects? It reads each
   object's dynamic symbols and lists the undefined ones that neither the table of glibc's names
   (src/glibctab.c: the same list, compiled in here) nor another of the objects given defines: the strong
   ones, each of which makes dlopen fail ("undefined symbol"), and the weak ones, which are 0 when the object
   runs. It also says when an object has thread-local storage, which the loader refuses. Made for NVIDIA's
   driver, whose libraries change what they import from one version to the next:
       dlcheck libcuda.so.620.70.06 libnvidia-ptxjitcompiler.so.620.70.06
   Without arguments it looks for libcuda.so.* and libnvidia-ptxjitcompiler.so.* where the loader looks (and
   where NVIDIA's container run time puts them). The status is 0 when nothing strong is missing.
   It only reads the files (any machine will do: the build's, with the libraries copied to it, or the image:
   /usr/bin/dlcheck). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLIBC_NAMES_ONLY
#include "src/glibctab.c"
#define NAME_F(n) #n,
#define NAME_A(n, fn) #n,
#define NAME_D(n, obj) #n,
static const char *const table[] = {GLIBC_LIST(NAME_F, NAME_A, NAME_D) 0};

typedef struct {
    const char *path;
    unsigned char *d;
    unsigned long n;
    unsigned long sym, nsym, str, strn; /* .dynsym and its strings: offsets in the file, counts */
    int tls;
} Obj;

static Obj objs[16];
static int nobjs;

static unsigned long rd(const unsigned char *p, int bytes)
{
    unsigned long v = 0;
    int i;
    for (i = bytes - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static int load(const char *path)
{
    Obj *o = &objs[nobjs];
    FILE *f = fopen(path, "rb");
    long size;
    unsigned long shoff, shnum, shentsize, phoff, phnum, phentsize, i;
    if (!f) {
        fprintf(stderr, "dlcheck: cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    o->d = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    if (size < 64 || !o->d || fread(o->d, 1, (size_t)size, f) != (size_t)size || memcmp(o->d, "\177ELF", 4) != 0 ||
        o->d[4] != 2 || o->d[5] != 1 || rd(o->d + 16, 2) != 3 || rd(o->d + 18, 2) != 62) {
        fprintf(stderr, "dlcheck: %s is not a shared object for x86-64\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    o->path = path;
    o->n = (unsigned long)size;
    phoff = rd(o->d + 32, 8);
    shoff = rd(o->d + 40, 8);
    phentsize = rd(o->d + 54, 2);
    phnum = rd(o->d + 56, 2);
    shentsize = rd(o->d + 58, 2);
    shnum = rd(o->d + 60, 2);
    for (i = 0; i < phnum && phoff + (i + 1) * phentsize <= o->n; i++)
        if (rd(o->d + phoff + i * phentsize, 4) == 7) /* PT_TLS */
            o->tls = 1;
    for (i = 0; i < shnum && shoff + (i + 1) * shentsize <= o->n; i++) {
        const unsigned char *sh = o->d + shoff + i * shentsize;
        if (rd(sh + 4, 4) == 11) { /* SHT_DYNSYM */
            unsigned long link = rd(sh + 40, 4), entsize = rd(sh + 56, 8);
            const unsigned char *st = o->d + shoff + link * shentsize;
            if (link >= shnum || entsize != 24)
                break;
            o->sym = rd(sh + 24, 8);
            o->nsym = rd(sh + 32, 8) / 24;
            o->str = rd(st + 24, 8);
            o->strn = rd(st + 32, 8);
        }
    }
    if (!o->nsym || o->sym + o->nsym * 24 > o->n || o->str + o->strn > o->n || !o->strn || o->d[o->str + o->strn - 1]) {
        fprintf(stderr, "dlcheck: %s: no table of dynamic symbols to read\n", path);
        return -1;
    }
    nobjs++;
    return 0;
}

/* symbol I of O: its name ("" when it has none); *UNDEF and *WEAK say what it is */
static const char *sym(const Obj *o, unsigned long i, int *undef, int *weak)
{
    const unsigned char *s = o->d + o->sym + i * 24;
    unsigned long name = rd(s, 4);
    *undef = rd(s + 6, 2) == 0;
    *weak = (s[4] >> 4) == 2;
    return name < o->strn ? (const char *)o->d + o->str + name : "";
}

static int in_table(const char *name)
{
    int i;
    for (i = 0; table[i]; i++)
        if (!strcmp(table[i], name))
            return 1;
    return 0;
}

static int defined_by_one(const char *name)
{
    int k, undef, weak;
    unsigned long i;
    for (k = 0; k < nobjs; k++)
        for (i = 1; i < objs[k].nsym; i++) {
            const char *n = sym(&objs[k], i, &undef, &weak);
            if (!undef && !strcmp(n, name))
                return 1;
        }
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* the objects of the driver in the loader's directories: the last in order of name of each kind */
static void find_driver(void)
{
    static const char *const dirs[] = {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/lib/x86_64-linux-gnu", "/lib64",
                                       "/usr/lib", "/lib", "/usr/local/lib", "/usr/local/nvidia/lib64",
                                       "/usr/local/nvidia/lib", 0};
    static const char *const kinds[] = {"libcuda.so.", "libnvidia-ptxjitcompiler.so.", 0};
    int i, k;
    for (k = 0; kinds[k]; k++) {
        char *best = 0;
        for (i = 0; dirs[i] && !best; i++) {
            DIR *d = opendir(dirs[i]);
            struct dirent *de;
            char cur[300];
            cur[0] = 0;
            if (!d)
                continue;
            while ((de = readdir(d)) != 0)
                if (!strncmp(de->d_name, kinds[k], strlen(kinds[k])) && strlen(de->d_name) < sizeof cur &&
                    strcmp(de->d_name, cur) > 0)
                    strcpy(cur, de->d_name);
            closedir(d);
            if (cur[0]) {
                best = (char *)malloc(strlen(dirs[i]) + strlen(cur) + 2);
                if (best)
                    sprintf(best, "%s/%s", dirs[i], cur);
            }
        }
        if (best)
            load(best);
        else
            fprintf(stderr, "dlcheck: no %s* in the directories of libraries\n", kinds[k]);
    }
}

int main(int argc, char **argv)
{
    int k, pass, undef, weak, strong_missing = 0, weak_missing = 0, tls = 0, failed = 0;
    unsigned long i, total = 0;
    for (k = 1; k < argc && nobjs < 16; k++)
        if (load(argv[k]) < 0)
            failed = 1;
    if (argc < 2)
        find_driver();
    if (!nobjs)
        return 2;
    for (k = 0; k < nobjs; k++) {
        const Obj *o = &objs[k];
        const char **miss = (const char **)malloc(o->nsym * sizeof *miss);
        if (!miss)
            return 2;
        printf("%s\n", o->path);
        if (o->tls) {
            printf("  has thread-local storage (PT_TLS): the loader refuses it\n");
            tls = 1;
        }
        for (pass = 0; pass < 2; pass++) { /* the strong ones, then the weak */
            unsigned long n = 0, j, und = 0;
            for (i = 1; i < o->nsym; i++) {
                const char *name = sym(o, i, &undef, &weak);
                if (!undef || !name[0] || weak != pass)
                    continue;
                und++;
                if (!in_table(name) && !defined_by_one(name))
                    miss[n++] = name;
            }
            total += und;
            qsort((void *)miss, n, sizeof *miss, by_name);
            printf("  %s: %lu undefined, %lu that this C library has not%s\n", pass ? "weak" : "strong", und, n,
                   n ? ":" : "");
            for (j = 0; j < n; j++)
                if (j == 0 || strcmp(miss[j], miss[j - 1]) != 0)
                    printf("    %s\n", miss[j]);
            if (pass)
                weak_missing += (int)n;
            else
                strong_missing += (int)n;
        }
        free((void *)miss);
    }
    printf("dlcheck: %d object%s, %lu undefined symbols: %d strong and %d weak ones are missing%s\n", nobjs,
           nobjs == 1 ? "" : "s", total, strong_missing, weak_missing,
           strong_missing || tls ? " (dlopen will fail)" : " (dlopen will take them)");
    return strong_missing || tls || failed ? 1 : 0;
}
