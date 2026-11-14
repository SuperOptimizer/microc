/* ELF linker for aarch64-linux: relocatable objects and archives in, a
   static executable out. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x10000
#define BASE 0x400000

typedef struct File File;
typedef struct Sec Sec;
typedef struct Sym Sym;
typedef struct Out Out;

struct Sec {
    File *f;
    const char *name;
    u32 type;
    u64 flags, size, align;
    const u8 *data;
    const u8 *rela; /* its relocations, 24 bytes each */
    u32 nrela;
    Out *out;
    u64 off; /* within out */
    u64 entsize;
    u8 taken; /* a script has placed or discarded it */
};

enum { UNDEF, DEFINED, COMMON, ABSOLUTE };

struct Sym {
    const char *name;
    File *f;
    Sec *sec;
    u64 value, size;
    u8 def, weak, type, vis;
    u8 used; /* something refers to it */
    u32 idx; /* in the symbol table of relocatable output; in the names of a shared object */
    Out *osec; /* given its value by a script inside this output section */
    u32 got; /* 1 + its place in the table of addresses, or 0 */
    Sym *next;
};

struct File {
    const char *name;
    const u8 *d;
    size_t len;
    u32 nsec;
    Sec *secs;
    const u8 *symtab;
    const char *strtab;
    u32 nsyms, first_global;
    Sym **gsyms; /* by symbol table index, from first_global on */
    File *next;
};

struct Out {
    const char *name;
    u32 type;
    u64 flags, addr, off, size, align;
    Sec **secs;
    u32 n, cap;
    u32 shndx, rank;
    u64 entsize;
    u32 nrela;
};

typedef struct Archive {
    const char *name;
    const u8 *d;
    size_t len;
    int thin;
    struct Archive *next;
} Archive;

#define NHASH 16384
static Sym *hash[NHASH];
static File *files, **files_tail = &files;
static Archive *archives, **archives_tail = &archives;
static Out **outs;
static u32 nouts, capouts;
static File synth; /* what the linker itself adds: common symbols, the table of addresses */
static Sec *got_sec, *common_sec;
static u8 *got_data;
static u32 ngot;
static Sym **got_syms;

static u32 r16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 r32(const u8 *p) { return r16(p) | r16(p + 2) << 16; }
static u64 r64(const u8 *p) { return (u64)r32(p) | (u64)r32(p + 4) << 32; }
static void w32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static void w64(u8 *p, u64 v)
{
    w32(p, (u32)v);
    w32(p + 4, (u32)(v >> 32));
}

static Sym *lookup(const char *name, int make)
{
    u32 h = 2166136261U;
    const char *p;
    Sym *s;
    for (p = name; *p; p++)
        h = (h ^ (u8)*p) * 16777619U;
    for (s = hash[h % NHASH]; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    if (!make)
        return 0;
    s = (Sym *)xmalloc(sizeof(Sym));
    memset(s, 0, sizeof *s);
    s->name = name;
    s->next = hash[h % NHASH];
    hash[h % NHASH] = s;
    return s;
}

/* ---- reading ---- */

static u32 eld_machine; /* e_machine of the objects read so far: 183 aarch64, 62 x86-64; 0 before the first */

static u32 machine(void)
{
    return eld_machine ? eld_machine : opt_arch == ARCH_X64 ? 62 : 183;
}

static void load_object(const char *name, const u8 *d, size_t len)
{
    File *f;
    u64 shoff;
    u32 shnum, shstrndx, i;
    const u8 *sh;
    const char *shstr;
    if (len < 64 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2 || r16(d + 16) != 1 ||
        (r16(d + 18) != 183 && r16(d + 18) != 62))
        die("%s: not an aarch64 or x86-64 ELF object", name);
    if (eld_machine && eld_machine != r16(d + 18))
        die("%s: an object for another processor than the ones before it", name);
    eld_machine = r16(d + 18);
    f = (File *)xmalloc(sizeof(File));
    memset(f, 0, sizeof *f);
    f->name = name;
    f->d = d;
    f->len = len;
    shoff = r64(d + 0x28);
    shnum = r16(d + 0x3c);
    shstrndx = r16(d + 0x3e);
    sh = d + shoff;
    shstr = (const char *)d + r64(sh + 64 * shstrndx + 24);
    f->nsec = shnum;
    f->secs = (Sec *)xmalloc((shnum + 1) * sizeof(Sec));
    memset(f->secs, 0, (shnum + 1) * sizeof(Sec));
    for (i = 0; i < shnum; i++) {
        const u8 *h = sh + 64 * i;
        Sec *s = &f->secs[i];
        s->f = f;
        s->name = shstr + r32(h);
        s->type = r32(h + 4);
        s->flags = r64(h + 8);
        s->data = d + r64(h + 24);
        s->size = r64(h + 32);
        s->align = r64(h + 48) ? r64(h + 48) : 1;
        s->entsize = r64(h + 56);
        if (s->type == 2) { /* SHT_SYMTAB */
            f->symtab = s->data;
            f->nsyms = (u32)(s->size / 24);
            f->first_global = r32(h + 44);
            f->strtab = (const char *)d + r64(sh + 64 * r32(h + 40) + 24);
        }
    }
    for (i = 0; i < shnum; i++) {
        const u8 *h = sh + 64 * i;
        if (r32(h + 4) == 4) { /* SHT_RELA */
            Sec *t = &f->secs[r32(h + 44)];
            t->rela = d + r64(h + 24);
            t->nrela = (u32)(r64(h + 32) / 24);
        }
    }
    f->gsyms = (Sym **)xmalloc((f->nsyms + 1) * sizeof(Sym *));
    memset(f->gsyms, 0, (f->nsyms + 1) * sizeof(Sym *));
    for (i = f->first_global; i < f->nsyms; i++) {
        const u8 *e = f->symtab + 24 * i;
        u32 shndx = r16(e + 6), bind = e[4] >> 4;
        Sym *s = lookup(f->strtab + r32(e), 1);
        int weak = bind == 2;
        f->gsyms[i] = s;
        if (shndx == 0) {
            if (s->def == UNDEF && !s->f)
                s->weak = (u8)weak, s->f = f;
            else if (s->def == UNDEF && !weak)
                s->weak = 0;
            continue;
        }
        if (s->def == DEFINED || s->def == ABSOLUTE) {
            if (weak || shndx == 0xfff2)
                continue;
            if (!s->weak)
                die("duplicate symbol %s in %s and %s", s->name, s->f ? s->f->name : "?", name);
        } else if (s->def == COMMON && shndx == 0xfff2) {
            if (r64(e + 16) > s->size)
                s->size = r64(e + 16);
            continue;
        }
        s->f = f;
        s->weak = (u8)weak;
        s->type = e[4] & 15;
        s->vis = e[5] & 3;
        s->size = r64(e + 16);
        s->value = r64(e + 8);
        if (shndx == 0xfff2) {
            s->def = COMMON; /* value is the alignment */
        } else if (shndx == 0xfff1) {
            s->def = ABSOLUTE;
        } else {
            s->def = DEFINED;
            s->sec = &f->secs[shndx];
        }
    }
    *files_tail = f;
    files_tail = &f->next;
}

static char *dir_join(const char *archive, const char *member)
{
    const char *slash = strrchr(archive, '/');
    size_t n = slash && member[0] != '/' ? (size_t)(slash - archive + 1) : 0;
    char *p = (char *)xmalloc(n + strlen(member) + 1);
    memcpy(p, archive, n);
    strcpy(p + n, member);
    return p;
}

/* Loads the members of an archive that define something still undefined, or all of them. */
static void each_member(Archive *a, int whole)
{
    size_t pos = 8;
    const char *longnames = 0;
    while (pos + 60 <= a->len) {
        const u8 *h = a->d + pos;
        char nm[17], *name;
        size_t size = (size_t)strtoul((const char *)h + 48, 0, 10), k;
        const u8 *body = h + 60;
        int special = 0;
        memcpy(nm, h, 16);
        nm[16] = 0;
        pos += 60;
        if (nm[0] == '/' && (nm[1] == ' ' || nm[1] == '/' || nm[1] == 'S')) {
            special = 1; /* the symbol table, or the table of long names */
            if (nm[1] == '/')
                longnames = (const char *)body;
            name = 0;
        } else if (nm[0] == '#' && nm[1] == '1' && nm[2] == '/') { /* BSD: the name starts the body */
            size_t l = (size_t)strtoul(nm + 3, 0, 10);
            name = (char *)xmalloc(l + 1);
            memcpy(name, body, l);
            name[l] = 0;
            if (strncmp(name, "__.SYMDEF", 9) == 0)
                special = 1;
            body += l;
            size -= l;
            pos += l;
        } else if (nm[0] == '/') {
            const char *s = longnames + strtoul(nm + 1, 0, 10), *e = s;
            while (*e != '\n' && !(*e == '/' && e[1] == '\n'))
                e++;
            name = (char *)xmalloc((size_t)(e - s) + 1);
            memcpy(name, s, (size_t)(e - s));
            name[e - s] = 0;
        } else {
            for (k = 16; k > 0 && (nm[k - 1] == ' ' || nm[k - 1] == '/'); k--)
                nm[k - 1] = 0;
            name = (char *)xmalloc(17);
            strcpy(name, nm);
        }
        if (!special) {
            const u8 *d = body;
            size_t len = size;
            char *full;
            if (a->thin) {
                full = dir_join(a->name, name);
                d = 0;
            } else {
                full = (char *)xmalloc(strlen(a->name) + strlen(name) + 3);
                sprintf(full, "%s(%s)", a->name, name);
            }
            /* is it wanted? */
            {
                int want = whole;
                const u8 *md = d;
                size_t mlen = len;
                if (!md) {
                    md = os_read_file(full, &mlen);
                    if (!md)
                        die("cannot read %s, a member of %s", full, a->name);
                }
                if (!want && mlen >= 64 && memcmp(md, "\177ELF", 4) == 0) {
                    /* does it define something that is still undefined? */
                    u64 shoff = r64(md + 0x28);
                    u32 shnum = r16(md + 0x3c), i, j;
                    for (i = 0; i < shnum && !want; i++) {
                        const u8 *sh = md + shoff + 64 * i;
                        if (r32(sh + 4) != 2)
                            continue;
                        {
                            const u8 *st = md + r64(sh + 24);
                            const char *str = (const char *)md + r64(md + shoff + 64 * r32(sh + 40) + 24);
                            u32 n = (u32)(r64(sh + 32) / 24);
                            for (j = r32(sh + 44); j < n && !want; j++) {
                                const u8 *e = st + 24 * j;
                                Sym *s;
                                if (r16(e + 6) == 0 || r16(e + 6) == 0xfff2)
                                    continue;
                                s = lookup(str + r32(e), 0);
                                if (s && s->def == UNDEF && s->f && !s->weak)
                                    want = 1;
                            }
                        }
                    }
                }
                if (want) {
                    File *f;
                    for (f = files; f; f = f->next)
                        if (strcmp(f->name, full) == 0)
                            break;
                    if (!f)
                        load_object(full, md, mlen);
                }
            }
        }
        if (!a->thin || special)
            pos += size + (size & 1);
    }
}

void eld_add(const char *name, const u8 *d, size_t len, int whole)
{
    if (len >= 8 && (memcmp(d, "!<arch>\n", 8) == 0 || memcmp(d, "!<thin>\n", 8) == 0)) {
        Archive *a = (Archive *)xmalloc(sizeof(Archive));
        a->name = name;
        a->d = d;
        a->len = len;
        a->thin = d[2] == 't';
        a->next = 0;
        if (whole) {
            each_member(a, 1);
        } else {
            *archives_tail = a;
            archives_tail = &a->next;
        }
        return;
    }
    load_object(name, d, len);
}

void eld_add_file(const char *path, int whole)
{
    size_t len;
    u8 *d = os_read_file(path, &len);
    if (!d)
        die("cannot read %s", path);
    eld_add(path, d, len, whole);
}

/* ---- layout ---- */

static const char *out_name(const char *n)
{
    static const char *const pre[] = {".text", ".rodata", ".data", ".bss", ".init_array", ".fini_array", ".tdata",
                                      ".tbss", 0};
    int k;
    for (k = 0; pre[k]; k++) {
        size_t l = strlen(pre[k]);
        if (strncmp(n, pre[k], l) == 0 && n[l] == '.')
            return pre[k];
    }
    return n;
}

static int relocatable; /* sections keep their names and all their flags */

static Out *out_for(Sec *s)
{
    const char *name = relocatable ? s->name : out_name(s->name);
    u64 mask = relocatable ? ~(u64)0x200 : 0x407;
    u32 k;
    Out *o;
    for (k = 0; k < nouts; k++)
        if (strcmp(outs[k]->name, name) == 0)
            break;
    if (k == nouts) {
        if (nouts == capouts) {
            capouts = capouts ? capouts * 2 : 32;
            outs = (Out **)xrealloc(outs, capouts * sizeof(Out *));
        }
        o = (Out *)xmalloc(sizeof(Out));
        memset(o, 0, sizeof *o);
        o->name = name;
        o->type = s->type;
        o->flags = s->flags & mask;
        o->entsize = s->entsize;
        o->align = 1;
        outs[nouts++] = o;
    }
    o = outs[k];
    if (o->type == 8 && s->type != 8)
        o->type = s->type;
    o->flags |= s->flags & mask;
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 16;
        o->secs = (Sec **)xrealloc(o->secs, o->cap * sizeof(Sec *));
    }
    o->secs[o->n++] = s;
    s->out = o;
    return o;
}

static int cmp_out(const void *a, const void *b)
{
    const Out *x = *(Out *const *)a, *y = *(Out *const *)b;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    return x->shndx < y->shndx ? -1 : x->shndx > y->shndx;
}

