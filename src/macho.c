/* Mach-O relocatable objects (MH_OBJECT, arm64) and ar archives. */
#include "mcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MH_MAGIC_64 0xfeedfacfU
#define CPU_ARM64 0x0100000cU
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_BUILD_VERSION 0x32

enum {
    MR_UNSIGNED,
    MR_SUBTRACTOR,
    MR_BRANCH26,
    MR_PAGE21,
    MR_PAGEOFF12,
    MR_GOT_LOAD_PAGE21,
    MR_GOT_LOAD_PAGEOFF12,
    MR_POINTER_TO_GOT,
    MR_TLVP_LOAD_PAGE21,
    MR_TLVP_LOAD_PAGEOFF12,
    MR_ADDEND
};

typedef struct InSect {
    int kind; /* SEC_*, or -1 if ignored */
    u64 off;  /* offset in the merged section */
    u64 addr;
    u64 size;
    u32 reloff, nreloc;
} InSect;

static int classify(const u8 *sh)
{
    const char *sect = (const char *)sh;
    const char *seg = (const char *)sh + 16;
    u32 flags = get_u32le(sh + 64);
    u32 type = flags & 0xff;
    if (strncmp(seg, "__DWARF", 16) == 0 || strncmp(seg, "__LD", 16) == 0)
        return -1;
    if (strncmp(sect, "__eh_frame", 16) == 0)
        return -1;
    if (strncmp(seg, "__MCC", 16) == 0)
        return SEC_IR;
    if (type == 1 || type == 12) /* zerofill */
        return SEC_BSS;
    if (type >= 17 && type <= 21)
        die("thread-local storage is not supported");
    if (flags & 0x80000000U)
        return SEC_TEXT;
    if (strncmp(seg, "__TEXT", 16) == 0)
        return SEC_CONST;
    return SEC_DATA;
}

