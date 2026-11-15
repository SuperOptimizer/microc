/* CFG cleanup, promotion of stack slots to SSA values, dead code removal. */
#include "ir.h"
#include <stdlib.h>
#include <string.h>

/* Drops unreachable blocks and the edges coming out of them. */
void opt_cfg_clean(Func *f)
{
    u32 *stack = (u32 *)xmalloc(f->nblk * sizeof(u32));
    u32 sp = 0, b, k, n;
    for (b = 1; b < f->nblk; b++)
        f->blk[b].flag = 0;
    stack[sp++] = 1;
    f->blk[1].flag = 1;
    while (sp) {
        Block *bb = &f->blk[stack[--sp]];
        n = bb->term == TERM_BR ? 2 : bb->term == TERM_JMP ? 1 : 0;
        for (k = 0; k < n; k++) {
            if (!f->blk[bb->succ[k]].flag) {
                f->blk[bb->succ[k]].flag = 1;
                stack[sp++] = bb->succ[k];
            }
        }
    }
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        if (!bb->flag) {
            u32 v, nx;
            for (v = bb->first; v; v = nx) {
                nx = f->ins[v].next;
                f->ins[v].blk = 0;
                f->ins[v].prev = f->ins[v].next = 0;
            }
            bb->first = bb->last = bb->phis = 0;
            bb->npreds = 0;
            bb->term = TERM_NONE;
            continue;
        }
        n = 0;
        for (k = 0; k < bb->npreds; k++)
            if (f->blk[bb->preds[k]].flag)
                bb->preds[n++] = bb->preds[k];
        bb->npreds = n;
    }
    free(stack);
}

/* ---- slot promotion (after Braun et al., on a complete CFG) ---- */

#define NOVAR 0xffffffffU

static Func *pf;
static u32 *slot_var; /* slot -> variable index or NOVAR */
static u32 *var_slot; /* -g: variable index -> slot */
static u8 *var_mem;
static u32 nvars;
static u32 *defs; /* [block * nvars + var]: value at the end of the block */
static u32 *work; /* phis whose operands are still to be filled */
static u32 nwork, capwork;
static u32 *work_var;
static u32 *pend, npend, cappend; /* placeholders for values on entry to a block */

static int mem_ty(int mem)
{
    switch (mem) {
    case M_64: return IT_I64;
    case M_F32: return IT_F32;
    case M_F64: return IT_F64;
    }
    return IT_I32;
}

static u32 new_phi(u32 b, u32 var)
{
    u32 v = ir_phi(pf, b, mem_ty(var_mem[var]));
    if (opt_debug)
        pf->ins[v].var = var_slot[var] + 1; /* the variable's value where paths join */
    if (nwork == capwork) {
        capwork = capwork ? capwork * 2 : 256;
        work = (u32 *)xrealloc(work, capwork * sizeof(u32));
        work_var = (u32 *)xrealloc(work_var, capwork * sizeof(u32));
    }
    work[nwork] = v;
    work_var[nwork++] = var;
    return v;
}

#define READING 0xffffffffU /* in defs: the value is being worked out */

/* A load that comes before any store in its block reads the value on
   entry. That is not known while blocks are still being scanned, so the
   load becomes a copy of a placeholder: an OP_NOP flagged PENDING, with the
   variable in b and the block in c. settle() turns it into a copy of the
   real value the first time someone needs it. */
#define PENDING 0x40
#define SETTLING 0x80

static u32 read_end(u32 b, u32 var);

static u32 settle(u32 v)
{
    for (;;) {
        Ins *i = &pf->ins[v];
        u32 b, var, p, same = 0, a;
        int differ = 0;
        Block *bb;
        if (i->op == OP_COPY) {
            v = i->a;
            continue;
        }
        if (i->op != OP_NOP || !(i->flags & PENDING))
            return v;
        b = i->c;
        var = i->b;
        if (i->flags & SETTLING) {
            /* a loop came back to the block: this is where its phi goes */
            a = new_phi(b, var);
        } else {
            i->flags |= SETTLING;
            bb = &pf->blk[b];
            for (p = 0; p < bb->npreds; p++) {
                a = read_end(bb->preds[p], var);
                if (same && a != same)
                    differ = 1;
                same = a;
            }
            i = &pf->ins[v];
            if (i->op == OP_COPY)
                return settle(v); /* settled meanwhile by the loop case */
            a = differ ? new_phi(b, var) : same;
        }
        i = &pf->ins[v];
        i->op = OP_COPY;
        i->flags = 0;
        i->a = a;
        i->b = i->c = 0;
        pf->copies = 1;
        return a;
    }
}

/* The value of variable var at the end of block b. Where several ways in
   meet, a phi is made only if they bring different values, or if the
   question comes back to the block while it is being answered (a loop). */
static u32 read_end(u32 b, u32 var)
{
    Block *bb = &pf->blk[b];
    u32 *d = &defs[(size_t)b * nvars + var];
    u32 v;
    if (*d == READING) {
        v = new_phi(b, var);
        defs[(size_t)b * nvars + var] = v;
        return v;
    }
    if (*d) {
        v = settle(*d);
        defs[(size_t)b * nvars + var] = v;
        return v;
    }
    if (bb->npreds == 0) {
        /* read of an uninitialised variable */
        int ty = mem_ty(var_mem[var]);
        v = ty == IT_F32 || ty == IT_F64 ? ir_fconst(pf, ty, 0.0) : ir_const(pf, ty, 0);
    } else if (bb->npreds == 1) {
        v = read_end(bb->preds[0], var);
    } else {
        u32 p, same = 0;
        int differ = 0;
        *d = READING;
        for (p = 0; p < bb->npreds; p++) {
            u32 a = read_end(bb->preds[p], var);
            if (same && a != same)
                differ = 1;
            same = a;
        }
        d = &defs[(size_t)b * nvars + var];
        if (*d != READING)
            return *d; /* a loop came back here and made the phi */
        v = differ ? new_phi(b, var) : same;
    }
    defs[(size_t)b * nvars + var] = v;
    return v;
}

