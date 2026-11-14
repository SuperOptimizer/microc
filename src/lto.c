/* Link-time optimisation. With -flto the compiler does not generate code:
   each function is stored, as parsed, in the object's IR section, and its
   symbol is given a one-instruction placeholder. At link time every object
   that carries IR is merged into one; its functions are then optimised and
   compiled callees first, so that calls can be inlined across source files,
   and functions nothing refers to are left out. */
#include "ir.h"
#include <stdlib.h>
#include <string.h>

int opt_lto;

#define NHDR 10 /* words: size, nsym, nins, nblk, nxargs, nslots, nparams, flags, npreds, 0 */
#define NBLKW 11

/* ---- compile time ---- */

static u32 *slot_of, *slot_stamp, slot_cap, stamp;

static u32 sym_slot(Obj *o, Buf *syms, u32 sym)
{
    if (o->nsyms > slot_cap) {
        u32 cap = o->nsyms * 2;
        slot_of = (u32 *)xrealloc(slot_of, cap * sizeof(u32));
        slot_stamp = (u32 *)xrealloc(slot_stamp, cap * sizeof(u32));
        memset(slot_stamp + slot_cap, 0, (cap - slot_cap) * sizeof(u32));
        slot_cap = cap;
    }
    if (slot_stamp[sym] != stamp) {
        slot_stamp[sym] = stamp;
        slot_of[sym] = (u32)(syms->len / sizeof(u32));
        buf_put(syms, &sym, sizeof(u32));
    }
    return slot_of[sym];
}

void lto_save(Func *f, Obj *o)
{
    Buf *ir = &o->sec[SEC_IR], *text = &o->sec[SEC_TEXT];
    Buf syms;
    u32 hdr[NHDR], w[NBLKW], v, b, k, nsym, npreds = 0, start;
    Ins *copy = (Ins *)xmalloc(f->nins * sizeof(Ins));

    memset(&syms, 0, sizeof syms);
    stamp++;
    sym_slot(o, &syms, f->sym);
    memcpy(copy, f->ins, f->nins * sizeof(Ins));
    for (v = 1; v < f->nins; v++) {
        if (copy[v].op == OP_ADDR)
            copy[v].a = sym_slot(o, &syms, copy[v].a);
        copy[v].loc = 0; /* source locations mean nothing outside this compilation */
    }
    nsym = (u32)(syms.len / sizeof(u32));
    for (b = 1; b < f->nblk; b++)
        npreds += f->blk[b].npreds;

    buf_align(ir, 8);
    o->align[SEC_IR] = 8;
    start = (u32)ir->len;
    memset(hdr, 0, sizeof hdr);
    hdr[1] = nsym;
    hdr[2] = f->nins;
    hdr[3] = f->nblk;
    hdr[4] = f->nxargs;
    hdr[5] = f->nslots;
    hdr[6] = f->nparams;
    hdr[7] = (u32)f->ret_ty | ((u32)f->has_sret << 8) | ((u32)f->variadic << 16);
    hdr[8] = npreds;
    buf_put(ir, hdr, sizeof hdr);
    /* the symbols the function names, as relocations so that they survive
       the renumbering an object file brings */
    for (k = 0; k < nsym; k++) {
        u32 sym;
        memcpy(&sym, syms.p + k * sizeof(u32), sizeof(u32));
        /* the addend carries the alignment the compiler knew for the symbol */
        obj_add_reloc(o, SEC_IR, (u32)ir->len, R_ABS64, sym, o->syms[sym].sec == SEC_COMMON ? 0 : o->syms[sym].align);
        buf_zero(ir, 8);
    }
    buf_put(ir, copy, f->nins * sizeof(Ins));
    for (b = 0; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        w[0] = bb->first;
        w[1] = bb->last;
        w[2] = bb->phis;
        w[3] = b ? bb->npreds : 0;
        w[4] = (u32)bb->term | ((u32)bb->flag << 8);
        w[5] = bb->arg;
        w[6] = bb->succ[0];
        w[7] = bb->succ[1];
        w[8] = bb->aux;
        w[9] = bb->aux2;
        w[10] = bb->freq;
        buf_put(ir, w, sizeof w);
    }
    for (b = 1; b < f->nblk; b++)
        if (f->blk[b].npreds)
            buf_put(ir, f->blk[b].preds, f->blk[b].npreds * sizeof(u32));
    if (f->nxargs)
        buf_put(ir, f->xargs, f->nxargs * sizeof(u32));
    for (k = 0; k < f->nslots; k++) {
        w[0] = f->slots[k].size;
        w[1] = f->slots[k].align;
        buf_put(ir, w, 2 * sizeof(u32));
    }
    if (f->nparams)
        buf_put(ir, f->param_ty, f->nparams);
    buf_align(ir, 8);
    hdr[0] = (u32)ir->len - start;
    memcpy(ir->p + start, hdr, sizeof(u32));
    free(copy);
    buf_free(&syms);

    /* the placeholder that defines the symbol: brk #1 */
    buf_align(text, 4);
    if (o->align[SEC_TEXT] < 4)
        o->align[SEC_TEXT] = 4;
    o->syms[f->sym].sec = SEC_TEXT;
    o->syms[f->sym].value = text->len;
    hdr[0] = 0xd4200020U;
    buf_put(text, hdr, 4);
}

