/* Mach-O arm64 executable writer: load commands, rebase info, ad-hoc signature. */
#include "mcc.h"
#include <string.h>

#define PAGE 0x4000UL
#define BASE 0x100000000UL

#define LC_REQ_DYLD 0x80000000U
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_LOAD_DYLIB 0xc
#define LC_LOAD_DYLINKER 0xe
#define LC_UUID 0x1b
#define LC_CODE_SIGNATURE 0x1d
#define LC_DYLD_INFO_ONLY (0x22 | LC_REQ_DYLD)
#define LC_MAIN (0x28 | LC_REQ_DYLD)
#define LC_BUILD_VERSION 0x32

#define DYLD_PATH "/usr/lib/dyld"
#define LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"

#define CS_PAGE 4096
#define CS_HASH 32
#define CS_CD_HEADER 88
#define CS_SB_HEADER 12

const char *exe_dylibs[EXE_MAXLIBS], *exe_ents[EXE_MAXLIBS];
int exe_ndylibs, exe_nents;

/* -framework NAME, --dylib=PATH, --entitlement=KEY */
int exe_option(const char *arg, const char *next)
{
    if (strcmp(arg, "-framework") == 0 && next) {
        size_t n = strlen(next);
        char *path = (char *)xmalloc(2 * n + 64);
        strcpy(path, "/System/Library/Frameworks/");
        strcat(path, next);
        strcat(path, ".framework/Versions/A/");
        strcat(path, next);
        if (exe_ndylibs == EXE_MAXLIBS)
            die("too many libraries");
        exe_dylibs[exe_ndylibs++] = path;
        return 2;
    }
    if (strncmp(arg, "--dylib=", 8) == 0) {
        if (exe_ndylibs == EXE_MAXLIBS)
            die("too many libraries");
        exe_dylibs[exe_ndylibs++] = arg + 8;
        return 1;
    }
    if (strncmp(arg, "--entitlement=", 14) == 0) {
        if (exe_nents == EXE_MAXLIBS)
            die("too many entitlements");
        exe_ents[exe_nents++] = arg + 14;
        return 1;
    }
    return 0;
}

static size_t header_size(int has_data)
{
    int i;
    size_t n = 32;                                      /* mach_header_64 */
    n += 72;                                            /* __PAGEZERO */
    n += 72 + 80;                                       /* __TEXT, __text */
    if (has_data)
        n += 72 + 2 * 80;                               /* __DATA, __data, __bss */
    n += 72;                                            /* __LINKEDIT */
    n += 48;                                            /* LC_DYLD_INFO_ONLY */
    n += 24;                                            /* LC_SYMTAB */
    n += 80;                                            /* LC_DYSYMTAB */
    n += align_up(12 + sizeof(DYLD_PATH), 8);           /* LC_LOAD_DYLINKER */
    n += 24;                                            /* LC_UUID */
    n += 24;                                            /* LC_BUILD_VERSION */
    n += 24;                                            /* LC_MAIN */
    n += align_up(24 + sizeof(LIBSYSTEM_PATH), 8);      /* LC_LOAD_DYLIB */
    n += 16;                                            /* LC_CODE_SIGNATURE */
    for (i = 0; i < exe_ndylibs; i++)
        n += align_up(24 + strlen(exe_dylibs[i]) + 1, 8);
    return n;
}

void macho_exe_layout(size_t text_len, size_t data_len, size_t bss_len, ExeLayout *lay)
{
    u64 text_seg;
    lay->has_data = data_len + bss_len != 0;
    lay->text_off = align_up(header_size(lay->has_data), 16);
    lay->text_addr = BASE + lay->text_off;
    text_seg = align_up(lay->text_off + text_len, PAGE);
    lay->data_addr = BASE + text_seg;
    lay->bss_addr = lay->data_addr + align_up(data_len, 16);
}

static void put_name16(Buf *b, const char *name)
{
    char tmp[16];
    memset(tmp, 0, sizeof tmp);
    memcpy(tmp, name, strlen(name));
    buf_put(b, tmp, 16);
}

static void put_segment(Buf *b, const char *name, u64 vmaddr, u64 vmsize, u64 fileoff, u64 filesize, u32 prot,
                        u32 nsects)
{
    buf_u32(b, LC_SEGMENT_64);
    buf_u32(b, 72 + 80 * nsects);
    put_name16(b, name);
    buf_u64(b, vmaddr);
    buf_u64(b, vmsize);
    buf_u64(b, fileoff);
    buf_u64(b, filesize);
    buf_u32(b, prot); /* maxprot */
    buf_u32(b, prot); /* initprot */
    buf_u32(b, nsects);
    buf_u32(b, 0);
}