static u64 sym_addr(Sym *s)
{
    if (s->def == ABSOLUTE)
        return s->value;
    if (s->def != DEFINED || !s->sec || !s->sec->out)
        return s->def == DEFINED ? s->value : 0;
    return s->sec->out->addr + s->sec->off + s->value;
}

/* The address a symbol table entry of a file stands for; *gs is the global behind it, if any. */
static u64 ref_addr(File *f, u32 idx, Sym **gs)
{
    const u8 *e = f->symtab + 24 * idx;
    u32 shndx;
    *gs = 0;
    if (idx >= f->first_global) {
        *gs = f->gsyms[idx];
        return sym_addr(*gs);
    }
    shndx = r16(e + 6);
    if (shndx == 0xfff1)
        return r64(e + 8);
    if (shndx == 0 || shndx >= f->nsec || !f->secs[shndx].out)
        return 0;
    return f->secs[shndx].out->addr + f->secs[shndx].off + r64(e + 8);
}

static void define_abs(const char *name, u64 v)
{
    Sym *s = lookup(name, 0);
    if (s && s->def == UNDEF) {
        s->def = ABSOLUTE;
        s->value = v;
    }
}

static Out *find_out(const char *name)
{
    u32 k;
    for (k = 0; k < nouts; k++)
        if (strcmp(outs[k]->name, name) == 0)
            return outs[k];
    return 0;
}

static void bounds(const char *sec, const char *start, const char *end)
{
    Out *o = find_out(sec);
    define_abs(start, o ? o->addr : 0);
    define_abs(end, o ? o->addr + o->size : 0);
}