/* Normalises a value stored to a narrow variable, as a load would. */
static u32 narrow(u32 store, u32 val, int mem)
{
    Ins *i = &pf->ins[val];
    u64 c;
    int op;
    u32 v;
    switch (mem) {
    case M_8S: op = OP_SEXT8; break;
    case M_8U: op = OP_ZEXT8; break;
    case M_16S: op = OP_SEXT16; break;
    case M_16U: op = OP_ZEXT16; break;
    default: return val;
    }
    if (ir_is_const(pf, val, &c)) {
        switch (op) {
        case OP_SEXT8: c = (u64)(i64)(i8)(u8)c; break;
        case OP_ZEXT8: c &= 0xff; break;
        case OP_SEXT16: c = (u64)(i64)(i16)(u16)c; break;
        default: c &= 0xffff; break;
        }
        return ir_const(pf, IT_I32, c);
    }
    if (i->op == op || (i->op == OP_LOAD && i->mem == mem))
        return val;
    if (op == OP_SEXT16 && (i->op == OP_SEXT8 || i->op == OP_ZEXT8))
        return val;
    if (op == OP_ZEXT16 && i->op == OP_ZEXT8)
        return val;
    if ((op == OP_ZEXT8 || op == OP_ZEXT16) && i->op >= OP_EQ && i->op <= OP_UGE)
        return val;
    v = ir_raw(pf, op, IT_I32, val, 0);
    ir_insert_before(pf, store, v);
    return v;
}

u32 opt_promoted; /* how many locals the last opt_promote turned into values */

void opt_promote(Func *f)
{
    u32 b, v, k, kk, n, nx, sweep;
    int changed;

    opt_cfg_clean(f);
    if (f->nslots == 0)
        return;
    pf = f;
    slot_var = (u32 *)xmalloc(f->nslots * sizeof(u32));
    var_slot = (u32 *)xmalloc(f->nslots * sizeof(u32));
    var_mem = (u8 *)xmalloc(f->nslots);
    /* 0xfe: not yet seen; 0xff: not promotable */
    memset(var_mem, 0xfe, f->nslots);

    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        /* an address that a phi chooses is in use as an address */
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++) {
                Ins *o = &f->ins[ir_resolve(f, f->xargs[f->ins[v].a + k])];
                if (o->op == OP_SLOT)
                    var_mem[o->a] = 0xff;
            }
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 *ops;
            IR_OPS(f, i, ops, n);
            if (i->op == OP_CALL && f->ins[i->c].op == OP_SLOT)
                var_mem[f->ins[i->c].a] = 0xff;
            for (k = 0; k < n; k++) {
                Ins *o = &f->ins[ops[k]];
                if (o->op != OP_SLOT)
                    continue;
                if (k == 0 && o->b == 0 && (i->op == OP_LOAD || i->op == OP_STORE)) {
                    u32 size = i->mem <= M_8U ? 1 : i->mem <= M_16U ? 2 : i->mem == M_32 || i->mem == M_F32 ? 4 : 8;
                    if (var_mem[o->a] == 0xfe && f->slots[o->a].size == size)
                        var_mem[o->a] = i->mem;
                    else if (var_mem[o->a] != i->mem)
                        var_mem[o->a] = 0xff;
                } else {
                    var_mem[o->a] = 0xff;
                }
            }
        }
        if (bb->arg && f->ins[bb->arg].op == OP_SLOT)
            var_mem[f->ins[bb->arg].a] = 0xff;
    }
    nvars = 0;
    opt_promoted = 0;
    for (k = 0; k < f->nslots; k++) {
        if (var_mem[k] < 0xfe) {
            var_mem[nvars] = var_mem[k];
            var_slot[nvars] = k;
            slot_var[k] = nvars++;
            f->slots[k].size = 0; /* no longer needs storage */
        } else {
            if (var_mem[k] == 0xfe)
                f->slots[k].size = 0; /* never accessed */
            slot_var[k] = NOVAR;
        }
    }
    opt_promoted = nvars;
    if (nvars) {
        defs = (u32 *)xmalloc((size_t)f->nblk * nvars * sizeof(u32));
        memset(defs, 0, (size_t)f->nblk * nvars * sizeof(u32));
        nwork = 0;
        npend = 0;

        /* Local pass: thread values through each block. A load before any
           store in its block reads the value at the end of the predecessors. */
        for (b = 1; b < f->nblk; b++) {
            Block *bb = &f->blk[b];
            u32 *cur = &defs[(size_t)b * nvars]; /* a block that only reads a variable still ends with the entry value */
            for (v = bb->first; v; v = nx) {
                Ins *i = &f->ins[v];
                Ins *o;
                u32 var;
                nx = i->next;
                if (i->op != OP_LOAD && i->op != OP_STORE)
                    continue;
                o = &f->ins[i->a];
                if (o->op != OP_SLOT || slot_var[o->a] == NOVAR)
                    continue;
                var = slot_var[o->a];
                if (i->op == OP_STORE) {
                    cur[var] = narrow(v, ir_resolve(f, i->b), i->mem);
                    if (opt_debug && f->ins[cur[var]].blk && !f->ins[cur[var]].var)
                        f->ins[cur[var]].var = o->a + 1; /* -g: the value the variable now has */
                    ir_remove(f, v);
                    f->ins[v].op = OP_NOP;
                } else {
                    if (!cur[var]) {
                        /* entry value: a phi placeholder, resolved below */
                        u32 ph;
                        if (bb->npreds == 0) {
                            int ty = mem_ty(var_mem[var]);
                            ph = ty == IT_F32 || ty == IT_F64 ? ir_fconst(f, ty, 0.0) : ir_const(f, ty, 0);
                        } else {
                            ph = ir_raw(f, OP_NOP, mem_ty(var_mem[var]), 0, var);
                            f->ins[ph].c = b;
                            f->ins[ph].flags = PENDING;
                            if (npend == cappend) {
                                cappend = cappend ? cappend * 2 : 256;
                                pend = (u32 *)xrealloc(pend, cappend * sizeof(u32));
                            }
                            pend[npend++] = ph;
                        }
                        cur[var] = ph;
                    }
                    ir_replace(f, v, cur[var]);
                }
            }
        }
        for (k = 0; k < npend; k++)
            settle(pend[k]);
        /* Fill phi operands; this may create more phis. */
        for (k = 0; k < nwork; k++) {
            u32 ph = work[k], var = work_var[k], p;
            Block *bb = &f->blk[f->ins[ph].blk];
            for (p = 0; p < bb->npreds; p++) {
                u32 val = read_end(bb->preds[p], var);
                f->xargs[f->ins[ph].a + p] = val;
            }
        }
        /* Remove trivial phis: all operands equal, ignoring self-references. */
        /* removals cascade from operands to users; sweeping the list in
           alternating directions follows the cascade whichever way it runs */
        sweep = 0;
        do {
            changed = 0;
            sweep++;
            for (kk = 0; kk < nwork; kk++) {
                u32 ph = work[(sweep & 1) ? nwork - 1 - kk : kk], same = 0, p;
                Ins *i = &f->ins[ph];
                if (i->op != OP_PHI)
                    continue;
                for (p = 0; p < i->b; p++) {
                    u32 a = ir_resolve(f, f->xargs[i->a + p]);
                    f->xargs[i->a + p] = a;
                    if (a == ph || a == same)
                        continue;
                    if (same) {
                        same = 0xffffffffU;
                        break;
                    }
                    same = a;
                }
                if (same == 0xffffffffU)
                    continue;
                if (same == 0) {
                    int ty = i->ty;
                    same = ty == IT_F32 || ty == IT_F64 ? ir_fconst(f, ty, 0.0) : ir_const(f, ty, 0);
                    i = &f->ins[ph];
                }
                i->op = OP_COPY; /* unlinked from its block below, all at once */
                f->copies = 1;
                i->a = same;
                changed = 1;
            }
        } while (changed);
        if (nwork)
            for (b = 1; b < f->nblk; b++) {
                u32 *link = &f->blk[b].phis;
                while (*link) {
                    Ins *i = &f->ins[*link];
                    if (i->op == OP_PHI) {
                        link = &i->next;
                    } else {
                        *link = i->next;
                        i->blk = 0;
                    }
                }
            }
        free(defs);
    }
    free(slot_var);
    free(var_slot);
    free(var_mem);
    /* copies left behind are resolved by value numbering, which reads
       every operand through them */
}

