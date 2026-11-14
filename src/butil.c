/* The small tools that go with an ELF linker: ar, nm, objcopy and strip. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 r16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 r32(const u8 *p) { return r16(p) | r16(p + 2) << 16; }
static u64 r64(const u8 *p) { return (u64)r32(p) | (u64)r32(p + 4) << 32; }
static void w16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static void w64(u8 *p, u64 v)
{
    int k;
    for (k = 0; k < 8; k++)
        p[k] = (u8)(v >> 8 * k);
}

static char *copy_str(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- ar ---- */

typedef struct Member {
    char *name; /* as stored; for a thin archive, a path relative to the archive */
    const u8 *data;
    size_t len;
} Member;

static Member *members;
static u32 nmembers;

static void add_member(char *name, const u8 *data, size_t len)
{
    u32 k;
    for (k = 0; k < nmembers; k++)
        if (strcmp(members[k].name, name) == 0)
            break;
    if (k == nmembers) {
        if ((nmembers & (nmembers + 1)) == 0)
            members = (Member *)xrealloc(members, (2 * nmembers + 2) * sizeof(Member));
        nmembers++;
    }
    members[k].name = name;
    members[k].data = data;
    members[k].len = len;
}

/* Calls fn for each member of an archive image; data is null for the members of a thin one. */
static void ar_each(const u8 *d, size_t len, void (*fn)(char *name, const u8 *data, size_t len))
{
    int thin = memcmp(d, "!<thin>\n", 8) == 0;
    const char *longnames = 0;
    size_t pos = 8;
    while (pos + 60 <= len) {
        const u8 *h = d + pos, *body = h + 60;
        size_t size = (size_t)strtoul((const char *)h + 48, 0, 10), skip = size;
        char nm[17], *name = 0;
        memcpy(nm, h, 16);
        nm[16] = 0;
        pos += 60;
        if (nm[0] == '/' && nm[1] == '/') {
            longnames = (const char *)body;
        } else if (nm[0] == '/' && (nm[1] == ' ' || nm[1] == 'S')) {
            ; /* a symbol index */
        } else if (nm[0] == '#' && nm[1] == '1' && nm[2] == '/') {
            size_t l = (size_t)strtoul(nm + 3, 0, 10);
            name = copy_str((const char *)body, strlen((const char *)body) < l ? strlen((const char *)body) : l);
            body += l;
            size -= l;
            if (strncmp(name, "__.SYMDEF", 9) == 0)
                name = 0;
        } else if (nm[0] == '/') {
            const char *s = longnames + strtoul(nm + 1, 0, 10), *e = s;
            while (*e != '\n' && !(*e == '/' && e[1] == '\n'))
                e++;
            name = copy_str(s, (size_t)(e - s));
        } else {
            size_t k = 16;
            while (k > 0 && nm[k - 1] == ' ')
                k--;
            if (k > 0 && nm[k - 1] == '/')
                k--;
            name = copy_str(nm, k);
        }
        if (name)
            fn(name, thin ? 0 : body, size);
        if (!thin || !name)
            pos += skip + (skip & 1);
    }
}

/* The path of `file` as seen from the directory holding `archive`. */
static char *relative_to(const char *archive, const char *file)
{
    const char *a = archive, *f = file, *slash;
    Buf b;
    if (file[0] == '/' && archive[0] != '/')
        return copy_str(file, strlen(file));
    for (;;) { /* drop the leading directories they share */
        const char *as = strchr(a, '/'), *fs = strchr(f, '/');
        if (!as || !fs || as - a != fs - f || memcmp(a, f, (size_t)(as - a)) != 0)
            break;
        a = as + 1;
        f = fs + 1;
    }
    memset(&b, 0, sizeof b);
    for (slash = strchr(a, '/'); slash; slash = strchr(slash + 1, '/'))
        buf_put(&b, "../", 3);
    buf_put(&b, f, strlen(f) + 1);
    return (char *)b.p;
}

static char *join_dir(const char *archive, const char *member)
{
    const char *slash = strrchr(archive, '/');
    size_t n = slash && member[0] != '/' ? (size_t)(slash - archive + 1) : 0;
    char *p = (char *)xmalloc(n + strlen(member) + 1);
    memcpy(p, archive, n);
    strcpy(p + n, member);
    return p;
}

static const char *ar_out, *ar_inner;
static int ar_thin;

static void add_existing(char *name, const u8 *data, size_t len) { add_member(name, data, len); }

