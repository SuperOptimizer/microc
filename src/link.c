/* Linker: symbol resolution, layout, relocation, executable output. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Def {
    Obj *obj;
    u32 sym;
} Def;

static Def *defs;       /* by interned name: winning definition */
static Obj **lazy_defs; /* by interned name: archive member defining it */
static u64 *common_off; /* by interned name: bss offset of a common */
static Obj **order;
static size_t norder;
static u64 sec_addr[SEC_COUNT];

static void load_obj(Obj *o)
{
    u32 i, k;
    u8 *used = (u8 *)xmalloc(o->nsyms + 1);
    o->loaded = 1;
    order[norder++] = o;
    /* a symbol that is declared but never referred to (by a function that was not emitted, say) asks for nothing */
    memset(used, 0, o->nsyms + 1);
    for (k = 0; k < o->nsec; k++)
        for (i = 0; i < o->nrel[k]; i++)
            used[o->rel[k][i].sym] = 1;
    for (i = 0; i < o->nsyms; i++)
        if (o->syms[i].sec == SEC_UNDEF && !used[i]) {
            o->syms[i].sec = SEC_TEXT;
            o->syms[i].global = 0;
            o->syms[i].name = 0;
            o->syms[i].value = 0;
        }
    free(used);
    for (i = 0; i < o->nsyms; i++) {
        ObjSym *s = &o->syms[i];
        Def *d;
        if (!s->global || !s->name)
            continue;
        d = &defs[s->name];
        if (s->sec < SEC_COUNT) {
            if (d->obj && d->obj->syms[d->sym].sec != SEC_COMMON)
                die("duplicate symbol %s in %s and %s", intern_str(s->name), d->obj->name, o->name);
            d->obj = o;
            d->sym = i;
        } else if (s->sec == SEC_COMMON) {
            if (!d->obj || (d->obj->syms[d->sym].sec == SEC_COMMON && d->obj->syms[d->sym].value < s->value)) {
                d->obj = o;
                d->sym = i;
            }
        }
    }
}

static u64 sym_addr(Obj *o, u32 idx)
{
    ObjSym *s = &o->syms[idx];
    if (s->sec >= SEC_COUNT) {
        Def *d = &defs[s->name];
        if (s->sec == SEC_NONE || !d->obj)
            die("internal: unresolved symbol %s in %s", intern_str(s->name), o->name);
        o = d->obj;
        s = &o->syms[d->sym];
        if (s->sec == SEC_COMMON)
            return sec_addr[SEC_BSS] + common_off[s->name];
    }
    return sec_addr[s->sec] + o->base[s->sec] + s->value;
}

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

static void reloc_range(Obj *o, ObjReloc *r)
{
    die("%s: relocation out of range against %s", o->name, intern_str(o->syms[r->sym].name));
}

