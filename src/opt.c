/* The optimisation pipeline: value numbering with folding and memory
   forwarding, and control-flow simplification. Every pass is near linear. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x == k ? c : e (or x != k ? e : c) where e is c when x is k: the value e, 0 if the select v is not one.
   n == 32 ? 0xffffffff : (1 << n) - 1, in 64 bits, is a bit reader's mask. */
static u32 select_same(Func *f, u32 v)
{
    Ins *i = &f->ins[v], *ci = &f->ins[i->a];
    u32 arm, other;
    u64 kk, want, got;
    if ((i->ty != IT_I32 && i->ty != IT_I64) || (ci->op != OP_EQ && ci->op != OP_NE) || !ir_is_const(f, ci->b, &kk) ||
        getenv("MCC_NOSELSAME"))
        return 0;
    arm = ci->op == OP_EQ ? i->b : i->c;
    other = ci->op == OP_EQ ? i->c : i->b;
    if (!ir_is_const(f, arm, &want) || ir_is_const(f, other, &got) ||
        !ir_eval_with(f, other, ir_resolve(f, ci->a), kk, 5, &got))
        return 0;
    return (i->ty == IT_I64 ? got == want : (u32)got == (u32)want) ? other : 0;
}

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

/* ---- which object an address is in (x86-64 host; MCC_NOALIASROOT: off) ----
   The root of an address is what it was computed from by adding and subtracting: a local (OP_SLOT), a global object
   (OP_ADDR), a pointer parameter (OP_PARAM with PARAM_PTR), or the result of an allocation (OP_CALL with IF_FRESH).
   Anything else (a pointer read from memory, a phi, a call's result) is its own root, of which nothing is known. Two
   addresses with different known roots are in different objects, except that a parameter may point into a global
   object or where another parameter points, unless it is declared restrict. An address of unknown root can be in a
   local, an allocation or what a restrict parameter points to only if that one's address has escaped: was stored,
   passed to a call, merged in a phi or a select, returned, or used in arithmetic other than the above. (ufsm fit_vox2:
   the stores to its local arrays and through its ic / gv parameters no longer make it read f->delta, f->h, f->lx again;
   LLVM's BasicAA reasons the same way from allocas, globals, noalias arguments and captures.) */
static u32 *al_root;    /* by value, for those that were there when alias_prepare ran: its root, 0 = not yet worked out */
static u8 *al_esc;      /* by value: the address this parameter or allocation gives has escaped */
static u8 *al_slot_esc; /* by slot */
static u32 al_n, al_nslots, al_cap, al_slotcap;
static int al_on, al_deep;
static int opt_noaliasroot = -1;

/* Are roots worked out in this compilation? (parse.c marks the pointer parameters and the allocations for it.
   MCC_ALIASROOT: on other machines than x86-64 too, for testing.) */
int opt_alias_roots(void)
{
    if (opt_noaliasroot < 0)
        opt_noaliasroot = getenv("MCC_NOALIASROOT") != 0 || (opt_arch != ARCH_X64 && !getenv("MCC_ALIASROOT"));
    return !opt_noaliasroot;
}

static int root_known(u32 r)
{
    const Ins *i = &f->ins[r];
    switch (i->op) {
    case OP_SLOT: case OP_ADDR:
        return 1;
    case OP_PARAM:
        return (i->flags & PARAM_PTR) != 0;
    case OP_CALL:
        return (i->flags & IF_FRESH) != 0;
    }
    return 0;
}

static u32 root_of(u32 v, int depth)
{
    const Ins *i;
    u32 r, ra, rb;
    int ka, kb;
    if (v < al_n && al_root[v])
        return al_root[v];
    i = &f->ins[v];
    r = v;
    if (depth > 1000) {
        al_deep = 1; /* (a chain this long is not followed: nothing is concluded from roots in this function) */
    } else if (i->op == OP_COPY || i->op == OP_PIN) {
        r = root_of(i->a, depth + 1);
    } else if ((i->op == OP_ADD || i->op == OP_SUB) && i->ty == IT_I64) {
        /* the pointer is the operand whose root is known (the other is an index, or a pointer of which nothing
           is known: then so is the sum) */
        ra = root_of(i->a, depth + 1);
        rb = root_of(i->b, depth + 1);
        ka = root_known(ra);
        kb = root_known(rb);
        if (ka && !kb)
            r = ra;
        else if (kb && !ka && i->op == OP_ADD)
            r = rb;
    }
    if (v < al_n)
        al_root[v] = r;
    return r;
}

static void root_escapes(u32 v)
{
    u32 r = root_of(v, 0);
    const Ins *i = &f->ins[r];
    if (i->op == OP_SLOT) {
        if (i->a < al_nslots)
            al_slot_esc[i->a] = 1;
    } else if (r < al_n) {
        al_esc[r] = 1;
    }
}

/* Before a pass that asks may_alias: the roots, and which of them escape. */
static void alias_prepare(void)
{
    u32 b, v, k, n;
    al_on = 0;
    al_n = 0;
    if (!opt_alias_roots() || opt_dev_fn || opt_cuda || opt_level < 2)
        return;
    if (al_cap < f->nins) {
        al_cap = f->nins * 2;
        al_root = (u32 *)xrealloc(al_root, al_cap * sizeof(u32));
        al_esc = (u8 *)xrealloc(al_esc, al_cap);
    }
    if (al_slotcap < f->nslots + 1) {
        al_slotcap = f->nslots * 2 + 16;
        al_slot_esc = (u8 *)xrealloc(al_slot_esc, al_slotcap);
    }
    memset(al_root, 0, f->nins * sizeof(u32));
    memset(al_esc, 0, f->nins);
    memset(al_slot_esc, 0, f->nslots + 1);
    al_n = f->nins;
    al_nslots = f->nslots;
    al_deep = 0;
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        for (v = bb->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                root_escapes(f->xargs[f->ins[v].a + k]);
        for (v = bb->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            switch (i->op) {
            case OP_LOAD: case OP_MEMCPY: case OP_MEMZERO: case OP_VLOAD:
            case OP_EQ: case OP_NE: case OP_SLT: case OP_SLE: case OP_SGT: case OP_SGE:
            case OP_ULT: case OP_ULE: case OP_UGT: case OP_UGE:
                continue; /* addresses read through, written through or compared */
            case OP_STORE:
                root_escapes(i->b);
                continue;
            case OP_VSTORE:
                root_escapes(i->b);
                continue;
            case OP_COPY: case OP_PIN:
                continue;
            case OP_ADD: case OP_SUB:
                if (i->ty == IT_I64) {
                    u32 ra = root_of(i->a, 0), rb = root_of(i->b, 0);
                    if (root_of(v, 0) != v)
                        continue; /* an address in the same object */
                    if (i->op == OP_SUB && root_known(ra) && root_known(rb))
                        continue; /* the distance between two addresses is not an address */
                }
                break;
            }
            n = ir_opcount(f, v);
            for (k = 0; k < n; k++)
                root_escapes(*ir_opref(f, v, k));
        }
        if (bb->term == TERM_RET && bb->arg)
            root_escapes(bb->arg);
    }
    al_on = !al_deep;
}

static int opt_norestrict = -1;

/* 1: the address of root r is in an object that only addresses with this root reach. */
static int root_private(u32 r)
{
    const Ins *i = &f->ins[r];
    switch (i->op) {
    case OP_SLOT:
        return i->a < al_nslots && !al_slot_esc[i->a];
    case OP_PARAM:
        if (opt_norestrict < 0)
            opt_norestrict = getenv("MCC_NORESTRICT") != 0; /* restrict (and a record passed by value) says nothing */
        return (i->flags & PARAM_RESTRICT) && !opt_norestrict && r < al_n && !al_esc[r];
    case OP_CALL:
        return (i->flags & IF_FRESH) && r < al_n && !al_esc[r];
    }
    return 0;
}

/* 1: root r is a static object whose address is in no pointer (parse.c works that out for the whole unit before its
   functions are generated). Unlike root_private, a call may well write it: only pointers do not reach it. */
static int root_static(u32 r)
{
    const Ins *i = &f->ins[r];
    return i->op == OP_ADDR && opt_sym_private && i->a < opt_sym_private_n && opt_sym_private[i->a];
}

/* 1: addresses a and b are known to be in different objects. */
static int roots_apart(u32 a, u32 b)
{
    u32 ra, rb;
    const Ins *ia, *ib;
    int ka, kb;
    if (!al_on)
        return 0;
    ra = root_of(a, 0);
    rb = root_of(b, 0);
    if (al_deep) {
        al_on = 0;
        return 0;
    }
    if (ra == rb)
        return 0;
    ka = root_known(ra);
    kb = root_known(rb);
    if (!ka && !kb)
        return 0;
    if (!ka || !kb)
        return root_private(ka ? ra : rb) || root_static(ka ? ra : rb);
    ia = &f->ins[ra];
    ib = &f->ins[rb];
    if (ia->op == ib->op && (ia->op == OP_SLOT || ia->op == OP_ADDR))
        return ia->a != ib->a; /* (the same local or global at another offset is another value) */
    if ((ia->op == OP_PARAM && (ib->op == OP_PARAM || ib->op == OP_ADDR)) || (ib->op == OP_PARAM && ia->op == OP_ADDR))
        return root_private(ra) || root_private(rb) || root_static(ra) || root_static(rb); /* (root_private of a global is 0) */
    return 1; /* a local, a global, what a parameter points to, an allocation: no two of different kinds share
                 memory, nor do two allocations */
}

