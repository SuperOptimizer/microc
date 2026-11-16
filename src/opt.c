/* The optimisation pipeline: value numbering with folding and memory
   forwarding, and control-flow simplification. Every pass is near linear. */
#include "ir.h"
#include <stdlib.h>
#include <string.h>

static Func *f;

/* ---- CFG orderings and dominators ---- */

static u32 *rpo, nrpo;  /* blocks in reverse postorder */
static u32 *rpo_num;    /* block -> index in rpo, or ~0 if unreachable */
static u32 *idom;       /* immediate dominator */
static u32 *dom_pre, *dom_post; /* dominator tree intervals */
static u32 cap_blocks;

static u32 nsuccs(const Block *b)
{
    return b->term == TERM_BR ? 2 : b->term == TERM_JMP ? 1 : 0;
}

static void compute_rpo(void)
{
    u32 *stack, sp = 0, b, k, n = 0;
    if (cap_blocks < (f->nblk + 1) * 2) { /* room for a preheader per loop */
        cap_blocks = (f->nblk + 1) * 3;
        rpo = (u32 *)xrealloc(rpo, cap_blocks * sizeof(u32));
        rpo_num = (u32 *)xrealloc(rpo_num, cap_blocks * sizeof(u32));
        idom = (u32 *)xrealloc(idom, cap_blocks * sizeof(u32));
        dom_pre = (u32 *)xrealloc(dom_pre, cap_blocks * sizeof(u32));
        dom_post = (u32 *)xrealloc(dom_post, cap_blocks * sizeof(u32));
    }
    stack = (u32 *)xmalloc(f->nblk * 2 * sizeof(u32));
    for (b = 0; b < f->nblk; b++)
        rpo_num[b] = 0xffffffffU;
    stack[sp++] = 1;
    stack[sp++] = 0;
    rpo_num[1] = 0;
    while (sp) {
        Block *bb = &f->blk[stack[sp - 2]];
        k = stack[sp - 1];
        if (k < nsuccs(bb)) {
            u32 s = bb->succ[nsuccs(bb) - 1 - k];
            stack[sp - 1] = k + 1;
            if (rpo_num[s] == 0xffffffffU) {
                rpo_num[s] = 0;
                stack[sp++] = s;
                stack[sp++] = 0;
            }
        } else {
            rpo[n++] = stack[sp - 2];
            sp -= 2;
        }
    }
    nrpo = n;
    for (k = 0; k < n / 2; k++) {
        u32 t = rpo[k];
        rpo[k] = rpo[n - 1 - k];
        rpo[n - 1 - k] = t;
    }
    for (k = 0; k < n; k++)
        rpo_num[rpo[k]] = k;
    free(stack);
}

/* Cooper, Harvey, Kennedy: "A simple, fast dominance algorithm". */
static void compute_dom(void)
{
    u32 k, p, *child, *sibling, *stack, sp = 0, clock = 0;
    int changed;
    compute_rpo();
    for (k = 0; k < f->nblk; k++)
        idom[k] = 0;
    idom[1] = 1;
    do {
        changed = 0;
        for (k = 1; k < nrpo; k++) {
            u32 b = rpo[k], best = 0;
            Block *bb = &f->blk[b];
            for (p = 0; p < bb->npreds; p++) {
                u32 q = bb->preds[p];
                if (rpo_num[q] == 0xffffffffU || !idom[q])
                    continue;
                if (!best) {
                    best = q;
                } else {
                    u32 x = q, y = best;
                    while (x != y) {
                        while (rpo_num[x] > rpo_num[y])
                            x = idom[x];
                        while (rpo_num[y] > rpo_num[x])
                            y = idom[y];
                    }
                    best = x;
                }
            }
            if (idom[b] != best) {
                idom[b] = best;
                changed = 1;
            }
        }
    } while (changed);
    /* number the dominator tree so that dominance is an interval test */
    child = (u32 *)xmalloc(f->nblk * 3 * sizeof(u32));
    sibling = child + f->nblk;
    stack = sibling + f->nblk;
    memset(child, 0, f->nblk * 2 * sizeof(u32));
    for (k = nrpo; k-- > 1;) {
        u32 b = rpo[k];
        sibling[b] = child[idom[b]];
        child[idom[b]] = b;
    }
    stack[sp++] = 1;
    dom_pre[1] = clock++;
    while (sp) {
        u32 b = stack[sp - 1], c = child[b];
        if (c) {
            child[b] = sibling[c];
            dom_pre[c] = clock++;
            stack[sp++] = c;
        } else {
            dom_post[b] = clock++;
            sp--;
        }
    }
    free(child);
}

static int dominates(u32 a, u32 b)
{
    return dom_pre[a] <= dom_pre[b] && dom_post[b] <= dom_post[a];
}

/* For the passes of opt_ssa.c: fn's dominator tree, which opt_dominates reads until the blocks change. A block
   that cannot be reached is dominated by every other (it never runs). */
void opt_dom(Func *fn)
{
    f = fn;
    compute_dom();
}

int opt_dominates(u32 a, u32 b)
{
    return rpo_num[b] == 0xffffffffU || (rpo_num[a] != 0xffffffffU && dominates(a, b));
}

/* ---- value numbering, folding, memory forwarding ---- */

static u32 *vn_tab;
static u32 vn_size;

typedef struct MemEntry {
    u32 addr, val;
    u32 join; /* nonzero: the ways into block join left different values here; val is not set (see mem_phi) */
    u8 mem;
    u8 from_store;
} MemEntry;

#define MEM_CAP 32
static MemEntry mem_tab[MEM_CAP];
static u32 nmem;

static int is_pure(int op)
{
    return ((op >= OP_ADD && op <= OP_ASMOUT) || op == OP_VSPLAT) && op != OP_SDIV && op != OP_UDIV && op != OP_SREM &&
           op != OP_UREM;
}

static u32 mem_size(int mem)
{
    return M_SIZE(mem);
}

/* Splits an address into a base value and a constant offset. */
static u32 addr_base(u32 a, i64 *off)
{
    Ins *i = &f->ins[a];
    u64 c;
    *off = 0;
    if (i->op == OP_ADD && i->ty == IT_I64) {
        if (ir_is_const(f, i->b, &c)) {
            *off = (i64)c;
            return i->a;
        }
        if (ir_is_const(f, i->a, &c)) {
            *off = (i64)c;
            return i->b;
        }
    }
    return a;
}

int opt_strict_aliasing = 1;

/* Accesses of different classes are to objects of different types. */
static int mem_class(int mem)
{
    switch (mem) {
    case M_16S: case M_16U: return 1;
    case M_32: return 2;
    case M_64: return 3;
    case M_F32: return 4;
    case M_F64: return 5;
    }
    return 0; /* character access: may touch anything */
}

/* May an access of kind ma at address a overlap one of kind mb at b? */
static int may_alias(u32 a, int ma, u32 b, int mb)
{
    Ins *ia = &f->ins[a], *ib = &f->ins[b];
    int fa = ia->op == OP_SLOT || ia->op == OP_ADDR, fb = ib->op == OP_SLOT || ib->op == OP_ADDR;
    u32 sa = mem_size(ma), sb = mem_size(mb);
    i64 oa, ob;
    if (a == b)
        return 1;
    if (fa && fb) {
        oa = (i32)ia->b;
        ob = (i32)ib->b;
        if (ia->op != ib->op || ia->a != ib->a)
            return 0;
        return oa < ob + (i64)sb && ob < oa + (i64)sa;
    }
    if (!fa && !fb && addr_base(a, &oa) == addr_base(b, &ob))
        return oa < ob + (i64)sb && ob < oa + (i64)sa;
    /* unrelated addresses: the language says objects are accessed through
       their own type (or as characters) */
    if (opt_strict_aliasing && mem_class(ma) != mem_class(mb) && mem_class(ma) && mem_class(mb))
        return 0;
    return 1;
}

static void mem_kill(u32 addr, int mem)
{
    u32 k, n = 0;
    for (k = 0; k < nmem; k++)
        if (!may_alias(mem_tab[k].addr, mem_tab[k].mem, addr, mem))
            mem_tab[n++] = mem_tab[k];
    nmem = n;
}

static void mem_add(u32 addr, int mem, u32 val, int from_store)
{
    MemEntry *e;
    if (nmem == MEM_CAP) {
        memmove(mem_tab, mem_tab + 1, (MEM_CAP - 1) * sizeof(MemEntry));
        nmem--;
    }
    e = &mem_tab[nmem++];
    e->addr = addr;
    e->val = val;
    e->join = 0;
    e->mem = (u8)mem;
    e->from_store = (u8)from_store;
}

/* What was known about memory at the end of each block already visited, by position in rpo: a block
   entered one way starts from what its predecessor knew, a block entered several ways from what all of
   them agree on. */
static MemEntry *out_tab;
static u32 *out_n;
static u32 out_cap;

/* Phis made by mem_phi, so that one is made once though many blocks carry the entry that asks for it. */
#define NMEMPHI 8
static struct {
    u32 join, addr, phi;
    u8 mem;
} mem_phis[NMEMPHI];
static u32 nmem_phis;

static MemEntry *out_find(u32 blk, u32 addr, int mem)
{
    MemEntry *t = out_tab + (size_t)rpo_num[blk] * MEM_CAP;
    u32 j;
    for (j = out_n[rpo_num[blk]]; j-- > 0;)
        if (t[j].addr == addr && t[j].mem == mem)
            return &t[j];
    return 0;
}

/* The state of memory on entry to block b, the k-th in rpo. */
static void mem_enter(u32 b, u32 k)
{
    Block *bb = &f->blk[b];
    MemEntry *t;
    u32 p, j, q;
    nmem = 0;
    if (!bb->npreds)
        return;
    for (p = 0; p < bb->npreds; p++)
        if (rpo_num[bb->preds[p]] >= k)
            return; /* a way in not yet visited (a loop), or one that cannot be reached */
    q = rpo_num[bb->preds[0]];
    t = out_tab + (size_t)q * MEM_CAP;
    if (bb->npreds == 1) {
        nmem = out_n[q];
        memcpy(mem_tab, t, nmem * sizeof(MemEntry));
        return;
    }
    for (j = 0; j < out_n[q]; j++) {
        MemEntry e = t[j];
        int differ = 0;
        for (p = 1; p < bb->npreds; p++) {
            MemEntry *o = out_find(bb->preds[p], e.addr, e.mem);
            if (!o)
                break;
            if (o->val != e.val || o->join != e.join) {
                /* a phi of the values can stand for what is there, if they are plain values of one type
                   (a narrow stored value is not what a load would give) */
                if (o->join || e.join || f->ins[o->val].ty != f->ins[e.val].ty)
                    break;
                differ = 1;
            }
            e.from_store |= o->from_store;
        }
        if (p < bb->npreds || (differ && e.from_store && e.mem < M_32))
            continue;
        if (differ) {
            e.join = b;
            e.val = 0;
        }
        mem_tab[nmem++] = e;
    }
}

/* The value of an entry whose block had different values coming in: a phi there, made when first asked
   for. Returns 0 if it is not of type ty. */
static u32 mem_phi(const MemEntry *e, int ty)
{
    Block *jb = &f->blk[e->join];
    u32 j, p, phi;
    for (j = 0; j < NMEMPHI && j < nmem_phis; j++)
        if (mem_phis[j].join == e->join && mem_phis[j].addr == e->addr && mem_phis[j].mem == e->mem)
            return f->ins[mem_phis[j].phi].ty == ty ? mem_phis[j].phi : 0;
    if (f->ins[out_find(jb->preds[0], e->addr, e->mem)->val].ty != ty)
        return 0;
    phi = ir_phi(f, e->join, ty);
    for (p = 0; p < jb->npreds; p++)
        f->xargs[f->ins[phi].a + p] = out_find(jb->preds[p], e->addr, e->mem)->val;
    j = nmem_phis++ % NMEMPHI;
    mem_phis[j].join = e->join;
    mem_phis[j].addr = e->addr;
    mem_phis[j].mem = e->mem;
    mem_phis[j].phi = phi;
    return phi;
}

/* What a branch already taken says about a value: in block blk and
   everything it dominates, x is known to be zero or known not to be. */
#define NFACTS 16
static struct {
    u32 x, blk;
    int nonzero;
} facts[NFACTS];
static u32 nfacts;

/* Reduces a condition to "x != 0" (returns 1) or "x == 0" (returns 0). */
static int cond_base(u32 c, u32 *x)
{
    int sense = 1;
    for (;;) {
        Ins *i = &f->ins[c];
        u64 z;
        if ((i->op != OP_EQ && i->op != OP_NE) || !ir_is_const(f, i->b, &z) || z != 0)
            break;
        if (i->op == OP_EQ)
            sense = !sense;
        c = i->a;
    }
    *x = c;
    return sense;
}

/* Device code: the same pure value worked out in two blocks neither of which comes before the other (the arms of
   per-channel conditions unrolled 48 times: conv_bwd_w_f8_k<1,6,...> loaded each GroupNorm parameter 96 times) is
   worked out once, at the end of the nearest block that comes before both, if what it reads is known there and
   that block is in no fewer loops than the two (a loop's head between them: the value stays where it is; moving
   things out of loops costs registers, see licm). MCC_NOHOIST: off. */
static u8 *hdr; /* device code: blocks a way back leads to */
static u32 hoist_depth = 4; /* (dominator-tree steps up from either block: 4 against 2, 8, any on the 161 kernels: 8 and more spill, MCC_HOISTD=N) */

static u32 hoist_to(u32 o, u32 a, u32 b)
{
    Ins *oi = &f->ins[o];
    u32 l = a, x, n, k, *p;
    if (oi->op == OP_ASMOUT || oi->op == OP_TLS || oi->op == OP_ISCONST || oi->op == OP_PHI || oi->op == OP_COPY ||
        oi->op == OP_PARAM || oi->ty == IT_V128)
        return 0;
    if (rpo_num[a] == 0xffffffffU || rpo_num[b] == 0xffffffffU)
        return 0;
    while (!dominates(l, b))
        l = idom[l];
    for (x = a, k = 0; x != l; x = idom[x], k++)
        if (hdr[x] || k >= hoist_depth)
            return 0;
    for (x = b, k = 0; x != l; x = idom[x], k++)
        if (hdr[x] || k >= hoist_depth)
            return 0;
    IR_OPS(f, oi, p, n);
    for (k = 0; k < n; k++) {
        Ins *q = &f->ins[p[k]];
        if (q->blk && !dominates(q->blk, l))
            return 0;
    }
    return l;
}