/* ---- link time ---- */

typedef struct LFn {
    const u8 *rec; /* the stored function */
    u32 *syms;     /* merged-object symbol of each symbol slot */
    u8 state;      /* 0 untouched, 1 reached, 2 compiled, 3 kept only for inlining */
    u8 escapes;    /* referred to other than by a call from IR */
    u32 ncalls;
} LFn;

static Obj *M;       /* the merged object */
static LFn *fns;
static u32 nfns, capfns;
static u32 *fn_of;   /* by merged symbol: function index + 1 */
static Func *F;

static u32 rd(const u8 *p, u32 word)
{
    u32 v;
    memcpy(&v, p + word * sizeof(u32), sizeof(u32));
    return v;
}

static const u8 *rec_ins(const u8 *rec)
{
    return rec + NHDR * sizeof(u32) + (size_t)rd(rec, 1) * 8;
}

static void load(LFn *fn)
{
    const u8 *rec = fn->rec, *p = rec_ins(rec);
    u32 nins = rd(rec, 2), nblk = rd(rec, 3), nxargs = rd(rec, 4), nslots = rd(rec, 5), nparams = rd(rec, 6);
    u32 flags = rd(rec, 7), b, k, v;
    const u8 *preds;
    const char *name;
    Func *f = F;

    ir_func_reset(f);
    if (f->capins < nins + 64) {
        f->capins = nins * 2 + 64;
        f->ins = (Ins *)xrealloc(f->ins, f->capins * sizeof(Ins));
    }
    memcpy(f->ins, p, nins * sizeof(Ins));
    f->nins = nins;
    p += nins * sizeof(Ins);
    if (f->capblk < nblk + 8) {
        f->capblk = nblk * 2 + 8;
        f->blk = (Block *)xrealloc(f->blk, f->capblk * sizeof(Block));
    }
    preds = p + (size_t)nblk * NBLKW * sizeof(u32);
    for (b = 0; b < nblk; b++, p += NBLKW * sizeof(u32)) {
        Block *bb = &f->blk[b];
        memset(bb, 0, sizeof *bb);
        bb->first = rd(p, 0);
        bb->last = rd(p, 1);
        bb->phis = rd(p, 2);
        bb->npreds = bb->cappreds = rd(p, 3);
        bb->term = (u8)rd(p, 4);
        bb->flag = (u8)(rd(p, 4) >> 8);
        bb->arg = rd(p, 5);
        bb->succ[0] = rd(p, 6);
        bb->succ[1] = rd(p, 7);
        bb->aux = rd(p, 8);
        bb->aux2 = rd(p, 9);
        bb->freq = rd(p, 10);
        if (bb->npreds) {
            bb->preds = (u32 *)arena_alloc_raw(&f->arena, bb->npreds * sizeof(u32));
            memcpy(bb->preds, preds, bb->npreds * sizeof(u32));
            preds += bb->npreds * sizeof(u32);
        }
    }
    f->nblk = nblk;
    p = preds;
    if (f->capxargs < nxargs) {
        f->capxargs = nxargs * 2;
        f->xargs = (u32 *)xrealloc(f->xargs, f->capxargs * sizeof(u32));
    }
    if (nxargs)
        memcpy(f->xargs, p, nxargs * sizeof(u32));
    f->nxargs = nxargs;
    p += nxargs * sizeof(u32);
    if (f->capslots < nslots) {
        f->capslots = nslots * 2;
        f->slots = (Slot *)xrealloc(f->slots, f->capslots * sizeof(Slot));
    }
    for (k = 0; k < nslots; k++, p += 2 * sizeof(u32)) {
        f->slots[k].size = rd(p, 0);
        f->slots[k].align = rd(p, 1);
        f->slots[k].off = 0;
    }
    f->nslots = nslots;
    if (f->capparams < nparams) {
        f->capparams = nparams * 2;
        f->param_ty = (u8 *)xrealloc(f->param_ty, f->capparams);
    }
    if (nparams)
        memcpy(f->param_ty, p, nparams);
    f->nparams = nparams;
    f->ret_ty = (u8)flags;
    f->has_sret = (u8)(flags >> 8);
    f->variadic = (u8)(flags >> 16);
    f->cur = 0;
    for (v = 1; v < nins; v++)
        if (f->ins[v].op == OP_ADDR)
            f->ins[v].a = fn->syms[f->ins[v].a];
    ir_rehash(f);
    f->sym = fn->syms[0];
    name = M->syms[f->sym].name ? intern_str(M->syms[f->sym].name) : "_?";
    f->name = name + 1;
}

