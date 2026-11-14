/* Debug information for ELF objects (-g): DWARF 5 line tables (.debug_line), compile units that name the
   functions and their code (.debug_info, .debug_abbrev, .debug_rnglists), and frame descriptions (.eh_frame)
   for x86-64, so that debuggers, profilers and stack traces see source lines and can walk the stack.

   The x86-64 back end reports each function once its code is final (dwarf_func): the offsets where the
   source location changes, and the frame events of its prologue and epilogues. dwarf_finish writes the
   sections when the unit is done. Strings are inline (DW_FORM_string), so the only relocations are the
   addresses of functions and the offsets of the sections into each other. */
#include "mcc.h"
#include <stdlib.h>
#include <string.h>

int opt_debug;

typedef struct DwFile {
    const char *name;
} DwFile;

typedef struct DwRow {
    u32 off, file, line, col;
} DwRow;

typedef struct DwFn {
    u32 sym, size;
    u32 file, line;     /* of the declaration */
    u32 row0, nrows;
    u32 cf0, ncf;
    u32 prologue_end;   /* offset of the first instruction after the prologue */
} DwFn;

static DwFile *files;
static u32 nfiles, capfiles, last_file;
static DwRow *rows;
static u32 nrows, caprows;
static DwCfi *cfis;
static u32 ncfis, capcfis;
static DwFn *fns;
static u32 nfns, capfns;
static u32 *floc; /* per object symbol: the location of its definition */
static u32 nfloc;
static const char *unit_path;
static char *cwd;

static void *grow(void *p, u32 *cap, u32 need, size_t size)
{
    if (need <= *cap)
        return p;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 64;
    return xrealloc(p, *cap * size);
}

/* File 0 of a DWARF 5 line table is the primary source file; it is listed again as file 1, which is what the
   rows use (some readers do not take file 0 in rows). */
void dwarf_reset(const char *path)
{
    nfiles = nrows = ncfis = nfns = 0;
    last_file = 0;
    unit_path = path;
    if (nfloc)
        memset(floc, 0, nfloc * sizeof(u32));
    files = (DwFile *)grow(files, &capfiles, 2, sizeof(DwFile));
    files[0].name = path;
    files[1].name = path;
    nfiles = 2;
}

void dwarf_func_loc(u32 sym, u32 loc)
{
    if (sym >= nfloc) {
        u32 n = nfloc ? nfloc : 256;
        while (n <= sym)
            n *= 2;
        floc = (u32 *)xrealloc(floc, n * sizeof(u32));
        memset(floc + nfloc, 0, (n - nfloc) * sizeof(u32));
        nfloc = n;
    }
    floc[sym] = loc;
}

u32 dwarf_get_func_loc(u32 sym)
{
    return sym < nfloc ? floc[sym] : 0;
}

/* The index of a file in the line table (1 for the unit's own), added when new. */
u32 dwarf_file(const char *name)
{
    u32 k;
    if (last_file && (files[last_file].name == name || strcmp(files[last_file].name, name) == 0))
        return last_file;
    for (k = 1; k < nfiles; k++)
        if (files[k].name == name || strcmp(files[k].name, name) == 0)
            return last_file = k;
    files = (DwFile *)grow(files, &capfiles, nfiles + 1, sizeof(DwFile));
    files[nfiles].name = name;
    return last_file = nfiles++;
}

const char *dwarf_file_name(u32 k)
{
    return k < nfiles ? files[k].name : "";
}

u32 dwarf_nfiles(void)
{
    return nfiles;
}

/* where: file index, line, column */
void dwarf_where(u32 loc, u32 *file, u32 *line, u32 *col)
{
    const char *name;
    loc_line(loc, &name, line, col);
    *file = dwarf_file(name);
}