/* ---- splitting of local arrays and records ---- */

/* A stack slot that is only ever read and written at constant places, each place always as the same kind of
   scalar and no two places overlapping, with its address going nowhere else, is as good as one variable for
   each place: it is replaced by one slot each, which opt_promote then turns into values. Clearing all or
   part of it becomes storing zeros, and a copy of bytes to or from it a load and a store for each place
   (a copy between two such slots gives them the same places; bytes that are only ever copied are places of
   plain words). What unrolled loops leave of the arrays and records of device code. */
typedef struct Place {
    i32 off;
    u32 size;
    u8 mem;
    u32 to;   /* the slot that takes the place over */
    u32 next; /* in the slot's list, by offset */
} Place;

typedef struct SUse {
    u32 v;            /* a load, a store, a clearing, a copy */
    u32 slot[2];      /* 1 + the slot of operand a, of operand b (copy: the source); 0: not a slot */
    i32 off[2];
} SUse;

static Place *places;
static u32 nplaces, capplaces;
static u32 *slot_places; /* by slot: 1 + its first place */
static u8 *slot_bad;
static u8 *slot_mixed;
static int split_changed;

static u32 mem_size(int mem)
{
    return mem <= M_8U ? 1 : mem <= M_16U ? 2 : mem == M_32 || mem == M_F32 ? 4 : 8;
}

static void slot_fail(u32 s)
{
    slot_bad[s] = 1;
    split_changed = 1;
}

static int mem_float(int mem)
{
    return mem == M_F32 || mem == M_F64;
}

/* The kind of a place of size bytes that is whole numbers. */
static int mem_int(u32 size)
{
    return size == 8 ? M_64 : size == 4 ? M_32 : size == 2 ? M_16U : M_8U;
}

/* Puts a place into slot s's list unless there is one at off. */
static void place_put(u32 s, i32 off, u32 size, int mem)
{
    u32 *link;
    if (nplaces == capplaces) {
        capplaces = capplaces ? capplaces * 2 : 256;
        places = (Place *)xrealloc(places, capplaces * sizeof(Place));
    }
    for (link = &slot_places[s]; *link && places[*link - 1].off < off; link = &places[*link - 1].next)
        ;
    if (*link && places[*link - 1].off == off)
        return;
    places[nplaces].off = off;
    places[nplaces].size = size;
    places[nplaces].mem = (u8)mem;
    places[nplaces].to = 0;
    places[nplaces].next = *link;
    *link = ++nplaces;
}