static int is_c_name(const char *s)
{
    if (!*s || (*s >= '0' && *s <= '9'))
        return 0;
    for (; *s; s++)
        if (!(*s == '_' || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
            return 0;
    return 1;
}

/* ---- relocation ---- */

static int dyn_relative; /* absolute addresses in the image get a relocation for the loader */
/* Thread-local storage: where the image that every thread gets a copy of starts, and how far past the
   thread pointer a thread's copy is (the pointer is at a block of 16 bytes, the copy follows, aligned). */
static u64 tls_start, tls_bias;
static u64 tls_top; /* x86-64: where the thread pointer is, as an address in the image: just past the aligned copy */
static int has_tls;
static int ld_apply_dyn;
static Buf rela_dyn;
static int addr_like(File *f, u32 idx);

static void put_bits(u8 *loc, u32 mask, u32 v)
{
    w32(loc, (r32(loc) & ~mask) | (v & mask));
}

static void reloc(Sec *s, u8 *base)
{
    u32 k;
    File *f = s->f;
    for (k = 0; k < s->nrela; k++) {
        const u8 *r = s->rela + 24 * k;
        u64 off = r64(r), info = r64(r + 8);
        i64 a = (i64)r64(r + 16), v;
        u32 type = (u32)info, idx = (u32)(info >> 32);
        Sym *gs;
        u64 S = ref_addr(f, idx, &gs), P = s->out->addr + s->off + off;
        u8 *loc = base + off;
        int undef_weak = gs && gs->def == UNDEF;
        switch (type) {
        case 0: break;
        /* x86-64 (its numbers are all below those of aarch64) */
        case 2: case 4: /* PC32, PLT32 */
            v = undef_weak && type == 4 ? 0 : (i64)(S + (u64)a - P); /* a call of nothing goes on to the next instruction */
            if (v < -0x7fffffffL - 1 || v > 0x7fffffffL)
                die("%s: %s is out of reach", f->name, gs ? gs->name : s->name);
            w32(loc, (u32)v);
            break;
        case 9: case 41: case 42: /* GOTPCREL: the address slot of the symbol */
            if (!gs || !gs->got)
                die("%s: internal: no address slot", f->name);
            w32(loc, (u32)(got_sec->out->addr + got_sec->off + 8 * (u64)(gs->got - 1) + (u64)a - P));
            break;
        case 10: case 11: /* 32, 32S */
            v = (i64)(S + (u64)a);
            if (type == 10 ? (u64)v > 0xffffffffUL : v < -0x7fffffffL - 1 || v > 0x7fffffffL)
                die("%s: the address of %s does not fit in 32 bits", f->name, gs ? gs->name : s->name);
            w32(loc, (u32)v);
            break;
        case 12: loc[0] = (u8)(S + (u64)a), loc[1] = (u8)((S + (u64)a) >> 8); break;
        case 13: v = (i64)(S + (u64)a - P); loc[0] = (u8)v, loc[1] = (u8)(v >> 8); break;
        case 14: loc[0] = (u8)(S + (u64)a); break;
        case 15:
            v = (i64)(S + (u64)a - P);
            if (v < -128 || v > 127)
                die("%s: %s is out of reach of a short jump", f->name, gs ? gs->name : s->name);
            loc[0] = (u8)v;
            break;
        case 23: /* TPOFF32: thread-local objects lie below the thread pointer */
            if (!has_tls || (gs && gs->def == DEFINED && gs->type != 6))
                die("%s: %s is used as a thread-local object and is not one", f->name, gs ? gs->name : s->name);
            w32(loc, (u32)(S + (u64)a - tls_top));
            break;
        case 24: w64(loc, S + (u64)a - P); break;
        case 1:
        case 257:
            if (dyn_relative && (s->flags & 2) && addr_like(f, idx)) {
                buf_u64(&rela_dyn, P);
                buf_u64(&rela_dyn, type == 1 ? 8 : 1027); /* R_X86_64_RELATIVE, R_AARCH64_RELATIVE */
                buf_u64(&rela_dyn, S + (u64)a);
                if (!ld_apply_dyn)
                    break;
            }
            w64(loc, S + (u64)a);
            break;
        case 258: w32(loc, (u32)(S + (u64)a)); break;
        case 259: loc[0] = (u8)(S + (u64)a), loc[1] = (u8)((S + (u64)a) >> 8); break;
        case 260: w64(loc, S + (u64)a - P); break;
        case 261: w32(loc, (u32)(S + (u64)a - P)); break;
        case 262: v = (i64)(S + (u64)a - P); loc[0] = (u8)v, loc[1] = (u8)(v >> 8); break;
        case 263: case 264: put_bits(loc, 0xffff << 5, (u32)((S + (u64)a) & 0xffff) << 5); break;
        case 265: case 266: put_bits(loc, 0xffff << 5, (u32)((S + (u64)a) >> 16 & 0xffff) << 5); break;
        case 267: case 268: put_bits(loc, 0xffff << 5, (u32)((S + (u64)a) >> 32 & 0xffff) << 5); break;
        case 269: put_bits(loc, 0xffff << 5, (u32)((S + (u64)a) >> 48 & 0xffff) << 5); break;
        case 273: case 280:
            v = (i64)(S + (u64)a - P);
            if (v < -(1 << 20) || v >= (1 << 20))
                die("%s: %s is out of reach", f->name, gs ? gs->name : s->name);
            put_bits(loc, 0x7ffff << 5, (u32)(v >> 2) << 5);
            break;
        case 274:
            v = (i64)(S + (u64)a - P);
            if (v < -(1 << 20) || v >= (1 << 20))
                die("%s: %s is out of reach", f->name, gs ? gs->name : s->name);
            put_bits(loc, 0x60ffffe0, ((u32)v & 3) << 29 | ((u32)(v >> 2) & 0x7ffff) << 5);
            break;
        case 275: case 276: case 311:
            if (type == 311) {
                if (!gs || !gs->got)
                    die("%s: internal: no address slot", f->name);
                S = got_sec->out->addr + got_sec->off + 8 * (u64)(gs->got - 1);
                a = 0;
            }
            v = (i64)(((S + (u64)a) & ~(u64)0xfff) - (P & ~(u64)0xfff)) >> 12;
            put_bits(loc, 0x60ffffe0, ((u32)v & 3) << 29 | ((u32)(v >> 2) & 0x7ffff) << 5);
            break;
        case 277: case 278: put_bits(loc, 0xfff << 10, (u32)((S + (u64)a) & 0xfff) << 10); break;
        case 284: put_bits(loc, 0xfff << 10, (u32)((S + (u64)a) & 0xfff) >> 1 << 10); break;
        case 285: put_bits(loc, 0xfff << 10, (u32)((S + (u64)a) & 0xfff) >> 2 << 10); break;
        case 286: put_bits(loc, 0xfff << 10, (u32)((S + (u64)a) & 0xfff) >> 3 << 10); break;
        case 299: put_bits(loc, 0xfff << 10, (u32)((S + (u64)a) & 0xfff) >> 4 << 10); break;
        case 312:
            if (!gs || !gs->got)
                die("%s: internal: no address slot", f->name);
            S = got_sec->out->addr + got_sec->off + 8 * (u64)(gs->got - 1);
            put_bits(loc, 0xfff << 10, (u32)(S & 0xfff) >> 3 << 10);
            break;
        case 549: case 550: case 551: /* an add of the offset from the thread pointer: high part, low part */
            if (!has_tls || (gs && gs->def == DEFINED && gs->type != 6))
                die("%s: %s is used as a thread-local object and is not one", f->name, gs ? gs->name : s->name);
            v = (i64)(S + (u64)a - tls_start + tls_bias);
            if (v < 0 || v >= (1 << 24))
                die("%s: thread-local storage is too large", f->name);
            put_bits(loc, 0xfff << 10, (u32)(type == 549 ? v >> 12 : v & 0xfff) << 10);
            break;
        case 279:
            v = (i64)(S + (u64)a - P);
            if (v < -(1 << 15) || v >= (1 << 15))
                die("%s: %s is out of reach", f->name, gs ? gs->name : s->name);
            put_bits(loc, 0x3fff << 5, (u32)(v >> 2) << 5);
            break;
        case 282: case 283:
            v = undef_weak ? 4 : (i64)(S + (u64)a - P); /* a call of nothing goes on to the next instruction */
            if (v < -(1 << 27) || v >= (1 << 27))
                die("%s: %s is out of reach of a branch", f->name, gs ? gs->name : s->name);
            put_bits(loc, 0x3ffffff, (u32)(v >> 2));
            break;
        default:
            die("%s: relocation type %u is not supported", f->name, type);
        }
    }
}

/* ---- output ---- */

void eld_link(const char *out_path, const char *entry)
{
    File *f;
    Archive *a;
    Sym *s, *e;
    u32 i, k, nsh, nph = 3, ncommon = 0;
    u64 tls_end = 0, tls_fsize = 0, tls_align = 1, tls_off = 0;
    u64 addr, off, common_size = 0, seg_off[2], seg_addr[2], seg_fsize[2], seg_msize[2];
    Buf out, symtab, strtab, shstr;
    int rw_started = 0, changed, bad = 0;
    u8 *img;

    if (!entry)
        entry = "_start";
    s = lookup(entry, 1);
    if (s->def == UNDEF && !s->f)
        s->f = &synth;
    /* archive members that define what is missing, until nothing more is found */
    do {
        File *last = 0;
        for (f = files; f; f = f->next)
            last = f;
        for (a = archives; a; a = a->next)
            each_member(a, 0);
        changed = last ? last->next != 0 : files != 0;
    } while (changed);

    /* sections that go into the image */
    synth.name = "<linker>";
    synth.secs = (Sec *)xmalloc(2 * sizeof(Sec));
    memset(synth.secs, 0, 2 * sizeof(Sec));
    got_sec = &synth.secs[0];
    common_sec = &synth.secs[1];
    for (f = files; f; f = f->next) {
        for (i = 0; i < f->nsec; i++) {
            Sec *c = &f->secs[i];
            if (!(c->flags & 2) && c->type == 1 && strncmp(c->name, ".debug_", 7) == 0) {
                out_for(c); /* debug information: kept, after the image */
                continue;
            }
            if (!(c->flags & 2) || (c->type != 1 && c->type != 8 && c->type != 14 && c->type != 15 && c->type != 7 &&
                                    c->type != 0x70000001 /* x86-64 .eh_frame */))
                continue;
            out_for(c);
        }
        /* which symbols need a slot holding their address */
        for (i = 0; i < f->nsec; i++) {
            Sec *c = &f->secs[i];
            if (!c->out)
                continue;
            for (k = 0; k < c->nrela; k++) {
                u64 info = r64(c->rela + 24 * k + 8);
                u32 type = (u32)info, idx = (u32)(info >> 32);
                if ((type == 311 || type == 312 || type == 9 || type == 41 || type == 42) && idx >= f->first_global &&
                    !f->gsyms[idx]->got) {
                    got_syms = (Sym **)xrealloc(got_syms, (ngot + 1) * sizeof(Sym *));
                    got_syms[ngot] = f->gsyms[idx];
                    f->gsyms[idx]->got = ++ngot;
                } else if (type == 311 || type == 312) {
                    if (idx < f->first_global)
                        die("%s: an address slot for a local symbol", f->name);
                }
            }
        }
    }
    if (ngot) {
        got_data = (u8 *)xmalloc(8 * ngot);
        memset(got_data, 0, 8 * ngot);
        got_sec->f = &synth;
        got_sec->name = ".got";
        got_sec->type = 1;
        got_sec->flags = 3;
        got_sec->size = 8 * ngot;
        got_sec->align = 8;
        got_sec->data = got_data;
        out_for(got_sec);
    }
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next)
            if (s->def == COMMON) {
                u64 al = s->value ? s->value : 1;
                common_size = align_up(common_size, al);
                s->value = common_size;
                common_size += s->size;
                s->def = DEFINED;
                s->sec = common_sec;
                ncommon++;
            }
    if (ncommon) {
        common_sec->f = &synth;
        common_sec->name = ".bss";
        common_sec->type = 8;
        common_sec->flags = 3;
        common_sec->size = common_size;
        common_sec->align = 16;
        out_for(common_sec);
    }

    /* order: code, constants, data, zeroed data */
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        o->shndx = k;
        o->rank = o->flags & 4 ? 0 : !(o->flags & 1) ? 1 : o->type != 8 ? 2 : 3;
        if (o->rank == 0 && strcmp(o->name, ".text") != 0)
            o->shndx += strcmp(o->name, ".init") == 0 ? 0 : 0x10000;
        if (!(o->flags & 2))
            o->rank = 4; /* not loaded: last, after the image */
        if (o->flags & 0x400) {
            /* thread-local: the initialised part, then the zeroed part, ahead of the other data */
            o->shndx = o->type != 8 ? 0 : 1;
            o->rank = 2;
            has_tls = 1;
        } else if (o->rank == 2) {
            o->shndx += 2;
        }
    }
    qsort(outs, nouts, sizeof(Out *), cmp_out);
    if (has_tls)
        nph++;
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        if (!(o->flags & 0x400))
            continue;
        for (i = 0; i < o->n; i++)
            if (o->secs[i]->align > tls_align)
                tls_align = o->secs[i]->align;
    }

    off = 64 + 56 * nph;
    addr = BASE + off;
    seg_off[0] = 0;
    seg_addr[0] = BASE;
    seg_off[1] = seg_addr[1] = seg_fsize[1] = seg_msize[1] = 0;
    seg_fsize[0] = seg_msize[0] = off;
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        u64 size = 0;
        for (i = 0; i < o->n; i++)
            if (o->secs[i]->align > o->align)
                o->align = o->secs[i]->align;
        if (o->rank == 4) {
            /* no address; its place in the file comes once the image is written */
            for (i = 0; i < o->n; i++) {
                Sec *c = o->secs[i];
                size = align_up(size, c->align);
                c->off = size;
                size += c->size;
            }
            o->size = size;
            o->addr = 0;
            continue;
        }
        if (o->rank >= 2 && !rw_started) {
            rw_started = 1;
            addr = align_up(addr, PAGE) + off % PAGE;
            seg_off[1] = off;
            seg_addr[1] = addr;
        }
        if (o->flags & 0x400) {
            /* the image starts as aligned as its strictest part */
            if (!tls_end)
                o->align = tls_align;
            if (o->type == 8) {
                /* the zeroed part takes no room: only the copies have it */
                u64 at = align_up(tls_end ? tls_end : addr, o->align);
                o->addr = at;
                o->off = off;
                for (i = 0; i < o->n; i++) {
                    Sec *c = o->secs[i];
                    size = align_up(size, c->align);
                    c->off = size;
                    size += c->size;
                }
                o->size = size;
                if (!tls_end) {
                    tls_start = at;
                    tls_off = off + (align_up(addr, o->align) - addr);
                }
                tls_end = at + size;
                continue;
            }
        }
        {
            u64 na = align_up(addr, o->align);
            off += na - addr;
            addr = na;
        }
        o->addr = addr;
        o->off = off;
        for (i = 0; i < o->n; i++) {
            Sec *c = o->secs[i];
            size = align_up(size, c->align);
            c->off = size;
            size += c->size;
        }
        o->size = size;
        if (o->flags & 0x400) {
            tls_start = addr;
            tls_off = off;
            tls_fsize = size;
            tls_end = addr + size;
        }
        addr += size;
        if (o->type != 8)
            off += size;
        if (rw_started) {
            seg_msize[1] = addr - seg_addr[1];
            if (o->type != 8)
                seg_fsize[1] = off - seg_off[1];
        } else {
            seg_fsize[0] = seg_msize[0] = off;
        }
    }
    if (!rw_started) {
        seg_off[1] = off;
        seg_addr[1] = align_up(addr, PAGE) + off % PAGE;
    }
    tls_bias = align_up(16, tls_align);
    tls_top = tls_start + align_up(tls_end - tls_start, tls_align);

    /* symbols the linker supplies */
    bounds(".init_array", "__init_array_start", "__init_array_end");
    bounds(".fini_array", "__fini_array_start", "__fini_array_end");
    bounds(".preinit_array", "__preinit_array_start", "__preinit_array_end");
    {
        Out *bss = find_out(".bss");
        define_abs("__bss_start", bss ? bss->addr : addr);
        define_abs("__bss_start__", bss ? bss->addr : addr);
        define_abs("_edata", seg_addr[1] + seg_fsize[1]);
        define_abs("edata", seg_addr[1] + seg_fsize[1]);
        define_abs("_end", addr);
        define_abs("end", addr);
        define_abs("__bss_end__", addr);
        define_abs("_etext", seg_addr[0] + seg_msize[0]);
        define_abs("etext", seg_addr[0] + seg_msize[0]);
        define_abs("__ehdr_start", BASE);
        define_abs("__executable_start", BASE);
    }
    for (k = 0; k < nouts; k++)
        if (is_c_name(outs[k]->name)) {
            char *n1 = (char *)xmalloc(strlen(outs[k]->name) + 9), *n2 = (char *)xmalloc(strlen(outs[k]->name) + 8);
            sprintf(n1, "__start_%s", outs[k]->name);
            sprintf(n2, "__stop_%s", outs[k]->name);
            define_abs(n1, outs[k]->addr);
            define_abs(n2, outs[k]->addr + outs[k]->size);
        }
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next)
            if (s->def == UNDEF && !s->weak) {
                fprintf(stderr, "mcc: undefined symbol: %s (first needed by %s)\n", s->name, s->f ? s->f->name : "?");
                bad = 1;
            }
    if (bad)
        die("link failed");

    /* the image */
    memset(&out, 0, sizeof out);
    buf_zero(&out, (size_t)off);
    img = out.p;
    for (k = 0; k < ngot; k++)
        w64(got_data + 8 * k, sym_addr(got_syms[k]));
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        if (o->type == 8)
            continue;
        if (o->rank == 4) {
            buf_zero(&out, (size_t)(align_up(out.len, o->align) - out.len));
            o->off = out.len;
            buf_zero(&out, (size_t)o->size);
            img = out.p;
        }
        for (i = 0; i < o->n; i++) {
            Sec *c = o->secs[i];
            if (c->type != 8 && c->size)
                memcpy(img + o->off + c->off, c->data, (size_t)c->size);
        }
        for (i = 0; i < o->n; i++)
            if (o->secs[i]->nrela)
                reloc(o->secs[i], img + o->off + o->secs[i]->off);
    }

    /* symbol table: file by file the local names, then the global ones */
    memset(&symtab, 0, sizeof symtab);
    memset(&strtab, 0, sizeof strtab);
    memset(&shstr, 0, sizeof shstr);
    buf_zero(&symtab, 24);
    buf_u8(&strtab, 0);
    for (k = 0; k < nouts; k++)
        outs[k]->shndx = k + 1;
    for (f = files; f; f = f->next)
        for (i = 1; i < f->first_global; i++) {
            const u8 *se = f->symtab + 24 * i;
            const char *nm = f->strtab + r32(se);
            u32 shndx = r16(se + 6);
            Sym *gs;
            if ((se[4] & 15) >= 3 || !*nm || nm[0] == '$' || (nm[0] == '.' && nm[1] == 'L') || shndx == 0 ||
                (shndx < 0xff00 && !f->secs[shndx].out))
                continue;
            buf_u32(&symtab, (u32)strtab.len);
            buf_u8(&symtab, se[4]);
            buf_u8(&symtab, 0);
            buf_u8(&symtab, (u8)(shndx >= 0xff00 ? 0xf1 : f->secs[shndx].out->shndx));
            buf_u8(&symtab, (u8)(shndx >= 0xff00 ? 0xff : f->secs[shndx].out->shndx >> 8));
            buf_u64(&symtab, ref_addr(f, i, &gs));
            buf_u64(&symtab, r64(se + 16));
            buf_put(&strtab, nm, strlen(nm) + 1);
        }
    nsh = (u32)(symtab.len / 24); /* the first global */
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next) {
            u32 shndx = s->def == DEFINED && s->sec && s->sec->out ? s->sec->out->shndx : s->def == UNDEF ? 0 : 0xfff1;
            buf_u32(&symtab, (u32)strtab.len);
            buf_u8(&symtab, (u8)((s->weak ? 2 : 1) << 4 | s->type));
            buf_u8(&symtab, s->vis);
            buf_u8(&symtab, (u8)shndx);
            buf_u8(&symtab, (u8)(shndx >> 8));
            buf_u64(&symtab, sym_addr(s) - (s->type == 6 && s->def == DEFINED ? tls_start : 0));
            buf_u64(&symtab, s->size);
            buf_put(&strtab, s->name, strlen(s->name) + 1);
        }

    /* section headers */
    {
        u64 symoff, stroff, shstroff, shoff;
        u32 *names = (u32 *)xmalloc((nouts + 4) * sizeof(u32)), n_sym, n_str, n_shstr;
        u8 *h;
        buf_u8(&shstr, 0);
        for (k = 0; k < nouts; k++) {
            names[k] = (u32)shstr.len;
            buf_put(&shstr, outs[k]->name, strlen(outs[k]->name) + 1);
        }
        n_sym = (u32)shstr.len;
        buf_put(&shstr, ".symtab", 8);
        n_str = (u32)shstr.len;
        buf_put(&shstr, ".strtab", 8);
        n_shstr = (u32)shstr.len;
        buf_put(&shstr, ".shstrtab", 10);
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        symoff = out.len;
        buf_put(&out, symtab.p, symtab.len);
        stroff = out.len;
        buf_put(&out, strtab.p, strtab.len);
        shstroff = out.len;
        buf_put(&out, shstr.p, shstr.len);
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        shoff = out.len;
        buf_zero(&out, 64 * (nouts + 4));
        img = out.p;
        for (k = 0; k < nouts; k++) {
            Out *o = outs[k];
            h = img + shoff + 64 * (k + 1);
            w32(h, names[k]);
            w32(h + 4, o->type);
            w64(h + 8, o->rank == 4 ? o->flags : o->flags | 2);
            w64(h + 16, o->addr);
            w64(h + 24, o->off);
            w64(h + 32, o->size);
            w64(h + 48, o->align);
            if (o->type == 14 || o->type == 15)
                w64(h + 56, 8);
        }
        h = img + shoff + 64 * (nouts + 1);
        w32(h, n_sym);
        w32(h + 4, 2);
        w64(h + 24, symoff);
        w64(h + 32, symtab.len);
        w32(h + 40, nouts + 2);
        w32(h + 44, nsh);
        w64(h + 48, 8);
        w64(h + 56, 24);
        h += 64;
        w32(h, n_str);
        w32(h + 4, 3);
        w64(h + 24, stroff);
        w64(h + 32, strtab.len);
        w64(h + 48, 1);
        h += 64;
        w32(h, n_shstr);
        w32(h + 4, 3);
        w64(h + 24, shstroff);
        w64(h + 32, shstr.len);
        w64(h + 48, 1);

        /* file header */
        e = lookup(entry, 0);
        memcpy(img, "\177ELF\2\1\1", 7);
        img[16] = 2; /* ET_EXEC */
        img[18] = (u8)machine();
        w32(img + 20, 1);
        w64(img + 24, e ? sym_addr(e) : 0);
        w64(img + 32, 64);
        w64(img + 40, shoff);
        img[52] = 64;
        img[54] = 56;
        img[56] = (u8)nph;
        img[58] = 64;
        img[60] = (u8)(nouts + 4);
        img[61] = (u8)((nouts + 4) >> 8);
        img[62] = (u8)(nouts + 3);
        img[63] = (u8)((nouts + 3) >> 8);
        free(names);
    }
    /* program headers */
    for (k = 0; k < 2; k++) {
        u8 *ph = img + 64 + 56 * k;
        w32(ph, 1);
        w32(ph + 4, k == 0 ? 5 : 6);
        w64(ph + 8, seg_off[k]);
        w64(ph + 16, seg_addr[k]);
        w64(ph + 24, seg_addr[k]);
        w64(ph + 32, seg_fsize[k]);
        w64(ph + 40, seg_msize[k]);
        w64(ph + 48, PAGE);
    }
    {
        u8 *ph = img + 64 + 56 * 2;
        w32(ph, 0x6474e551); /* PT_GNU_STACK */
        w32(ph + 4, 6);
        w64(ph + 48, 16);
    }
    if (has_tls) {
        u8 *ph = img + 64 + 56 * 3;
        w32(ph, 7); /* PT_TLS */
        w32(ph + 4, 4);
        w64(ph + 8, tls_off);
        w64(ph + 16, tls_start);
        w64(ph + 24, tls_start);
        w64(ph + 32, tls_fsize);
        w64(ph + 40, tls_end - tls_start);
        w64(ph + 48, tls_align);
    }
    os_write_file(out_path, out.p, out.len, 1);
    buf_free(&out);
    buf_free(&symtab);
    buf_free(&strtab);
    buf_free(&shstr);
}

/* ---- relocatable output: the inputs joined into one object ---- */

static void put_sym(Buf *symtab, Buf *strtab, const char *name, u32 info, u32 other, u32 shndx, u64 value, u64 size)
{
    buf_u32(symtab, *name ? (u32)strtab->len : 0);
    buf_u8(symtab, info);
    buf_u8(symtab, other);
    buf_u8(symtab, shndx & 255);
    buf_u8(symtab, shndx >> 8);
    buf_u64(symtab, value);
    buf_u64(symtab, size);
    if (*name)
        buf_put(strtab, name, strlen(name) + 1);
}