static void add_flattened(char *name, const u8 *data, size_t len)
{
    char *path = join_dir(ar_inner, name);
    (void)data;
    add_member(relative_to(ar_out, path), 0, len);
}

static void list_member(char *name, const u8 *data, size_t len)
{
    (void)data;
    (void)len;
    puts(name);
}

static void extract_member(char *name, const u8 *data, size_t len)
{
    const char *base = strrchr(name, '/');
    if (data)
        os_write_file(base ? base + 1 : name, data, len, 0);
}

int elf_ar_main(int argc, char **argv)
{
    const char *keys;
    int op = 0, i;
    size_t len;
    u8 *d;
    Buf out, names;
    u32 k;
    if (argc < 2)
        die("usage: ar [-]{r|q|t|x}[cDPsST] archive member...");
    for (keys = argv[0]; *keys; keys++) {
        if (strchr("rqtx", *keys))
            op = *keys;
        else if (*keys == 'T')
            ar_thin = 1;
        else if (!strchr("-cDPsSuUv", *keys))
            die("ar: unknown key letter '%c'", *keys);
    }
    ar_out = argv[1];
    d = os_read_file(ar_out, &len);
    if (op == 't' || op == 'x') {
        if (!d || len < 8)
            die("ar: cannot read %s", ar_out);
        ar_each(d, len, op == 't' ? list_member : extract_member);
        return 0;
    }
    if (!op)
        die("ar: no operation given");
    if (d && len >= 8) {
        if (memcmp(d, "!<thin>\n", 8) == 0)
            ar_thin = 1;
        ar_each(d, len, add_existing);
    }
    for (i = 2; i < argc; i++) {
        size_t flen;
        u8 *fd = os_read_file(argv[i], &flen);
        const char *base = strrchr(argv[i], '/');
        if (!fd)
            die("ar: cannot read %s", argv[i]);
        if (ar_thin && flen >= 8 && memcmp(fd, "!<thin>\n", 8) == 0) {
            ar_inner = argv[i]; /* a thin archive in a thin archive: its members join this one */
            ar_each(fd, flen, add_flattened);
        } else if (ar_thin) {
            add_member(relative_to(ar_out, argv[i]), 0, flen);
        } else {
            base = base ? base + 1 : argv[i];
            add_member(copy_str(base, strlen(base)), fd, flen);
        }
    }
    memset(&out, 0, sizeof out);
    memset(&names, 0, sizeof names);
    buf_put(&out, ar_thin ? "!<thin>\n" : "!<arch>\n", 8);
    for (k = 0; k < nmembers; k++)
        if (ar_thin || strlen(members[k].name) > 15 || strchr(members[k].name, ' ')) {
            buf_put(&names, members[k].name, strlen(members[k].name));
            buf_put(&names, "/\n", 2);
        }
    if (!ar_thin) {
        /* the symbol index GNU ld wants: a count, the offset of the member that defines each global
           symbol (big-endian words), then the names */
        Buf who, syms;
        u32 count = 0, j;
        size_t at, idxlen;
        char hdr[96]; /* 60 used; room for what gcc -Wformat-overflow assumes the numbers can be */
        memset(&who, 0, sizeof who);
        memset(&syms, 0, sizeof syms);
        for (k = 0; k < nmembers; k++) {
            const u8 *e = members[k].data, *sh, *st = 0;
            size_t elen = members[k].len, nsh, n, stn = 0;
            const char *str = 0;
            if (!e || elen < 64 || memcmp(e, "\177ELF\2\1", 6) != 0)
                continue;
            sh = e + r64(e + 0x28);
            nsh = (size_t)(e[0x3c] | e[0x3d] << 8);
            for (n = 0; n < nsh; n++)
                if (r32(sh + n * 64 + 4) == 2) { /* SHT_SYMTAB */
                    st = e + r64(sh + n * 64 + 0x18);
                    stn = (size_t)(r64(sh + n * 64 + 0x20) / 24);
                    str = (const char *)e + r64(sh + (size_t)r32(sh + n * 64 + 0x28) * 64 + 0x18);
                }
            for (n = 1; n < stn; n++) {
                const u8 *y = st + n * 24;
                u32 ndx = (u32)(y[6] | y[7] << 8);
                if (y[4] >> 4 == 0 || ndx == 0)
                    continue; /* local, or not defined here */
                buf_u32(&who, k);
                buf_put(&syms, str + r32(y), strlen(str + r32(y)) + 1);
                count++;
            }
        }
        idxlen = 4 + 4 * (size_t)count + syms.len;
        idxlen += idxlen & 1;
        at = 8 + 60 + idxlen + (names.len ? 60 + names.len + (names.len & 1) : 0);
        sprintf(hdr, "%-16s%-12d%-6d%-6d%-8s%-10lu`\n", "/", 0, 0, 0, "0", (unsigned long)idxlen);
        buf_put(&out, hdr, 60);
        buf_u8(&out, count >> 24); buf_u8(&out, count >> 16 & 255); buf_u8(&out, count >> 8 & 255); buf_u8(&out, count & 255);
        for (j = 0, k = 0; k < nmembers; k++) {
            for (; j < count && r32(who.p + 4 * j) == k; j++) {
                buf_u8(&out, (u32)(at >> 24)); buf_u8(&out, (u32)(at >> 16 & 255));
                buf_u8(&out, (u32)(at >> 8 & 255)); buf_u8(&out, (u32)(at & 255));
            }
            at += 60 + members[k].len + (members[k].len & 1);
        }
        buf_put(&out, syms.p, syms.len);
        if (out.len & 1)
            buf_u8(&out, '\n');
        buf_free(&who);
        buf_free(&syms);
    }
    if (names.len) {
        char hdr[96]; /* 60 used; room for what gcc -Wformat-overflow assumes the numbers can be */
        if (names.len & 1)
            buf_u8(&names, '\n');
        sprintf(hdr, "%-16s%-12d%-6d%-6d%-8s%-10lu`\n", "//", 0, 0, 0, "0", (unsigned long)names.len);
        buf_put(&out, hdr, 60);
        buf_put(&out, names.p, names.len);
    }
    names.len = 0;
    for (k = 0; k < nmembers; k++) {
        Member *m = &members[k];
        char hdr[96], nm[32]; /* hdr: 60 used, see above */
        if (ar_thin || strlen(m->name) > 15 || strchr(m->name, ' ')) {
            sprintf(nm, "/%lu", (unsigned long)names.len);
            names.len += strlen(m->name) + 2;
        } else {
            sprintf(nm, "%s/", m->name);
        }
        sprintf(hdr, "%-16s%-12d%-6d%-6d%-8s%-10lu`\n", nm, 0, 0, 0, "644", (unsigned long)m->len);
        buf_put(&out, hdr, 60);
        if (!ar_thin) {
            buf_put(&out, m->data, m->len);
            if (out.len & 1)
                buf_u8(&out, '\n');
        }
    }
    os_write_file(ar_out, out.p, out.len, 0);
    return 0;
}