/* Slot s is read or written as a mem at off: there is a place for that, or, in device code, the places it
   lies over or in are cut into equal smaller ones of whole numbers that it is made of (the bits of a
   floating value are such a number). If that cannot be, s is not split. */
static void place_add(u32 s, i32 off, int mem)
{
    u32 size = mem_size(mem), p, g = size, lo, hi, *link;
    int n = 0, same = 0;
    if (slot_bad[s])
        return;
    if (off < 0 || (u32)off + size > pf->slots[s].size) {
        slot_fail(s);
        return;
    }
    lo = (u32)off;
    hi = lo + size;
    for (p = slot_places[s]; p; p = places[p - 1].next) {
        Place *q = &places[p - 1];
        if ((u32)q->off >= (u32)off + size || (u32)q->off + q->size <= (u32)off)
            continue;
        n++;
        if (q->off == off && q->size == size) {
            same = 1;
            if (q->mem == mem)
                return;
            if (!slot_mixed[s]) {
                slot_mixed[s] = 1; /* read or written as more than one kind: every use is gone over */
                split_changed = 1;
            }
            if (!mem_float(q->mem) && !mem_float(mem))
                return;
            if (!opt_dev_fn) {
                slot_fail(s);
                return;
            }
            q->mem = (u8)mem_int(size); /* its bits */
            split_changed = 1;
            return;
        }
        if (q->size < g)
            g = q->size;
        if ((u32)q->off < lo)
            lo = (u32)q->off;
        if ((u32)q->off + q->size > hi)
            hi = (u32)q->off + q->size;
    }
    (void)same;
    if (!n) {
        place_put(s, off, size, mem_float(mem) ? mem : mem_int(size));
        split_changed = 1;
        return;
    }
    if (!opt_dev_fn) {
        slot_fail(s);
        return;
    }
    /* every end must be at a multiple of the smallest size */
    if (((u32)off - lo) % g || size % g) {
        slot_fail(s);
        return;
    }
    for (p = slot_places[s]; p; p = places[p - 1].next) {
        Place *q = &places[p - 1];
        if ((u32)q->off >= hi || (u32)q->off + q->size <= lo)
            continue;
        if (((u32)q->off - lo) % g || q->size % g) {
            slot_fail(s);
            return;
        }
    }
    /* out with the places in [lo, hi), in with the small ones */
    for (link = &slot_places[s]; (p = *link) != 0;) {
        Place *q = &places[p - 1];
        if ((u32)q->off >= lo && (u32)q->off < hi) {
            u32 a0 = (u32)q->off, a1 = a0 + q->size;
            *link = q->next;
            q->size = 0;
            /* (remembered below by its range: put back as small ones) */
            q->next = 0;
            q->to = a1; /* scratch */
            q->off = (i32)a0;
            q->mem = 0xff;
        } else {
            link = &q->next;
        }
    }
    {
        u32 k, cnt = nplaces, x;
        for (k = 0; k < cnt; k++)
            if (places[k].mem == 0xff) {
                u32 a0 = (u32)places[k].off, a1 = places[k].to;
                places[k].mem = 0xfe; /* dead */
                places[k].to = 0;
                for (x = a0; x < a1; x += g)
                    place_put(s, (i32)x, g, mem_int(g));
            }
        for (x = (u32)off; x < (u32)off + size; x += g)
            place_put(s, (i32)x, g, mem_int(g));
    }
    split_changed = 1;
}

/* No place of slot s may lie across an end of the range [off, off + n). */
static void place_range(u32 s, i32 off, u32 n)
{
    u32 p;
    if (slot_bad[s])
        return;
    if (off < 0 || (u32)off + n > pf->slots[s].size) {
        slot_bad[s] = 1;
        split_changed = 1;
        return;
    }
    for (p = slot_places[s]; p; p = places[p - 1].next) {
        Place *q = &places[p - 1];
        u32 a0 = (u32)q->off, a1 = a0 + q->size, z0 = (u32)off, z1 = z0 + n;
        if (a0 < z1 && z0 < a1 && !(a0 >= z0 && a1 <= z1)) {
            slot_bad[s] = 1;
            split_changed = 1;
            return;
        }
    }
}

/* The range [off, off + n) of slot s is copied to or from memory that is aligned to al and no more: a place
   in it that is wider, or not at a multiple of its width, cannot be read or written there by itself. */
static void place_align(u32 s, i32 off, u32 n, u32 al)
{
    u32 p;
    if (slot_bad[s])
        return;
    for (p = slot_places[s]; p; p = places[p - 1].next) {
        Place *q = &places[p - 1];
        if (q->off < off || (u32)q->off >= (u32)off + n)
            continue;
        if (q->size > al && opt_dev_fn && (al == 1 || al == 2 || al == 4) && (u32)(q->off - off) % al == 0) {
            /* device code: the place is cut into whole numbers that can be */
            place_add(s, q->off, mem_int(al));
            if (slot_bad[s])
                return;
            p = slot_places[s]; /* (the list is another now: from the start) */
            if (!p)
                return;
            q = &places[p - 1];
            if (q->off < off || (u32)q->off >= (u32)off + n)
                continue;
        }
        if (q->size > al || (u32)(q->off - off) % q->size) {
            slot_bad[s] = 1;
            split_changed = 1;
            return;
        }
    }
}