void eld_relocatable(const char *out_path)
{
    File *f;
    Archive *a;
    Sym *s;
    u32 i, k, nrel = 0, first_global, nsh, r;
    Buf out, symtab, strtab, shstr;
    int changed;
    u32 **maps, nfiles = 0, fi;
    u8 *h;
    u64 shoff;

    relocatable = 1;
    do {
        File *last = 0;
        for (f = files; f; f = f->next)
            last = f;
        for (a = archives; a; a = a->next)
            each_member(a, 0);
        changed = last ? last->next != 0 : files != 0;
    } while (changed);

    for (f = files; f; f = f->next) {
        nfiles++;
        for (i = 1; i < f->nsec; i++) {
            Sec *c = &f->secs[i];
            if (c->type == 0 || c->type == 2 || c->type == 3 || c->type == 4 || c->type == 17 ||
                (c->type >= 0x60000000 && c->type != 0x70000001 /* x86-64 .eh_frame */))
                continue;
            out_for(c);
        }
    }
    memset(&out, 0, sizeof out);
    memset(&symtab, 0, sizeof symtab);
    memset(&strtab, 0, sizeof strtab);
    memset(&shstr, 0, sizeof shstr);
    buf_zero(&out, 64);
    buf_zero(&symtab, 24);
    buf_u8(&strtab, 0);
    buf_u8(&shstr, 0);

    /* contents */
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        u64 size = 0;
        o->shndx = k + 1;
        for (i = 0; i < o->n; i++) {
            Sec *c = o->secs[i];
            if (c->align > o->align)
                o->align = c->align;
            size = align_up(size, c->align);
            c->off = size;
            size += c->size;
            o->nrela += c->nrela;
        }
        o->size = size;
        buf_zero(&out, (size_t)(align_up(out.len, o->align) - out.len));
        o->off = out.len;
        if (o->type != 8) {
            buf_zero(&out, (size_t)size);
            for (i = 0; i < o->n; i++)
                if (o->secs[i]->type != 8 && o->secs[i]->size)
                    memcpy(out.p + o->off + o->secs[i]->off, o->secs[i]->data, (size_t)o->secs[i]->size);
        }
        if (o->nrela)
            nrel++;
        put_sym(&symtab, &strtab, "", 3, 0, o->shndx, 0, 0);
    }
    if (nouts + nrel + 4 >= 0xff00)
        die("too many sections for %s", out_path);

    /* symbols: the local ones file by file, then the global ones */
    maps = (u32 **)xmalloc((nfiles + 1) * sizeof(u32 *));
    for (f = files, fi = 0; f; f = f->next, fi++) {
        u32 *map = (u32 *)xmalloc((f->nsyms + 1) * sizeof(u32));
        memset(map, 0, (f->nsyms + 1) * sizeof(u32));
        maps[fi] = map;
        for (i = 1; i < f->first_global && i < f->nsyms; i++) {
            const u8 *se = f->symtab + 24 * i;
            u32 shndx = r16(se + 6), type = se[4] & 15;
            Sec *c = shndx && shndx < 0xff00 ? &f->secs[shndx] : 0;
            if (type == 4 || (c && !c->out) || (!c && shndx != 0xfff1))
                continue;
            if (type == 3) {
                map[i] = c->out->shndx; /* the section symbols come first, in section order */
                continue;
            }
            map[i] = (u32)(symtab.len / 24);
            put_sym(&symtab, &strtab, f->strtab + r32(se), se[4], se[5], c ? c->out->shndx : 0xfff1,
                    r64(se + 8) + (c ? c->off : 0), r64(se + 16));
        }
    }
    first_global = (u32)(symtab.len / 24);
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next) {
            u32 shndx = 0;
            u64 value = s->value;
            if (s->def == DEFINED && s->sec && s->sec->out) {
                shndx = s->sec->out->shndx;
                value += s->sec->off;
            } else if (s->def == COMMON)
                shndx = 0xfff2;
            else if (s->def == ABSOLUTE)
                shndx = 0xfff1;
            else
                value = 0;
            s->idx = (u32)(symtab.len / 24);
            put_sym(&symtab, &strtab, s->name, (u32)((s->weak ? 2 : 1) << 4 | s->type), s->vis, shndx, value, s->size);
        }

    /* relocations, with places and symbols renumbered */
    nsh = nouts + 1;
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        u64 at;
        if (!o->nrela)
            continue;
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        at = out.len;
        for (i = 0; i < o->n; i++) {
            Sec *c = o->secs[i];
            u32 *map = 0;
            for (f = files, fi = 0; f; f = f->next, fi++)
                if (f == c->f)
                    map = maps[fi];
            for (r = 0; r < c->nrela; r++) {
                const u8 *e = c->rela + 24 * r;
                u64 info = r64(e + 8), addend = r64(e + 16);
                u32 idx = (u32)(info >> 32), nidx;
                if (idx >= c->f->first_global)
                    nidx = c->f->gsyms[idx]->idx;
                else {
                    const u8 *se = c->f->symtab + 24 * idx;
                    nidx = map[idx];
                    if (!nidx && idx)
                        die("%s: a relocation against a discarded section", c->f->name);
                    if ((se[4] & 15) == 3)
                        addend += c->f->secs[r16(se + 6)].off;
                }
                buf_u64(&out, r64(e) + c->off);
                buf_u64(&out, (u64)nidx << 32 | (u32)info);
                buf_u64(&out, addend);
            }
        }
        o->addr = at; /* where its relocations are in the file */
        o->rank = nsh++;
    }

    /* section headers */
    {
        u32 *names = (u32 *)xmalloc((nouts + 1) * sizeof(u32)), *rnames = (u32 *)xmalloc((nouts + 1) * sizeof(u32));
        u32 n_sym, n_str, n_shstr, i_sym = nsh;
        u64 symoff, stroff, shstroff;
        for (k = 0; k < nouts; k++) {
            if (outs[k]->nrela) {
                rnames[k] = (u32)shstr.len;
                buf_put(&shstr, ".rela", 5);
                names[k] = (u32)shstr.len;
            } else
                names[k] = (u32)shstr.len;
            buf_put(&shstr, outs[k]->name, strlen(outs[k]->name) + 1);
        }
        n_sym = (u32)shstr.len;
        buf_put(&shstr, ".symtab", 8);
        n_str = (u32)shstr.len;
        buf_put(&shstr, ".strtab", 8);
        n_shstr = (u32)shstr.len;
        buf_put(&shstr, ".shstrtab", 10);
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        symoff = out.len;
        buf_put(&out, symtab.p, symtab.len);
        stroff = out.len;
        buf_put(&out, strtab.p, strtab.len);
        shstroff = out.len;
        buf_put(&out, shstr.p, shstr.len);
        buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
        shoff = out.len;
        buf_zero(&out, 64 * (nsh + 3));
        for (k = 0; k < nouts; k++) {
            Out *o = outs[k];
            h = out.p + shoff + 64 * (k + 1);
            w32(h, names[k]);
            w32(h + 4, o->type);
            w64(h + 8, o->flags);
            w64(h + 24, o->off);
            w64(h + 32, o->size);
            w64(h + 48, o->align);
            w64(h + 56, o->entsize);
            if (!o->nrela)
                continue;
            h = out.p + shoff + 64 * o->rank;
            w32(h, rnames[k]);
            w32(h + 4, 4);
            w64(h + 8, 0x40);
            w64(h + 24, o->addr);
            w64(h + 32, 24 * (u64)o->nrela);
            w32(h + 40, i_sym);
            w32(h + 44, k + 1);
            w64(h + 48, 8);
            w64(h + 56, 24);
        }
        h = out.p + shoff + 64 * i_sym;
        w32(h, n_sym);
        w32(h + 4, 2);
        w64(h + 24, symoff);
        w64(h + 32, symtab.len);
        w32(h + 40, i_sym + 1);
        w32(h + 44, first_global);
        w64(h + 48, 8);
        w64(h + 56, 24);
        h += 64;
        w32(h, n_str);
        w32(h + 4, 3);
        w64(h + 24, stroff);
        w64(h + 32, strtab.len);
        w64(h + 48, 1);
        h += 64;
        w32(h, n_shstr);
        w32(h + 4, 3);
        w64(h + 24, shstroff);
        w64(h + 32, shstr.len);
        w64(h + 48, 1);
    }
    h = out.p;
    memcpy(h, "\177ELF\2\1\1", 7);
    h[16] = 1;
    h[18] = (u8)machine();
    w32(h + 20, 1);
    w64(h + 0x28, shoff);
    h[0x34] = 64;
    h[0x3a] = 64;
    h[0x3c] = (u8)(nsh + 3);
    h[0x3d] = (u8)((nsh + 3) >> 8);
    h[0x3e] = (u8)(nsh + 2);
    h[0x3f] = (u8)((nsh + 2) >> 8);
    os_write_file(out_path, out.p, out.len, 0);
}

/* ---- linker scripts ---- */

typedef struct LExpr LExpr;
struct LExpr {
    int op; /* a character for an operator, or one of the X_ codes */
    u64 num;
    const char *name;
    LExpr *a, *b, *c;
};
enum { X_NUM = 256, X_SYM, X_DOT, X_SHL, X_SHR, X_EQ, X_NE, X_LE, X_GE, X_AND, X_OR, X_NEG, X_ALIGN, X_ADDR,
       X_SIZEOF, X_ABSOLUTE, X_HEADERS, X_DEFINED, X_MAX, X_MIN };

typedef struct LPat {
    const char *pat;
    int sort;
} LPat;

typedef struct LCmd LCmd;
enum { L_ASSIGN, L_ASSERT, L_OUT, L_IN };
struct LCmd {
    int kind;
    const char *name; /* the symbol assigned, the output section, or an assertion's message */
    int op, provide, active;
    LExpr *e;
    LExpr *addr, *align; /* L_OUT */
    LCmd *sub;
    int discard, has_symbols, nphdrs;
    const char *phdrs[4];
    Out *out;
    LPat *pats; /* L_IN */
    u32 npats;
    Sec **secs;
    u32 nsecs;
    LCmd *next;
};

typedef struct LPhdr {
    const char *name;
    u32 type, flags;
    int has_flags, filehdr;
    u64 lo, hi, fhi; /* what it covers: addresses, and where its file contents end */
    u64 align;
    int used;
} LPhdr;

typedef struct LVersion {
    const char *name;
    const char **globals;
    u32 nglobals;
} LVersion;

static const char *lp;      /* the script being read */
static const char *lp_name;
static LCmd *script, *script_after; /* commands in SECTIONS, and the assignments outside it */
static LPhdr lphdrs[8];
static u32 nlphdrs;
static LVersion lversions[4];
static u32 nlversions;
static const char *script_entry;

static void lskip(void)
{
    for (;;) {
        while (*lp == ' ' || *lp == '\t' || *lp == '\n' || *lp == '\r')
            lp++;
        if (lp[0] == '/' && lp[1] == '*') {
            for (lp += 2; *lp && !(lp[0] == '*' && lp[1] == '/'); lp++)
                ;
            if (*lp)
                lp += 2;
        } else if (lp[0] == '#') { /* a line marker left by the preprocessor */
            while (*lp && *lp != '\n')
                lp++;
        } else
            break;
    }
}

static void lerr(const char *msg)
{
    const char *e = lp;
    int n = 0;
    while (*e && *e != '\n' && n < 40)
        e++, n++;
    die("%s: %s near '%.*s'", lp_name, msg, n, lp);
}

static int laccept(const char *s)
{
    size_t n = strlen(s);
    lskip();
    if (strncmp(lp, s, n) != 0)
        return 0;
    lp += n;
    return 1;
}

static void lexpect(const char *s)
{
    if (!laccept(s)) {
        char msg[64];
        sprintf(msg, "expected '%s'", s);
        lerr(msg);
    }
}

static int is_sym_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$';
}

/* A word made of the given kind of characters: 0 a symbol, 1 a section name, 2 a pattern. */
static const char *lword(int kind)
{
    const char *s;
    char *w;
    lskip();
    s = lp;
    if (kind == 0)
        while (is_sym_char((u8)*lp))
            lp++;
    else if (kind == 1)
        while (is_sym_char((u8)*lp) || *lp == '/' || *lp == '-' || *lp == '~')
            lp++;
    else
        while (*lp && !strchr(" \t\r\n(){};,", *lp))
            lp++;
    if (lp == s)
        lerr("expected a name");
    w = (char *)xmalloc((size_t)(lp - s) + 1);
    memcpy(w, s, (size_t)(lp - s));
    w[lp - s] = 0;
    return w;
}

/* Is the keyword here, as a whole word followed by an opening parenthesis? */
static int lkeyword(const char *kw)
{
    size_t n = strlen(kw);
    const char *save;
    lskip();
    if (strncmp(lp, kw, n) != 0 || is_sym_char((u8)lp[n]))
        return 0;
    save = lp;
    lp += n;
    lskip();
    if (*lp != '(') {
        lp = save;
        return 0;
    }
    lp++;
    return 1;
}

static LExpr *lnode(int op, LExpr *a, LExpr *b)
{
    LExpr *e = (LExpr *)xmalloc(sizeof(LExpr));
    memset(e, 0, sizeof *e);
    e->op = op;
    e->a = a;
    e->b = b;
    return e;
}

static LExpr *lexpr(void);