/* gvn: the pure calls seen (IF_PURE), the latest of each kind last */
#define PURE_CAP 64
static u32 pure_tab[PURE_CAP], npure;

static int pure_same(u32 x, u32 y)
{
    Ins *a = &f->ins[x], *b = &f->ins[y];
    u32 k;
    if (a->c != b->c || a->b != b->b || a->ty != b->ty || !b->blk)
        return 0;
    for (k = 0; k < a->b; k++)
        if (ir_resolve(f, f->xargs[a->a + k]) != ir_resolve(f, f->xargs[b->a + k]))
            return 0;
    return 1;
}

static u32 pure_find(u32 v)
{
    u32 k;
    for (k = npure; k-- > 0;)
        if (pure_same(v, pure_tab[k]))
            return pure_tab[k];
    return 0;
}

static void pure_add(u32 v)
{
    if (npure == PURE_CAP) {
        memmove(pure_tab, pure_tab + 1, (PURE_CAP - 1) * sizeof(u32));
        npure--;
    }
    pure_tab[npure++] = v;
}

static void gvn(void)
{
    u32 k, v, nx, n, j, mask;
    compute_dom();
    hdr = 0;
    if (getenv("MCC_HOISTD"))
        hoist_depth = (u32)atoi(getenv("MCC_HOISTD"));
    if (opt_dev_fn && !opt_nohoist) {
        hdr = (u8 *)xmalloc(f->nblk + 1);
        memset(hdr, 0, f->nblk + 1);
        for (k = 0; k < nrpo; k++) {
            Block *bb = &f->blk[rpo[k]];
            for (j = 0; j < bb->npreds; j++)
                if (rpo_num[bb->preds[j]] != 0xffffffffU && dominates(rpo[k], bb->preds[j]))
                    hdr[rpo[k]] = 1;
        }
    }
    nfacts = 0;
    nmem_phis = 0;
    if (out_cap < nrpo) {
        out_cap = nrpo * 2;
        out_tab = (MemEntry *)xrealloc(out_tab, (size_t)out_cap * MEM_CAP * sizeof(MemEntry));
        out_n = (u32 *)xrealloc(out_n, out_cap * sizeof(u32));
    }
    n = 64;
    while (n < f->nins * 2)
        n *= 2;
    if (n > vn_size) {
        vn_size = n;
        vn_tab = (u32 *)xrealloc(vn_tab, vn_size * sizeof(u32));
    }
    memset(vn_tab, 0, n * sizeof(u32));
    mask = n - 1; /* only this much of the table is in use for this function */
    npure = 0;

    for (k = 0; k < nrpo; k++) {
        u32 b = rpo[k];
        Block *bb = &f->blk[b];
        u32 *link;
        /* what is known about memory survives into a block entered only
           from the block just processed */
        if (bb->npreds == 1) {
            Block *pb = &f->blk[bb->preds[0]];
            if (pb->term == TERM_BR && pb->succ[0] != pb->succ[1]) {
                u32 x;
                int sense = cond_base(pb->arg, &x);
                j = nfacts++ % NFACTS;
                facts[j].x = x;
                facts[j].blk = b;
                facts[j].nonzero = pb->succ[0] == b ? sense : !sense;
            }
        }
        if (!(k > 0 && bb->npreds == 1 && bb->preds[0] == rpo[k - 1]))
            mem_enter(b, k);
        /* phis whose arguments all agree are copies */
        link = &bb->phis;
        while ((v = *link) != 0) {
            Ins *i = &f->ins[v];
            u32 same = 0;
            for (j = 0; j < i->b; j++) {
                u32 a = ir_resolve(f, f->xargs[i->a + j]);
                f->xargs[i->a + j] = a;
                if (a == v || a == same)
                    continue;
                if (same) {
                    same = 0xffffffffU;
                    break;
                }
                same = a;
            }
            if (same && same != 0xffffffffU) {
                *link = i->next;
                i->op = OP_COPY;
                f->copies = 1;
                i->a = same;
                i->blk = 0;
                continue;
            }
            link = &i->next;
        }
        for (v = bb->first; v; v = nx) {
            Ins *i = &f->ins[v];
            u32 r, h, n0;
            nx = i->next;
            {
                u32 *ops;
                IR_OPS(f, i, ops, n);
                if (i->op == OP_CALL)
                    i->c = ir_resolve(f, i->c);
                for (j = 0; j < n; j++)
                    while (f->ins[ops[j]].op == OP_COPY)
                        ops[j] = f->ins[ops[j]].a;
            }
            switch (i->op) {
            case OP_LOAD:
                for (j = nmem; j-- > 0;) {
                    MemEntry *e = &mem_tab[j];
                    if (e->addr != i->a)
                        continue;
                    if (e->mem != i->mem)
                        break;
                    if (e->join) {
                        u32 phi = mem_phi(e, i->ty);
                        if (!phi)
                            break;
                        i = &f->ins[v]; /* the instruction array may have moved */
                        e->val = phi;
                        e->join = 0;
                    }
                    if ((!e->from_store || i->mem >= M_32) && f->ins[e->val].ty == i->ty) {
                        ir_replace(f, v, e->val);
                        v = 0;
                    }
                    break;
                }
                if (v)
                    mem_add(i->a, i->mem, v, 0);
                continue;
            case OP_STORE:
                mem_kill(i->a, i->mem);
                mem_add(i->a, i->mem, i->b, 1);
                continue;
            case OP_CALL:
                if (i->flags & IF_PURE) {
                    /* a math function of its arguments: memory is as it was, and an earlier call alike that
                       dominates this one gave the same value */
                    u32 o = pure_find(v);
                    if (o && dominates(f->ins[o].blk, b))
                        ir_replace(f, v, o);
                    else
                        pure_add(v);
                    continue;
                }
                nmem = 0;
                continue;
            case OP_MEMCPY:
            case OP_MEMZERO:
            case OP_VSTORE:
                nmem = 0;
                continue;
            case OP_XI:
                if (i->flags & XI_EFFECT)
                    nmem = 0;
                continue;
            case OP_SELECT: {
                u64 cv;
                if (ir_is_const(f, i->a, &cv)) {
                    ir_replace(f, v, cv ? i->b : i->c);
                } else if (i->b == i->c) {
                    ir_replace(f, v, i->b);
                } else if (i->ty == IT_I32 && ir_is_bool(f, i->a, 3)) {
                    /* truth values: c ? 1 : x is c | x, and c ? x : 0 is c & x */
                    if (ir_is_const(f, i->b, &cv) && cv == 1 && ir_is_bool(f, i->c, 3)) {
                        i->op = OP_OR;
                        i->b = i->c;
                        i->c = 0;
                        break;
                    }
                    if (ir_is_const(f, i->c, &cv) && cv == 0 && ir_is_bool(f, i->b, 3)) {
                        i->op = OP_AND;
                        i->c = 0;
                        break;
                    }
                }
                if (opt_dev_fn && !opt_noselk && f->ins[v].op == OP_SELECT && (i->ty == IT_I32 || i->ty == IT_I64)) {
                    /* device code: c ? x + j : x + k is (x + (c ? j - k : 0)) + k, where the choice is one that
                       others like it share and the constant goes into an address */
                    Ins *ab = &f->ins[i->b], *ac = &f->ins[i->c];
                    u32 xb = i->b, xc = i->c;
                    u64 kb = 0, kc = 0;
                    if (ab->op == OP_ADD && ir_is_const(f, ab->b, &kb))
                        xb = ab->a;
                    if (ac->op == OP_ADD && ir_is_const(f, ac->b, &kc))
                        xc = ac->a;
                    if (xb == xc && kb != kc) {
                        int ty = i->ty;
                        u32 sel = ir_raw(f, OP_SELECT, ty, i->a, ir_const(f, ty, kb - kc)), sum;
                        f->ins[sel].c = ir_const(f, ty, 0);
                        ir_insert_before(f, v, sel);
                        sum = ir_raw(f, OP_ADD, ty, xb, sel);
                        ir_insert_before(f, v, sum);
                        i = &f->ins[v];
                        if (kc) {
                            i->op = OP_ADD;
                            i->a = sum;
                            i->b = ir_const(f, ty, kc);
                            i->c = 0;
                        } else {
                            ir_replace(f, v, sum);
                        }
                        continue;
                    }
                    goto number;
                }
                continue;
            }
            }
            if (i->op == OP_VSPLAT)
                goto number;
            if (!(i->op >= OP_ADD && i->op <= OP_ASMOUT))
                continue;
            n0 = f->nins;
            r = ir_fold_at(f, v);
            i = &f->ins[v]; /* the instruction array may have moved */
            if (r) {
                u32 k;
                ir_replace(f, v, r);
                /* what the folder made stands where v stood and is numbered next: it may be a value that
                   there is already */
                for (k = n0; k < f->nins; k++)
                    if (f->ins[k].blk == b && f->ins[k].op >= OP_ADD && f->ins[k].op <= OP_ASMOUT) {
                        nx = k;
                        break;
                    }
                continue;
            }
            /* device code: an integer division never traps on a GPU (by zero it gives some value), so equal ones are one
               (conv_bwd_w_f8_k<1,6,...>: the GroupNorm slab row, a division by a parameter, done for each of 48
               channels and 2 planes). MCC_NODIVCSE: as on the host */
            if (!is_pure(i->op) && !(opt_dev_fn && !opt_nodivcse && i->op >= OP_SDIV && i->op <= OP_UREM))
                continue;
        number:
            i = &f->ins[v];
            h = ((i->a + (i->op == OP_SELECT ? i->c * 31U : 0)) * 2654435761U ^ i->b * 40503U ^ ((u32)i->op << 8 | i->ty)) & mask;
            for (;;) {
                u32 o = vn_tab[h];
                Ins *oi = &f->ins[o];
                if (!o) {
                    vn_tab[h] = v;
                    break;
                }
                if (oi->op == i->op && oi->ty == i->ty && oi->a == i->a && oi->b == i->b &&
                    (i->op != OP_SELECT || oi->c == i->c) && oi->mem == i->mem) { /* (vectors: mem = lane kind) */
                    u32 l;
                    if (oi->blk && dominates(oi->blk, b))
                        ir_replace(f, v, o);
                    else if (hdr && oi->blk && (l = hoist_to(o, oi->blk, b)) != 0) {
                        ir_remove(f, o);
                        ir_append(f, l, o);
                        ir_replace(f, v, o);
                        oi = &f->ins[o];
                    } else
                        vn_tab[h] = v; /* the older one is out of scope here */
                    break;
                }
                h = (h + 1) & mask;
            }
        }
        if (bb->arg)
            bb->arg = ir_resolve(f, bb->arg);
        if (bb->term == TERM_BR && nfacts) {
            /* a test whose outcome an earlier branch settled */
            u32 x;
            int sense = cond_base(bb->arg, &x);
            for (j = 0; j < NFACTS && j < nfacts; j++)
                if (facts[j].x == x && dominates(facts[j].blk, b)) {
                    bb->arg = ir_const(f, IT_I32, (u64)(facts[j].nonzero == sense));
                    break;
                }
        }
        out_n[k] = nmem;
        memcpy(out_tab + (size_t)k * MEM_CAP, mem_tab, nmem * sizeof(MemEntry));
    }
    /* Every block was visited after the blocks that dominate it, and its
       operands were resolved then; only phi operands can still name a value
       that was replaced later. Blocks that were not visited at all (dead
       ones that still hold code) need the full pass. */
    for (k = 1; k < f->nblk; k++)
        if (rpo_num[k] == 0xffffffffU && (f->blk[k].first || f->blk[k].phis || f->blk[k].arg))
            break;
    if (k < f->nblk) {
        ir_resolve_all(f);
    } else {
        for (k = 0; k < nrpo; k++)
            for (v = f->blk[rpo[k]].phis; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                for (j = 0; j < i->b; j++)
                    f->xargs[i->a + j] = ir_resolve(f, f->xargs[i->a + j]);
            }
        f->copies = 0;
    }
    free(hdr);
    hdr = 0;
}

/* ---- control-flow simplification ---- */

static void unlink_phi(u32 blk, u32 phi, u32 with)
{
    u32 *link = &f->blk[blk].phis;
    while (*link != phi)
        link = &f->ins[*link].next;
    *link = f->ins[phi].next;
    /* a phi left depending only on itself is in a loop that can no longer
       be entered: any value will do */
    if (ir_resolve(f, with) == phi) {
        int ty = f->ins[phi].ty;
        with = ty == IT_F32 || ty == IT_F64 ? ir_fconst(f, ty, 0.0) : ir_const(f, ty, 0);
    }
    f->ins[phi].op = OP_COPY;
    f->copies = 1;
    f->ins[phi].a = with;
    f->ins[phi].blk = 0;
}

/* Removes predecessor number k of block b, with its phi arguments. */
static void remove_pred(u32 b, u32 k)
{
    Block *bb = &f->blk[b];
    u32 v, j, nx;
    for (v = bb->phis; v; v = nx) {
        Ins *i = &f->ins[v];
        nx = i->next;
        for (j = k; j + 1 < i->b; j++)
            f->xargs[i->a + j] = f->xargs[i->a + j + 1];
        i->b--;
        if (i->b == 1)
            unlink_phi(b, v, ir_resolve(f, f->xargs[i->a]));
    }
    for (j = k; j + 1 < bb->npreds; j++)
        bb->preds[j] = bb->preds[j + 1];
    bb->npreds--;
}

static u32 pred_index(u32 b, u32 p)
{
    Block *bb = &f->blk[b];
    u32 k;
    for (k = 0; k < bb->npreds; k++)
        if (bb->preds[k] == p)
            return k;
    return 0xffffffffU;
}

/* Removes the edge from..to, deleting whatever becomes unreachable. */
static void remove_edge(u32 from, u32 to)
{
    u32 k = pred_index(to, from);
    Block *tb = &f->blk[to];
    if (k == 0xffffffffU)
        return;
    remove_pred(to, k);
    if (tb->npreds == 0 && to != 1) {
        u32 n = nsuccs(tb), s0 = tb->succ[0], s1 = tb->succ[1], v, nx;
        for (v = tb->first; v; v = nx) {
            nx = f->ins[v].next;
            f->ins[v].blk = 0;
            f->ins[v].prev = f->ins[v].next = 0;
        }
        tb->first = tb->last = 0;
        tb->term = TERM_NONE;
        tb->arg = 0;
        if (n >= 1)
            remove_edge(to, s0);
        if (n == 2)
            remove_edge(to, s1);
    }
}