/* The bytes of [off, off + n) of slot s that are in no place become places of words no wider than al. */
static void place_fill(u32 s, i32 off, u32 n, u32 al)
{
    u32 p, pos = (u32)off, end = (u32)off + n;
    if (slot_bad[s])
        return;
    while (pos < end && !slot_bad[s]) {
        u32 lim = end, c;
        for (p = slot_places[s]; p; p = places[p - 1].next) {
            Place *q = &places[p - 1];
            if ((u32)q->off <= pos && pos < (u32)q->off + q->size)
                break;
            if ((u32)q->off > pos) {
                lim = (u32)q->off < end ? (u32)q->off : end;
                p = 0;
                break;
            }
        }
        if (p) {
            pos = (u32)places[p - 1].off + places[p - 1].size;
            continue;
        }
        for (c = 8; c > 1 && (c > al || c > lim - pos || (pos - (u32)off) % c); c >>= 1)
            ;
        place_add(s, (i32)pos, c == 8 ? M_64 : c == 4 ? M_32 : c == 2 ? M_16U : M_8U);
        pos += c;
    }
}

static u32 split_addr(Func *f, u32 at, u32 base, u32 d)
{
    u32 a;
    if (f->ins[base].op == OP_SLOT)
        return ir_slot_addr(f, f->ins[base].a, (i64)(i32)f->ins[base].b + d);
    if (!d)
        return base;
    a = ir_raw(f, OP_ADD, IT_I64, base, ir_const(f, IT_I64, d));
    ir_insert_before(f, at, a);
    return a;
}

static u32 split_ins(Func *f, u32 at, int op, int ty, u32 a, u32 b)
{
    u32 v = ir_raw(f, op, ty, a, b);
    ir_insert_before(f, at, v);
    return v;
}

/* What a load of kind mem at off of the split slot s gives, worked out before the instruction at. */
static u32 split_read(Func *f, u32 at, u32 s, i32 off, int mem)
{
    u32 size = mem_size(mem), p, v, acc = 0;
    int wty = size == 8 ? IT_I64 : IT_I32;
    for (p = slot_places[s]; p && places[p - 1].off < off; p = places[p - 1].next)
        ;
    if (places[p - 1].size == size) {
        int pm = places[p - 1].mem;
        v = split_ins(f, at, OP_LOAD, mem_ty(pm), ir_slot_addr(f, places[p - 1].to, 0), 0);
        f->ins[v].mem = (u8)pm;
        if (pm == mem)
            return v;
    } else {
        for (; p && (u32)places[p - 1].off < (u32)off + size; p = places[p - 1].next) {
            int pm = places[p - 1].mem;
            u32 sh = (u32)(places[p - 1].off - off) * 8;
            v = split_ins(f, at, OP_LOAD, IT_I32, ir_slot_addr(f, places[p - 1].to, 0), 0);
            f->ins[v].mem = (u8)pm;
            if (wty == IT_I64)
                v = split_ins(f, at, OP_ZEXT32, IT_I64, v, 0);
            if (sh)
                v = split_ins(f, at, OP_SHL, wty, v, ir_const(f, wty, sh));
            acc = acc ? split_ins(f, at, OP_OR, wty, acc, v) : v;
        }
        v = acc;
    }
    /* v: the bits, as a whole number */
    if (mem_float(mem))
        return split_ins(f, at, OP_BITCAST, mem_ty(mem), v, 0);
    if (mem == M_8S)
        return split_ins(f, at, OP_SEXT8, IT_I32, v, 0);
    if (mem == M_16S)
        return split_ins(f, at, OP_SEXT16, IT_I32, v, 0);
    return v;
}

/* A store of val as a mem at off of the split slot s, put before the instruction at. */
static void split_write(Func *f, u32 at, u32 s, i32 off, int mem, u32 val)
{
    u32 size = mem_size(mem), p, st;
    int wty = size == 8 ? IT_I64 : IT_I32;
    for (p = slot_places[s]; p && places[p - 1].off < off; p = places[p - 1].next)
        ;
    if (places[p - 1].size == size) {
        int pm = places[p - 1].mem;
        if (mem_float(mem) && !mem_float(pm))
            val = split_ins(f, at, OP_BITCAST, wty, val, 0);
        st = split_ins(f, at, OP_STORE, IT_VOID, ir_slot_addr(f, places[p - 1].to, 0), val);
        f->ins[st].mem = (u8)pm;
        return;
    }
    if (mem_float(mem))
        val = split_ins(f, at, OP_BITCAST, wty, val, 0);
    for (; p && (u32)places[p - 1].off < (u32)off + size; p = places[p - 1].next) {
        int pm = places[p - 1].mem;
        u32 sh = (u32)(places[p - 1].off - off) * 8, x = val, to = places[p - 1].to;
        if (sh)
            x = split_ins(f, at, OP_SHR, wty, x, ir_const(f, wty, sh));
        if (wty == IT_I64)
            x = split_ins(f, at, OP_TRUNC, IT_I32, x, 0);
        st = split_ins(f, at, OP_STORE, IT_VOID, ir_slot_addr(f, to, 0), x);
        f->ins[st].mem = (u8)pm;
    }
}

/* Device code: a load through an address that is one of several locals' (a reference bound to "c ? a : b",
   say) becomes the same choice between what is in each, so that the locals need not stay in memory for
   the address's sake. This is done for locals written only directly and in the entry block, and choices
   that are only read through. Returns whether anything was changed. */
