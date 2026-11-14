/* Inlining of small functions defined earlier in the translation unit.
   Each small function's optimised IR is kept; a call to it is replaced by a
   copy of that IR, spliced into the caller's control-flow graph. */
#include "ir.h"
#include <stdlib.h>
#include <string.h>

int opt_size;
int opt_level = 2; /* 3: spend compile time freely */
int opt_inline_limit = 12; /* instructions, beyond one per parameter */
#define MAX_INLINE_BLOCKS 48
#define ONCE_INLINE_INS 200 /* a function called from one place only */
#define MAX_CALLER_INS 60000

typedef struct Body {
    Ins *ins;
    u32 nins;
    Block *blk;
    u32 nblk;
    u32 *preds; /* all predecessor lists, concatenated */
    u32 *xargs;
    u32 nxargs;
    Slot *slots;
    u32 nslots;
    u8 *param_ty;
    u32 nparams;
    u8 ret_ty;
    u8 has_sret;
    u8 variadic;
    u8 avx;
    u8 always; /* always_inline */
    u32 copies;
    u32 size; /* instructions */
} Body;

static Body **bodies; /* by object symbol */
static u32 nbodies;

static void body_free(Body *y)
{
    free(y->ins);
    free(y->blk);
    free(y->preds);
    free(y->xargs);
    free(y->slots);
    free(y->param_ty);
    free(y);
}

static void stash_reset(void);

void inline_reset(void)
{
    u32 k;
    for (k = 0; k < nbodies; k++)
        if (bodies[k])
            body_free(bodies[k]);
    free(bodies);
    bodies = 0;
    nbodies = 0;
    stash_reset();
}

static void *dup(const void *p, size_t n)
{
    void *q = xmalloc(n);
    if (n)
        memcpy(q, p, n);
    return q;
}

int opt_inline_once; /* the function being compiled has a single call site */
int opt_inline_hot = 40; /* limit at call sites a profile shows to be hot */
int opt_hot_shift = 10;  /* hot: runs at least 1/2^n as often as the busiest block of the program */
#define LIMIT (opt_size ? 3 : opt_inline_limit)

int opt_inline_mode; /* of the function being optimised: 0 inline by size, 1 always, 2 never */
int opt_novec;       /* of the function being optimised: no vector code, which only the processor's back end takes */

static Body *body_copy(Func *f)
{
    Body *y = (Body *)xmalloc(sizeof(Body));
    u32 b, npreds = 0, at = 0;
    for (b = 1; b < f->nblk; b++)
        if (f->blk[b].term != TERM_NONE)
            npreds += f->blk[b].npreds;
    y->size = 0;
    y->always = 0;
    y->ins = (Ins *)dup(f->ins, f->nins * sizeof(Ins));
    y->nins = f->nins;
    y->blk = (Block *)dup(f->blk, f->nblk * sizeof(Block));
    y->nblk = f->nblk;
    y->preds = (u32 *)xmalloc((npreds + 1) * sizeof(u32));
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &y->blk[b];
        if (bb->term == TERM_NONE)
            continue;
        memcpy(y->preds + at, f->blk[b].preds, bb->npreds * sizeof(u32));
        bb->preds = y->preds + at;
        at += bb->npreds;
    }
    y->xargs = (u32 *)dup(f->xargs, f->nxargs * sizeof(u32));
    y->nxargs = f->nxargs;
    y->slots = (Slot *)dup(f->slots, f->nslots * sizeof(Slot));
    y->nslots = f->nslots;
    y->param_ty = (u8 *)dup(f->param_ty, f->nparams);
    y->nparams = f->nparams;
    y->ret_ty = f->ret_ty;
    y->has_sret = f->has_sret;
    y->variadic = f->variadic;
    y->avx = f->avx;
    y->copies = f->copies;
    return y;
}

int inline_saved(u32 sym)
{
    return sym < nbodies && bodies[sym] != 0;
}