void dwarf_func(u32 sym, u32 size, const u32 *off, const u32 *loc, u32 n, const DwCfi *cf, u32 ncf, u32 prologue_end)
{
    DwFn *fn;
    u32 k, l = dwarf_get_func_loc(sym);
    fns = (DwFn *)grow(fns, &capfns, nfns + 1, sizeof(DwFn));
    fn = &fns[nfns++];
    fn->sym = sym;
    fn->size = size;
    fn->file = fn->line = 0;
    if (l) {
        u32 col;
        dwarf_where(l, &fn->file, &fn->line, &col);
    }
    fn->prologue_end = prologue_end;
    fn->row0 = nrows;
    for (k = 0; k < n; k++) {
        DwRow r;
        if (!loc[k] || off[k] >= size)
            continue;
        r.off = off[k];
        dwarf_where(loc[k], &r.file, &r.line, &r.col);
        if (nrows > fn->row0) {
            DwRow *p = &rows[nrows - 1];
            if (p->off == r.off) { /* the code between went away (a jump to the next instruction) */
                *p = r;
                continue;
            }
            if (p->file == r.file && p->line == r.line && p->col == r.col)
                continue;
        }
        rows = (DwRow *)grow(rows, &caprows, nrows + 1, sizeof(DwRow));
        rows[nrows++] = r;
    }
    fn->nrows = nrows - fn->row0;
    fn->cf0 = ncfis;
    cfis = (DwCfi *)grow(cfis, &capcfis, ncfis + ncf + 1, sizeof(DwCfi));
    for (k = 0; k < ncf; k++)
        if (cf[k].off < size)
            cfis[ncfis++] = cf[k];
    fn->ncf = ncfis - fn->cf0;
}

/* ---- writing ---- */

static void sleb(Buf *b, i64 v)
{
    for (;;) {
        u32 byte = (u32)(v & 0x7f);
        v >>= 7; /* (arithmetic shift of a negative number: what every compiler mcc is built with does) */
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
            buf_u8(b, byte);
            return;
        }
        buf_u8(b, byte | 0x80);
    }
}

static void u16le(Buf *b, u32 v)
{
    buf_u8(b, v & 0xff);
    buf_u8(b, v >> 8 & 0xff);
}

static void str(Buf *b, const char *s)
{
    buf_put(b, s, strlen(s) + 1);
}

static void put32(Buf *b, size_t at, u32 v)
{
    b->p[at] = (u8)v;
    b->p[at + 1] = (u8)(v >> 8);
    b->p[at + 2] = (u8)(v >> 16);
    b->p[at + 3] = (u8)(v >> 24);
}

enum {
    DW_TAG_compile_unit = 0x11, DW_TAG_subprogram = 0x2e,
    DW_AT_name = 0x03, DW_AT_stmt_list = 0x10, DW_AT_low_pc = 0x11, DW_AT_high_pc = 0x12, DW_AT_language = 0x13,
    DW_AT_comp_dir = 0x1b, DW_AT_producer = 0x25, DW_AT_decl_file = 0x3a, DW_AT_decl_line = 0x3b,
    DW_AT_external = 0x3f, DW_AT_frame_base = 0x40, DW_AT_ranges = 0x55,
    DW_FORM_addr = 0x01, DW_FORM_data2 = 0x05, DW_FORM_data8 = 0x07, DW_FORM_string = 0x08, DW_FORM_udata = 0x0f,
    DW_FORM_sec_offset = 0x17, DW_FORM_exprloc = 0x18, DW_FORM_flag_present = 0x19,
    DW_LNCT_path = 1, DW_LNCT_directory_index = 2
};

#define LINE_BASE (-5)
#define LINE_RANGE 14
#define OPCODE_BASE 13

static Obj *O;
static int s_line, s_info, s_abbrev, s_rng, s_eh;
static u32 y_line, y_abbrev, y_rng; /* a symbol at the start of each, for offsets into it */

static Buf *sec(int k)
{
    return &O->sec[k];
}

static void reloc_here(int k, int type, u32 sym, i64 addend, u32 size)
{
    obj_add_reloc(O, k, (u32)sec(k)->len, type, sym, addend);
    buf_zero(sec(k), size);
}