Obj *macho_read_obj(const char *name, const u8 *data, size_t len)
{
    Obj *o;
    InSect *sects = 0;
    u32 nsects = 0, ncmds, i, j;
    u32 symoff = 0, nsyms = 0, stroff = 0;
    u32 secsym[SEC_NSTORE];
    const u8 *lc;

    if (len < 32 || get_u32le(data) != MH_MAGIC_64 || get_u32le(data + 4) != CPU_ARM64 || get_u32le(data + 12) != 1)
        die("%s: not an arm64 Mach-O object file", name);
    o = obj_new(name);
    ncmds = get_u32le(data + 16);
    lc = data + 32;
    for (i = 0; i < ncmds; i++) {
        u32 cmd = get_u32le(lc), size = get_u32le(lc + 4);
        if (cmd == LC_SEGMENT_64) {
            u32 n = get_u32le(lc + 64);
            const u8 *sh = lc + 72;
            sects = (InSect *)xrealloc(sects, (nsects + n) * sizeof(InSect));
            for (j = 0; j < n; j++, sh += 80) {
                InSect *in = &sects[nsects++];
                u32 align = (u32)1 << get_u32le(sh + 52);
                in->kind = classify(sh);
                in->addr = get_u64le(sh + 32);
                in->size = get_u64le(sh + 40);
                in->reloff = get_u32le(sh + 56);
                in->nreloc = get_u32le(sh + 60);
                in->off = 0;
                if (in->kind < 0)
                    continue;
                in->off = obj_reserve(o, in->kind, in->size, align);
                if (in->kind != SEC_BSS)
                    buf_put(&o->sec[in->kind], data + get_u32le(sh + 48), (size_t)in->size);
            }
        } else if (cmd == LC_SYMTAB) {
            symoff = get_u32le(lc + 8);
            nsyms = get_u32le(lc + 12);
            stroff = get_u32le(lc + 16);
        }
        lc += size;
    }

    for (i = 0; i < nsyms; i++) {
        const u8 *nl = data + symoff + i * 16;
        const char *str = (const char *)data + stroff + get_u32le(nl);
        u32 type = nl[4], sect = nl[5];
        u64 value = get_u64le(nl + 8);
        u32 id;
        if (type & 0xe0) {
            obj_add_sym(o, 0, SEC_NONE, 0, 0);
        } else if ((type & 0x0e) == 0x0e) {
            InSect *in = &sects[sect - 1];
            if (in->kind < 0)
                obj_add_sym(o, 0, SEC_NONE, 0, 0);
            else
                obj_add_sym(o, intern_cstr(str), in->kind, value - in->addr + in->off, type & 1);
        } else if ((type & 0x0e) == 0) {
            if (value) {
                id = obj_add_sym(o, intern_cstr(str), SEC_COMMON, value, 1);
                o->syms[id].align = (u8)((get_u32le(nl + 4) >> 24) & 0xf);
            } else {
                obj_add_sym(o, intern_cstr(str), SEC_UNDEF, 0, 1);
            }
        } else {
            die("%s: unsupported symbol type 0x%x (%s)", name, type, str);
        }
    }

    for (i = 0; i < SEC_NSTORE; i++)
        secsym[i] = 0xffffffffU;
    for (i = 0; i < nsects; i++) {
        InSect *in = &sects[i];
        i64 pending = 0;
        if (in->kind < 0)
            continue;
        for (j = 0; j < in->nreloc; j++) {
            const u8 *r = data + in->reloff + j * 8;
            u32 w = get_u32le(r + 4);
            u32 off = (u32)(in->off + get_u32le(r));
            u32 symnum = w & 0xffffff, rlen = (w >> 25) & 3, ext = (w >> 27) & 1, type = w >> 28;
            int rtype = -1;
            switch (type) {
            case MR_ADDEND:
                pending = (i64)symnum;
                if (pending & 0x800000)
                    pending -= 0x1000000;
                continue;
            case MR_UNSIGNED: {
                u8 *p = o->sec[in->kind].p + off;
                u64 content;
                if (rlen != 3)
                    die("%s: unsupported 32-bit absolute relocation", name);
                content = get_u64le(p);
                put_u64le(p, 0);
                if (ext) {
                    obj_add_reloc(o, in->kind, off, R_ABS64, symnum, (i64)content);
                } else {
                    u32 k;
                    InSect *t = 0;
                    for (k = 0; k < nsects; k++)
                        if (sects[k].kind >= 0 && content >= sects[k].addr && content < sects[k].addr + sects[k].size)
                            t = &sects[k];
                    if (!t)
                        die("%s: relocation target outside any section", name);
                    if (secsym[t->kind] == 0xffffffffU)
                        secsym[t->kind] = obj_add_sym(o, 0, t->kind, 0, 0);
                    obj_add_reloc(o, in->kind, off, R_ABS64, secsym[t->kind], (i64)(content - t->addr + t->off));
                }
                continue;
            }
            case MR_BRANCH26:
                rtype = R_CALL26;
                break;
            case MR_PAGE21:
                rtype = R_PAGE21;
                break;
            case MR_PAGEOFF12:
                rtype = R_PAGEOFF12;
                break;
            case MR_GOT_LOAD_PAGE21:
                rtype = R_GOTPAGE21;
                break;
            case MR_GOT_LOAD_PAGEOFF12:
                rtype = R_GOTPAGEOFF12;
                break;
            default:
                die("%s: unsupported relocation type %u", name, type);
            }
            if (!ext)
                die("%s: unsupported section-relative relocation type %u", name, type);
            obj_add_reloc(o, in->kind, off, rtype, symnum, pending);
            pending = 0;
        }
    }
    free(sects);
    return o;
}

/* ---- writer ---- */

static void put_name16(Buf *b, const char *name)
{
    char tmp[16];
    memset(tmp, 0, sizeof tmp);
    memcpy(tmp, name, strlen(name));
    buf_put(b, tmp, 16);
}

static u32 log2u(u32 v)
{
    u32 n = 0;
    while (((u32)1 << n) < v)
        n++;
    return n;
}