static LExpr *lprimary(void)
{
    LExpr *e;
    lskip();
    if (*lp == '(') {
        lp++;
        e = lexpr();
        lexpect(")");
        return e;
    }
    if (*lp == '-' || *lp == '~' || *lp == '!' || *lp == '+') {
        int c = *lp++;
        e = lprimary();
        return c == '+' ? e : lnode(c == '-' ? X_NEG : c, e, 0);
    }
    if (*lp >= '0' && *lp <= '9') {
        char *end;
        e = lnode(X_NUM, 0, 0);
        e->num = strtoul(lp, &end, 0);
        lp = end;
        if (*lp == 'K' || *lp == 'k')
            e->num <<= 10, lp++;
        else if (*lp == 'M' || *lp == 'm')
            e->num <<= 20, lp++;
        return e;
    }
    if (lkeyword("ALIGN")) {
        e = lnode(X_ALIGN, lexpr(), 0);
        if (laccept(",")) { /* ALIGN(value, alignment) */
            e->b = e->a;
            e->a = lexpr();
        }
        lexpect(")");
        return e;
    }
    if (lkeyword("ADDR") || lkeyword("LOADADDR")) {
        e = lnode(X_ADDR, 0, 0);
        e->name = lword(1);
        lexpect(")");
        return e;
    }
    if (lkeyword("SIZEOF")) {
        e = lnode(X_SIZEOF, 0, 0);
        e->name = lword(1);
        lexpect(")");
        return e;
    }
    if (lkeyword("DEFINED")) {
        e = lnode(X_DEFINED, 0, 0);
        e->name = lword(0);
        lexpect(")");
        return e;
    }
    if (lkeyword("ABSOLUTE")) {
        e = lnode(X_ABSOLUTE, lexpr(), 0);
        lexpect(")");
        return e;
    }
    if (lkeyword("MAX")) {
        e = lnode(X_MAX, lexpr(), 0);
        lexpect(",");
        e->b = lexpr();
        lexpect(")");
        return e;
    }
    if (lkeyword("MIN")) {
        e = lnode(X_MIN, lexpr(), 0);
        lexpect(",");
        e->b = lexpr();
        lexpect(")");
        return e;
    }
    if (lkeyword("CONSTANT")) {
        const char *w = lword(0);
        lexpect(")");
        e = lnode(X_NUM, 0, 0);
        e->num = strcmp(w, "MAXPAGESIZE") == 0 ? PAGE : 0x1000;
        return e;
    }
    if (strncmp(lp, "SIZEOF_HEADERS", 14) == 0 && !is_sym_char((u8)lp[14])) {
        lp += 14;
        return lnode(X_HEADERS, 0, 0);
    }
    if (is_sym_char((u8)*lp)) {
        const char *w = lword(0);
        if (strcmp(w, ".") == 0)
            return lnode(X_DOT, 0, 0);
        e = lnode(X_SYM, 0, 0);
        e->name = w;
        return e;
    }
    lerr("expected an expression");
    return 0;
}

static int lbinop(int *prec)
{
    static const struct {
        const char *s;
        int op, prec;
    } ops[] = {{"||", X_OR, 1}, {"&&", X_AND, 2}, {"==", X_EQ, 6}, {"!=", X_NE, 6}, {"<<", X_SHL, 8}, {">>", X_SHR, 8},
               {"<=", X_LE, 7}, {">=", X_GE, 7}, {"|", '|', 3}, {"^", '^', 4}, {"&", '&', 5}, {"<", '<', 7}, {">", '>', 7},
               {"+", '+', 9}, {"-", '-', 9}, {"*", '*', 10}, {"/", '/', 10}, {"%", '%', 10}};
    u32 k;
    lskip();
    for (k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        size_t n = strlen(ops[k].s);
        if (strncmp(lp, ops[k].s, n) == 0) {
            if (lp[n] == '=' && n == 1 && (lp[0] != '<' && lp[0] != '>'))
                return 0; /* an assignment operator such as += */
            if (n == 2 && lp[2] == '=' && (ops[k].op == X_SHL || ops[k].op == X_SHR))
                return 0;
            *prec = ops[k].prec;
            return ops[k].op | (int)n << 16;
        }
    }
    return 0;
}

static LExpr *lbinary(int min)
{
    LExpr *e = lprimary();
    for (;;) {
        int prec = 0, op = lbinop(&prec);
        if (!op || prec < min)
            return e;
        lp += op >> 16;
        e = lnode(op & 0xffff, e, lbinary(prec + 1));
    }
}

static LExpr *lexpr(void)
{
    LExpr *e = lbinary(1);
    if (laccept("?")) {
        LExpr *t = lnode('?', e, lexpr());
        lexpect(":");
        t->c = lexpr();
        return t;
    }
    return e;
}

static LCmd *lcmd(int kind)
{
    LCmd *c = (LCmd *)xmalloc(sizeof(LCmd));
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

/* "name = expr" and its relatives, after the name. */
static LCmd *lassign(const char *name, int provide)
{
    static const char *const ops[] = {"=", "+=", "-=", "*=", "/=", "&=", "|=", "<<=", ">>=", 0};
    static const int codes[] = {'=', '+', '-', '*', '/', '&', '|', X_SHL, X_SHR};
    LCmd *c = lcmd(L_ASSIGN);
    int k;
    c->name = name;
    c->provide = provide;
    lskip();
    for (k = 0; ops[k]; k++)
        if (laccept(ops[k]))
            break;
    if (!ops[k])
        lerr("expected an assignment");
    c->op = codes[k];
    c->e = lexpr();
    return c;
}

static LCmd *lassert(void)
{
    LCmd *c = lcmd(L_ASSERT);
    const char *s;
    char *m;
    c->e = lexpr();
    lexpect(",");
    lskip();
    if (*lp != '"')
        lerr("expected a message");
    s = ++lp;
    while (*lp && *lp != '"')
        lp++;
    m = (char *)xmalloc((size_t)(lp - s) + 1);
    memcpy(m, s, (size_t)(lp - s));
    m[lp - s] = 0;
    c->name = m;
    if (*lp)
        lp++;
    lexpect(")");
    laccept(";");
    return c;
}

/* A statement that can stand both in SECTIONS and in an output section; 0 if there is none here. */
static LCmd *lcommon(void)
{
    LCmd *c;
    if (lkeyword("ASSERT"))
        return lassert();
    if (lkeyword("PROVIDE") || lkeyword("PROVIDE_HIDDEN") || lkeyword("HIDDEN")) {
        c = lassign(lword(0), 1);
        lexpect(")");
        laccept(";");
        return c;
    }
    return 0;
}

static void linput(LCmd *c)
{
    /* after "*(": patterns up to the closing parenthesis */
    for (;;) {
        int sort = 0;
        lskip();
        if (*lp == ')') {
            lp++;
            return;
        }
        if (lkeyword("SORT") || lkeyword("SORT_BY_NAME"))
            sort = 1;
        else if (lkeyword("SORT_BY_ALIGNMENT") || lkeyword("SORT_NONE"))
            sort = 2;
        if ((c->npats & (c->npats + 1)) == 0)
            c->pats = (LPat *)xrealloc(c->pats, (2 * c->npats + 2) * sizeof(LPat));
        c->pats[c->npats].pat = lword(2);
        c->pats[c->npats].sort = sort == 1;
        c->npats++;
        if (sort)
            lexpect(")");
    }
}

static LCmd *loutput(const char *name)
{
    LCmd *o = lcmd(L_OUT), **tail = &o->sub, *c;
    o->name = name;
    o->discard = strcmp(name, "/DISCARD/") == 0;
    lskip();
    if (*lp != ':')
        o->addr = lexpr();
    if (laccept("(")) { /* (NOLOAD) and the like */
        lword(0);
        lexpect(")");
    }
    lexpect(":");
    for (;;) {
        if (lkeyword("AT")) {
            lexpr();
            lexpect(")");
        } else if (lkeyword("ALIGN")) {
            o->align = lexpr();
            lexpect(")");
        } else if (lkeyword("SUBALIGN")) {
            lexpr();
            lexpect(")");
        } else
            break;
    }
    lexpect("{");
    for (;;) {
        lskip();
        if (*lp == '}') {
            lp++;
            break;
        }
        if (!*lp)
            lerr("unterminated output section");
        if (*lp == ';') {
            lp++;
            continue;
        }
        if ((c = lcommon()) != 0) {
            if (c->kind == L_ASSIGN)
                o->has_symbols = 1;
        } else if (lkeyword("KEEP")) {
            c = lcmd(L_IN);
            lword(2); /* the file pattern */
            lexpect("(");
            linput(c);
            lexpect(")");
        } else if (strncmp(lp, "CONSTRUCTORS", 12) == 0 && !is_sym_char((u8)lp[12])) {
            lp += 12;
            continue;
        } else if (lkeyword("FILL")) {
            lexpr();
            lexpect(")");
            continue;
        } else if (*lp == '*' || *lp == '[' || *lp == '?') {
            c = lcmd(L_IN);
            lword(2);
            lexpect("(");
            linput(c);
        } else {
            const char *w = lword(0), *save;
            lskip();
            save = lp;
            if (*lp == '(') { /* a file name pattern without wildcards */
                lp++;
                c = lcmd(L_IN);
                linput(c);
            } else {
                lp = save;
                c = lassign(w, 0);
                lexpect(";");
                if (strcmp(w, ".") != 0)
                    o->has_symbols = 1;
            }
        }
        *tail = c;
        tail = &c->next;
    }
    for (;;) { /* what follows the braces: segments, a fill pattern */
        lskip();
        if (*lp == ':' ) {
            lp++;
            if (o->nphdrs < 4)
                o->phdrs[o->nphdrs++] = lword(0);
        } else if (*lp == '=') {
            lp++;
            lprimary();
        } else if (*lp == ',') {
            lp++;
        } else
            break;
    }
    return o;
}

static void lsections(void)
{
    LCmd **tail = &script, *c;
    while (*tail)
        tail = &(*tail)->next;
    lexpect("{");
    for (;;) {
        lskip();
        if (*lp == '}') {
            lp++;
            return;
        }
        if (!*lp)
            lerr("unterminated SECTIONS");
        if (*lp == ';') {
            lp++;
            continue;
        }
        if ((c = lcommon()) == 0) {
            const char *w = lword(1), *save;
            lskip();
            save = lp;
            if ((*lp == '=' && lp[1] != '=') || (lp[1] == '=' && strchr("+-*/&|", *lp)) ||
                ((lp[0] == '<' || lp[0] == '>') && lp[1] == lp[0] && lp[2] == '=')) {
                c = lassign(w, 0);
                lexpect(";");
            } else {
                lp = save;
                c = loutput(w);
            }
        }
        *tail = c;
        tail = &c->next;
    }
}

static void lphdrs_block(void)
{
    lexpect("{");
    for (;;) {
        LPhdr *p;
        const char *t;
        lskip();
        if (*lp == '}') {
            lp++;
            return;
        }
        if (nlphdrs == 8)
            lerr("too many program headers");
        p = &lphdrs[nlphdrs++];
        memset(p, 0, sizeof *p);
        p->name = lword(0);
        t = lword(0);
        p->type = strcmp(t, "PT_LOAD") == 0 ? 1 : strcmp(t, "PT_DYNAMIC") == 0 ? 2 : strcmp(t, "PT_NOTE") == 0 ? 4 :
                  strcmp(t, "PT_GNU_STACK") == 0 ? 0x6474e551 : strcmp(t, "PT_GNU_EH_FRAME") == 0 ? 0x6474e550 :
                  strcmp(t, "PT_PHDR") == 0 ? 6 : strcmp(t, "PT_INTERP") == 0 ? 3 : strcmp(t, "PT_TLS") == 0 ? 7 : 0;
        for (;;) {
            lskip();
            if (*lp == ';') {
                lp++;
                break;
            }
            if (lkeyword("FLAGS")) {
                LExpr *e = lexpr();
                lexpect(")");
                if (e->op != X_NUM)
                    lerr("FLAGS must be a number");
                p->flags = (u32)e->num;
                p->has_flags = 1;
            } else {
                const char *w = lword(0);
                if (strcmp(w, "FILEHDR") == 0)
                    p->filehdr = 1;
            }
        }
    }
}

static void lversion_block(void)
{
    lexpect("{");
    for (;;) {
        LVersion *v;
        int global = 1;
        lskip();
        if (*lp == '}') {
            lp++;
            return;
        }
        if (nlversions == 4)
            lerr("too many versions");
        v = &lversions[nlversions++];
        memset(v, 0, sizeof *v);
        v->name = lword(0);
        lexpect("{");
        for (;;) {
            const char *w;
            lskip();
            if (*lp == '}') {
                lp++;
                break;
            }
            if (laccept("global:")) {
                global = 1;
                continue;
            }
            if (laccept("local:")) {
                global = 0;
                continue;
            }
            w = lword(2);
            lexpect(";");
            if (global && strcmp(w, "*") != 0) {
                v->globals = (const char **)xrealloc((void *)v->globals, (v->nglobals + 1) * sizeof(char *));
                v->globals[v->nglobals++] = w;
            }
        }
        while (*lp && *lp != ';') /* the version it inherits from */
            lp++;
        laccept(";");
    }
}

static void script_parse(const char *name, const char *text)
{
    LCmd **after = &script_after, *c;
    lp = text;
    lp_name = name;
    while (*after)
        after = &(*after)->next;
    for (;;) {
        lskip();
        if (!*lp)
            return;
        if (*lp == ';') {
            lp++;
            continue;
        }
        if (lkeyword("OUTPUT_ARCH") || lkeyword("OUTPUT_FORMAT") || lkeyword("TARGET") || lkeyword("SEARCH_DIR")) {
            while (*lp && *lp != ')')
                lp++;
            lexpect(")");
        } else if (lkeyword("ENTRY")) {
            script_entry = lword(0);
            lexpect(")");
        } else if (strncmp(lp, "SECTIONS", 8) == 0 && !is_sym_char((u8)lp[8])) {
            lp += 8;
            lsections();
        } else if (strncmp(lp, "PHDRS", 5) == 0 && !is_sym_char((u8)lp[5])) {
            lp += 5;
            lphdrs_block();
        } else if (strncmp(lp, "VERSION", 7) == 0 && !is_sym_char((u8)lp[7])) {
            lp += 7;
            lversion_block();
        } else {
            if ((c = lcommon()) == 0) {
                c = lassign(lword(0), 0);
                lexpect(";");
            }
            *after = c;
            after = &c->next;
        }
    }
}

/* ---- linking as a script directs ---- */

typedef struct LVal {
    u64 v;
    int addr; /* an address, which moves with the image, rather than a plain number */
    Out *sec;
} LVal;

static u64 ldot, lheaders;
static Out *ldot_sec;
static int lchanged;

/* options of the link */
static int ld_shared, ld_symbolic, ld_orphan_warn;
static const char *ld_soname;

/* what the linker makes for a shared object; the first two are older than scripts */
enum { Y_GOT, Y_COMMON, Y_RELA, Y_DYNSYM, Y_DYNSTR, Y_HASH, Y_VERSYM, Y_VERDEF, Y_DYNAMIC, Y_COUNT };
static Sym **exports;
static u32 *export_ver, nexports;
static u32 soname_off, *version_off;

static LVal leval(LExpr *e)
{
    LVal a, b, r;
    Out *o;
    Sym *s;
    r.v = 0;
    r.addr = 0;
    r.sec = 0;
    switch (e->op) {
    case X_NUM:
        r.v = e->num;
        return r;
    case X_DOT:
        r.v = ldot;
        r.addr = 1;
        r.sec = ldot_sec;
        return r;
    case X_SYM:
        s = lookup(e->name, 1);
        if (!s->f)
            s->f = &synth;
        r.v = sym_addr(s);
        r.addr = s->def == DEFINED;
        r.sec = s->def == DEFINED ? (s->sec ? s->sec->out : s->osec) : 0;
        return r;
    case X_ADDR:
    case X_SIZEOF:
        o = find_out(e->name);
        if (e->op == X_SIZEOF) {
            r.v = o ? o->size : 0;
        } else {
            r.v = o ? o->addr : 0;
            r.addr = 1;
            r.sec = o;
        }
        return r;
    case X_HEADERS:
        r.v = lheaders;
        return r;
    case X_DEFINED:
        s = lookup(e->name, 0);
        r.v = s && s->def != UNDEF;
        return r;
    case X_ABSOLUTE:
        r = leval(e->a);
        r.addr = 0;
        r.sec = 0;
        return r;
    case X_ALIGN:
        a = leval(e->a);
        if (e->b)
            r = leval(e->b);
        else {
            r.v = ldot;
            r.addr = 1;
            r.sec = ldot_sec;
        }
        if (a.v > 1)
            r.v = align_up(r.v, a.v);
        return r;
    case X_NEG:
        r.v = 0 - leval(e->a).v;
        return r;
    case '~':
        r.v = ~leval(e->a).v;
        return r;
    case '!':
        r.v = !leval(e->a).v;
        return r;
    case '?':
        return leval(e->a).v ? leval(e->b) : leval(e->c);
    }
    a = leval(e->a);
    b = leval(e->b);
    switch (e->op) {
    case '+':
        r.v = a.v + b.v;
        r.addr = a.addr || b.addr;
        r.sec = a.sec ? a.sec : b.sec;
        break;
    case '-':
        r.v = a.v - b.v;
        if (a.addr && !b.addr) {
            r.addr = 1;
            r.sec = a.sec;
        }
        break;
    case '&':
    case '|':
        r.v = e->op == '&' ? a.v & b.v : a.v | b.v;
        if (a.addr && !b.addr) {
            r.addr = 1;
            r.sec = a.sec;
        }
        break;
    case '^': r.v = a.v ^ b.v; break;
    case '*': r.v = a.v * b.v; break;
    case '/': r.v = b.v ? a.v / b.v : 0; break;
    case '%': r.v = b.v ? a.v % b.v : 0; break;
    case X_SHL: r.v = a.v << (b.v & 63); break;
    case X_SHR: r.v = a.v >> (b.v & 63); break;
    case X_EQ: r.v = a.v == b.v; break;
    case X_NE: r.v = a.v != b.v; break;
    case '<': r.v = a.v < b.v; break;
    case '>': r.v = a.v > b.v; break;
    case X_LE: r.v = a.v <= b.v; break;
    case X_GE: r.v = a.v >= b.v; break;
    case X_AND: r.v = a.v && b.v; break;
    case X_OR: r.v = a.v || b.v; break;
    case X_MAX: r = a.v > b.v ? a : b; break;
    case X_MIN: r = a.v < b.v ? a : b; break;
    }
    return r;
}

static u64 lapply(int op, u64 old, u64 v)
{
    switch (op) {
    case '+': return old + v;
    case '-': return old - v;
    case '*': return old * v;
    case '/': return v ? old / v : 0;
    case '&': return old & v;
    case '|': return old | v;
    case X_SHL: return old << (v & 63);
    case X_SHR: return old >> (v & 63);
    }
    return v;
}

static void lassign_run(LCmd *c)
{
    LVal v = leval(c->e);
    Sym *s;
    u8 def;
    if (strcmp(c->name, ".") == 0) {
        ldot = lapply(c->op, ldot, v.v);
        return;
    }
    if (!c->active)
        return;
    s = lookup(c->name, 1);
    if (c->op != '=') {
        v.v = lapply(c->op, sym_addr(s), v.v);
        v.addr = s->def == DEFINED;
        v.sec = s->osec;
    }
    def = (u8)(v.addr ? DEFINED : ABSOLUTE);
    if (s->def != def || s->value != v.v || s->sec || s->osec != v.sec)
        lchanged = 1;
    s->def = def;
    s->sec = 0;
    s->osec = v.sec;
    s->value = v.v;
    s->weak = 0;
    if (!s->f)
        s->f = &synth;
}

static int glob_match(const char *p, const char *s)
{
    for (;; p++, s++) {
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return 1;
            for (; *s; s++)
                if (glob_match(p, s))
                    return 1;
            return 0;
        }
        if (!*s || (*p != '?' && *p != *s))
            return !*p && !*s;
    }
}