static u32 *order, norder; /* reached functions, callees first */

static void reach(u32 n)
{
    LFn *fn = &fns[n];
    const u8 *p = rec_ins(fn->rec);
    u32 nins = rd(fn->rec, 2), v;
    fn->state = 1;
    for (v = 1; v < nins; v++) {
        Ins i;
        u32 t;
        memcpy(&i, p + (size_t)v * sizeof(Ins), sizeof(Ins));
        if (i.op != OP_ADDR)
            continue;
        t = fn_of[fn->syms[i.a]];
        if (t && !fns[t - 1].state)
            reach(t - 1);
    }
    order[norder++] = n;
}

static void root(u32 sym)
{
    if (sym == 0xffffffffU || !fn_of[sym])
        return;
    fns[fn_of[sym] - 1].escapes = 1;
    if (!fns[fn_of[sym] - 1].state)
        reach(fn_of[sym] - 1);
}

/* Counts how the loaded function uses other functions. */
static void count_uses(void)
{
    Func *f = F;
    u32 b, v, k, n;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        if (bb->term == TERM_NONE)
            continue;
        for (v = bb->phis; v; v = f->ins[v].next) {
            for (k = 0; k < f->ins[v].b; k++) {
                Ins *a = &f->ins[f->xargs[f->ins[v].a + k]];
                if (a->op == OP_ADDR && fn_of[a->a])
                    fns[fn_of[a->a] - 1].escapes = 1;
            }
        }
        for (v = bb->first; v; v = f->ins[v].next) {
            n = ir_opcount(f, v);
            for (k = 0; k < n; k++) {
                Ins *a = &f->ins[*ir_opref(f, v, k)];
                if (a->op != OP_ADDR || !fn_of[a->a])
                    continue;
                if (f->ins[v].op == OP_CALL && k == 0 && a->b == 0)
                    fns[fn_of[a->a] - 1].ncalls++;
                else
                    fns[fn_of[a->a] - 1].escapes = 1;
            }
        }
        if (bb->arg && f->ins[bb->arg].op == OP_ADDR && fn_of[f->ins[bb->arg].a])
            fns[fn_of[f->ins[bb->arg].a] - 1].escapes = 1;
    }
}

/* Generates code for a function that was kept back for inlining. */
static void emit_late(u32 n)
{
    load(&fns[n]);
    opt_run(F);
    arm64_gen(F, M);
    fns[n].state = 2;
}

static void compile(u32 n)
{
    LFn *fn = &fns[n];
    load(fn);
    /* a function with one call site is inlined there even when large, and
       then needs no code of its own */
    opt_inline_once = fn->ncalls == 1 && !fn->escapes;
    opt_run(F);
    if (!fn->escapes && inline_saved(F->sym)) {
        /* every call may get inlined; code is generated later if one is not */
        fn->state = 3;
    } else {
        arm64_gen(F, M);
        fn->state = 2;
    }
    opt_inline_once = 0;
}