void macho_write_obj(Buf *out, Obj *o)
{
    static const char *sectname[SEC_NSTORE] = { "__text", "__const", "__data", "__bss", "__ir" };
    static const char *segname[SEC_NSTORE] = { "__TEXT", "__TEXT", "__DATA", "__DATA", "__MCC" };
    static const u32 sectflags[SEC_NSTORE] = { 0x80000400U, 0, 0, 1, 0 };
    /* address order: the IR section, when there is one, sits before the bss */
    static const int place[SEC_NSTORE] = { SEC_TEXT, SEC_CONST, SEC_DATA, SEC_IR, SEC_BSS };
    const u32 ns = o->sec[SEC_IR].len ? SEC_NSTORE : SEC_COUNT;
    const int last = ns == SEC_NSTORE ? SEC_IR : SEC_DATA; /* last section with bytes in the file */
    u64 addr[SEC_NSTORE], fileoff[SEC_NSTORE];
    u32 reloff[SEC_NSTORE], nreloc[SEC_NSTORE];
    u32 *map = (u32 *)xmalloc((o->nsyms + 1) * sizeof(u32));
    u32 *order = (u32 *)xmalloc((o->nsyms + 1) * sizeof(u32));
    u32 nlocal = 0, nextdef = 0, nundef = 0, nout = 0;
    u32 i, k, pass;
    u64 a = 0, off, total_vm, symoff, stroff;
    const u32 hdr = 32 + 72 + ns * 80 + 24 + 80 + 24;
    Buf str;
    char tmp[32];

    str.p = 0;
    str.len = str.cap = 0;

    /* Symbol order required by LC_DYSYMTAB: locals, defined externals, undefined. */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < o->nsyms; i++) {
            ObjSym *s = &o->syms[i];
            int cls;
            if (s->sec == SEC_NONE)
                continue;
            cls = s->sec >= SEC_COUNT ? 2 : s->global ? 1 : 0;
            if (cls != (int)pass)
                continue;
            map[i] = nout;
            order[nout++] = i;
            if (cls == 0)
                nlocal++;
            else if (cls == 1)
                nextdef++;
            else
                nundef++;
        }
    }

    /* File layout mirrors the address layout, so filesize <= vmsize. */
    for (i = 0; i < SEC_NSTORE; i++) {
        k = (u32)place[i];
        if (k >= ns)
            continue;
        a = align_up(a, o->align[k]);
        addr[k] = a;
        a += o->sec[k].len;
        fileoff[k] = k == SEC_BSS ? 0 : align_up(hdr, 16) + addr[k];
    }
    off = fileoff[last] + o->sec[last].len;
    total_vm = a;
    off = align_up(off, 8);
    for (k = 0; k < ns; k++) {
        u32 n = 0;
        for (i = 0; i < o->nrel[k]; i++)
            n += (o->rel[k][i].type != R_ABS64 && o->rel[k][i].addend != 0) ? 2 : 1;
        nreloc[k] = n;
        reloff[k] = n ? (u32)off : 0;
        off += (u64)n * 8;
    }
    symoff = off;
    stroff = symoff + (u64)nout * 16;

    buf_u32(out, MH_MAGIC_64);
    buf_u32(out, CPU_ARM64);
    buf_u32(out, 0);
    buf_u32(out, 1); /* MH_OBJECT */
    buf_u32(out, 4);
    buf_u32(out, hdr - 32);
    buf_u32(out, 0);
    buf_u32(out, 0);

    buf_u32(out, LC_SEGMENT_64);
    buf_u32(out, 72 + ns * 80);
    put_name16(out, "");
    buf_u64(out, 0);
    buf_u64(out, total_vm);
    buf_u64(out, fileoff[SEC_TEXT]);
    buf_u64(out, fileoff[last] + o->sec[last].len - fileoff[SEC_TEXT]);
    buf_u32(out, 7);
    buf_u32(out, 7);
    buf_u32(out, ns);
    buf_u32(out, 0);
    for (k = 0; k < ns; k++) {
        put_name16(out, sectname[k]);
        put_name16(out, segname[k]);
        buf_u64(out, addr[k]);
        buf_u64(out, o->sec[k].len);
        buf_u32(out, (u32)fileoff[k]);
        buf_u32(out, log2u(o->align[k]));
        buf_u32(out, reloff[k]);
        buf_u32(out, nreloc[k]);
        buf_u32(out, sectflags[k]);
        buf_zero(out, 12);
    }

    buf_u32(out, LC_SYMTAB);
    buf_u32(out, 24);
    buf_u32(out, (u32)symoff);
    buf_u32(out, nout);
    buf_u32(out, (u32)stroff);
    buf_u32(out, 0); /* strsize, patched below */

    buf_u32(out, LC_DYSYMTAB);
    buf_u32(out, 80);
    buf_u32(out, 0);
    buf_u32(out, nlocal);
    buf_u32(out, nlocal);
    buf_u32(out, nextdef);
    buf_u32(out, nlocal + nextdef);
    buf_u32(out, nundef);
    buf_zero(out, 48);

    buf_u32(out, LC_BUILD_VERSION);
    buf_u32(out, 24);
    buf_u32(out, 1);
    buf_u32(out, 0x000b0000);
    buf_u32(out, 0x000b0000);
    buf_u32(out, 0);

    for (pass = 0; pass < SEC_NSTORE; pass++) {
        k = (u32)place[pass];
        if (k == SEC_BSS || k >= ns)
            continue;
        buf_zero(out, (size_t)(fileoff[k] - out->len));
        buf_put(out, o->sec[k].p, o->sec[k].len);
        /* absolute relocations keep their addend in the section contents */
        for (i = 0; i < o->nrel[k]; i++) {
            ObjReloc *r = &o->rel[k][i];
            if (r->type == R_ABS64)
                put_u64le(out->p + fileoff[k] + r->off, (u64)r->addend);
        }
    }
    buf_align(out, 8);
    for (k = 0; k < ns; k++) {
        for (i = 0; i < o->nrel[k]; i++) {
            ObjReloc *r = &o->rel[k][i];
            u32 type = 0, pcrel = 0, rlen = 2;
            switch (r->type) {
            case R_ABS64:
                type = MR_UNSIGNED;
                rlen = 3;
                break;
            case R_CALL26:
                type = MR_BRANCH26;
                pcrel = 1;
                break;
            case R_PAGE21:
                type = MR_PAGE21;
                pcrel = 1;
                break;
            case R_PAGEOFF12:
                type = MR_PAGEOFF12;
                break;
            case R_GOTPAGE21:
                type = MR_GOT_LOAD_PAGE21;
                pcrel = 1;
                break;
            case R_GOTPAGEOFF12:
                type = MR_GOT_LOAD_PAGEOFF12;
                break;
            }
            if (r->type != R_ABS64 && r->addend != 0) {
                buf_u32(out, r->off);
                buf_u32(out, ((u32)r->addend & 0xffffff) | (2U << 25) | ((u32)MR_ADDEND << 28));
            }
            buf_u32(out, r->off);
            buf_u32(out, map[r->sym] | (pcrel << 24) | (rlen << 25) | (1U << 27) | (type << 28));
        }
    }

    buf_u8(&str, 0);
    for (i = 0; i < nout; i++) {
        ObjSym *s = &o->syms[order[i]];
        const char *nm = intern_str(s->name);
        u32 type, sect = 0, desc = 0;
        u64 value = 0;
        if (!s->name) {
            sprintf(tmp, "l_mcc.%u", order[i]);
            nm = tmp;
        }
        if (s->sec < SEC_COUNT) {
            type = 0x0e | (s->global ? 1 : 0);
            sect = (u32)s->sec + 1;
            value = addr[s->sec] + s->value;
        } else {
            type = 0x01;
            if (s->sec == SEC_COMMON) {
                value = s->value;
                desc = (u32)s->align << 8;
            }
        }
        buf_u32(out, (u32)str.len);
        buf_u8(out, type);
        buf_u8(out, sect);
        buf_u8(out, desc & 0xff);
        buf_u8(out, desc >> 8);
        buf_u64(out, value);
        buf_put(&str, nm, strlen(nm) + 1);
    }
    buf_align(&str, 8);
    buf_put(out, str.p, str.len);
    put_u32le(out->p + 32 + 72 + ns * 80 + 20, (u32)str.len);
    buf_free(&str);
    free(map);
    free(order);
}