static int cmp_sec_name(const void *a, const void *b)
{
    const Sec *x = *(Sec *const *)a, *y = *(Sec *const *)b;
    int c = strcmp(x->name, y->name);
    return c ? c : x < y ? -1 : x > y;
}

static void out_take(Out *o, Sec *s)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 16;
        o->secs = (Sec **)xrealloc(o->secs, o->cap * sizeof(Sec *));
    }
    o->secs[o->n++] = s;
    s->out = o;
    o->flags |= s->flags & 7;
    if (s->align > o->align)
        o->align = s->align;
    if (s->type != 8 && (o->type == 8 || !o->type)) {
        o->type = s->type;
        o->entsize = s->entsize;
    } else if (!o->type)
        o->type = 8;
}

static Out *new_out(const char *name)
{
    Out *o = (Out *)xmalloc(sizeof(Out));
    memset(o, 0, sizeof *o);
    o->name = name;
    o->align = 1;
    if (nouts == capouts) {
        capouts = capouts ? capouts * 2 : 32;
        outs = (Out **)xrealloc(outs, capouts * sizeof(Out *));
    }
    outs[nouts++] = o;
    return o;
}

static void cmd_take(LCmd *c, Sec *s, LCmd *o)
{
    if ((c->nsecs & (c->nsecs + 1)) == 0)
        c->secs = (Sec **)xrealloc(c->secs, (2 * c->nsecs + 2) * sizeof(Sec *));
    c->secs[c->nsecs++] = s;
    s->taken = 1;
    if (o->discard)
        s->out = 0;
    else
        out_take(o->out, s);
}

static u32 out_rank(Out *o)
{
    return !(o->flags & 2) ? 4 : o->flags & 4 ? 0 : !(o->flags & 1) ? 1 : o->type != 8 ? 2 : 3;
}

/* Gives every input section its place: the first pattern in the script that matches wins. */
static void script_assign(Sec **all, u32 nall)
{
    LCmd *o, *c, *last;
    u32 k, p, i;
    for (o = script; o; o = o->next) {
        if (o->kind != L_OUT)
            continue;
        if (!o->discard)
            o->out = new_out(o->name);
        for (c = o->sub; c; c = c->next) {
            int sorted = 0;
            if (c->kind != L_IN)
                continue;
            for (p = 0; p < c->npats; p++)
                sorted |= c->pats[p].sort;
            if (!sorted) {
                for (k = 0; k < nall; k++)
                    for (p = 0; p < c->npats && !all[k]->taken; p++)
                        if (glob_match(c->pats[p].pat, all[k]->name))
                            cmd_take(c, all[k], o);
            } else {
                for (p = 0; p < c->npats; p++) {
                    u32 from = c->nsecs;
                    for (k = 0; k < nall; k++)
                        if (!all[k]->taken && glob_match(c->pats[p].pat, all[k]->name))
                            cmd_take(c, all[k], o);
                    if (c->pats[p].sort && c->nsecs > from) {
                        qsort(c->secs + from, c->nsecs - from, sizeof(Sec *), cmp_sec_name);
                        for (i = from; i < c->nsecs && !o->discard; i++) /* the output section lists them the same way */
                            o->out->secs[o->out->n - (c->nsecs - i)] = c->secs[i];
                    }
                }
            }
        }
    }
    /* sections the script does not mention go after the last output section of their kind */
    for (k = 0; k < nall; k++) {
        Sec *s = all[k];
        u32 rank;
        if (s->taken || !(s->flags & 2) || !s->size)
            continue;
        rank = s->flags & 4 ? 0 : !(s->flags & 1) ? 1 : s->type != 8 ? 2 : 3;
        last = 0;
        for (o = script; o; o = o->next)
            if (o->kind == L_OUT && !o->discard) {
                if (strcmp(o->name, s->name) == 0 && o->provide) { /* an output section made for an earlier orphan */
                    last = o;
                    break;
                }
                if (out_rank(o->out) == rank && o->out->n)
                    last = o;
            }
        if (last && last->provide && strcmp(last->name, s->name) == 0) {
            o = last;
        } else {
            o = lcmd(L_OUT);
            o->name = s->name;
            o->provide = 1; /* marks the ones made here */
            o->out = new_out(s->name);
            o->sub = lcmd(L_IN);
            if (last) {
                o->next = last->next;
                last->next = o;
            } else {
                LCmd **tail = &script;
                while (*tail)
                    tail = &(*tail)->next;
                *tail = o;
            }
            if (ld_orphan_warn)
                fprintf(stderr, "mcc ld: warning: %s: section %s is not placed by the script\n", s->f->name, s->name);
        }
        cmd_take(o->sub, s, o);
    }
}

static void script_layout(void)
{
    LCmd *c, *d;
    u32 i;
    ldot = 0;
    ldot_sec = 0;
    for (c = script; c; c = c->next) {
        Out *o;
        u64 al, start;
        if (c->kind == L_ASSIGN)
            lassign_run(c);
        if (c->kind != L_OUT || c->discard)
            continue;
        o = c->out;
        if (c->addr)
            ldot = leval(c->addr).v;
        al = o->align;
        if (c->align) {
            u64 a = leval(c->align).v;
            if (a > al)
                al = a;
        }
        if (al > 1)
            ldot = align_up(ldot, al);
        start = ldot;
        if (o->addr != start)
            lchanged = 1;
        o->addr = start;
        ldot_sec = o;
        for (d = c->sub; d; d = d->next) {
            if (d->kind == L_ASSIGN)
                lassign_run(d);
            else if (d->kind == L_IN)
                for (i = 0; i < d->nsecs; i++) {
                    Sec *s = d->secs[i];
                    ldot = align_up(ldot, s->align);
                    s->off = ldot - start;
                    ldot += s->size;
                }
        }
        if (o->size != ldot - start)
            lchanged = 1;
        o->size = ldot - start;
        ldot_sec = 0;
    }
    for (c = script_after; c; c = c->next)
        if (c->kind == L_ASSIGN)
            lassign_run(c);
}

static void script_asserts(LCmd *c)
{
    for (; c; c = c->next) {
        if (c->kind == L_ASSERT && !leval(c->e).v)
            die("%s", c->name);
        if (c->kind == L_OUT)
            script_asserts(c->sub);
    }
}

static void script_activate(LCmd *c)
{
    for (; c; c = c->next) {
        if (c->kind == L_ASSIGN) {
            Sym *s = c->provide ? lookup(c->name, 0) : 0;
            c->active = !c->provide || (s && s->def == UNDEF);
        } else if (c->kind == L_OUT)
            script_activate(c->sub);
    }
}

/* Does this symbol table entry of a file stand for an address in the image? */
static int addr_like(File *f, u32 idx)
{
    u32 shndx;
    if (idx >= f->first_global)
        return f->gsyms[idx]->def == DEFINED;
    shndx = r16(f->symtab + 24 * idx + 6);
    return shndx && shndx < f->nsec && f->secs[shndx].out;
}

static u64 count_dynrel(void)
{
    u64 n = 0;
    u32 k, i, r;
    for (k = 0; k < nouts; k++)
        for (i = 0; i < outs[k]->n; i++) {
            Sec *s = outs[k]->secs[i];
            if (!(s->flags & 2))
                continue;
            for (r = 0; r < s->nrela; r++) {
                u64 info = r64(s->rela + 24 * r + 8);
                if ((u32)info == 257 && addr_like(s->f, (u32)(info >> 32)))
                    n++;
            }
        }
    for (k = 0; k < ngot; k++)
        if (got_syms[k]->def == DEFINED)
            n++;
    return n;
}