static void put_section(Buf *b, const char *sect, const char *seg, u64 addr, u64 size, u32 offset, u32 align,
                        u32 flags)
{
    put_name16(b, sect);
    put_name16(b, seg);
    buf_u64(b, addr);
    buf_u64(b, size);
    buf_u32(b, offset);
    buf_u32(b, align);
    buf_u32(b, 0); /* reloff */
    buf_u32(b, 0); /* nreloc */
    buf_u32(b, flags);
    buf_u32(b, 0);
    buf_u32(b, 0);
    buf_u32(b, 0);
}

static void put_path_cmd(Buf *b, u32 cmd, u32 fixed, const char *path, size_t path_size)
{
    size_t start = b->len;
    size_t total = (size_t)align_up(fixed + path_size, 8);
    buf_u32(b, cmd);
    buf_u32(b, (u32)total);
    buf_u32(b, fixed); /* offset of the string */
    if (cmd == LC_LOAD_DYLIB) {
        buf_u32(b, 2);          /* timestamp */
        buf_u32(b, 0x00010000); /* current version */
        buf_u32(b, 0x00010000); /* compatibility version */
    }
    buf_put(b, path, path_size);
    buf_zero(b, total - (b->len - start));
}

/* Rebase opcodes for pointers in __DATA that need sliding. */
static void build_rebase(Buf *b, const u64 *offs, size_t n, u32 segidx)
{
    size_t i = 0;
    if (n == 0)
        return;
    buf_u8(b, 0x10 | 1); /* SET_TYPE_IMM pointer */
    while (i < n) {
        size_t run = 1;
        while (i + run < n && offs[i + run] == offs[i] + 8 * run)
            run++;
        buf_u8(b, 0x20 | segidx); /* SET_SEGMENT_AND_OFFSET_ULEB */
        buf_uleb(b, offs[i]);
        if (run < 16) {
            buf_u8(b, 0x50 | (u32)run); /* DO_REBASE_IMM_TIMES */
        } else {
            buf_u8(b, 0x60); /* DO_REBASE_ULEB_TIMES */
            buf_uleb(b, run);
        }
        i += run;
    }
    buf_u8(b, 0); /* DONE */
    buf_align(b, 8);
}

/* Imported symbols: one pointer each, found by name in any loaded library. */
static void build_bind(Buf *b, const ExeImage *img, u32 segidx)
{
    size_t i;
    if (img->nbinds == 0)
        return;
    for (i = 0; i < img->nbinds; i++) {
        buf_u8(b, 0x30 | 0xe);     /* SET_DYLIB_SPECIAL_IMM: flat lookup, libSystem included */
        buf_u8(b, 0x40);           /* SET_SYMBOL_TRAILING_FLAGS_IMM */
        buf_put(b, img->bind_names[i], strlen(img->bind_names[i]) + 1);
        buf_u8(b, 0x50 | 1);       /* SET_TYPE_IMM pointer */
        buf_u8(b, 0x70 | segidx);  /* SET_SEGMENT_AND_OFFSET_ULEB */
        buf_uleb(b, img->binds[i]);
        buf_u8(b, 0x90); /* DO_BIND */
    }
    buf_u8(b, 0); /* DONE */
    buf_align(b, 8);
}

static void der_len(Buf *b, size_t n)
{
    if (n < 128)
        buf_u8(b, (u32)n);
    else if (n < 256) {
        buf_u8(b, 0x81);
        buf_u8(b, (u32)n);
    } else {
        buf_u8(b, 0x82);
        buf_u8(b, (u32)(n >> 8));
        buf_u8(b, (u32)(n & 0xff));
    }
}

static size_t der_size(size_t n) { return 1 + (n < 128 ? 1 : n < 256 ? 2 : 3) + n; }

/* The entitlements, each a key with the value true, as the two blobs a signature carries: a property list
   and its DER form. */