/* ---- file loading ---- */

static void push_obj(Obj ***objs, size_t *nobjs, Obj *o)
{
    if ((*nobjs & (*nobjs - 1)) == 0)
        *objs = (Obj **)xrealloc(*objs, (*nobjs ? *nobjs * 2 : 1) * sizeof(Obj *));
    (*objs)[(*nobjs)++] = o;
}

void macho_load_file(const char *path, Obj ***objs, size_t *nobjs)
{
    size_t len, pos;
    u8 *data = os_read_file(path, &len);
    if (!data)
        die("cannot read %s", path);
    if (len < 8 || memcmp(data, "!<arch>\n", 8) != 0) {
        push_obj(objs, nobjs, macho_read_obj(path, data, len));
        free(data);
        return;
    }
    pos = 8;
    while (pos + 60 <= len) {
        const char *h = (const char *)data + pos;
        char num[11];
        size_t size, skip = 0;
        const char *mname = h;
        size_t mlen = 16;
        memcpy(num, h + 48, 10);
        num[10] = 0;
        size = (size_t)strtoul(num, 0, 10);
        pos += 60;
        if (memcmp(h, "#1/", 3) == 0) {
            skip = (size_t)strtoul(h + 3, 0, 10);
            mname = (const char *)data + pos;
            mlen = skip;
        }
        while (mlen && (mname[mlen - 1] == 0 || mname[mlen - 1] == ' ' || mname[mlen - 1] == '/'))
            mlen--;
        if (size - skip >= 32 && get_u32le(data + pos + skip) == MH_MAGIC_64) {
            char *full = (char *)xmalloc(strlen(path) + mlen + 3);
            Obj *o;
            sprintf(full, "%s(%.*s)", path, (int)mlen, mname);
            o = macho_read_obj(full, data + pos + skip, size - skip);
            o->lazy = 1;
            push_obj(objs, nobjs, o);
        }
        pos += size + (size & 1);
    }
    free(data);
}