/* ---- reading an ELF file ---- */

typedef struct Elf {
    const u8 *d;
    size_t len;
    const u8 *sh;
    u32 shnum, shstrndx;
    const char *shstr;
} Elf;

static void elf_open(Elf *e, const char *path)
{
    e->d = os_read_file(path, &e->len);
    if (!e->d)
        die("cannot read %s", path);
    if (e->len < 64 || memcmp(e->d, "\177ELF\2\1", 6) != 0)
        die("%s: not a 64-bit little-endian ELF file", path);
    e->sh = e->d + r64(e->d + 0x28);
    e->shnum = r16(e->d + 0x3c);
    e->shstrndx = r16(e->d + 0x3e);
    e->shstr = e->shnum ? (const char *)e->d + r64(e->sh + 64 * e->shstrndx + 24) : "";
}

#define SH(e, i) ((e)->sh + 64 * (i))
#define SH_NAME(e, i) ((e)->shstr + r32(SH(e, i)))
#define SH_TYPE(e, i) r32(SH(e, i) + 4)
#define SH_FLAGS(e, i) r64(SH(e, i) + 8)
#define SH_ADDR(e, i) r64(SH(e, i) + 16)
#define SH_OFF(e, i) r64(SH(e, i) + 24)
#define SH_SIZE(e, i) r64(SH(e, i) + 32)

/* ---- nm ---- */

typedef struct NmSym {
    const char *name;
    u64 value;
    int letter, undef;
    u32 order;
} NmSym;

static int nm_numeric;

static int cmp_nm(const void *a, const void *b)
{
    const NmSym *x = (const NmSym *)a, *y = (const NmSym *)b;
    int c;
    if (nm_numeric) {
        if (x->undef != y->undef)
            return y->undef - x->undef;
        if (x->value != y->value)
            return x->value < y->value ? -1 : 1;
    } else if ((c = strcmp(x->name, y->name)) != 0)
        return c;
    return x->order < y->order ? -1 : x->order > y->order;
}

