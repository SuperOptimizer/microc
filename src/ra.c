/* Linear-scan register allocation over machine IR. Target independent.
   Each virtual register has a list of live ranges, so a register can be
   shared with values that live entirely inside its lifetime holes.
   Positions: instruction i of block b occupies slot i+b (so there is a gap
   between blocks); slot s is read at 2s+2 and written at 2s+3. */
#include "mir.h"
#include <stdlib.h>
#include <string.h>

typedef struct Range {
    u32 start, end;
} Range;

typedef struct RangeList {
    Range *r;
    u32 n, cap;
} RangeList;

/* live ranges of virtual registers: linked lists in a pool, earliest first */
static u32 *rg_start, *rg_end, *rg_next;
static u32 nrg, caprg;
static u32 *head;   /* first range of each interval, 0 = none */
static u32 *cursor; /* first range not entirely before the current position */
static u32 *iv_end;
static u32 *dense_map; /* after ra_run: virtual register (less VREG0) as selected -> as allocated, or NOREG */
static u32 dense_n;

static void range_add(RangeList *l, u32 start, u32 end)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->r = (Range *)xrealloc(l->r, l->cap * sizeof(Range));
    }
    l->r[l->n].start = start;
    l->r[l->n].end = end;
    l->n++;
}

/* Does [s, e] overlap any range of the list (sorted, disjoint)? */
static int range_conflict(const RangeList *l, u32 s, u32 e)
{
    u32 lo = 0, hi = l->n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (l->r[mid].end < s)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < l->n && l->r[lo].start <= e;
}

/* Prepends [s, e] to v's ranges, merging with the first one if they touch. */
static void live_add(u32 v, u32 s, u32 e)
{
    u32 h = head[v], r;
    if (h && rg_start[h] <= e) {
        if (s < rg_start[h])
            rg_start[h] = s;
        if (e > rg_end[h])
            rg_end[h] = e;
        return;
    }
    if (nrg == caprg) {
        caprg = caprg ? caprg * 2 : 1024;
        rg_start = (u32 *)xrealloc(rg_start, caprg * sizeof(u32));
        rg_end = (u32 *)xrealloc(rg_end, caprg * sizeof(u32));
        rg_next = (u32 *)xrealloc(rg_next, caprg * sizeof(u32));
    }
    r = nrg++;
    rg_start[r] = s;
    rg_end[r] = e;
    rg_next[r] = h;
    head[v] = r;
    if (!h)
        iv_end[v] = e;
}

static int lowest_bit(u64 m)
{
    int n = 0;
    if (!(m & 0xffffffffUL)) {
        n += 32;
        m >>= 32;
    }
    if (!(m & 0xffff)) {
        n += 16;
        m >>= 16;
    }
    if (!(m & 0xff)) {
        n += 8;
        m >>= 8;
    }
    if (!(m & 0xf)) {
        n += 4;
        m >>= 4;
    }
    if (!(m & 3)) {
        n += 2;
        m >>= 2;
    }
    return n + !(m & 1);
}

/* Two range lists that do not intersect, as one. */
static u32 ranges_join(u32 a, u32 b)
{
    u32 h = 0, *link = &h;
    while (a && b) {
        if (rg_start[a] < rg_start[b]) {
            *link = a;
            link = &rg_next[a];
            a = *link;
        } else {
            *link = b;
            link = &rg_next[b];
            b = *link;
        }
    }
    *link = a ? a : b;
    return h;
}

/* The union of two range lists that do not intersect, as a new list (both stay as they are). */
static u32 ranges_union(u32 a, u32 b)
{
    u32 h = 0, last = 0, r, x; /* (the tail by index: growing the pool moves rg_next) */
    while (a || b) {
        if (a && (!b || rg_start[a] < rg_start[b])) {
            x = a;
            a = rg_next[a];
        } else {
            x = b;
            b = rg_next[b];
        }
        if (nrg == caprg) {
            caprg = caprg * 2;
            rg_start = (u32 *)xrealloc(rg_start, caprg * sizeof(u32));
            rg_end = (u32 *)xrealloc(rg_end, caprg * sizeof(u32));
            rg_next = (u32 *)xrealloc(rg_next, caprg * sizeof(u32));
        }
        r = nrg++;
        rg_start[r] = rg_start[x];
        rg_end[r] = rg_end[x];
        rg_next[r] = 0;
        if (last)
            rg_next[last] = r;
        else
            h = r;
        last = r;
    }
    return h;
}