static void line_program(void)
{
    Buf *b;
    size_t len_at, hdr_at;
    u32 k, r;
    static const u8 std_len[OPCODE_BASE - 1] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};

    b = sec(s_line);
    len_at = b->len;
    buf_u32(b, 0);
    u16le(b, 5);
    buf_u8(b, 8); /* address size */
    buf_u8(b, 0); /* segment selector size */
    hdr_at = b->len;
    buf_u32(b, 0);
    buf_u8(b, 1); /* minimum instruction length */
    buf_u8(b, 1); /* maximum operations per instruction */
    buf_u8(b, 1); /* default is_stmt */
    buf_u8(b, (u8)LINE_BASE);
    buf_u8(b, LINE_RANGE);
    buf_u8(b, OPCODE_BASE);
    buf_put(b, std_len, sizeof std_len);
    /* directories: the compilation's */
    buf_u8(b, 1);
    buf_uleb(b, DW_LNCT_path);
    buf_uleb(b, DW_FORM_string);
    buf_uleb(b, 1);
    str(b, cwd ? cwd : ".");
    /* files, each as it was named (relative ones are relative to that directory) */
    buf_u8(b, 2);
    buf_uleb(b, DW_LNCT_path);
    buf_uleb(b, DW_FORM_string);
    buf_uleb(b, DW_LNCT_directory_index);
    buf_uleb(b, DW_FORM_udata);
    buf_uleb(b, nfiles);
    for (k = 0; k < nfiles; k++) {
        str(b, files[k].name);
        buf_uleb(b, 0);
    }
    put32(b, hdr_at, (u32)(b->len - hdr_at - 4));

    /* a sequence of its own for each function: they may be in different sections */
    for (k = 0; k < nfns; k++) {
        DwFn *fn = &fns[k];
        u32 file = 1, line = 1, col = 0, addr = 0;
        int pe = fn->prologue_end != 0;
        buf_u8(b, 0);
        buf_uleb(b, 9);
        buf_u8(b, 2); /* DW_LNE_set_address */
        reloc_here(s_line, R_ABS64, fn->sym, 0, 8);
        b = sec(s_line);
        for (r = fn->row0; r < fn->row0 + fn->nrows; r++) {
            DwRow *w = &rows[r];
            i64 dl = (i64)w->line - (i64)line;
            u32 da = w->off - addr, op;
            if (w->file != file) {
                buf_u8(b, 4); /* DW_LNS_set_file */
                buf_uleb(b, w->file);
                file = w->file;
            }
            if (w->col != col) {
                buf_u8(b, 5); /* DW_LNS_set_column */
                buf_uleb(b, w->col);
                col = w->col;
            }
            if (pe && w->off >= fn->prologue_end) {
                buf_u8(b, 10); /* DW_LNS_set_prologue_end */
                pe = 0;
            }
            if (dl < LINE_BASE || dl >= LINE_BASE + LINE_RANGE) {
                buf_u8(b, 3); /* DW_LNS_advance_line */
                sleb(b, dl);
                dl = 0;
            }
            op = (u32)(dl - LINE_BASE) + LINE_RANGE * da + OPCODE_BASE;
            if (op > 255) {
                buf_u8(b, 2); /* DW_LNS_advance_pc */
                buf_uleb(b, da);
                op = (u32)(dl - LINE_BASE) + OPCODE_BASE;
            }
            buf_u8(b, op);
            line = w->line;
            addr = w->off;
        }
        if (fn->size > addr) {
            buf_u8(b, 2);
            buf_uleb(b, fn->size - addr);
        }
        buf_u8(b, 0);
        buf_uleb(b, 1);
        buf_u8(b, 1); /* DW_LNE_end_sequence */
    }
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

