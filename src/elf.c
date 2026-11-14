/* ELF relocatable object writer (aarch64, little endian, RELA). */
#include "mcc.h"
#include <stdlib.h>
#include <string.h>

static void u16le(Buf *b, u32 v)
{
    buf_u8(b, v & 0xff);
    buf_u8(b, (v >> 8) & 0xff);
}

static u32 str_add(Buf *tab, const char *s)
{
    u32 off = (u32)tab->len;
    buf_put(tab, s, strlen(s) + 1);
    return off;
}

static const char *sec_name(Obj *o, u32 i)
{
    static const char *const std[SEC_NSTORE] = {".text", ".rodata", ".data", ".bss", ".mcc.ir"};
    return i < SEC_NSTORE ? std[i] : intern_str(o->secname[i]);
}

/* The ELF type of a relocation; "page offset" depends on the instruction. */
static u32 rel_type(Obj *o, u32 sec, const ObjReloc *r)
{
    u32 insn;
    if (opt_arch == ARCH_X64) {
        switch (r->type) {
        case R_ABS64: return 1;
        case R_X64_PC32: case R_PREL32: return 2;
        case R_X64_PLT32: return 4;
        case R_X64_32: case R_ABS32: return 10;
        case R_X64_32S: return 11;
        case R_ABS16: return 12;
        case R_PREL16: return 13;
        case R_X64_PC8: return 15;
        case R_X64_TPOFF32: return 23;
        case R_PREL64: return 24;
        }
        die("internal: relocation type %u has no x86-64 ELF form", r->type);
    }
    switch (r->type) {
    case R_ABS64: return 257;
    case R_ABS32: return 258;
    case R_ABS16: return 259;
    case R_PREL64: return 260;
    case R_PREL32: return 261;
    case R_PREL16: return 262;
    case R_LIT19: return 273;
    case R_ADR21: return 274;
    case R_PAGE21: return 275;
    case R_TSTBR14: return 279;
    case R_CONDBR19: return 280;
    case R_JUMP26: return 282;
    case R_CALL26: return 283;
    case R_GOTPAGE21: return 311;
    case R_GOTPAGEOFF12: return 312;
    case R_TPREL_HI12: return 549;
    case R_TPREL_LO12: return 551;
    case R_PAGEOFF12:
        insn = get_u32le(o->sec[sec].p + r->off);
        if ((insn & 0x1f000000) == 0x11000000)
            return 277; /* add */
        if ((insn & 0x04800000) == 0x04800000)
            return 299; /* 128-bit load or store */
        switch (insn >> 30) {
        case 0: return 278;
        case 1: return 284;
        case 2: return 285;
        }
        return 286;
    }
    die("internal: relocation type %u has no ELF form", r->type);
    return 0;
}