/* Device code: is the assembly statement of the call v arithmetic on registers alone (a conversion, an
   approximated function, a shift), which can be done where its result is not wanted? */
static int asm_arith(u32 v)
{
    static const char *const ok[] = {"tanh.", "ex2.", "lg2.", "rcp.", "sqrt.", "rsqrt.", "sin.", "cos.", "fma.", "cvt.",
                                     "min.", "max.", "abs.", "neg.", "popc.", "clz.", "bfind.", "brev.", "shf.",
                                     "mul.", "mad.", "add.", "sub.", "and.", "or.", "xor.", "shl.", "shr.", "mov.",
                                     "selp.", "setp.", "prmt.", ".reg "};
    u64 idx;
    const char *t;
    u32 k;
    if (!ir_is_const(f, f->ins[v].c, &idx) || !asm_stmts[idx].dev)
        return 0;
    t = asm_stmts[idx].tmpl;
    if (strchr(t, '['))
        return 0; /* memory */
    for (;;) {
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '{' || *t == '}' || *t == ';')
            t++;
        if (!*t)
            return 1;
        for (k = 0; k < sizeof ok / sizeof ok[0] && strncmp(t, ok[k], strlen(ok[k])); k++)
            ;
        if (k == sizeof ok / sizeof ok[0])
            return 0;
        while (*t && *t != ';')
            t++;
    }
}

/* one MUFU instruction (tanh.approx and the like: ufsm act_ab's SiLU behind its "if (!G)"): taken at the default MCC_IFC; the
   rest of asm_arith only above it (MCC_IFC 12 / 24 cost spills in lp_s2) */
static int asm_mufu(u32 v)
{
    static const char *const ok[] = {"tanh.", "ex2.", "lg2.", "rcp.", "sqrt.", "rsqrt.", "sin.", "cos."};
    u64 idx;
    const char *t;
    u32 k;
    if (!ir_is_const(f, f->ins[v].c, &idx) || !asm_stmts[idx].dev)
        return 0;
    for (t = asm_stmts[idx].tmpl; *t == ' ' || *t == '\t' || *t == '\n'; t++)
        ;
    for (k = 0; k < sizeof ok / sizeof ok[0]; k++)
        if (!strncmp(t, ok[k], strlen(ok[k])))
            return !strchr(strchr(t, ';') ? strchr(t, ';') + 1 : t + strlen(t), ';') && !strchr(t, '{');
    return 0;
}

/* Is block b empty, or one or two instructions that are safe and cheap to
   execute unconditionally? */
static int cheap_block(u32 b)
{
    u32 v, n = 0;
    for (v = f->blk[b].first; v; v = f->ins[v].next) {
        int op = f->ins[v].op;
        if (opt_dev_fn && !getenv("MCC_NOIFC")) {
            /* device code: a branch costs more than a few instructions done in vain, and one that the threads
               of a warp take differently makes them take turns (MCC_IFC=n: how many instructions) */
            if (op == OP_ASMOUT)
                continue;
            if (op == OP_CALL && (f->ins[v].flags & IF_ASM) && (opt_ifc > 6 || (!opt_noifasm && asm_mufu(v))) && asm_arith(v)) {
                if ((n += 2) > (u32)opt_ifc)
                    return 0;
                continue;
            }
            if (op == OP_CALL && !opt_noifcall && ptx_pure_call(f, v)) {
                if (++n > (u32)opt_ifc)
                    return 0;
                continue;
            }
            if (op >= OP_SDIV && op <= OP_UREM && !opt_nodivcse) {
                /* (an integer division does not trap on a GPU: done in vain it costs time only, about 4 instructions
                   by a constant, a dozen by a value; gn_row's slab row behind its "if (!gp.ns)") */
                if ((n += 4) > (u32)opt_ifc)
                    return 0;
                continue;
            }
            if ((!is_pure(op) && (op != OP_SELECT || opt_noifcall)) || op == OP_FDIV || ++n > (u32)opt_ifc)
                return 0;
            continue;
        }
        /* -O3: a little more, so that the vectoriser finds the loop's choices as selects (clamps and the like) */
        if (opt_level >= 3 && !opt_size) {
            if (!(is_pure(op) || op == OP_SELECT) || op == OP_FDIV || op == OP_SDIV || op == OP_UDIV || op == OP_SREM || op == OP_UREM || ++n > 4)
                return 0;
            continue;
        }
        if (!is_pure(op) || op == OP_MUL || (op >= OP_FADD && op <= OP_FDIV) || ++n > 2)
            return 0;
    }
    return 1;
}

/* -O3: the two arms of the branch at the end of b (each reached only from b) start with the same computation, as
   (c ? a[i] : 1 - a[i]) reads a[i] in both: it moves up into b, once. What moves is an instruction at the start of
   the first arm that the second computes from the same values before it stores or calls anything, so both paths
   did it at that point anyway (a load included). The arms then are often cheap enough to become selects. */
static int hoist_arms(u32 b)
{
    u32 t = f->blk[b].succ[0], e = f->blk[b].succ[1], x, y, k, n = 0;
    if (t == e || t == b || e == b || f->blk[t].npreds != 1 || f->blk[e].npreds != 1 || f->blk[t].phis || f->blk[e].phis)
        return 0;
    while ((x = f->blk[t].first) != 0 && n < 16) {
        Ins *xi = &f->ins[x];
        u32 nx = ir_opcount(f, x);
        if (!(is_pure(xi->op) || xi->op == OP_SELECT || xi->op == OP_LOAD) || nx > 3)
            break;
        for (y = f->blk[e].first; y; y = f->ins[y].next) {
            Ins *yi = &f->ins[y];
            if (yi->op == OP_STORE || yi->op == OP_CALL || yi->op == OP_XI || !(is_pure(yi->op) || yi->op == OP_SELECT || yi->op == OP_LOAD))
                break; /* (a load past this may see another value) */
            if (yi->op != xi->op || yi->ty != xi->ty || yi->mem != xi->mem || yi->flags != xi->flags)
                continue;
            for (k = 0; k < nx && ir_resolve(f, *ir_opref(f, x, k)) == ir_resolve(f, *ir_opref(f, y, k)); k++)
                ;
            if (k == nx)
                break;
        }
        if (!y || f->ins[y].op != xi->op)
            break;
        ir_remove(f, x);
        ir_append(f, b, x);
        ir_replace(f, y, x);
        n++;
    }
    return n != 0;
}

/* Is what block b computes before its branch little, and safe to compute when the branch is not reached? */
static int chain_block(u32 b)
{
    u32 v, n = 0;
    for (v = f->blk[b].first; v; v = f->ins[v].next) {
        int op = f->ins[v].op;
        if (!is_pure(op) || op == OP_FDIV || ++n > 4)
            return 0;
    }
    return 1;
}

/* A block with nothing but phis and a jump to a block that merges again:
   its predecessors can go there directly. This needs the block not to
   dominate its successor, for then its phis have no use but the successor's
   phis. The dominator tree must be current (taking such a block out does
   not change who dominates whom among the rest). */
static int forward_phi_blocks(void)
{
    u32 b, v, k;
    int changed = 0, again;
    do {
        again = 0;
        for (b = 2; b < f->nblk && b < cap_blocks; b++) {
            Block *bb = &f->blk[b];
            u32 s = bb->succ[0], bi, j, q;
            int ok = 1;
            if (bb->term != TERM_JMP || bb->first || !bb->phis || s == b || rpo_num[b] == 0xffffffffU ||
                s >= cap_blocks || rpo_num[s] == 0xffffffffU || f->blk[s].npreds < 2 || !bb->npreds || dominates(b, s))
                continue;
            for (k = 0; k < bb->npreds && ok; k++) {
                q = bb->preds[k];
                if (q == b || q == s || pred_index(s, q) != 0xffffffffU)
                    ok = 0;
                for (j = 0; j < k; j++)
                    if (bb->preds[j] == q)
                        ok = 0;
            }
            if (!ok)
                continue;
            bi = pred_index(s, b);
            for (k = 0; k < bb->npreds; k++) {
                q = bb->preds[k];
                for (v = f->blk[s].phis; v; v = f->ins[v].next) {
                    u32 val = ir_resolve(f, f->xargs[f->ins[v].a + bi]);
                    if (f->ins[val].op == OP_PHI && f->ins[val].blk == b)
                        val = ir_resolve(f, f->xargs[f->ins[val].a + k]);
                    ir_phi_add_arg(f, v, val);
                }
                for (j = 0; j < 2; j++)
                    if (f->blk[q].succ[j] == b && (j == 0 || f->blk[q].term == TERM_BR))
                        f->blk[q].succ[j] = s;
                ir_add_pred(f, s, q);
            }
            remove_pred(s, bi);
            bb = &f->blk[b];
            for (v = bb->phis; v; v = f->ins[v].next)
                f->ins[v].op = OP_NOP;
            bb->phis = 0;
            bb->term = TERM_NONE;
            bb->npreds = 0;
            again = changed = 1;
        }
    } while (again);
    return changed;
}

/* Jump threading through a block whose branch the incoming values decide. The test of an inlined function that
   returns a truth value (if (!get(&r, n, &v)) ...: the callee's returns meet in a phi of constants) is the case:
   each predecessor of block B knows which way B's branch goes, so it goes there directly, and B (phis, a few pure
   instructions, the branch) disappears. Values B defines are used only in B, or (its phis) where its successors
   dominate, which get them through a new phi of the predecessors that now come to them (or the one value when one
   does), or as the arguments of the successors' own phis on B's edge. Not for a loop's head. MCC_NOJT: off. */
static int opt_nojt = -1;

static int jt_eval(int op, int ty, u64 a, u64 b, u64 *r)
{
    u64 v;
    int w32 = ty == IT_I32;
    i64 sa = w32 ? (i64)(i32)a : (i64)a, sb = w32 ? (i64)(i32)b : (i64)b;
    u64 ua = w32 ? (u32)a : a, ub = w32 ? (u32)b : b;
    switch (op) {
    case OP_ADD: v = a + b; break;
    case OP_SUB: v = a - b; break;
    case OP_AND: v = a & b; break;
    case OP_OR: v = a | b; break;
    case OP_XOR: v = a ^ b; break;
    case OP_NOT: v = ~a; break;
    case OP_EQ: v = ua == ub; break;
    case OP_NE: v = ua != ub; break;
    case OP_SLT: v = sa < sb; break;
    case OP_SLE: v = sa <= sb; break;
    case OP_SGT: v = sa > sb; break;
    case OP_SGE: v = sa >= sb; break;
    case OP_ULT: v = ua < ub; break;
    case OP_ULE: v = ua <= ub; break;
    case OP_UGT: v = ua > ub; break;
    case OP_UGE: v = ua >= ub; break;
    case OP_ZEXT8: v = a & 0xff; break;
    case OP_ZEXT16: v = a & 0xffff; break;
    case OP_SEXT8: v = (u64)(i64)(signed char)a; break;
    case OP_SEXT16: v = (u64)(i64)(short)a; break;
    case OP_ZEXT32: v = (u32)a; break;
    case OP_SEXT32: v = (u64)(i64)(i32)a; break;
    case OP_TRUNC: v = a; break;
    default: return 0;
    }
    if (w32 || op == OP_TRUNC)
        v = (u64)(i64)(i32)v;
    *r = v;
    return 1;
}