/* A call that is not known to leave memory alone may have written wherever an address that escaped points. */
static int call_spares(u32 addr)
{
    u32 r;
    if (!al_on)
        return 0;
    r = root_of(addr, 0);
    return !al_deep && root_private(r);
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
    if (roots_apart(a, b))
        return 0;
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

/* After a call: what is known of memory the callee cannot reach stays known. */
/* After a call of a library function that writes nothing but errno (IF_NOWRITE): what is in a local or a named
   global other than errno itself is as it was. (ufsm's wind fixture: a getenv between two hypot calls of the same
   sums of a global made the second one a call again: 1.5 times clang's instructions in hypot and atan2.)
   MCC_NONOWRITE: such a call is like any other. */
static int opt_nonowrite = -1;

static int call_nowrite(const Ins *i)
{
    if (opt_nonowrite < 0)
        opt_nonowrite = getenv("MCC_NONOWRITE") != 0;
    return al_on && !opt_nonowrite && (i->flags & (IF_NOWRITE | IF_SRET | IF_ASM | IF_ATOMIC | IF_ALLOCA)) == IF_NOWRITE;
}

static int nowrite_spares(u32 addr)
{
    const Ins *r = &f->ins[root_of(addr, 0)];
    if (r->op == OP_SLOT)
        return 1;
    return r->op == OP_ADDR && opt_obj->syms[r->a].name && strcmp(intern_str(opt_obj->syms[r->a].name), "errno") != 0;
}

static void mem_kill_nowrite(void)
{
    u32 k, n = 0;
    for (k = 0; k < nmem; k++)
        if (nowrite_spares(mem_tab[k].addr))
            mem_tab[n++] = mem_tab[k];
    nmem = n;
}

static void mem_kill_call(void)
{
    u32 k, n = 0;
    for (k = 0; k < nmem; k++)
        if (call_spares(mem_tab[k].addr))
            mem_tab[n++] = mem_tab[k];
    nmem = n;
}

/* After a write of some length at addr (a block copied or cleared, a vector stored): what is in other objects stays
   known. */
static void mem_kill_object(u32 addr)
{
    u32 k, n = 0;
    for (k = 0; k < nmem; k++)
        if (roots_apart(mem_tab[k].addr, addr))
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

/* x86-64 host (with roots; MCC_NOMEMLOOP: off): the state of memory on entry to the head b of a loop, the k-th block in
   rpo. What the ways in from outside agree on is still so at every trip unless the loop writes there: its stores are
   looked at one by one, its calls spare what they cannot reach (call_spares). So a load inside a loop, or after it,
   of what was read or written before it is that value, even where licm may not move the load (one that not every
   trip runs). Without this a loop head started from nothing. (MCC_MEMLOOPALL: also values the loop does not use
   otherwise, see the end of mem_enter_loop; for measuring.) */
static int opt_nomemloop = -1;
static u32 *lp_mark, *lp_stack, lp_cap, lp_stamp;

/* mem_enter_loop: the entries (and their marks in used) that a store at addr (how = 0), a call (1) or a write of some
   length at addr (2) leaves known. */
static void mem_kill_used(u32 addr, int mem, int how, u8 *used)
{
    u32 j, n = 0;
    for (j = 0; j < nmem; j++) {
        const MemEntry *e = &mem_tab[j];
        if (how == 0 ? may_alias(e->addr, e->mem, addr, mem) : how == 1 ? !call_spares(e->addr) : how == 3 ? !nowrite_spares(e->addr) : !roots_apart(e->addr, addr))
            continue;
        used[n] = used[j];
        mem_tab[n++] = *e;
    }
    nmem = n;
}

static void mem_enter_loop(u32 b, u32 k)
{
    Block *bb = &f->blk[b];
    u32 p, j, q = 0, sp = 0, nin = 0, v;
    u8 used[MEM_CAP];
    if (opt_nomemloop < 0)
        opt_nomemloop = getenv("MCC_NOMEMLOOP") != 0 ? 1 : getenv("MCC_MEMLOOPALL") != 0 ? 2 : 0;
    if (opt_nomemloop == 1 || !al_on)
        return;
    for (p = 0; p < bb->npreds; p++) {
        u32 r = rpo_num[bb->preds[p]];
        if (r == 0xffffffffU)
            return;
        if (r >= k) {
            if (!dominates(b, bb->preds[p]))
                return; /* (entered in the middle: not a loop with one head) */
        } else if (!nin++) {
            q = r;
        }
    }
    if (!nin || !out_n[q])
        return;
    /* what all the ways in agree on (no phi is made here: the ways back have no state yet) */
    for (j = 0; j < out_n[q]; j++) {
        MemEntry e = out_tab[(size_t)q * MEM_CAP + j];
        for (p = 0; p < bb->npreds; p++) {
            MemEntry *o;
            if (rpo_num[bb->preds[p]] >= k || rpo_num[bb->preds[p]] == q)
                continue;
            o = out_find(bb->preds[p], e.addr, e.mem);
            if (!o || o->val != e.val || o->join != e.join || o->from_store != e.from_store)
                break;
        }
        if (p == bb->npreds)
            mem_tab[nmem++] = e;
    }
    if (!nmem)
        return;
    /* the blocks of the loop: those from which a way back is reached without passing b */
    if (lp_cap < f->nblk + 1) {
        lp_cap = f->nblk * 2 + 16;
        lp_mark = (u32 *)xrealloc(lp_mark, lp_cap * sizeof(u32));
        lp_stack = (u32 *)xrealloc(lp_stack, lp_cap * sizeof(u32));
        memset(lp_mark, 0, lp_cap * sizeof(u32));
        lp_stamp = 0;
    }
    if (++lp_stamp == 0) {
        memset(lp_mark, 0, lp_cap * sizeof(u32));
        lp_stamp = 1;
    }
    lp_mark[b] = lp_stamp;
    for (p = 0; p < bb->npreds; p++)
        if (rpo_num[bb->preds[p]] >= k && lp_mark[bb->preds[p]] != lp_stamp) {
            lp_mark[bb->preds[p]] = lp_stamp;
            lp_stack[sp++] = bb->preds[p];
        }
    while (sp) {
        Block *xb = &f->blk[lp_stack[--sp]];
        for (p = 0; p < xb->npreds; p++) {
            u32 x = xb->preds[p];
            if (x >= lp_cap || rpo_num[x] == 0xffffffffU) {
                nmem = 0; /* (not expected: a way into the loop that no path reaches) */
                return;
            }
            if (lp_mark[x] != lp_stamp) {
                lp_mark[x] = lp_stamp;
                lp_stack[sp++] = x;
            }
        }
    }
    for (j = 0; j < nmem; j++)
        used[j] = opt_nomemloop == 2 || (!mem_tab[j].join && !f->ins[mem_tab[j].val].blk);
    for (j = k; j < nrpo && nmem; j++) {
        u32 x = rpo[j], *ops, n, o;
        if (lp_mark[x] != lp_stamp)
            continue;
        for (p = 0; p < 2; p++)
            for (v = p ? f->blk[x].first : f->blk[x].phis; v && nmem; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                IR_OPS(f, i, ops, n);
                for (o = 0; o < n; o++) {
                    u32 a = ir_resolve(f, ops[o]), e;
                    for (e = 0; e < nmem; e++)
                        if (mem_tab[e].val == a)
                            used[e] = 1;
                }
            }
        if (f->blk[x].arg) {
            u32 a = ir_resolve(f, f->blk[x].arg), e;
            for (e = 0; e < nmem; e++)
                if (mem_tab[e].val == a)
                    used[e] = 1;
        }
        for (v = f->blk[x].first; v && nmem; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            switch (i->op) {
            case OP_STORE:
                mem_kill_used(ir_resolve(f, i->a), i->mem, 0, used); /* (gvn has not been here yet: the operand may be a copy) */
                break;
            case OP_CALL:
                if (!(i->flags & IF_PURE))
                    mem_kill_used(0, 0, call_nowrite(i) ? 3 : 1, used);
                break;
            case OP_XI:
                if (i->flags & XI_EFFECT)
                    mem_kill_used(0, 0, 1, used);
                break;
            case OP_VSTORE:
                if (i->c && root_known(root_of(i->c, 0)))
                    nmem = 0;
                else
                    mem_kill_used(ir_resolve(f, i->a), 0, 2, used);
                break;
            case OP_MEMCPY: case OP_MEMZERO:
                mem_kill_used(ir_resolve(f, i->a), 0, 2, used);
                break;
            }
        }
    }
    /* A value that the loop does not use otherwise would have to be kept through all of it, in a register that the
       loop may not have to spare: read from a stack slot it is no cheaper than read from where it is, and dearer
       where the read was part of another instruction (ufsm test_spatial_augment: +8 G instructions when every known
       value was kept). So only what the loop uses anyway stays known (and constants, which need no register). */
    for (j = 0, sp = 0; j < nmem; j++)
        if (used[j])
            mem_tab[sp++] = mem_tab[j];
    nmem = sp;
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
        if (rpo_num[bb->preds[p]] >= k) {
            mem_enter_loop(b, k); /* a way in not yet visited (a loop), or one that cannot be reached */
            return;
        }
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

static int opt_noselcse = -1;

static void gvn(void)
{
    u32 k, v, nx, n, j, mask;
    compute_dom();
    alias_prepare();
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
                if (call_nowrite(i))
                    mem_kill_nowrite();
                else
                    mem_kill_call();
                continue;
            case OP_VSTORE:
                if (i->c && (!al_on || root_known(root_of(i->c, 0)))) { /* (the address is a + c: a is taken for the pointer) */
                    nmem = 0;
                    continue;
                }
                mem_kill_object(i->a);
                continue;
            case OP_MEMCPY:
            case OP_MEMZERO:
                mem_kill_object(i->a);
                continue;
            case OP_XI:
                if (i->flags & XI_EFFECT)
                    mem_kill_call();
                continue;
            case OP_SELECT: {
                u64 cv;
                if (ir_is_const(f, i->a, &cv)) {
                    ir_replace(f, v, cv ? i->b : i->c);
                } else if (select_same(f, v)) {
                    ir_replace(f, v, select_same(f, v));
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
                /* x86-64 host: a choice made twice from the same test and values is one (the argument of a min / max
                   macro, itself a ?:, is written out in both the comparison and the result: fit_vox2's
                   FIT_IMIN (x, n - 2 < 0 ? 0 : n - 2) computed its clamp twice). MCC_NOSELCSE: off. */
                if (opt_noselcse < 0)
                    opt_noselcse = getenv("MCC_NOSELCSE") != 0;
                if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && !opt_noselcse && f->ins[v].op == OP_SELECT &&
                    f->ins[v].ty != IT_V128)
                    goto number;
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

static int opt_ifcsh = -1;
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
   does), or as the arguments of the successors' own phis on B's edge. Not for a loop's head. MCC_NOJT: off.
   Device code too (and the host code of .cu units): ufsm's volcomp GPU decoder (k_entropy: a bit reader's
   gbr_get(...) inlined some 8 times per token) kept each call's success as a 0 / 1 value, a convergence region and
   a test of it; threaded, sm_86: 88.0 -> 61.1 ms over 64 real chunks (nvcc 68.6), the same bytes. MCC_NOJTDEV: host only. */
static int opt_nojt = -1;
static int opt_nojtdev = -1;

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

/* A comparison of an I32 value known to lie in 0..hi (hi below 2^31: a byte loaded unsigned, a masked value)
   with the constant k, when every such value compares the same way: a predecessor that brings such a value
   decides the branch as one that brings a constant does. The case is a decoder's symbol function that
   returns a table's byte or -1 for failure, tested with "< 0" after inlining (vf_rdec_get in ufsm's volcomp:
   the symbol went through test / jl once per token). MCC_NOJTR: constants only. */
static int opt_nojtr = -1;

static int jt_range(int op, u64 hi, u64 k, u64 *r)
{
    i64 sk = (i64)(i32)(u32)k, shi = (i64)hi;
    u64 uk = (u32)k;
    int v = -1;
    switch (op) {
    case OP_SLT: v = sk <= 0 ? 0 : sk > shi ? 1 : -1; break;
    case OP_SGE: v = sk <= 0 ? 1 : sk > shi ? 0 : -1; break;
    case OP_SLE: v = sk < 0 ? 0 : sk >= shi ? 1 : -1; break;
    case OP_SGT: v = sk < 0 ? 1 : sk >= shi ? 0 : -1; break;
    case OP_ULT: v = uk == 0 ? 0 : uk > hi ? 1 : -1; break;
    case OP_UGE: v = uk == 0 ? 1 : uk > hi ? 0 : -1; break;
    case OP_ULE: v = uk >= hi ? 1 : -1; break;
    case OP_UGT: v = uk >= hi ? 0 : -1; break;
    case OP_EQ: v = uk > hi ? 0 : -1; break;
    case OP_NE: v = uk > hi ? 1 : -1; break;
    default: break;
    }
    if (v < 0)
        return 0;
    *r = (u64)v;
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
/* What a branch passed on the way says of o, in predecessor p of block B: p's own branch on o (or on o == 0), when
   only one of its ways is B, or that of a block above p whose one way, to a block nothing else comes to, all of p's
   paths took. A flag set in a few ifs and tested twice (ufsm's write_spatial_ref: if (!inside) ...; if (inside)
   { ... }) is the case: the second test is decided by the first, so its block goes (and the flag, with one use left,
   becomes branches: br_bools). exact: the value is wanted, not only whether it is zero. MCC_NOJTI: off. */
static int opt_nojti = -1;
static int opt_nojtjoin = -1;

/* Device code: c (a comparison of x with a constant) known to be truth decides o, another comparison of x with a
   constant: 1 / 0, or -1. if (k < n) ... if (k + 1 < n) with n the bound min(bw, C - base) is the case (a guard
   that failed fails for every later k). The two in the same order (signed, or unsigned). MCC_NOJTCMP: off. */
static int opt_nojtcmp = -1;
static int jt_cmp_implies(u32 c, int truth, u32 o)
{
    Ins *ic = &f->ins[c], *io = &f->ins[o];
    u64 k1, k2, lo, hi, top;
    int op, op2 = io->op, uns, w64;
    if (opt_nojtcmp < 0)
        opt_nojtcmp = getenv("MCC_NOJTCMP") != 0;
    if (opt_nojtcmp || ic->op < OP_EQ || ic->op > OP_UGE || op2 < OP_EQ || op2 > OP_UGE ||
        ir_resolve(f, ic->a) != ir_resolve(f, io->a) || !ir_is_const(f, ic->b, &k1) || !ir_is_const(f, io->b, &k2))
        return -1;
    w64 = f->ins[ir_resolve(f, ic->a)].ty == IT_I64;
    if (!w64 && f->ins[ir_resolve(f, ic->a)].ty != IT_I32)
        return -1;
    op = ic->op;
    if (!truth)
        op = op == OP_EQ ? OP_NE : op == OP_NE ? OP_EQ : op == OP_SLT ? OP_SGE : op == OP_SGE ? OP_SLT : op == OP_SLE ? OP_SGT :
             op == OP_SGT ? OP_SLE : op == OP_ULT ? OP_UGE : op == OP_UGE ? OP_ULT : op == OP_ULE ? OP_UGT : OP_ULE;
    if (op == OP_NE)
        return -1; /* (a hole, not an interval) */
    uns = op >= OP_ULT || op2 >= OP_ULT;
    if ((op >= OP_SLT && op <= OP_SGE && op2 >= OP_ULT) || (op >= OP_ULT && op2 >= OP_SLT && op2 <= OP_SGE))
        return -1; /* (orders that differ) */
    if (op2 == OP_EQ || op2 == OP_NE)
        uns = op >= OP_ULT;
    /* the values in one domain: signed as i64 offset by 2^63 into u64 order, so one unsigned compare serves both */
    if (uns) {
        if (!w64) { k1 = (u32)k1; k2 = (u32)k2; }
        top = w64 ? ~(u64)0 : 0xffffffffUL;
        lo = 0;
    } else {
        if (!w64) { k1 = (u64)(i64)(i32)(u32)k1; k2 = (u64)(i64)(i32)(u32)k2; }
        k1 ^= (u64)1 << 63; k2 ^= (u64)1 << 63;
        top = w64 ? ~(u64)0 : ((u64)1 << 63) + 0x7fffffffUL;
        lo = w64 ? 0 : ((u64)1 << 63) - 0x80000000UL;
    }
    hi = top;
    switch (op) {
    case OP_EQ: lo = hi = k1; break;
    case OP_SLT: case OP_ULT: if (k1 == lo) return -1; hi = k1 - 1; break;
    case OP_SLE: case OP_ULE: hi = k1; break;
    case OP_SGT: case OP_UGT: if (k1 == top) return -1; lo = k1 + 1; break;
    default: lo = k1; break; /* (SGE, UGE) */
    }
    if (lo > hi)
        return -1;
    switch (op2) {
    case OP_EQ: return lo == hi && lo == k2 ? 1 : k2 < lo || k2 > hi ? 0 : -1;
    case OP_NE: return lo == hi && lo == k2 ? 0 : k2 < lo || k2 > hi ? 1 : -1;
    case OP_SLT: case OP_ULT: return hi < k2 ? 1 : lo >= k2 ? 0 : -1;
    case OP_SLE: case OP_ULE: return hi <= k2 ? 1 : lo > k2 ? 0 : -1;
    case OP_SGT: case OP_UGT: return lo > k2 ? 1 : hi <= k2 ? 0 : -1;
    default: return lo >= k2 ? 1 : hi < k2 ? 0 : -1; /* (SGE, UGE) */
    }
}
static int jt_implied(u32 o, u32 p, u32 B, int exact, u64 *val)
{
    u32 x = p, d, steps = 0;
    u64 z;
    int oinv = 0;
    if (opt_nojti < 0)
        opt_nojti = getenv("MCC_NOJTI") != 0;
    if (opt_nojti || (f->ins[o].ty != IT_I32 && f->ins[o].ty != IT_I64))
        return 0;
    if (exact && !ir_is_bool(f, o, 3))
        exact = 2; /* only "is zero" is a value */
    /* (x != 0 and x == 0 are what x says, the second the other way round) */
    while ((f->ins[o].op == OP_EQ || f->ins[o].op == OP_NE) && ir_is_const(f, f->ins[o].b, &z) && z == 0) {
        oinv ^= f->ins[o].op == OP_EQ;
        o = ir_resolve(f, f->ins[o].a);
    }
    for (d = p; steps < 48; steps++) {
        Block *db = &f->blk[d];
        if (db->term == TERM_BR && db->succ[0] != db->succ[1] && db->arg) {
            u32 c = ir_resolve(f, db->arg), way = 2;
            int inv = 0, hit = 0;
            if (steps == 0)
                way = db->succ[0] == B ? 0 : db->succ[1] == B ? 1 : 2;
            else if (f->blk[x].npreds == 1)
                way = db->succ[0] == x ? 0 : db->succ[1] == x ? 1 : 2;
            while ((f->ins[c].op == OP_EQ || f->ins[c].op == OP_NE) && ir_is_const(f, f->ins[c].b, &z) && z == 0) {
                inv ^= f->ins[c].op == OP_EQ;
                c = ir_resolve(f, f->ins[c].a);
            }
            hit = c == o;
            if (!hit && way < 2 && opt_dev_fn) {
                int d = jt_cmp_implies(c, ((way == 0) != inv), o);
                if (d >= 0) {
                    int nz = d != oinv;
                    if (nz && exact == 2)
                        return 0;
                    *val = nz ? 1 : 0;
                    return 1;
                }
            }
            if (hit && way < 2) {
                int nz = ((way == 0) != inv) != oinv;
                if (nz && exact == 2)
                    return 0;
                *val = nz ? 1 : 0;
                return 1;
            }
        }
        if (d == 1 || rpo_num[d] == 0xffffffffU || !idom[d] || idom[d] == d)
            return 0;
        x = d;
        d = idom[d];
    }
    return 0;
}

static int jump_thread(void)
{
    u32 B, v, k, j;
    int changed = 0;
    if (opt_nojt < 0)
        opt_nojt = getenv("MCC_NOJT") != 0;
    if (opt_nojtr < 0)
        opt_nojtr = getenv("MCC_NOJTR") != 0;
    if (opt_nojtdev < 0)
        opt_nojtdev = getenv("MCC_NOJTDEV") != 0;
    if (opt_nojt || ((opt_dev_fn || opt_cuda) && opt_nojtdev))
        return 0;
    compute_dom();
    for (B = 2; B < f->nblk; B++) {
        Block *bb = &f->blk[B];
        u32 S[2], np = bb->npreds, to[JT_MAXP], body[JT_MAXI], nb = 0, phis[JT_MAXI * 2], nphi = 0, cnt[2] = {0, 0};
        u64 val[JT_MAXI];
        u32 rrep[2][JT_MAXI * 2];
        int ok = 1, sd[2], rep[2] = {0, 0};
        if (bb->term != TERM_BR || rpo_num[B] == 0xffffffffU || np < 2 || np > JT_MAXP)
            continue;
        if (!bb->phis) {
            /* (no phi: only what a branch above says of the condition can decide) */
            u32 c0 = ir_resolve(f, bb->arg);
            if (!jt_implied(c0, bb->preds[0], B, 0, &val[0]))
                continue;
        }
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
                int ar[3] = {0, 0, 0}, ety = i->ty; /* ar[q]: operand q is not a constant but an I32 in 0..a[q] */
                IR_OPS(f, i, ops, n);
                if (n > 2) {
                    ok = 0;
                    break;
                }
                /* (a comparison is of its operands' width, not its result's) */
                if (i->op >= OP_EQ && i->op <= OP_UGE)
                    ety = f->ins[ir_resolve(f, ops[0])].ty;
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
                    if (!found && !ir_is_const(f, o, &a[q]) && !jt_implied(o, p, B, 1, &a[q])) {
                        if (!opt_nojtr && f->ins[o].ty == IT_I32 && ety == IT_I32 && ir_umax32(f, o) < 0x80000000UL) {
                            a[q] = ir_umax32(f, o);
                            ar[q] = 1;
                        } else {
                            ok = 0;
                        }
                    }
                }
                if (ok && (ar[0] || ar[1])) {
                    if (ar[1] || !jt_range(i->op, a[0], a[1], &val[m]))
                        ok = 0;
                } else if (ok && !jt_eval(i->op, ety, a[0], a[1], &val[m]))
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
            } else if (!jt_implied(c, p, B, 0, &cv)) {
                ok = 0;
            }
            to[k] = cv ? 0 : 1;
            cnt[to[k]]++;
        }
        if (!ok)
            continue;
        sd[0] = f->blk[S[0]].npreds == 1;
        sd[1] = f->blk[S[1]].npreds == 1;
        /* A successor with other predecessors (the join of an if: if (j < q) {...} if (j + 1 < q) {...}, the second
           test decided by the first) can take B's phis too when each of its other predecessors is reached only through
           the other successor, which has B alone before it: a new phi there gets, per predecessor, the value B's phi had
           on that way (bwd_data_s2_mx2_k's 32 x 8 guarded loads: the guard after a taken one stayed, tested again).
           MCC_NOJTJOIN: off. */
        if (opt_nojtjoin < 0)
            opt_nojtjoin = getenv("MCC_NOJTJOIN") != 0;
        for (j = 0; j < 2 && !opt_nojtjoin && opt_dev_fn; j++) {
            Block *sb = &f->blk[S[j]];
            u32 q;
            if (sd[j] || !sd[1 - j] || !cnt[j] || !cnt[1 - j] || sb->npreds > JT_MAXP)
                continue;
            rep[j] = 1;
            for (q = 0; q < sb->npreds; q++) {
                u32 p = sb->preds[q];
                if (p != B && (rpo_num[p] == 0xffffffffU || !dominates(S[1 - j], p)))
                    rep[j] = 0;
            }
        }
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
            ok = jt_used_ok(phis[k], B, S[0], S[1], sd[0] || rep[0], sd[1] || rep[1], &nin);
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
                    rrep[j][k] = r;
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
        /* (the join: its predecessors as they now are, B's ones and those of the other successor's side) */
        for (j = 0; j < 2; j++) {
            u32 s = S[j], q, t;
            if (!rep[j])
                continue;
            for (k = 0; k < nphi; k++) {
                u32 x = phis[k], r = ir_raw(f, OP_PHI, f->ins[x].ty, 0, 0), av[2 * JT_MAXP];
                for (q = 0; q < f->blk[s].npreds && q < 2 * JT_MAXP; q++) {
                    u32 p = f->blk[s].preds[q], a = rrep[1 - j][k];
                    for (t = 0; t < np; t++)
                        if (bb->preds[t] == p && to[t] == j)
                            a = ir_resolve(f, f->xargs[f->ins[x].a + t]);
                    av[q] = a;
                }
                ir_phi_set_args(f, r, av, q);
                f->ins[r].blk = s;
                f->ins[r].next = f->blk[s].phis;
                f->blk[s].phis = r;
                jt_rewrite(x, s, r);
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

/* Device code: one predecessor p of B that decides B's branch (jt_implied) goes straight to the successor Sd it
   decides, B staying for the others. The other successor So has B alone before it, so what B's phis give on the
   way through So stays as it is; Sd gets a phi per phi of B (p's argument, or the phi itself on the old edges),
   which takes its place in what Sd dominates. B's other instructions are used in B only. The chain
   if (k < n) {...} if (k + 1 < n) {...} ... (bwd_data_s2_mx2_k's channel guard, k < min(bw, C - base) over 32
   unrolled k) is the case: a guard that failed went through every later test and its phi copies; now it goes
   to the end. MCC_NOJTP: off. */
static int opt_nojtp = -1;
#define JTP_MAXPHI 192 /* (an unrolled step carries 32 sums and 32 loaded values, and what the guards before it merged) */
#define JTP_MAXH 64 /* (fwd_epilogue's join: some 40 index values ahead of the flag test) */

static int jtp_use_ok(u32 x, u32 B, u32 Sd, u32 So, int body)
{
    u32 bi, v, k, n, *ops;
    for (bi = 1; bi < f->nblk; bi++) {
        Block *ub = &f->blk[bi];
        if (ub->term == TERM_NONE || rpo_num[bi] == 0xffffffffU)
            continue;
        for (v = ub->phis; v; v = f->ins[v].next)
            for (k = 0; k < f->ins[v].b; k++)
                if (ir_resolve(f, f->xargs[f->ins[v].a + k]) == x) {
                    u32 at = ub->preds[k];
                    if (rpo_num[at] == 0xffffffffU)
                        return 0;
                    if (at == B ? bi != So && (body || bi != Sd) : !dominates(So, at) && (body || !dominates(Sd, at)))
                        return 0;
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
            if (hit && bi != B && !dominates(So, bi) && (body || !dominates(Sd, bi)))
                return 0;
        }
        if (ub->arg && ir_resolve(f, ub->arg) == x && bi != B && !dominates(So, bi) && (body || !dominates(Sd, bi)))
            return 0;
    }
    return 1;
}

/* c, B's condition, on the way in from predecessor k: decided when it is a phi of B, or a comparison of phis of B
   and constants, whose arguments on that way are constants (a flag set on some ways: fwd_epilogue's mx_done, set by
   the epilogue variant that ran and tested by the next, behind the index arithmetic the passes put in the join) */
static int jtp_phi_decides(u32 c, u32 B, u32 k, u64 *cv)
{
    Ins *i = &f->ins[c];
    u64 a[2];
    u32 q;
    if (i->op == OP_PHI && i->blk == B)
        return ir_is_const(f, ir_resolve(f, f->xargs[i->a + k]), cv);
    if (i->op < OP_EQ || i->op > OP_UGE)
        return 0;
    for (q = 0; q < 2; q++) {
        u32 o = ir_resolve(f, q ? i->b : i->a);
        if (f->ins[o].op == OP_PHI && f->ins[o].blk == B)
            o = ir_resolve(f, f->xargs[f->ins[o].a + k]);
        if (!ir_is_const(f, o, &a[q]))
            return 0;
    }
    return jt_eval(i->op, f->ins[ir_resolve(f, i->a)].ty, a[0], a[1], cv);
}

static int jt_partial(void)
{
    u32 B, v, k, q;
    int changed = 0;
    if (opt_nojtp < 0)
        opt_nojtp = getenv("MCC_NOJTP") != 0;
    if (opt_nojtp || !opt_dev_fn || opt_nojt > 0 || opt_nojtdev > 0)
        return 0;
    compute_dom();
    for (B = 2; B < f->nblk; B++) {
        Block *bb = &f->blk[B];
        u32 c, p = 0, pk = 0, d = 2, Sd, So, nphi = 0, hs[JTP_MAXH], nh = 0;
        u64 cv;
        int ok = 1, mk = 1;
        if (bb->term != TERM_BR || rpo_num[B] == 0xffffffffU || bb->npreds < 2 || bb->npreds > JT_MAXP || !bb->arg ||
            bb->succ[0] == bb->succ[1] || bb->succ[0] == B || bb->succ[1] == B || bb->succ[0] == 1 || bb->succ[1] == 1)
            continue;
        c = ir_resolve(f, bb->arg);
        /* (not a loop's head: a way round it into the loop's body would make a loop with two ways in) */
        for (k = 0; k < bb->npreds; k++)
            if (rpo_num[bb->preds[k]] != 0xffffffffU && dominates(B, bb->preds[k]))
                break;
        if (k < bb->npreds)
            continue;
        for (k = 0; k < bb->npreds && d == 2; k++) {
            u32 pp = bb->preds[k];
            Block *pb = &f->blk[pp];
            if (pp == B || rpo_num[pp] == 0xffffffffU || dominates(B, pp) || (pb->term != TERM_JMP && pb->term != TERM_BR))
                continue;
            if (pb->term == TERM_BR && pb->succ[0] == pb->succ[1])
                continue;
            for (q = 0; q < bb->npreds; q++)
                if (q != k && bb->preds[q] == pp)
                    break;
            if (q < bb->npreds)
                continue;
            if (!jtp_phi_decides(c, B, k, &cv) && !jt_implied(c, pp, B, 0, &cv))
                continue;
            p = pp;
            pk = k;
            d = cv ? 0 : 1;
        }
        if (d == 2)
            continue;
        Sd = bb->succ[d];
        So = bb->succ[1 - d];
        if (f->blk[So].npreds != 1 || f->blk[Sd].npreds >= JT_MAXP || Sd == p ||
            (f->blk[p].term == TERM_BR && (f->blk[p].succ[0] == Sd || f->blk[p].succ[1] == Sd)))
            continue;
        /* (B above all of Sd's ways: B's phis may be used below Sd, and get one there; else they are not, B being
           above none of Sd's blocks) */
        for (k = 0; k < f->blk[Sd].npreds; k++)
            if (f->blk[Sd].preds[k] != B && (rpo_num[f->blk[Sd].preds[k]] == 0xffffffffU || !dominates(B, f->blk[Sd].preds[k])))
                mk = 0;
        /* B's instructions: used on So's side only, or (wanted below Sd too: values the program's index arithmetic
           shares between the unrolled steps) made of values from above B, and then moved up to B's dominator, where
           p's way has them as well. No division: it is done where it was. */
        nh = 0;
        for (v = bb->first; v && ok; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 *ops, n, q2, t;
            if (!is_pure(i->op))
                ok = 0;
            else if (!jtp_use_ok(v, B, Sd, So, 1)) {
                ok = nh < JTP_MAXH && i->op != OP_SDIV && i->op != OP_UDIV && i->op != OP_SREM && i->op != OP_UREM &&
                     i->op != OP_PHI && idom[B] && idom[B] != B;
                IR_OPS(f, i, ops, n);
                for (q2 = 0; q2 < n && ok; q2++) {
                    u32 o = ir_resolve(f, ops[q2]);
                    if (f->ins[o].blk == B) {
                        for (t = 0; t < nh && hs[t] != o; t++)
                            ;
                        ok = t < nh;
                    }
                }
                if (ok)
                    hs[nh++] = v;
            }
        }
        for (v = bb->phis; v && ok; v = f->ins[v].next, nphi++)
            ok = nphi < JTP_MAXPHI && jtp_use_ok(v, B, Sd, So, 0);
        /* (Sd's phis: what B's edge brings, for p) */
        for (v = f->blk[Sd].phis; v && ok; v = f->ins[v].next) {
            u32 a = ir_resolve(f, f->xargs[f->ins[v].a + pred_index(Sd, B)]), t;
            for (t = 0; t < nh && hs[t] != a; t++)
                ;
            if (f->ins[a].blk == B && f->ins[a].op != OP_PHI && t == nh)
                ok = 0;
        }
        if (!ok)
            continue;
        for (k = 0; k < nh; k++) {
            ir_remove(f, hs[k]);
            ir_append(f, idom[B], hs[k]);
        }
        {
            u32 bi = pred_index(Sd, B), nps, old[JTP_MAXPHI], nold = 0, newp[JTP_MAXPHI];
            for (v = f->blk[Sd].phis; v && nold < JTP_MAXPHI; v = f->ins[v].next)
                old[nold++] = v;
            if (v)
                continue;
            /* B's phis: one in Sd each */
            nps = f->blk[Sd].npreds;
            k = 0;
            for (v = mk ? bb->phis : 0; v; v = f->ins[v].next) {
                u32 r = ir_raw(f, OP_PHI, f->ins[v].ty, 0, 0), av[JT_MAXP + 1];
                for (q = 0; q < nps; q++)
                    av[q] = v;
                av[nps] = ir_resolve(f, f->xargs[f->ins[v].a + pk]);
                ir_phi_set_args(f, r, av, nps + 1);
                f->ins[r].blk = Sd;
                newp[k++] = r;
            }
            /* Sd's own phis: p's argument is what B's edge brought, seen from p */
            for (q = 0; q < nold; q++) {
                u32 a = ir_resolve(f, f->xargs[f->ins[old[q]].a + bi]);
                if (f->ins[a].op == OP_PHI && f->ins[a].blk == B)
                    a = ir_resolve(f, f->xargs[f->ins[a].a + pk]);
                ir_phi_add_arg(f, old[q], a);
            }
            ir_add_pred(f, Sd, p);
            k = 0;
            for (v = mk ? bb->phis : 0; v; v = f->ins[v].next) {
                jt_rewrite(v, Sd, newp[k]);
                /* (jt_rewrite took the new phi's own arguments on Sd's old edges for uses: they are B's phi) */
                for (q = 0; q < nps; q++)
                    f->xargs[f->ins[newp[k]].a + q] = v;
                f->ins[newp[k]].next = f->blk[Sd].phis;
                f->blk[Sd].phis = newp[k];
                k++;
            }
            for (q = 0; q < 2; q++)
                if (f->blk[p].succ[q] == B && (q == 0 || f->blk[p].term == TERM_BR))
                    f->blk[p].succ[q] = Sd;
            remove_edge(p, B);
            changed = 1;
            compute_dom();
        }
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
    if (opt_arch == ARCH_X64 && opt_level >= 2 && jt_partial())
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
            /* x86-64 host: one way of the branch goes through an empty block that other blocks come through too, the
               other way straight to the join: this branch gets an empty block of its own on that way, with the same
               values for the join's phis, and is then a branch around nothing as below (a select of two values that
               are there already). MCC_NOIFCSH: off. MCC_IFCSH=2: also when the other way is a cheap arm (the 0 of
               "i < 256 ? 0 : u[i - 256]" and of the "a == 0 ? 0 : (a <= 2 ? 1 : 2)" that follows share one block
               in ufsm's vll_dense_ctx): not the default, for in the volcomp decoder that took 4.3 G branches of 32 G
               away and added 3.7 G instructions to 177 G, the arm's selects now computed on every pass. */
            if (bb->term == TERM_BR && bb->succ[0] != bb->succ[1] && opt_level >= 3 && !opt_size && !opt_dev_fn && !opt_cuda &&
                opt_arch == ARCH_X64) {
                int side;
                if (opt_ifcsh < 0)
                    opt_ifcsh = getenv("MCC_NOIFCSH") ? 0 : getenv("MCC_IFCSH") ? atoi(getenv("MCC_IFCSH")) : 1;
                for (side = 0; side < 2 && opt_ifcsh; side++) {
                    u32 t = bb->succ[side], e = bb->succ[!side], j, n, kt, nphi = 0;
                    Block *tb = &f->blk[t], *eb = &f->blk[e];
                    if (t == b || t == 1 || tb->term != TERM_JMP || tb->first || tb->phis || tb->npreds < 2)
                        continue;
                    j = tb->succ[0];
                    if (j == t || j == b || j == 1 || !f->blk[j].phis)
                        continue;
                    if (e == j) {
                        if (pred_index(j, b) == 0xffffffffU)
                            continue;
                    } else if (!(opt_ifcsh >= 2 && eb->term == TERM_JMP && eb->succ[0] == j && !eb->phis && eb->npreds == 1 &&
                                 e != b && e != 1 && pred_index(j, b) == 0xffffffffU && cheap_block(e))) {
                        continue;
                    }
                    for (v = f->blk[j].phis; v; v = f->ins[v].next)
                        nphi++;
                    kt = pred_index(j, t);
                    if (nphi > 2 || kt == 0xffffffffU)
                        continue;
                    n = ir_new_block(f);
                    f->blk[n].term = TERM_JMP;
                    f->blk[n].succ[0] = j;
                    ir_add_pred(f, n, b);
                    f->blk[b].succ[side] = n;
                    remove_pred(t, pred_index(t, b));
                    for (v = f->blk[j].phis; v; v = f->ins[v].next)
                        ir_phi_add_arg(f, v, f->xargs[f->ins[v].a + kt]);
                    ir_add_pred(f, j, n);
                    bb = &f->blk[b];
                    break;
                }
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

static int opt_licmcalls = -1;
#define LICM_STORES 48 /* the stores of a loop that are remembered (16 without roots) */
#define LICM_WHOLE 0xff /* store_mem: a write of unknown length */

/* Device code: loops are hoisted from (without MCC_LICM) when what they carry round is no more than opt_licm_phi values
   (the phis of the loop's head): a loop of many accumulators (conv_fwd_f4v_k's 224 MMAs) has no registers to spare for
   values worked out before it, and spills with them, where a loop of few (conv_fwd_s2_f8_k: 32) recomputed each trip
   its addresses' 64-bit parts, divisions by constants and lane numbers (664 SASS instructions in its MMA loop against
   nvcc's 218; hoisted, 270). MCC_LICMPHI=N; 0 (the default): no device loop hoisted from. Off because the GPU did not follow the
   counts: on an RTX 4060, at 16, conv_fwd_s2_f8_k ran as fast as without and bwd_data_s2_f4_k<4,8> 31 % slower. */
#ifndef LICM_PHI_DEFAULT
#define LICM_PHI_DEFAULT 0
#endif
int opt_licm_phi = -1;
int opt_licm_cost = -1; /* MCC_LICMCOST=0: every pure value of a device loop may go (see licm) */

static int licm(void)
{
    u32 *mark, *stack, k, j, p, v, nx, sp, nblk0 = f->nblk;
    int any_replaced = 0;
    if (opt_nopurelicm < 0)
        opt_nopurelicm = getenv("MCC_NOPURELICM") != 0;
    if (opt_licmcalls < 0)
        opt_licmcalls = getenv("MCC_NOLICMROOT") ? 2 : getenv("MCC_LICMCALLS") != 0; /* (MCC_NOLICMROOT: licm without roots) */
    compute_dom();
    alias_prepare();
    if (opt_licmcalls == 2)
        al_on = 0;
    mark = (u32 *)xmalloc(cap_blocks * 2 * sizeof(u32));
    stack = mark + cap_blocks;
    memset(mark, 0, cap_blocks * sizeof(u32));

    /* inner loops first: their headers come later in reverse postorder */
    for (k = nrpo; k-- > 0;) {
        u32 h = rpo[k], pre = 0, noutside = 0;
        u32 store_addr[LICM_STORES], nstores = 0, deref[24], nderef = 0, exiting[8], nexiting = 0;
        u8 store_mem[LICM_STORES], deref_size[24];
        u32 hoisted[32], nhoisted = 0;
        int clobbers_all = 0, replaced = 0, has_call = 0;
        Block *hb = &f->blk[h];

        if (opt_dev_fn && !opt_dolicm) {
            int nph = 0;
            for (v = hb->phis; v && nph <= opt_licm_phi; v = f->ins[v].next)
                nph++;
            if (nph > opt_licm_phi)
                continue;
        }
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
                if ((i->op == OP_CALL && !(i->flags & IF_PURE && !opt_nopurelicm)) || (i->op == OP_XI && (i->flags & XI_EFFECT))) {
                    /* A call writes only where an escaped address points (call_spares), but a load taken out of a
                       loop with calls has its value in a callee-saved register or, more often, in a stack slot read
                       after every call: no fewer loads, and a test of memory becomes a load and a test (ufsm
                       warp_w4_ref: +2 G instructions in test_spatial_augment). So such loops keep their loads,
                       unless MCC_LICMCALLS asks (for measuring). */
                    if (al_on && opt_licmcalls)
                        has_call = 1;
                    else
                        clobbers_all = 1;
                } else if (i->op == OP_MEMCPY || i->op == OP_MEMZERO || i->op == OP_VSTORE) {
                    /* a write of some length: with roots, into the object i->a is in */
                    if (!al_on || nstores == LICM_STORES || (i->op == OP_VSTORE && i->c && root_known(root_of(i->c, 0)))) {
                        clobbers_all = 1;
                    } else {
                        store_addr[nstores] = i->a;
                        store_mem[nstores++] = LICM_WHOLE;
                    }
                } else if (i->op == OP_STORE) {
                    if (nstores == (al_on ? LICM_STORES : 16)) {
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
                    if (ok && opt_dev_fn && !opt_dolicm && opt_licm_cost) {
                        /* device code (not MCC_LICM): only what costs more than an instruction where it is, a 64-bit
                           value (two registers, and its arithmetic two or more instructions: an address's sign extension
                           and product) or a division: a 32-bit sum of lane numbers costs as much in the loop as the
                           register it would hold through it (conv_bwd_w_f4_k at its cap of 96 registers spilled) */
                        int op = i->op;
                        ok = i->ty == IT_I64 || op == OP_SDIV || op == OP_UDIV || op == OP_SREM || op == OP_UREM;
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
                    if (ok && has_call && !call_spares(i->a))
                        ok = 0;
                    for (p = 0; ok && p < nstores; p++)
                        if (store_mem[p] == LICM_WHOLE ? !roots_apart(store_addr[p], i->a) : may_alias(store_addr[p], store_mem[p], i->a, i->mem))
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
            u32 pin_of[6], pin_v[6], npins = 0, maxpins = clobbers_all || has_call ? 2 : 6;
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
int opt_wrapv;
u8 *opt_sym_private;
u32 opt_sym_private_n;

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
static int sink_calls(void);
static int unselect_calls(void);

static void addr_shape(Func *fn);
static void iv_widen(Func *fn);
static void unswitch(Func *fn);

/* x86-64 host, before selection: a branch on whether a flag is still set that several tests may have cleared,
       int inside = 1; for (d = 0; d < 3; d++) if (v[d] < 0 || v[d] > P - 1) inside = 0; ... if (!inside) continue;
   is, after if-conversion, a chain select(c3, 0, select(c2, 0, c1 == 0)) compared with 0: every test made into 0 / 1,
   or-ed, selected and tested (ufsm's warp_phase_ref / warp_w4_ref: 27 instructions a voxel, where a branch per
   test is 12). The chain is 0 exactly when one of the tests holds, so the branch is given their or, which the
   selector turns into a branch per test (x64.c, FUSE_BR2). Only chains used by that branch alone, in its block.
   MCC_NOBRBOOL: off. */
static void br_bools(Func *fn)
{
    static int off = -1;
    u32 b, nuses, *uses;
    if (off < 0)
        off = getenv("MCC_NOBRBOOL") != 0;
    if (off)
        return;
    f = fn;
    uses = count_uses(&nuses);
    for (b = 1; b < f->nblk; b++) {
        Block *bb = &f->blk[b];
        u32 x, s, c[8], n = 0, k, t, acc, sw = 0;
        u64 z;
        Ins *i;
        if (bb->term != TERM_BR || !bb->arg)
            continue;
        x = ir_resolve(f, bb->arg);
        i = &f->ins[x];
        /* the branch is on "the chain is 0" (x = (s == 0)), or on the chain itself (then the two ways change places) */
        if (i->op == OP_EQ && i->ty == IT_I32 && x < nuses && uses[x] == 1 && i->blk == b && ir_is_const(f, ir_resolve(f, i->b), &z) && z == 0) {
            s = ir_resolve(f, i->a);
        } else {
            s = x;
            sw = 1;
        }
        t = s;
        while (n < 7) {
            Ins *si = &f->ins[t];
            if (si->op != OP_SELECT || si->ty != IT_I32 || t >= nuses || uses[t] != 1 || si->blk != b ||
                !ir_is_const(f, ir_resolve(f, si->b), &z) || z != 0)
                break;
            c[n++] = ir_resolve(f, si->a);
            t = ir_resolve(f, si->c);
        }
        if (!n)
            continue;
        /* the innermost: c1 == 0 (its test is c1), or any truth value (its test: that it is 0) */
        i = &f->ins[t];
        if (i->op == OP_EQ && i->ty == IT_I32 && t < nuses && uses[t] == 1 && i->blk == b && ir_is_const(f, ir_resolve(f, i->b), &z) && z == 0 &&
            f->ins[ir_resolve(f, i->a)].ty == IT_I32) {
            c[n++] = ir_resolve(f, i->a);
        } else if (ir_is_bool(f, t, 3) && f->ins[t].ty == IT_I32) {
            u32 e = ir_raw(f, OP_EQ, IT_I32, t, ir_const(f, IT_I32, 0));
            ir_append(f, b, e);
            c[n++] = e;
        } else {
            continue;
        }
        for (k = 0; k < n; k++)
            if (f->ins[c[k]].ty != IT_I32)
                break;
        if (k < n)
            continue;
        /* in the order the program made the tests: the innermost first */
        acc = c[n - 1];
        for (k = n - 1; k-- > 0;) {
            u32 o = ir_raw(f, OP_OR, IT_I32, acc, c[k]);
            ir_append(f, b, o);
            acc = o;
        }
        bb->arg = acc;
        if (sw) {
            /* (the chain itself was the condition: it holds when no test does) */
            u32 t0 = bb->succ[0];
            bb->succ[0] = bb->succ[1];
            bb->succ[1] = t0;
        }
    }
    free(uses);
    opt_dce(f);
}

/* ---- loop unswitching (x86-64 host) ----
   A branch inside a loop on a value the loop does not change goes the same way every trip. Where a loop has
   several such branches on one value (ufsm's write_spatial_ref and its kin: `ap ? ap->lut[c] : (c - mean) * isd`
   and `if (!hard)` at each of the eight corners of every voxel, the corner loops unrolled: a test, a branch and a
   jump each, 27 G of the 151 G there), the loop is made twice, the value is tested once before them, and each
   copy keeps the side that is its own. What the loop computes and something after it reads goes through a stack
   slot while there are two loops (opt_promote makes values of the slots again). The copies cost code: a loop of
   at most UNSW_LOOP instructions, UNSW_TOTAL new ones in a function, and only for two branches or more on the
   value (MCC_UNSWMIN) and one for every UNSW_PER instructions of the loop (MCC_UNSWPER: two branches in the 1900
   instructions of ufsm's vf_decode_sub made test_volcomp_fast 1 G slower). The value is one tested as it is (a
   pointer, a flag), not a comparison (MCC_UNSWCMP: those too; they are mostly the tests before inner loops, and
   test_spatial_augment was 1 to 2 G better without). Done before the vectoriser, on the function with its calls inlined. MCC_NOUNSWITCH: off;
   MCC_UNSWLOG: what was done. */
#define UNSW_LOOP 4000
#define UNSW_TOTAL 12000
#define UNSW_PER 300 /* instructions of loop for each branch that goes */

static int unsw_nocmp;

/* the value a condition is about: x != 0 and x == 0 say what x says (*inv: the other way round) */
static u32 unsw_base(u32 c, int *inv)
{
    u64 z;
    *inv = 0;
    c = ir_resolve(f, c);
    while ((f->ins[c].op == OP_EQ || f->ins[c].op == OP_NE) && ir_is_const(f, f->ins[c].b, &z) && z == 0 &&
           (f->ins[ir_resolve(f, f->ins[c].a)].ty == IT_I32 || f->ins[ir_resolve(f, f->ins[c].a)].ty == IT_I64)) {
        *inv ^= f->ins[c].op == OP_EQ;
        c = ir_resolve(f, f->ins[c].a);
    }
    return c;
}

static int unsw_mem(int ty)
{
    return ty == IT_I32 ? M_32 : ty == IT_I64 ? M_64 : ty == IT_F32 ? M_F32 : ty == IT_F64 ? M_F64 : ty == IT_V128 ? M_V128 :
           ty == IT_V256 ? M_V256 : M_V512;
}

/* One loop unswitched on one value. Returns the number of instructions copied, 0 if nothing was done. */
static u32 unswitch_one(u32 room, u32 minbr, u32 per, int *slots)
{
    u32 *mark, *stack, *vmap, *bmap, *lblk, nl, k, p, v, sp, n0, nb0, q;
    u32 best_h = 0, best_c = 0, best_n = 0, best_size = 0, h, c, pre, N, done = 0;
    compute_dom();
    nb0 = f->nblk;
    mark = (u32 *)xmalloc((size_t)nb0 * 4 * sizeof(u32));
    stack = mark + nb0;
    bmap = stack + nb0;
    lblk = bmap + nb0;
    /* the loop and the value: the most branches, of the innermost loops first */
    for (k = nrpo; k-- > 0;) {
        u32 size = 0, cand[16], cnt[16], nc = 0, nout = 0;
        Block *hb;
        int ok = 1;
        h = rpo[k];
        hb = &f->blk[h];
        sp = 0;
        for (p = 0; p < hb->npreds; p++) {
            q = hb->preds[p];
            if (q < nb0 && rpo_num[q] != 0xffffffffU && dominates(h, q))
                stack[sp++] = q;
            else
                nout++;
        }
        if (getenv("MCC_UNSWLOG") && atoi(getenv("MCC_UNSWLOG")) > 2)
            fprintf(stderr, "  head b%lu: %lu back, %lu other\n", (unsigned long)h, (unsigned long)sp, (unsigned long)nout);
        if (!sp || nout != 1 || h == 1)
            continue;
        for (p = 0; p < nb0; p++)
            mark[p] = 0;
        mark[h] = h;
        while (sp) {
            u32 x = stack[--sp];
            if (mark[x] == h)
                continue;
            mark[x] = h;
            for (p = 0; p < f->blk[x].npreds; p++) {
                q = f->blk[x].preds[p];
                if (mark[q] != h && sp < nb0)
                    stack[sp++] = q;
                else if (mark[q] != h)
                    ok = 0;
            }
        }
        for (p = 1; p < nb0 && ok; p++) {
            Block *bb = &f->blk[p];
            if (mark[p] != h)
                continue;
            if (rpo_num[p] == 0xffffffffU || (bb->term != TERM_JMP && bb->term != TERM_BR)) {
                ok = 0;
                break;
            }
            for (v = bb->phis; v; v = f->ins[v].next)
                size++;
            for (v = bb->first; v && ok; v = f->ins[v].next) {
                Ins *i = &f->ins[v];
                size++;
                if ((i->op == OP_CALL && (i->flags & (IF_ALLOCA | IF_ASM))) ||
                    (ir_opcount_tab[i->op] == 255 && i->op != OP_CALL && i->op != OP_XI) || i->op == OP_COPY || i->op == OP_NOP)
                    ok = 0;
            }
            if (bb->term == TERM_BR && bb->succ[0] != bb->succ[1] && bb->arg) {
                int inv;
                u64 z;
                c = unsw_base(bb->arg, &inv);
                if (!ir_is_const(f, c, &z) && defined_outside(c, mark, h) && f->ins[c].blk &&
                    !(unsw_nocmp && ((f->ins[c].op >= OP_EQ && f->ins[c].op <= OP_UGE) || (f->ins[c].op >= OP_FEQ && f->ins[c].op <= OP_FGE)))) {
                    for (q = 0; q < nc && cand[q] != c; q++)
                        ;
                    if (q < nc)
                        cnt[q]++;
                    else if (nc < 16) {
                        cand[nc] = c;
                        cnt[nc++] = 1;
                    }
                }
            }
        }
        if (getenv("MCC_UNSWLOG") && atoi(getenv("MCC_UNSWLOG")) > 1)
            fprintf(stderr, "  %s loop b%lu: ok %d, %lu instructions, %lu values\n", f->name ? f->name : "?", (unsigned long)h, ok, (unsigned long)size, (unsigned long)nc);
        if (!ok || size > UNSW_LOOP || size > room)
            continue;
        for (q = 0; q < nc; q++)
            if (cnt[q] >= minbr && cnt[q] > best_n && size <= cnt[q] * per) {
                best_n = cnt[q];
                best_c = cand[q];
                best_h = h;
                best_size = size;
            }
    }
    if (!best_h) {
        free(mark);
        return 0;
    }
    h = best_h;
    c = best_c;
    /* the loop's blocks again */
    for (p = 0; p < nb0; p++)
        mark[p] = 0;
    sp = 0;
    pre = 0;
    for (p = 0; p < f->blk[h].npreds; p++) {
        q = f->blk[h].preds[p];
        if (rpo_num[q] != 0xffffffffU && dominates(h, q))
            stack[sp++] = q;
        else
            pre = q;
    }
    mark[h] = h;
    while (sp) {
        u32 x = stack[--sp];
        if (mark[x] == h)
            continue;
        mark[x] = h;
        for (p = 0; p < f->blk[x].npreds; p++)
            if (mark[f->blk[x].preds[p]] != h)
                stack[sp++] = f->blk[x].preds[p];
    }
    nl = 0;
    for (p = 1; p < nb0; p++)
        if (mark[p] == h)
            lblk[nl++] = p;
    n0 = f->nins;
    /* what the loop makes and something outside it reads (but a phi where the loop leaves, which gets the copy's
       value on the copy's edge): through a slot */
    {
        u32 *sl = (u32 *)xmalloc((size_t)n0 * sizeof(u32)), b, n, j;
        memset(sl, 0, (size_t)n0 * sizeof(u32));
#define UNSW_IN(x) ((x) < n0 && f->ins[x].blk && f->ins[x].blk < nb0 && mark[f->ins[x].blk] == h && \
                    f->ins[x].op != OP_CONST && f->ins[x].op != OP_FCONST && f->ins[x].op != OP_ADDR && f->ins[x].op != OP_SLOT)
#define UNSW_SLOT(x) \
    do { \
        if (!sl[x]) { \
            int ty_ = f->ins[x].ty; \
            u32 st_; \
            u32 sz_ = M_SIZE(unsw_mem(ty_)); \
            sl[x] = 1 + ir_new_slot(f, sz_, sz_); \
            st_ = ir_raw(f, OP_STORE, IT_VOID, ir_slot_addr(f, sl[x] - 1, 0), x); \
            f->ins[st_].mem = (u8)unsw_mem(ty_); \
            if (f->ins[x].op == OP_PHI) { \
                if (f->blk[f->ins[x].blk].first) \
                    ir_insert_before(f, f->blk[f->ins[x].blk].first, st_); \
                else \
                    ir_append(f, f->ins[x].blk, st_); \
            } else if (f->ins[x].next) \
                ir_insert_before(f, f->ins[x].next, st_); \
            else \
                ir_append(f, f->ins[x].blk, st_); \
            *slots = 1; \
        } \
    } while (0)
#define UNSW_LOAD(x, ld) \
    do { \
        (ld) = ir_raw(f, OP_LOAD, f->ins[x].ty, ir_slot_addr(f, sl[x] - 1, 0), 0); \
        f->ins[ld].mem = (u8)unsw_mem(f->ins[x].ty); \
    } while (0)
        for (b = 1; b < nb0; b++) {
            u32 nx;
            if (mark[b] == h || f->blk[b].term == TERM_NONE)
                continue;
            for (v = f->blk[b].phis; v; v = f->ins[v].next)
                for (j = 0; j < f->ins[v].b; j++) {
                    u32 x = ir_resolve(f, f->xargs[f->ins[v].a + j]), ld, pb = f->blk[b].preds[j];
                    if (!UNSW_IN(x) || mark[pb] == h)
                        continue;
                    UNSW_SLOT(x);
                    UNSW_LOAD(x, ld);
                    ir_append(f, pb, ld);
                    f->xargs[f->ins[v].a + j] = ld;
                }
            for (v = f->blk[b].first; v; v = nx) {
                nx = f->ins[v].next;
                if (v >= n0)
                    continue; /* (a load just put in) */
                n = ir_opcount(f, v);
                for (j = 0; j < n; j++) {
                    u32 x = ir_resolve(f, *ir_opref(f, v, j)), ld;
                    if (!UNSW_IN(x))
                        continue;
                    UNSW_SLOT(x);
                    UNSW_LOAD(x, ld);
                    ir_insert_before(f, v, ld);
                    *ir_opref(f, v, j) = ld;
                }
                if (f->ins[v].op == OP_CALL) {
                    u32 x = ir_resolve(f, f->ins[v].c), ld;
                    if (UNSW_IN(x)) {
                        UNSW_SLOT(x);
                        UNSW_LOAD(x, ld);
                        ir_insert_before(f, v, ld);
                        f->ins[v].c = ld;
                    }
                }
            }
            if (f->blk[b].arg) {
                u32 x = ir_resolve(f, f->blk[b].arg), ld;
                if (UNSW_IN(x)) {
                    UNSW_SLOT(x);
                    UNSW_LOAD(x, ld);
                    ir_append(f, b, ld);
                    f->blk[b].arg = ld;
                }
            }
        }
        free(sl);
    }
    n0 = f->nins;
    vmap = (u32 *)xmalloc((size_t)n0 * sizeof(u32));
    memset(vmap, 0, (size_t)n0 * sizeof(u32));
#define UNSW_MAP(x) ((x) < n0 && vmap[x] ? vmap[x] : (x))
    /* the block between: where the value is tested */
    N = ir_new_block(f);
    f->blk[N].freq = f->blk[pre].freq < f->blk[h].freq || !f->blk[h].freq ? f->blk[pre].freq : f->blk[h].freq;
    ir_add_pred(f, N, pre);
    for (q = 0; q < 2; q++)
        if (f->blk[pre].succ[q] == h && (q == 0 || f->blk[pre].term == TERM_BR))
            f->blk[pre].succ[q] = N;
    for (p = 0; p < f->blk[h].npreds; p++)
        if (f->blk[h].preds[p] == pre)
            f->blk[h].preds[p] = N;
    for (k = 0; k < nb0; k++)
        bmap[k] = k;
    for (k = 0; k < nl; k++) {
        u32 nb = ir_new_block(f);
        bmap[lblk[k]] = nb;
        f->blk[nb].freq = f->blk[lblk[k]].freq;
        f->blk[nb].lhdr = f->blk[lblk[k]].lhdr;
        f->blk[nb].nounroll = f->blk[lblk[k]].nounroll;
    }
    for (k = 0; k < nl; k++) {
        u32 ob = lblk[k], nb = bmap[ob];
        for (p = 0; p < f->blk[ob].npreds; p++)
            ir_add_pred(f, nb, f->blk[ob].preds[p] < nb0 ? bmap[f->blk[ob].preds[p]] : f->blk[ob].preds[p]); /* (N is its own) */
        for (v = f->blk[ob].phis; v; v = f->ins[v].next) {
            u32 np = ir_phi(f, nb, f->ins[v].ty);
            f->ins[np].mem = f->ins[v].mem;
            f->ins[np].flags = f->ins[v].flags;
            f->ins[np].var = f->ins[v].var;
            vmap[v] = np;
            done++;
        }
        for (v = f->blk[ob].first; v; v = f->ins[v].next) {
            u32 nv;
            if (f->ins[v].op == OP_CALL) {
                u32 na = f->ins[v].b, *tmp = (u32 *)xmalloc((size_t)(na + 1) * sizeof(u32)), cur = f->cur;
                memcpy(tmp, f->xargs + f->ins[v].a, (size_t)na * sizeof(u32));
                f->cur = 0;
                nv = ir_call(f, f->ins[v].ty, f->ins[v].c, tmp, na, f->xargs[f->ins[v].a - 1], f->ins[v].flags);
                f->cur = cur;
                free(tmp);
            } else if (f->ins[v].op == OP_XI) {
                u32 na = f->ins[v].b, *tmp = (u32 *)xmalloc((size_t)(na + 1) * sizeof(u32)), cur = f->cur;
                memcpy(tmp, f->xargs + f->ins[v].a, (size_t)na * sizeof(u32));
                f->cur = 0;
                nv = ir_xi(f, f->ins[v].ty, f->ins[v].c, tmp, na);
                f->cur = cur;
                free(tmp);
            } else {
                nv = ir_raw(f, f->ins[v].op, f->ins[v].ty, f->ins[v].a, f->ins[v].b);
                f->ins[nv].c = f->ins[v].c;
            }
            f->ins[nv].mem = f->ins[v].mem;
            f->ins[nv].flags = f->ins[v].flags;
            f->ins[nv].loc = f->ins[v].loc;
            f->ins[nv].var = f->ins[v].var;
            ir_append(f, nb, nv);
            vmap[v] = nv;
            done++;
        }
    }
    for (k = 0; k < nl; k++) {
        u32 ob = lblk[k], nb = bmap[ob], n, j;
        for (v = f->blk[ob].phis; v; v = f->ins[v].next)
            for (j = 0; j < f->ins[v].b; j++) {
                u32 x = ir_resolve(f, f->xargs[f->ins[v].a + j]);
                f->xargs[f->ins[vmap[v]].a + j] = UNSW_MAP(x);
            }
        for (v = f->blk[nb].first; v; v = f->ins[v].next) {
            n = ir_opcount(f, v);
            for (j = 0; j < n; j++) {
                u32 x = ir_resolve(f, *ir_opref(f, v, j));
                *ir_opref(f, v, j) = UNSW_MAP(x);
            }
            if (f->ins[v].op == OP_CALL) {
                u32 x = ir_resolve(f, f->ins[v].c);
                f->ins[v].c = UNSW_MAP(x);
            }
        }
        f->blk[nb].term = f->blk[ob].term;
        if (f->blk[ob].arg) {
            u32 x = ir_resolve(f, f->blk[ob].arg);
            f->blk[nb].arg = UNSW_MAP(x);
        }
        n = nsuccs(&f->blk[ob]);
        for (j = 0; j < n; j++) {
            u32 s2 = f->blk[ob].succ[j];
            f->blk[nb].succ[j] = bmap[s2];
            if (mark[s2] != h && !(j == 1 && f->blk[ob].succ[0] == s2)) {
                /* a way out: the block there is come to from the copy too */
                u32 pi = pred_index(s2, ob);
                ir_add_pred(f, s2, nb);
                for (v = f->blk[s2].phis; v; v = f->ins[v].next) {
                    u32 x = ir_resolve(f, f->xargs[f->ins[v].a + pi]);
                    ir_phi_add_arg(f, v, UNSW_MAP(x));
                }
            }
        }
    }
    /* the test, and each loop's own side of the branches */
    f->blk[N].term = TERM_BR;
    f->blk[N].arg = c;
    f->blk[N].succ[0] = h;
    f->blk[N].succ[1] = bmap[h];
    for (k = 0; k < nl; k++) {
        int copy;
        for (copy = 0; copy < 2; copy++) {
            u32 b = copy ? bmap[lblk[k]] : lblk[k], keep, drop;
            Block *bb = &f->blk[b];
            int inv;
            if (bb->term != TERM_BR || bb->succ[0] == bb->succ[1] || !bb->arg || unsw_base(bb->arg, &inv) != c)
                continue;
            /* (the first loop runs where c is not 0) */
            keep = ((copy == 0) != (inv != 0)) ? 0 : 1;
            drop = bb->succ[!keep];
            keep = bb->succ[keep];
            bb->term = TERM_JMP;
            bb->arg = 0;
            bb->succ[0] = keep;
            bb->succ[1] = 0;
            remove_edge(b, drop);
        }
    }
    if (getenv("MCC_UNSWLOG"))
        fprintf(stderr, "unswitch %s: loop at b%lu, %lu instructions, %lu branches on v%lu\n", f->name ? f->name : "?",
                (unsigned long)h, (unsigned long)best_size, (unsigned long)best_n, (unsigned long)c);
    free(vmap);
    free(mark);
    return done ? done : 1;
}
#undef UNSW_IN
#undef UNSW_SLOT
#undef UNSW_LOAD
#undef UNSW_MAP

static void unswitch(Func *fn)
{
    static int off = -1, minbr = 2, per = UNSW_PER;
    u32 room = UNSW_TOTAL, n;
    int slots = 0, any = 0, rounds = 0;
    if (off < 0) {
        off = getenv("MCC_NOUNSWITCH") != 0;
        if (getenv("MCC_UNSWMIN"))
            minbr = atoi(getenv("MCC_UNSWMIN"));
        unsw_nocmp = getenv("MCC_UNSWCMP") == 0;
        if (getenv("MCC_UNSWPER"))
            per = atoi(getenv("MCC_UNSWPER"));
    }
    if (off)
        return;
    f = fn;
    if (getenv("MCC_UNSWSKIP") && f->name && strstr(getenv("MCC_UNSWSKIP"), f->name))
        return; /* (debugging: not in the functions named) */
    ir_resolve_all(f);
    while (rounds++ < 6 && (n = unswitch_one(room, (u32)minbr, (u32)per, &slots)) != 0) {
        any = 1;
        room = n < room ? room - n : 0;
        if (getenv("MCC_UNSWLOG") && atoi(getenv("MCC_UNSWLOG")) > 3)
            ir_dump(f);
        f->copies = 1;
        ir_resolve_all(f);
        compute_dom(); /* (simplify_cfg's forward_phi_blocks reads it: the copy's blocks have none yet, stale ones of
                          earlier block numbers made a copied header seem not to dominate its loop, and its phis went) */
        simplify_cfg();
        ir_resolve_all(f);
        opt_dce(f);
        f = fn;
    }
    if (!any)
        return;
    if (slots) {
        opt_promote(f);
        f->copies = 1;
        ir_resolve_all(f);
        f = fn;
    }
    gvn();
    ir_resolve_all(f);
    compute_dom();
    if (simplify_cfg()) {
        ir_resolve_all(f);
        gvn();
        ir_resolve_all(f);
    }
    opt_dce(f);
    /* a loop of one block that is now come to straight from a branch (the test before it went: it was on the value)
       gets a block of its own before it, which the vectoriser wants (opt_vec.c, try_loop) */
    {
        u32 l, nb0 = f->nblk;
        for (l = 2; l < nb0; l++) {
            Block *lb = &f->blk[l];
            u32 P, N, q, pi;
            if (lb->term != TERM_BR || lb->npreds != 2 || (lb->succ[0] != l && lb->succ[1] != l) || lb->succ[0] == lb->succ[1])
                continue;
            pi = lb->preds[0] == l ? 1 : 0;
            P = lb->preds[pi];
            if (P == l || f->blk[P].term != TERM_BR || f->blk[P].succ[0] == f->blk[P].succ[1])
                continue;
            N = ir_new_block(f);
            f->blk[N].freq = f->blk[P].freq < f->blk[l].freq || !f->blk[l].freq ? f->blk[P].freq : f->blk[l].freq;
            f->blk[N].term = TERM_JMP;
            f->blk[N].succ[0] = l;
            ir_add_pred(f, N, P);
            for (q = 0; q < 2; q++)
                if (f->blk[P].succ[q] == l)
                    f->blk[P].succ[q] = N;
            f->blk[l].preds[pi] = N;
        }
    }
}

static void iv_reduce(Func *fn);

void opt_finish(Func *fn)
{
    u32 saved = ir_loc;
    ir_loc = 0;
    finish_passes(fn);
    if (opt_arch == ARCH_X64 && !opt_dev_fn && !opt_cuda && opt_level >= 2) {
        iv_widen(fn);
        iv_reduce(fn);
        addr_shape(fn);
        br_bools(fn);
    }
    ir_loc = saved;
}

/* x86-64 host, last before selection (the vectoriser and the inliner know a loop by its 32-bit counter): a 32-bit
   counter i that the loop reads widened ((long)i: an index) becomes a 64-bit counter, and i its low half, when
   i + step is known not to wrap: the loop goes round only while i + step < n (so the next i + 1 fits; the first i
   is a constant, known small, or was compared the same way before the loop), or the header leaves unless i < n.
   The widening was an instruction every trip (movslq). MCC_NOIVWIDEN: off; MCC_IVWLOG: what was done. */
#define IVW_MAX 256

/* what holds of x on the edge taken when the condition c is true (taken) or false: 1 x < *o, 2 x <= *o, 3 x > *o,
   4 x >= *o (32-bit, signed), 0 nothing known */
static int ivw_rel(u32 c, int taken, u32 x, u32 *o)
{
    Ins *i = &f->ins[c];
    u32 a, b;
    int r;
    if (i->op < OP_SLT || i->op > OP_SGE)
        return 0;
    a = ir_resolve(f, i->a);
    b = ir_resolve(f, i->b);
    if (f->ins[a].ty != IT_I32)
        return 0;
    r = i->op - OP_SLT + 1;
    if (a == x) {
        *o = b;
    } else if (b == x) {
        *o = a;
        r = r <= 2 ? r + 2 : r - 2;
    } else {
        return 0;
    }
    if (!taken)
        r = 5 - r;
    return r;
}

/* x + c does not wrap, given what ivw_rel said of x */
static int ivw_fits(int rel, u32 o, i64 c)
{
    u64 k;
    int konst = ir_is_const(f, o, &k);
    i64 kv = (i64)(i32)(u32)k;
    if (c > 0) {
        if (rel == 1)
            return konst ? kv - 1 + c <= 0x7fffffffL : c == 1;
        return rel == 2 && konst && kv + c <= 0x7fffffffL;
    }
    if (rel == 3)
        return konst ? kv + 1 + c >= -0x7fffffffL - 1 : c == -1;
    return rel == 4 && konst && kv + c >= -0x7fffffffL - 1;
}

static void iv_widen(Func *fn)
{
    static int off = -1, log, nonsw;
    struct Cand {
        u32 h, phi, next, pre, lat, uses, w, wn, li; /* li: which of the header's predecessors is the way back */
        i64 c;
        int nonneg;
    } cand[IVW_MAX];
    u32 nc = 0, k, b, v, nx, *role, *hit, *hitk, *hitr, nhit = 0, nins0, done = 0;
    if (off < 0) {
        off = getenv("MCC_NOIVWIDEN") != 0;
        log = getenv("MCC_IVWLOG") != 0;
        nonsw = getenv("MCC_NONSWIV") != 0;
    }
    if (off || fn->nins > 400000)
        return;
    f = fn;
    compute_dom();
    for (k = 0; k < nrpo && nc < IVW_MAX; k++) {
        u32 h = rpo[k], pi, li, pre, lat, pj, nback = 0;
        Block *hb = &f->blk[h];
        /* one way back, and one way in or several (the loop left over after a vector loop is entered from before
           that and from after it) */
        if (hb->npreds < 2 || hb->npreds > 6)
            continue;
        for (pj = 0, li = 0; pj < hb->npreds; pj++) {
            if (rpo_num[hb->preds[pj]] == 0xffffffffU)
                break;
            if (dominates(h, hb->preds[pj]))
                li = pj, nback++;
        }
        if (pj < hb->npreds || nback != 1)
            continue;
        pi = li ? 0 : 1; /* (the one way in, where there is one) */
        pre = hb->preds[pi];
        lat = hb->preds[li];
        for (v = hb->phis; v && nc < IVW_MAX; v = f->ins[v].next) {
            Ins *i = &f->ins[v], *ni;
            u32 init, next, o = 0, x, g, steps;
            u64 cv, iv;
            i64 c;
            int ok = 0, rel, nonneg;
            if (i->ty != IT_I32 || i->b != hb->npreds)
                continue;
            init = ir_resolve(f, f->xargs[i->a + pi]);
            next = ir_resolve(f, f->xargs[i->a + li]);
            ni = &f->ins[next];
            if (ni->op != OP_ADD || ni->ty != IT_I32 || !ni->blk)
                continue;
            x = ir_resolve(f, ni->a) == v ? ir_resolve(f, ni->b) : ir_resolve(f, ni->b) == v ? ir_resolve(f, ni->a) : 0;
            if (!x || !ir_is_const(f, x, &cv) || !(u32)cv)
                continue;
            c = (i64)(i32)(u32)cv;
            /* the way back taken only while next is bounded, and the first value bounded too */
            if (f->blk[lat].term == TERM_BR && f->blk[lat].succ[0] != f->blk[lat].succ[1]) {
                rel = ivw_rel(ir_resolve(f, f->blk[lat].arg), f->blk[lat].succ[0] == h, next, &o);
                if (rel && ivw_fits(rel, o, c)) {
                    for (pj = 0, ok = 1; pj < hb->npreds && ok; pj++) {
                        u32 in = ir_resolve(f, f->xargs[i->a + pj]);
                        if (pj == li)
                            continue;
                        if (ir_is_const(f, in, &iv)) {
                            i64 s = (i64)(i32)(u32)iv + c;
                            ok = s <= 0x7fffffffL && s >= -0x7fffffffL - 1;
                        } else if (ir_umax32(f, in) < 0x80000000UL) {
                            ok = c < 0 || ir_umax32(f, in) + (u64)c <= 0x7fffffffUL;
                        } else {
                            ok = 0;
                        }
                    }
                    for (g = pre, steps = 0; !ok && hb->npreds == 2 && steps < 6 && f->blk[g].npreds == 1; steps++) {
                        u32 q = f->blk[g].preds[0];
                        Block *qb = &f->blk[q];
                        if (qb->term == TERM_BR && qb->succ[0] != qb->succ[1]) {
                            rel = ivw_rel(ir_resolve(f, qb->arg), qb->succ[0] == g, init, &o);
                            if (rel && ivw_fits(rel, o, c))
                                ok = 1;
                        }
                        g = q;
                    }
                }
            }
            /* or the header leaves unless the counter is bounded, and next is made past that test */
            if (!ok && hb->term == TERM_BR && hb->succ[0] != hb->succ[1]) {
                u32 sk;
                for (sk = 0; sk < 2 && !ok; sk++) {
                    u32 sb = hb->succ[sk];
                    if (f->blk[sb].npreds != 1 || rpo_num[sb] == 0xffffffffU || !dominates(sb, ni->blk))
                        continue;
                    rel = ivw_rel(ir_resolve(f, hb->arg), sk == 0, v, &o);
                    ok = rel && ivw_fits(rel, o, c);
                }
            }
            /* or the program added there itself, as ints (IF_NSW): the sum did not wrap. MCC_NONSWIV: not looked at */
            if (!ok && (ni->flags & IF_NSW) && !opt_wrapv && !nonsw)
                ok = 1;
            if (log && !ok)
                fprintf(stderr, "ivwiden: %s block %lu not bounded (next op flags %d)\n", f->name, (unsigned long)h, ni->flags);
            if (!ok)
                continue;
            nonneg = c > 0;
            for (pj = 0; pj < hb->npreds && nonneg; pj++) {
                u32 in = ir_resolve(f, f->xargs[i->a + pj]);
                if (pj != li)
                    nonneg = ir_is_const(f, in, &iv) ? !((u32)iv >> 31) : ir_umax32(f, in) < 0x80000000UL;
            }
            cand[nc].li = li;
            cand[nc].h = h;
            cand[nc].phi = v;
            cand[nc].next = next;
            cand[nc].pre = pre;
            cand[nc].lat = hb->npreds == 2 ? pi : 0xffffffffU; /* (the index of the one entry among the header's predecessors) */
            cand[nc].uses = 0;
            cand[nc].c = c;
            cand[nc].nonneg = nonneg;
            nc++;
        }
    }
    if (!nc)
        return;
    nins0 = f->nins;
    role = (u32 *)xmalloc(nins0 * 4 * sizeof(u32));
    hit = role + nins0;
    hitk = hit + nins0;
    hitr = hitk + nins0;
    memset(role, 0, nins0 * sizeof(u32));
    for (k = 0; k < nc; k++) {
        if (role[cand[k].phi] || role[cand[k].next])
            cand[k].h = 0; /* (a value that is the next of two counters: left alone) */
        else
            role[cand[k].phi] = 2 * k + 2, role[cand[k].next] = 2 * k + 3;
    }
    for (b = 1; b < f->nblk; b++) {
        if (f->blk[b].term == TERM_NONE)
            continue;
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            u32 a;
            if (i->op != OP_SEXT32 && i->op != OP_ZEXT32)
                continue;
            a = ir_resolve(f, i->a);
            hitk[nhit] = 0;
            if (a < nins0 && !role[a] && i->op == OP_SEXT32 && f->ins[a].op == OP_ADD && f->ins[a].ty == IT_I32) {
                /* (long)(i - k): a counter that goes up from a first value not below k - INT_MIN stays above it */
                u32 x = ir_resolve(f, f->ins[a].a);
                u64 kv, iv;
                struct Cand *cd;
                if (x >= nins0 || !role[x] || !ir_is_const(f, ir_resolve(f, f->ins[a].b), &kv) || !((u32)kv >> 31))
                    continue;
                cd = &cand[role[x] / 2 - 1];
                if (!cd->h || cd->c < 0)
                    continue;
                if (!cd->nonneg && !(cd->lat != 0xffffffffU && ir_is_const(f, ir_resolve(f, f->xargs[f->ins[cd->phi].a + cd->lat]), &iv) &&
                                     (i64)(i32)(u32)iv + (i64)(i32)(u32)kv >= -0x7fffffffL - 1))
                    continue;
                hitk[nhit] = (u32)kv;
                a = x;
            }
            if (a >= nins0 || !role[a] || !cand[role[a] / 2 - 1].h || (i->op == OP_ZEXT32 && !cand[role[a] / 2 - 1].nonneg))
                continue;
            hitr[nhit] = role[a];
            cand[role[a] / 2 - 1].uses++;
            hit[nhit++] = v;
        }
    }
    for (k = 0; k < nc; k++) {
        u32 h = cand[k].h, w, wn, s, t, tn, init, pj;
        u64 iv;
        if (!h || !cand[k].uses)
            continue;
        w = ir_phi(f, h, IT_I64);
        wn = ir_raw(f, OP_ADD, IT_I64, w, ir_const(f, IT_I64, (u64)cand[k].c));
        ir_insert_before(f, cand[k].next, wn);
        for (pj = 0; pj < f->blk[h].npreds; pj++) {
            if (pj == cand[k].li) {
                f->xargs[f->ins[w].a + pj] = wn;
                continue;
            }
            init = ir_resolve(f, f->xargs[f->ins[cand[k].phi].a + pj]);
            if (ir_is_const(f, init, &iv)) {
                s = ir_const(f, IT_I64, (u64)(i64)(i32)(u32)iv);
            } else {
                s = ir_raw(f, OP_SEXT32, IT_I64, init, 0);
                ir_append(f, f->blk[h].preds[pj], s);
            }
            f->xargs[f->ins[w].a + pj] = s;
        }
        tn = ir_raw(f, OP_TRUNC, IT_I32, wn, 0);
        ir_insert_before(f, cand[k].next, tn);
        t = ir_raw(f, OP_TRUNC, IT_I32, w, 0);
        if (f->blk[h].first)
            ir_insert_before(f, f->blk[h].first, t);
        else
            ir_append(f, h, t);
        cand[k].w = w;
        cand[k].wn = wn;
        cand[k].pre = t;  /* (from here on: the two low halves) */
        cand[k].lat = tn;
        done++;
        if (log)
            fprintf(stderr, "ivwiden: %s block %u step %ld, %u widenings\n", f->name ? f->name : "?", h, (long)cand[k].c,
                    cand[k].uses);
    }
    for (k = 0; k < nhit; k++) {
        u32 r = hitr[k], with;
        if (!cand[r / 2 - 1].uses)
            continue;
        with = r & 1 ? cand[r / 2 - 1].wn : cand[r / 2 - 1].w;
        if (hitk[k]) {
            with = ir_raw(f, OP_ADD, IT_I64, with, ir_const(f, IT_I64, (u64)(i64)(i32)hitk[k]));
            ir_insert_before(f, hit[k], with);
        }
        ir_replace(f, hit[k], with);
    }
    for (k = 0; k < nc; k++) {
        if (!cand[k].h || !cand[k].uses)
            continue;
        ir_replace(f, cand[k].next, cand[k].lat);
        nx = cand[k].phi;
        unlink_phi(cand[k].h, nx, cand[k].pre);
    }
    free(role);
    if (done) {
        ir_resolve_all(f);
        opt_dce(f);
    }
}

/* x86-64 host, after iv_widen: strength reduction. In a loop with a 64-bit counter p (p, p + c, ...), a value made
   from p by additions, subtractions, constant shifts and constant products with values fixed for the loop is
   K * p + B; where K is not a scale the selector folds into an address (+-1, 2, 4, 8), the loop's uses of it take a
   counter of its own instead: worked out once in front of the loop (the same instructions, the first p in place of p)
   and advanced by K * c each time round. Exact, as all of it is arithmetic mod 2^64. matmul's b[k][j] was
   k * 960 (shl, sub, shl, add) every trip. MCC_NOIVSR: off. */
#define SR_MAX 4
static u8 *sr_in;     /* block in the loop */
static u8 *sr_lin;    /* value linear in the counter */
static i64 *sr_k;     /* its factor */
static u32 *sr_clone; /* its copy in front of the loop (for the root being done) */

static int sr_inv(u32 v)
{
    return !f->ins[v].blk || !sr_in[f->ins[v].blk];
}

static u32 sr_copy(u32 v, u32 p, u32 init, u32 pre, u32 nins0)
{
    Ins *i;
    u32 a, b, n;
    v = ir_resolve(f, v);
    if (v == p)
        return init;
    if (v >= nins0 || sr_inv(v))
        return v;
    if (sr_clone[v])
        return sr_clone[v];
    i = &f->ins[v];
    a = sr_copy(i->a, p, init, pre, nins0);
    b = sr_copy(i->b, p, init, pre, nins0);
    {
        u32 cur = f->cur;
        f->cur = pre;
        n = ir_ins(f, i->op, i->ty, a, b); /* (folded where it can be: the first p is often 0) */
        f->cur = cur;
    }
    sr_clone[v] = n;
    return n;
}

/* the instructions of the loop that make v from the counter (at most 8 counted) */
static u32 sr_cost(u32 v, u32 p, u32 nins0, int depth)
{
    Ins *i;
    u32 n;
    v = ir_resolve(f, v);
    if (v == p || v >= nins0 || sr_inv(v) || depth > 8)
        return 0;
    i = &f->ins[v];
    n = 1 + sr_cost(i->a, p, nins0, depth + 1);
    if (i->op == OP_ADD || i->op == OP_SUB)
        n += sr_cost(i->b, p, nins0, depth + 1);
    return n;
}

static void iv_reduce(Func *fn)
{
    static int off = -1, log;
    u32 k, h, nins0, done = 0;
    if (off < 0) {
        off = getenv("MCC_NOIVSR") != 0;
        log = getenv("MCC_IVSRLOG") != 0;
    }
    if (off || fn->nins > 400000)
        return;
    f = fn;
    compute_dom();
    nins0 = f->nins;
    sr_in = (u8 *)xmalloc(f->nblk + nins0);
    sr_lin = sr_in + f->nblk;
    sr_k = (i64 *)xmalloc(nins0 * sizeof(i64));
    sr_clone = (u32 *)xmalloc(nins0 * sizeof(u32));
    for (k = 0; k < nrpo; k++) {
        u32 pi, li, pre, lat, ph, *stack, sp = 0, b, v, j, nroot = 0, roots[SR_MAX];
        Block *hb;
        h = rpo[k];
        hb = &f->blk[h];
        if (hb->npreds != 2 || rpo_num[hb->preds[0]] == 0xffffffffU || rpo_num[hb->preds[1]] == 0xffffffffU)
            continue;
        li = dominates(h, hb->preds[0]) ? 0 : 1;
        pi = 1 - li;
        lat = hb->preds[li];
        pre = hb->preds[pi];
        if (!dominates(h, lat) || dominates(h, pre))
            continue;
        /* the loop: what reaches the way back without passing h */
        memset(sr_in, 0, f->nblk);
        sr_in[h] = 1;
        stack = (u32 *)xmalloc(f->nblk * sizeof(u32));
        if (!sr_in[lat])
            sr_in[lat] = 1, stack[sp++] = lat;
        while (sp) {
            b = stack[--sp];
            for (j = 0; j < f->blk[b].npreds; j++) {
                u32 q = f->blk[b].preds[j];
                if (!sr_in[q] && rpo_num[q] != 0xffffffffU)
                    sr_in[q] = 1, stack[sp++] = q;
            }
        }
        free(stack);
        for (ph = hb->phis; ph && nroot < SR_MAX; ph = f->ins[ph].next) {
            Ins *pi_ = &f->ins[ph], *ni;
            u32 next, init, x, r;
            u64 cv;
            i64 c;
            if (pi_->ty != IT_I64 || pi_->b != 2)
                continue;
            next = ir_resolve(f, f->xargs[pi_->a + li]);
            init = ir_resolve(f, f->xargs[pi_->a + pi]);
            ni = &f->ins[next];
            if (next >= nins0 || ni->op != OP_ADD || ni->ty != IT_I64 || !ni->blk || !sr_in[ni->blk])
                continue;
            x = ir_resolve(f, ni->a) == ph ? ir_resolve(f, ni->b) : ir_resolve(f, ni->b) == ph ? ir_resolve(f, ni->a) : 0;
            if (!x || !ir_is_const(f, x, &cv) || !cv)
                continue;
            c = (i64)cv;
            /* the linear values, in order (an operand is made before its use, except through phis) */
            memset(sr_lin, 0, nins0);
            sr_lin[ph] = 1;
            sr_k[ph] = 1;
            for (j = 0; j < nrpo; j++) {
                b = rpo[j];
                if (!sr_in[b])
                    continue;
                for (v = f->blk[b].first; v; v = f->ins[v].next) {
                    Ins *i = &f->ins[v];
                    u32 a, bb;
                    u64 kc;
                    if (v >= nins0 || i->ty != IT_I64)
                        continue;
                    if (i->op == OP_ADD || i->op == OP_SUB) {
                        a = ir_resolve(f, i->a);
                        bb = ir_resolve(f, i->b);
                        if (a >= nins0 || bb >= nins0 || (!sr_lin[a] && !sr_inv(a)) || (!sr_lin[bb] && !sr_inv(bb)) ||
                            (!sr_lin[a] && !sr_lin[bb]))
                            continue;
                        sr_k[v] = (i64)((u64)(sr_lin[a] ? sr_k[a] : 0) +
                                        (i->op == OP_ADD ? 1 : (u64)-1) * (u64)(sr_lin[bb] ? sr_k[bb] : 0));
                        sr_lin[v] = 1;
                    } else if ((i->op == OP_SHL || i->op == OP_MUL) && (a = ir_resolve(f, i->a)) < nins0 && sr_lin[a] &&
                               ir_is_const(f, ir_resolve(f, i->b), &kc) && (i->op == OP_MUL || kc < 64)) {
                        sr_k[v] = (i64)(i->op == OP_SHL ? (u64)sr_k[a] << kc : (u64)sr_k[a] * kc);
                        sr_lin[v] = 1;
                    }
                }
            }
            /* the roots: linear values with a factor no address takes, used in the loop by what is not linear */
            for (j = 0; j < nrpo && nroot < SR_MAX; j++) {
                b = rpo[j];
                if (!sr_in[b])
                    continue;
                for (v = f->blk[b].first; v && nroot < SR_MAX; v = f->ins[v].next) {
                    Ins *i = &f->ins[v];
                    u32 *ops, nops, o;
                    if (v >= nins0 || sr_lin[v] || i->op == OP_PHI)
                        continue;
                    IR_OPS(f, i, ops, nops);
                    for (o = 0; o < nops && nroot < SR_MAX; o++) {
                        u32 q = ir_resolve(f, ops[o]), z;
                        u64 kk;
                        if (q >= nins0 || !sr_lin[q] || q == ph || q == next)
                            continue;
                        /* (x + 4, x + 8, ...: the fields of one record) x is the root, and the additions stay */
                        while (f->ins[q].op == OP_ADD) {
                            u32 a0 = ir_resolve(f, f->ins[q].a), b0 = ir_resolve(f, f->ins[q].b), o2;
                            u64 kc;
                            if (ir_is_const(f, b0, &kc))
                                o2 = a0;
                            else if (ir_is_const(f, a0, &kc))
                                o2 = b0;
                            else
                                break;
                            if (o2 >= nins0 || !sr_lin[o2] || o2 == ph || o2 == next)
                                break;
                            q = o2;
                        }
                        kk = (u64)sr_k[q];
                        if ((i64)kk < 0)
                            kk = (u64)0 - kk; /* (b - p * 4 is a subtraction and an address: queens' diagonals) */
                        if (!kk || kk == 1 || kk == 2 || kk == 4 || kk == 8)
                            continue;
                        if (f->ins[q].op != OP_MUL && sr_cost(q, ph, nins0, 0) < 2)
                            continue; /* (one shift for one addition and a register) */
                        for (z = 0; z < nroot && roots[z] != q; z++)
                            ;
                        if (z == nroot)
                            roots[nroot++] = q;
                    }
                }
            }
            for (r = 0; r < nroot; r++) {
                u32 R = roots[r], q, qn, st, args[2];
                memset(sr_clone, 0, nins0 * sizeof(u32));
                st = sr_copy(R, ph, init, pre, nins0);
                q = ir_phi(f, h, IT_I64);
                qn = ir_raw(f, OP_ADD, IT_I64, q, ir_const(f, IT_I64, (u64)sr_k[R] * (u64)c));
                ir_append(f, lat, qn);
                args[pi] = st;
                args[li] = qn;
                ir_phi_set_args(f, q, args, 2);
                /* the loop's uses of R take q (it is R's value wherever the loop is) */
                for (j = 0; j < nrpo; j++) {
                    b = rpo[j];
                    if (!sr_in[b])
                        continue;
                    for (v = f->blk[b].first; v; v = f->ins[v].next) {
                        Ins *i = &f->ins[v];
                        u32 *ops, nops, o;
                        if (v >= nins0)
                            continue;
                        IR_OPS(f, i, ops, nops);
                        for (o = 0; o < nops; o++)
                            if (ir_resolve(f, ops[o]) == R)
                                ops[o] = q;
                    }
                    for (v = f->blk[b].phis; v; v = f->ins[v].next) {
                        Ins *i = &f->ins[v];
                        u32 o;
                        if (v >= nins0 || v == q)
                            continue;
                        for (o = 0; o < i->b; o++)
                            if (ir_resolve(f, f->xargs[i->a + o]) == R)
                                f->xargs[i->a + o] = q;
                    }
                }
                done++;
                if (log)
                    fprintf(stderr, "ivsr: %s block %u: factor %ld step %ld\n", f->name ? f->name : "?", h,
                            (long)sr_k[R], (long)c);
            }
            sr_k[0] = 0;
        }
    }
    free(sr_in);
    free(sr_k);
    free(sr_clone);
    if (done) {
        ir_resolve_all(f);
        opt_dce(f);
    }
}

/* x86-64 host, last before selection: the address of a load or store that is a sum with constants in several
   places is put together again as ((the terms) + one constant) + the index, the shape the selector takes as
   base + index * scale + displacement. A constant inside a product counts ((x + k) * c is x * c + k * c, also
   under a widening when x is known small enough for x + k not to wrap). &p->models[5 + ctx].dt[st] in ufsm's
   volcomp decoder was add 5, imul, add p, add 258 once per symbol: now imul, add p, the rest in the address.
   Only sums whose inner nodes have no other use, and only when at least one addition goes away: every other
   address keeps its shape. MCC_NOADDRSHAPE: off. */
#define AS_MAX 6
static u32 as_leaf[AS_MAX], as_n, as_consts, as_bad;
static u64 as_k;
static u32 *as_uses, as_nuses;

/* a node of the sum that only this sum uses, in the root's block (what was moved out of a loop stays out) */
static int as_inner(u32 v, u32 blk)
{
    return v < as_nuses && as_uses[v] == 1 && f->ins[v].blk == blk;
}

static void as_collect(u32 v, u32 root, int depth)
{
    Ins *i = &f->ins[v];
    u64 c;
    if (as_bad)
        return;
    if (ir_is_const(f, v, &c)) {
        as_k += c;
        as_consts++;
        return;
    }
    if (i->op == OP_ADD && i->ty == IT_I64 && depth < 8 && (v == root || as_inner(v, f->ins[root].blk))) {
        u32 a = i->a, b = i->b;
        as_collect(a, root, depth + 1);
        as_collect(b, root, depth + 1);
        return;
    }
    if (as_n == AS_MAX) {
        as_bad = 1;
        return;
    }
    as_leaf[as_n++] = v;
}

/* The leaf v as c * (x + k), k a constant that can leave it: x, c (shift: as a shift count, or -1), k * c, and
   whether x is widened from 32 bits; 0 if v is not of that form. */
static int as_scaled(u32 v, u32 blk, u32 *x, u64 *mul, int *shift, u64 *kc, int *widen)
{
    Ins *i = &f->ins[v], *xi;
    u64 c = 1, k;
    u32 in = v;
    *shift = -1;
    *widen = 0;
    if (!as_inner(v, blk) || i->ty != IT_I64)
        return 0;
    if ((i->op == OP_MUL || i->op == OP_SHL) && ir_is_const(f, i->b, &c)) {
        if (i->op == OP_SHL) {
            if (c > 31)
                return 0;
            *shift = (int)c;
            c = (u64)1 << c;
        }
        in = i->a;
        if (!as_inner(in, blk))
            return 0;
    }
    xi = &f->ins[in];
    if (xi->op == OP_ZEXT32) {
        u32 y = xi->a;
        Ins *yi = &f->ins[y];
        if (!as_inner(y, blk) || yi->op != OP_ADD || yi->ty != IT_I32 || !ir_is_const(f, yi->b, &k))
            return 0;
        k &= 0xffffffffUL;
        if (ir_umax32(f, yi->a) >= 0x80000000UL || k >= 0x80000000UL)
            return 0; /* (the 32-bit sum may wrap) */
        *widen = 1;
        *x = yi->a;
    } else if (xi->op == OP_ADD && xi->ty == IT_I64 && in != v && ir_is_const(f, xi->b, &k)) {
        *x = xi->a;
    } else {
        return 0;
    }
    *mul = c;
    *kc = k * c;
    return 1;
}

static void addr_shape(Func *fn)
{
    static int off = -1;
    u32 b, v, k, changed = 0, nroot;
    u8 *root;
    if (off < 0)
        off = getenv("MCC_NOADDRSHAPE") != 0;
    if (off || fn->nins > 400000)
        return;
    f = fn;
    as_uses = count_uses(&as_nuses);
    /* the sums to look at: the addresses of loads and stores, and what such an address adds a constant to (the
       element whose fields are read one by one) */
    nroot = f->nins;
    root = (u8 *)xmalloc(nroot);
    memset(root, 0, nroot);
    for (b = 1; b < f->nblk; b++) {
        if (f->blk[b].term == TERM_NONE)
            continue;
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            u32 r;
            u64 c;
            if (f->ins[v].op != OP_LOAD && f->ins[v].op != OP_STORE)
                continue;
            r = f->ins[v].a;
            if (f->ins[r].op != OP_ADD || f->ins[r].ty != IT_I64)
                continue;
            root[r] = 1;
            if (ir_is_const(f, f->ins[r].b, &c) && f->ins[f->ins[r].a].op == OP_ADD && f->ins[f->ins[r].a].ty == IT_I64 &&
                f->ins[r].a < as_nuses && as_uses[f->ins[r].a] > 1)
                root[f->ins[r].a] = 1; /* (used once, it is part of r's sum) */
        }
    }
    for (b = 1; b < f->nblk; b++) {
        if (f->blk[b].term == TERM_NONE)
            continue;
        for (v = f->blk[b].first; v; v = f->ins[v].next) {
            u32 r = v, idx = 0xffffffffU, sum = 0, dist = 0, nu, kk;
            u32 lx[AS_MAX];
            u64 lm[AS_MAX], lk[AS_MAX];
            int lsh[AS_MAX], lw[AS_MAX], ld[AS_MAX];
            if (r >= nroot || !root[r] || f->ins[r].op != OP_ADD || f->ins[r].ty != IT_I64)
                continue;
            as_n = as_consts = as_bad = 0;
            as_k = 0;
            as_collect(r, r, 0);
            if (as_bad || !as_n)
                continue;
            for (k = 0; k < as_n; k++) {
                ld[k] = as_scaled(as_leaf[k], b, &lx[k], &lm[k], &lsh[k], &lk[k], &lw[k]);
                dist += (u32)ld[k];
            }
            /* worth it: constants in two places, or one inside a product */
            if (as_consts + dist < 2 && !dist)
                continue;
            /* the leaves that held a constant, made anew before the root without it */
            for (k = 0; k < as_n; k++) {
                u32 x, t;
                if (!ld[k])
                    continue;
                x = lx[k];
                if (lw[k]) {
                    t = ir_raw(f, OP_ZEXT32, IT_I64, x, 0);
                    ir_insert_before(f, r, t);
                    x = t;
                }
                if (lm[k] != 1) {
                    kk = lsh[k] >= 0 ? ir_const(f, IT_I64, (u64)lsh[k]) : ir_const(f, IT_I64, lm[k]);
                    t = ir_raw(f, lsh[k] >= 0 ? OP_SHL : OP_MUL, IT_I64, x, kk);
                    ir_insert_before(f, r, t);
                    x = t;
                }
                as_leaf[k] = x;
                as_k += lk[k];
            }
            /* the index: a leaf shifted by 1..3 if there is one, else the last of several */
            for (k = 0; k < as_n; k++) {
                Ins *li = &f->ins[as_leaf[k]];
                u64 sh;
                if (li->op == OP_SHL && ir_is_const(f, li->b, &sh) && sh >= 1 && sh <= 3)
                    idx = k;
            }
            if (idx == 0xffffffffU && as_n >= 2)
                idx = as_n - 1;
            for (k = 0; k < as_n; k++) {
                if (k == idx)
                    continue;
                if (!sum) {
                    sum = as_leaf[k];
                } else {
                    nu = ir_raw(f, OP_ADD, IT_I64, sum, as_leaf[k]);
                    ir_insert_before(f, r, nu);
                    sum = nu;
                }
            }
            kk = ir_const(f, IT_I64, as_k);
            if (!sum) {
                /* (one leaf) */
                f->ins[r].a = as_leaf[idx];
                f->ins[r].b = kk;
            } else if (idx == 0xffffffffU) {
                f->ins[r].a = sum;
                f->ins[r].b = kk;
            } else {
                if (as_k) {
                    nu = ir_raw(f, OP_ADD, IT_I64, sum, kk);
                    ir_insert_before(f, r, nu);
                    sum = nu;
                }
                f->ins[r].a = sum;
                f->ins[r].b = as_leaf[idx];
            }
            changed = 1;
        }
    }
    free(as_uses);
    free(root);
    as_uses = 0;
    if (changed) {
        /* (what the sums no longer use goes; a sum that came to "+ 0" is its other operand) */
        ir_resolve_all(f);
        gvn();
        ir_resolve_all(f);
        opt_dce(f);
    }
}

/* Device code: a load at a known place of a read-only object of this unit is the constant stored there. A table such as
       __device__ static const float FLW[4][3] = {{0.375f, 0.625f, 0.f}, ...};   ... FLW[RZ0 + dz][k] ...
   read with indices the unroller and the template arguments made constant was read through mcudahw's table of device
   globals and a generic load each time (ufsm's fstem_fwd_k: 78 LD.E and 32 more table reads in its MMA loop; nvcc folds them).
   The object's bytes are those of the unit's read-only section, with no relocation in the bytes read (an address stays a
   load). MCC_NOCLOAD: off. */
static int const_loads(void)
{
    static int off = -1;
    u32 b, v, nx, n = 0;
    if (off < 0)
        off = getenv("MCC_NOCLOAD") != 0;
    if (off || !opt_dev_fn || !opt_obj)
        return 0;
    for (b = 1; b < f->nblk; b++) {
        for (v = f->blk[b].first; v; v = nx) {
            Ins *i = &f->ins[v];
            u32 a, sz, k, sym;
            i64 at = 0;
            u64 c, bits = 0, base;
            ObjSym *os;
            const u8 *p;
            nx = i->next;
            if (i->op != OP_LOAD || i->mem > M_F64)
                continue;
            a = ir_resolve(f, i->a);
            for (k = 0; k < 8; k++) { /* (address + constant) chains */
                Ins *ai = &f->ins[a];
                if (ai->op != OP_ADD || ai->ty != IT_I64)
                    break;
                if (ir_is_const(f, ir_resolve(f, ai->b), &c))
                    a = ir_resolve(f, ai->a);
                else if (ir_is_const(f, ir_resolve(f, ai->a), &c))
                    a = ir_resolve(f, ai->b);
                else
                    break;
                at += (i64)c;
            }
            if (f->ins[a].op != OP_ADDR)
                continue;
            sym = f->ins[a].a;
            at += (i64)(i32)f->ins[a].b;
            if (sym >= opt_obj->nsyms)
                continue;
            os = &opt_obj->syms[sym];
            sz = M_SIZE(i->mem);
            if (os->sec != SEC_CONST || os->kind == 1 || !os->size || at < 0 || (u64)at + sz > os->size)
                continue;
            base = os->value + (u64)at;
            if (base + sz > opt_obj->sec[SEC_CONST].len)
                continue;
            for (k = 0; k < opt_obj->nrel[SEC_CONST]; k++) {
                const ObjReloc *r = &opt_obj->rel[SEC_CONST][k];
                if (r->off + 8 > base && r->off < base + sz)
                    break;
            }
            if (k < opt_obj->nrel[SEC_CONST])
                continue;
            p = opt_obj->sec[SEC_CONST].p + base;
            for (k = 0; k < sz; k++)
                bits |= (u64)p[k] << (8 * k);
            switch (i->mem) {
            case M_8S: bits = (u64)(i64)(i8)bits; break;
            case M_16S: bits = (u64)(i64)(i16)bits; break;
            case M_32: if (i->ty == IT_I64) bits = (u64)(i64)(i32)bits; break;
            default: break;
            }
            if (i->ty == IT_F32 || i->ty == IT_F64) {
                if ((i->ty == IT_F32) != (i->mem == M_F32) || (i->ty == IT_F64) != (i->mem == M_F64))
                    continue;
                ir_replace(f, v, ir_fconst_bits(f, i->ty, bits));
            } else if (i->ty == IT_I32 || i->ty == IT_I64) {
                if (i->mem == M_F32 || i->mem == M_F64)
                    continue;
                ir_replace(f, v, ir_const(f, i->ty, i->ty == IT_I32 ? (u64)(u32)bits : bits));
            } else {
                continue;
            }
            n++;
        }
    }
    return n != 0;
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
    if (const_loads())
        ir_resolve_all(f);
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
    if (const_loads()) {
        ir_resolve_all(f);
        gvn();
        ir_resolve_all(f);
    }
    if (opt_dev_fn && ptx_single_trip(f))
        ir_resolve_all(f);
    if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && opt_level >= 2)
        sink_calls();
    if (simplify_cfg()) {
        int rounds = opt_level >= 3 ? 6 : 1;
        do {
            ir_resolve_all(f);
            gvn();
        } while (simplify_cfg() && --rounds);
        ir_resolve_all(f);
    }
    if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && opt_level >= 2)
        unselect_calls();
    /* device code: the GPU's assembler moves what it finds worth moving; values moved out of a kernel's loops here
       stay in registers through all of it, and it runs out of them */
    if (opt_dev_fn && opt_dolicm >= 3 && dev_reassoc()) {
        opt_dce(f);
        gvn();
        ir_resolve_all(f);
    }
    if (opt_licm_cost < 0)
        opt_licm_cost = getenv("MCC_LICMCOST") ? atoi(getenv("MCC_LICMCOST")) : 1;
    if (opt_licm_phi < 0)
        opt_licm_phi = getenv("MCC_LICMPHI") ? atoi(getenv("MCC_LICMPHI")) : LICM_PHI_DEFAULT;
    if (!(opt_dev_fn && !opt_dolicm && !opt_licm_phi) && !(opt_cuda && opt_nolicm) && licm())
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
    /* (before the vectoriser: the scalar passes that tidy up after it do not know vector code) */
    if (!opt_dev_fn && !opt_cuda && opt_arch == ARCH_X64 && opt_level >= 2 && !opt_size) {
        unswitch(fn);
        f = fn;
    }
    if (opt_level >= 3 && !opt_size && !opt_novec && (opt_dce(f), opt_vectorise(f))) { /* dead code would only be in the way */
        opt_dce(f);
        return; /* vector code stays out of the inliner: the scalar passes do not know it */
    }
    opt_dce(f);
    if (opt_slotcheck)
        slot_check(fn, "end");
    inline_save(f);
}

/* x86-64 host: what is computed before a branch and read only on one side of it is computed on that side, where
   a call of a pure function (IF_PURE: atan2, hypot ...) is part of it: `w = f (x); ... a[i] = in ? g (w) : 255`
   called f also where in is 0 (clang sinks the same). The side: a successor of the block with that block as its
   only predecessor, dominating every read. Only pure values with no read left in their block move, the calls and
   what leads to them or from them; loads stay. MCC_NOSINK: off; MCC_SINKLOG: the count per function. */
static int sink_ok(const Ins *i)
{
    if (i->op == OP_CALL)
        return (i->flags & IF_PURE) && !(i->flags & (IF_ASM | IF_ATOMIC | IF_SRET | IF_ALLOCA));
    return (is_pure(i->op) && i->op != OP_ASMOUT && i->op != OP_TLS && i->op != OP_ISCONST) || i->op == OP_SDIV ||
           i->op == OP_UDIV || i->op == OP_SREM || i->op == OP_UREM || i->op == OP_SELECT;
}

static int sink_calls(void)
{
    u32 *total, *out, *lca, *cnt, *tt, *dest, B, v, j, n, *ops, nmoved = 0;
    if (getenv("MCC_NOSINK"))
        return 0;
    for (B = 1, n = 0; B < f->nblk && !n; B++)
        for (v = f->blk[B].first; v && !n; v = f->ins[v].next)
            n = f->ins[v].op == OP_CALL && sink_ok(&f->ins[v]);
    if (!n)
        return 0;
    ir_resolve_all(f);
    compute_dom();
    total = (u32 *)xmalloc(f->nins * 6 * sizeof(u32));
    out = total + f->nins;
    lca = out + f->nins;
    cnt = lca + f->nins;
    tt = cnt + f->nins;
    dest = tt + f->nins;
    memset(total, 0, f->nins * 6 * sizeof(u32));
#define SINK_USE(x, ub) \
    do { \
        u32 x_ = (x), d_ = f->ins[x_].blk, u_ = (ub); \
        if (d_) { \
            total[x_]++; \
            if (u_ != d_) { \
                if (!out[x_]++) { \
                    lca[x_] = u_; \
                } else { \
                    u32 l_ = lca[x_]; \
                    while (!dominates(l_, u_) && l_ != idom[l_]) \
                        l_ = idom[l_]; \
                    lca[x_] = l_; \
                } \
            } \
        } \
    } while (0)
    for (B = 1; B < f->nblk; B++) {
        Block *ub = &f->blk[B];
        if (ub->term == TERM_NONE)
            continue;
        for (v = ub->phis; v; v = f->ins[v].next)
            for (j = 0; j < f->ins[v].b && j < ub->npreds; j++)
                SINK_USE(f->xargs[f->ins[v].a + j], ub->preds[j]);
        for (v = ub->first; v; v = f->ins[v].next) {
            Ins *i = &f->ins[v];
            IR_OPS(f, i, ops, n);
            for (j = 0; j < n; j++)
                SINK_USE(ops[j], B);
            if (i->op == OP_CALL)
                SINK_USE(i->c, B);
        }
        if (ub->arg)
            SINK_USE(ub->arg, B);
    }
#undef SINK_USE
    for (B = 1; B < f->nblk; B++) {
        Block *bb = &f->blk[B];
        u32 T[2], ncall[2], q, nx;
        if (bb->term != TERM_BR || bb->succ[0] == bb->succ[1])
            continue;
        for (q = 0; q < 2; q++) {
            u32 t = bb->succ[q];
            T[q] = f->blk[t].npreds == 1 && t != B && f->blk[t].term != TERM_NONE && dominates(B, t) ? t : 0;
            ncall[q] = 0;
        }
        if (!T[0] && !T[1])
            continue;
        for (v = bb->last; v; v = f->ins[v].prev) {
            Ins *i = &f->ins[v];
            u32 t = 0;
            if (!sink_ok(i) || !total[v])
                continue;
            if (out[v]) {
                for (q = 0; q < 2; q++)
                    if (T[q] && dominates(T[q], lca[v]))
                        t = T[q];
                if (!t)
                    continue;
            }
            if (cnt[v]) {
                if (t && tt[v] != t)
                    continue;
                t = tt[v];
            }
            if (!t || t == 0xffffffffU || total[v] != out[v] + cnt[v])
                continue;
            dest[v] = t;
            if (i->op == OP_CALL)
                ncall[t == T[1]]++;
            IR_OPS(f, i, ops, n);
            for (j = 0; j <= n; j++) {
                u32 x;
                if (j == n && i->op != OP_CALL)
                    break;
                x = j < n ? ops[j] : i->c;
                if (f->ins[x].blk != B)
                    continue;
                if (!cnt[x])
                    tt[x] = t;
                else if (tt[x] != t)
                    tt[x] = 0xffffffffU;
                cnt[x]++;
            }
        }
        for (q = 0; q < 2; q++) {
            u32 pos;
            if (!T[q] || !ncall[q] || (q == 1 && T[1] == T[0]))
                continue;
            pos = f->blk[T[q]].first;
            for (v = bb->first; v; v = nx) {
                nx = f->ins[v].next;
                if (dest[v] != T[q])
                    continue;
                ir_remove(f, v);
                if (pos)
                    ir_insert_before(f, pos, v);
                else
                    ir_append(f, T[q], v);
                nmoved++;
            }
        }
    }
    free(total);
    if (nmoved && getenv("MCC_SINKLOG"))
        fprintf(stderr, "sink %s: %lu\n", f->name, (unsigned long)nmoved);
    return nmoved != 0;
}

/* ... and where the choice is a select: `c ? g (f (x)) : k`, f pure and its value read by nothing else, runs f
   whatever c is. The select becomes a branch on c round the side's values (those of its block that only the
   select reads, directly or through each other, a pure call among them), and a phi where the two ways meet; what
   came after the select goes on from there. One select at a time, the uses counted again after each. */
static int unselect_calls(void)
{
    u32 *total = 0, *cnt = 0, *tt, *dest, B, v, j, n, *ops, nuses, nx, nmade = 0;
    int again;
    if (getenv("MCC_NOSINK"))
        return 0;
    do {
        again = 0;
        for (B = 1; B < f->nblk && !again; B++) {
            Block *bb = &f->blk[B];
            u32 s, hascall = 0;
            if (bb->term == TERM_NONE)
                continue;
            for (v = bb->first; v && !hascall; v = f->ins[v].next)
                hascall = f->ins[v].op == OP_CALL && sink_ok(&f->ins[v]);
            if (!hascall)
                continue;
            for (s = bb->last; s && !again; s = f->ins[s].prev) {
                Ins *i = &f->ins[s];
                u32 x[2], ncall[2], q;
                if (i->op != OP_SELECT || i->ty > IT_F64 || i->b == i->c)
                    continue;
                if (!total) {
                    ir_resolve_all(f);
                    i = &f->ins[s];
                    total = count_uses(&nuses);
                    cnt = (u32 *)xmalloc(nuses * 3 * sizeof(u32));
                    tt = cnt + nuses;
                    dest = tt + nuses;
                    memset(cnt, 0, nuses * 3 * sizeof(u32));
                }
                x[0] = i->b;
                x[1] = i->c;
                ncall[0] = ncall[1] = 0;
                for (q = 0; q < 2; q++)
                    if (f->ins[x[q]].blk == B) {
                        cnt[x[q]]++;
                        tt[x[q]] = q + 1;
                    }
                for (v = i->prev; v; v = f->ins[v].prev) {
                    Ins *u = &f->ins[v];
                    u32 t = tt[v];
                    if (!cnt[v] || !sink_ok(u) || t == 0xffffffffU || total[v] != cnt[v])
                        continue;
                    dest[v] = t;
                    if (u->op == OP_CALL)
                        ncall[t - 1]++;
                    IR_OPS(f, u, ops, n);
                    for (j = 0; j <= n; j++) {
                        u32 y;
                        if (j == n && u->op != OP_CALL)
                            break;
                        y = j < n ? ops[j] : u->c;
                        if (f->ins[y].blk != B)
                            continue;
                        if (!cnt[y])
                            tt[y] = t;
                        else if (tt[y] != t)
                            tt[y] = 0xffffffffU;
                        cnt[y]++;
                    }
                }
                if (ncall[0] || ncall[1]) {
                    u32 J = ir_new_block(f), S[2], ph, cond = i->a, ty = i->ty, k;
                    S[0] = ncall[0] ? ir_new_block(f) : 0;
                    S[1] = ncall[1] ? ir_new_block(f) : 0;
                    bb = &f->blk[B];
                    f->blk[J].term = bb->term;
                    f->blk[J].arg = bb->arg;
                    f->blk[J].succ[0] = bb->succ[0];
                    f->blk[J].succ[1] = bb->succ[1];
                    f->blk[J].freq = bb->freq;
                    for (q = 0; q < (bb->term == TERM_BR ? 2u : bb->term == TERM_JMP ? 1u : 0u); q++) {
                        Block *sb = &f->blk[bb->succ[q]];
                        for (k = 0; k < sb->npreds; k++)
                            if (sb->preds[k] == B)
                                sb->preds[k] = J;
                    }
                    for (v = f->ins[s].next; v; v = nx) {
                        nx = f->ins[v].next;
                        ir_remove(f, v);
                        ir_append(f, J, v);
                    }
                    for (v = f->blk[B].first; v; v = nx) {
                        nx = f->ins[v].next;
                        if (v != s && dest[v] && dest[v] != 0xffffffffU && S[dest[v] - 1]) {
                            ir_remove(f, v);
                            ir_append(f, S[dest[v] - 1], v);
                        }
                    }
                    bb = &f->blk[B];
                    bb->term = TERM_BR;
                    bb->arg = cond;
                    for (q = 0; q < 2; q++) {
                        bb->succ[q] = S[q] ? S[q] : J;
                        if (S[q]) {
                            f->blk[S[q]].term = TERM_JMP;
                            f->blk[S[q]].succ[0] = J;
                            f->blk[S[q]].freq = bb->freq;
                            ir_add_pred(f, S[q], B);
                        }
                        ir_add_pred(f, J, S[q] ? S[q] : B);
                    }
                    ph = ir_phi(f, J, (int)ty);
                    f->xargs[f->ins[ph].a] = x[0];
                    f->xargs[f->ins[ph].a + 1] = x[1];
                    f->ins[ph].loc = f->ins[s].loc;
                    ir_replace(f, s, ph);
                    nmade++;
                    again = 1;
                } else {
                    for (v = f->ins[s].prev; v; v = f->ins[v].prev)
                        cnt[v] = tt[v] = dest[v] = 0;
                }
            }
        }
        if (total) {
            free(total);
            free(cnt);
            total = 0;
        }
    } while (again && nmade < 64);
    if (nmade) {
        ir_resolve_all(f);
        compute_dom();
        if (getenv("MCC_SINKLOG"))
            fprintf(stderr, "unselect %s: %lu\n", f->name, (unsigned long)nmade);
    }
    return nmade != 0;
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
