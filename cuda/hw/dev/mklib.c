/* mklib OUT.c: writes the device library of mcudahw as a C file (lib.c: the table hw_lib of hw.h).
   The library's sources are CUDA files (dev/NAME.cu) that the compiler turns into PTX like any device code.
   This program is linked with their objects, compiled for the machine it runs on; each has a function
   mcd_unit_NAME that launches a kernel, and the launch arrives here (__mcuda_launch below) with the unit's
   module record: its PTX and the host objects it names. The PTX is taken apart into pieces, one for every
   function it defines:
     mcdw_NAME   becomes the piece __mcuda_NAME, which is what compiled device code calls for NAME;
     any other   is a helper under a name of its own.
   A host object that the PTX names (a table) becomes a piece too, a .global with the object's bytes. Each
   piece lists the pieces it uses, so that the run time links only what a program calls. */
#include <mcuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mcd_unit_mathf(void);
void mcd_unit_mathd(void);
void mcd_unit_mem(void);
static void (*const units[])(void) = {mcd_unit_mathf, mcd_unit_mathd, mcd_unit_mem};

static struct __mcuda_module *mods[32];
static int nmods;

int __mcuda_launch(struct __mcuda_module *m, const char *kernel, const unsigned grid[3], const unsigned block[3],
                   unsigned long shared_bytes, void *stream, void **args)
{
    (void)kernel;
    (void)grid;
    (void)block;
    (void)shared_bytes;
    (void)stream;
    (void)args;
    mods[nmods++] = m;
    return 0;
}

typedef struct {
    char *orig; /* its name in the unit's PTX */
    char *name; /* and in the library */
    char *text;
    char *deps;
    int unit;
} Piece;
static Piece *pieces;
static int npieces, cappieces;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "mklib: out of memory\n");
        exit(1);
    }
    return p;
}

