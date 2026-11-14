/* Debug information for ELF objects (-g): DWARF 5 line tables (.debug_line), compile units that name the
   functions and their code (.debug_info, .debug_abbrev, .debug_rnglists), and frame descriptions (.eh_frame)
   for x86-64, so that debuggers, profilers and stack traces see source lines and can walk the stack.

   The x86-64 back end reports each function once its code is final (dwarf_func): the offsets where the
   source location changes, and the frame events of its prologue and epilogues. dwarf_finish writes the
   sections when the unit is done. Strings are inline (DW_FORM_string), so the only relocations are the
   addresses of functions and the offsets of the sections into each other.

   Variables (parameters and locals, recorded by the parser as they are declared) are described by their
   types and by where they are: one place in the frame for those that stay in memory, and a list of places
   (.debug_loclists) for those that became values: wherever a value that held the variable is live, the
   register or spill slot the allocator gave it. */
#include "cc.h"
#include <stdlib.h>
#include <string.h>

int opt_debug;

typedef struct DwFile {
    const char *name;
} DwFile;

typedef struct DwRow {
    u32 off, file, line, col;
    u32 inst; /* the inlined call it is part of (1 + DwInl), 0 if none */
} DwRow;

/* An inlined call: the function called, and where the call was (itself perhaps in another inlined call). Code
   copied from an inlined body gets locations of its own above VLOC0, each one a location in the body and the
   call it was copied into, so that the line table and the inlined calls can both be found from it. */
#define VLOC0 0xe0000000U
typedef struct DwInl {
    u32 callee, call_loc;
} DwInl;
typedef struct DwVloc {
    u32 loc, inst; /* a real location; 1 + DwInl */
} DwVloc;

typedef struct DwUse {
    u32 fn;     /* index of the function */
    u32 inst;   /* 1 + DwInl */
    u32 parent; /* the inlined call it is inside, 0 for the function itself */
    u32 rng;    /* offset of its list in .debug_rnglists */
} DwUse;

static DwUse *uses;
static u32 nuses, capuses;

typedef struct DwFn {
    u32 sym, size;
    u32 file, line;     /* of the declaration */
    u32 row0, nrows;
    u32 cf0, ncf;
    u32 prologue_end;   /* offset of the first instruction after the prologue */
} DwFn;

typedef struct DwVar {
    u32 fsym, name, slot;
    Type *ty;
    u8 param, byref;
    u8 kind;        /* 0 nowhere (optimised out), 1 in the frame (at CFA + fboff), 2 a list of places */
    u32 file, line;
    i32 fboff;
    u32 lr;         /* 1 + its first place (DwPlace), most recent first */
    u32 next;       /* 1 + the next variable of the same function */
} DwVar;

typedef struct DwPlace {
    u32 start, end; /* offsets in the function */
    u32 next;
    u8 len;
    u8 expr[11];
} DwPlace;

static DwFile *files;
static u32 nfiles, capfiles, last_file;
static DwRow *rows;
static u32 nrows, caprows;
static DwCfi *cfis;
static u32 ncfis, capcfis;
static DwFn *fns;
static u32 nfns, capfns;
static u32 *floc; /* per object symbol: the location of its definition */
static Type **fret; /* ... its result type */
static u32 *vhead, *vtail; /* ... 1 + its first and last variable */
static u32 nfloc;
static DwVar *vars;
static u32 nvars, capvars;
static DwPlace *places;
static u32 nplaces, capplaces;
static const char *unit_path;
static DwInl *inls;
static u32 ninls, capinls;
static DwVloc *vlocs;
static u32 nvlocs, capvlocs;
static u32 *ihash, capihash, *vhash, capvhash; /* memo: (callee, call) -> inlined call; (loc, call) -> location */
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
    nfiles = nrows = ncfis = nfns = nvars = nplaces = ninls = nvlocs = 0;
    if (capihash)
        memset(ihash, 0, capihash * sizeof(u32));
    if (capvhash)
        memset(vhash, 0, capvhash * sizeof(u32));
    last_file = 0;
    unit_path = path;
    if (nfloc) {
        memset(floc, 0, nfloc * sizeof(u32));
        memset(fret, 0, nfloc * sizeof(Type *));
        memset(vhead, 0, nfloc * sizeof(u32));
        memset(vtail, 0, nfloc * sizeof(u32));
    }
    files = (DwFile *)grow(files, &capfiles, 2, sizeof(DwFile));
    files[0].name = path;
    files[1].name = path;
    nfiles = 2;
}

static void sym_room(u32 sym)
{
    if (sym >= nfloc) {
        u32 n = nfloc ? nfloc : 256;
        while (n <= sym)
            n *= 2;
        floc = (u32 *)xrealloc(floc, n * sizeof(u32));
        fret = (Type **)xrealloc(fret, n * sizeof(Type *));
        vhead = (u32 *)xrealloc(vhead, n * sizeof(u32));
        vtail = (u32 *)xrealloc(vtail, n * sizeof(u32));
        memset(floc + nfloc, 0, (n - nfloc) * sizeof(u32));
        memset(fret + nfloc, 0, (n - nfloc) * sizeof(Type *));
        memset(vhead + nfloc, 0, (n - nfloc) * sizeof(u32));
        memset(vtail + nfloc, 0, (n - nfloc) * sizeof(u32));
        nfloc = n;
    }
}