static int select_loads(Func *f)
{
    u32 b, v, k, n, *ops, *dof, nins0 = f->nins;
    u64 cv;
    u8 *drv, *sw;
    int phis, changed = 0;
    drv = (u8 *)xmalloc(f->nins + 1);  /* 1: a choice between addresses of locals; 2: one not to be touched */
    memset(drv, 0, f->nins + 1);
    dof = (u32 *)xmalloc((f->nins + 1) * sizeof(u32)); /* of a choice plus a constant: the choice */
    memset(dof, 0, (f->nins + 1) * sizeof(u32));
    sw = (u8 *)xmalloc(f->nslots + 1); /* the local is written elsewhere, or its address is used otherwise */
    memset(sw, 0, f->nslots + 1);
    for (b = 1; b < f->nblk; b++) {
        for (v = f->blk[b].phis; v; v = f->ins[v].next) {
            IR_OPS(f, &f->ins[v], ops, n);
            for (k = 0; k < n && f->ins[ops[k]].op == OP_SLOT; k++)
                ;
            if (n && k == n && n == f->blk[b].npreds && b != 1)
                drv[v] = 1;
        }
        for (v = f->blk[b].first; v; v = f->ins[v].next)
            if (f->ins[v].op == OP_SELECT && f->ins[f->ins[v].b].op == OP_SLOT && f->ins[f->ins[v].c].op == OP_SLOT)
                drv[v] = 1;
    }
    for (b = 1; b < f->nblk; b++)
        for (v = f->blk[b].first; v; v = f->ins[v].next)
            if (f->ins[v].op == OP_ADD && drv[f->ins[v].a] && ir_is_const(f, f->ins[v].b, &cv))
                dof[v] = f->ins[v].a;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        for (phis = 1; phis >= 0; phis--)
            for (v = phis ? bb->phis : bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                int rd = i->op == OP_LOAD && !i->flags;
                IR_OPS(f, i, ops, n);
                if (i->op == OP_CALL && f->ins[i->c].op == OP_SLOT)
                    sw[f->ins[i->c].a] = 1;
                for (k = 0; k < n; k++) {
                    u32 o = ops[k];
                    if (f->ins[o].op == OP_SLOT) {
                        if (!((rd && k == 0) || (i->op == OP_STORE && k == 0 && b == 1 && !i->flags) ||
                              (i->op == OP_MEMCPY && k == 1) || (drv[v] && (phis || k))))
                            sw[f->ins[o].a] = 1;
                    } else if (drv[o]) {
                        if (!((rd && k == 0) || (dof[v] == o && k == 0)))
                            drv[o] = 2;
                    } else if (dof[o]) {
                        if (!(rd && k == 0))
                            drv[dof[o]] = 2;
                    }
                }
            }
        if (bb->arg) {
            if (f->ins[bb->arg].op == OP_SLOT)
                sw[f->ins[bb->arg].a] = 1;
            else if (drv[bb->arg])
                drv[bb->arg] = 2;
            else if (dof[bb->arg])
                drv[dof[bb->arg]] = 2;
        }
    }
    for (b = 1; b < f->nblk; b++) {
        u32 next;
        int clean = 1;
        for (v = f->blk[b].first; v; v = next) {
            u32 a, np, size, db;
            u64 d = 0;
            int mem = f->ins[v].mem, ty = f->ins[v].ty, ok = 1, op = f->ins[v].op, early;
            next = f->ins[v].next;
            if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_VSTORE || op == OP_BYVAL ||
                op == OP_VASTART)
                clean = 0;
            if (f->ins[v].op != OP_LOAD || f->ins[v].flags)
                continue;
            a = f->ins[v].a;
            if (v >= nins0 || a >= nins0)
                continue; /* made here */
            if (dof[a]) {
                ir_is_const(f, f->ins[a].b, &d);
                a = dof[a];
            }
            if (!drv[a])
                continue;
            /* (read at the top of the block that chooses, before anything is written there: what each local
               holds at the end of the block it comes from, whatever else is done with it) */
            early = clean && f->ins[a].op == OP_PHI && f->ins[a].blk == b;
            if (drv[a] != 1 && !early)
                continue;
            size = mem_size(mem);
            if (f->ins[a].op == OP_SELECT) {
                ops = &f->ins[a].b;
                n = 2;
            } else {
                IR_OPS(f, &f->ins[a], ops, n);
            }
            for (k = 0; k < n; k++) {
                Ins *o = &f->ins[ops[k]];
                i64 off = (i64)(i32)o->b + (i64)d;
                /* (a choice made by a select is read where the load stood: the same bytes at the same moment,
                   whatever is done with the locals elsewhere) */
                if ((sw[o->a] && !early && f->ins[a].op != OP_SELECT) || off < 0 || (u64)off + size > f->slots[o->a].size)
                    ok = 0;
            }
            if (!ok)
                continue;
            if (f->ins[a].op == OP_SELECT) {
                u32 x = f->ins[a].b, y = f->ins[a].c, lx, ly;
                lx = ir_raw(f, OP_LOAD, ty, ir_slot_addr(f, f->ins[x].a, (i64)(i32)f->ins[x].b + (i64)d), 0);
                f->ins[lx].mem = (u8)mem;
                ir_insert_before(f, v, lx);
                ly = ir_raw(f, OP_LOAD, ty, ir_slot_addr(f, f->ins[y].a, (i64)(i32)f->ins[y].b + (i64)d), 0);
                f->ins[ly].mem = (u8)mem;
                ir_insert_before(f, v, ly);
                f->ins[v].op = OP_SELECT;
                f->ins[v].mem = 0;
                f->ins[v].a = f->ins[a].a;
                f->ins[v].b = lx;
                f->ins[v].c = ly;
                changed = 1;
                continue;
            }
            db = f->ins[a].blk;
            np = ir_phi(f, db, ty);
            for (k = 0; k < n; k++) {
                u32 x = f->xargs[f->ins[a].a + k], l;
                l = ir_raw(f, OP_LOAD, ty, ir_slot_addr(f, f->ins[x].a, (i64)(i32)f->ins[x].b + (i64)d), 0);
                f->ins[l].mem = (u8)mem;
                ir_append(f, f->blk[db].preds[k], l);
                f->xargs[f->ins[np].a + k] = l;
            }
            ir_replace(f, v, np);
            changed = 1;
        }
    }
    free(drv);
    free(dof);
    free(sw);
    return changed;
}