/* Do the remaining ranges of a (from ra) and b (from rb) intersect? */
static int intersects(u32 ra, u32 rb)
{
    while (ra && rb) {
        if (rg_end[ra] <= rg_start[rb])
            ra = rg_next[ra];
        else if (rg_end[rb] <= rg_start[ra])
            rb = rg_next[rb];
        else
            return 1;
    }
    return 0;
}

/* Does the interval whose first range is r overlap a time when physical
   register pr is taken? Intervals are placed in order of their start, so
   each register keeps a cursor past the fixed ranges that are over, and the
   first (usually only) range of the interval needs no search. */
static RangeList fixed[64];
static u32 fpos[64];

static int fixed_conflict(u32 pr, u32 r)
{
    const RangeList *l = &fixed[pr];
    u32 c = fpos[pr], s = rg_start[r];
    while (c < l->n && l->r[c].end < s)
        c++;
    fpos[pr] = c;
    if (c == l->n)
        return 0;
    if (l->r[c].start <= rg_end[r])
        return 1;
    for (r = rg_next[r]; r; r = rg_next[r])
        if (range_conflict(l, rg_start[r], rg_end[r]))
            return 1;
    return 0;
}

/* ra_run again over the same function after it was changed (x64.c's loop splits): the first run's renumbering is kept
   composed with the second's, so ra_dense still answers for instruction selection's numbers. */
void ra_rerun(MFunc *m, const MTarget *t)
{
    static u32 *prev;
    static u32 capprev;
    u32 n = dense_n, k;
    if (capprev < n + 1) {
        capprev = n * 2 + 1;
        prev = (u32 *)xrealloc(prev, capprev * sizeof(u32));
    }
    if (n && dense_map != prev)
        memcpy(prev, dense_map, n * sizeof(u32));
    ra_run(m, t); /* dense_map: the first run's numbers -> the second's */
    for (k = 0; k < n; k++)
        prev[k] = prev[k] == NOREG || prev[k] >= dense_n ? NOREG : dense_map[prev[k]];
    /* the composition lives in prev from now on; ra_run's map buffer is reused on the next function */
    dense_map = prev;
    dense_n = n;
}

/* Debug information, after ra_run: the number a virtual register (VREG0 + r as instruction selection made it)
   was allocated under (an index of MFunc.loc), or NOREG if it is not used. */
u32 ra_dense(u32 r)
{
    return r < dense_n && head ? dense_map[r] : NOREG;
}

/* ... and the positions where it is live (see the numbering above), earliest first; returns how many ranges
   there are, of which the first max are stored. */
u32 ra_ranges(u32 d, u32 *start, u32 *end, u32 max)
{
    u32 r, n = 0;
    for (r = head ? head[d] : 0; r; r = rg_next[r], n++)
        if (n < max) {
            start[n] = rg_start[r];
            end[n] = rg_end[r];
        }
    return n;
}