/* the block a use is in: a phi's argument k is used at the end of predecessor k */
static int jt_used_ok(u32 x, u32 B, u32 S1, u32 S2, int s1d, int s2d, u32 *nin)
{
    u32 bi, v, k, n, *ops;
    for (bi = 1; bi < f->nblk; bi++) {
        Block *ub = &f->blk[bi];
        if (ub->term == TERM_NONE)
            continue;
        for (v = ub->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                if (ir_resolve(f, f->xargs[f->ins[v].a + k]) == x) {
                    u32 at = ub->preds[k];
                    if (at == B && (bi == S1 || bi == S2))
                        continue; /* through B's edge into a successor's phi */
                    if (rpo_num[at] == 0xffffffffU || at == B)
                        return 0;
                    if (!((s1d && dominates(S1, at)) || (s2d && dominates(S2, at))))
                        return 0;
                    ++*nin;
                }
        for (v = ub->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            int hit = 0;
            IR_OPS(f, i, ops, n);
            for (k = 0; k < n; k++)
                if (ir_resolve(f, ops[k]) == x)
                    hit = 1;
            if (i->op == OP_CALL && ir_resolve(f, i->c) == x)
                hit = 1;
            if (!hit)
                continue;
            if (bi == B)
                continue;
            if (rpo_num[bi] == 0xffffffffU || !((s1d && dominates(S1, bi)) || (s2d && dominates(S2, bi))))
                return 0;
            ++*nin;
        }
        if (ub->arg && ir_resolve(f, ub->arg) == x && bi != B) {
            if (rpo_num[bi] == 0xffffffffU || !((s1d && dominates(S1, bi)) || (s2d && dominates(S2, bi))))
                return 0;
            ++*nin;
        }
    }
    return 1;
}

/* replace x by r in the blocks S dominates (phi arguments: by the edge's predecessor) */
static void jt_rewrite(u32 x, u32 S, u32 r)
{
    u32 bi, v, k, n, *ops;
    for (bi = 1; bi < f->nblk; bi++) {
        Block *ub = &f->blk[bi];
        if (ub->term == TERM_NONE || rpo_num[bi] == 0xffffffffU)
            continue;
        for (v = ub->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                if (ir_resolve(f, f->xargs[f->ins[v].a + k]) == x && rpo_num[ub->preds[k]] != 0xffffffffU &&
                    dominates(S, ub->preds[k]))
                    f->xargs[f->ins[v].a + k] = r;
        if (!dominates(S, bi))
            continue;
        for (v = ub->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, ops, n);
            for (k = 0; k < n; k++)
                if (ir_resolve(f, ops[k]) == x)
                    ops[k] = r;
            if (i->op == OP_CALL && ir_resolve(f, i->c) == x)
                i->c = r;
        }
        if (ub->arg && ir_resolve(f, ub->arg) == x)
            ub->arg = r;
    }
}

#define JT_MAXP 16
#define JT_MAXI 8
static int jump_thread(void)
{
    u32 B, v, k, j;
    int changed = 0;
    if (opt_nojt < 0)
        opt_nojt = getenv("MCC_NOJT") != 0;
    if (opt_nojt || opt_dev_fn || opt_cuda)
        return 0;
    compute_dom();
    for (B = 2; B < f->nblk; B++) {
        Block *bb = &f->blk[B];
        u32 S[2], np = bb->npreds, to[JT_MAXP], body[JT_MAXI], nb = 0, phis[JT_MAXI * 2], nphi = 0, cnt[2] = {0, 0};
        u64 val[JT_MAXI];
        int ok = 1, sd[2];
        if (bb->term != TERM_BR || rpo_num[B] == 0xffffffffU || np < 2 || np > JT_MAXP || !bb->phis)
            continue;
        S[0] = bb->succ[0];
        S[1] = bb->succ[1];
        if (S[0] == S[1] || S[0] == B || S[1] == B || S[0] == 1 || S[1] == 1)
            continue;
        for (v = bb->first; v && ok; v = f->ins[v].next) {
            if (nb == JT_MAXI || !is_pure(f->ins[v].op) || (f->ins[v].ty != IT_I32 && f->ins[v].ty != IT_I64))
                ok = 0;
            else
                body[nb++] = v;
        }
        for (v = bb->phis; v && ok; v = f->ins[v].next)
            if (nphi == JT_MAXI * 2)
                ok = 0;
            else
                phis[nphi++] = v;
        /* the branch must depend on the phis through the body only, and each predecessor must decide it */
        for (k = 0; k < np && ok; k++) {
            u32 p = bb->preds[k], c = ir_resolve(f, bb->arg), m;
            u64 cv = 0;
            Block *pb = &f->blk[p];
            if (p == B || rpo_num[p] == 0xffffffffU || dominates(B, p) || (pb->term != TERM_JMP && pb->term != TERM_BR))
                ok = 0;
            for (j = 0; j < k && ok; j++)
                if (bb->preds[j] == p)
                    ok = 0;
            if (pb->term == TERM_BR && (pb->succ[0] == S[0] || pb->succ[0] == S[1] || pb->succ[1] == S[0] || pb->succ[1] == S[1]))
                ok = 0;
            if (!ok)
                break;
            for (m = 0; m < nb && ok; m++) {
                Ins *i = &f->ins[body[m]];
                u32 *ops, n, q;
                u64 a[3] = {0, 0, 0};
                IR_OPS(f, i, ops, n);
                if (n > 2) {
                    ok = 0;
                    break;
                }
                for (q = 0; q < n && ok; q++) {
                    u32 o = ir_resolve(f, ops[q]), t;
                    int found = 0;
                    if (f->ins[o].op == OP_PHI && f->ins[o].blk == B)
                        o = ir_resolve(f, f->xargs[f->ins[o].a + k]);
                    for (t = 0; t < m; t++)
                        if (body[t] == o) {
                            a[q] = val[t];
                            found = 1;
                        }
                    if (!found && !ir_is_const(f, o, &a[q]))
                        ok = 0;
                }
                if (ok && !jt_eval(i->op, i->ty, a[0], a[1], &val[m]))
                    ok = 0;
            }
            if (!ok)
                break;
            for (m = 0; m < nb; m++)
                if (body[m] == c)
                    break;
            if (m < nb) {
                cv = val[m];
            } else if (f->ins[c].op == OP_PHI && f->ins[c].blk == B) {
                if (!ir_is_const(f, ir_resolve(f, f->xargs[f->ins[c].a + k]), &cv))
                    ok = 0;
            } else {
                ok = 0;
            }
            to[k] = cv ? 0 : 1;
            cnt[to[k]]++;
        }
        if (!ok)
            continue;
        sd[0] = f->blk[S[0]].npreds == 1;
        sd[1] = f->blk[S[1]].npreds == 1;
        for (j = 0; j < 2 && ok; j++) {
            u32 nq = 0;
            for (v = f->blk[S[j]].phis; v; v = f->ins[v].next)
                nq++;
            if (nq > 64)
                ok = 0;
        }
        /* the body's values: only in B */
        for (k = 0; k < nb && ok; k++) {
            u32 nin = 0;
            ok = jt_used_ok(body[k], B, S[0], S[1], 0, 0, &nin);
        }
        for (k = 0; k < nb && ok; k++) {
            /* (nor in the successors' phis) */
            for (j = 0; j < 2 && ok; j++)
                for (v = f->blk[S[j]].phis; v; v = f->ins[v].next) {
                    u32 bi2 = pred_index(S[j], B);
                    if (bi2 != 0xffffffffU && ir_resolve(f, f->xargs[f->ins[v].a + bi2]) == body[k])
                        ok = 0;
                }
        }
        for (k = 0; k < nphi && ok; k++) {
            u32 nin = 0;
            ok = jt_used_ok(phis[k], B, S[0], S[1], sd[0], sd[1], &nin);
        }
        if (!ok)
            continue;
        /* each successor: the predecessors that now come to it, its phis' arguments for them, and B's phis in what it
           dominates */
        for (j = 0; j < 2; j++) {
            u32 s = S[j], bi = pred_index(s, B), first = 1, old[64], orig[64], nold = 0, q;
            if (!cnt[j])
                continue;
            for (q = f->blk[s].phis; q && nold < 64; q = f->ins[q].next) {
                orig[nold] = ir_resolve(f, f->xargs[f->ins[q].a + bi]); /* (B's edge, before it is rewritten) */
                old[nold++] = q;
            }
            if (sd[j]) {
                for (k = 0; k < nphi; k++) {
                    u32 x = phis[k], r = 0;
                    if (cnt[j] == 1) {
                        for (v = 0; v < np; v++)
                            if (to[v] == j)
                                r = ir_resolve(f, f->xargs[f->ins[x].a + v]);
                    } else {
                        r = ir_raw(f, OP_PHI, f->ins[x].ty, 0, 0);
                        for (v = 0; v < np; v++)
                            if (to[v] == j)
                                ir_phi_add_arg(f, r, ir_resolve(f, f->xargs[f->ins[x].a + v]));
                        f->ins[r].blk = s;
                        f->ins[r].next = f->blk[s].phis;
                        f->blk[s].phis = r;
                    }
                    jt_rewrite(x, s, r);
                }
            }
            /* the successor's own phis: B's edge becomes one per predecessor that comes */
            for (v = 0; v < np; v++) {
                u32 p = bb->preds[v], t;
                if (to[v] != j)
                    continue;
                if (first) {
                    f->blk[s].preds[bi] = p;
                    for (t = 0; t < nold; t++) {
                        u32 a = orig[t];
                        if (f->ins[a].op == OP_PHI && f->ins[a].blk == B)
                            a = ir_resolve(f, f->xargs[f->ins[a].a + v]);
                        f->xargs[f->ins[old[t]].a + bi] = a;
                    }
                    first = 0;
                } else {
                    ir_add_pred(f, s, p);
                    for (t = 0; t < nold; t++) {
                        u32 a = orig[t];
                        if (f->ins[a].op == OP_PHI && f->ins[a].blk == B)
                            a = ir_resolve(f, f->xargs[f->ins[a].a + v]);
                        ir_phi_add_arg(f, old[t], a);
                    }
                }
                for (q = 0; q < 2; q++)
                    if (f->blk[p].succ[q] == B && (q == 0 || f->blk[p].term == TERM_BR))
                        f->blk[p].succ[q] = s;
            }
        }
        for (j = 0; j < 2; j++)
            if (!cnt[j])
                remove_edge(B, S[j]);
        /* B is gone */
        bb = &f->blk[B];
        for (v = bb->phis; v; v = f->ins[v].next)
            f->ins[v].op = OP_NOP;
        for (v = bb->first; v; v = f->ins[v].next) {
            f->ins[v].op = OP_NOP;
            f->ins[v].blk = 0;
        }
        bb->phis = bb->first = bb->last = 0;
        bb->term = TERM_NONE;
        bb->arg = 0;
        bb->npreds = 0;
        changed = 1;
        compute_dom();
    }
    return changed;
}

/* How many times each value is used, as things stand now. */
static u32 *count_uses(u32 *nuses)
{
    u32 *uses = (u32 *)xmalloc(f->nins * sizeof(u32)), bi, n, j, v, *ops;
    *nuses = f->nins;
    memset(uses, 0, f->nins * sizeof(u32));
    for (bi = 1; bi < f->nblk; bi++) {
        Block *ub = &f->blk[bi];
        if (ub->term == TERM_NONE)
            continue;
        for (v = ub->phis; v; v = f->ins[v].next)
            for (j = 0; j < f->ins[v].b; j++)
                uses[ir_resolve(f, f->xargs[f->ins[v].a + j])]++;
        for (v = ub->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, ops, n);
            for (j = 0; j < n; j++)
                uses[f->ins[ops[j]].op == OP_COPY ? ir_resolve(f, ops[j]) : ops[j]]++;
            if (i->op == OP_CALL)
                uses[ir_resolve(f, i->c)]++;
        }
        if (ub->arg)
            uses[ir_resolve(f, ub->arg)]++;
    }
    return uses;
}

/* Is block u reached only through s (a chain of blocks with one predecessor each, up to s)? */
static int only_through(u32 u, u32 s)
{
    int n;
    for (n = 0; n < 16; n++) {
        if (u == s)
            return 1;
        if (f->blk[u].npreds != 1)
            return 0;
        u = f->blk[u].preds[0];
    }
    return 0;
}

/* The uses of x but skip: bit s set when all are beyond successor s of b (reached only through it, and it only
   from b). */
static int uses_beyond(u32 b, u32 x, u32 skip)
{
    Block *bb = &f->blk[b];
    u32 c, v, k, n, *ops, s[2];
    int res = 0, side;
    if (f->nins > 200000)
        return 0;
    s[0] = bb->succ[0];
    s[1] = bb->succ[1];
    for (side = 0; side < 2; side++) {
        int good = s[side] != b && s[0] != s[1] && f->blk[s[side]].npreds == 1;
        for (c = 1; c < f->nblk && good; c++) {
            Block *cb = &f->blk[c];
            if (cb->term == TERM_NONE)
                continue;
            for (v = cb->phis; v && good; v = f->ins[v].next)
                for (k = 0; k < f->ins[v].b && good; k++)
                    if (ir_resolve(f, f->xargs[f->ins[v].a + k]) == x && (v == x || !only_through(cb->preds[k], s[side])))
                        good = 0;
            for (v = cb->first; v && good; v = f->ins[v].next) {
                if (v == skip)
                    continue;
                IR_OPS(f, &f->ins[v], ops, n);
                for (k = 0; k < n && good; k++)
                    if (ir_resolve(f, ops[k]) == x && !only_through(c, s[side]))
                        good = 0;
                if (f->ins[v].op == OP_CALL && ir_resolve(f, f->ins[v].c) == x && !only_through(c, s[side]))
                    good = 0;
            }
            if (good && cb->arg && ir_resolve(f, cb->arg) == x && !only_through(c, s[side]))
                good = 0;
        }
        if (good)
            res |= 1 << side;
    }
    return res;
}

/* The phis of block b other than p: bit s set when all their uses are beyond successor s of b (reached only through
   it, and it only from b), so that a predecessor sent past b to the other successor needs none of them. */
static int phis_under(u32 b, u32 p)
{
    Block *bb = &f->blk[b];
    u32 c, v, k, n, *ops, s[2];
    int res = 0, side;
    if (f->nins > 200000)
        return 0;
    s[0] = bb->succ[0];
    s[1] = bb->succ[1];
    for (side = 0; side < 2; side++) {
        int good = s[side] != b && s[0] != s[1] && f->blk[s[side]].npreds == 1;
        for (c = 1; c < f->nblk && good; c++) {
            Block *cb = &f->blk[c];
            if (cb->term == TERM_NONE)
                continue;
            for (v = cb->phis; v && good; v = f->ins[v].next)
                for (k = 0; k < f->ins[v].b && good; k++) {
                    u32 x = ir_resolve(f, f->xargs[f->ins[v].a + k]);
                    if (x != p && f->ins[x].op == OP_PHI && f->ins[x].blk == b && (x == v || !only_through(cb->preds[k], s[side])))
                        good = 0;
                }
            for (v = cb->first; v && good; v = f->ins[v].next) {
                IR_OPS(f, &f->ins[v], ops, n);
                for (k = 0; k < n && good; k++) {
                    u32 x = ir_resolve(f, ops[k]);
                    if (x != p && f->ins[x].op == OP_PHI && f->ins[x].blk == b && !only_through(c, s[side]))
                        good = 0;
                }
                if (f->ins[v].op == OP_CALL) {
                    u32 x = ir_resolve(f, f->ins[v].c);
                    if (x != p && f->ins[x].op == OP_PHI && f->ins[x].blk == b && !only_through(c, s[side]))
                        good = 0;
                }
            }
            if (good && cb->arg) {
                u32 x = ir_resolve(f, cb->arg);
                if (x != p && f->ins[x].op == OP_PHI && f->ins[x].blk == b && !only_through(c, s[side]))
                    good = 0;
            }
        }
        if (good)
            res |= 1 << side;
    }
    return res;
}

