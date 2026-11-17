/* symprefix PREFIX NAMES IN.o OUT.o: a copy of the ELF64 object IN.o in which every global symbol whose name is a
   line of the file NAMES, defined or only referred to, has PREFIX before its name. mkcuda.sh makes the two CUDA
   run times (the CPU's and the GPU's, which define the same names) fit into one library with it; mcc's objcopy
   does not rename. Little-endian objects; the new names go into a copy of the string table at the end of the
   file, so nothing else moves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rd(const unsigned char *p, int n)
{
    unsigned long v = 0;
    while (n--)
        v = v << 8 | p[n];
    return v;
}

static void wr(unsigned char *p, int n, unsigned long v)
{
    int i;
    for (i = 0; i < n; i++, v >>= 8)
        p[i] = (unsigned char)(v & 255);
}

static char **names;
static long nnames;

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int listed(const char *s)
{
    long lo = 0, hi = nnames - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int c = strcmp(s, names[mid]);
        if (c == 0)
            return 1;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "symprefix: cannot read %s\n", path);
        exit(1);
    }
    b = (unsigned char *)malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "symprefix: cannot read %s\n", path);
        exit(1);
    }
    b[len] = 0;
    fclose(f);
    *n = len;
    return b;
}

int main(int argc, char **argv)
{
    unsigned char *list, *o, *sh, *str;
    char *p, *add;
    long nlist, no, i, cap, nadd = 0, capadd, renamed = 0;
    unsigned long shoff, shnum, symoff = 0, symsize = 0, link = 0, stroff, strsize, k;
    size_t plen;
    FILE *out;
    if (argc != 5) {
        fprintf(stderr, "usage: symprefix PREFIX NAMES IN.o OUT.o\n");
        return 2;
    }
    plen = strlen(argv[1]);
    list = slurp(argv[2], &nlist);
    cap = 1;
    for (i = 0; i < nlist; i++)
        cap += list[i] == '\n';
    names = (char **)malloc((size_t)cap * sizeof *names);
    for (p = strtok((char *)list, "\r\n"); p; p = strtok(0, "\r\n"))
        if (*p)
            names[nnames++] = p;
    qsort(names, (size_t)nnames, sizeof *names, by_str);
    o = slurp(argv[3], &no);
    if (no < 64 || memcmp(o, "\177ELF\2\1", 6) != 0 || rd(o + 16, 2) != 1) {
        fprintf(stderr, "symprefix: %s: not a 64-bit little-endian ELF object\n", argv[3]);
        return 1;
    }
    shoff = rd(o + 0x28, 8);
    shnum = rd(o + 0x3c, 2);
    if (shoff + shnum * 64 > (unsigned long)no) {
        fprintf(stderr, "symprefix: %s: damaged\n", argv[3]);
        return 1;
    }
    sh = o + shoff;
    for (k = 0; k < shnum; k++)
        if (rd(sh + k * 64 + 4, 4) == 2) { /* SHT_SYMTAB */
            symoff = rd(sh + k * 64 + 0x18, 8);
            symsize = rd(sh + k * 64 + 0x20, 8);
            link = rd(sh + k * 64 + 0x28, 4);
        }
    if (!symsize || link >= shnum) { /* no symbols: nothing to rename */
        stroff = strsize = 0;
        str = o;
    } else {
        stroff = rd(sh + link * 64 + 0x18, 8);
        strsize = rd(sh + link * 64 + 0x20, 8);
        str = o + stroff;
        if (stroff + strsize > (unsigned long)no || symoff + symsize > (unsigned long)no) {
            fprintf(stderr, "symprefix: %s: damaged\n", argv[3]);
            return 1;
        }
    }
    capadd = 1 << 16;
    add = (char *)malloc((size_t)capadd);
    for (k = 0; k + 24 <= symsize; k += 24) {
        unsigned char *s = o + symoff + k;
        unsigned long nm = rd(s, 4), bind = s[4] >> 4;
        const char *name;
        size_t len;
        if ((bind != 1 && bind != 2) || nm >= strsize)
            continue;
        name = (const char *)str + nm;
        if (!listed(name))
            continue;
        len = strlen(name);
        if (nadd + (long)(plen + len) + 1 > capadd) {
            capadd = 2 * capadd + (long)(plen + len) + 1;
            add = (char *)realloc(add, (size_t)capadd);
        }
        if (!add) {
            fprintf(stderr, "symprefix: no memory\n");
            return 1;
        }
        wr(s, 4, strsize + (unsigned long)nadd);
        memcpy(add + nadd, argv[1], plen);
        memcpy(add + nadd + plen, name, len + 1);
        nadd += (long)(plen + len) + 1;
        renamed++;
    }
    out = fopen(argv[4], "wb");
    if (!out) {
        fprintf(stderr, "symprefix: cannot write %s\n", argv[4]);
        return 1;
    }
    if (renamed) {
        /* the string table moves to the end, with the new names after the old ones */
        unsigned char *copy = (unsigned char *)malloc((size_t)strsize + 1);
        long pad = (8 - no % 8) % 8;
        static const unsigned char zero[8];
        memcpy(copy, str, (size_t)strsize);
        wr(sh + link * 64 + 0x18, 8, (unsigned long)(no + pad));
        wr(sh + link * 64 + 0x20, 8, strsize + (unsigned long)nadd);
        if (fwrite(o, 1, (size_t)no, out) != (size_t)no || fwrite(zero, 1, (size_t)pad, out) != (size_t)pad ||
            fwrite(copy, 1, (size_t)strsize, out) != (size_t)strsize || fwrite(add, 1, (size_t)nadd, out) != (size_t)nadd) {
            fprintf(stderr, "symprefix: cannot write %s\n", argv[4]);
            return 1;
        }
    } else if (fwrite(o, 1, (size_t)no, out) != (size_t)no) {
        fprintf(stderr, "symprefix: cannot write %s\n", argv[4]);
        return 1;
    }
    if (fclose(out)) {
        fprintf(stderr, "symprefix: cannot write %s\n", argv[4]);
        return 1;
    }
    return 0;
}