static void build_entitlements(Buf *xml, Buf *der)
{
    static const char head1[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC ";
    static const char head2[] = "\"-//Apple//DTD PLIST 1.0//EN\" \"https://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    static const char head3[] = "<plist version=\"1.0\"><dict>";
    static const char tail[] = "</dict></plist>";
    size_t body = 0, start;
    int i;
    buf_zero(xml, 8);
    buf_put(xml, head1, sizeof head1 - 1);
    buf_put(xml, head2, sizeof head2 - 1);
    buf_put(xml, head3, sizeof head3 - 1);
    for (i = 0; i < exe_nents; i++) {
        buf_put(xml, "<key>", 5);
        buf_put(xml, exe_ents[i], strlen(exe_ents[i]));
        buf_put(xml, "</key><true/>", 13);
        body += der_size(der_size(strlen(exe_ents[i])) + 3);
    }
    buf_put(xml, tail, sizeof tail - 1);
    put_u32be(xml->p, 0xfade7171);
    put_u32be(xml->p + 4, (u32)xml->len);

    buf_zero(der, 8);
    start = der->len;
    buf_u8(der, 0x70); /* [APPLICATION 16] */
    der_len(der, 3 + der_size(body));
    buf_u8(der, 0x02); /* INTEGER 1 */
    buf_u8(der, 1);
    buf_u8(der, 1);
    buf_u8(der, 0xb0); /* [CONTEXT 16] */
    der_len(der, body);
    for (i = 0; i < exe_nents; i++) {
        size_t n = strlen(exe_ents[i]);
        buf_u8(der, 0x30); /* SEQUENCE */
        der_len(der, der_size(n) + 3);
        buf_u8(der, 0x0c); /* UTF8String */
        der_len(der, n);
        buf_put(der, exe_ents[i], n);
        buf_u8(der, 0x01); /* BOOLEAN true */
        buf_u8(der, 1);
        buf_u8(der, 0xff);
    }
    (void)start;
    put_u32be(der->p, 0xfade7172);
    put_u32be(der->p + 4, (u32)der->len);
}

static size_t signature_size(size_t ident_size, size_t code_limit, const Buf *xml, const Buf *der)
{
    size_t nslots = (code_limit + CS_PAGE - 1) / CS_PAGE, nspecial = exe_nents ? 7 : 0;
    size_t nblobs = exe_nents ? 3 : 1;
    return CS_SB_HEADER + 8 * nblobs + CS_CD_HEADER + ident_size + (nspecial + nslots) * CS_HASH + xml->len + der->len;
}

static void put_signature(Buf *out, const char *ident, u64 text_seg_size, const Buf *xml, const Buf *der)
{
    size_t code_limit = out->len;
    size_t ident_size = strlen(ident) + 1;
    size_t nslots = (code_limit + CS_PAGE - 1) / CS_PAGE, nspecial = exe_nents ? 7 : 0;
    size_t nblobs = exe_nents ? 3 : 1, sb_header = CS_SB_HEADER + 8 * nblobs;
    size_t cd_size = CS_CD_HEADER + ident_size + (nspecial + nslots) * CS_HASH;
    size_t total = signature_size(ident_size, code_limit, xml, der);
    size_t start = out->len;
    size_t i;
    u8 *p, *sb;

    buf_zero(out, total);
    sb = p = out->p + start;
    put_u32be(p, 0xfade0cc0); /* embedded signature superblob */
    put_u32be(p + 4, (u32)total);
    put_u32be(p + 8, (u32)nblobs);
    put_u32be(p + 12, 0); /* slot: code directory */
    put_u32be(p + 16, (u32)sb_header);
    if (exe_nents) {
        put_u32be(p + 20, 5); /* slot: entitlements */
        put_u32be(p + 24, (u32)(sb_header + cd_size));
        put_u32be(p + 28, 7); /* slot: DER entitlements */
        put_u32be(p + 32, (u32)(sb_header + cd_size + xml->len));
        memcpy(sb + sb_header + cd_size, xml->p, xml->len);
        memcpy(sb + sb_header + cd_size + xml->len, der->p, der->len);
    }
    p += sb_header;
    put_u32be(p, 0xfade0c02); /* code directory */
    put_u32be(p + 4, (u32)cd_size);
    put_u32be(p + 8, 0x20400);                           /* version */
    put_u32be(p + 12, exe_nents ? 0x2 : 0x20002);        /* adhoc (| linker-signed) */
    put_u32be(p + 16, (u32)(CS_CD_HEADER + ident_size + nspecial * CS_HASH)); /* hashOffset */
    put_u32be(p + 20, CS_CD_HEADER);                     /* identOffset */
    put_u32be(p + 24, (u32)nspecial);
    put_u32be(p + 28, (u32)nslots);
    put_u32be(p + 32, (u32)code_limit);
    p[36] = CS_HASH;
    p[37] = 2;  /* SHA-256 */
    p[38] = 0;  /* platform */
    p[39] = 12; /* log2 page size */
    put_u64be(p + 64, 0); /* execSegBase */
    put_u64be(p + 72, text_seg_size);
    put_u64be(p + 80, 1); /* main binary */
    memcpy(p + CS_CD_HEADER, ident, ident_size);
    p += CS_CD_HEADER + ident_size + nspecial * CS_HASH;
    if (exe_nents) { /* special slots sit below the code hashes, slot -k at k hashes down */
        sha256(xml->p, xml->len, p - 5 * CS_HASH);
        sha256(der->p, der->len, p - 7 * CS_HASH);
    }
    for (i = 0; i < nslots; i++) {
        size_t off = i * CS_PAGE;
        size_t n = code_limit - off < CS_PAGE ? code_limit - off : CS_PAGE;
        sha256(out->p + off, n, p + i * CS_HASH);
    }
}

void macho_exe_write(Buf *out, const ExeImage *img)
{
    ExeLayout lay;
    Buf rebase, bind, xml, der;
    int di;
    u8 uuid[32];
    u64 text_seg, data_file, data_vm, link_off, link_addr, sig_off, sig_size, link_size, sym_off, str_off;
    size_t hsize, ncmds, ident_size;

    macho_exe_layout(img->text_len, img->data_len, img->bss_len, &lay);
    hsize = header_size(lay.has_data);
    ncmds = (size_t)(lay.has_data ? 13 : 12) + (size_t)exe_ndylibs;

    rebase.p = 0;
    rebase.len = rebase.cap = 0;
    build_rebase(&rebase, img->rebases, img->nrebases, 2);
    memset(&bind, 0, sizeof bind);
    memset(&xml, 0, sizeof xml);
    memset(&der, 0, sizeof der);
    build_bind(&bind, img, 2);
    if (exe_nents)
        build_entitlements(&xml, &der);

    text_seg = align_up(lay.text_off + img->text_len, PAGE);
    data_file = align_up(img->data_len, PAGE);
    data_vm = lay.has_data ? align_up(align_up(img->data_len, 16) + img->bss_len, PAGE) : 0;
    link_off = text_seg + data_file;
    link_addr = BASE + text_seg + data_vm;
    sym_off = align_up(link_off + rebase.len + bind.len, 8);
    str_off = sym_off + img->nsyms * 16;
    sig_off = align_up(str_off + (img->nsyms ? img->strtab_len : 0), 16);
    ident_size = strlen(img->ident) + 1;
    sig_size = signature_size(ident_size, (size_t)sig_off, &xml, &der);
    link_size = sig_off + sig_size - link_off;

    /* The UUID only has to differ when the contents do: a quick 128-bit
       mix of the image, not a second cryptographic hash of it. */
    {
        u64 h1 = 0x6a09e667f3bcc908UL, h2 = 0xbb67ae8584caa73bUL;
        int part;
        for (part = 0; part < 2; part++) {
            const u8 *p = part ? img->data : img->text;
            size_t n = part ? img->data_len : img->text_len, k;
            for (k = 0; k + 8 <= n; k += 8) {
                u64 w = (u64)p[k] | (u64)p[k + 1] << 8 | (u64)p[k + 2] << 16 | (u64)p[k + 3] << 24 |
                        (u64)p[k + 4] << 32 | (u64)p[k + 5] << 40 | (u64)p[k + 6] << 48 | (u64)p[k + 7] << 56;
                h1 = (h1 ^ w) * 0x9e3779b97f4a7c15UL;
                h1 = h1 << 31 | h1 >> 33;
                h2 = (h2 + (w ^ h1)) * 0xc2b2ae3d27d4eb4fUL;
                h2 ^= h2 >> 29;
            }
            for (; k < n; k++)
                h1 = (h1 ^ p[k]) * 0x100000001b3UL;
            h1 ^= (u64)n;
            h2 = (h2 ^ h1) * 0x9e3779b97f4a7c15UL;
            h2 ^= h2 >> 32;
        }
        h1 = (h1 ^ h2 >> 31) * 0xc2b2ae3d27d4eb4fUL;
        h1 ^= h1 >> 29;
        put_u64le(uuid, h1);
        put_u64le(uuid + 8, h2);
    }
    uuid[6] = (u8)((uuid[6] & 0x0f) | 0x40);
    uuid[8] = (u8)((uuid[8] & 0x3f) | 0x80);

    /* mach_header_64 */
    buf_u32(out, 0xfeedfacf);
    buf_u32(out, 0x0100000c); /* CPU_TYPE_ARM64 */
    buf_u32(out, 0);
    buf_u32(out, 2); /* MH_EXECUTE */
    buf_u32(out, (u32)ncmds);
    buf_u32(out, (u32)(hsize - 32));
    buf_u32(out, 0x200085); /* NOUNDEFS | DYLDLINK | TWOLEVEL | PIE */
    buf_u32(out, 0);

    put_segment(out, "__PAGEZERO", 0, BASE, 0, 0, 0, 0);
    put_segment(out, "__TEXT", BASE, text_seg, 0, text_seg, 5, 1);
    put_section(out, "__text", "__TEXT", lay.text_addr, img->text_len, (u32)lay.text_off, 4, 0x80000400);
    if (lay.has_data) {
        put_segment(out, "__DATA", lay.data_addr, data_vm, text_seg, data_file, 3, 2);
        put_section(out, "__data", "__DATA", lay.data_addr, img->data_len, img->data_len ? (u32)text_seg : 0, 4, 0);
        put_section(out, "__bss", "__DATA", lay.bss_addr, img->bss_len, 0, 4, 1);
    }
    put_segment(out, "__LINKEDIT", link_addr, align_up(link_size, PAGE), link_off, link_size, 1, 0);

    buf_u32(out, LC_DYLD_INFO_ONLY);
    buf_u32(out, 48);
    buf_u32(out, rebase.len ? (u32)link_off : 0);
    buf_u32(out, (u32)rebase.len);
    buf_u32(out, bind.len ? (u32)(link_off + rebase.len) : 0);
    buf_u32(out, (u32)bind.len);
    buf_zero(out, 24); /* weak bind, lazy bind, export */

    buf_u32(out, LC_SYMTAB);
    buf_u32(out, 24);
    buf_u32(out, (u32)sym_off);
    buf_u32(out, (u32)img->nsyms);
    buf_u32(out, (u32)str_off);
    buf_u32(out, (u32)img->strtab_len);

    buf_u32(out, LC_DYSYMTAB);
    buf_u32(out, 80);
    buf_u32(out, 0); /* local symbols: all of them */
    buf_u32(out, (u32)img->nsyms);
    buf_u32(out, (u32)img->nsyms); /* external */
    buf_u32(out, 0);
    buf_u32(out, (u32)img->nsyms); /* undefined */
    buf_u32(out, 0);
    buf_zero(out, 48);

    put_path_cmd(out, LC_LOAD_DYLINKER, 12, DYLD_PATH, sizeof(DYLD_PATH));

    buf_u32(out, LC_UUID);
    buf_u32(out, 24);
    buf_put(out, uuid, 16);

    buf_u32(out, LC_BUILD_VERSION);
    buf_u32(out, 24);
    buf_u32(out, 1);          /* PLATFORM_MACOS */
    buf_u32(out, 0x000b0000); /* minos 11.0 */
    buf_u32(out, 0x000b0000); /* sdk 11.0 */
    buf_u32(out, 0);          /* ntools */

    buf_u32(out, LC_MAIN);
    buf_u32(out, 24);
    buf_u64(out, lay.text_off + img->entry_off);
    buf_u64(out, 0);

    put_path_cmd(out, LC_LOAD_DYLIB, 24, LIBSYSTEM_PATH, sizeof(LIBSYSTEM_PATH));
    for (di = 0; di < exe_ndylibs; di++)
        put_path_cmd(out, LC_LOAD_DYLIB, 24, exe_dylibs[di], strlen(exe_dylibs[di]) + 1);

    buf_u32(out, LC_CODE_SIGNATURE);
    buf_u32(out, 16);
    buf_u32(out, (u32)sig_off);
    buf_u32(out, (u32)sig_size);

    if (out->len != hsize)
        die("internal: Mach-O header size mismatch (%lu != %lu)", (unsigned long)out->len, (unsigned long)hsize);

    buf_align(out, 16);
    buf_put(out, img->text, img->text_len);
    buf_align(out, PAGE);
    buf_put(out, img->data, img->data_len);
    buf_align(out, PAGE);
    buf_put(out, rebase.p, rebase.len);
    buf_put(out, bind.p, bind.len);
    buf_align(out, 8);
    if (img->nsyms) {
        buf_put(out, img->symtab, img->nsyms * 16);
        buf_put(out, img->strtab, img->strtab_len);
    }
    buf_align(out, 16);
    if (out->len != sig_off)
        die("internal: Mach-O signature offset mismatch");
    put_signature(out, img->ident, text_seg, &xml, &der);
    buf_free(&rebase);
    buf_free(&bind);
    buf_free(&xml);
    buf_free(&der);
}