/* Remembers f's body if it is small and simple enough to inline. */
void inline_save(Func *f)
{
    u32 b, v, count = 0, live = 0;
    Body *y;

    if (f->variadic || f->blk[1].npreds || opt_inline_mode == 2)
        return;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        if (bb->term == TERM_NONE)
            continue;
        live++;
        for (v = bb->phis; v; v = f->ins[v].next)
            count++;
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            count++;
            if (i->op == OP_VASTART)
                return;
            if (i->op == OP_CALL && f->ins[i->c].op == OP_ADDR && f->ins[i->c].a == f->sym)
                return; /* recursive */
        }
    }
    /* a function the profile saw running is kept up to a larger size, for
       the call sites that matter */
    if (opt_inline_mode != 1 &&
        (count > (opt_inline_once ? ONCE_INLINE_INS : f->blk[1].freq > 1 && !opt_size ? (u32)opt_inline_hot : (u32)LIMIT) + f->nparams ||
         live > MAX_INLINE_BLOCKS))
        return;
    y = body_copy(f);
    y->size = count;
    y->always = opt_inline_mode == 1;
    if (f->sym >= nbodies) {
        u32 n = nbodies ? nbodies : 256;
        while (n <= f->sym)
            n *= 2;
        bodies = (Body **)xrealloc(bodies, n * sizeof(Body *));
        memset(bodies + nbodies, 0, (n - nbodies) * sizeof(Body *));
        nbodies = n;
    }
    if (bodies[f->sym])
        body_free(bodies[f->sym]);
    bodies[f->sym] = y;
}

/* Makes f the remembered body of sym again (optimised, ready for code generation). */
static void body_load(Func *f, Body *y, u32 sym);

void inline_restore(Func *f, u32 sym)
{
    body_load(f, bodies[sym], sym);
    f->variadic = 0;
}

static void body_load(Func *f, Body *y, u32 sym)
{
    u32 b;
    ir_func_reset(f);
    if (f->capins < y->nins) {
        f->capins = y->nins * 2;
        f->ins = (Ins *)xrealloc(f->ins, f->capins * sizeof(Ins));
    }
    memcpy(f->ins, y->ins, y->nins * sizeof(Ins));
    f->nins = y->nins;
    if (f->capblk < y->nblk) {
        f->capblk = y->nblk * 2;
        f->blk = (Block *)xrealloc(f->blk, f->capblk * sizeof(Block));
    }
    memcpy(f->blk, y->blk, y->nblk * sizeof(Block));
    f->nblk = y->nblk;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        if (bb->term == TERM_NONE) {
            bb->npreds = bb->cappreds = 0;
            bb->preds = 0;
            continue;
        }
        bb->cappreds = bb->npreds;
        if (bb->npreds) {
            u32 *p = (u32 *)arena_alloc_raw(&f->arena, bb->npreds * sizeof(u32));
            memcpy(p, y->blk[b].preds, bb->npreds * sizeof(u32));
            bb->preds = p;
        }
    }
    if (f->capxargs < y->nxargs) {
        f->capxargs = y->nxargs * 2;
        f->xargs = (u32 *)xrealloc(f->xargs, f->capxargs * sizeof(u32));
    }
    if (y->nxargs)
        memcpy(f->xargs, y->xargs, y->nxargs * sizeof(u32));
    f->nxargs = y->nxargs;
    if (f->capslots < y->nslots) {
        f->capslots = y->nslots * 2;
        f->slots = (Slot *)xrealloc(f->slots, f->capslots * sizeof(Slot));
    }
    if (y->nslots)
        memcpy(f->slots, y->slots, y->nslots * sizeof(Slot));
    f->nslots = y->nslots;
    if (f->capparams < y->nparams) {
        f->capparams = y->nparams * 2;
        f->param_ty = (u8 *)xrealloc(f->param_ty, f->capparams);
    }
    if (y->nparams)
        memcpy(f->param_ty, y->param_ty, y->nparams);
    f->nparams = y->nparams;
    f->ret_ty = y->ret_ty;
    f->has_sret = y->has_sret;
    f->variadic = y->variadic;
    f->avx = y->avx;
    f->copies = y->copies;
    f->sym = sym;
    ir_rehash(f);
}

/* ---- functions compiled on demand ----
   A function can be put aside as parsed. It is optimised when something
   that is compiled refers to it (after the functions it refers to itself,
   so that they can be inlined into it), and code is generated for it only
   if a reference remains. */

typedef struct Stash {
    Body *y;
    const char *name;
    u8 state; /* 0 none, 1 as parsed, 2 being optimised, 3 optimised */
    u8 mode;  /* opt_inline_mode for it */
    u8 novec; /* opt_novec for it */
    u8 dev;   /* device code only: opt_dev_fn for it */
} Stash;

static Stash *stash; /* by object symbol */
static u32 nstash;

void func_stash(Func *f, int mode)
{
    if (f->sym >= nstash) {
        u32 n = nstash ? nstash : 256;
        while (n <= f->sym)
            n *= 2;
        stash = (Stash *)xrealloc(stash, n * sizeof(Stash));
        memset(stash + nstash, 0, (n - nstash) * sizeof(Stash));
        nstash = n;
    }
    if (stash[f->sym].y)
        body_free(stash[f->sym].y);
    stash[f->sym].y = body_copy(f);
    stash[f->sym].name = f->name;
    stash[f->sym].state = 1;
    stash[f->sym].mode = (u8)(mode & 3);
    stash[f->sym].novec = (u8)(mode >> 2 & 1);
    stash[f->sym].dev = (u8)(mode >> 3 & 1);
}