static void abbrevs(void)
{
    Buf *b = sec(s_abbrev);
    int ext;
    buf_uleb(b, 1);
    buf_uleb(b, DW_TAG_compile_unit);
    buf_u8(b, 1);
    buf_uleb(b, DW_AT_producer), buf_uleb(b, DW_FORM_string);
    buf_uleb(b, DW_AT_language), buf_uleb(b, DW_FORM_data2);
    buf_uleb(b, DW_AT_name), buf_uleb(b, DW_FORM_string);
    buf_uleb(b, DW_AT_comp_dir), buf_uleb(b, DW_FORM_string);
    buf_uleb(b, DW_AT_low_pc), buf_uleb(b, DW_FORM_addr);
    buf_uleb(b, DW_AT_ranges), buf_uleb(b, DW_FORM_sec_offset);
    buf_uleb(b, DW_AT_stmt_list), buf_uleb(b, DW_FORM_sec_offset);
    buf_u8(b, 0), buf_u8(b, 0);
    for (ext = 0; ext < 2; ext++) {
        buf_uleb(b, 2 + (u32)ext);
        buf_uleb(b, DW_TAG_subprogram);
        buf_u8(b, 0);
        buf_uleb(b, DW_AT_name), buf_uleb(b, DW_FORM_string);
        if (ext)
            buf_uleb(b, DW_AT_external), buf_uleb(b, DW_FORM_flag_present);
        buf_uleb(b, DW_AT_decl_file), buf_uleb(b, DW_FORM_udata);
        buf_uleb(b, DW_AT_decl_line), buf_uleb(b, DW_FORM_udata);
        buf_uleb(b, DW_AT_low_pc), buf_uleb(b, DW_FORM_addr);
        buf_uleb(b, DW_AT_high_pc), buf_uleb(b, DW_FORM_data8);
        buf_uleb(b, DW_AT_frame_base), buf_uleb(b, DW_FORM_exprloc);
        buf_u8(b, 0), buf_u8(b, 0);
    }
    buf_u8(b, 0);
}