int opt_split(Func *f)
{
    u32 b, v, k, n, nu = 0, capu = 0, nslots = f->nslots, j, p, done = 0;
    SUse *use = 0;
    if (!nslots)
        return 0;
    if (opt_dev_fn && select_loads(f)) {
        ir_resolve_all(f);
        opt_dce(f);
    }
    pf = f;
    slot_bad = (u8 *)xmalloc(nslots);
    memset(slot_bad, 0, nslots);
    slot_mixed = (u8 *)xmalloc(nslots);
    memset(slot_mixed, 0, nslots);
    slot_places = (u32 *)xmalloc(nslots * sizeof(u32));
    memset(slot_places, 0, nslots * sizeof(u32));
    nplaces = 0;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        int phis;
        for (phis = 1; phis >= 0; phis--) {
            for (v = phis ? bb->phis : bb->first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                u32 *ops;
                int any = 0;
                SUse u;
                IR_OPS(f, i, ops, n);
                if (i->op == OP_CALL && f->ins[i->c].op == OP_SLOT)
                    slot_bad[f->ins[i->c].a] = 1;
                u.v = v;
                u.slot[0] = u.slot[1] = 0;
                u.off[0] = u.off[1] = 0;
                for (k = 0; k < n; k++) {
                    Ins *o = &f->ins[ops[k]];
                    if (o->op != OP_SLOT)
                        continue;
                    if (phis || (i32)o->b < 0 ||
                        !((k == 0 && (i->op == OP_LOAD || i->op == OP_STORE || i->op == OP_MEMZERO)) || (k < 2 && i->op == OP_MEMCPY))) {
                        slot_bad[o->a] = 1;
                        continue;
                    }
                    u.slot[k] = 1 + o->a;
                    u.off[k] = (i32)o->b;
                    any = 1;
                }
                if (!any)
                    continue;
                if (nu == capu) {
                    capu = capu ? capu * 2 : 256;
                    use = (SUse *)xrealloc(use, capu * sizeof(SUse));
                }
                use[nu++] = u;
            }
        }
        if (bb->arg && f->ins[bb->arg].op == OP_SLOT)
            slot_bad[f->ins[bb->arg].a] = 1;
    }
    /* the places */
    for (j = 0; j < nu; j++) {
        Ins *i = &f->ins[use[j].v];
        if (i->op == OP_LOAD || i->op == OP_STORE)
            place_add(use[j].slot[0] - 1, use[j].off[0], i->mem);
    }
    do {
        split_changed = 0;
        for (j = 0; j < nu; j++) {
            Ins *i = &f->ins[use[j].v];
            u32 s0 = use[j].slot[0], s1 = use[j].slot[1];
            if (i->op == OP_MEMZERO) {
                place_range(s0 - 1, use[j].off[0], i->c);
            } else if (i->op == OP_MEMCPY) {
                int g0 = s0 && !slot_bad[s0 - 1], g1 = s1 && !slot_bad[s1 - 1];
                u32 al = i->mem ? i->mem : 1;
                if (g0 && g1 && s0 == s1) {
                    slot_bad[s0 - 1] = 1;
                    split_changed = 1;
                    continue;
                }
                if (g0)
                    place_range(s0 - 1, use[j].off[0], i->c);
                if (g1)
                    place_range(s1 - 1, use[j].off[1], i->c);
                g0 = s0 && !slot_bad[s0 - 1];
                g1 = s1 && !slot_bad[s1 - 1];
                if (g0 && g1) {
                    int side;
                    for (side = 0; side < 2; side++) {
                        u32 from = (side ? s1 : s0) - 1, to = (side ? s0 : s1) - 1;
                        i32 fo = use[j].off[side], too = use[j].off[!side];
                        for (p = slot_places[from]; p; p = places[p - 1].next) {
                            i32 po = places[p - 1].off;
                            int pm = places[p - 1].mem;
                            if (po < fo || (u32)po >= (u32)fo + i->c)
                                continue;
                            place_add(to, too + (po - fo), pm);
                            if (slot_bad[to] || slot_bad[from])
                                break;
                        }
                    }
                } else if (g0) {
                    place_align(s0 - 1, use[j].off[0], i->c, al);
                    place_fill(s0 - 1, use[j].off[0], i->c, al);
                } else if (g1) {
                    place_align(s1 - 1, use[j].off[1], i->c, al);
                    place_fill(s1 - 1, use[j].off[1], i->c, al);
                }
            }
        }
    } while (split_changed);
    /* a slot that is one scalar already, or is never read or written as scalars, stays as it is */
    for (k = 0; k < nslots; k++) {
        if (slot_bad[k])
            continue;
        p = slot_places[k];
        if (!p || (!slot_mixed[k] && !places[p - 1].next && places[p - 1].off == 0 && places[p - 1].size == f->slots[k].size))
            slot_bad[k] = 2;
    }
    for (j = 0; j < nu; j++) {
        /* (a copy between a slot that is one scalar and one that is split is still taken apart) */
        Ins *i = &f->ins[use[j].v];
        if (i->op == OP_MEMCPY || i->op == OP_MEMZERO)
            for (k = 0; k < 2; k++)
                if (use[j].slot[k] && slot_bad[use[j].slot[k] - 1] == 2 && slot_places[use[j].slot[k] - 1])
                    slot_bad[use[j].slot[k] - 1] = 0;
    }
    for (k = 0; k < nslots; k++)
        if (!slot_bad[k])
            for (p = slot_places[k]; p; p = places[p - 1].next)
                places[p - 1].to = ir_new_slot(f, places[p - 1].size, places[p - 1].size);
    for (j = 0; j < nu; j++) {
        u32 at = use[j].v;
        int op = f->ins[at].op, amem = f->ins[at].mem;
        u32 s0 = use[j].slot[0], s1 = use[j].slot[1];
        int g0 = s0 && !slot_bad[s0 - 1], g1 = s1 && !slot_bad[s1 - 1];
        if (!g0 && !g1)
            continue;
        done = 1;
        if (op == OP_LOAD) {
            u32 val = split_read(f, at, s0 - 1, use[j].off[0], amem);
            ir_replace(f, at, val);
        } else if (op == OP_STORE) {
            u32 val = ir_resolve(f, f->ins[at].b);
            split_write(f, at, s0 - 1, use[j].off[0], amem, val);
            ir_remove(f, at);
            f->ins[at].op = OP_NOP;
        } else if (op == OP_MEMZERO) {
            u32 cnt = f->ins[at].c;
            for (p = slot_places[s0 - 1]; p; p = places[p - 1].next) {
                int pm = places[p - 1].mem;
                u32 st, zv, to = places[p - 1].to;
                if (places[p - 1].off < use[j].off[0] || (u32)places[p - 1].off >= (u32)use[j].off[0] + cnt)
                    continue;
                zv = pm == M_F32 ? ir_fconst(f, IT_F32, 0.0) : pm == M_F64 ? ir_fconst(f, IT_F64, 0.0)
                                                                            : ir_const(f, pm == M_64 ? IT_I64 : IT_I32, 0);
                st = ir_raw(f, OP_STORE, IT_VOID, ir_slot_addr(f, to, 0), zv);
                f->ins[st].mem = (u8)pm;
                ir_insert_before(f, at, st);
            }
            ir_remove(f, at);
            f->ins[at].op = OP_NOP;
        } else {
            /* a copy: by the places of a side that is split (both have the same, if both are) */
            int side = g1 ? 1 : 0;
            u32 from = use[j].slot[side] - 1, cnt = f->ins[at].c, dst = f->ins[at].a, src = f->ins[at].b;
            i32 fo = use[j].off[side];
            for (p = slot_places[from]; p; p = places[p - 1].next) {
                u32 d, ld, st, sa, da;
                int pm = places[p - 1].mem;
                i32 po = places[p - 1].off;
                if (po < fo || (u32)po >= (u32)fo + cnt)
                    continue;
                d = (u32)(po - fo);
                if (g1) {
                    sa = ir_slot_addr(f, places[p - 1].to, 0);
                } else {
                    sa = split_addr(f, at, src, d);
                }
                ld = ir_raw(f, OP_LOAD, mem_ty(pm), sa, 0);
                f->ins[ld].mem = (u8)pm;
                ir_insert_before(f, at, ld);
                if (g0) {
                    split_write(f, at, s0 - 1, use[j].off[0] + (i32)d, pm, ld);
                } else {
                    da = split_addr(f, at, dst, d);
                    st = ir_raw(f, OP_STORE, IT_VOID, da, ld);
                    f->ins[st].mem = (u8)pm;
                    ir_insert_before(f, at, st);
                }
            }
            ir_remove(f, at);
            f->ins[at].op = OP_NOP;
        }
    }
    if (done) {
        f->copies = 1;
        ir_resolve_all(f);
    }
    for (k = 0; k < nslots; k++)
        if (!slot_bad[k])
            f->slots[k].size = 0;
    free(use);
    free(slot_bad);
    free(slot_mixed);
    free(slot_places);
    return (int)done;
}