int func_stashed(u32 sym)
{
    return sym < nstash ? stash[sym].state : 0;
}

void func_unstash(Func *f, u32 sym)
{
    body_load(f, stash[sym].y, sym);
    f->name = stash[sym].name;
}

void func_realise(Func *f, u32 sym)
{
    Body *y;
    u32 v;
    if (sym >= nstash || stash[sym].state != 1)
        return;
    stash[sym].state = 2;
    y = stash[sym].y;
    for (v = 1; v < y->nins; v++)
        if (y->ins[v].op == OP_ADDR && y->ins[v].a != sym)
            func_realise(f, y->ins[v].a);
    func_unstash(f, sym);
    opt_inline_mode = stash[sym].mode;
    opt_novec = stash[sym].novec;
    {
        int was = opt_dev_fn;
        opt_dev_fn = stash[sym].dev;
        opt_run(f);
        opt_dev_fn = was;
    }
    opt_inline_mode = 0;
    opt_novec = 0;
    body_free(stash[sym].y);
    stash[sym].y = body_copy(f);
    stash[sym].state = 3;
}

static Func *f;
static Body *y;
static u32 *vmap, *bmap, *smap;

static u32 map_value(u32 yv)
{
    Ins *i;
    if (vmap[yv])
        return vmap[yv];
    i = &y->ins[yv];
    switch (i->op) {
    case OP_CONST:
        vmap[yv] = ir_const(f, i->ty, (u64)i->a | ((u64)i->b << 32));
        break;
    case OP_FCONST:
        vmap[yv] = ir_fconst_bits(f, i->ty, (u64)i->a | ((u64)i->b << 32));
        break;
    case OP_ADDR:
        vmap[yv] = ir_addr(f, i->a, (i32)i->b);
        break;
    case OP_SLOT:
        if (smap[i->a] == 0xffffffffU)
            smap[i->a] = ir_new_slot(f, y->slots[i->a].size, y->slots[i->a].align);
        vmap[yv] = ir_slot_addr(f, smap[i->a], (i32)i->b);
        break;
    default:
        die("internal: inlining met an unmapped value");
    }
    return vmap[yv];
}

/* Replaces call v (in block b) by a copy of body y. Returns the block that
   holds the instructions that followed the call. */