static int simplify_cfg(void)
{
    u32 b, v, k;
    int changed = forward_phi_blocks(), again;
    u32 *uses = 0, nuses = 0;
    if (opt_arch == ARCH_X64 && opt_level >= 2 && jump_thread())
        changed = 1;

    do {
        again = 0;
        for (b = 1; b < f->nblk; b++) {
            Block *bb = &f->blk[b];
            u64 c;
            if (bb->term == TERM_NONE || (b != 1 && bb->npreds == 0))
                continue;
            /* branch on a constant */
            if (bb->term == TERM_BR) {
                bb->arg = ir_resolve(f, bb->arg);
                if (ir_is_const(f, bb->arg, &c)) {
                    u32 keep = bb->succ[c ? 0 : 1], drop = bb->succ[c ? 1 : 0];
                    bb->term = TERM_JMP;
                    bb->succ[0] = keep;
                    bb->arg = 0;
                    remove_edge(b, drop);
                    bb = &f->blk[b];
                    again = 1;
                }
            }
            /* jump to a block with no other predecessor: merge it in */
            while (bb->term == TERM_JMP && bb->succ[0] != b && bb->succ[0] != 1 && f->blk[bb->succ[0]].npreds == 1) {
                u32 s = bb->succ[0], n;
                Block *sb = &f->blk[s];
                while (sb->phis)
                    unlink_phi(s, sb->phis, ir_resolve(f, f->xargs[f->ins[sb->phis].a]));
                for (v = sb->first; v; v = f->ins[v].next)
                    f->ins[v].blk = b;
                if (sb->first) {
                    if (bb->last) {
                        f->ins[bb->last].next = sb->first;
                        f->ins[sb->first].prev = bb->last;
                    } else {
                        bb->first = sb->first;
                    }
                    bb->last = sb->last;
                }
                bb->term = sb->term;
                bb->arg = sb->arg;
                bb->succ[0] = sb->succ[0];
                bb->succ[1] = sb->succ[1];
                n = nsuccs(sb);
                for (k = 0; k < n; k++) {
                    u32 pi = pred_index(sb->succ[k], s);
                    f->blk[sb->succ[k]].preds[pi] = b;
                }
                sb->first = sb->last = 0;
                sb->term = TERM_NONE;
                sb->npreds = 0;
                sb->arg = 0;
                again = 1;
            }
            /* branch on a phi (or on phi != 0, phi == 0) of this otherwise empty
               block: each predecessor can decide the branch itself. One that
               supplies a constant goes straight to the right successor; one
               that ends in a jump gets its own copy of the branch. */
            if (bb->term == TERM_BR && bb->phis && (!f->ins[bb->phis].next || (!opt_dev_fn && opt_level >= 2))) {
                u32 p = bb->phis, cmp = 0, cop = OP_NE, nphi = 0, q, np;
                int inv = 0, ok = 0, under = 3, pside; /* pside: see below */ /* under: bit s, the other phis are used only beyond successor s */
                u64 ck = 0;
                for (q = bb->phis; q; q = f->ins[q].next)
                    nphi++;
                if (nphi > 1) {
                    /* several phis (a bool function inlined that also wrote through a pointer): the branch's own is
                       found from the branch */
                    u32 x = ir_resolve(f, bb->arg);
                    if (bb->first && bb->first == bb->last && x == bb->first)
                        x = ir_resolve(f, f->ins[x].a);
                    p = f->ins[x].op == OP_PHI && f->ins[x].blk == b ? x : 0;
                    under = p && nphi <= 5 ? phis_under(b, p) : 0;
                    if (!under)
                        p = 0;
                }
                if (!p) {
                } else if (!bb->first && bb->arg == p) {
                    ok = 1;
                } else if (bb->first && bb->first == bb->last && bb->arg == bb->first) {
                    Ins *ci = &f->ins[bb->first];
                    u64 z;
                    if ((ci->op == OP_NE || ci->op == OP_EQ) && ir_resolve(f, ci->a) == p &&
                        ir_is_const(f, ir_resolve(f, ci->b), &z) && z == 0) {
                        ok = 1;
                        cmp = bb->first;
                        inv = ci->op == OP_EQ;
                    } else if (!opt_dev_fn && ci->op >= OP_SLT && ci->op <= OP_UGE && ir_resolve(f, ci->a) == p &&
                               (f->ins[p].ty == IT_I32 || f->ins[p].ty == IT_I64) && ir_is_const(f, ir_resolve(f, ci->b), &z)) {
                        /* x < k and the like (a function returning -1 for failure, inlined: its result < 0) */
                        ok = 1;
                        cmp = bb->first;
                        cop = ci->op;
                        ck = z;
                    }
                }
                if (ok && !uses) {
                    uses = count_uses(&nuses);
                    bb = &f->blk[b];
                }
                /* the phi used beyond the branch too (the symbol a decoder's function returned, -1 for failure): only
                   when all those uses are beyond one successor; a predecessor sent there gets a phi of its own value
                   there, one sent to the other needs none */
                pside = -1;
                np = 0;
                if (ok && cmp && p < nuses && uses[p] > 1 && cmp < nuses && uses[cmp] == 1 && !opt_dev_fn && opt_level >= 2) {
                    int u = uses_beyond(b, p, cmp);
                    pside = u & 1 ? 0 : u & 2 ? 1 : -1;
                    if (pside < 0)
                        ok = 0;
                }
                if (ok && p < nuses && (uses[p] == 1 || pside >= 0) && (!cmp || (cmp < nuses && uses[cmp] == 1))) {
                    u32 s0 = bb->succ[0], s1 = bb->succ[1];
                    int pty = f->ins[p].ty;
                    for (k = 0; k < bb->npreds && f->ins[p].op == OP_PHI;) {
                        u32 pr = bb->preds[k], target, j, tv;
                        Block *pb = &f->blk[pr], *tb;
                        u32 arg = ir_resolve(f, f->xargs[f->ins[p].a + k]), arg0 = arg;
                        if (pr == b) {
                            k++;
                            continue;
                        }
                        if (!ir_is_const(f, arg, &c) && cop >= OP_SLT && cop <= OP_SGE && pty == IT_I32 && ir_nonneg(f, arg) &&
                            (i64)(i32)(u32)ck <= ((cop == OP_SLT || cop == OP_SGE) ? 0 : -1)) {
                            /* a value that is not negative against a bound at or below zero: decided (the -1 / byte
                               result of a decoder's symbol function) */
                            arg = ir_const(f, pty, 0x7fffffff); /* (compares as any such value would) */
                        }
                        if (!ir_is_const(f, arg, &c)) {
                            u32 cond, saved = f->cur, t;
                            if (nphi > 1 || pside >= 0 || pb->term != TERM_JMP || s0 == s1 || s0 == b || s1 == b || pty != IT_I32 ||
                                pred_index(s0, pr) != 0xffffffffU || pred_index(s1, pr) != 0xffffffffU) {
                                k++;
                                continue;
                            }
                            f->cur = pr;
                            cond = cop != OP_NE ? ir_ins(f, cop, IT_I32, arg, ir_const(f, pty, ck))
                                                : ir_ins(f, inv ? OP_EQ : OP_NE, IT_I32, arg, ir_const(f, pty, 0));
                            f->cur = saved;
                            pb = &f->blk[pr];
                            for (t = 0; t < 2; t++) {
                                target = t ? s1 : s0;
                                tb = &f->blk[target];
                                j = pred_index(target, b);
                                for (tv = tb->phis; tv; tv = f->ins[tv].next)
                                    ir_phi_add_arg(f, tv, f->xargs[f->ins[tv].a + j]);
                                ir_add_pred(f, target, pr);
                            }
                            pb = &f->blk[pr];
                            pb->term = TERM_BR;
                            pb->arg = cond;
                            pb->succ[0] = s0;
                            pb->succ[1] = s1;
                            remove_pred(b, k);
                            bb = &f->blk[b];
                            again = 1;
                            continue;
                        }
                        if (cop != OP_NE) {
                            /* the comparison of the constant, in the phi's width */
                            int w64 = pty == IT_I64;
                            i64 sa = w64 ? (i64)c : (i64)(i32)(u32)c, sb = w64 ? (i64)ck : (i64)(i32)(u32)ck;
                            u64 ua = w64 ? c : c & 0xffffffffUL, ub = w64 ? ck : ck & 0xffffffffUL;
                            switch (cop) {
                            case OP_SLT: c = sa < sb; break;
                            case OP_SLE: c = sa <= sb; break;
                            case OP_SGT: c = sa > sb; break;
                            case OP_SGE: c = sa >= sb; break;
                            case OP_ULT: c = ua < ub; break;
                            case OP_ULE: c = ua <= ub; break;
                            case OP_UGT: c = ua > ub; break;
                            default: c = ua >= ub; break;
                            }
                        } else if (inv)
                            c = !c;
                        target = c ? s0 : s1;
                        tb = &f->blk[target];
                        if (target == b || pred_index(target, pr) != 0xffffffffU || (nphi > 1 && !(under >> (c ? 1 : 0) & 1)) ||
                            (nsuccs(pb) == 2 && pb->succ[0] == pb->succ[1])) {
                            k++;
                            continue;
                        }
                        if (pside >= 0 && target == (pside ? s1 : s0) && !np) {
                            /* the first sent where the phi is used: a phi there takes its uses */
                            u32 c2, w, n2, *o2;
                            if (tb->npreds != 1) {
                                k++;
                                continue;
                            }
                            np = ir_phi(f, target, pty);
                            f->xargs[f->ins[np].a] = p;
                            for (c2 = 1; c2 < f->nblk; c2++) {
                                Block *cb = &f->blk[c2];
                                if (cb->term == TERM_NONE)
                                    continue;
                                /* (a phi's argument by the block it comes from) */
                                for (w = cb->phis; w; w = f->ins[w].next)
                                    if (w != np)
                                        for (j = 0; j < f->ins[w].b; j++)
                                            if (ir_resolve(f, f->xargs[f->ins[w].a + j]) == p && only_through(cb->preds[j], target))
                                                f->xargs[f->ins[w].a + j] = np;
                                if (!only_through(c2, target))
                                    continue;
                                for (w = cb->first; w; w = f->ins[w].next) {
                                    IR_OPS(f, &f->ins[w], o2, n2);
                                    for (j = 0; j < n2; j++)
                                        if (ir_resolve(f, o2[j]) == p)
                                            o2[j] = np;
                                    if (f->ins[w].op == OP_CALL && ir_resolve(f, f->ins[w].c) == p)
                                        f->ins[w].c = np;
                                }
                                if (cb->arg && ir_resolve(f, cb->arg) == p)
                                    cb->arg = np;
                            }
                            tb = &f->blk[target];
                        }
                        /* the new edge carries the same phi values as the edge from b */
                        j = pred_index(target, b);
                        for (tv = tb->phis; tv; tv = f->ins[tv].next)
                            ir_phi_add_arg(f, tv, tv == np ? arg0 : f->xargs[f->ins[tv].a + j]);
                        ir_add_pred(f, target, pr);
                        for (j = 0; j < nsuccs(pb); j++)
                            if (pb->succ[j] == b)
                                pb->succ[j] = target;
                        remove_pred(b, k);
                        bb = &f->blk[b];
                        again = 1;
                    }
                    if (bb->npreds == 0 && b != 1) {
                        bb->term = TERM_NONE;
                        bb->arg = 0;
                        bb->phis = 0;
                        bb->first = bb->last = 0;
                        remove_edge(b, s0);
                        remove_edge(b, s1);
                        continue;
                    }
                    bb->arg = ir_resolve(f, bb->arg);
                }
            }
            /* if-conversion: a branch around nothing, or to two empty arms,
               that only chooses values for phis becomes selects */
            if (bb->term == TERM_BR && bb->succ[0] != bb->succ[1] && opt_level >= 3 && !opt_size && !opt_dev_fn && hoist_arms(b)) {
                bb = &f->blk[b];
                changed = 1;
            }
            if (bb->term == TERM_BR && bb->succ[0] != bb->succ[1]) {
                u32 t = bb->succ[0], e = bb->succ[1], j = 0, pt, pe, nphi = 0;
                Block *tb = &f->blk[t], *eb = &f->blk[e];
                int t_empty = tb->term == TERM_JMP && cheap_block(t) && !tb->phis && tb->npreds == 1 && t != b;
                int e_empty = eb->term == TERM_JMP && cheap_block(e) && !eb->phis && eb->npreds == 1 && e != b;
                if (t_empty && e_empty && tb->succ[0] == eb->succ[0])
                    j = tb->succ[0];
                else if (t_empty && tb->succ[0] == e)
                    j = e;
                else if (e_empty && eb->succ[0] == t)
                    j = t;
                if (j && j != b && j != 1 && f->blk[j].phis && !opt_dev_fn && !opt_size) {
                    /* A test of something just loaded, used for nothing else: as a branch, what follows
                       can start on a guess while the load is on its way; a select would make everything
                       after it wait for the load. */
                    Ins *ci = &f->ins[bb->arg];
                    u32 ld = 0;
                    if (ci->op == OP_LOAD)
                        ld = bb->arg;
                    else if (ci->op >= OP_EQ && ci->op <= OP_UGE)
                        ld = f->ins[ci->a].op == OP_LOAD ? ci->a : f->ins[ci->b].op == OP_LOAD ? ci->b : 0;
                    if (ld && !uses) {
                        uses = count_uses(&nuses);
                        bb = &f->blk[b];
                    }
                    if (ld && ld < nuses && uses[ld] == 1 && bb->arg < nuses && uses[bb->arg] == 1)
                        j = 0;
                }
                if (j && j != b && j != 1 && f->blk[j].phis) {
                    Block *jb = &f->blk[j];
                    pt = pred_index(j, j == t ? b : t);
                    pe = pred_index(j, j == e ? b : e);
                    for (v = jb->phis; v; v = f->ins[v].next)
                        nphi++;
                    if (nphi <= (opt_dev_fn ? 4U : 2U) && pt != 0xffffffffU && pe != 0xffffffffU && pt != pe) {
                        u32 cond = bb->arg, keep = j == t ? pt : j == e ? pe : pt, drop = keep == pt ? pe : pt;
                        u32 arm, nx2;
                        /* what the arms compute is now computed either way */
                        for (arm = 0; arm < 2; arm++) {
                            u32 ab = arm ? e : t;
                            if (ab == j)
                                continue;
                            for (v = f->blk[ab].first; v; v = nx2) {
                                nx2 = f->ins[v].next;
                                ir_remove(f, v);
                                ir_append(f, b, v);
                            }
                        }
                        for (v = jb->phis; v; v = f->ins[v].next) {
                            Ins *pi = &f->ins[v];
                            u32 vt = ir_resolve(f, f->xargs[pi->a + pt]), ve = ir_resolve(f, f->xargs[pi->a + pe]), sel;
                            u64 ct, ce;
                            int cop = f->ins[cond].op;
                            int is_bool = (cop >= OP_EQ && cop <= OP_UGE) || (cop >= OP_FEQ && cop <= OP_FGE);
                            if (vt == ve) {
                                sel = vt;
                            } else if (is_bool && pi->ty == IT_I32 && ir_is_const(f, vt, &ct) && ir_is_const(f, ve, &ce) &&
                                       ct == 1 && ce == 0) {
                                sel = cond; /* cond ? 1 : 0 */
                            } else if (is_bool && pi->ty == IT_I32 && ir_is_const(f, vt, &ct) && ir_is_const(f, ve, &ce) &&
                                       ct == 0 && ce == 1) {
                                u32 saved = f->cur;
                                f->cur = b;
                                sel = ir_ins(f, OP_EQ, IT_I32, cond, ir_const(f, IT_I32, 0));
                                f->cur = saved;
                            } else if (is_bool && pi->ty == IT_I32 &&
                                       ((ir_is_const(f, vt, &ct) && ct == 1 && ir_is_bool(f, ve, 3)) ||
                                        (ir_is_const(f, ve, &ce) && ce == 0 && ir_is_bool(f, vt, 3)))) {
                                /* truth values: cond ? 1 : x is cond | x, cond ? x : 0 is cond & x */
                                u32 saved = f->cur;
                                int isor = ir_is_const(f, vt, &ct);
                                f->cur = b;
                                sel = ir_ins(f, isor ? OP_OR : OP_AND, IT_I32, cond, isor ? ve : vt);
                                f->cur = saved;
                            } else {
                                sel = ir_raw(f, OP_SELECT, pi->ty, cond, vt);
                                f->ins[sel].c = ve;
                                ir_append(f, b, sel);
                            }
                            pi = &f->ins[v];
                            f->xargs[pi->a + keep] = sel;
                        }
                        /* b now jumps straight to j along the edge kept */
                        bb = &f->blk[b];
                        jb = &f->blk[j];
                        jb->preds[keep] = b;
                        bb->term = TERM_JMP;
                        bb->arg = 0;
                        bb->succ[0] = j;
                        if (j != t) {
                            tb = &f->blk[t];
                            tb->term = TERM_NONE;
                            tb->npreds = 0;
                        }
                        if (j != e) {
                            eb = &f->blk[e];
                            eb->term = TERM_NONE;
                            eb->npreds = 0;
                        }
                        remove_pred(j, drop);
                        bb = &f->blk[b];
                        again = 1;
                    }
                }
            }
            /* device code: a branch to a block that does little and branches again, one of its ways being a
               way of this branch too, becomes one branch on both conditions (a && b, a || b): fewer blocks
               for the GPU's assembler, as NVIDIA's compiler leaves it */
            if (opt_dev_fn && !opt_nochain && bb->term == TERM_BR && bb->succ[0] != bb->succ[1]) {
                int side;
                for (side = 0; side < 2; side++) {
                    u32 t = bb->succ[side], x = bb->succ[!side], y, c1, c2, n1, n2, nc, saved, px, pt, nx2;
                    Block *tb = &f->blk[t];
                    int tside, ok = 1, o1, o2;
                    if (t == b || t == 1 || x == t || tb->term != TERM_BR || tb->npreds != 1 || tb->phis ||
                        tb->succ[0] == tb->succ[1] || !chain_block(t))
                        continue;
                    if (tb->succ[0] == x)
                        tside = 1;
                    else if (tb->succ[1] == x)
                        tside = 0;
                    else
                        continue;
                    y = tb->succ[tside];
                    if (y == t || y == b || y == x || pred_index(y, b) != 0xffffffffU)
                        continue;
                    c1 = ir_resolve(f, bb->arg);
                    c2 = ir_resolve(f, tb->arg);
                    o1 = f->ins[c1].op;
                    o2 = f->ins[c2].op;
                    if (!((o1 >= OP_EQ && o1 <= OP_UGE) || (o1 >= OP_FEQ && o1 <= OP_FGE)) ||
                        !((o2 >= OP_EQ && o2 <= OP_UGE) || (o2 >= OP_FEQ && o2 <= OP_FGE)))
                        continue;
                    px = pred_index(x, b);
                    pt = pred_index(x, t);
                    if (px == 0xffffffffU || pt == 0xffffffffU)
                        continue;
                    for (v = f->blk[x].phis; v; v = f->ins[v].next)
                        if (ir_resolve(f, f->xargs[f->ins[v].a + px]) != ir_resolve(f, f->xargs[f->ins[v].a + pt]))
                            ok = 0;
                    if (!ok)
                        continue;
                    for (v = tb->first; v; v = nx2) {
                        nx2 = f->ins[v].next;
                        ir_remove(f, v);
                        ir_append(f, b, v);
                    }
                    saved = f->cur;
                    f->cur = b;
                    n1 = side == 0 ? c1 : ir_ins(f, OP_EQ, IT_I32, c1, ir_const(f, IT_I32, 0));
                    n2 = tside == 0 ? c2 : ir_ins(f, OP_EQ, IT_I32, c2, ir_const(f, IT_I32, 0));
                    nc = ir_ins(f, OP_AND, IT_I32, n1, n2);
                    f->cur = saved;
                    bb = &f->blk[b];
                    tb = &f->blk[t];
                    bb->arg = nc;
                    bb->succ[0] = y;
                    bb->succ[1] = x;
                    f->blk[y].preds[pred_index(y, t)] = b;
                    remove_pred(x, pred_index(x, t));
                    tb->term = TERM_NONE;
                    tb->arg = 0;
                    tb->npreds = 0;
                    tb->first = tb->last = 0;
                    bb = &f->blk[b];
                    again = 1;
                    break;
                }
            }
            /* an empty block that only jumps on: route its predecessors past it */
            if (bb->term == TERM_JMP && !bb->first && !bb->phis && b != 1 && bb->succ[0] != b) {
                u32 s = bb->succ[0];
                Block *sb = &f->blk[s];
                if (!sb->phis) {
                    while (bb->npreds) {
                        u32 pr = bb->preds[0], j;
                        Block *pb = &f->blk[pr];
                        for (j = 0; j < nsuccs(pb); j++)
                            if (pb->succ[j] == b)
                                pb->succ[j] = s;
                        if (pb->term == TERM_BR && pb->succ[0] == pb->succ[1]) {
                            pb->term = TERM_JMP;
                            pb->arg = 0;
                        } else {
                            ir_add_pred(f, s, pr);
                        }
                        remove_pred(b, 0);
                    }
                    bb->term = TERM_NONE;
                    remove_pred(s, pred_index(s, b));
                    again = 1;
                } else if (bb->npreds == 1 && pred_index(s, bb->preds[0]) == 0xffffffffU) {
                    u32 pr = bb->preds[0], j;
                    Block *pb = &f->blk[pr];
                    for (j = 0; j < nsuccs(pb); j++)
                        if (pb->succ[j] == b)
                            pb->succ[j] = s;
                    sb->preds[pred_index(s, b)] = pr;
                    bb->npreds = 0;
                    bb->term = TERM_NONE;
                    again = 1;
                }
            }
        }
        if (again) {
            /* loops cut off from the entry still have predecessors: sweep
               away whatever can no longer be reached */
            u32 *stack = (u32 *)xmalloc(f->nblk * sizeof(u32)), sp = 0;
            for (b = 1; b < f->nblk; b++)
                f->blk[b].flag = 0;
            stack[sp++] = 1;
            f->blk[1].flag = 1;
            while (sp) {
                Block *xb = &f->blk[stack[--sp]];
                u32 n = nsuccs(xb);
                for (k = 0; k < n; k++) {
                    if (!f->blk[xb->succ[k]].flag) {
                        f->blk[xb->succ[k]].flag = 1;
                        stack[sp++] = xb->succ[k];
                    }
                }
            }
            for (b = 1; b < f->nblk; b++) {
                Block *xb = &f->blk[b];
                u32 n = nsuccs(xb), s0 = xb->succ[0], s1 = xb->succ[1];
                if (xb->flag || xb->term == TERM_NONE)
                    continue;
                xb->term = TERM_NONE;
                xb->arg = 0;
                if (n >= 1 && f->blk[s0].flag)
                    remove_pred(s0, pred_index(s0, b));
                if (n == 2 && f->blk[s1].flag)
                    remove_pred(s1, pred_index(s1, b));
            }
            for (b = 1; b < f->nblk; b++) {
                Block *xb = &f->blk[b];
                if (!xb->flag && xb->term == TERM_NONE && (xb->first || xb->phis || xb->npreds)) {
                    for (v = xb->first; v; v = k) {
                        k = f->ins[v].next;
                        f->ins[v].blk = 0;
                        f->ins[v].prev = f->ins[v].next = 0;
                    }
                    xb->first = xb->last = 0;
                    xb->phis = 0;
                    xb->npreds = 0;
                }
                xb->flag = 0;
            }
            free(stack);
            changed = 1;
            free(uses);
            uses = 0;
        }
    } while (again);
    free(uses);
    return changed;
}