/* ---- dead code elimination ---- */

void opt_dce(Func *f)
{
    u8 *live = (u8 *)xmalloc(f->nins);
    u32 *stack = (u32 *)xmalloc(f->nins * sizeof(u32));
    u32 sp = 0, b, v, k, n, nx;

    memset(live, 0, f->nins);
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        for (v = bb->first; v; v = f->ins[v].next) {
            int op = f->ins[v].op;
            if (op == OP_STORE || op == OP_CALL || op == OP_MEMCPY || op == OP_MEMZERO || op == OP_PARAM || op == OP_VSTORE) {
                live[v] = 1;
                stack[sp++] = v;
            }
        }
        if (bb->arg && !live[bb->arg]) {
            live[bb->arg] = 1;
            stack[sp++] = bb->arg;
        }
    }
    while (sp) {
        Ins *i = &f->ins[stack[--sp]];
        u32 *ops;
        IR_OPS(f, i, ops, n);
        if (i->op == OP_CALL && i->c && !live[i->c]) {
            live[i->c] = 1;
            stack[sp++] = i->c;
        }
        for (k = 0; k < n; k++) {
            u32 a = ops[k];
            if (a && !live[a]) {
                live[a] = 1;
                stack[sp++] = a;
            }
        }
    }
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        u32 *link = &bb->phis;
        while (*link) {
            if (live[*link])
                link = &f->ins[*link].next;
            else
                *link = f->ins[*link].next;
        }
        for (v = bb->first; v; v = nx) {
            nx = f->ins[v].next;
            if (!live[v])
                ir_remove(f, v);
        }
    }
    free(live);
    free(stack);
}