static u32 inline_one(u32 b, u32 v)
{
    u32 yb, yv, k, n, cont, nrets = 0, *rets, *retvals, result = 0;
    u32 first_new = f->nins, argbase = f->ins[v].a, after, inst = 0;
    int ty = f->ins[v].ty;

    if (opt_debug && f->ins[v].loc && f->ins[f->ins[v].c].op == OP_ADDR)
        inst = dwarf_inline(f->ins[f->ins[v].c].a, f->ins[v].loc); /* -g: the copy's code is this call's */

    /* (an always_inline function may have any number of returns) */
    rets = (u32 *)xmalloc((2 * y->nblk + 2) * sizeof(u32));
    retvals = rets + y->nblk + 1;
    vmap = (u32 *)xmalloc((y->nins + y->nblk + y->nslots + 1) * sizeof(u32));
    bmap = vmap + y->nins;
    smap = bmap + y->nblk;
    memset(vmap, 0, (y->nins + y->nblk) * sizeof(u32));
    memset(smap, 0xff, (y->nslots + 1) * sizeof(u32));

    for (yb = 1; yb < y->nblk; yb++)
        if (y->blk[yb].term != TERM_NONE) {
            u32 site = f->blk[b].freq, in = y->blk[1].freq, fr = y->blk[yb].freq;
            bmap[yb] = ir_new_block(f);
            /* the copy's share of the callee's counts */
            if (site && in > 1 && fr) {
                u64 c = (u64)(fr - 1) * (site - 1) / (in - 1);
                f->blk[bmap[yb]].freq = site > 1 && fr > 1 && !c ? 2 : c > 0x7fffffff ? 0x80000000 : (u32)c + 1;
            } else {
                f->blk[bmap[yb]].freq = site;
            }
        }
    /* copy instructions; operands still name callee values */
    for (yb = 1; yb < y->nblk; yb++) {
        Block *ybb = &y->blk[yb];
        if (ybb->term == TERM_NONE)
            continue;
        for (yv = ybb->phis; yv; yv = y->ins[yv].next) {
            u32 p = ir_phi(f, bmap[yb], y->ins[yv].ty), at;
            /* ir_phi sized the argument list for a block with no predecessors yet */
            f->ins[p].b = 0;
            for (k = 0; k < y->ins[yv].b; k++)
                ir_phi_add_arg(f, p, y->xargs[y->ins[yv].a + k]);
            at = f->ins[p].a;
            (void)at;
            vmap[yv] = p;
        }
        for (yv = ybb->first; yv; yv = y->ins[yv].next) {
            Ins *yi = &y->ins[yv];
            u32 nv;
            if (yi->op == OP_PARAM) {
                /* (x86-64: a parameter that is a block of memory has place holders after it that the call has
                   no arguments for, and its argument is the block to copy: the copy is made here) */
                u32 pk, ak = 0, av;
                for (pk = 0; pk < yi->a; pk++)
                    ak += !(y->param_ty[pk] & 0x40);
                if (y->param_ty[yi->a] & 0x40) {
                    vmap[yv] = 0;
                    continue;
                }
                av = f->xargs[argbase + ak];
                if (y->param_ty[yi->a] & 0x80) {
                    Ins *bv = &f->ins[av];
                    u32 size = bv->b, al = bv->c > 8 ? bv->c : 8, src = bv->a;
                    u32 sl = ir_slot_addr(f, ir_new_slot(f, size ? size : 1, al), 0), m;
                    m = ir_raw(f, OP_MEMCPY, IT_VOID, sl, src);
                    f->ins[m].c = size;
                    f->ins[m].mem = (u8)(f->ins[av].c > 16 ? 16 : f->ins[av].c ? f->ins[av].c : 1);
                    ir_insert_before(f, v, m);
                    ir_remove(f, av);
                    f->ins[av].op = OP_NOP;
                    av = sl;
                }
                vmap[yv] = av;
                continue;
            }
            if (yi->op == OP_CALL) {
                nv = ir_call(f, yi->ty, yi->c, y->xargs + yi->a, yi->b, y->xargs[yi->a - 1], yi->flags);
                f->ins[nv].mem = yi->mem;
                f->ins[nv].loc = dwarf_inline_loc(inst, yi->loc);
                ir_remove(f, nv); /* ir_call may have placed it in f->cur */
            } else if (yi->op == OP_XI) {
                u32 cur = f->cur;
                f->cur = 0;
                nv = ir_xi(f, yi->ty, yi->c, y->xargs + yi->a, yi->b);
                f->cur = cur;
                f->ins[nv].mem = yi->mem;
                f->ins[nv].flags = yi->flags;
            } else {
                nv = ir_raw(f, yi->op, yi->ty, yi->a, yi->b);
                f->ins[nv].mem = yi->mem;
                f->ins[nv].loc = dwarf_inline_loc(inst, yi->loc);
                f->ins[nv].flags = yi->flags;
                f->ins[nv].c = yi->c;
            }
            ir_append(f, bmap[yb], nv);
            vmap[yv] = nv;
        }
    }
    /* rewrite operands, terminators and edges into caller terms */
    for (yb = 1; yb < y->nblk; yb++) {
        Block *ybb = &y->blk[yb];
        u32 nb = bmap[yb];
        if (ybb->term == TERM_NONE)
            continue;
        for (yv = f->blk[nb].phis; yv; yv = f->ins[yv].next)
            for (k = 0; k < f->ins[yv].b; k++)
                f->xargs[f->ins[yv].a + k] = map_value(f->xargs[f->ins[yv].a + k]);
        for (yv = f->blk[nb].first; yv; yv = f->ins[yv].next) {
            n = ir_opcount(f, yv);
            for (k = 0; k < n; k++) {
                u32 mapped = map_value(*ir_opref(f, yv, k));
                *ir_opref(f, yv, k) = mapped;
            }
        }
        for (k = 0; k < ybb->npreds; k++)
            ir_add_pred(f, nb, bmap[ybb->preds[k]]);
        f->blk[nb].term = ybb->term;
        if (ybb->term == TERM_RET) {
            rets[nrets] = nb;
            retvals[nrets++] = ybb->arg ? map_value(ybb->arg) : 0;
        } else {
            f->blk[nb].arg = ybb->arg ? map_value(ybb->arg) : 0;
            f->blk[nb].succ[0] = bmap[ybb->succ[0]];
            if (ybb->term == TERM_BR)
                f->blk[nb].succ[1] = bmap[ybb->succ[1]];
        }
    }
    (void)first_new;

    /* split the caller's block after the call */
    cont = ir_new_block(f);
    f->blk[cont].freq = f->blk[b].freq;
    after = f->ins[v].next;
    {
        Block *bb = &f->blk[b], *cb = &f->blk[cont];
        u32 w;
        if (after) {
            cb->first = after;
            cb->last = bb->last;
            bb->last = v;
            f->ins[v].next = 0;
            f->ins[after].prev = 0;
            for (w = after; w; w = f->ins[w].next)
                f->ins[w].blk = cont;
        }
        cb->term = bb->term;
        cb->arg = bb->arg;
        cb->succ[0] = bb->succ[0];
        cb->succ[1] = bb->succ[1];
        n = cb->term == TERM_BR ? 2 : cb->term == TERM_JMP ? 1 : 0;
        for (k = 0; k < n; k++) {
            Block *sb = &f->blk[cb->succ[k]];
            u32 j;
            for (j = 0; j < sb->npreds; j++)
                if (sb->preds[j] == b)
                    sb->preds[j] = cont;
        }
        bb->term = TERM_JMP;
        bb->arg = 0;
        bb->succ[0] = bmap[1];
        ir_add_pred(f, bmap[1], b);
    }
    for (k = 0; k < nrets; k++) {
        f->blk[rets[k]].term = TERM_JMP;
        f->blk[rets[k]].arg = 0;
        f->blk[rets[k]].succ[0] = cont;
        ir_add_pred(f, cont, rets[k]);
    }
    if (ty != IT_VOID) {
        if (nrets == 1) {
            result = retvals[0];
        } else if (nrets > 1) {
            result = ir_phi(f, cont, ty);
            for (k = 0; k < nrets; k++)
                f->xargs[f->ins[result].a + k] = retvals[k];
        } else {
            result = ty == IT_F32 || ty == IT_F64 ? ir_fconst(f, ty, 0.0) : ir_const(f, ty, 0);
        }
        ir_replace(f, v, result);
    } else {
        ir_remove(f, v);
        f->ins[v].op = OP_NOP;
    }
    free(rets);
    free(vmap);
    return cont;
}