int elf_nm_main(int argc, char **argv)
{
    int i, nosort = 0, externs = 0, undef_only = 0, defined_only = 0, dynamic = 0, nfiles = 0;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-')
            nfiles++;
        else if (strcmp(a, "--defined-only") == 0)
            defined_only = 1;
        else if (strcmp(a, "--undefined-only") == 0)
            undef_only = 1;
        else if (strcmp(a, "--extern-only") == 0)
            externs = 1;
        else if (strcmp(a, "--numeric-sort") == 0)
            nm_numeric = 1;
        else if (strcmp(a, "--no-sort") == 0)
            nosort = 1;
        else if (strcmp(a, "--dynamic") == 0)
            dynamic = 1;
        else if (strcmp(a, "--print-size") == 0)
            ;
        else if (a[1] == '-')
            die("nm: unknown option %s", a);
        else
            for (a++; *a; a++)
                switch (*a) {
                case 'n': case 'v': nm_numeric = 1; break;
                case 'p': nosort = 1; break;
                case 'g': externs = 1; break;
                case 'u': undef_only = 1; break;
                case 'D': dynamic = 1; break;
                case 'S': case 'B': case 'a': break;
                default: die("nm: unknown option -%c", *a);
                }
    }
    for (i = 0; i < argc; i++) {
        Elf e;
        NmSym *syms = 0;
        u32 n = 0, s, k;
        if (argv[i][0] == '-')
            continue;
        elf_open(&e, argv[i]);
        if (nfiles > 1)
            printf("\n%s:\n", argv[i]);
        for (s = 0; s < e.shnum; s++) {
            const u8 *st;
            const char *str;
            u32 count;
            if (SH_TYPE(&e, s) != (dynamic ? 11U : 2U))
                continue;
            st = e.d + SH_OFF(&e, s);
            str = (const char *)e.d + SH_OFF(&e, r32(SH(&e, s) + 40));
            count = (u32)(SH_SIZE(&e, s) / 24);
            syms = (NmSym *)xmalloc((count + 1) * sizeof(NmSym));
            for (k = 1; k < count; k++) {
                const u8 *se = st + 24 * k;
                u32 shndx = r16(se + 6), bind = se[4] >> 4, type = se[4] & 15;
                int c;
                if (type == 3 || type == 4)
                    continue;
                if (shndx == 0)
                    c = 'U';
                else if (shndx == 0xfff1)
                    c = 'a';
                else if (shndx == 0xfff2)
                    c = 'c';
                else if (shndx >= e.shnum)
                    c = '?';
                else {
                    u64 fl = SH_FLAGS(&e, shndx);
                    c = !(fl & 2) ? 'n' : (fl & 4) ? 't' : SH_TYPE(&e, shndx) == 8 ? 'b' : (fl & 1) ? 'd' : 'r';
                }
                if (bind == 2) {
                    c = type == 1 ? (shndx ? 'V' : 'v') : (shndx ? 'W' : 'w');
                } else if (bind == 1 && c != 'U')
                    c += 'A' - 'a';
                else if (c == 'c')
                    c = 'C';
                if ((externs && bind == 0) || (undef_only && shndx) || (defined_only && !shndx))
                    continue;
                syms[n].name = str + r32(se);
                syms[n].value = r64(se + 8);
                syms[n].letter = c;
                syms[n].undef = shndx == 0;
                syms[n].order = n;
                n++;
            }
            if (!nosort)
                qsort(syms, n, sizeof(NmSym), cmp_nm);
            for (k = 0; k < n; k++) {
                if (syms[k].undef)
                    printf("%16s %c %s\n", "", syms[k].letter, syms[k].name);
                else
                    printf("%016lx %c %s\n", (unsigned long)syms[k].value, syms[k].letter, syms[k].name);
            }
            break;
        }
    }
    return 0;
}

/* ---- objcopy and strip ---- */

static const char *removed[64];
static int nremoved;
static const char *only[64]; /* -j: nothing but these */
static int nonly;

static int is_removed(const char *name)
{
    int k;
    for (k = 0; k < nremoved; k++)
        if (strcmp(removed[k], name) == 0)
            return 1;
    for (k = 0; k < nonly; k++)
        if (strcmp(only[k], name) == 0)
            return 0;
    return nonly != 0;
}