static void ranges(void)
{
    Buf *b = sec(s_rng);
    size_t len_at = b->len;
    u32 k;
    buf_u32(b, 0);
    u16le(b, 5);
    buf_u8(b, 8);
    buf_u8(b, 0);
    buf_u32(b, 0); /* offset entry count */
    for (k = 0; k < nfns; k++) {
        buf_u8(sec(s_rng), 7); /* DW_RLE_start_length */
        reloc_here(s_rng, R_ABS64, fns[k].sym, 0, 8);
        buf_uleb(sec(s_rng), fns[k].size);
    }
    b = sec(s_rng);
    buf_u8(b, 0); /* DW_RLE_end_of_list */
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

static void info(const char *path)
{
    Buf *b = sec(s_info);
    size_t len_at = b->len;
    u32 k;
    buf_u32(b, 0);
    u16le(b, 5);
    buf_u8(b, 1); /* DW_UT_compile */
    buf_u8(b, 8);
    reloc_here(s_info, R_ABS32, y_abbrev, 0, 4);
    b = sec(s_info);
    buf_uleb(b, 1);
    str(b, "mcc");
    u16le(b, opt_cxx || opt_cuda ? 0x21 /* DW_LANG_C_plus_plus_14 */ : opt_c23 ? 0x1d /* DW_LANG_C11 */ : 0x0c /* C99 */);
    str(b, path);
    str(b, cwd ? cwd : ".");
    buf_zero(b, 8); /* the base address: the ranges are absolute */
    reloc_here(s_info, R_ABS32, y_rng, 12, 4); /* the list, after the 12-byte header */
    reloc_here(s_info, R_ABS32, y_line, 0, 4);
    for (k = 0; k < nfns; k++) {
        DwFn *fn = &fns[k];
        ObjSym *s = &O->syms[fn->sym];
        b = sec(s_info);
        buf_uleb(b, s->global || s->weak ? 3 : 2);
        str(b, s->name ? intern_str(s->name) : "?");
        buf_uleb(b, fn->file);
        buf_uleb(b, fn->line);
        reloc_here(s_info, R_ABS64, fn->sym, 0, 8);
        b = sec(s_info);
        buf_u64(b, fn->size);
        buf_uleb(b, 1);
        buf_u8(b, 0x9c); /* DW_OP_call_frame_cfa */
    }
    b = sec(s_info);
    buf_u8(b, 0);
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

/* x86-64 DWARF register numbers of the processor's 0..15 */
static const u8 x64_dwreg[16] = {0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};

static void cfa_advance(Buf *b, u32 *at, u32 to)
{
    u32 d = to - *at;
    if (!d)
        return;
    if (d < 64) {
        buf_u8(b, 0x40 | d); /* DW_CFA_advance_loc */
    } else if (d < 256) {
        buf_u8(b, 2); /* DW_CFA_advance_loc1 */
        buf_u8(b, d);
    } else if (d < 65536) {
        buf_u8(b, 3);
        u16le(b, d);
    } else {
        buf_u8(b, 4);
        buf_u32(b, d);
    }
    *at = to;
}

static void pad_entry(Buf *b, size_t len_at)
{
    while ((b->len - len_at) & 7)
        buf_u8(b, 0); /* DW_CFA_nop */
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

static void eh_frame(void)
{
    Buf *b = sec(s_eh);
    size_t cie_at = b->len, len_at;
    u32 k, c;
    /* the CIE: on entry the frame's address (CFA) is rsp + 8, and the return address is just below it */
    buf_u32(b, 0);
    buf_u32(b, 0); /* CIE id */
    buf_u8(b, 1);  /* version */
    str(b, "zR");
    buf_uleb(b, 1);  /* code alignment */
    sleb(b, -8);     /* data alignment */
    buf_uleb(b, 16); /* return address register */
    buf_uleb(b, 1);  /* augmentation data: */
    buf_u8(b, 0x1b); /* DW_EH_PE_pcrel | DW_EH_PE_sdata4 */
    buf_u8(b, 0x0c), buf_uleb(b, 7), buf_uleb(b, 8);  /* DW_CFA_def_cfa rsp, 8 */
    buf_u8(b, 0x80 | 16), buf_uleb(b, 1);             /* DW_CFA_offset rip, cfa - 8 */
    pad_entry(b, cie_at);
    for (k = 0; k < nfns; k++) {
        DwFn *fn = &fns[k];
        u32 at = 0;
        b = sec(s_eh);
        len_at = b->len;
        buf_u32(b, 0);
        buf_u32(b, (u32)(b->len - cie_at)); /* back to the CIE */
        reloc_here(s_eh, R_X64_PC32, fn->sym, 0, 4);
        b = sec(s_eh);
        buf_u32(b, fn->size);
        buf_uleb(b, 0); /* no augmentation data */
        for (c = fn->cf0; c < fn->cf0 + fn->ncf; c++) {
            DwCfi *e = &cfis[c];
            cfa_advance(b, &at, e->off);
            switch (e->op) {
            case CFI_PUSHFP:
                buf_u8(b, 0x0e), buf_uleb(b, 16);      /* DW_CFA_def_cfa_offset 16 */
                buf_u8(b, 0x80 | 6), buf_uleb(b, 2);   /* DW_CFA_offset rbp, cfa - 16 */
                break;
            case CFI_SETFP:
                buf_u8(b, 0x0d), buf_uleb(b, 6);       /* DW_CFA_def_cfa_register rbp */
                break;
            case CFI_SAVE:
                buf_u8(b, 0x80 | x64_dwreg[e->reg & 15]), buf_uleb(b, e->n);
                break;
            case CFI_EPI:
                buf_u8(b, 0x0a);                       /* DW_CFA_remember_state */
                buf_u8(b, 0x0c), buf_uleb(b, 7), buf_uleb(b, 8);
                break;
            case CFI_RESTORE:
                buf_u8(b, 0x0b);                       /* DW_CFA_restore_state */
                break;
            }
        }
        pad_entry(b, len_at);
    }
}

void dwarf_finish(Obj *o)
{
    if (!nfns || opt_arch != ARCH_X64)
        return;
    O = o;
    if (!cwd)
        cwd = os_getcwd();
    s_line = obj_section(o, intern_cstr(".debug_line"), 0);
    s_info = obj_section(o, intern_cstr(".debug_info"), 0);
    s_abbrev = obj_section(o, intern_cstr(".debug_abbrev"), 0);
    s_rng = obj_section(o, intern_cstr(".debug_rnglists"), 0);
    s_eh = obj_section(o, intern_cstr(".eh_frame"), SF_ALLOC);
    o->align[s_eh] = 8;
    y_line = obj_add_sym(o, 0, s_line, 0, 0);
    y_abbrev = obj_add_sym(o, 0, s_abbrev, 0, 0);
    y_rng = obj_add_sym(o, 0, s_rng, 0, 0);
    line_program();
    abbrevs();
    ranges();
    info(unit_path ? unit_path : o->name);
    eh_frame();
}