void link_exe(Obj **objs, size_t nobjs, const char *entry, const char *out_path)
{
    u32 nnames, entry_id = intern_cstr(entry);
    size_t i, nreb = 0, capreb = 0;
    u64 size[SEC_COUNT], const_base, *rebases = 0;
    u32 j, k;
    int errors = 0;
    ExeLayout lay;
    ExeImage img;
    Buf text, data, out, symtab, strtab;
    u32 nsyms = 0;
    const char *slash;
    Obj *imp = 0; /* stubs and pointers for the symbols a library will supply */
    const char **imp_names = 0;
    u64 *imp_offs = 0;
    size_t nimp = 0, capimp = 0;

    lto_link(objs, &nobjs, entry);
    nnames = intern_count();
    defs = (Def *)xmalloc(nnames * sizeof(Def));
    lazy_defs = (Obj **)xmalloc(nnames * sizeof(Obj *));
    common_off = (u64 *)xmalloc(nnames * sizeof(u64));
    order = (Obj **)xmalloc((nobjs + 1) * sizeof(Obj *));
    memset(defs, 0, nnames * sizeof(Def));
    memset(lazy_defs, 0, nnames * sizeof(Obj *));
    norder = 0;

    /* Resolve: explicit objects first, then archive members on demand. */
    for (i = 0; i < nobjs; i++) {
        Obj *o = objs[i];
        if (!o->lazy) {
            load_obj(o);
            continue;
        }
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *s = &o->syms[j];
            if (s->global && s->name && s->sec < SEC_COUNT && !lazy_defs[s->name])
                lazy_defs[s->name] = o;
        }
    }
    if (!defs[entry_id].obj && lazy_defs[entry_id])
        load_obj(lazy_defs[entry_id]);
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *s = &o->syms[j];
            if (s->sec != SEC_UNDEF)
                continue;
            if (!defs[s->name].obj && lazy_defs[s->name] && !lazy_defs[s->name]->loaded)
                load_obj(lazy_defs[s->name]);
        }
    }
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *s = &o->syms[j];
            if (s->sec == SEC_UNDEF && !defs[s->name].obj && exe_ndylibs) {
                /* an import: calls go to a stub that jumps through a pointer dyld binds */
                u32 slot, toff;
                u32 name = s->name;
                if (!imp) {
                    imp = obj_new("<imports>");
                    imp->align[SEC_TEXT] = 4;
                    imp->align[SEC_DATA] = 8;
                    imp->loaded = 1;
                }
                slot = obj_add_sym(imp, 0, SEC_DATA, imp->sec[SEC_DATA].len, 0);
                buf_zero(&imp->sec[SEC_DATA], 8);
                toff = (u32)imp->sec[SEC_TEXT].len;
                buf_u32(&imp->sec[SEC_TEXT], 0x90000010U); /* adrp x16, slot */
                buf_u32(&imp->sec[SEC_TEXT], 0xf9400210U); /* ldr x16, [x16, #slot] */
                buf_u32(&imp->sec[SEC_TEXT], 0xd61f0200U); /* br x16 */
                obj_add_reloc(imp, SEC_TEXT, toff, R_PAGE21, slot, 0);
                obj_add_reloc(imp, SEC_TEXT, toff + 4, R_PAGEOFF12, slot, 0);
                defs[name].obj = imp;
                defs[name].sym = obj_add_sym(imp, name, SEC_TEXT, toff, 1);
                if (nimp == capimp) {
                    capimp = capimp ? capimp * 2 : 16;
                    imp_names = (const char **)xrealloc((void *)imp_names, capimp * sizeof(char *));
                    imp_offs = (u64 *)xrealloc(imp_offs, capimp * sizeof(u64));
                }
                imp_names[nimp++] = intern_str(name);
                continue;
            }
            if (s->sec == SEC_UNDEF && !defs[s->name].obj) {
                fprintf(stderr, "mcc: undefined symbol %s (referenced from %s)\n", intern_str(s->name), o->name);
                defs[s->name].obj = o; /* report once */
                defs[s->name].sym = j;
                errors++;
            }
        }
    }
    if (!defs[entry_id].obj) {
        fprintf(stderr, "mcc: undefined entry point %s\n", entry);
        errors++;
    }
    if (errors)
        exit(1);
    if (imp)
        order[norder++] = imp;

    /* Layout. */
    for (k = 0; k < SEC_COUNT; k++)
        size[k] = 0;
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        for (k = 0; k < SEC_COUNT; k++) {
            size[k] = align_up(size[k], o->align[k]);
            o->base[k] = size[k];
            size[k] += o->sec[k].len;
        }
    }
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *s = &o->syms[j];
            if (s->sec == SEC_COMMON && defs[s->name].obj == o && defs[s->name].sym == j) {
                u64 al = (u64)1 << s->align;
                if (al < 8 && s->value >= 8)
                    al = 8;
                size[SEC_BSS] = align_up(size[SEC_BSS], al);
                common_off[s->name] = size[SEC_BSS];
                size[SEC_BSS] += s->value;
            }
        }
    }
    const_base = align_up(size[SEC_TEXT], 16);
    macho_exe_layout((size_t)(const_base + size[SEC_CONST]), (size_t)size[SEC_DATA], (size_t)size[SEC_BSS], &lay);
    sec_addr[SEC_TEXT] = lay.text_addr;
    sec_addr[SEC_CONST] = lay.text_addr + const_base;
    sec_addr[SEC_DATA] = lay.data_addr;
    sec_addr[SEC_BSS] = lay.bss_addr;

    /* Copy contents. */
    memset(&text, 0, sizeof text);
    memset(&data, 0, sizeof data);
    memset(&out, 0, sizeof out);
    buf_zero(&text, (size_t)(const_base + size[SEC_CONST]));
    buf_zero(&data, (size_t)size[SEC_DATA]);
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        if (o->sec[SEC_TEXT].len)
            memcpy(text.p + o->base[SEC_TEXT], o->sec[SEC_TEXT].p, o->sec[SEC_TEXT].len);
        if (o->sec[SEC_CONST].len)
            memcpy(text.p + const_base + o->base[SEC_CONST], o->sec[SEC_CONST].p, o->sec[SEC_CONST].len);
        if (o->sec[SEC_DATA].len)
            memcpy(data.p + o->base[SEC_DATA], o->sec[SEC_DATA].p, o->sec[SEC_DATA].len);
    }

    /* Relocate. */
    for (i = 0; i < norder; i++) {
        Obj *o = order[i];
        for (k = 0; k < SEC_BSS; k++) {
            u8 *secp = k == SEC_DATA ? data.p + o->base[k] : text.p + (k == SEC_CONST ? const_base : 0) + o->base[k];
            u64 secva = sec_addr[k] + o->base[k];
            for (j = 0; j < o->nrel[k]; j++) {
                ObjReloc *r = &o->rel[k][j];
                u8 *p = secp + r->off;
                u64 P = secva + r->off;
                u64 S = sym_addr(o, r->sym) + (u64)r->addend;
                u32 insn;
                i64 d;
                switch (r->type) {
                case R_ABS64:
                    if (k != SEC_DATA)
                        die("%s: absolute address in read-only section", o->name);
                    put_u64le(p, S);
                    if (nreb == capreb) {
                        capreb = capreb ? capreb * 2 : 256;
                        rebases = (u64 *)xrealloc(rebases, capreb * sizeof(u64));
                    }
                    rebases[nreb++] = P - lay.data_addr;
                    break;
                case R_CALL26:
                    d = (i64)(S - P);
                    if (d < -0x8000000L || d >= 0x8000000L || (d & 3))
                        reloc_range(o, r);
                    insn = get_u32le(p);
                    put_u32le(p, (insn & 0xfc000000U) | ((u32)(d >> 2) & 0x3ffffff));
                    break;
                case R_PAGE21:
                case R_GOTPAGE21:
                    d = (i64)(S >> 12) - (i64)(P >> 12);
                    if (d < -0x100000L || d >= 0x100000L)
                        reloc_range(o, r);
                    insn = get_u32le(p);
                    put_u32le(p, (insn & 0x9f00001fU) | (((u32)d & 3) << 29) | ((((u32)d >> 2) & 0x7ffff) << 5));
                    break;
                case R_GOTPAGEOFF12:
                    /* No GOT: the symbol is in this image, so turn
                       "ldr xN, [xM, #got]" into "add xN, xM, #lo12". */
                    insn = get_u32le(p);
                    if ((insn & 0xffc00000U) != 0xf9400000U)
                        die("%s: unexpected instruction for GOT load", o->name);
                    put_u32le(p, 0x91000000U | (((u32)S & 0xfff) << 10) | (insn & 0x3ff));
                    break;
                case R_PAGEOFF12: {
                    u32 lo = (u32)S & 0xfff, shift = 0;
                    insn = get_u32le(p);
                    if ((insn & 0x3b000000U) == 0x39000000U) { /* load/store, scaled offset */
                        shift = insn >> 30;
                        if ((insn & 0x04800000U) == 0x04800000U)
                            shift = 4; /* 128-bit vector */
                        if (lo & (((u32)1 << shift) - 1))
                            die("%s: misaligned access to %s", o->name, intern_str(o->syms[r->sym].name));
                    }
                    put_u32le(p, (insn & 0xffc003ffU) | ((lo >> shift) << 10));
                    break;
                }
                default:
                    die("internal: bad relocation type");
                }
            }
        }
    }
    if (nreb > 1)
        qsort(rebases, nreb, sizeof(u64), cmp_u64);

    memset(&img, 0, sizeof img);
    img.text = text.p;
    img.text_len = text.len;
    img.data = data.p;
    img.data_len = data.len;
    img.bss_len = (size_t)size[SEC_BSS];
    img.entry_off = sym_addr(defs[entry_id].obj, defs[entry_id].sym) - lay.text_addr;
    img.rebases = rebases;
    img.nrebases = nreb;
    for (i = 0; i < nimp; i++)
        imp_offs[i] = imp->base[SEC_DATA] + 8 * i;
    img.binds = imp_offs;
    img.bind_names = imp_names;
    img.nbinds = nimp;
    slash = strrchr(out_path, '/');
    img.ident = slash ? slash + 1 : out_path;

    /* symbol table: every named definition, as a local symbol */
    memset(&symtab, 0, sizeof symtab);
    memset(&strtab, 0, sizeof strtab);
    buf_u8(&strtab, ' ');
    buf_u8(&strtab, 0);
    for (k = 0; k < norder; k++) {
        Obj *o = order[k];
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *sy = &o->syms[j];
            const char *nm;
            if (!sy->name || sy->sec >= SEC_COUNT)
                continue;
            nm = intern_str(sy->name);
            if (nm[0] == 'l' && strncmp(nm, "l_mcc.", 6) == 0)
                continue; /* a private label that only got a name to survive an object file */
            buf_u32(&symtab, (u32)strtab.len);
            buf_u8(&symtab, 0x0e); /* N_SECT */
            buf_u8(&symtab, (u8)(sy->sec == SEC_TEXT || sy->sec == SEC_CONST ? 1 : sy->sec == SEC_DATA ? 2 : 3));
            buf_u8(&symtab, 0);
            buf_u8(&symtab, 0);
            buf_u64(&symtab, sym_addr(o, j));
            buf_put(&strtab, nm, strlen(nm) + 1);
            nsyms++;
        }
    }
    buf_align(&strtab, 8);
    img.symtab = symtab.p;
    img.nsyms = nsyms;
    img.strtab = strtab.p;
    img.strtab_len = strtab.len;
    macho_exe_write(&out, &img);
    buf_free(&symtab);
    buf_free(&strtab);
    os_write_file(out_path, out.p, out.len, 1);

    buf_free(&text);
    buf_free(&data);
    buf_free(&out);
    free(rebases);
    free(defs);
    free(lazy_defs);
    free(common_off);
    free(order);
}

int ld_main(int argc, char **argv)
{
    const char *out_path = "a.out", *entry = "_main";
    Obj **objs = 0;
    size_t nobjs = 0;
    int i, relocatable = 0, used;
    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc)
            entry = argv[++i];
        else if (strcmp(argv[i], "--rewrite") == 0)
            relocatable = 1; /* developer option: read one object, write it back */
        else if ((used = exe_option(argv[i], i + 1 < argc ? argv[i + 1] : 0)) != 0)
            i += used - 1;
        else if (argv[i][0] == '-')
            die("ld: unknown option %s", argv[i]);
        else
            macho_load_file(argv[i], &objs, &nobjs);
    }
    if (nobjs == 0)
        die("ld: no input files");
    if (relocatable) {
        Buf out;
        memset(&out, 0, sizeof out);
        macho_write_obj(&out, objs[0]);
        os_write_file(out_path, out.p, out.len, 0);
        return 0;
    }
    link_exe(objs, nobjs, entry, out_path);
    return 0;
}
