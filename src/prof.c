/* Profile-guided optimisation.

   -fprofile-generate: every basic block of every function counts its
   executions in a table in the data section; the C library writes the
   tables to a file when the program exits (libc/src/prof.c).

   -fprofile-use: the counts are attached to the blocks right after parsing,
   before any optimisation, so they line up with the blocks the instrumented
   build numbered. Later passes read them from Block.freq.

   The file is "MCCPROF1" followed by records { key, n, count[n] } of 64-bit
   words. The key hashes the source path and the function name. */
#include "mcc.h"
#include "ir.h"
#include <string.h>

int opt_prof_gen;
int opt_prof_use;
u32 prof_max; /* the largest count, as scaled */

static u64 key_of(const char *path, const char *name)
{
    u64 h = ((u64)0xcbf29ce4 << 32) | 0x84222325, prime = ((u64)0x100 << 32) | 0x1b3;
    for (; *path; path++)
        h = (h ^ (u8)*path) * prime;
    h *= prime;
    for (; *name; name++)
        h = (h ^ (u8)*name) * prime;
    return h ? h : 1;
}

/* ---- generating ---- */

static Obj *obj;
static Buf counters;
static int have_syms;
static u32 cnt_sym, desc_sym, enter_sym;

void prof_begin(Obj *o)
{
    obj = o;
    counters.len = 0;
    have_syms = 0;
}

void prof_instrument(Func *f)
{
    u32 b, cur = f->cur, pos, v[4], k, n;
    size_t base = counters.len;
    if (!have_syms) {
        have_syms = 1;
        cnt_sym = obj_add_sym(obj, 0, SEC_UNDEF, 0, 0);
        desc_sym = obj_add_sym(obj, 0, SEC_UNDEF, 0, 0);
        obj->syms[cnt_sym].align = 3;
        obj->syms[desc_sym].align = 3;
        enter_sym = obj_add_sym(obj, intern_cstr(opt_target == TARGET_LINUX ? "__mcc_prof_enter" : "___mcc_prof_enter"), SEC_UNDEF, 0, 1);
    }
    buf_u64(&counters, key_of(obj->name, f->name));
    buf_u64(&counters, f->nblk - 1);
    buf_zero(&counters, (size_t)(f->nblk - 1) * 8);
    f->cur = 0;
    for (b = 1; b < f->nblk; b++) {
        u32 addr;
        if (f->blk[b].term == TERM_NONE)
            continue;
        n = 0;
        pos = f->blk[b].first;
        if (b == 1) {
            u32 arg = ir_addr(f, desc_sym, 0);
            while (pos && f->ins[pos].op == OP_PARAM)
                pos = f->ins[pos].next;
            v[n++] = ir_call(f, IT_VOID, ir_addr(f, enter_sym, 0), &arg, 1, 1, 0);
        }
        addr = ir_addr(f, cnt_sym, (i64)(base + 16 + (size_t)(b - 1) * 8));
        v[n] = ir_raw(f, OP_LOAD, IT_I64, addr, 0);
        f->ins[v[n]].mem = M_64;
        v[n + 1] = ir_raw(f, OP_ADD, IT_I64, v[n], ir_const(f, IT_I64, 1));
        v[n + 2] = ir_raw(f, OP_STORE, IT_VOID, addr, v[n + 1]);
        f->ins[v[n + 2]].mem = M_64;
        n += 3;
        for (k = 0; k < n; k++) {
            if (pos)
                ir_insert_before(f, pos, v[k]);
            else
                ir_append(f, b, v[k]);
        }
    }
    f->cur = cur;
}

/* the tables, and the descriptor the runtime chains up:
   { registered, next, number of words, table } */
void prof_end(void)
{
    u64 off;
    if (!have_syms)
        return;
    off = obj_reserve(obj, SEC_DATA, counters.len, 8);
    buf_put(&obj->sec[SEC_DATA], counters.p, counters.len);
    obj->syms[cnt_sym].sec = SEC_DATA;
    obj->syms[cnt_sym].value = off;
    off = obj_reserve(obj, SEC_DATA, 32, 8);
    buf_zero(&obj->sec[SEC_DATA], 16);
    buf_u64(&obj->sec[SEC_DATA], counters.len / 8);
    buf_zero(&obj->sec[SEC_DATA], 8);
    obj_add_reloc(obj, SEC_DATA, (u32)off + 24, R_ABS64, cnt_sym, 0);
    obj->syms[desc_sym].sec = SEC_DATA;
    obj->syms[desc_sym].value = off;
    have_syms = 0;
}

/* ---- using ---- */

static u8 *file;
static u32 *tab; /* offset of the record's key in the file, 0 = empty */
static u32 tab_mask;
static int shift;

static u64 word(size_t off)
{
    u64 v;
    memcpy(&v, file + off, 8);
    return v;
}

void prof_load(const char *path)
{
    size_t len, off, nrec = 0;
    u64 max = 0;
    file = os_read_file(path, &len);
    if (!file || len < 8 || memcmp(file, "MCCPROF1", 8) != 0)
        die("%s: not a profile", path);
    for (off = 8; off + 16 <= len; off += 16 + (size_t)word(off + 8) * 8) {
        if (word(off + 8) > (len - off - 16) / 8)
            die("%s: damaged profile", path);
        nrec++;
    }
    tab_mask = 63;
    while (tab_mask < nrec * 2)
        tab_mask = tab_mask * 2 + 1;
    tab = (u32 *)xmalloc(((size_t)tab_mask + 1) * sizeof(u32));
    memset(tab, 0, ((size_t)tab_mask + 1) * sizeof(u32));
    for (off = 8; off + 16 <= len; off += 16 + (size_t)word(off + 8) * 8) {
        u64 key = word(off), n = word(off + 8), k;
        u32 h;
        if (!key)
            continue; /* superseded */
        for (h = (u32)key & tab_mask; tab[h]; h = (h + 1) & tab_mask)
            ;
        tab[h] = (u32)off;
        for (k = 0; k < n; k++)
            if (word(off + 16 + (size_t)k * 8) > max)
                max = word(off + 16 + (size_t)k * 8);
    }
    /* counts are kept in 32 bits, comparable across the whole program */
    for (shift = 0; (max >> shift) >> 30; shift++)
        ;
    prof_max = (u32)(max >> shift);
    opt_prof_use = 1;
}

/* freq: 0 = unknown, otherwise 1 + scaled count */
int prof_apply(Func *f, const char *path)
{
    u64 key = key_of(path, f->name);
    u32 h, b;
    for (h = (u32)key & tab_mask; tab[h]; h = (h + 1) & tab_mask) {
        size_t off = tab[h];
        if (word(off) != key)
            continue;
        if (word(off + 8) != f->nblk - 1)
            return 0; /* the source has changed */
        for (b = 1; b < f->nblk; b++) {
            u64 c = word(off + 16 + (size_t)(b - 1) * 8), s = c >> shift;
            f->blk[b].freq = c ? (u32)(s ? s : 1) + 1 : 1;
        }
        return 1;
    }
    return 0;
}