/* Inlines calls in fn. Returns the number of calls replaced. */
int inline_calls(Func *fn)
{
    u32 b, v, nx, nblk = fn->nblk;
    int done = 0;
    f = fn;
    for (b = 1; b < nblk; b++) {
        u32 cur = b;
        if (f->blk[b].term == TERM_NONE)
            continue;
        for (v = f->blk[cur].first; v; v = nx) {
            Ins *i = &f->ins[v], *c;
            int ok = 1;
            nx = i->next;
            if (i->op != OP_CALL)
                continue;
            c = &f->ins[ir_resolve(f, i->c)];
            if (c->op != OP_ADDR || c->b != 0 || c->a >= nbodies || !bodies[c->a] || c->a == f->sym)
                continue;
            y = bodies[c->a];
            if (i->ty != y->ret_ty || (f->nins > MAX_CALLER_INS && !y->always))
                continue;
            {
                u32 pk, ak = 0;
                for (pk = 0; pk < y->nparams && ok; pk++) {
                    u32 a;
                    if (y->param_ty[pk] & 0x40)
                        continue; /* the rest of a block of memory */
                    if (ak == i->b) {
                        ok = 0;
                        break;
                    }
                    a = ir_resolve(f, f->xargs[i->a + ak]);
                    f->xargs[i->a + ak] = a;
                    if (y->param_ty[pk] & 0x80) {
                        /* a block of memory: in device code, where the copy can become values */
                        if (!opt_dev_fn || f->ins[a].op != OP_BYVAL || !f->ins[a].blk)
                            ok = 0;
                    } else if (f->ins[a].ty != y->param_ty[pk] || f->ins[a].op == OP_BYVAL) {
                        ok = 0;
                    }
                    ak++;
                }
                if (ak != i->b)
                    ok = 0;
            }
            if (!ok)
                continue;
            if (!opt_inline_once && !y->always) {
                /* what the profile says about this call decides how much may be copied here */
                u32 fr = f->blk[cur].freq;
                u32 lim = fr == 1 ? 3 : fr && fr - 1 >= (prof_max >> opt_hot_shift) && !opt_size ? (u32)opt_inline_hot : (u32)LIMIT;
                if (y->size > lim + y->nparams)
                    continue;
            }
            cur = inline_one(cur, v);
            nx = f->blk[cur].first;
            done++;
        }
    }
    return done;
}

static void stash_reset(void)
{
    u32 k;
    for (k = 0; k < nstash; k++)
        if (stash[k].y)
            body_free(stash[k].y);
    free(stash);
    stash = 0;
    nstash = 0;
}
