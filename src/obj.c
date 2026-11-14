/* In-memory relocatable object: sections, symbols, relocations. */
#include "mcc.h"
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
/* a compiler that runs on x86-64 Linux compiles for it unless told otherwise (see cc_main too) */
int opt_target = TARGET_LINUX;
int opt_arch = ARCH_X64;
#else
int opt_target = TARGET_MACOS;
int opt_arch = ARCH_ARM64;
#endif

static void grow(Obj *o, u32 cap)
{
    u32 old = o->capsec;
    o->sec = (Buf *)xrealloc(o->sec, cap * sizeof(Buf));
    o->align = (u32 *)xrealloc(o->align, cap * sizeof(u32));
    o->rel = (ObjReloc **)xrealloc(o->rel, cap * sizeof(ObjReloc *));
    o->nrel = (u32 *)xrealloc(o->nrel, cap * sizeof(u32));
    o->caprel = (u32 *)xrealloc(o->caprel, cap * sizeof(u32));
    o->secname = (u32 *)xrealloc(o->secname, cap * sizeof(u32));
    o->secflags = (u32 *)xrealloc(o->secflags, cap * sizeof(u32));
    o->secent = (u32 *)xrealloc(o->secent, cap * sizeof(u32));
    memset(o->sec + old, 0, (cap - old) * sizeof(Buf));
    memset(o->rel + old, 0, (cap - old) * sizeof(ObjReloc *));
    memset(o->nrel + old, 0, (cap - old) * sizeof(u32));
    memset(o->caprel + old, 0, (cap - old) * sizeof(u32));
    memset(o->secname + old, 0, (cap - old) * sizeof(u32));
    memset(o->secflags + old, 0, (cap - old) * sizeof(u32));
    memset(o->secent + old, 0, (cap - old) * sizeof(u32));
    for (; old < cap; old++)
        o->align[old] = 1;
    o->capsec = cap;
}

Obj *obj_new(const char *name)
{
    Obj *o = (Obj *)xmalloc(sizeof(Obj));
    memset(o, 0, sizeof *o);
    o->name = name;
    grow(o, 16);
    o->nsec = SEC_NSTORE;
    o->secflags[SEC_TEXT] = SF_ALLOC | SF_EXEC;
    o->secflags[SEC_CONST] = SF_ALLOC;
    o->secflags[SEC_DATA] = SF_ALLOC | SF_WRITE;
    o->secflags[SEC_BSS] = SF_ALLOC | SF_WRITE | SF_NOBITS;
    return o;
}

int obj_section(Obj *o, u32 name, u32 flags)
{
    static const char *const std[SEC_COUNT] = {".text", ".rodata", ".data", ".bss"};
    const char *n = intern_str(name);
    u32 k;
    for (k = 0; k < SEC_COUNT; k++)
        if (strcmp(n, std[k]) == 0)
            return (int)k;
    for (k = SEC_NSTORE; k < o->nsec; k++)
        if (o->secname[k] == name) {
            o->secflags[k] |= flags & (SF_WRITE | SF_EXEC);
            return (int)k;
        }
    return obj_section_new(o, name, flags);
}

/* Another section, even if one of that name exists. */
int obj_section_new(Obj *o, u32 name, u32 flags)
{
    if (o->nsec >= 0xfff0)
        die("too many sections");
    if (o->nsec == o->capsec)
        grow(o, o->capsec * 2);
    o->secname[o->nsec] = name;
    o->secflags[o->nsec] = flags;
    return (int)o->nsec++;
}

u32 obj_add_sym(Obj *o, u32 name, int sec, u64 value, int global)
{
    ObjSym *s;
    if (o->nsyms == o->capsyms) {
        o->capsyms = o->capsyms ? o->capsyms * 2 : 64;
        o->syms = (ObjSym *)xrealloc(o->syms, o->capsyms * sizeof(ObjSym));
    }
    s = &o->syms[o->nsyms];
    memset(s, 0, sizeof *s);
    s->value = value;
    s->name = name;
    s->sec = (u16)sec;
    s->global = (u8)global;
    return o->nsyms++;
}

/* The symbol of a linker name, 0 if there is none (else its index + 1). */
u32 obj_find_sym(Obj *o, u32 name)
{
    return name < o->capnames ? o->byname[name] : 0;
}

/* The symbol with a linker name: there is one of each. It starts out local and undefined. */
u32 obj_named_sym(Obj *o, u32 name)
{
    if (name >= o->capnames) {
        u32 cap = o->capnames ? o->capnames : 1024;
        while (cap <= name)
            cap *= 2;
        o->byname = (u32 *)xrealloc(o->byname, cap * sizeof(u32));
        memset(o->byname + o->capnames, 0, (cap - o->capnames) * sizeof(u32));
        o->capnames = cap;
    }
    if (!o->byname[name])
        o->byname[name] = obj_add_sym(o, name, SEC_UNDEF, 0, 0) + 1;
    return o->byname[name] - 1;
}

/* Gives the anonymous symbol sym a name by which assembly can refer to it. */
void obj_bind_name(Obj *o, u32 sym, u32 name)
{
    u32 probe = obj_named_sym(o, name); /* sizes the table; leaves a stray symbol if the name was new */
    (void)probe;
    o->syms[sym].name = name;
    o->byname[name] = sym + 1;
}

void obj_add_reloc(Obj *o, int sec, u32 off, int type, u32 sym, i64 addend)
{
    ObjReloc *r;
    if (o->nrel[sec] == o->caprel[sec]) {
        o->caprel[sec] = o->caprel[sec] ? o->caprel[sec] * 2 : 64;
        o->rel[sec] = (ObjReloc *)xrealloc(o->rel[sec], o->caprel[sec] * sizeof(ObjReloc));
    }
    r = &o->rel[sec][o->nrel[sec]++];
    r->addend = addend;
    r->off = off;
    r->sym = sym;
    r->type = (u32)type;
}

u64 obj_reserve(Obj *o, int sec, u64 size, u32 align)
{
    Buf *b = &o->sec[sec];
    u64 off = align_up(b->len, align);
    if (align > o->align[sec])
        o->align[sec] = align;
    if (o->secflags[sec] & SF_NOBITS) {
        b->len = (size_t)(off + size);
    } else {
        buf_zero(b, (size_t)(off - b->len));
        buf_reserve(b, (size_t)size);
    }
    return off;
}