void elf_write_obj(Buf *out, Obj *o)
{
    u32 nsec = o->nsec, i, k, nshdr, nlocal;
    u32 *shndx = (u32 *)xmalloc(nsec * sizeof(u32));   /* ELF section index of each section, 0 if left out */
    u32 *symidx = (u32 *)xmalloc((o->nsyms + 1) * sizeof(u32)); /* ELF symbol index, 0 if left out */
    u8 *used = (u8 *)xmalloc(o->nsyms + 1);
    u64 *secoff = (u64 *)xmalloc(nsec * sizeof(u64));
    Buf shstr, str, sym, *rela;
    u32 *shname, *relaname, *relaidx;
    u64 *relaoff;
    u64 symoff, stroff, shstroff, shoff;
    u32 symtab_idx, strtab_idx, shstr_idx, nsym, n_symtab, n_strtab, n_shstr, n_stack, stack_note;

    memset(&shstr, 0, sizeof shstr);
    memset(&str, 0, sizeof str);
    memset(&sym, 0, sizeof sym);
    buf_u8(&shstr, 0);
    buf_u8(&str, 0);
    memset(used, 0, o->nsyms + 1);
    for (i = 0; i < nsec; i++)
        for (k = 0; k < o->nrel[i]; k++)
{
            u32 ty = o->rel[i][k].type;
            /* 2: named by a thread-local relocation, so a thread-local symbol even if not defined here */
            if (ty == R_X64_TPOFF32 || ty == R_TPREL_HI12 || ty == R_TPREL_LO12)
                used[o->rel[i][k].sym] = 2;
            else if (!used[o->rel[i][k].sym])
                used[o->rel[i][k].sym] = 1;
        }

    /* which sections exist */
    nshdr = 1;
    for (i = 0; i < nsec; i++) {
        int keep = o->sec[i].len != 0 || o->nrel[i] != 0;
        if (i == SEC_IR && !o->sec[i].len)
            keep = 0;
        for (k = 0; !keep && k < o->nsyms; k++)
            keep = o->syms[k].sec == i;
        shndx[i] = keep ? nshdr++ : 0;
    }
    shname = (u32 *)xmalloc((nsec + 1) * sizeof(u32));
    relaname = (u32 *)xmalloc((nsec + 1) * sizeof(u32));
    relaidx = (u32 *)xmalloc((nsec + 1) * sizeof(u32));
    relaoff = (u64 *)xmalloc((nsec + 1) * sizeof(u64));
    rela = (Buf *)xmalloc((nsec + 1) * sizeof(Buf));
    memset(rela, 0, (nsec + 1) * sizeof(Buf));
    for (i = 0; i < nsec; i++) {
        relaidx[i] = 0;
        if (!shndx[i])
            continue;
        shname[i] = str_add(&shstr, sec_name(o, i));
        if (o->nrel[i]) {
            char tmp[300];
            const char *n = sec_name(o, i);
            if (strlen(n) > 280)
                die("section name too long");
            strcpy(tmp, ".rela");
            strcat(tmp, n);
            relaname[i] = str_add(&shstr, tmp);
            relaidx[i] = nshdr++;
        }
    }
    symtab_idx = nshdr++;
    strtab_idx = nshdr++;
    shstr_idx = nshdr++;
    /* an empty .note.GNU-stack says the code needs no executable stack (GNU ld warns without it) */
    stack_note = 1;
    for (i = 0; i < nsec; i++)
        if (shndx[i] && strcmp(sec_name(o, i), ".note.GNU-stack") == 0)
            stack_note = 0;
    nshdr += stack_note;

    /* symbols: null, one per section, locals, then the rest */
    buf_zero(&sym, 24);
    nsym = 1;
    for (i = 0; i < nsec; i++) {
        if (!shndx[i])
            continue;
        buf_u32(&sym, 0);
        buf_u8(&sym, 3); /* local section symbol */
        buf_u8(&sym, 0);
        u16le(&sym, shndx[i]);
        buf_u64(&sym, 0);
        buf_u64(&sym, 0);
        nsym++;
    }
    for (k = 0; k < o->nsyms; k++)
        symidx[k] = 0;
    {
        int pass;
        nlocal = 0;
        for (pass = 0; pass < 2; pass++) {
            for (k = 0; k < o->nsyms; k++) {
                ObjSym *s = &o->syms[k];
                int defined = s->sec < 0xfff0, global = s->global || s->weak, bind, type;
                u32 ndx;
                if (!s->name || s->sec == SEC_NONE)
                    continue; /* anonymous: relocations use the section symbol */
                if (s->sec == SEC_UNDEF) {
                    if (!used[k])
                        continue;
                    global = 1;
                }
                if (global != pass)
                    continue;
                if (defined && !shndx[s->sec])
                    die("internal: symbol in a section left out");
                bind = s->weak ? 2 : global ? 1 : 0;
                type = s->kind == 1 ? 2 : s->kind == 2 ? 1 : s->kind == 3 ? 6 : 0;
                if (s->sec == SEC_UNDEF && used[k] == 2)
                    type = 6;
                ndx = defined ? shndx[s->sec] : s->sec == SEC_COMMON ? 0xfff2 : 0;
                buf_u32(&sym, str_add(&str, intern_str(s->name)));
                buf_u8(&sym, (u32)(bind << 4 | type));
                buf_u8(&sym, s->hidden ? 2 : 0);
                u16le(&sym, ndx);
                buf_u64(&sym, s->sec == SEC_COMMON ? (u64)1 << s->align : defined ? s->value : 0);
                buf_u64(&sym, s->sec == SEC_COMMON ? s->value : s->size);
                symidx[k] = nsym++;
            }
            if (!pass)
                nlocal = nsym;
        }
    }

    /* relocations */
    for (i = 0; i < nsec; i++) {
        for (k = 0; k < o->nrel[i]; k++) {
            ObjReloc *r = &o->rel[i][k];
            ObjSym *s = &o->syms[r->sym];
            u32 si = symidx[r->sym];
            i64 addend = r->addend;
            if (!si || (!s->global && !s->weak && s->sec < 0xfff0)) {
                /* a local: the section's symbol and where in it */
                u32 j, n = 1;
                if (s->sec >= 0xfff0)
                    die("internal: relocation against a symbol that was dropped (%s)", intern_str(s->name));
                for (j = 0; j < s->sec; j++)
                    n += shndx[j] != 0;
                si = n;
                addend += (i64)s->value;
            }
            buf_u64(&rela[i], r->off);
            buf_u64(&rela[i], (u64)si << 32 | rel_type(o, i, r));
            buf_u64(&rela[i], (u64)addend);
        }
    }

    /* layout */
    {
        u64 off = 64;
        for (i = 0; i < nsec; i++) {
            if (!shndx[i])
                continue;
            off = align_up(off, o->align[i]);
            secoff[i] = off;
            if (!(o->secflags[i] & SF_NOBITS))
                off += o->sec[i].len;
        }
        for (i = 0; i < nsec; i++) {
            if (!relaidx[i])
                continue;
            off = align_up(off, 8);
            relaoff[i] = off;
            off += rela[i].len;
        }
        symoff = align_up(off, 8);
        stroff = symoff + sym.len;
        n_symtab = str_add(&shstr, ".symtab");
        n_strtab = str_add(&shstr, ".strtab");
        n_shstr = str_add(&shstr, ".shstrtab");
        n_stack = stack_note ? str_add(&shstr, ".note.GNU-stack") : 0;
        shstroff = stroff + str.len;
        shoff = align_up(shstroff + shstr.len, 8);
    }

    /* header */
    buf_put(out, "\177ELF\2\1\1\0\0\0\0\0\0\0\0\0", 16);
    u16le(out, 1);   /* ET_REL */
    u16le(out, opt_arch == ARCH_X64 ? 62 : 183); /* EM_X86_64, EM_AARCH64 */
    buf_u32(out, 1);
    buf_u64(out, 0);
    buf_u64(out, 0);
    buf_u64(out, shoff);
    buf_u32(out, 0);
    u16le(out, 64);
    u16le(out, 0);
    u16le(out, 0);
    u16le(out, 64);
    u16le(out, nshdr);
    u16le(out, shstr_idx);

    for (i = 0; i < nsec; i++) {
        if (!shndx[i])
            continue;
        buf_zero(out, (size_t)(secoff[i] - out->len));
        if (!(o->secflags[i] & SF_NOBITS))
            buf_put(out, o->sec[i].p, o->sec[i].len);
    }
    for (i = 0; i < nsec; i++) {
        if (!relaidx[i])
            continue;
        buf_zero(out, (size_t)(relaoff[i] - out->len));
        buf_put(out, rela[i].p, rela[i].len);
    }
    buf_zero(out, (size_t)(symoff - out->len));
    buf_put(out, sym.p, sym.len);
    buf_put(out, str.p, str.len);
    {
        buf_put(out, shstr.p, shstr.len);
        buf_zero(out, (size_t)(shoff - out->len));
        /* section headers, in index order: the sections, then their
           relocation sections, then the tables */
        buf_zero(out, 64);
        for (i = 0; i < nsec; i++) {
            u32 fl = o->secflags[i], ef = 0, type = fl & SF_NOBITS ? 8 : 1;
            const char *n;
            if (!shndx[i])
                continue;
            n = sec_name(o, i);
            if (strncmp(n, ".note", 5) == 0)
                type = 7;
            if (fl & SF_WRITE) ef |= 1;
            if (fl & SF_ALLOC) ef |= 2;
            if (fl & SF_EXEC) ef |= 4;
            if (fl & SF_MERGE) ef |= 0x10;
            if (fl & SF_STRINGS) ef |= 0x20;
            if (fl & SF_TLS) ef |= 0x400;
            buf_u32(out, shname[i]);
            buf_u32(out, type);
            buf_u64(out, ef);
            buf_u64(out, 0);
            buf_u64(out, secoff[i]);
            buf_u64(out, o->sec[i].len);
            buf_u32(out, 0);
            buf_u32(out, 0);
            buf_u64(out, o->align[i]);
            buf_u64(out, o->secent[i]);
        }
        for (i = 0; i < nsec; i++) {
            if (!relaidx[i])
                continue;
            buf_u32(out, relaname[i]);
            buf_u32(out, 4); /* SHT_RELA */
            buf_u64(out, 0x40); /* SHF_INFO_LINK */
            buf_u64(out, 0);
            buf_u64(out, relaoff[i]);
            buf_u64(out, rela[i].len);
            buf_u32(out, symtab_idx);
            buf_u32(out, shndx[i]);
            buf_u64(out, 8);
            buf_u64(out, 24);
        }
        buf_u32(out, n_symtab);
        buf_u32(out, 2);
        buf_u64(out, 0);
        buf_u64(out, 0);
        buf_u64(out, symoff);
        buf_u64(out, sym.len);
        buf_u32(out, strtab_idx);
        buf_u32(out, nlocal);
        buf_u64(out, 8);
        buf_u64(out, 24);

        buf_u32(out, n_strtab);
        buf_u32(out, 3);
        buf_u64(out, 0);
        buf_u64(out, 0);
        buf_u64(out, stroff);
        buf_u64(out, str.len);
        buf_u32(out, 0);
        buf_u32(out, 0);
        buf_u64(out, 1);
        buf_u64(out, 0);

        buf_u32(out, n_shstr);
        buf_u32(out, 3);
        buf_u64(out, 0);
        buf_u64(out, 0);
        buf_u64(out, shstroff);
        buf_u64(out, shstr.len);
        buf_u32(out, 0);
        buf_u32(out, 0);
        buf_u64(out, 1);
        buf_u64(out, 0);

        if (stack_note) {
            buf_u32(out, n_stack);
            buf_u32(out, 1);
            buf_u64(out, 0);
            buf_u64(out, 0);
            buf_u64(out, shstroff);
            buf_u64(out, 0);
            buf_u32(out, 0);
            buf_u32(out, 0);
            buf_u64(out, 1);
            buf_u64(out, 0);
        }
    }
    for (i = 0; i < nsec; i++)
        buf_free(&rela[i]);
    free(rela);
    free(shname);
    free(relaname);
    free(relaidx);
    free(relaoff);
    free(shndx);
    free(symidx);
    free(used);
    free(secoff);
    buf_free(&shstr);
    buf_free(&str);
    buf_free(&sym);
}