void dwarf_func_loc(u32 sym, u32 loc, Type *ret)
{
    sym_room(sym);
    floc[sym] = loc;
    fret[sym] = ret;
    vhead[sym] = vtail[sym] = 0; /* (a definition read again: what it declares now) */
}

void dwarf_var(u32 fsym, u32 name, Type *ty, u32 slot, int param, int byref, u32 loc)
{
    DwVar *v;
    u32 col;
    sym_room(fsym);
    vars = (DwVar *)grow(vars, &capvars, nvars + 1, sizeof(DwVar));
    v = &vars[nvars];
    memset(v, 0, sizeof *v);
    v->fsym = fsym;
    v->name = name;
    v->ty = ty;
    v->slot = slot;
    v->param = (u8)param;
    v->byref = (u8)(byref || (ty && ty->vslot)); /* (a variable length array: the slot holds its address) */
    dwarf_where(loc, &v->file, &v->line, &col);
    if (vtail[fsym])
        vars[vtail[fsym] - 1].next = nvars + 1;
    else
        vhead[fsym] = nvars + 1;
    vtail[fsym] = ++nvars;
}

u32 dwarf_vars(u32 fsym)
{
    u32 k;
    if (fsym >= nfloc)
        return 0;
    for (k = vhead[fsym]; k; k = vars[k - 1].next) { /* the back end fills them in afresh */
        vars[k - 1].kind = 0;
        vars[k - 1].lr = 0;
    }
    return vhead[fsym];
}

u32 dwarf_var_next(u32 k)
{
    return vars[k - 1].next;
}

u32 dwarf_var_slot(u32 k)
{
    return vars[k - 1].slot;
}

int dwarf_var_byref(u32 k)
{
    return vars[k - 1].byref;
}

void dwarf_var_frame(u32 k, i32 cfa_off)
{
    vars[k - 1].kind = 1;
    vars[k - 1].fboff = cfa_off;
}