static void write_binary(Elf *e, const char *out_path)
{
    u64 lo = ~(u64)0, hi = 0;
    u32 s;
    Buf out;
    memset(&out, 0, sizeof out);
    for (s = 1; s < e->shnum; s++) {
        if (!(SH_FLAGS(e, s) & 2) || SH_TYPE(e, s) == 8 || !SH_SIZE(e, s) || is_removed(SH_NAME(e, s)))
            continue;
        if (SH_ADDR(e, s) < lo)
            lo = SH_ADDR(e, s);
        if (SH_ADDR(e, s) + SH_SIZE(e, s) > hi)
            hi = SH_ADDR(e, s) + SH_SIZE(e, s);
    }
    if (hi > lo) {
        buf_zero(&out, (size_t)(hi - lo));
        for (s = 1; s < e->shnum; s++) {
            if (!(SH_FLAGS(e, s) & 2) || SH_TYPE(e, s) == 8 || !SH_SIZE(e, s) || is_removed(SH_NAME(e, s)))
                continue;
            memcpy(out.p + (SH_ADDR(e, s) - lo), e->d + SH_OFF(e, s), (size_t)SH_SIZE(e, s));
        }
    }
    os_write_file(out_path, out.p, out.len, 0);
}

/* Writes the file again without some sections; what is kept stays where it was. */
static void write_stripped(Elf *e, const char *out_path, int strip_all)
{
    u32 *map = (u32 *)xmalloc((e->shnum + 1) * sizeof(u32)), s, n = 1, phnum = r16(e->d + 0x38);
    u64 end = 64, phoff = r64(e->d + 0x20), shoff;
    Buf out, shstr;
    u8 *h;
    int exec = r16(e->d + 16) != 1;
    memset(&out, 0, sizeof out);
    memset(&shstr, 0, sizeof shstr);
    buf_u8(&shstr, 0);
    if (phnum && phoff + 56 * phnum > end)
        end = phoff + 56 * phnum;
    for (s = 0; s < phnum; s++) {
        const u8 *ph = e->d + phoff + 56 * s;
        if (r64(ph + 8) + r64(ph + 32) > end)
            end = r64(ph + 8) + r64(ph + 32);
    }
    map[0] = 0;
    for (s = 1; s < e->shnum; s++) {
        u32 type = SH_TYPE(e, s);
        int alloc = (SH_FLAGS(e, s) & 2) != 0;
        int drop = is_removed(SH_NAME(e, s)) || s == e->shstrndx ||
                   (strip_all && !alloc && (type == 2 || type == 3 || type == 4));
        map[s] = drop ? 0 : n++;
        if (!drop && type != 8 && SH_OFF(e, s) + SH_SIZE(e, s) > end)
            end = SH_OFF(e, s) + SH_SIZE(e, s);
    }
    if (end > e->len)
        end = e->len;
    buf_put(&out, e->d, (size_t)end);
    {
        u64 at = out.len;
        u32 *names = (u32 *)xmalloc((e->shnum + 1) * sizeof(u32));
        for (s = 1; s < e->shnum; s++)
            if (map[s]) {
                names[s] = (u32)shstr.len;
                buf_put(&shstr, SH_NAME(e, s), strlen(SH_NAME(e, s)) + 1);
            }
        names[0] = (u32)shstr.len;
        buf_put(&shstr, ".shstrtab", 10);
        buf_put(&out, shstr.p, shstr.len);
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        shoff = out.len;
        buf_zero(&out, 64 * (n + 1));
        for (s = 1; s < e->shnum; s++) {
            u32 link, info;
            if (!map[s])
                continue;
            h = out.p + shoff + 64 * map[s];
            memcpy(h, SH(e, s), 64);
            h[0] = (u8)names[s];
            h[1] = (u8)(names[s] >> 8);
            h[2] = (u8)(names[s] >> 16);
            h[3] = (u8)(names[s] >> 24);
            link = r32(h + 40);
            info = r32(h + 44);
            if (link < e->shnum)
                w16(h + 40, map[link]), w16(h + 42, map[link] >> 16);
            if ((SH_TYPE(e, s) == 4 || (SH_FLAGS(e, s) & 0x40)) && info < e->shnum)
                w16(h + 44, map[info]), w16(h + 46, map[info] >> 16);
        }
        h = out.p + shoff + 64 * n;
        h[0] = (u8)names[0];
        h[1] = (u8)(names[0] >> 8);
        h[2] = (u8)(names[0] >> 16);
        h[3] = (u8)(names[0] >> 24);
        h[4] = 3;
        w64(h + 24, at);
        w64(h + 32, shstr.len);
        w64(h + 48, 1);
    }
    w64(out.p + 0x28, shoff);
    w16(out.p + 0x3c, n + 1);
    w16(out.p + 0x3e, n);
    os_write_file(out_path, out.p, out.len, exec);
}