static u32 elf_hash(const char *name)
{
    u32 h = 0, g;
    for (; *name; name++) {
        h = (h << 4) + (u8)*name;
        g = h & 0xf0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static void synth_sec(int which, const char *name, u32 type, u64 flags, u64 size, u64 align, u64 entsize)
{
    Sec *s = &synth.secs[which];
    s->f = &synth;
    s->name = name;
    s->type = type;
    s->flags = flags;
    s->size = size;
    s->align = align;
    s->entsize = entsize;
    if (size && type != 8) {
        u8 *d = (u8 *)xmalloc((size_t)size);
        memset(d, 0, (size_t)size);
        s->data = d;
    }
}

static u64 synth_addr(int which)
{
    Sec *s = &synth.secs[which];
    return s->out ? s->out->addr + s->off : 0;
}

static u32 sym_shndx(Sym *s)
{
    Out *o = s->def != DEFINED ? 0 : s->sec ? s->sec->out : s->osec;
    if (s->def == UNDEF)
        return 0;
    return o && o->shndx ? o->shndx : 0xfff1;
}

void eld_link_script(const char *out_path, const char *script_path, const char *entry)
{
    File *f;
    Archive *a;
    Sym *s;
    Sec **all = 0;
    u32 nall = 0, i, k, nph, nsh, first_global, ndynamic = 0;
    u64 base = 0, first_addr = 0, alloc_end = 0, mem_end = 0, off, common_size = 0, symoff, stroff, shstroff, shoff;
    int changed, pass, bad = 0, have_alloc = 0, has_dynamic, filehdr = 0, rela_tags = 0;
    Buf out, symtab, strtab, shstr, dynstr;
    u32 *names;
    u8 *img, *h;
    size_t len;
    u8 *text = os_read_file(script_path, &len);
    LCmd *c;

    if (!text)
        die("cannot read %s", script_path);
    text = (u8 *)xrealloc(text, len + 1);
    text[len] = 0;
    script_parse(script_path, (const char *)text);
    do {
        File *last = 0;
        for (f = files; f; f = f->next)
            last = f;
        for (a = archives; a; a = a->next)
            each_member(a, 0);
        changed = last ? last->next != 0 : files != 0;
    } while (changed);

    /* what the linker adds */
    synth.name = "<linker>";
    synth.nsec = Y_COUNT;
    synth.secs = (Sec *)xmalloc(Y_COUNT * sizeof(Sec));
    memset(synth.secs, 0, Y_COUNT * sizeof(Sec));
    got_sec = &synth.secs[Y_GOT];
    common_sec = &synth.secs[Y_COMMON];
    for (f = files; f; f = f->next)
        for (i = 0; i < f->nsec; i++) {
            Sec *sc = &f->secs[i];
            if (!(sc->flags & 2))
                continue;
            for (k = 0; k < sc->nrela; k++) {
                u64 info = r64(sc->rela + 24 * k + 8);
                u32 type = (u32)info, idx = (u32)(info >> 32);
                if (type != 311 && type != 312)
                    continue;
                if (idx < f->first_global)
                    die("%s: an address slot for a local symbol", f->name);
                if (!f->gsyms[idx]->got) {
                    got_syms = (Sym **)xrealloc(got_syms, (ngot + 1) * sizeof(Sym *));
                    got_syms[ngot] = f->gsyms[idx];
                    f->gsyms[idx]->got = ++ngot;
                }
            }
        }
    synth_sec(Y_GOT, ".got", 1, 3, 8 * (u64)ngot, 8, 8);
    got_data = (u8 *)got_sec->data;
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next)
            if (s->def == COMMON) {
                u64 al = s->value ? s->value : 1;
                common_size = align_up(common_size, al);
                s->value = common_size;
                common_size += s->size;
                s->def = DEFINED;
                s->sec = common_sec;
            }
    synth_sec(Y_COMMON, "COMMON", 8, 3, common_size, 16, 0);
    memset(&dynstr, 0, sizeof dynstr);
    if (ld_shared) {
        /* the symbols other objects may use: those a version names, or all that are defined */
        if (nlversions) {
            for (k = 0; k < nlversions; k++)
                for (i = 0; i < lversions[k].nglobals; i++) {
                    s = lookup(lversions[k].globals[i], 0);
                    if (!s || s->def == UNDEF)
                        continue;
                    exports = (Sym **)xrealloc(exports, (nexports + 1) * sizeof(Sym *));
                    export_ver = (u32 *)xrealloc(export_ver, (nexports + 1) * sizeof(u32));
                    exports[nexports] = s;
                    export_ver[nexports++] = k + 2;
                }
        } else {
            for (i = 0; i < NHASH; i++)
                for (s = hash[i]; s; s = s->next)
                    if (s->def == DEFINED && !s->vis) {
                        exports = (Sym **)xrealloc(exports, (nexports + 1) * sizeof(Sym *));
                        export_ver = (u32 *)xrealloc(export_ver, (nexports + 1) * sizeof(u32));
                        exports[nexports] = s;
                        export_ver[nexports++] = 1;
                    }
        }
        buf_u8(&dynstr, 0);
        for (k = 0; k < nexports; k++) {
            exports[k]->idx = (u32)dynstr.len;
            buf_put(&dynstr, exports[k]->name, strlen(exports[k]->name) + 1);
        }
        if (ld_soname) {
            soname_off = (u32)dynstr.len;
            buf_put(&dynstr, ld_soname, strlen(ld_soname) + 1);
        }
        version_off = (u32 *)xmalloc((nlversions + 1) * sizeof(u32));
        for (k = 0; k < nlversions; k++) {
            version_off[k] = (u32)dynstr.len;
            buf_put(&dynstr, lversions[k].name, strlen(lversions[k].name) + 1);
        }
        dyn_relative = 1;
        synth_sec(Y_DYNSYM, ".dynsym", 11, 2, 24 * (u64)(nexports + 1), 8, 24);
        synth_sec(Y_DYNSTR, ".dynstr", 3, 2, dynstr.len, 1, 0);
        memcpy((u8 *)synth.secs[Y_DYNSTR].data, dynstr.p, dynstr.len);
        synth_sec(Y_HASH, ".hash", 5, 2, 4 * (u64)(2 + 2 * (nexports + 1)), 4, 4);
        if (nlversions) {
            synth_sec(Y_VERSYM, ".gnu.version", 0x6fffffff, 2, 2 * (u64)(nexports + 1), 2, 2);
            synth_sec(Y_VERDEF, ".gnu.version_d", 0x6ffffffd, 2, 28 * (u64)(nlversions + 1), 4, 0);
        }
    }

    /* every section that could go into the output, in the order of the command line */
    for (pass = 0; pass < 2; pass++) {
        k = 0;
        for (f = files; f; f = f->next)
            for (i = 1; i < f->nsec; i++) {
                Sec *sc = &f->secs[i];
                if (sc->type != 1 && sc->type != 8 && sc->type != 7 && sc->type != 14 && sc->type != 15 && sc->type != 16)
                    continue;
                if (pass)
                    all[k] = sc;
                k++;
            }
        for (i = 0; i < Y_COUNT; i++)
            if (synth.secs[i].name && i != Y_DYNAMIC && i != Y_RELA) {
                if (pass)
                    all[k] = &synth.secs[i];
                k++;
            }
        if (!pass)
            all = (Sec **)xmalloc((k + 3) * sizeof(Sec *));
        nall = k;
    }
    if (ld_shared) {
        /* the relocations for the loader and the table that points at everything: their sizes
           depend on what else is in the image, so their sections join the list last */
        rela_tags = 0;
        for (f = files; f && !rela_tags; f = f->next)
            for (i = 1; i < f->nsec && !rela_tags; i++)
                for (k = 0; k < f->secs[i].nrela; k++)
                    if ((u32)r64(f->secs[i].rela + 24 * k + 8) == 257 && (f->secs[i].flags & 2)) {
                        rela_tags = 1;
                        break;
                    }
        if (ngot)
            rela_tags = 1;
        synth_sec(Y_RELA, ".rela.dyn", 4, 2, 0, 8, 24);
        ndynamic = (ld_soname ? 1 : 0) + (ld_symbolic ? 1 : 0) + 5 + (rela_tags ? 3 : 0) + (nlversions ? 3 : 0) + 1;
        synth_sec(Y_DYNAMIC, ".dynamic", 6, 3, 16 * (u64)ndynamic, 8, 16);
        all[nall++] = &synth.secs[Y_RELA];
        all[nall++] = &synth.secs[Y_DYNAMIC];
    }
    script_assign(all, nall);
    script_activate(script);
    script_activate(script_after);
    has_dynamic = ld_shared && synth.secs[Y_DYNAMIC].out;
    for (k = 0; k < nlphdrs; k++)
        filehdr |= lphdrs[k].filehdr;
    nph = nlphdrs ? nlphdrs : 2 + (u32)has_dynamic;
    lheaders = 64 + 56 * (u64)nph;

    /* places: symbols may be used before the script gives them a value, so go round until nothing moves */
    for (pass = 0;; pass++) {
        lchanged = 0;
        if (ld_shared)
            synth.secs[Y_RELA].size = 24 * count_dynrel();
        script_layout();
        if (!lchanged && pass)
            break;
        if (pass == 30)
            die("%s: the layout does not settle", script_path);
    }
    script_asserts(script);
    script_asserts(script_after);
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next)
            if (s->def == UNDEF && !s->weak) {
                fprintf(stderr, "mcc ld: undefined symbol: %s (first needed by %s)\n", s->name, s->f ? s->f->name : "?");
                bad = 1;
            }
    if (bad)
        die("link failed");

    /* which output sections are written, and where in the file */
    nsh = 1;
    for (c = script; c; c = c->next) {
        Out *o;
        if (c->kind != L_OUT || c->discard)
            continue;
        o = c->out;
        if (!o->size && !c->has_symbols)
            continue;
        if (!o->type)
            o->type = 1;
        if (!o->n && !(o->flags & 2) && !c->addr)
            o->flags |= 2; /* empty but holding symbols: it is part of the image */
        o->shndx = nsh++;
        if ((o->flags & 2) && !have_alloc) {
            have_alloc = 1;
            first_addr = o->addr;
        }
    }
    if (filehdr || ld_shared) {
        if (first_addr < lheaders)
            die("%s: no room for the headers before the first section", script_path);
        base = (first_addr - lheaders) & ~(u64)(PAGE - 1);
        off = 0;
    } else {
        base = first_addr & ~(u64)(PAGE - 1);
        off = PAGE;
    }
    alloc_end = off + lheaders;
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        if (!o->shndx || !(o->flags & 2))
            continue;
        if (o->addr < base)
            die("%s: section %s lies before the start of the image", script_path, o->name);
        o->off = off + (o->addr - base);
        if (o->type != 8 && o->off + o->size > alloc_end)
            alloc_end = o->off + o->size;
        if (o->addr + o->size > mem_end)
            mem_end = o->addr + o->size;
    }
    memset(&out, 0, sizeof out);
    buf_zero(&out, (size_t)alloc_end);
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        if (!o->shndx || (o->flags & 2))
            continue;
        o->off = out.len;
        if (o->type != 8)
            buf_zero(&out, (size_t)o->size);
    }
    img = out.p;

    /* the tables of a shared object */
    if (ld_shared) {
        u8 *d = (u8 *)synth.secs[Y_DYNSYM].data, *hs = (u8 *)synth.secs[Y_HASH].data, *p;
        u32 n = nexports + 1;
        w32(hs, n);
        w32(hs + 4, n);
        for (k = 0; k < nexports; k++) {
            u8 *e = d + 24 * (k + 1);
            u32 shndx = sym_shndx(exports[k]), hv = elf_hash(exports[k]->name) % n;
            w32(e, exports[k]->idx);
            e[4] = (u8)((exports[k]->weak ? 2 : 1) << 4 | exports[k]->type);
            e[6] = (u8)shndx;
            e[7] = (u8)(shndx >> 8);
            w64(e + 8, sym_addr(exports[k]));
            w64(e + 16, exports[k]->size);
            w32(hs + 8 + 4 * n + 4 * (k + 1), r32(hs + 8 + 4 * hv)); /* chain on what the bucket held */
            w32(hs + 8 + 4 * hv, k + 1);
        }
        if (nlversions) {
            u8 *vs = (u8 *)synth.secs[Y_VERSYM].data, *vd = (u8 *)synth.secs[Y_VERDEF].data;
            for (k = 0; k < nexports; k++) {
                vs[2 * (k + 1)] = (u8)export_ver[k];
                vs[2 * (k + 1) + 1] = (u8)(export_ver[k] >> 8);
            }
            for (k = 0; k <= nlversions; k++) {
                const char *nm = k ? lversions[k - 1].name : ld_soname ? ld_soname : "";
                u8 *e = vd + 28 * k;
                e[0] = 1;                  /* vd_version */
                e[2] = (u8)(k ? 0 : 1);    /* vd_flags: the first stands for the file itself */
                e[4] = (u8)(k + 1);        /* vd_ndx */
                e[6] = 1;                  /* vd_cnt */
                w32(e + 8, elf_hash(nm));
                w32(e + 12, 20);           /* vd_aux */
                w32(e + 16, k < nlversions ? 28 : 0);
                w32(e + 20, k ? version_off[k - 1] : soname_off);
            }
        }
        p = (u8 *)synth.secs[Y_DYNAMIC].data;
#define DYN(tag, val) (w64(p, tag), w64(p + 8, val), p += 16)
        if (ld_soname)
            DYN(14, soname_off);
        if (ld_symbolic)
            DYN(30, 2);
        DYN(6, synth_addr(Y_DYNSYM));
        DYN(11, 24);
        DYN(5, synth_addr(Y_DYNSTR));
        DYN(10, dynstr.len);
        DYN(4, synth_addr(Y_HASH));
        if (rela_tags) {
            DYN(7, synth_addr(Y_RELA));
            DYN(8, synth.secs[Y_RELA].size);
            DYN(9, 24);
        }
        if (nlversions) {
            DYN(0x6ffffff0, synth_addr(Y_VERSYM));
            DYN(0x6ffffffc, synth_addr(Y_VERDEF));
            DYN(0x6ffffffd, nlversions + 1);
        }
#undef DYN
    }
    for (k = 0; k < ngot; k++) {
        if (dyn_relative && got_syms[k]->def == DEFINED) {
            buf_u64(&rela_dyn, synth_addr(Y_GOT) + 8 * k);
            buf_u64(&rela_dyn, 1027);
            buf_u64(&rela_dyn, sym_addr(got_syms[k]));
            if (!ld_apply_dyn)
                continue;
        }
        w64(got_data + 8 * k, sym_addr(got_syms[k]));
    }

    /* contents */
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        if (!o->shndx || o->type == 8)
            continue;
        for (i = 0; i < o->n; i++) {
            Sec *sc = o->secs[i];
            if (sc->type != 8 && sc->size && sc->data)
                memcpy(img + o->off + sc->off, sc->data, (size_t)sc->size);
        }
        for (i = 0; i < o->n; i++)
            if (o->secs[i]->nrela)
                reloc(o->secs[i], img + o->off + o->secs[i]->off);
    }
    if (ld_shared && synth.secs[Y_RELA].out) {
        Sec *sc = &synth.secs[Y_RELA];
        if (rela_dyn.len != sc->size)
            die("internal error: %lu bytes of relocations for the loader, room for %lu", (unsigned long)rela_dyn.len,
                (unsigned long)sc->size);
        if (sc->out->shndx && rela_dyn.len)
            memcpy(img + sc->out->off + sc->off, rela_dyn.p, rela_dyn.len);
    }

    /* symbol table */
    memset(&symtab, 0, sizeof symtab);
    memset(&strtab, 0, sizeof strtab);
    memset(&shstr, 0, sizeof shstr);
    buf_zero(&symtab, 24);
    buf_u8(&strtab, 0);
    for (f = files; f; f = f->next)
        for (i = 1; i < f->first_global && i < f->nsyms; i++) {
            const u8 *se = f->symtab + 24 * i;
            const char *nm = f->strtab + r32(se);
            u32 shndx = r16(se + 6);
            Sym *gs;
            if ((se[4] & 15) >= 3 || !*nm || (nm[0] == '.' && nm[1] == 'L') || shndx == 0 ||
                (shndx < 0xff00 && (!f->secs[shndx].out || !f->secs[shndx].out->shndx)))
                continue;
            put_sym(&symtab, &strtab, nm, se[4], 0, shndx >= 0xff00 ? 0xfff1 : f->secs[shndx].out->shndx,
                    ref_addr(f, i, &gs), r64(se + 16));
        }
    first_global = (u32)(symtab.len / 24);
    for (i = 0; i < NHASH; i++)
        for (s = hash[i]; s; s = s->next)
            put_sym(&symtab, &strtab, s->name, (u32)((s->weak ? 2 : 1) << 4 | s->type), s->vis, sym_shndx(s),
                    sym_addr(s), s->size);

    /* section headers */
    names = (u32 *)xmalloc((nsh + 4) * sizeof(u32));
    buf_u8(&shstr, 0);
    for (k = 0; k < nouts; k++)
        if (outs[k]->shndx) {
            names[outs[k]->shndx] = (u32)shstr.len;
            buf_put(&shstr, outs[k]->name, strlen(outs[k]->name) + 1);
        }
    names[nsh] = (u32)shstr.len;
    buf_put(&shstr, ".symtab", 8);
    names[nsh + 1] = (u32)shstr.len;
    buf_put(&shstr, ".strtab", 8);
    names[nsh + 2] = (u32)shstr.len;
    buf_put(&shstr, ".shstrtab", 10);
    buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
    symoff = out.len;
    buf_put(&out, symtab.p, symtab.len);
    stroff = out.len;
    buf_put(&out, strtab.p, strtab.len);
    shstroff = out.len;
    buf_put(&out, shstr.p, shstr.len);
    buf_zero(&out, (size_t)(align_up(out.len, 8) - out.len));
    shoff = out.len;
    buf_zero(&out, 64 * (nsh + 3));
    img = out.p;
    for (k = 0; k < nouts; k++) {
        Out *o = outs[k];
        u32 link = 0, info = 0;
        if (!o->shndx)
            continue;
        if (ld_shared) {
            Out *dsym = synth.secs[Y_DYNSYM].out, *dstr = synth.secs[Y_DYNSTR].out;
            if (o->type == 5 || o->type == 0x6fffffff || o->type == 4)
                link = dsym ? dsym->shndx : 0;
            else if (o->type == 11 || o->type == 6 || o->type == 0x6ffffffd)
                link = dstr ? dstr->shndx : 0;
            if (o->type == 11)
                info = 1;
            else if (o->type == 0x6ffffffd)
                info = nlversions + 1;
        }
        h = img + shoff + 64 * o->shndx;
        w32(h, names[o->shndx]);
        w32(h + 4, o->type);
        w64(h + 8, o->flags & 7);
        w64(h + 16, o->flags & 2 ? o->addr : 0);
        w64(h + 24, o->off);
        w64(h + 32, o->size);
        w32(h + 40, link);
        w32(h + 44, info);
        w64(h + 48, o->align);
        w64(h + 56, o->entsize);
    }
    h = img + shoff + 64 * nsh;
    w32(h, names[nsh]);
    w32(h + 4, 2);
    w64(h + 24, symoff);
    w64(h + 32, symtab.len);
    w32(h + 40, nsh + 1);
    w32(h + 44, first_global);
    w64(h + 48, 8);
    w64(h + 56, 24);
    h += 64;
    w32(h, names[nsh + 1]);
    w32(h + 4, 3);
    w64(h + 24, stroff);
    w64(h + 32, strtab.len);
    w64(h + 48, 1);
    h += 64;
    w32(h, names[nsh + 2]);
    w32(h + 4, 3);
    w64(h + 24, shstroff);
    w64(h + 32, shstr.len);
    w64(h + 48, 1);

    /* program headers */
    if (nlphdrs) {
        LCmd *prev = 0;
        for (c = script; c; c = c->next) {
            LCmd *src;
            Out *o;
            if (c->kind != L_OUT || c->discard)
                continue;
            src = c->nphdrs ? c : prev;
            if (c->nphdrs)
                prev = c; /* without a list of its own a section goes where the one before went, loadable segments only */
            o = c->out;
            if (!src || !o->shndx || !(o->flags & 2))
                continue;
            for (i = 0; i < (u32)src->nphdrs; i++)
                for (k = 0; k < nlphdrs; k++) {
                    LPhdr *p = &lphdrs[k];
                    if (strcmp(p->name, src->phdrs[i]) != 0 || (src != c && p->type != 1))
                        continue;
                    if (!p->used || o->addr < p->lo)
                        p->lo = o->addr;
                    if (!p->used || o->addr + o->size > p->hi)
                        p->hi = o->addr + o->size;
                    if (o->type != 8 && (!p->used || o->addr + o->size > p->fhi))
                        p->fhi = o->addr + o->size;
                    if (o->align > p->align)
                        p->align = o->align;
                    if (!p->has_flags)
                        p->flags |= 4 | (o->flags & 1 ? 2 : 0) | (o->flags & 4 ? 1 : 0);
                    p->used = 1;
                }
        }
        for (k = 0; k < nlphdrs; k++) {
            LPhdr *p = &lphdrs[k];
            u8 *ph = img + 64 + 56 * k;
            if (p->filehdr && p->used)
                p->lo = base;
            if (p->fhi < p->lo)
                p->fhi = p->lo;
            w32(ph, p->type);
            w32(ph + 4, p->flags);
            if (p->used) {
                w64(ph + 8, off + (p->lo - base));
                w64(ph + 16, p->lo);
                w64(ph + 24, p->lo);
                w64(ph + 32, p->fhi - p->lo);
                w64(ph + 40, p->hi - p->lo);
            }
            w64(ph + 48, p->type == 1 ? PAGE : p->align ? p->align : 1);
        }
    } else {
        u8 *ph = img + 64;
        u64 lo = filehdr || ld_shared ? base : first_addr, lo_off = off + (lo - base);
        w32(ph, 1);
        w32(ph + 4, 7);
        w64(ph + 8, lo_off);
        w64(ph + 16, lo);
        w64(ph + 24, lo);
        w64(ph + 32, alloc_end > lo_off ? alloc_end - lo_off : 0);
        w64(ph + 40, mem_end > lo ? mem_end - lo : 0);
        w64(ph + 48, PAGE);
        ph += 56;
        if (has_dynamic) {
            Sec *d = &synth.secs[Y_DYNAMIC];
            w32(ph, 2);
            w32(ph + 4, 6);
            w64(ph + 8, d->out->off + d->off);
            w64(ph + 16, synth_addr(Y_DYNAMIC));
            w64(ph + 24, synth_addr(Y_DYNAMIC));
            w64(ph + 32, d->size);
            w64(ph + 40, d->size);
            w64(ph + 48, 8);
            ph += 56;
        }
        w32(ph, 0x6474e551);
        w32(ph + 4, 6);
        w64(ph + 48, 16);
    }

    /* file header */
    if (!entry)
        entry = script_entry;
    s = entry ? lookup(entry, 0) : 0;
    memcpy(img, "\177ELF\2\1\1", 7);
    img[16] = (u8)(ld_shared ? 3 : 2);
    img[18] = (u8)machine();
    w32(img + 20, 1);
    w64(img + 24, s ? sym_addr(s) : 0);
    w64(img + 32, 64);
    w64(img + 40, shoff);
    img[52] = 64;
    img[54] = 56;
    img[56] = (u8)nph;
    img[58] = 64;
    img[60] = (u8)(nsh + 3);
    img[61] = (u8)((nsh + 3) >> 8);
    img[62] = (u8)(nsh + 2);
    img[63] = (u8)((nsh + 2) >> 8);
    os_write_file(out_path, out.p, out.len, 1);
}