static char *dup(const char *s, size_t n)
{
    char *d = (char *)xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int isid(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static Piece *piece_new(void)
{
    if (npieces == cappieces) {
        cappieces = cappieces ? 2 * cappieces : 256;
        pieces = (Piece *)realloc(pieces, (size_t)cappieces * sizeof *pieces);
        if (!pieces)
            exit(1);
    }
    memset(&pieces[npieces], 0, sizeof *pieces);
    return &pieces[npieces++];
}

/* The plain name inside a C++ name of the form [_]Z<length><name>...; the name itself if it is not one. */
static char *plain(const char *s, size_t n)
{
    const char *p = s;
    size_t len = 0;
    if (*p == '_')
        p++;
    if (*p == 'Z' && p[1] >= '1' && p[1] <= '9') {
        for (p++; *p >= '0' && *p <= '9'; p++)
            len = len * 10 + (size_t)(*p - '0');
        if (len && p + len <= s + n)
            return dup(p, len);
    }
    return dup(s, n);
}

/* The name that a line declaring a function gives it: p is after ".func". */
static const char *func_name(const char *p, size_t *len)
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
    *len = (size_t)(q - p);
    return p;
}

static void take(struct __mcuda_module *m, int unit)
{
    const char *p = m->ptx, *e, *l, *q;
    char buf[256];
    size_t n;
    int k = 0;
    for (; *p; p = *e ? e + 1 : e) {
        e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        l = p;
        if (strncmp(l, ".visible ", 9) == 0)
            l += 9;
        if (strncmp(p, ".extern .global", 15) == 0) {
            /* .extern .global .align A .b8 name[size]; */
            const struct __mcuda_sym *y;
            const unsigned char *host = 0;
            unsigned long size, align = 1, i;
            Piece *pc;
            char *t;
            size_t at;
            for (q = e; q > p && *q != '['; q--)
                ;
            size = strtoul(q + 1, 0, 10);
            for (l = q; l > p && isid((unsigned char)l[-1]); l--)
                ;
            n = (size_t)(q - l);
            for (y = m->syms; y && y->name; y++)
                if (strlen(y->name) == n && memcmp(y->name, l, n) == 0)
                    host = (const unsigned char *)y->addr;
            if (!host || !size) {
                fprintf(stderr, "mklib: %s: the object %.*s has no %s\n", m->name ? m->name : "?", (int)n, l, host ? "size in the PTX" : "address");
                exit(1);
            }
            if ((q = strstr(p, ".align ")) != 0 && q < e)
                align = strtoul(q + 7, 0, 10);
            pc = piece_new();
            pc->unit = unit;
            pc->orig = dup(l, n);
            sprintf(buf, "__mcuda_d%d_%d", unit, k++);
            pc->name = dup(buf, strlen(buf));
            t = (char *)xmalloc(size * 5 + 128);
            sprintf(t, ".global .align %lu .b8 %s[%lu] = {", align < 8 ? 8 : align, pc->name, size);
            at = strlen(t);
            for (i = 0; i < size; i++)
                at += (size_t)sprintf(t + at, i ? ", %u" : "%u", host[i]);
            strcpy(t + at, "};\n");
            pc->text = t;
        } else if (strncmp(l, ".func", 5) == 0 && e > p && e[-1] != ';') {
            /* a function: up to the line that is its closing brace */
            const char *end = strstr(e, "\n}\n");
            Piece *pc = piece_new();
            char *pl;
            if (!end) {
                fprintf(stderr, "mklib: a function without an end\n");
                exit(1);
            }
            end += 3;
            q = func_name(l + 5, &n);
            pc->unit = unit;
            pc->orig = dup(q, n);
            pl = plain(q, n);
            if (strncmp(pl, "mcdw_", 5) == 0 && strlen(pl) < 200)
                sprintf(buf, "__mcuda_%s", pl + 5);
            else
                sprintf(buf, "__mcuda_i%d_%.200s", unit, pl);
            pc->name = dup(buf, strlen(buf));
            pc->text = dup(l, (size_t)(end - l));
            e = end - 1;
        } else if (strncmp(l, ".entry", 6) == 0 && e > p && e[-1] != ';') {
            const char *end = strstr(e, "\n}\n");
            if (end)
                e = end + 2;
        }
    }
}

/* The piece's text with the names of the library for those of its unit, and the list of what it uses. */
static void finish(Piece *pc)
{
    const char *p = pc->text, *q;
    size_t cap = strlen(p) * 2 + 256, len = 0, dlen = 0, dcap = 256, n;
    char *t = (char *)xmalloc(cap), *d = (char *)xmalloc(dcap);
    int k;
    d[0] = 0;
    while (*p) {
        if (isid((unsigned char)*p) && (p == pc->text || (!isid((unsigned char)p[-1]) && p[-1] != '%'))) {
            const char *use = p;
            for (q = p; isid((unsigned char)*q); q++)
                ;
            n = (size_t)(q - p);
            for (k = 0; k < npieces; k++)
                if (pieces[k].unit == pc->unit && strlen(pieces[k].orig) == n && memcmp(pieces[k].orig, p, n) == 0) {
                    use = pieces[k].name;
                    n = strlen(use);
                    break;
                }
            if (len + n + 2 > cap) {
                cap = 2 * cap + n;
                t = (char *)realloc(t, cap);
            }
            memcpy(t + len, use, n);
            len += n;
            if (n > 8 && memcmp(use, "__mcuda_", 8) == 0 && (strlen(pc->name) != n || memcmp(use, pc->name, n))) {
                /* (a name once: found as a whole word of the list) */
                char *word = dup(use, n);
                int have = 0;
                const char *s = d;
                while ((s = strstr(s, word)) != 0) {
                    if ((s == d || s[-1] == ' ') && (s[n] == ' ' || !s[n]))
                        have = 1;
                    s += n;
                }
                free(word);
                if (!have) {
                    if (dlen + n + 2 > dcap) {
                        dcap = 2 * dcap + n;
                        d = (char *)realloc(d, dcap);
                    }
                    if (dlen)
                        d[dlen++] = ' ';
                    memcpy(d + dlen, use, n);
                    dlen += n;
                    d[dlen] = 0;
                }
            }
            p = q;
        } else {
            if (len + 2 > cap) {
                cap = 2 * cap;
                t = (char *)realloc(t, cap);
            }
            t[len++] = *p++;
        }
    }
    t[len] = 0;
    pc->text = t;
    pc->deps = d;
}

static int by_name(const void *a, const void *b) { return strcmp(((const Piece *)a)->name, ((const Piece *)b)->name); }

int main(int argc, char **argv)
{
    FILE *f;
    int i, k;
    size_t j, n;
    if (argc != 2) {
        fprintf(stderr, "usage: mklib OUT.c\n");
        return 1;
    }
    for (i = 0; i < (int)(sizeof units / sizeof units[0]); i++) {
        k = nmods;
        units[i]();
        if (nmods != k + 1) {
            fprintf(stderr, "mklib: unit %d did not launch\n", i);
            return 1;
        }
        take(mods[k], i);
    }
    for (i = 0; i < npieces; i++)
        finish(&pieces[i]);
    qsort(pieces, (size_t)npieces, sizeof *pieces, by_name);
    /* everything that a piece uses must be a piece */
    for (i = 0; i < npieces; i++) {
        const char *d = pieces[i].deps, *de;
        for (; *d; d = *de ? de + 1 : de) {
            de = strchr(d, ' ');
            if (!de)
                de = d + strlen(d);
            for (k = 0; k < npieces; k++)
                if (strlen(pieces[k].name) == (size_t)(de - d) && memcmp(pieces[k].name, d, (size_t)(de - d)) == 0)
                    break;
            if (k == npieces) {
                fprintf(stderr, "mklib: %s uses %.*s, which the library does not have\n", pieces[i].name, (int)(de - d), d);
                return 1;
            }
        }
    }
    for (i = 1; i < npieces; i++)
        if (strcmp(pieces[i - 1].name, pieces[i].name) == 0) {
            fprintf(stderr, "mklib: %s is defined twice\n", pieces[i].name);
            return 1;
        }
    f = fopen(argv[1], "w");
    if (!f) {
        fprintf(stderr, "mklib: cannot write %s\n", argv[1]);
        return 1;
    }
    fprintf(f, "/* made by mklib from the device library's sources (cuda/hw/dev): do not edit */\n#include \"hw.h\"\n\n");
    for (i = 0; i < npieces; i++) {
        /* (as numbers: a string literal may not be this long) */
        n = strlen(pieces[i].text);
        fprintf(f, "static const char t%d[] = {", i);
        for (j = 0; j <= n; j++)
            fprintf(f, "%s%d%s", j % 32 == 0 ? "\n    " : "", pieces[i].text[j], j < n ? "," : "");
        fprintf(f, "\n};\n");
    }
    fprintf(f, "\nconst struct HwLibPiece hw_lib[] = {\n");
    for (i = 0; i < npieces; i++)
        fprintf(f, "    {\"%s\", t%d, \"%s\"}%s\n", pieces[i].name, i, pieces[i].deps, i + 1 < npieces ? "," : "");
    fprintf(f, "};\nconst int hw_nlib = %d;\n", npieces);
    if (fclose(f)) {
        fprintf(stderr, "mklib: cannot write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