int elf_objcopy_main(int argc, char **argv, int strip)
{
    const char *in = 0, *out = 0, *format = 0;
    int i, strip_all = strip, changes = strip;
    Elf e;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-O") == 0 && i + 1 < argc)
            format = argv[++i];
        else if (strncmp(a, "--output-target=", 16) == 0)
            format = a + 16;
        else if ((strcmp(a, "-R") == 0 || strcmp(a, "--remove-section") == 0) && i + 1 < argc && nremoved < 64)
            removed[nremoved++] = argv[++i];
        else if (strncmp(a, "--remove-section=", 17) == 0 && nremoved < 64)
            removed[nremoved++] = a + 17;
        else if ((strcmp(a, "-j") == 0 || strcmp(a, "--only-section") == 0) && i + 1 < argc && nonly < 64)
            only[nonly++] = argv[++i];
        else if (strncmp(a, "--only-section=", 15) == 0 && nonly < 64)
            only[nonly++] = a + 15;
        else if (strcmp(a, "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(a, "-S") == 0 || strcmp(a, "-s") == 0 || strcmp(a, "--strip-all") == 0)
            strip_all = changes = 1;
        else if (strcmp(a, "-g") == 0 || strcmp(a, "--strip-debug") == 0 || strcmp(a, "-d") == 0 ||
                 strcmp(a, "--strip-unneeded") == 0 || strcmp(a, "-x") == 0 || strcmp(a, "-X") == 0 ||
                 strcmp(a, "-p") == 0 || strcmp(a, "-D") == 0)
            ; /* mcc writes no debugging information, and nothing here depends on the rest */
        else if (strcmp(a, "-I") == 0 || strcmp(a, "-F") == 0 || strcmp(a, "-B") == 0)
            i++;
        else if (a[0] == '-')
            die("%s: unknown option %s", strip ? "strip" : "objcopy", a);
        else if (!in)
            in = a;
        else if (!strip && !out)
            out = a;
        else
            die("%s: too many files", strip ? "strip" : "objcopy");
    }
    if (!in)
        die("%s: no input file", strip ? "strip" : "objcopy");
    if (!out)
        out = in;
    elf_open(&e, in);
    if (format && strcmp(format, "binary") == 0)
        write_binary(&e, out);
    else if (changes || nremoved || nonly)
        write_stripped(&e, out, strip_all);
    else
        os_write_file(out, e.d, e.len, r16(e.d + 16) != 1);
    return 0;
}

/* objdump -R: the relocations left for the loader, which is all the kernel's makefiles ask of it. */
int elf_objdump_main(int argc, char **argv)
{
    int i, dyn = 0;
    for (i = 0; i < argc; i++)
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--dynamic-reloc") == 0)
            dyn = 1;
        else if (argv[i][0] == '-')
            die("objdump: only -R is supported, not %s", argv[i]);
    if (!dyn)
        die("objdump: only -R is supported");
    for (i = 0; i < argc; i++) {
        Elf e;
        u32 s;
        if (argv[i][0] == '-')
            continue;
        elf_open(&e, argv[i]);
        printf("\n%s:     file format elf64-littleaarch64\n\nDYNAMIC RELOCATION RECORDS\n", argv[i]);
        for (s = 1; s < e.shnum; s++) {
            u64 k, n;
            if (SH_TYPE(&e, s) != 4 || !(SH_FLAGS(&e, s) & 2))
                continue;
            n = SH_SIZE(&e, s) / 24;
            for (k = 0; k < n; k++) {
                const u8 *r = e.d + SH_OFF(&e, s) + 24 * k;
                u32 type = r32(r + 8);
                const char *name = type == 1027 ? "R_AARCH64_RELATIVE" : type == 1026 ? "R_AARCH64_JUMP_SLOT" :
                                   type == 1025 ? "R_AARCH64_GLOB_DAT" : type == 257 ? "R_AARCH64_ABS64" : "R_AARCH64_?";
                printf("%016lx %-24s *ABS*+0x%lx\n", (unsigned long)r64(r), name, (unsigned long)r64(r + 16));
            }
        }
    }
    return 0;
}