/* ---- loop-invariant code motion ---- */

static int defined_outside(u32 v, const u32 *mark, u32 h)
{
    Ins *i = &f->ins[v];
    if (!i->blk)
        return i->op == OP_CONST || i->op == OP_FCONST || i->op == OP_ADDR || i->op == OP_SLOT;
    return mark[i->blk] != h;
}


/* a new preheader runs as often as the blocks that enter it, and no more
   often than the loop's header */
static void preheader_freq(u32 n, u32 h)
{
    u32 p;
    u64 sum = 0;
    if (!f->blk[h].freq)
        return;
    for (p = 0; p < f->blk[n].npreds; p++) {
        u32 q = f->blk[f->blk[n].preds[p]].freq;
        if (!q)
            return;
        sum += q - 1;
    }
    if (sum > f->blk[h].freq - 1)
        sum = f->blk[h].freq - 1;
    f->blk[n].freq = (u32)sum + 1;
}

/* Device code: the sums in loops are put in the order of how deep in loops their terms are worked out, so
   that what does not change in a loop is one sum (which licm then moves out as one value), and the terms that
   the unrolled copies of a body share are added first, in the same order (which gvn then makes one). */
static int dev_reassoc(void)
{
    u32 *depth, *stamp, *stack, *nuse, k, p, v, q, sp, nloop = 0;
    u8 *inner;
    int changed = 0;
    compute_dom();
    depth = (u32 *)xmalloc(cap_blocks * 3 * sizeof(u32));
    stamp = depth + cap_blocks;
    stack = stamp + cap_blocks;
    memset(depth, 0, cap_blocks * 2 * sizeof(u32));
    for (k = 0; k < nrpo; k++) {
        u32 h = rpo[k];
        Block *hb = &f->blk[h];
        sp = 0;
        for (p = 0; p < hb->npreds; p++) {
            u32 x = hb->preds[p];
            if (x < cap_blocks && rpo_num[x] != 0xffffffffU && dominates(h, x))
                stack[sp++] = x;
        }
        if (!sp)
            continue;
        nloop++;
        stamp[h] = nloop;
        depth[h]++;
        while (sp) {
            u32 x = stack[--sp];
            if (stamp[x] == nloop)
                continue;
            stamp[x] = nloop;
            depth[x]++;
            for (p = 0; p < f->blk[x].npreds; p++)
                if (sp < cap_blocks && stamp[f->blk[x].preds[p]] != nloop)
                    stack[sp++] = f->blk[x].preds[p];
        }
    }
    if (!nloop) {
        free(depth);
        return 0;
    }
    nuse = (u32 *)xmalloc((size_t)f->nins * sizeof(u32));
    inner = (u8 *)xmalloc(f->nins);
    memset(nuse, 0, (size_t)f->nins * sizeof(u32));
    memset(inner, 0, f->nins);
    for (k = 0; k < nrpo; k++) {
        Block *bb = &f->blk[rpo[k]];
        if (bb->arg)
            nuse[ir_resolve(f, bb->arg)]++;
        for (p = 0; p < 2; p++)
            for (v = p ? bb->first : bb->phis; v; v = f->ins[v].next) {
                u32 n = ir_opcount(f, v);
                for (q = 0; q < n; q++)
                    nuse[*ir_opref(f, v, q)]++;
            }
    }
#define SUMTERM(o, i) (f->ins[o].op == OP_ADD && f->ins[o].ty == (i)->ty && f->ins[o].blk && nuse[o] == 1 && \
                       depth[f->ins[o].blk] == depth[(i)->blk])
    for (k = 0; k < nrpo; k++)
        for (v = f->blk[rpo[k]].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            if (i->op != OP_ADD || (i->ty != IT_I32 && i->ty != IT_I64) || !depth[i->blk])
                continue;
            if (SUMTERM(i->a, i))
                inner[i->a] = 1;
            if (SUMTERM(i->b, i))
                inner[i->b] = 1;
        }
    for (k = 0; k < nrpo; k++) {
        u32 nx;
        for (v = f->blk[rpo[k]].first; v; v = nx) {
            Ins *i = &f->ins[v];
            u32 leaf[16], rank[16], todo[16], nl = 0, nt = 0, t, lo, hi, n0 = f->nins;
            u64 csum = 0, c;
            int ty = i->ty, many = 0, sorted = 1;
            nx = i->next;
            if (v >= n0 || i->op != OP_ADD || (ty != IT_I32 && ty != IT_I64) || !depth[i->blk] || inner[v])
                continue;
            todo[nt++] = i->b;
            todo[nt++] = i->a;
            while (nt && !many) {
                u32 o = todo[--nt];
                if (o < n0 && inner[o]) {
                    if (nt + 2 > 16) {
                        many = 1;
                        break;
                    }
                    todo[nt++] = f->ins[o].b;
                    todo[nt++] = f->ins[o].a;
                } else if (ir_is_const(f, o, &c)) {
                    csum += c;
                } else if (nl == 16) {
                    many = 1;
                } else {
                    leaf[nl] = o;
                    rank[nl++] = f->ins[o].blk ? depth[f->ins[o].blk] : 0;
                }
            }
            if (many || nl < 3)
                continue;
            lo = hi = rank[0];
            for (p = 1; p < nl; p++) {
                if (rank[p] < lo)
                    lo = rank[p];
                if (rank[p] > hi)
                    hi = rank[p];
            }
            if (lo == hi)
                continue;
            /* by rank, and within it by number */
            for (p = 1; p < nl; p++) {
                u32 lv = leaf[p], lr = rank[p];
                for (q = p; q && (rank[q - 1] > lr || (rank[q - 1] == lr && leaf[q - 1] > lv)); q--) {
                    leaf[q] = leaf[q - 1];
                    rank[q] = rank[q - 1];
                    sorted = 0;
                }
                leaf[q] = lv;
                rank[q] = lr;
            }
            (void)sorted;
            t = leaf[0];
            for (p = 1; p < nl; p++) {
                if (p == nl - 1 && !csum)
                    break;
                t = ir_raw(f, OP_ADD, ty, t, leaf[p]);
                ir_insert_before(f, v, t);
            }
            i = &f->ins[v];
            i->a = t;
            i->b = csum ? ir_const(f, ty, csum) : leaf[nl - 1];
            changed = 1;
        }
    }
#undef SUMTERM
    free(inner);
    free(nuse);
    free(depth);
    return changed;
}