/* ---- the ld command ---- */

int elf_ld_main(int argc, char **argv)
{
    const char *out = "a.out", *script_path = 0, *entry = 0, *libdirs[16];
    int i, whole = 0, relocatable_out = 0, ninputs = 0, version = 0, nlibdirs = 0;
    static const char *const ignored[] = {
        "-EL", "--no-undefined", "-X", "-n", "-N", "--start-group", "--end-group", "-(", "-)", "-nostdlib",
        "--strip-debug", "-S", "-s", "--gc-sections", "--no-gc-sections", "-static", "--fatal-warnings",
        "--warn-common", "--sort-common", "-d", "--no-dynamic-linker", "--discard-locals", "--discard-none",
        "-Bstatic", "--no-warn-mismatch", 0};
    static const char *const ignored_prefix[] = {"-m", "--build-id", "--hash-style", "--sort-section", "-O", "-Map=",
                                                 "-z", "--orphan-handling=place", 0};
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        int k;
        if (a[0] != '-') {
            eld_add_file(a, whole);
            ninputs++;
            continue;
        }
        for (k = 0; ignored[k]; k++)
            if (strcmp(a, ignored[k]) == 0)
                break;
        if (ignored[k])
            continue;
        if (strcmp(a, "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(a, "-T") == 0 && i + 1 < argc)
            script_path = argv[++i];
        else if (strncmp(a, "--script=", 9) == 0)
            script_path = a + 9;
        else if (strcmp(a, "-r") == 0 || strcmp(a, "--relocatable") == 0)
            relocatable_out = 1;
        else if (strcmp(a, "-shared") == 0 || strcmp(a, "--shared") == 0 || strcmp(a, "-pie") == 0)
            ld_shared = 1;
        else if (strcmp(a, "-Bsymbolic") == 0)
            ld_symbolic = 1;
        else if (strcmp(a, "--no-apply-dynamic-relocs") == 0)
            ld_apply_dyn = 0;
        else if (strcmp(a, "--apply-dynamic-relocs") == 0)
            ld_apply_dyn = 1;
        else if (strcmp(a, "--orphan-handling=warn") == 0)
            ld_orphan_warn = 1;
        else if (strncmp(a, "-soname=", 8) == 0)
            ld_soname = a + 8;
        else if (strncmp(a, "--soname=", 9) == 0)
            ld_soname = a + 9;
        else if ((strcmp(a, "-soname") == 0 || strcmp(a, "-h") == 0) && i + 1 < argc)
            ld_soname = argv[++i];
        else if ((strcmp(a, "-e") == 0 || strcmp(a, "--entry") == 0) && i + 1 < argc)
            entry = argv[++i];
        else if (strncmp(a, "--entry=", 8) == 0)
            entry = a + 8;
        else if (strcmp(a, "--whole-archive") == 0)
            whole = 1;
        else if (strcmp(a, "--no-whole-archive") == 0)
            whole = 0;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0)
            version = 1;
        else if ((strcmp(a, "-m") == 0 || strcmp(a, "-z") == 0 || strcmp(a, "-Map") == 0) && i + 1 < argc)
            i++;
        else if (a[1] == 'L' && nlibdirs < 16)
            libdirs[nlibdirs++] = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : ".";
        else if (a[1] == 'l') {
            const char *nm = a[2] ? a + 2 : i + 1 < argc ? argv[++i] : "";
            int d, found = 0;
            for (d = 0; d < nlibdirs && !found; d++) {
                char *path = (char *)xmalloc(strlen(libdirs[d]) + strlen(nm) + 8);
                size_t len;
                u8 *data;
                sprintf(path, "%s/lib%s.a", libdirs[d], nm);
                data = os_read_file(path, &len);
                if (data) {
                    eld_add(path, data, len, whole);
                    found = 1;
                }
            }
            if (!found)
                die("ld: cannot find -l%s", nm);
            ninputs++;
        } else {
            for (k = 0; ignored_prefix[k]; k++)
                if (strncmp(a, ignored_prefix[k], strlen(ignored_prefix[k])) == 0)
                    break;
            if (!ignored_prefix[k])
                die("ld: unknown option %s", a);
        }
    }
    if (version) {
        puts("GNU ld (mcc 0.9) 2.30");
        if (!ninputs)
            return 0;
    }
    if (!ninputs)
        die("ld: no input files");
    if (relocatable_out)
        eld_relocatable(out);
    else if (script_path)
        eld_link_script(out, script_path, entry);
    else
        eld_link(out, entry);
    return 0;
}