void lto_link(Obj **objs, size_t *nobjs, const char *entry)
{
    size_t i, n = *nobjs, first = n, out = 0;
    u32 nnames = intern_count(), *gsym, **maps, j, k, sec;
    u8 *is_fn = 0;
    u32 cap_fn = 0;

    for (i = 0; i < n; i++)
        if (objs[i]->sec[SEC_IR].len && first == n)
            first = i;
    if (first == n)
        return;
    M = obj_new("<lto>");
    gsym = (u32 *)xmalloc(nnames * sizeof(u32));
    memset(gsym, 0xff, nnames * sizeof(u32));
    maps = (u32 **)xmalloc(n * sizeof(u32 *));

    /* merge data and symbols */
    for (i = 0; i < n; i++) {
        Obj *o = objs[i];
        u64 base[SEC_COUNT];
        u32 *map;
        if (!o->sec[SEC_IR].len)
            continue;
        if (o->nrel[SEC_TEXT])
            die("%s: an object with IR also has compiled code", o->name);
        map = maps[i] = (u32 *)xmalloc((o->nsyms + 1) * sizeof(u32));
        base[SEC_TEXT] = 0;
        for (sec = SEC_CONST; sec < SEC_COUNT; sec++) {
            base[sec] = obj_reserve(M, (int)sec, o->sec[sec].len, o->align[sec]);
            if (sec != SEC_BSS)
                buf_put(&M->sec[sec], o->sec[sec].p, o->sec[sec].len);
        }
        for (j = 0; j < o->nsyms; j++) {
            ObjSym *s = &o->syms[j], *ms;
            u32 m;
            if (s->sec == SEC_NONE) {
                map[j] = 0xffffffffU;
                continue;
            }
            if (s->global && s->name) {
                m = gsym[s->name];
                if (m == 0xffffffffU)
                    m = gsym[s->name] = obj_add_sym(M, s->name, SEC_UNDEF, 0, 1);
            } else {
                m = obj_add_sym(M, s->name, SEC_UNDEF, 0, 0);
            }
            map[j] = m;
            if (M->nsyms > cap_fn) {
                u32 cap = M->nsyms * 2 + 64;
                is_fn = (u8 *)xrealloc(is_fn, cap);
                memset(is_fn + cap_fn, 0, cap - cap_fn);
                cap_fn = cap;
            }
            ms = &M->syms[m];
            if (s->sec < SEC_COUNT) {
                if (ms->sec < SEC_COUNT || is_fn[m])
                    die("duplicate symbol %s (in %s)", intern_str(s->name), o->name);
                if (s->sec == SEC_TEXT) {
                    is_fn[m] = 1; /* defined once its function is compiled */
                } else {
                    ms->sec = s->sec;
                    ms->value = s->value + base[s->sec];
                }
            } else if (s->sec == SEC_COMMON && !is_fn[m] && ms->sec >= SEC_COUNT) {
                if (ms->sec != SEC_COMMON || ms->value < s->value) {
                    ms->value = s->value;
                    ms->align = s->align;
                }
                ms->sec = SEC_COMMON;
            }
        }
        for (sec = SEC_CONST; sec <= SEC_DATA; sec++) {
            for (k = 0; k < o->nrel[sec]; k++) {
                ObjReloc *r = &o->rel[sec][k];
                obj_add_reloc(M, (int)sec, (u32)(r->off + base[sec]), (int)r->type, map[r->sym], r->addend);
            }
        }
    }

    /* find the functions */
    fn_of = (u32 *)xmalloc((M->nsyms + 1) * sizeof(u32));
    memset(fn_of, 0, (M->nsyms + 1) * sizeof(u32));
    nfns = 0;
    for (i = 0; i < n; i++) {
        Obj *o = objs[i];
        Buf *ir = &o->sec[SEC_IR];
        size_t at = 0;
        u32 r = 0;
        if (!ir->len)
            continue;
        while (at + NHDR * sizeof(u32) <= ir->len) {
            const u8 *rec = ir->p + at;
            u32 size = rd(rec, 0), nsym = rd(rec, 1);
            LFn *fn;
            if (!size || at + size > ir->len || r + nsym > o->nrel[SEC_IR])
                die("%s: damaged IR section", o->name);
            if (nfns == capfns) {
                capfns = capfns ? capfns * 2 : 256;
                fns = (LFn *)xrealloc(fns, capfns * sizeof(LFn));
            }
            fn = &fns[nfns++];
            fn->rec = rec;
            fn->state = 0;
            fn->escapes = 0;
            fn->ncalls = 0;
            fn->syms = (u32 *)xmalloc(nsym * sizeof(u32));
            for (k = 0; k < nsym; k++, r++) {
                ObjReloc *rl = &o->rel[SEC_IR][r];
                if (rl->off != at + NHDR * sizeof(u32) + (size_t)k * 8 || maps[i][rl->sym] == 0xffffffffU)
                    die("%s: damaged IR section", o->name);
                fn->syms[k] = maps[i][rl->sym];
                if (M->syms[fn->syms[k]].sec != SEC_COMMON && rl->addend > 0 && rl->addend <= 4 &&
                    M->syms[fn->syms[k]].align < (u8)rl->addend)
                    M->syms[fn->syms[k]].align = (u8)rl->addend;
            }
            fn_of[fn->syms[0]] = nfns;
            at += size;
        }
    }

    /* compile what the program can reach: the entry point, whatever other
       objects refer to by name, and whatever initialised data points at */
    if (!F)
        F = ir_func_new();
    inline_reset();
    order = (u32 *)xmalloc((nfns + 1) * sizeof(u32));
    norder = 0;
    if (intern_cstr(entry) < nnames)
        root(gsym[intern_cstr(entry)]);
    for (i = 0; i < n; i++) {
        Obj *o = objs[i];
        if (o->sec[SEC_IR].len)
            continue;
        for (j = 0; j < o->nsyms; j++)
            if (o->syms[j].sec == SEC_UNDEF && o->syms[j].name && o->syms[j].name < nnames)
                root(gsym[o->syms[j].name]);
    }
    for (sec = SEC_CONST; sec <= SEC_DATA; sec++) {
        /* compiling adds no relocations to these sections */
        for (k = 0; k < M->nrel[sec]; k++)
            root(M->rel[sec][k].sym);
    }
    for (j = 0; j < norder; j++) {
        load(&fns[order[j]]);
        count_uses();
    }
    {
        u32 done = 0, nsyms0 = M->nsyms, seen_syms = nsyms0, seen_rel = 0;
        for (;;) {
            for (; done < norder; done++)
                compile(order[done]);
            /* the code generator calls a few library functions on its own */
            for (; seen_syms < M->nsyms; seen_syms++) {
                ObjSym *ms = &M->syms[seen_syms];
                u32 g = ms->sec == SEC_UNDEF && ms->name && ms->name < nnames ? gsym[ms->name] : 0xffffffffU;
                root(g);
                if (g != 0xffffffffU && fn_of[g] && fns[fn_of[g] - 1].state == 3)
                    emit_late(fn_of[g] - 1);
            }
            /* a call that was not inlined after all still needs its target */
            for (; seen_rel < M->nrel[SEC_TEXT]; seen_rel++) {
                u32 rs = M->rel[SEC_TEXT][seen_rel].sym, t = rs < nsyms0 ? fn_of[rs] : 0;
                if (t && fns[t - 1].state == 3)
                    emit_late(t - 1);
            }
            if (done == norder && seen_syms == M->nsyms)
                break;
        }
    }
    inline_reset();
    free(order);
    for (j = 0; j < nfns; j++) {
        if (fns[j].state != 2)
            M->syms[fns[j].syms[0]].sec = SEC_NONE; /* never referred to */
        free(fns[j].syms);
    }

    for (i = 0; i < n; i++) {
        if (objs[i]->sec[SEC_IR].len) {
            free(maps[i]);
            if (i != first)
                continue;
            objs[out++] = M;
        } else {
            objs[out++] = objs[i];
        }
    }
    *nobjs = out;
    free(maps);
    free(gsym);
    free(fn_of);
    free(is_fn);
}