/* ---- archive writer: mcc ar [rcs] out.a member.o ... ---- */

int ar_main(int argc, char **argv)
{
    Buf out;
    int i = 0;
    const char *out_path;
    memset(&out, 0, sizeof out);
    if (argc && argv[0][0] != '/' && !strchr(argv[0], '.'))
        i++; /* traditional key letters such as "rcs" */
    if (argc - i < 2)
        die("usage: mcc ar [rcs] archive.a member.o ...");
    out_path = argv[i++];
    buf_put(&out, "!<arch>\n", 8);
    for (; i < argc; i++) {
        size_t len, nlen, padded;
        u8 *data = os_read_file(argv[i], &len);
        const char *name = strrchr(argv[i], '/');
        char hdr[61];
        if (!data)
            die("cannot read %s", argv[i]);
        name = name ? name + 1 : argv[i];
        nlen = strlen(name);
        padded = (size_t)align_up(nlen + 1, 8);
        sprintf(hdr, "#1/%-13lu%-12d%-6d%-6d%-8s%-10lu`\n", (unsigned long)padded, 0, 0, 0, "100644",
                (unsigned long)(padded + len));
        buf_put(&out, hdr, 60);
        buf_put(&out, name, nlen);
        buf_zero(&out, padded - nlen);
        buf_put(&out, data, len);
        if (out.len & 1)
            buf_u8(&out, '\n');
        free(data);
    }
    os_write_file(out_path, out.p, out.len, 0);
    return 0;
}