void dwarf_var_place(u32 k, u32 start, u32 end, const u8 *expr, u32 len)
{
    DwPlace *p;
    if (start >= end || len > sizeof p->expr || vars[k - 1].kind == 1)
        return;
    places = (DwPlace *)grow(places, &capplaces, nplaces + 1, sizeof(DwPlace));
    p = &places[nplaces];
    p->start = start;
    p->end = end;
    p->len = (u8)len;
    memcpy(p->expr, expr, len);
    p->next = vars[k - 1].lr;
    vars[k - 1].lr = ++nplaces;
    vars[k - 1].kind = 2;
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
static u32 mix(u32 a, u32 b)
{
    u32 h = a * 0x9e3779b1U ^ b * 0x85ebca6bU;
    return h ^ h >> 16;
}

/* the inlined call of callee at call_loc (1 + its index), made when new */
u32 dwarf_inline(u32 callee, u32 call_loc)
{
    u32 h, n;
    if (2 * (ninls + 1) > capihash) {
        u32 j;
        capihash = capihash ? capihash * 2 : 256;
        ihash = (u32 *)xrealloc(ihash, capihash * sizeof(u32));
        memset(ihash, 0, capihash * sizeof(u32));
        for (j = 0; j < ninls; j++) {
            for (h = mix(inls[j].callee, inls[j].call_loc) & (capihash - 1); ihash[h]; h = (h + 1) & (capihash - 1))
                ;
            ihash[h] = j + 1;
        }
    }
    for (h = mix(callee, call_loc) & (capihash - 1); (n = ihash[h]) != 0; h = (h + 1) & (capihash - 1))
        if (inls[n - 1].callee == callee && inls[n - 1].call_loc == call_loc)
            return n;
    inls = (DwInl *)grow(inls, &capinls, ninls + 1, sizeof(DwInl));
    inls[ninls].callee = callee;
    inls[ninls].call_loc = call_loc;
    ihash[h] = ninls + 1;
    return ++ninls;
}

static u32 vloc_of(u32 loc, u32 inst)
{
    u32 h, n;
    if (2 * (nvlocs + 1) > capvhash) {
        u32 j;
        capvhash = capvhash ? capvhash * 2 : 1024;
        vhash = (u32 *)xrealloc(vhash, capvhash * sizeof(u32));
        memset(vhash, 0, capvhash * sizeof(u32));
        for (j = 0; j < nvlocs; j++) {
            for (h = mix(vlocs[j].loc, vlocs[j].inst) & (capvhash - 1); vhash[h]; h = (h + 1) & (capvhash - 1))
                ;
            vhash[h] = j + 1;
        }
    }
    for (h = mix(loc, inst) & (capvhash - 1); (n = vhash[h]) != 0; h = (h + 1) & (capvhash - 1))
        if (vlocs[n - 1].loc == loc && vlocs[n - 1].inst == inst)
            return VLOC0 + n - 1;
    if (VLOC0 + nvlocs == 0xffffffffU)
        return loc; /* (out of numbers: the location without the call) */
    vlocs = (DwVloc *)grow(vlocs, &capvlocs, nvlocs + 1, sizeof(DwVloc));
    vlocs[nvlocs].loc = loc;
    vlocs[nvlocs].inst = inst;
    vhash[h] = nvlocs + 1;
    return VLOC0 + nvlocs++;
}

/* The location of code at loc in a body that is copied into inlined call inst. Code of the body that came from
   calls inlined into it goes with copies of those calls, made inside inst. */
u32 dwarf_inline_loc(u32 inst, u32 loc)
{
    DwVloc v;
    if (!loc || !inst)
        return loc;
    if (loc < VLOC0)
        return vloc_of(loc, inst);
    v = vlocs[loc - VLOC0];
    return vloc_of(v.loc, dwarf_inline(inls[v.inst - 1].callee, dwarf_inline_loc(inst, inls[v.inst - 1].call_loc)));
}

static u32 real_loc(u32 loc, u32 *inst)
{
    *inst = 0;
    if (loc < VLOC0 || loc - VLOC0 >= nvlocs)
        return loc;
    *inst = vlocs[loc - VLOC0].inst;
    return vlocs[loc - VLOC0].loc;
}

void dwarf_where(u32 loc, u32 *file, u32 *line, u32 *col)
{
    const char *name;
    u32 inst;
    loc_line(real_loc(loc, &inst), &name, line, col);
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
        real_loc(loc[k], &r.inst);
        if (nrows > fn->row0) {
            DwRow *p = &rows[nrows - 1];
            if (p->off == r.off) { /* the code between went away (a jump to the next instruction) */
                *p = r;
                continue;
            }
            if (p->file == r.file && p->line == r.line && p->col == r.col && p->inst == r.inst)
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
static int s_line, s_info, s_abbrev, s_rng, s_eh, s_loc;
static u32 y_line, y_abbrev, y_rng, y_loc; /* a symbol at the start of each, for offsets into it */

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

/* abbreviation codes */
enum {
    AB_CU = 1, AB_FN, AB_FN_EXT, AB_PARAM, AB_PARAM_LIST, AB_VAR, AB_VAR_LIST, AB_PARAM_NONE, AB_VAR_NONE,
    AB_BASE, AB_PTR, AB_REF, AB_STRUCT, AB_STRUCT_ANON, AB_UNION, AB_UNION_ANON, AB_STRUCT_DECL, AB_UNION_DECL,
    AB_MEMBER, AB_MEMBER_BITS, AB_MEMBER_ANON, AB_ARRAY, AB_SUBRANGE, AB_SUBRANGE_OPEN, AB_FNTYPE, AB_VOID,
    AB_INLINED, AB_ABSTRACT
};

enum {
    DW_TAG_array_type = 0x01, DW_TAG_formal_parameter = 0x05, DW_TAG_member = 0x0d, DW_TAG_pointer_type = 0x0f,
    DW_TAG_reference_type = 0x10, DW_TAG_structure_type = 0x13, DW_TAG_subroutine_type = 0x15,
    DW_TAG_union_type = 0x17, DW_TAG_subrange_type = 0x21, DW_TAG_base_type = 0x24, DW_TAG_variable = 0x34,
    DW_TAG_unspecified_type = 0x3b, DW_TAG_inlined_subroutine = 0x1d,
    DW_AT_inline = 0x20, DW_AT_abstract_origin = 0x31, DW_AT_call_column = 0x57, DW_AT_call_file = 0x58,
    DW_AT_call_line = 0x59,
    DW_AT_location = 0x02, DW_AT_byte_size = 0x0b, DW_AT_bit_size = 0x0d, DW_AT_prototyped = 0x27,
    DW_AT_count = 0x37, DW_AT_data_member_location = 0x38, DW_AT_declaration = 0x3c, DW_AT_encoding = 0x3e,
    DW_AT_type = 0x49, DW_AT_data_bit_offset = 0x6b,
    DW_FORM_data1 = 0x0b, DW_FORM_ref4 = 0x13
};

static void abbrev(Buf *b, u32 code, u32 tag, int children, const u32 *at)
{
    buf_uleb(b, code);
    buf_uleb(b, tag);
    buf_u8(b, (u32)children);
    for (; *at; at += 2) {
        buf_uleb(b, at[0]);
        buf_uleb(b, at[1]);
    }
    buf_u8(b, 0);
    buf_u8(b, 0);
}

static void abbrevs(void)
{
    static const u32 cu[] = {DW_AT_producer, DW_FORM_string, DW_AT_language, DW_FORM_data2, DW_AT_name, DW_FORM_string,
                             DW_AT_comp_dir, DW_FORM_string, DW_AT_low_pc, DW_FORM_addr, DW_AT_ranges, DW_FORM_sec_offset,
                             DW_AT_stmt_list, DW_FORM_sec_offset, 0};
    static const u32 fn[] = {DW_AT_name, DW_FORM_string, DW_AT_decl_file, DW_FORM_udata, DW_AT_decl_line, DW_FORM_udata,
                             DW_AT_type, DW_FORM_ref4, DW_AT_low_pc, DW_FORM_addr, DW_AT_high_pc, DW_FORM_data8,
                             DW_AT_frame_base, DW_FORM_exprloc, 0};
    static const u32 fn_ext[] = {DW_AT_name, DW_FORM_string, DW_AT_external, DW_FORM_flag_present, DW_AT_decl_file,
                                 DW_FORM_udata, DW_AT_decl_line, DW_FORM_udata, DW_AT_type, DW_FORM_ref4, DW_AT_low_pc,
                                 DW_FORM_addr, DW_AT_high_pc, DW_FORM_data8, DW_AT_frame_base, DW_FORM_exprloc, 0};
    static const u32 var[] = {DW_AT_name, DW_FORM_string, DW_AT_decl_file, DW_FORM_udata, DW_AT_decl_line, DW_FORM_udata,
                              DW_AT_type, DW_FORM_ref4, DW_AT_location, DW_FORM_exprloc, 0};
    static const u32 var_list[] = {DW_AT_name, DW_FORM_string, DW_AT_decl_file, DW_FORM_udata, DW_AT_decl_line,
                                   DW_FORM_udata, DW_AT_type, DW_FORM_ref4, DW_AT_location, DW_FORM_sec_offset, 0};
    static const u32 var_none[] = {DW_AT_name, DW_FORM_string, DW_AT_decl_file, DW_FORM_udata, DW_AT_decl_line,
                                   DW_FORM_udata, DW_AT_type, DW_FORM_ref4, 0};
    static const u32 base[] = {DW_AT_name, DW_FORM_string, DW_AT_encoding, DW_FORM_data1, DW_AT_byte_size, DW_FORM_udata, 0};
    static const u32 ptr[] = {DW_AT_byte_size, DW_FORM_data1, DW_AT_type, DW_FORM_ref4, 0};
    static const u32 rec[] = {DW_AT_name, DW_FORM_string, DW_AT_byte_size, DW_FORM_udata, 0};
    static const u32 rec_anon[] = {DW_AT_byte_size, DW_FORM_udata, 0};
    static const u32 rec_decl[] = {DW_AT_name, DW_FORM_string, DW_AT_declaration, DW_FORM_flag_present, 0};
    static const u32 mem[] = {DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4, DW_AT_data_member_location,
                              DW_FORM_udata, 0};
    static const u32 mem_bits[] = {DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4, DW_AT_bit_size, DW_FORM_udata,
                                   DW_AT_data_bit_offset, DW_FORM_udata, 0};
    static const u32 mem_anon[] = {DW_AT_type, DW_FORM_ref4, DW_AT_data_member_location, DW_FORM_udata, 0};
    static const u32 arr[] = {DW_AT_type, DW_FORM_ref4, 0};
    static const u32 sub[] = {DW_AT_count, DW_FORM_udata, 0};
    static const u32 none[] = {0};
    static const u32 fnty[] = {DW_AT_type, DW_FORM_ref4, DW_AT_prototyped, DW_FORM_flag_present, 0};
    static const u32 named[] = {DW_AT_name, DW_FORM_string, 0};
    static const u32 inlined[] = {DW_AT_abstract_origin, DW_FORM_ref4, DW_AT_call_file, DW_FORM_udata, DW_AT_call_line,
                                  DW_FORM_udata, DW_AT_call_column, DW_FORM_udata, DW_AT_ranges, DW_FORM_sec_offset, 0};
    static const u32 abstract[] = {DW_AT_name, DW_FORM_string, DW_AT_decl_file, DW_FORM_udata, DW_AT_decl_line,
                                   DW_FORM_udata, DW_AT_inline, DW_FORM_data1, 0};
    Buf *b = sec(s_abbrev);
    abbrev(b, AB_CU, DW_TAG_compile_unit, 1, cu);
    abbrev(b, AB_FN, DW_TAG_subprogram, 1, fn);
    abbrev(b, AB_FN_EXT, DW_TAG_subprogram, 1, fn_ext);
    abbrev(b, AB_PARAM, DW_TAG_formal_parameter, 0, var);
    abbrev(b, AB_PARAM_LIST, DW_TAG_formal_parameter, 0, var_list);
    abbrev(b, AB_VAR, DW_TAG_variable, 0, var);
    abbrev(b, AB_VAR_LIST, DW_TAG_variable, 0, var_list);
    abbrev(b, AB_PARAM_NONE, DW_TAG_formal_parameter, 0, var_none);
    abbrev(b, AB_VAR_NONE, DW_TAG_variable, 0, var_none);
    abbrev(b, AB_BASE, DW_TAG_base_type, 0, base);
    abbrev(b, AB_PTR, DW_TAG_pointer_type, 0, ptr);
    abbrev(b, AB_REF, DW_TAG_reference_type, 0, ptr);
    abbrev(b, AB_STRUCT, DW_TAG_structure_type, 1, rec);
    abbrev(b, AB_STRUCT_ANON, DW_TAG_structure_type, 1, rec_anon);
    abbrev(b, AB_UNION, DW_TAG_union_type, 1, rec);
    abbrev(b, AB_UNION_ANON, DW_TAG_union_type, 1, rec_anon);
    abbrev(b, AB_STRUCT_DECL, DW_TAG_structure_type, 0, rec_decl);
    abbrev(b, AB_UNION_DECL, DW_TAG_union_type, 0, rec_decl);
    abbrev(b, AB_MEMBER, DW_TAG_member, 0, mem);
    abbrev(b, AB_MEMBER_BITS, DW_TAG_member, 0, mem_bits);
    abbrev(b, AB_MEMBER_ANON, DW_TAG_member, 0, mem_anon);
    abbrev(b, AB_ARRAY, DW_TAG_array_type, 1, arr);
    abbrev(b, AB_SUBRANGE, DW_TAG_subrange_type, 0, sub);
    abbrev(b, AB_SUBRANGE_OPEN, DW_TAG_subrange_type, 0, none);
    abbrev(b, AB_FNTYPE, DW_TAG_subroutine_type, 0, fnty);
    abbrev(b, AB_VOID, DW_TAG_unspecified_type, 0, named);
    abbrev(b, AB_INLINED, DW_TAG_inlined_subroutine, 1, inlined);
    abbrev(b, AB_ABSTRACT, DW_TAG_subprogram, 0, abstract);
    buf_u8(b, 0);
}

/* ---- types: each distinct one is a DIE, written after the functions; references to it are patched ---- */

typedef struct DwType {
    u8 kind;
    u32 a;     /* the referenced type's number */
    u64 b;     /* an array's length */
    const void *p; /* a record's defining type */
    Type *t;
    u32 die;   /* offset in .debug_info, once written */
} DwType;

static DwType *types;
static u32 ntypes, captypes;
static u32 *thash, capthash;
static u32 *tfix_at, *tfix_ty, ntfix, captfix; /* a reference at offset at in .debug_info to type ty */

static u32 type_hash(const DwType *k)
{
    u32 h = (u32)k->kind * 0x9e3779b1U ^ k->a * 0x85ebca6bU ^ (u32)k->b * 0xc2b2ae35U ^ (u32)(k->b >> 32) ^
            (u32)((size_t)k->p >> 4) * 0x27d4eb2fU;
    return h ^ h >> 15;
}

static u32 type_find(const DwType *k)
{
    u32 h, n;
    if (2 * (ntypes + 1) > capthash) {
        u32 j;
        capthash = capthash ? capthash * 2 : 256;
        thash = (u32 *)xrealloc(thash, capthash * sizeof(u32));
        memset(thash, 0, capthash * sizeof(u32));
        for (j = 0; j < ntypes; j++) {
            for (h = type_hash(&types[j]) & (capthash - 1); thash[h]; h = (h + 1) & (capthash - 1))
                ;
            thash[h] = j + 1;
        }
    }
    for (h = type_hash(k) & (capthash - 1); (n = thash[h]) != 0; h = (h + 1) & (capthash - 1)) {
        DwType *x = &types[n - 1];
        if (x->kind == k->kind && x->a == k->a && x->b == k->b && x->p == k->p)
            return n - 1;
    }
    types = (DwType *)grow(types, &captypes, ntypes + 1, sizeof(DwType));
    types[ntypes] = *k;
    types[ntypes].die = 0;
    thash[h] = ntypes + 1;
    return ntypes++;
}

/* the number of the type's DIE (made when new); records and their members are only named here */
static u32 type_id(Type *t)
{
    DwType k;
    memset(&k, 0, sizeof k);
    k.t = t;
    if (!t || t->kind == TY_VOID || t->kind == TY_TPARAM) {
        k.kind = TY_VOID;
        return type_find(&k);
    }
    k.kind = t->kind;
    switch (t->kind) {
    case TY_PTR: case TY_REF:
        k.a = type_id(t->base);
        break;
    case TY_ARRAY:
        k.a = type_id(t->base);
        k.b = t->complete && !t->vslot ? t->len + 1 : 0;
        break;
    case TY_FUNC:
        k.a = type_id(t->base);
        break;
    case TY_STRUCT: case TY_UNION:
        if (t->vkind == 1) { /* a GNU vector: an array of its elements */
            k.kind = TY_ARRAY;
            k.a = type_id(t->base);
            k.b = t->len + 1;
            k.p = t; /* (not one with an ordinary array) */
            break;
        }
        if (t->vkind == 2) {
            k.a = 2;
            k.b = t->size;
            break;
        }
        k.p = t->canon ? t->canon : t;
        break;
    default:
        k.b = t->size;
        break;
    }
    return type_find(&k);
}

static void tref(u32 id)
{
    Buf *b = sec(s_info);
    tfix_at = (u32 *)grow(tfix_at, &captfix, ntfix + 1, sizeof(u32));
    tfix_ty = (u32 *)xrealloc(tfix_ty, captfix * sizeof(u32));
    tfix_at[ntfix] = (u32)b->len;
    tfix_ty[ntfix++] = id;
    buf_u32(b, 0);
}

static void base_type(Buf *b, const char *name, u32 enc, u64 size)
{
    buf_uleb(b, AB_BASE);
    str(b, name);
    buf_u8(b, enc);
    buf_uleb(b, size);
}

static void type_die(u32 id)
{
    DwType *k = &types[id];
    Type *t = k->t;
    Buf *b = sec(s_info);
    Field *fl;
    k->die = (u32)b->len;
    if (k->kind == 0xfe) {
        /* a function as inlined calls refer to it */
        ObjSym *s = &O->syms[k->a];
        u32 l = k->a < nfloc ? floc[k->a] : 0, file = 0, line = 0, col;
        if (l)
            dwarf_where(l, &file, &line, &col);
        buf_uleb(b, AB_ABSTRACT);
        str(b, s->name ? intern_str(s->name) : "?");
        buf_uleb(b, file);
        buf_uleb(b, line);
        buf_u8(b, 1); /* DW_INL_inlined */
        return;
    }
    if (k->kind == TY_VOID) {
        buf_uleb(b, AB_VOID);
        str(b, "void");
        return;
    }
    switch (t->kind) {
    case TY_BOOL: base_type(b, "_Bool", 2, 1); return;
    case TY_CHAR: base_type(b, "char", 6, 1); return;
    case TY_SCHAR: base_type(b, "signed char", 6, 1); return;
    case TY_UCHAR: base_type(b, "unsigned char", 8, 1); return;
    case TY_SHORT: base_type(b, "short", 5, 2); return;
    case TY_USHORT: base_type(b, "unsigned short", 7, 2); return;
    case TY_INT: base_type(b, "int", 5, 4); return;
    case TY_UINT: base_type(b, "unsigned int", 7, 4); return;
    case TY_LONG: base_type(b, "long", 5, 8); return;
    case TY_ULONG: base_type(b, "unsigned long", 7, 8); return;
    case TY_LLONG: base_type(b, "long long", 5, 8); return;
    case TY_ULLONG: base_type(b, "unsigned long long", 7, 8); return;
    case TY_INT128: base_type(b, "__int128", 5, 16); return;
    case TY_UINT128: base_type(b, "unsigned __int128", 7, 16); return;
    case TY_BF16: base_type(b, "__bf16", 4, 2); return;
    case TY_FLOAT16: base_type(b, "_Float16", 4, 2); return;
    case TY_FLOAT: base_type(b, "float", 4, 4); return;
    case TY_DOUBLE: base_type(b, "double", 4, 8); return;
    case TY_LDOUBLE: base_type(b, "long double", 4, t->size); return;
    case TY_PTR: case TY_REF:
        buf_uleb(b, t->kind == TY_PTR ? AB_PTR : AB_REF);
        buf_u8(b, 8);
        tref(k->a);
        return;
    case TY_FUNC:
        buf_uleb(b, AB_FNTYPE);
        tref(k->a);
        return;
    case TY_ARRAY:
        break;
    case TY_STRUCT: case TY_UNION:
        if (k->kind == TY_ARRAY)
            break;
        if (t->vkind == 2) {
            base_type(b, t->size == 8 ? "complex float" : t->size == 16 ? "complex double" : "complex", 3, t->size);
            return;
        }
        t = (Type *)k->p;
        if (!t->complete) {
            buf_uleb(b, t->kind == TY_STRUCT ? AB_STRUCT_DECL : AB_UNION_DECL);
            str(b, t->tag ? intern_str(t->tag) : t->lname ? intern_str(t->lname) : "?");
            return;
        }
        if (t->tag || t->lname) {
            buf_uleb(b, t->kind == TY_STRUCT ? AB_STRUCT : AB_UNION);
            str(b, intern_str(t->tag ? t->tag : t->lname));
        } else {
            buf_uleb(b, t->kind == TY_STRUCT ? AB_STRUCT_ANON : AB_UNION_ANON);
        }
        buf_uleb(b, t->size);
        for (fl = t->fields; fl; fl = fl->next) {
            u32 ft = type_id(fl->ty);
            b = sec(s_info);
            if (fl->bitwidth) {
                buf_uleb(b, AB_MEMBER_BITS);
                str(b, fl->name ? intern_str(fl->name) : "");
                tref(ft);
                buf_uleb(b, fl->bitwidth);
                buf_uleb(b, (u64)fl->off * 8 + fl->bitoff);
            } else if (fl->name) {
                buf_uleb(b, AB_MEMBER);
                str(b, intern_str(fl->name));
                tref(ft);
                buf_uleb(b, fl->off);
            } else {
                buf_uleb(b, AB_MEMBER_ANON);
                tref(ft);
                buf_uleb(b, fl->off);
            }
        }
        buf_u8(sec(s_info), 0);
        return;
    default:
        buf_uleb(b, AB_VOID);
        str(b, "void");
        return;
    }
    /* an array (or a vector): its elements, and how many */
    buf_uleb(b, AB_ARRAY);
    tref(k->a);
    if (k->b) {
        buf_uleb(b, AB_SUBRANGE);
        buf_uleb(b, k->b - 1);
    } else {
        buf_uleb(b, AB_SUBRANGE_OPEN);
    }
    buf_u8(b, 0);
}

static u32 loclists_at(DwVar *v); /* where the variable's list of places is */

/* the inlined calls in function fn that are directly inside parent (0: the function itself) */
static void inlined_dies(u32 fn, u32 parent)
{
    u32 i;
    for (i = 0; i < nuses; i++) {
        DwUse *u = &uses[i];
        DwType key;
        u32 id, file, line, col, real, dummy;
        Buf *b;
        if (u->fn != fn || u->parent != parent)
            continue;
        memset(&key, 0, sizeof key);
        key.kind = 0xfe;
        key.a = inls[u->inst - 1].callee;
        id = type_find(&key);
        real = real_loc(inls[u->inst - 1].call_loc, &dummy);
        dwarf_where(real, &file, &line, &col);
        b = sec(s_info);
        buf_uleb(b, AB_INLINED);
        tref(id);
        b = sec(s_info);
        buf_uleb(b, file);
        buf_uleb(b, line);
        buf_uleb(b, col);
        reloc_here(s_info, R_ABS32, y_rng, u->rng, 4);
        inlined_dies(fn, u->inst);
        buf_u8(sec(s_info), 0);
    }
}

static void var_die(DwVar *v)
{
    Buf *b = sec(s_info);
    u32 ty = type_id(v->ty);
    buf_uleb(b, v->kind == 1 ? (v->param ? AB_PARAM : AB_VAR) : v->kind == 2 ? (v->param ? AB_PARAM_LIST : AB_VAR_LIST)
                                                                             : (v->param ? AB_PARAM_NONE : AB_VAR_NONE));
    str(b, intern_str(v->name));
    buf_uleb(b, v->file);
    buf_uleb(b, v->line);
    tref(ty);
    if (v->kind == 1) {
        Buf e;
        memset(&e, 0, sizeof e);
        buf_u8(&e, 0x91); /* DW_OP_fbreg: from the frame's address, which the frame descriptions give */
        sleb(&e, v->fboff);
        if (v->byref)
            buf_u8(&e, 0x06); /* DW_OP_deref: the slot holds its address */
        buf_uleb(b, e.len);
        buf_put(b, e.p, e.len);
        buf_free(&e);
    } else if (v->kind == 2) {
        reloc_here(s_info, R_ABS32, y_loc, loclists_at(v), 4);
    }
}

/* ---- inlined calls ---- */


static u32 inl_parent(u32 inst)
{
    u32 p;
    real_loc(inls[inst - 1].call_loc, &p);
    return p;
}

/* For each function, the inlined calls whose code is in it, and where (from the rows of its line table); their
   lists of ranges go into .debug_rnglists. */
static void inline_ranges(void)
{
    u32 k, r, *last_use, *rs, *re, *ru, nr, capr = 0;
    nuses = 0;
    if (!ninls)
        return;
    last_use = (u32 *)xmalloc((ninls + 1) * sizeof(u32)); /* by inlined call: 1 + its DwUse in this function */
    memset(last_use, 0, (ninls + 1) * sizeof(u32));
    rs = re = ru = 0;
    for (k = 0; k < nfns; k++) {
        DwFn *fn = &fns[k];
        u32 first = nuses, i, j;
        nr = 0;
        for (r = fn->row0; r < fn->row0 + fn->nrows; r++) {
            u32 c, end = r + 1 < fn->row0 + fn->nrows ? rows[r + 1].off : fn->size;
            for (c = rows[r].inst; c; c = inl_parent(c)) {
                if (!last_use[c]) {
                    uses = (DwUse *)grow(uses, &capuses, nuses + 1, sizeof(DwUse));
                    uses[nuses].fn = k;
                    uses[nuses].inst = c;
                    uses[nuses].parent = inl_parent(c);
                    last_use[c] = ++nuses;
                }
                /* a range of it: joined to the one before when they meet */
                for (i = nr; i > 0 && ru[i - 1] != last_use[c] - 1; i--)
                    ;
                if (i > 0 && re[i - 1] == rows[r].off) {
                    re[i - 1] = end;
                } else {
                    if (nr == capr) {
                        capr = capr ? capr * 2 : 64;
                        rs = (u32 *)xrealloc(rs, capr * sizeof(u32));
                        re = (u32 *)xrealloc(re, capr * sizeof(u32));
                        ru = (u32 *)xrealloc(ru, capr * sizeof(u32));
                    }
                    rs[nr] = rows[r].off;
                    re[nr] = end;
                    ru[nr++] = last_use[c] - 1;
                }
            }
        }
        for (i = first; i < nuses; i++) {
            Buf *b = sec(s_rng);
            uses[i].rng = (u32)b->len;
            buf_u8(b, 5); /* DW_RLE_base_address: the function */
            reloc_here(s_rng, R_ABS64, fn->sym, 0, 8);
            b = sec(s_rng);
            for (j = 0; j < nr; j++)
                if (ru[j] == i && re[j] > rs[j]) {
                    buf_u8(b, 4); /* DW_RLE_offset_pair */
                    buf_uleb(b, rs[j]);
                    buf_uleb(b, re[j]);
                }
            buf_u8(b, 0);
            last_use[uses[i].inst] = 0;
        }
    }
    free(last_use);
    free(rs);
    free(re);
    free(ru);
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
    inline_ranges();
    b = sec(s_rng);
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
    ntypes = ntfix = 0;
    if (capthash)
        memset(thash, 0, capthash * sizeof(u32));
    for (k = 0; k < nfns; k++) {
        DwFn *fn = &fns[k];
        ObjSym *s = &O->syms[fn->sym];
        u32 v, ret = type_id(fn->sym < nfloc ? fret[fn->sym] : 0);
        b = sec(s_info);
        buf_uleb(b, s->global || s->weak ? AB_FN_EXT : AB_FN);
        str(b, s->name ? intern_str(s->name) : "?");
        buf_uleb(b, fn->file);
        buf_uleb(b, fn->line);
        tref(ret);
        reloc_here(s_info, R_ABS64, fn->sym, 0, 8);
        b = sec(s_info);
        buf_u64(b, fn->size);
        buf_uleb(b, 1);
        buf_u8(b, 0x9c); /* DW_OP_call_frame_cfa */
        for (v = fn->sym < nfloc ? vhead[fn->sym] : 0; v; v = vars[v - 1].next)
            var_die(&vars[v - 1]);
        inlined_dies(k, 0);
        buf_u8(sec(s_info), 0);
    }
    /* the types, and the references to them */
    for (k = 0; k < ntypes; k++)
        type_die(k);
    b = sec(s_info);
    for (k = 0; k < ntfix; k++)
        put32(b, tfix_at[k], (u32)(types[tfix_ty[k]].die - len_at));
    buf_u8(b, 0);
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

/* .debug_loclists: for each variable that lives in registers, its places, relative to its function's start */
static u32 *loc_off; /* by variable: 1 + offset of its list */

static void loclists(void)
{
    Buf *b = sec(s_loc);
    size_t len_at = b->len;
    u32 k, v;
    buf_u32(b, 0);
    u16le(b, 5);
    buf_u8(b, 8);
    buf_u8(b, 0);
    buf_u32(b, 0);
    loc_off = (u32 *)xrealloc(loc_off, (nvars + 1) * sizeof(u32));
    memset(loc_off, 0, (nvars + 1) * sizeof(u32));
    for (k = 0; k < nfns; k++) {
        u32 fsym = fns[k].sym;
        for (v = fsym < nfloc ? vhead[fsym] : 0; v; v = vars[v - 1].next) {
            DwVar *x = &vars[v - 1];
            u32 p, n = 0, i, j, *ord;
            if (x->kind != 2)
                continue;
            for (p = x->lr; p; p = places[p - 1].next)
                n++;
            ord = (u32 *)xmalloc(n * sizeof(u32));
            for (p = x->lr, i = 0; p; p = places[p - 1].next)
                ord[i++] = p - 1;
            for (i = 1; i < n; i++) /* by where they start (insertion sort: lists are short) */
                for (j = i; j > 0 && places[ord[j - 1]].start > places[ord[j]].start; j--) {
                    u32 t = ord[j];
                    ord[j] = ord[j - 1];
                    ord[j - 1] = t;
                }
            b = sec(s_loc);
            loc_off[v - 1] = (u32)b->len + 1;
            buf_u8(b, 6); /* DW_LLE_base_address: the function */
            reloc_here(s_loc, R_ABS64, fsym, 0, 8);
            b = sec(s_loc);
            for (i = 0; i < n; i++) {
                DwPlace *q = &places[ord[i]];
                buf_u8(b, 4); /* DW_LLE_offset_pair */
                buf_uleb(b, q->start);
                buf_uleb(b, q->end);
                buf_uleb(b, q->len);
                buf_put(b, q->expr, q->len);
            }
            buf_u8(b, 0); /* DW_LLE_end_of_list */
            free(ord);
        }
    }
    put32(b, len_at, (u32)(b->len - len_at - 4));
}

static u32 loclists_at(DwVar *v)
{
    return loc_off[v - vars] - 1;
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
    s_loc = obj_section(o, intern_cstr(".debug_loclists"), 0);
    o->align[s_eh] = 8;
    y_line = obj_add_sym(o, 0, s_line, 0, 0);
    y_abbrev = obj_add_sym(o, 0, s_abbrev, 0, 0);
    y_rng = obj_add_sym(o, 0, s_rng, 0, 0);
    y_loc = obj_add_sym(o, 0, s_loc, 0, 0);
    line_program();
    loclists();
    abbrevs();
    ranges();
    info(unit_path ? unit_path : o->name);
    eh_frame();
}