/* ---- || and && chains regrouped (after licm): an or (and) of truth values in block b whose leaves are partly
   made outside b becomes or(or of those, or of the rest), so licm can take the first part out of the loop; and
   x < 0 || x >= k (x >= 0 && x < k), k >= 0 a constant, is the one unsigned x >= k (x < k). ---- */
#define RG_MAX 16
static u32 *rg_uses;

static int rg_collect(u32 v, int op, u32 b, u32 *lv, u32 n, int root)
{
    Ins *i = &f->ins[v];
    if (n > RG_MAX)
        return n;
    if (i->op == op && i->ty == IT_I32 && i->blk == b && (root || rg_uses[v] == 1)) {
        n = (u32)rg_collect(i->a, op, b, lv, n, 0);
        return rg_collect(i->b, op, b, lv, n, 0);
    }
    if (n < RG_MAX)
        lv[n] = v;
    return (int)(n + 1);
}

/* x < 0 (low = 1) or x >= k (k >= 0: low = 0, *k) for the or; for the and x >= 0 (low = 1), x < k */
static u32 rg_bound(u32 v, int isand, int *low, i64 *k)
{
    Ins *i = &f->ins[v];
    u64 c;
    int t;
    if (i->op < OP_SLT || i->op > OP_SGE || !ir_is_const(f, i->b, &c))
        return 0;
    t = f->ins[i->a].ty;
    if (t != IT_I32 && t != IT_I64)
        return 0;
    if (t == IT_I32)
        c = (u64)(i64)(i32)(u32)c;
    if (!isand) {
        if ((i->op == OP_SLT && (i64)c == 0) || (i->op == OP_SLE && (i64)c == -1)) {
            *low = 1;
            return i->a;
        }
        if ((i->op == OP_SGE && (i64)c >= 0) || (i->op == OP_SGT && (i64)c >= -1)) {
            *low = 0;
            *k = i->op == OP_SGE ? (i64)c : (i64)c + 1;
            return i->a;
        }
    } else {
        if ((i->op == OP_SGE && (i64)c == 0) || (i->op == OP_SGT && (i64)c == -1)) {
            *low = 1;
            return i->a;
        }
        if ((i->op == OP_SLT && (i64)c >= 0) || (i->op == OP_SLE && (i64)c >= -1)) {
            *low = 0;
            *k = i->op == OP_SLT ? (i64)c : (i64)c + 1;
            return i->a;
        }
    }
    return 0;
}

static int bool_regroup(void)
{
    u32 b, v, k, n, *ops, nx;
    int changed = 0;
    rg_uses = (u32 *)xmalloc((f->nins + 1) * sizeof(u32));
    memset(rg_uses, 0, (f->nins + 1) * sizeof(u32));
    for (b = 1; b < f->nblk; b++) {
        for (v = f->blk[b].phis; v; v = f->ins[v].next) {
            IR_OPS(f, &f->ins[v], ops, n);
            for (k = 0; k < n; k++)
                rg_uses[ops[k]]++;
        }
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            IR_OPS(f, &f->ins[v], ops, n);
            for (k = 0; k < n; k++)
                rg_uses[ops[k]]++;
            if (f->ins[v].op == OP_CALL)
                rg_uses[f->ins[v].c]++;
        }
        if (f->blk[b].arg)
            rg_uses[f->blk[b].arg]++;
    }
    for (b = 1; b < f->nblk; b++) {
        for (v = f->blk[b].first; v; v = nx) {
            Ins *i = &f->ins[v];
            u32 lv[RG_MAX + 1], out[RG_MAX], in[RG_MAX], nout = 0, nin = 0, nl, j, x, y, acc;
            int op = i->op, isand = op == OP_AND, users_same = 0;
            nx = i->next;
            if ((op != OP_OR && op != OP_AND) || i->ty != IT_I32)
                continue;
            /* only roots: not itself one link of a larger chain of the same operation in this block */
            for (x = i->next; x && !users_same; x = f->ins[x].next)
                if (f->ins[x].op == op && (f->ins[x].a == v || f->ins[x].b == v) && rg_uses[v] == 1)
                    users_same = 1;
            if (users_same)
                continue;
            nl = (u32)rg_collect(v, op, b, lv, 0, 1);
            if (nl < 3 || nl > RG_MAX)
                continue;
            for (j = 0; j < nl; j++)
                if (!ir_is_bool(f, lv[j], 3))
                    break;
            if (j < nl)
                continue;
            /* pairs of bounds of one value: one unsigned comparison */
            for (x = 0; x < nl; x++) {
                int lx, ly;
                i64 kx = 0, ky = 0;
                u32 vx, vy;
                if (!lv[x] || !(vx = rg_bound(lv[x], isand, &lx, &kx)))
                    continue;
                for (y = 0; y < nl; y++) {
                    if (y == x || !lv[y] || !(vy = rg_bound(lv[y], isand, &ly, &ky)) || vy != vx || lx == ly)
                        continue;
                    {
                        i64 kk = lx ? ky : kx;
                        u32 c = ir_raw(f, isand ? OP_ULT : OP_UGE, IT_I32, vx, ir_const(f, f->ins[vx].ty, (u64)kk));
                        ir_insert_before(f, v, c);
                        lv[x] = c;
                        lv[y] = 0;
                        changed = 1;
                    }
                    break;
                }
            }
            for (j = 0; j < nl; j++) {
                if (!lv[j])
                    continue;
                if (f->ins[lv[j]].blk != b && f->ins[lv[j]].op != OP_CONST)
                    out[nout++] = lv[j];
                else
                    in[nin++] = lv[j];
            }
            if (nout < 2 && !changed)
                continue;
            if (nout >= 2 || (nout && nin))
                changed = 1;
            acc = 0;
            for (j = 0; j < nout; j++) {
                if (!acc) {
                    acc = out[j];
                    continue;
                }
                x = ir_raw(f, op, IT_I32, acc, out[j]);
                ir_insert_before(f, v, x);
                acc = x;
            }
            for (j = 0; j < nin; j++) {
                if (!acc) {
                    acc = in[j];
                    continue;
                }
                x = ir_raw(f, op, IT_I32, acc, in[j]);
                ir_insert_before(f, v, x);
                acc = x;
            }
            ir_replace(f, v, acc);
        }
    }
    free(rg_uses);
    if (changed) {
        ir_resolve_all(f);
        opt_dce(f);
    }
    return changed;
}

/* x86-64 host: a pure math call (IF_PURE) neither writes memory nor stops a load's hoisting, and one of loop-invariant
   arguments that every trip makes goes before the loop (clang: readnone calls). MCC_NOPURELICM: off */
static int opt_nopurelicm = -1;

static int licm(void)
{
    u32 *mark, *stack, k, j, p, v, nx, sp, nblk0 = f->nblk;
    int any_replaced = 0;
    if (opt_nopurelicm < 0)
        opt_nopurelicm = getenv("MCC_NOPURELICM") != 0;
    compute_dom();
    mark = (u32 *)xmalloc(cap_blocks * 2 * sizeof(u32));
    stack = mark + cap_blocks;
    memset(mark, 0, cap_blocks * sizeof(u32));

    /* inner loops first: their headers come later in reverse postorder */
    for (k = nrpo; k-- > 0;) {
        u32 h = rpo[k], pre = 0, noutside = 0;
        u32 store_addr[16], nstores = 0, deref[24], nderef = 0, exiting[8], nexiting = 0;
        u8 store_mem[16], deref_size[24];
        u32 hoisted[32], nhoisted = 0;
        int clobbers_all = 0, replaced = 0;
        Block *hb = &f->blk[h];

        sp = 0;
        for (p = 0; p < hb->npreds; p++) {
            u32 q = hb->preds[p];
            if (q < cap_blocks && rpo_num[q] != 0xffffffffU && dominates(h, q))
                stack[sp++] = q;
        }
        if (!sp)
            continue;
        mark[h] = h;
        while (sp) {
            u32 x = stack[--sp];
            if (mark[x] == h)
                continue;
            mark[x] = h;
            for (p = 0; p < f->blk[x].npreds; p++)
                if (sp < cap_blocks)
                    stack[sp++] = f->blk[x].preds[p];
        }
        for (p = 0; p < hb->npreds; p++) {
            if (mark[hb->preds[p]] != h) {
                pre = hb->preds[p];
                noutside++;
            }
        }
        if (noutside == 0)
            continue;
        if (noutside > 1) {
            /* entered from several places: they meet in a new preheader,
               which takes over the entry values of the header's phis */
            u32 n, w, first;
            if (f->nblk + 1 >= cap_blocks)
                continue;
            n = ir_new_block(f);
            hb = &f->blk[h];
            f->blk[n].term = TERM_JMP;
            f->blk[n].succ[0] = h;
            for (p = 0; p < hb->npreds; p++) {
                u32 q = hb->preds[p], s;
                if (mark[q] == h)
                    continue;
                ir_add_pred(f, n, q);
                for (s = 0; s < 2; s++)
                    if (f->blk[q].succ[s] == h && (s == 0 || f->blk[q].term == TERM_BR))
                        f->blk[q].succ[s] = n;
            }
            for (v = hb->phis; v; v = f->ins[v].next) {
                u32 same = 0, merged, j2 = 0;
                int differ = 0;
                for (p = 0; p < hb->npreds; p++) {
                    u32 a = f->xargs[f->ins[v].a + p];
                    if (mark[hb->preds[p]] == h)
                        continue;
                    if (same && a != same)
                        differ = 1;
                    same = a;
                }
                merged = same;
                if (differ) {
                    merged = ir_phi(f, n, f->ins[v].ty);
                    for (p = 0; p < hb->npreds; p++)
                        if (mark[hb->preds[p]] != h)
                            f->xargs[f->ins[merged].a + j2++] = f->xargs[f->ins[v].a + p];
                }
                w = 0;
                first = 1;
                for (p = 0; p < hb->npreds; p++) {
                    u32 *args = &f->xargs[f->ins[v].a];
                    if (mark[hb->preds[p]] == h) {
                        args[w++] = args[p];
                    } else if (first) {
                        args[w++] = merged;
                        first = 0;
                    }
                }
                f->ins[v].b = w;
            }
            w = 0;
            first = 1;
            for (p = 0; p < hb->npreds; p++) {
                if (mark[hb->preds[p]] == h) {
                    hb->preds[w++] = hb->preds[p];
                } else if (first) {
                    hb->preds[w++] = n;
                    first = 0;
                }
            }
            hb->npreds = w;
            mark[n] = 0;
            rpo_num[n] = 0xffffffffU;
            preheader_freq(n, h);
            pre = n;
        } else if (f->blk[pre].term != TERM_JMP) {
            /* give the loop a preheader of its own */
            u32 n, s;
            if (f->nblk + 1 >= cap_blocks)
                continue;
            n = ir_new_block(f);
            hb = &f->blk[h];
            f->blk[n].term = TERM_JMP;
            f->blk[n].succ[0] = h;
            ir_add_pred(f, n, pre);
            hb->preds[pred_index(h, pre)] = n;
            for (s = 0; s < 2; s++)
                if (f->blk[pre].succ[s] == h)
                    f->blk[pre].succ[s] = n;
            mark[n] = 0;
            rpo_num[n] = 0xffffffffU;
            preheader_freq(n, h);
            pre = n;
        }
        /* what does the loop write? */
        for (j = k; j < nrpo; j++) {
            u32 b = rpo[j];
            if (mark[b] != h)
                continue;
            {
                /* blocks that can leave the loop */
                Block *xb = &f->blk[b];
                u32 ns = nsuccs(xb), s;
                int leaves = xb->term == TERM_RET;
                for (s = 0; s < ns; s++)
                    if (mark[xb->succ[s]] != h)
                        leaves = 1;
                if (leaves) {
                    if (nexiting < 8)
                        exiting[nexiting] = b;
                    nexiting++;
                }
            }
            for (v = f->blk[b].first; v; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                if ((i->op == OP_CALL && !(i->flags & IF_PURE && !opt_nopurelicm)) || i->op == OP_MEMCPY || i->op == OP_MEMZERO ||
                    i->op == OP_VSTORE || (i->op == OP_XI && (i->flags & XI_EFFECT))) {
                    clobbers_all = 1;
                } else if (i->op == OP_STORE) {
                    if (nstores == 16) {
                        clobbers_all = 1;
                    } else {
                        store_addr[nstores] = i->a;
                        store_mem[nstores++] = i->mem;
                    }
                }
            }
        }
        /* addresses the code just before the loop reads or writes: loads from
           them cannot fault */
        if (!clobbers_all) {
            u32 q = pre;
            for (j = 0; j < 2 && q; j++) {
                for (v = f->blk[q].last; v; v = f->ins[v].prev) {
                    Ins *i = &f->ins[v];
                    if (i->op == OP_CALL)
                        break;
                    if ((i->op == OP_LOAD || i->op == OP_STORE) && nderef < 24) {
                        deref[nderef] = i->a;
                        deref_size[nderef++] = (u8)mem_size(i->mem);
                    }
                }
                q = v || f->blk[q].npreds != 1 ? 0 : f->blk[q].preds[0];
            }
        }
        /* the loop's blocks, then preheaders made for the loops inside it */
        for (j = k; j < nrpo + (f->nblk - nblk0); j++) {
            u32 b = j < nrpo ? rpo[j] : nblk0 + (j - nrpo);
            int always = j < nrpo && nexiting >= 1 && nexiting <= 8; /* does b run in every trip around the loop? */
            if (b == pre || mark[b] != h)
                continue;
            for (p = 0; always && p < nexiting; p++)
                if (!dominates(b, exiting[p]))
                    always = 0;
            for (v = f->blk[b].first; v; v = nx) {
                Ins *i = &f->ins[v];
                int ok;
                nx = i->next;
                if (replaced) {
                    u32 n = ir_opcount(f, v), q;
                    for (q = 0; q < n; q++)
                        *ir_opref(f, v, q) = ir_resolve(f, *ir_opref(f, v, q));
                }
                if (i->op == OP_PIN) {
                    ok = 1;
                } else if (is_pure(i->op)) {
                    ok = defined_outside(i->a, mark, h) && (ir_opcount(f, v) < 2 || defined_outside(i->b, mark, h));
                    if (ok && i->op == OP_SELECT)
                        ok = defined_outside(i->c, mark, h);
                    if (ok && opt_dev_fn && opt_dolicm >= 2) {
                        /* device code: what costs nothing where it is stays there, and does not hold a register
                           through the loop: a constant added (it goes into an address), and what is made of
                           constants alone */
                        u64 cv;
                        if ((i->op == OP_ADD || i->op == OP_SUB) && ir_is_const(f, i->b, &cv))
                            ok = 0;
                        else if (!f->ins[i->a].blk && (ir_opcount(f, v) < 2 || !f->ins[i->b].blk))
                            ok = 0;
                    }
                } else if (i->op == OP_CALL && (i->flags & IF_PURE) && !opt_nopurelicm && always) {
                    /* a math function of loop-invariant arguments in a block every trip runs (so it is not
                       made speculative: the loop's first trip would have called it) */
                    u32 n = ir_opcount(f, v), q;
                    ok = 1;
                    for (q = 0; ok && q < n; q++)
                        ok = defined_outside(*ir_opref(f, v, q), mark, h);
                } else if (i->op == OP_LOAD && !clobbers_all) {
                    /* only loads that cannot fault where the loop would not have:
                       fixed stack or global addresses, addresses used just
                       before the loop, and loads that every trip executes */
                    int aop = f->ins[i->a].op;
                    ok = aop == OP_SLOT || aop == OP_ADDR || always;
                    for (p = 0; !ok && p < nderef; p++)
                        ok = deref[p] == i->a && mem_size(i->mem) <= deref_size[p];
                    if (ok)
                        ok = defined_outside(i->a, mark, h);
                    for (p = 0; ok && p < nstores; p++)
                        if (may_alias(store_addr[p], store_mem[p], i->a, i->mem))
                            ok = 0;
                } else {
                    ok = 0;
                }
                if (ok && i->op == OP_LOAD) {
                    /* the same load may have come out of another block */
                    for (p = 0; p < nhoisted; p++) {
                        Ins *o = &f->ins[hoisted[p]];
                        if (o->a == i->a && o->mem == i->mem && o->ty == i->ty)
                            break;
                    }
                    if (p < nhoisted) {
                        ir_replace(f, v, hoisted[p]);
                        replaced = 1;
                        any_replaced = 1;
                        continue;
                    }
                    if (nhoisted < 32)
                        hoisted[nhoisted++] = v;
                }
                if (ok) {
                    ir_remove(f, v);
                    ir_append(f, pre, v);
                }
            }
        }
        /* addresses and constants that take more than one instruction to
           produce are put in registers before the loop */
        {
            u32 pin_of[6], pin_v[6], npins = 0, maxpins = clobbers_all ? 2 : 6;
            for (v = f->blk[pre].first; v; v = f->ins[v].next) {
                if (f->ins[v].op == OP_PIN && npins < maxpins) {
                    pin_of[npins] = f->ins[v].a;
                    pin_v[npins++] = v;
                }
            }
            for (j = k; j < nrpo + (f->nblk - nblk0); j++) {
                u32 b = j < nrpo ? rpo[j] : nblk0 + (j - nrpo);
                if (b == pre || mark[b] != h)
                    continue;
                for (v = f->blk[b].first; v; v = f->ins[v].next) {
                    int op = f->ins[v].op;
                    u32 n = ir_opcount(f, v), q;
                    if (op == OP_PIN || op == OP_CALL || op == OP_TLS)
                        continue; /* (the operand of OP_TLS names a symbol: it is never in a register) */
                    for (q = (op == OP_LOAD || op == OP_STORE) ? 1 : 0; q < n; q++) {
                        u32 a = *ir_opref(f, v, q), pin = 0;
                        Ins *ai = &f->ins[a];
                        u64 c = (u64)ai->a | ((u64)ai->b << 32);
                        int want = ai->op == OP_ADDR || (ai->op == OP_FCONST && c != 0) ||
                                   (ai->op == OP_CONST && c > 0xffff && (i64)c > 0 &&
                                    (op == OP_MUL || op == OP_SDIV || op == OP_UDIV || op == OP_SREM || op == OP_UREM));
                        if (!want)
                            continue;
                        for (p = 0; p < npins; p++)
                            if (pin_of[p] == a)
                                pin = pin_v[p];
                        if (!pin) {
                            if (npins == maxpins)
                                continue;
                            pin = ir_raw(f, OP_PIN, ai->ty, a, 0);
                            ir_append(f, pre, pin);
                            pin_of[npins] = a;
                            pin_v[npins++] = pin;
                        }
                        *ir_opref(f, v, q) = pin;
                    }
                }
            }
        }
    }
    free(mark);
    return any_replaced;
}