void ra_run(MFunc *m, const MTarget *t)
{
    u32 nv = m->nvreg, words, b, i, k, v, maxfreq;
    u64 *gen, *kill, *livein, *liveout;
    u32 *hint, *order, *active, *inactive, nactive = 0, ninactive = 0, norder = 0;
    u32 *cc;    /* cc[s]: calls among the first s slots */
    u32 nslot = m->n + m->nblk;
    u32 *stamp; /* scratch mark per register */
    u32 *gidx, *glist, ng = 0; /* registers live across block boundaries, numbered densely */
    static Arena arena;
    u32 pend[64];
    u64 pendm; /* which entries of pend are set */
    u32 *bucket, *bnext, npos;
    u64 *weight; /* per interval: uses and definitions, weighted by loop depth */
    u8 *depth, *xcall; /* xcall: the interval is live across a call */
    u32 *same;         /* a register that shares the interval of another: which (itself if none) */
    u32 *avoid;        /* MF_NOTS1: the register whose place this one had better not take (NOREG if none) */
    u32 *copies, ncopy; /* register-to-register copies still to look at: instruction, depth in the top two bits */
    u32 *callins, *callblk, ncall = 0; /* the calls, in order: instruction and block */
    int changed, dp;

    m->nspill = 0;
    m->used = 0;
    m->cs_n = 0;
    m->cs_cost = 0;
    /* Instruction selection numbers registers sparsely (one per IR value,
       most never used). Renumber densely: everything below is sized by nv. */
    {
        static u32 *map;
        static u8 *cls;
        static u32 capmap;
        u32 n = 0;
        if (capmap < nv) {
            capmap = nv * 2;
            map = (u32 *)xrealloc(map, capmap * sizeof(u32));
            cls = (u8 *)xrealloc(cls, capmap);
        }
        memset(map, 0xff, nv * sizeof(u32));
#define DENSE(x) \
    if ((x) != NOREG && (x) >= VREG0) { \
        u32 o = (x) - VREG0; \
        if (map[o] == NOREG) { \
            cls[n] = m->vclass[o]; \
            map[o] = n++; \
        } \
        (x) = VREG0 + map[o]; \
    }
        for (i = 0; i < m->n; i++) {
            MIns *in = &m->ins[i];
            DENSE(in->d)
            DENSE(in->s0)
            DENSE(in->s1)
            DENSE(in->s2)
        }
#undef DENSE
        dense_map = map;
        dense_n = nv;
        memcpy(m->vclass, cls, n);
        m->nvreg = nv = n;
    }
    m->loc = (u32 *)xrealloc(m->loc, (nv + 1) * sizeof(u32));
    if (nv == 0) {
        head = 0;
        return;
    }

#define NEW(type, n) ((type *)arena_alloc_raw(&arena, (size_t)(n) * sizeof(type)))
    arena_reset(&arena);
    head = NEW(u32, nv);
    cursor = NEW(u32, nv);
    iv_end = NEW(u32, nv);
    hint = NEW(u32, nv);
    order = NEW(u32, nv);
    active = NEW(u32, nv);
    inactive = NEW(u32, nv);
    stamp = NEW(u32, nv);
    gidx = NEW(u32, nv);
    glist = NEW(u32, nv);
    cc = NEW(u32, nslot + 2);
    callins = NEW(u32, m->n + 1);
    callblk = NEW(u32, m->n + 1);
    for (k = 0; k < 64; k++)
        fixed[k].n = fpos[k] = 0;
    memset(head, 0, nv * sizeof(u32));

    /* Most registers live and die inside one block. Only those read in a
       block that has not yet written them take part in global liveness. */
    memset(stamp, 0xff, nv * sizeof(u32));
    memset(gidx, 0xff, nv * sizeof(u32));
    cc[0] = 0;
    for (b = 0; b < m->nblk; b++) {
        MBlock *mb = &m->blk[b];
        for (i = mb->first; i < mb->first + mb->n; i++) {
            MIns *in = &m->ins[i];
            u32 s[3];
            s[0] = in->s0;
            s[1] = in->s1;
            s[2] = in->s2;
            for (k = 0; k < 3; k++) {
                if (s[k] != NOREG && s[k] >= VREG0) {
                    v = s[k] - VREG0;
                    if (stamp[v] != b && gidx[v] == NOREG) {
                        gidx[v] = ng;
                        glist[ng++] = v;
                    }
                }
            }
            if (in->d != NOREG && in->d >= VREG0)
                stamp[in->d - VREG0] = b;
            cc[i + b + 1] = cc[i + b] + ((in->flags & MF_CALL) != 0);
            if (in->flags & MF_CALL) {
                callins[ncall] = i;
                callblk[ncall++] = b;
            }
        }
        cc[mb->first + mb->n + b + 1] = cc[mb->first + mb->n + b]; /* the gap */
    }
    words = (ng + 63) / 64 + 1;
    gen = NEW(u64, (size_t)m->nblk * words * 4);
    memset(gen, 0, (size_t)m->nblk * words * 4 * sizeof(u64));
    kill = gen + (size_t)m->nblk * words;
    livein = kill + (size_t)m->nblk * words;
    liveout = livein + (size_t)m->nblk * words;
    nrg = 1; /* range 0 is the list terminator */
    if (!caprg) {
        caprg = 1024;
        rg_start = (u32 *)xmalloc(caprg * sizeof(u32));
        rg_end = (u32 *)xmalloc(caprg * sizeof(u32));
        rg_next = (u32 *)xmalloc(caprg * sizeof(u32));
    }

#define BIT(set, blk, x) ((set)[(size_t)(blk) * words + ((x) >> 6)] & ((u64)1 << ((x)&63)))
#define SET(set, blk, x) ((set)[(size_t)(blk) * words + ((x) >> 6)] |= ((u64)1 << ((x)&63)))

    /* Local use/def sets. */
    for (b = 0; b < m->nblk; b++) {
        MBlock *mb = &m->blk[b];
        for (i = mb->first; i < mb->first + mb->n; i++) {
            MIns *in = &m->ins[i];
            u32 s[3];
            s[0] = in->s0;
            s[1] = in->s1;
            s[2] = in->s2;
            for (k = 0; k < 3; k++) {
                if (s[k] != NOREG && s[k] >= VREG0) {
                    u32 g = gidx[s[k] - VREG0];
                    if (g != NOREG && !BIT(kill, b, g))
                        SET(gen, b, g);
                }
            }
            if (in->d != NOREG && in->d >= VREG0 && gidx[in->d - VREG0] != NOREG)
                SET(kill, b, gidx[in->d - VREG0]);
        }
    }
    /* Liveness to a fixed point, visiting blocks backwards. */
    do {
        changed = 0;
        for (b = m->nblk; b-- > 0;) {
            MBlock *mb = &m->blk[b];
            u64 *out = liveout + (size_t)b * words, *in = livein + (size_t)b * words;
            u64 *g = gen + (size_t)b * words, *kl = kill + (size_t)b * words;
            for (k = 0; k < mb->nsucc; k++) {
                u64 *sin = livein + (size_t)mb->succ[k] * words;
                for (i = 0; i < words; i++)
                    out[i] |= sin[i];
            }
            for (i = 0; i < words; i++) {
                u64 nw = g[i] | (out[i] & ~kl[i]);
                if (nw != in[i]) {
                    in[i] = nw;
                    changed = 1;
                }
            }
        }
    } while (changed);

    /* Live ranges, call positions, hints, and the ranges where physical
       registers are occupied. Everything is walked backwards, so lists are
       built from the end. "live" marks registers live at the current point. */
    weight = NEW(u64, nv);
    depth = NEW(u8, m->nblk);
    memset(stamp, 0, nv * sizeof(u32)); /* from here: stamp[v] == b + 1 when v is live in block b */
    memset(depth, 0, m->nblk);
    /* a branch back to an earlier block closes a loop over the blocks between; one loop however many branches
       go back to its head (a continue is a second one), and up to six deep (3-D voxel code: z y x and a window c b a) */
    {
        u32 *latch = NEW(u32, m->nblk);
        memset(latch, 0xff, m->nblk * sizeof(u32));
        for (b = 0; b < m->nblk; b++) {
            MBlock *mb = &m->blk[b];
            for (k = 0; k < mb->nsucc; k++)
                if (mb->succ[k] <= b && (latch[mb->succ[k]] == NOREG || latch[mb->succ[k]] < b))
                    latch[mb->succ[k]] = b;
        }
        for (b = 0; b < m->nblk; b++)
            if (latch[b] != NOREG)
                for (i = b; i <= latch[b]; i++)
                    if (depth[i] < 6)
                        depth[i]++;
    }
    /* with a profile, what counts is how often each block really ran */
    maxfreq = 0;
    if (m->blk[0].freq > 1)
        for (b = 0; b < m->nblk; b++)
            if (m->blk[b].freq > maxfreq)
                maxfreq = m->blk[b].freq;
    avoid = NEW(u32, nv);
    for (v = 0; v < nv; v++) {
        hint[v] = NOREG;
        weight[v] = 0;
        avoid[v] = NOREG;
    }
    for (b = m->nblk; b-- > 0;) {
        MBlock *mb = &m->blk[b];
        u32 bstart = 2 * (mb->first + b) + 1, bend = 2 * (mb->first + mb->n + b) + 2;
        u64 *out = liveout + (size_t)b * words;
        u32 bw = maxfreq ? 1 + (u32)((u64)(m->blk[b].freq ? m->blk[b].freq - 1 : 1) * 4096 / maxfreq) : (u32)1 << (3 * depth[b]);
        for (i = 0; i < words; i++) {
            u64 w = out[i];
            while (w) {
                v = glist[i * 64 + (u32)lowest_bit(w)];
                w &= w - 1;
                stamp[v] = b + 1;
                live_add(v, bstart, bend);
            }
        }
        pendm = 0;
        for (i = mb->first + mb->n; i-- > mb->first;) {
            MIns *in2 = &m->ins[i];
            u32 rd = 2 * (i + b) + 2, wr = 2 * (i + b) + 3, s[3];
            u64 pm;
            if (in2->d != NOREG) {
                if (in2->d >= VREG0) {
                    v = in2->d - VREG0;
                    weight[v] += bw;
                    if ((in2->flags & MF_NOTS1) && in2->s1 != NOREG && in2->s1 >= VREG0 && in2->s1 != in2->s0) {
                        /* (s1 of the other register file, a gather's base: it is s2, the index, that is meant) */
                        if (!((m->vclass[v] ^ m->vclass[in2->s1 - VREG0]) & 1))
                            avoid[v] = in2->s1 - VREG0;
                        else if (in2->s2 != NOREG && in2->s2 >= VREG0 && in2->s2 != in2->s0)
                            avoid[v] = in2->s2 - VREG0;
                    }
                    if (stamp[v] == b + 1) {
                        rg_start[head[v]] = wr; /* the range begins at this definition */
                        stamp[v] = 0;
                    } else {
                        live_add(v, wr, wr); /* dead definition still needs a place */
                    }
                } else {
                    range_add(&fixed[in2->d], wr, (pendm >> in2->d & 1) ? pend[in2->d] : wr);
                    pendm &= ~((u64)1 << in2->d);
                }
            }
            if (in2->flags & MF_CALL) {
                /* a result register read after the call is occupied from the call on */
                pm = pendm & t->caller_saved;
                pendm &= ~pm;
                while (pm) {
                    k = (u32)lowest_bit(pm);
                    pm &= pm - 1;
                    range_add(&fixed[k], wr, pend[k]);
                }
            }
            if ((in2->flags & MF_TIED) && in2->d != NOREG && in2->d >= VREG0 && hint[in2->d - VREG0] == NOREG) {
                /* two-address: an operand that dies here leaves its register to the result (stamp: live below) */
                if (in2->s0 != NOREG && in2->s0 >= VREG0 && stamp[in2->s0 - VREG0] != b + 1)
                    hint[in2->d - VREG0] = in2->s0;
                else if ((in2->flags & MF_COMMUT) && in2->s1 != NOREG && in2->s1 >= VREG0 && stamp[in2->s1 - VREG0] != b + 1)
                    hint[in2->d - VREG0] = in2->s1;
            }
            s[0] = in2->s0;
            s[1] = in2->s1;
            s[2] = in2->s2;
            for (k = 0; k < 3; k++) {
                if (s[k] == NOREG)
                    continue;
                if (s[k] >= VREG0) {
                    v = s[k] - VREG0;
                    weight[v] += bw;
                    if (stamp[v] != b + 1) {
                        stamp[v] = b + 1;
                        live_add(v, bstart, rd);
                    }
                } else if (!(pendm >> s[k] & 1)) {
                    pendm |= (u64)1 << s[k];
                    pend[s[k]] = rd;
                }
            }
            pm = in2->pmask & ~pendm;
            pendm |= pm;
            while (pm) {
                k = (u32)lowest_bit(pm);
                pm &= pm - 1;
                pend[k] = rd;
            }
            if (in2->op == t->move_op && in2->d != NOREG && in2->s0 != NOREG) {
                u32 d = in2->d, s0 = in2->s0;
                /* a physical register is the better hint; otherwise the copy partner */
                if (d >= VREG0 && (hint[d - VREG0] == NOREG || (s0 < VREG0 && hint[d - VREG0] >= VREG0)))
                    hint[d - VREG0] = s0;
                if (s0 >= VREG0 && (hint[s0 - VREG0] == NOREG || (d < VREG0 && hint[s0 - VREG0] >= VREG0)))
                    hint[s0 - VREG0] = d;
            }
        }
        /* registers live into the block (incoming arguments) */
        while (pendm) {
            k = (u32)lowest_bit(pendm);
            pendm &= pendm - 1;
            range_add(&fixed[k], bstart, pend[k]);
        }
    }
    for (k = 0; k < 64; k++) {
        RangeList *l = &fixed[k];
        for (i = 0; i < l->n / 2; i++) {
            Range tmp = l->r[i];
            l->r[i] = l->r[l->n - 1 - i];
            l->r[l->n - 1 - i] = tmp;
        }
    }

    /* a call strictly inside a live range rules out caller-saved registers */
    xcall = NEW(u8, nv);
    for (v = 0; v < nv; v++) {
        u32 r;
        xcall[v] = 0;
        /* a call in slot j writes at position 2j + 3; count those
           strictly between a range's start and end */
        for (r = head[v]; r && !xcall[v] && cc[nslot]; r = rg_next[r]) {
            u32 lo = rg_start[r] + 1, hi = rg_end[r];
            lo = lo < 2 ? 0 : (lo - 2) / 2;
            hi = hi < 2 ? 0 : (hi - 2) / 2;
            if (lo > nslot)
                lo = nslot;
            if (hi > nslot)
                hi = nslot;
            xcall[v] = cc[hi] > cc[lo];
        }
        if (m->cs_mode && !(m->vclass[v] & 3))
            xcall[v] = 0; /* (a general register: saved round the calls where it is caller-saved, see below) */
    }

    /* Coalescing: the two sides of a copy whose lifetimes do not overlap become one interval, and the
       copy goes away. The copies in the innermost loops are looked at first. Phis leave such copies at
       the ends of blocks; without this, each trip round a loop moves its variables from where the
       body left them to where it expects them. */
    same = NEW(u32, nv);
    for (v = 0; v < nv; v++)
        same[v] = v;
    copies = NEW(u32, m->n + 1);
    ncopy = 0;
    for (dp = 3; dp >= 0 && (dp == 3 || ncopy); dp--) {
        u32 n = 0;
        for (b = 0; b < m->nblk && dp == 3; b++) {
            MBlock *mb = &m->blk[b];
            for (i = mb->first; i < mb->first + mb->n; i++) {
                MIns *in = &m->ins[i];
                if (in->op == t->move_op && in->d != NOREG && in->d >= VREG0 && in->s0 != NOREG && in->s0 >= VREG0 &&
                    !(in->flags & MF_SPLIT)) /* (a split is made to keep the two apart) */
                    copies[ncopy++] = i | (u32)(depth[b] < 3 ? depth[b] : 3) << 30;
            }
        }
        for (k = 0; k < ncopy; k++) {
            MIns *in = &m->ins[copies[k] & 0x3fffffff];
            u32 x, y;
            if ((int)(copies[k] >> 30) != dp) {
                copies[n++] = copies[k]; /* for a later round */
                continue;
            }
            x = in->d - VREG0;
            y = in->s0 - VREG0;
            while (same[x] != x)
                x = same[x] = same[same[x]];
            while (same[y] != y)
                y = same[y] = same[same[y]];
            if (x == y || ((m->vclass[x] ^ m->vclass[y]) & ~VC_LOW) || (m->vclass[x] & 2) || !head[x] || !head[y] ||
                xcall[x] != xcall[y] || intersects(head[x], head[y]))
                continue;
            m->vclass[x] |= m->vclass[y] & VC_LOW; /* (what either side cannot be in, the interval cannot) */
            head[x] = ranges_join(head[x], head[y]);
            head[y] = 0;
            same[y] = x;
            if (iv_end[y] > iv_end[x])
                iv_end[x] = iv_end[y];
            weight[x] += weight[y];
            if (hint[y] != NOREG && hint[y] < VREG0 && (hint[x] == NOREG || hint[x] >= VREG0))
                hint[x] = hint[y];
        }
        ncopy = n;
    }
    for (v = 0; v < nv; v++) {
        if (hint[v] != NOREG && hint[v] >= VREG0) {
            u32 h = hint[v] - VREG0;
            while (same[h] != h)
                h = same[h];
            hint[v] = h == v ? NOREG : VREG0 + h;
        }
    }

    /* order the intervals by start position (then by number): a bucket sort */
    npos = 2 * nslot + 4;
    bucket = NEW(u32, npos);
    bnext = NEW(u32, nv);
    memset(bucket, 0xff, npos * sizeof(u32));
    for (v = nv; v-- > 0;) {
        m->loc[v] = NOREG;
        cursor[v] = head[v];
        if (head[v]) {
            u32 p = rg_start[head[v]];
            bnext[v] = bucket[p];
            bucket[p] = v;
        }
    }
    for (i = 0; i < npos; i++)
        for (v = bucket[i]; v != NOREG; v = bnext[v])
            order[norder++] = v;

    /* class bit 1 without bit 4: a vector with no spill slot to go to (x64.c's legacy SSE code) */
    for (v = 0; v < nv; v++)
        if ((m->vclass[v] & 0x12) == 2)
            weight[v] = (u64)1 << 40;
    for (i = 0; i < norder; i++) {
        u32 cur = order[i], s = rg_start[head[cur]], n;
        int cls = m->vclass[cur] & 1, crosses = xcall[cur], reg = -1;
        u64 cand = t->allocatable[cls];

        /* advance the active and inactive sets to position s */
        n = 0;
        for (k = 0; k < nactive; k++) {
            u32 a = active[k];
            while (cursor[a] && rg_end[cursor[a]] <= s)
                cursor[a] = rg_next[cursor[a]];
            if (!cursor[a])
                continue;
            if (rg_start[cursor[a]] <= s)
                active[n++] = a;
            else
                inactive[ninactive++] = a;
        }
        nactive = n;
        n = 0;
        for (k = 0; k < ninactive; k++) {
            u32 a = inactive[k];
            while (cursor[a] && rg_end[cursor[a]] <= s)
                cursor[a] = rg_next[cursor[a]];
            if (!cursor[a])
                continue;
            if (rg_start[cursor[a]] <= s)
                active[nactive++] = a;
            else
                inactive[n++] = a;
        }
        ninactive = n;

        for (k = 0; k < nactive; k++)
            cand &= ~((u64)1 << m->loc[active[k]]);
        for (k = 0; k < ninactive && cand; k++) {
            u32 a = inactive[k];
            if ((cand >> m->loc[a] & 1) && intersects(cursor[a], head[cur]))
                cand &= ~((u64)1 << m->loc[a]);
        }
        if (crosses)
            cand &= ~t->caller_saved;
        if (m->vclass[cur] & VC_LOW)
            cand &= ~t->ext;
        /* try the hint first, then caller-saved, then callee-saved */
        if (hint[cur] != NOREG) {
            u32 h = hint[cur];
            if (h >= VREG0)
                h = m->loc[h - VREG0];
            if (h < 64 && (cand >> h & 1) && !fixed_conflict(h, head[cur]))
                reg = (int)h;
        }
        if (reg < 0 && t->ext_first && (cand & t->ext)) {
            /* one of the registers not everything can be in, while there is one */
            u64 c = cand & t->ext;
            while (c && reg < 0) {
                int pr = lowest_bit(c);
                c &= c - 1;
                if (!fixed_conflict((u32)pr, head[cur]))
                    reg = pr;
            }
        }
        if (reg < 0 && avoid[cur] != NOREG) {
            /* first without the register of the right operand of a two-address subtraction or division */
            u32 a = avoid[cur];
            while (same[a] != a)
                a = same[a];
            if (m->loc[a] < 64 && (cand >> m->loc[a] & 1) && (cand & ~((u64)1 << m->loc[a]))) {
                u64 c = cand & ~((u64)1 << m->loc[a]), c2 = c & t->caller_saved;
                int pass;
                for (pass = 0; pass < 2 && reg < 0; pass++) {
                    while (c2) {
                        int pr = lowest_bit(c2);
                        c2 &= c2 - 1;
                        if (!fixed_conflict((u32)pr, head[cur])) {
                            reg = pr;
                            break;
                        }
                    }
                    c2 = c & ~t->caller_saved;
                }
            }
        }
        if (reg < 0) {
            u64 c = cand & t->caller_saved;
            int pass;
            /* a copy partner still to be placed that lives across calls will
               want a callee-saved register: take one now so they can share */
            if (hint[cur] != NOREG && hint[cur] >= VREG0 && m->loc[hint[cur] - VREG0] == NOREG &&
                xcall[hint[cur] - VREG0])
                c = cand & ~t->caller_saved;
            for (pass = 0; pass < 3 && reg < 0; pass++) {
                while (c) {
                    int pr = lowest_bit(c);
                    c &= c - 1;
                    if (!fixed_conflict((u32)pr, head[cur])) {
                        reg = pr;
                        break;
                    }
                }
                c = pass == 0 ? cand & t->caller_saved : cand & ~t->caller_saved;
            }
        }
        if (reg < 0) {
            /* spill whichever of the competing intervals is used least for its length */
            u32 victim = NOREG, j;
            u64 bw = weight[cur], blen = iv_end[cur] - s + 1;
            for (k = 0; k < nactive; k++) {
                u32 a = active[k];
                u64 alen = iv_end[a] - rg_start[head[a]] + 1;
                int ok = 1;
                if ((m->vclass[a] & 1) != cls || (u64)weight[a] * blen >= bw * alen)
                    continue;
                if (crosses && (t->caller_saved >> m->loc[a] & 1))
                    continue;
                if ((m->vclass[cur] & VC_LOW) && (t->ext >> m->loc[a] & 1))
                    continue;
                if (fixed_conflict(m->loc[a], head[cur]))
                    continue;
                for (j = 0; j < ninactive && ok; j++)
                    if (m->loc[inactive[j]] == m->loc[a] && intersects(cursor[inactive[j]], head[cur]))
                        ok = 0;
                if (!ok)
                    continue;
                victim = k;
                bw = weight[a];
                blen = alen;
            }
            if (victim == NOREG) {
                m->loc[cur] = SPILL0 + m->nspill++;
                continue;
            }
            reg = (int)m->loc[active[victim]];
            m->loc[active[victim]] = SPILL0 + m->nspill++;
            active[victim] = active[--nactive];
        }
        m->loc[cur] = (u32)reg;
        m->used |= (u64)1 << reg;
        active[nactive++] = cur;
    }

    for (v = 0; v < nv; v++) {
        u32 r = v;
        while (same[r] != r)
            r = same[r];
        m->loc[v] = m->loc[r];
    }

    /* Spill slots shared: the two sides of a copy that both went to memory, and are never live at once, get one
       slot, so the copy goes (a value promoted in a loop, its phi and the value round the loop each in a slot of
       its own were a load and a store per copy per trip). The copies in the innermost loops first. */
    if (m->nspill > 1 && !getenv("MCC_NOSLOTCO")) {
        u32 ns = m->nspill, *sroot = NEW(u32, ns), *shead = NEW(u32, ns), *renum = NEW(u32, ns), nn = 0;
        u8 *scls = NEW(u8, ns);
        for (k = 0; k < ns; k++) {
            sroot[k] = k;
            shead[k] = 0;
            scls[k] = 0xff;
            renum[k] = NOREG;
        }
        for (v = 0; v < nv; v++)
            if (same[v] == v && head[v] && m->loc[v] != NOREG && m->loc[v] >= SPILL0) {
                u32 sl = m->loc[v] - SPILL0;
                shead[sl] = shead[sl] ? ranges_union(shead[sl], head[v]) : head[v];
                scls[sl] = m->vclass[v] & (u8)~VC_LOW;
            }
        for (dp = 6; dp >= 0; dp--)
            for (b = 0; b < m->nblk; b++) {
                MBlock *mb = &m->blk[b];
                if (depth[b] != dp)
                    continue;
                for (i = mb->first; i < mb->first + mb->n; i++) {
                    MIns *in = &m->ins[i];
                    u32 x, y;
                    if (in->op != t->move_op || in->d == NOREG || in->d < VREG0 || in->s0 == NOREG || in->s0 < VREG0)
                        continue;
                    x = m->loc[in->d - VREG0];
                    y = m->loc[in->s0 - VREG0];
                    if (x == NOREG || y == NOREG || x < SPILL0 || y < SPILL0)
                        continue;
                    x -= SPILL0;
                    y -= SPILL0;
                    while (sroot[x] != x)
                        x = sroot[x];
                    while (sroot[y] != y)
                        y = sroot[y];
                    if (x == y || scls[x] != scls[y] || scls[x] == 0xff || intersects(shead[x], shead[y]))
                        continue;
                    shead[x] = ranges_union(shead[x], shead[y]);
                    sroot[y] = x;
                }
            }
        for (v = 0; v < nv; v++)
            if (m->loc[v] != NOREG && m->loc[v] >= SPILL0) {
                u32 sl = m->loc[v] - SPILL0;
                while (sroot[sl] != sl)
                    sl = sroot[sl];
                if (renum[sl] == NOREG)
                    renum[sl] = nn++;
                m->loc[v] = SPILL0 + renum[sl];
            }
        m->nspill = nn;
    }

    /* the general registers that calls lie in the ranges of: what saving them round those calls costs, and with
       cs_mode the list of them for the target (see mir.h) */
    m->cs_n = 0;
    m->cs_cost = 0;
    for (v = 0; v < nv && ncall; v++) {
        u32 r, slot = NOREG;
        if (same[v] != v || !head[v] || (m->vclass[v] & 3) || m->loc[v] >= 64)
            continue;
        if (m->cs_mode && !(t->caller_saved >> m->loc[v] & 1))
            continue;
        for (r = head[v]; r; r = rg_next[r]) {
            u32 lo = 0, hi = ncall;
            /* the first call written after the range's start (a call in instruction i of block b writes at
               2 (i + b) + 3) */
            while (lo < hi) {
                u32 mid = (lo + hi) / 2;
                if (2 * (callins[mid] + callblk[mid]) + 3 <= rg_start[r])
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (; lo < ncall && 2 * (callins[lo] + callblk[lo]) + 3 < rg_end[r]; lo++) {
                m->cs_cost += 2 * (u64)(m->blk[callblk[lo]].freq ? m->blk[callblk[lo]].freq : 1);
                if (!m->cs_mode)
                    continue;
                if (slot == NOREG)
                    slot = m->nspill++;
                if (m->cs_n + 3 > m->cs_cap) {
                    m->cs_cap = m->cs_cap ? m->cs_cap * 2 : 48;
                    m->cs_list = (u32 *)xrealloc(m->cs_list, m->cs_cap * sizeof(u32));
                }
                m->cs_list[m->cs_n++] = callins[lo];
                m->cs_list[m->cs_n++] = m->loc[v];
                m->cs_list[m->cs_n++] = slot;
            }
        }
    }

#undef BIT
#undef SET
#undef NEW
}