/* Debugging (MCC_SLOTCHECK): no instruction may take the address of a local that was given up. */
static void slot_check(Func *fn, const char *when)
{
    u32 b, v, k, n, *ops;
    for (b = 1; b < fn->nblk; b++) {
        int phis;
        for (phis = 1; phis >= 0; phis--)
            for (v = phis ? fn->blk[b].phis : fn->blk[b].first; v; v = fn->ins[v].next) {
                Ins *i = &fn->ins[v];
                IR_OPS(fn, i, ops, n);
                for (k = 0; k < n; k++) {
                    Ins *o = &fn->ins[ir_resolve(fn, ops[k])];
                    if (o->op == OP_SLOT && !fn->slots[o->a].size && !((i->op == OP_MEMZERO || i->op == OP_MEMCPY) && !i->c))
                        die("slot check (%s): op %d (c %u, operand %u of %u) reads the address of slot %u (+%d), which has no size", when,
                            (int)i->op, (unsigned)i->c, (unsigned)k, (unsigned)n, (unsigned)o->a, (int)(i32)o->b);
                }
            }
    }
}

static void run_passes(Func *fn);
int opt_noparamfwd; /* MCC_NOPARAMFWD: debugging */

/* (what the passes make has no source location of its own: the line table carries on with the one before) */
void opt_run(Func *fn)
{
    u32 saved = ir_loc;
    ir_loc = 0;
    run_passes(fn);
    ir_loc = saved;
}

static void finish_passes(Func *fn);
static int fuse_sincos(void);

void opt_finish(Func *fn)
{
    u32 saved = ir_loc;
    ir_loc = 0;
    finish_passes(fn);
    ir_loc = saved;
}

static void run_passes(Func *fn)
{
    int round;
    f = fn;
    if (opt_level == 0 && !opt_cuda && !opt_dev_fn) {
        /* -O0 (host): as written, the variables in their stack slots (for a debugger), nothing inlined or moved */
        ir_resolve_all(f);
        opt_dce(f);
        return;
    }
    /* x86-64 too: records and arrays of a few scalars, and the locals of memcpy type puns, become values */
    if ((opt_cuda || (opt_arch == ARCH_X64 && opt_level >= 1)) && !opt_nosplit) {
        opt_split(f);
        if (opt_slotcheck)
            slot_check(fn, "first split");
    }
    opt_promote(f);
    if (opt_slotcheck)
        slot_check(fn, "first promote");
    if (inline_calls(f)) {
        f->copies = 1;
        ir_resolve_all(f);
    }
    f = fn;
    gvn();
    if (opt_cuda || (opt_arch == ARCH_X64 && opt_level >= 1 && !opt_nosplit)) {
        /* arrays handed to functions that are now part of this one (x86-64: a decoder's state record whose
           functions were inlined, its fields then values) */
        ir_resolve_all(f);
        /* ... and again while it brings something: an address read back from a record that was split (what
           a lambda captured by reference) is then seen to be that of a local, which can now be a value too */
        for (round = 0; round < 4 && !opt_nosplit; round++) {
            int split = opt_split(f);
            if (opt_slotcheck)
                slot_check(fn, "second split");
            if (!split && opt_norounds)
                break;
            opt_promote(f);
            if (opt_slotcheck)
                slot_check(fn, "second promote");
            if (!split && !opt_promoted)
                break;
            f->copies = 1;
            ir_resolve_all(f);
            f = fn;
            gvn();
            ir_resolve_all(f);
            if (opt_slotcheck)
                slot_check(fn, "gvn after split");
            if (opt_norounds)
                break;
        }
    }
    if (opt_dev_fn && ptx_single_trip(f))
        ir_resolve_all(f);
    if (simplify_cfg()) {
        int rounds = opt_level >= 3 ? 6 : 1;
        do {
            ir_resolve_all(f);
            gvn();
        } while (simplify_cfg() && --rounds);
        ir_resolve_all(f);
    }
    /* device code: the GPU's assembler moves what it finds worth moving; values moved out of a kernel's loops here
       stay in registers through all of it, and it runs out of them */
    if (opt_dev_fn && opt_dolicm >= 3 && dev_reassoc()) {
        opt_dce(f);
        gvn();
        ir_resolve_all(f);
    }
    if (!(opt_dev_fn && !opt_dolicm) && !(opt_cuda && opt_nolicm) && licm())
        ir_resolve_all(f);
    /* x86-64: the tests of an || / && chain that the loop does not change, now outside it, grouped, so the group
       leaves the loop too */
    if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && opt_level >= 2 && !getenv("MCC_NOREGROUP")) {
        int r;
        for (r = 0; r < 3 && bool_regroup(); r++) /* one loop level per round (licm reports only replacements) */
            if (licm())
                ir_resolve_all(f);
    }
    if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && opt_level >= 2 && !opt_size && opt_glibc && opt_obj && !getenv("MCC_NOSINCOS") &&
        fuse_sincos())
        ir_resolve_all(f);
    if (opt_level >= 3 && !opt_size && !opt_novec && (opt_dce(f), opt_vectorise(f))) { /* dead code would only be in the way */
        opt_dce(f);
        return; /* vector code stays out of the inliner: the scalar passes do not know it */
    }
    opt_dce(f);
    if (opt_slotcheck)
        slot_check(fn, "end");
    inline_save(f);
}

/* x86-64 host, the GNU C library: sin (x) and cos (x) of one x are one call of sincos (x, &s, &c), which computes the
   two from one reduction of x (clang does the same). The pair: pure calls (IF_PURE) of the same width on the same value, the
   first dominating the second; the call goes where the first was, the two results through a stack slot (glibc 2.27 or
   newer: the call is by the sincosf64 / sincosf32 alias). ufsm's
   surflabels / patchid / score_par called both side by side (cos 10.9 + sin 7.1 G cycles where clang's sincos took 12.5).
   MCC_NOSINCOS: off. */
int opt_glibc;
#define SINCOS_MAX 64
static int fuse_sincos(void)
{
    u32 b, v, k, j, nc = 0, cv[SINCOS_MAX], ca[SINCOS_MAX], cpos[SINCOS_MAX], pos = 0;
    u8 ck[SINCOS_MAX]; /* bit 0: cos, bit 1: float */
    int any = 0;
    static u32 nm[4];
    static const char *const names[4] = {"sin", "cos", "sinf", "cosf"};
    if (!nm[0])
        for (k = 0; k < 4; k++)
            nm[k] = intern_cstr(names[k]);
    compute_dom();
    for (k = 0; k < nrpo && nc < SINCOS_MAX; k++) {
        b = rpo[k];
        for (v = f->blk[b].first; v && nc < SINCOS_MAX; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            Ins *ce;
            u32 q, name;
            pos++;
            if (i->op != OP_CALL || !(i->flags & IF_PURE) || i->b != 1)
                continue;
            ce = &f->ins[ir_resolve(f, i->c)];
            if (ce->op != OP_ADDR || ce->b || !opt_obj->syms[ce->a].name)
                continue;
            name = opt_obj->syms[ce->a].name;
            for (q = 0; q < 4 && nm[q] != name; q++)
                ;
            if (q == 4 || i->ty != (q >= 2 ? IT_F32 : IT_F64))
                continue;
            cv[nc] = v;
            ca[nc] = ir_resolve(f, f->xargs[i->a]);
            ck[nc] = (u8)q;
            cpos[nc++] = pos; /* (in reverse postorder, so within a block the earlier first) */
        }
    }
    for (k = 0; k < nc; k++) {
        u32 a = cv[k], fb;
        if (!a)
            continue;
        for (j = k + 1; j < nc; j++) {
            u32 bv = cv[j], first, sv, cvv, slot, sz, call, ls, lc, args[3], sym, callee;
            const char *fname;
            int fl = ck[k] >> 1;
            if (!bv || ca[j] != ca[k] || ck[j] != (ck[k] ^ 1))
                continue;
            fb = f->ins[a].blk;
            if (fb == f->ins[bv].blk ? cpos[k] > cpos[j] : !dominates(fb, f->ins[bv].blk))
                continue;
            first = a;
            sv = ck[k] & 1 ? bv : a;
            cvv = ck[k] & 1 ? a : bv;
            sz = fl ? 4 : 8;
            fname = fl ? "sincosf32" : "sincosf64"; /* (glibc 2.27's aliases of sincosf / sincos: a name of the C library's own
                                                     that no program defines, where a static sincos of the unit would be called) */
            sym = obj_named_sym(opt_obj, intern_cstr(fname));
            opt_obj->syms[sym].global = 1;
            callee = ir_addr(f, sym, 0);
            slot = ir_new_slot(f, 2 * sz, sz);
            args[0] = ca[k];
            args[1] = ir_slot_addr(f, slot, 0);
            args[2] = ir_slot_addr(f, slot, sz);
            {
                u32 saved = f->cur;
                f->cur = 0;
                call = ir_call(f, IT_VOID, callee, args, 3, 0xffffffffU, 0);
                f->cur = saved;
            }
            ir_insert_before(f, first, call);
            ls = ir_raw(f, OP_LOAD, fl ? IT_F32 : IT_F64, args[1], 0);
            f->ins[ls].mem = fl ? M_F32 : M_F64;
            ir_insert_before(f, first, ls);
            lc = ir_raw(f, OP_LOAD, fl ? IT_F32 : IT_F64, args[2], 0);
            f->ins[lc].mem = fl ? M_F32 : M_F64;
            ir_insert_before(f, first, lc);
            ir_replace(f, sv, ls);
            ir_replace(f, cvv, lc);
            cv[k] = cv[j] = 0;
            any = 1;
            break;
        }
    }
    return any;
}

static void finish_passes(Func *fn)
{
    /* what is still not known to be constant is taken not to be */
    u32 v, n = 0;
    for (v = 1; v < fn->nins; v++)
        if (fn->ins[v].op == OP_ISCONST && fn->ins[v].blk) {
            ir_replace(fn, v, ir_const(fn, IT_I32, 0));
            n++;
        }
    if (!n)
        return;
    f = fn;
    ir_resolve_all(f);
    gvn();
    if (simplify_cfg()) {
        ir_resolve_all(f);
        gvn();
    }
    ir_resolve_all(f);
    opt_dce(f);
}
