/* The CPU: an interpreter for A64 at EL0 and EL1 with a stage-1 MMU, exceptions, system registers
   and the generic timers. Time is counted in executed instructions, so runs are repeatable. */
#include "vm.h"

#define M32 0xffffffffUL
#define ADDRMASK 0x0000fffffffff000UL
#define POLL 400000

static void tlb_flush(Cpu *c) { memset(c->tlb, 0xff, sizeof c->tlb); }

static void tlb_flush_all(Vm *vm) /* TLBI reaches every CPU */
{
    int i;
    for (i = 0; i < vm->ncpu; i++)
        tlb_flush(&vm->cpus[i]);
}

static void set_mode(Cpu *c, int el, int spsel)
{
    c->sp_bank[c->spsel ? c->el : 0] = c->sp;
    c->el = el;
    c->spsel = spsel;
    c->sp = c->sp_bank[spsel ? el : 0];
}

/* Enter EL1 for an exception or an interrupt. */
static void take(Cpu *c, int irq, u64 esr, u64 far, u64 ret)
{
    u64 spsr = (u64)c->nzcv << 28 | (u64)c->daif << 6 | (u64)c->el << 2 | (u64)c->spsel;
    u64 off = c->el == 0 ? 0x400 : c->spsel ? 0x200 : 0;
    if (irq)
        off += 0x80;
    else {
        c->esr = esr;
        c->far = far;
    }
    set_mode(c, 1, 1);
    c->spsr = spsr;
    c->elr = ret;
    c->daif = 0xf;
    c->pc = c->vbar + off;
    c->excl_valid = 0;
}

static void fault(Cpu *c, u64 esr, u64 far, u64 ret)
{
    take(c, 0, esr | 0x02000000, far, ret);
    longjmp(c->jb, 1);
}

static void undefined(Cpu *c, u32 insn)
{
    if (c->debug)
        fprintf(stderr, "mvm: undefined instruction %08x at %lx (el%d)\n", insn, c->pc - 4, c->el);
    fault(c, 0, 0, c->pc - 4);
}

/* ---- memory ---- */

/* Translate; 0 and *pa, or the fault status code. */
static int walk(Cpu *c, u64 va, int acc, int el, u64 *pa)
{
    int top = (int)(va >> 55 & 1), tsz, bits, level, shift, ok;
    u64 base, desc, idx, mask;
    if (!(c->sctlr & 1)) {
        *pa = va & 0x0000ffffffffffffUL;
        return 0;
    }
    if (top) {
        tsz = (int)(c->tcr >> 16 & 63);
        if (c->tcr >> 38 & 1)
            va |= 0xff00000000000000UL;
    } else {
        tsz = (int)(c->tcr & 63);
        if (c->tcr >> 37 & 1)
            va &= 0x00ffffffffffffffUL;
    }
    bits = 64 - tsz;
    level = 4 - (bits - 12 + 8) / 9;
    if (bits < 25 || bits > 48 || (top ? ~va >> bits : va >> bits) != 0 || (c->tcr >> (top ? 23 : 7) & 1))
        return 0x04 | level;
    base = (top ? c->ttbr1 : c->ttbr0) & ADDRMASK;
    shift = 12 + 9 * (3 - level);
    idx = (va >> shift) & (((u64)1 << (bits - shift)) - 1);
    for (;;) {
        u64 a = base + idx * 8;
        if (a < RAM_BASE || a - RAM_BASE >= c->vm->ram_size)
            return 0x10;
        desc = *(u64 *)(c->vm->ram + (a - RAM_BASE));
        if (!(desc & 1) || (level == 3 && !(desc & 2)) || (level == 0 && !(desc & 2)))
            return 0x04 | level;
        if (level == 3 || !(desc & 2))
            break;
        base = desc & ADDRMASK;
        level++;
        shift -= 9;
        idx = (va >> shift) & 511;
    }
    if (!(desc & 0x400))
        return 0x08 | level;
    if (el == 0)
        ok = acc == 0 ? (int)(desc >> 6 & 1) : acc == 1 ? (desc >> 6 & 3) == 1 : !(desc >> 54 & 1);
    else
        ok = acc == 0 ? 1 : acc == 1 ? !(desc >> 7 & 1) : !(desc >> 53 & 1);
    if (!ok)
        return 0x0c | level;
    mask = ((u64)1 << shift) - 1;
    *pa = (desc & ADDRMASK & ~mask) | (va & mask);
    return 0;
}

/* The host address of va for this kind of access; 0 with c->mmio_pa set when it is a device. */
static u8 *resolve(Cpu *c, u64 va, int acc, int el)
{
    u64 pa;
    Tlb *e;
    int f = walk(c, va, acc, el, &pa);
    if (f) {
        if (acc == 2)
            fault(c, (u64)(c->el ? 0x21 : 0x20) << 26 | (u64)f, va, c->pc);
        fault(c, (u64)(c->el ? 0x25 : 0x24) << 26 | (u64)f | (acc == 1 ? 0x40 : 0), va, c->pc - 4);
    }
    if (pa < RAM_BASE || pa - RAM_BASE >= c->vm->ram_size) {
        if (acc == 2)
            fault(c, (u64)(c->el ? 0x21 : 0x20) << 26 | 0x10, va, c->pc);
        c->mmio_pa = pa;
        return 0;
    }
    e = &c->tlb[el][acc][(va >> 12) & (NTLB - 1)];
    e->tag = va >> 12;
    e->host = c->vm->ram + ((pa - RAM_BASE) & ~(u64)0xfff);
    return e->host + (va & 0xfff);
}

static u64 rd(Cpu *c, u64 va, int sz, int el);

static u64 rd_slow(Cpu *c, u64 va, int sz, int el)
{
    u8 *p;
    if ((va & 0xfff) + ((u64)1 << sz) > 0x1000) {
        u64 v = 0;
        int i;
        for (i = 0; i < 1 << sz; i++)
            v |= rd(c, va + (u64)i, 0, el) << (8 * i);
        return v;
    }
    p = resolve(c, va, 0, el);
    if (!p) {
        c->next_event = 0;
        return vm_mmio_read(c->vm, c->id, c->mmio_pa, 1 << sz);
    }
    switch (sz) {
    case 0: return *p;
    case 1: return *(u16 *)p;
    case 2: return *(u32 *)p;
    }
    return *(u64 *)p;
}

static u64 rd(Cpu *c, u64 va, int sz, int el)
{
    Tlb *e = &c->tlb[el][0][(va >> 12) & (NTLB - 1)];
    if (e->tag == va >> 12 && (va & 0xfff) + ((u64)1 << sz) <= 0x1000) {
        u8 *p = e->host + (va & 0xfff);
        switch (sz) {
        case 0: return *p;
        case 1: return *(u16 *)p;
        case 2: return *(u32 *)p;
        }
        return *(u64 *)p;
    }
    return rd_slow(c, va, sz, el);
}

static void wr(Cpu *c, u64 va, int sz, int el, u64 v);

static void wr_slow(Cpu *c, u64 va, int sz, int el, u64 v)
{
    u8 *p;
    if ((va & 0xfff) + ((u64)1 << sz) > 0x1000) {
        int i;
        resolve(c, va + ((u64)1 << sz) - 1, 1, el); /* fault before any byte is written */
        for (i = 0; i < 1 << sz; i++)
            wr(c, va + (u64)i, 0, el, v >> (8 * i) & 0xff);
        return;
    }
    p = resolve(c, va, 1, el);
    if (!p) {
        c->next_event = 0;
        vm_mmio_write(c->vm, c->id, c->mmio_pa, 1 << sz, v);
        return;
    }
    switch (sz) {
    case 0: *p = (u8)v; break;
    case 1: *(u16 *)p = (u16)v; break;
    case 2: *(u32 *)p = (u32)v; break;
    default: *(u64 *)p = v;
    }
}

static void wr(Cpu *c, u64 va, int sz, int el, u64 v)
{
    Tlb *e = &c->tlb[el][1][(va >> 12) & (NTLB - 1)];
    if (e->tag == va >> 12 && (va & 0xfff) + ((u64)1 << sz) <= 0x1000) {
        u8 *p = e->host + (va & 0xfff);
        switch (sz) {
        case 0: *p = (u8)v; break;
        case 1: *(u16 *)p = (u16)v; break;
        case 2: *(u32 *)p = (u32)v; break;
        default: *(u64 *)p = v;
        }
        return;
    }
    wr_slow(c, va, sz, el, v);
}

/* ---- timers and interrupts ---- */

static int timer_level(u64 ctl, u64 cval, u64 now) { return (ctl & 3) == 1 && now >= cval; }

static void timers(Cpu *c)
{
    u64 now = c->icount + c->toff;
    gic_set_level(c->vm, c->id, IRQ_VTIMER, timer_level(c->cntv_ctl, c->cntv_cval, now));
    gic_set_level(c->vm, c->id, IRQ_PTIMER, timer_level(c->cntp_ctl, c->cntp_cval, now));
}

/* Ticks until the next timer interrupt can rise; 0 when no timer is armed. */
static u64 timer_wait(Cpu *c)
{
    u64 now = c->icount + c->toff, d = 0;
    if ((c->cntv_ctl & 3) == 1 && c->cntv_cval > now)
        d = c->cntv_cval - now;
    if ((c->cntp_ctl & 3) == 1 && c->cntp_cval > now && (!d || c->cntp_cval - now < d))
        d = c->cntp_cval - now;
    return d;
}

static void events(Cpu *c)
{
    u64 d, next = c->slice_end;
    timers(c);
    if (c->vm->irq_out[c->id] && !(c->daif & 2))
        take(c, 1, 0, 0, c->pc);
    d = timer_wait(c);
    if (d && c->icount + d < next)
        next = c->icount + d;
    c->next_event = next;
}

/* The guest waits for an interrupt: this CPU sits out until one is pending. */
static void wfi(Cpu *c)
{
    timers(c);
    if (!c->vm->irq_out[c->id])
        c->halted = 1;
    c->next_event = 0;
}

/* ---- system registers ---- */

#define SR(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

static u64 sysreg_read(Cpu *c, u32 r)
{
    u64 now = c->icount + c->toff;
    switch (r) {
    case SR(3, 0, 0, 0, 0): return 0x411fd070;
    case SR(3, 0, 0, 0, 5): return 0x80000000UL | (u64)c->id;
    case SR(3, 0, 0, 4, 0): return 0x1100000000000011UL; /* EL0/EL1 AArch64, FP, AdvSIMD, CSV2, CSV3 */
    case SR(3, 0, 0, 5, 0): return 0x10305106;
    case SR(3, 0, 0, 7, 0): return 0x0f000022; /* 4K pages only, 40-bit PA, 16-bit ASIDs */
    case SR(3, 0, 1, 0, 0): return c->sctlr;
    case SR(3, 0, 1, 0, 1): return c->actlr;
    case SR(3, 0, 1, 0, 2): return c->cpacr;
    case SR(3, 0, 2, 0, 0): return c->ttbr0;
    case SR(3, 0, 2, 0, 1): return c->ttbr1;
    case SR(3, 0, 2, 0, 2): return c->tcr;
    case SR(3, 0, 4, 0, 0): return c->spsr;
    case SR(3, 0, 4, 0, 1): return c->elr;
    case SR(3, 0, 4, 1, 0): return c->spsel ? c->sp_bank[0] : c->sp;
    case SR(3, 0, 4, 2, 0): return (u64)c->spsel;
    case SR(3, 0, 4, 2, 2): return (u64)c->el << 2;
    case SR(3, 3, 4, 2, 0): return (u64)c->nzcv << 28;
    case SR(3, 3, 4, 2, 1): return (u64)c->daif << 6;
    case SR(3, 3, 4, 4, 0): return c->fpcr;
    case SR(3, 3, 4, 4, 1): return c->fpsr;
    case SR(3, 0, 5, 1, 0): return c->afsr0;
    case SR(3, 0, 5, 1, 1): return c->afsr1;
    case SR(3, 0, 5, 2, 0): return c->esr;
    case SR(3, 0, 6, 0, 0): return c->far;
    case SR(3, 0, 7, 4, 0): return c->par;
    case SR(3, 0, 10, 2, 0): return c->mair;
    case SR(3, 0, 10, 3, 0): return c->amair;
    case SR(3, 0, 12, 0, 0): return c->vbar;
    case SR(3, 0, 13, 0, 1): return c->contextidr;
    case SR(3, 0, 13, 0, 4): return c->tpidr_el1;
    case SR(3, 3, 13, 0, 2): return c->tpidr_el0;
    case SR(3, 3, 13, 0, 3): return c->tpidrro;
    case SR(3, 0, 14, 1, 0): return c->cntkctl;
    case SR(3, 3, 14, 0, 0): return TIMER_HZ;
    case SR(3, 3, 14, 0, 1):
    case SR(3, 3, 14, 0, 2): return now;
    case SR(3, 3, 14, 2, 0): return (c->cntp_cval - now) & M32;
    case SR(3, 3, 14, 2, 1): return (c->cntp_ctl & 3) | (now >= c->cntp_cval ? 4 : 0);
    case SR(3, 3, 14, 2, 2): return c->cntp_cval;
    case SR(3, 3, 14, 3, 0): return (c->cntv_cval - now) & M32;
    case SR(3, 3, 14, 3, 1): return (c->cntv_ctl & 3) | (now >= c->cntv_cval ? 4 : 0);
    case SR(3, 3, 14, 3, 2): return c->cntv_cval;
    case SR(3, 3, 0, 0, 1): return 0x8444c004UL;
    case SR(3, 3, 0, 0, 7): return 4; /* DC ZVA clears 64 bytes */
    case SR(3, 2, 0, 0, 0): return c->csselr;
    case SR(2, 0, 0, 2, 2): return c->mdscr;
    }
    if (c->debug > 1)
        fprintf(stderr, "mvm: read of system register %x at %lx\n", r, c->pc - 4);
    return 0;
}

static void sysreg_write(Cpu *c, u32 r, u64 v)
{
    u64 now = c->icount + c->toff;
    switch (r) {
    case SR(3, 0, 1, 0, 0): c->sctlr = v; tlb_flush(c); break;
    case SR(3, 0, 1, 0, 1): c->actlr = v; break;
    case SR(3, 0, 1, 0, 2): c->cpacr = v; break;
    case SR(3, 0, 2, 0, 0): c->ttbr0 = v; tlb_flush(c); break;
    case SR(3, 0, 2, 0, 1): c->ttbr1 = v; tlb_flush(c); break;
    case SR(3, 0, 2, 0, 2): c->tcr = v; tlb_flush(c); break;
    case SR(3, 0, 4, 0, 0): c->spsr = v; break;
    case SR(3, 0, 4, 0, 1): c->elr = v; break;
    case SR(3, 0, 4, 1, 0):
        if (c->spsel)
            c->sp_bank[0] = v;
        else
            c->sp = v;
        break;
    case SR(3, 0, 4, 2, 0): set_mode(c, c->el, (int)(v & 1)); break;
    case SR(3, 3, 4, 2, 0): c->nzcv = (u32)(v >> 28) & 15; break;
    case SR(3, 3, 4, 2, 1): c->daif = (u32)(v >> 6) & 15; c->next_event = 0; break;
    case SR(3, 3, 4, 4, 0): c->fpcr = (u32)v; break;
    case SR(3, 3, 4, 4, 1): c->fpsr = (u32)v; break;
    case SR(3, 0, 5, 1, 0): c->afsr0 = v; break;
    case SR(3, 0, 5, 1, 1): c->afsr1 = v; break;
    case SR(3, 0, 5, 2, 0): c->esr = v; break;
    case SR(3, 0, 6, 0, 0): c->far = v; break;
    case SR(3, 0, 7, 4, 0): c->par = v; break;
    case SR(3, 0, 10, 2, 0): c->mair = v; break;
    case SR(3, 0, 10, 3, 0): c->amair = v; break;
    case SR(3, 0, 12, 0, 0): c->vbar = v; break;
    case SR(3, 0, 13, 0, 1): c->contextidr = v; break;
    case SR(3, 0, 13, 0, 4): c->tpidr_el1 = v; break;
    case SR(3, 3, 13, 0, 2): c->tpidr_el0 = v; break;
    case SR(3, 3, 13, 0, 3): c->tpidrro = v; break;
    case SR(3, 0, 14, 1, 0): c->cntkctl = v; break;
    case SR(3, 3, 14, 2, 0): c->cntp_cval = now + (u64)(s64)(s32)v; c->next_event = 0; break;
    case SR(3, 3, 14, 2, 1): c->cntp_ctl = v & 3; c->next_event = 0; break;
    case SR(3, 3, 14, 2, 2): c->cntp_cval = v; c->next_event = 0; break;
    case SR(3, 3, 14, 3, 0): c->cntv_cval = now + (u64)(s64)(s32)v; c->next_event = 0; break;
    case SR(3, 3, 14, 3, 1): c->cntv_ctl = v & 3; c->next_event = 0; break;
    case SR(3, 3, 14, 3, 2): c->cntv_cval = v; c->next_event = 0; break;
    case SR(3, 2, 0, 0, 0): c->csselr = v; break;
    case SR(2, 0, 0, 2, 2): c->mdscr = v; break;
    default:
        if (c->debug > 1)
            fprintf(stderr, "mvm: write of system register %x at %lx\n", r, c->pc - 4);
    }
}

static void sysinsn(Cpu *c, u32 insn)
{
    u32 l = insn >> 21 & 1, op0 = insn >> 19 & 3, op1 = insn >> 16 & 7, crn = insn >> 12 & 15, crm = insn >> 8 & 15;
    u32 op2 = insn >> 5 & 7, rt = insn & 31;
    if (c->el == 0 && op1 != 3 && !(op0 == 0 && crn != 4)) {
        undefined(c, insn);
        return;
    }
    if (op0 == 0) {
        if (crn == 4) { /* msr pstate field, #imm */
            if (op1 == 0 && op2 == 5)
                set_mode(c, c->el, (int)(crm & 1));
            else if (op1 == 3 && op2 == 6)
                c->daif |= crm;
            else if (op1 == 3 && op2 == 7) {
                c->daif &= ~crm;
                c->next_event = 0;
            }
        } else if (crn == 2) {
            if (crm == 0 && op2 == 3 && c->el)
                wfi(c);
            else if (crm == 0 && op2 == 1 && c->vm->ncpu > 1) { /* yield: let the other CPUs run */
                c->slice_end = c->icount;
                c->next_event = 0;
            }
        } else if (crn == 3 && op2 == 2)
            c->excl_valid = 0;
    } else if (op0 == 1) {
        u64 v = c->x[rt];
        if (crn == 7 && op1 == 3 && crm == 4 && op2 == 1) { /* dc zva */
            int i;
            v &= ~(u64)63;
            for (i = 0; i < 8; i++)
                wr(c, v + 8 * (u64)i, 3, c->el, 0);
        } else if (crn == 7 && op1 == 0 && crm == 8 && op2 < 4) { /* at s1e[01][rw] */
            u64 pa;
            int f = walk(c, v, (int)(op2 & 1), op2 < 2, &pa);
            c->par = f ? (u64)(f << 1 | 1) : (pa & ADDRMASK);
        } else if (crn == 8)
            tlb_flush_all(c->vm);
    } else if (l)
        c->x[rt] = sysreg_read(c, SR(op0, op1, crn, crm, op2));
    else
        sysreg_write(c, SR(op0, op1, crn, crm, op2), c->x[rt]);
}

/* ---- arithmetic helpers ---- */

static u64 adds(Cpu *c, int sf, u64 a, u64 b, u64 cin)
{
    u64 r;
    u32 f = 0;
    if (sf) {
        r = a + b + cin;
        if (r >> 63) f |= 8;
        if (!r) f |= 4;
        if (cin ? r <= a : r < a) f |= 2;
        if ((~(a ^ b) & (a ^ r)) >> 63) f |= 1;
    } else {
        a &= M32;
        b &= M32;
        r = a + b + cin;
        if (r >> 31 & 1) f |= 8;
        if (!(r & M32)) f |= 4;
        if (r >> 32) f |= 2;
        if ((~(a ^ b) & (a ^ r)) >> 31 & 1) f |= 1;
        r &= M32;
    }
    c->nzcv = f;
    return r;
}

static void logic_flags(Cpu *c, int sf, u64 r)
{
    c->nzcv = (r >> (sf ? 63 : 31) & 1 ? 8 : 0) | ((sf ? r : r & M32) ? 0 : 4);
}

static int cond(Cpu *c, u32 cc)
{
    u32 f = c->nzcv;
    int n = f >> 3 & 1, z = f >> 2 & 1, cy = f >> 1 & 1, v = f & 1, r;
    switch (cc >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return 1;
    }
    return (cc & 1) ? !r : r;
}

static u64 shifted(u64 v, u32 type, u32 n, int sf)
{
    if (!sf)
        v &= M32;
    if (!n)
        return v;
    switch (type) {
    case 0: v <<= n; break;
    case 1: v >>= n; break;
    case 2: v = sf ? (u64)((s64)v >> n) : (u64)(s64)((s32)v >> n); break;
    default: v = sf ? v >> n | v << (64 - n) : v >> n | v << (32 - n);
    }
    return sf ? v : v & M32;
}

static u64 extended(u64 v, u32 option, u32 shift)
{
    switch (option) {
    case 0: v &= 0xff; break;
    case 1: v &= 0xffff; break;
    case 2: v &= M32; break;
    case 4: v = (u64)(s64)(signed char)v; break;
    case 5: v = (u64)(s64)(short)v; break;
    case 6: v = (u64)(s64)(s32)v; break;
    }
    return v << shift;
}

static u64 sext(u64 v, int bits) { return (u64)((s64)(v << (64 - bits)) >> (64 - bits)); }
static u64 ror(u64 v, u32 n, u32 size) { return n ? (v >> n | v << (size - n)) & (size == 64 ? ~(u64)0 : M32) : v; }

static int bitmask(u32 n, u32 immr, u32 imms, int sf, u64 *wmask, u64 *tmask)
{
    u32 len, size, levels, s, r, d, i;
    u64 w, t, e;
    u32 x = n << 6 | (~imms & 63);
    if (!x || (n && !sf))
        return 0;
    for (len = 6; !(x >> len & 1); len--)
        ;
    size = (u32)1 << len;
    levels = size - 1;
    s = imms & levels;
    r = immr & levels;
    d = (s - r) & levels;
    e = size == 64 ? ~(u64)0 : ((u64)1 << size) - 1;
    w = s == 63 ? ~(u64)0 : ((u64)1 << (s + 1)) - 1;
    if (r)
        w = (w >> r | w << (size - r)) & e;
    t = d == 63 ? ~(u64)0 : ((u64)1 << (d + 1)) - 1;
    for (i = size; i < 64; i *= 2) {
        w |= w << i;
        t |= t << i;
    }
    if (!sf) {
        w &= M32;
        t &= M32;
    }
    *wmask = w;
    *tmask = t;
    return 1;
}

static u64 umulh(u64 a, u64 b)
{
    u64 al = a & M32, ah = a >> 32, bl = b & M32, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (lh & M32) + (hl & M32);
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

static u32 clz(u64 v, u32 size)
{
    u32 n = 0;
    if (size == 32)
        v <<= 32;
    if (!v)
        return size;
    while (!(v >> 63)) {
        v <<= 1;
        n++;
    }
    return n;
}

static u64 rbit(u64 v, u32 size)
{
    u64 r = 0;
    u32 i;
    for (i = 0; i < size; i++)
        if (v >> i & 1)
            r |= (u64)1 << (size - 1 - i);
    return r;
}

static u64 rev(u64 v, u32 bytes) /* reverse the bytes within each group of `bytes` */
{
    u64 r = 0;
    u32 i;
    for (i = 0; i < 8; i++)
        r |= (v >> (8 * i) & 0xff) << (8 * ((i & ~(bytes - 1)) + (bytes - 1 - (i & (bytes - 1)))));
    return r;
}

/* ---- instruction groups ---- */

#define RD (insn & 31)
#define RN (insn >> 5 & 31)
#define RM (insn >> 16 & 31)
#define RA (insn >> 10 & 31)
#define XSP(r) ((r) == 31 ? c->sp : c->x[r])

static void dp_imm(Cpu *c, u32 insn, u64 pc)
{
    int sf = (int)(insn >> 31);
    u32 rd = RD, rn = RN, opc = insn >> 29 & 3;
    u64 a, r, imm, wmask, tmask;
    switch (insn >> 23 & 7) {
    case 0:
    case 1:
        imm = sext((u64)(insn >> 5 & 0x7ffff) << 2 | (insn >> 29 & 3), 21);
        c->x[rd] = sf ? (pc & ~(u64)0xfff) + (imm << 12) : pc + imm;
        return;
    case 2:
        imm = insn >> 10 & 0xfff;
        if (insn >> 22 & 1)
            imm <<= 12;
        a = XSP(rn);
        if (opc & 1)
            r = adds(c, sf, a, opc & 2 ? ~imm : imm, opc >> 1), c->x[rd] = r;
        else {
            r = opc & 2 ? a - imm : a + imm;
            if (!sf)
                r &= M32;
            if (rd == 31)
                c->sp = r;
            else
                c->x[rd] = r;
        }
        return;
    case 4:
        if (!bitmask(insn >> 22 & 1, insn >> 16 & 63, insn >> 10 & 63, sf, &wmask, &tmask))
            break;
        a = c->x[rn];
        r = opc == 1 ? a | wmask : opc == 2 ? a ^ wmask : a & wmask;
        if (!sf)
            r &= M32;
        if (opc == 3) {
            logic_flags(c, sf, r);
            c->x[rd] = r;
        } else if (rd == 31)
            c->sp = r;
        else
            c->x[rd] = r;
        return;
    case 5: {
        u32 sh = (insn >> 21 & 3) * 16;
        imm = (u64)(insn >> 5 & 0xffff) << sh;
        if (opc == 0)
            r = ~imm;
        else if (opc == 2)
            r = imm;
        else if (opc == 3)
            r = (c->x[rd] & ~((u64)0xffff << sh)) | imm;
        else
            break;
        c->x[rd] = sf ? r : r & M32;
        return;
    }
    case 6: {
        u32 immr = insn >> 16 & 63, imms = insn >> 10 & 63, size = sf ? 64 : 32;
        u64 src = c->x[rn], bot, dst;
        if (opc == 3 || !bitmask(insn >> 22 & 1, immr, imms, sf, &wmask, &tmask))
            break;
        if (!sf)
            src &= M32;
        dst = opc == 1 ? c->x[rd] : 0;
        bot = (dst & ~wmask) | (ror(src, immr, size) & wmask);
        if (opc == 0) {
            u64 top = src >> imms & 1 ? ~(u64)0 : 0;
            r = (top & ~tmask) | (bot & tmask);
        } else
            r = (dst & ~tmask) | (bot & tmask);
        c->x[rd] = sf ? r : r & M32;
        return;
    }
    case 7: {
        u32 lsb = insn >> 10 & 63;
        u64 hi = c->x[rn], lo = c->x[RM];
        if (sf)
            r = lsb ? lo >> lsb | hi << (64 - lsb) : lo;
        else
            r = lsb ? ((lo & M32) >> lsb | hi << (32 - lsb)) & M32 : lo & M32;
        c->x[rd] = r;
        return;
    }
    }
    undefined(c, insn);
}

static void branch(Cpu *c, u32 insn, u64 pc)
{
    u32 rt = RD;
    if ((insn & 0x7c000000) == 0x14000000) {
        if (insn >> 31)
            c->x[30] = pc + 4;
        c->pc = pc + (sext(insn & 0x3ffffff, 26) << 2);
    } else if ((insn & 0x7e000000) == 0x34000000) {
        u64 v = insn >> 31 ? c->x[rt] : c->x[rt] & M32;
        if ((v == 0) != (int)(insn >> 24 & 1))
            c->pc = pc + (sext(insn >> 5 & 0x7ffff, 19) << 2);
    } else if ((insn & 0x7e000000) == 0x36000000) {
        u32 bit = (insn >> 31) << 5 | (insn >> 19 & 31);
        if ((c->x[rt] >> bit & 1) == (insn >> 24 & 1))
            c->pc = pc + (sext(insn >> 5 & 0x3fff, 14) << 2);
    } else if ((insn & 0xff000010) == 0x54000000) {
        if (cond(c, insn & 15))
            c->pc = pc + (sext(insn >> 5 & 0x7ffff, 19) << 2);
    } else if ((insn & 0xff000000) == 0xd4000000) {
        u32 imm = insn >> 5 & 0xffff;
        switch (insn & 0x00e0001f) {
        case 0x000001: fault(c, (u64)0x15 << 26 | imm, 0, pc + 4); break;
        case 0x000002:
            if (c->el == 0)
                undefined(c, insn);
            c->x[0] = psci_call(c->vm, c->id, c->x[0], c->x[1], c->x[2], c->x[3]);
            c->next_event = 0;
            break;
        case 0x200000: fault(c, (u64)0x3c << 26 | imm, 0, pc); break;
        default: undefined(c, insn);
        }
    } else if ((insn & 0xffc00000) == 0xd5000000)
        sysinsn(c, insn);
    else if ((insn & 0xfe000000) == 0xd6000000) {
        u64 target = c->x[RN];
        switch (insn >> 21 & 15) {
        case 1: c->x[30] = pc + 4; /* fall through */
        case 0:
        case 2: c->pc = target; break;
        case 4: {
            u64 s = c->spsr;
            int el = (int)(s >> 2 & 3);
            if (c->el == 0 || el > 1) {
                undefined(c, insn);
                break;
            }
            set_mode(c, el, (int)(s & 1));
            c->nzcv = (u32)(s >> 28) & 15;
            c->daif = (u32)(s >> 6) & 15;
            c->pc = c->elr;
            c->excl_valid = 0;
            c->next_event = 0;
            break;
        }
        default: undefined(c, insn);
        }
    } else
        undefined(c, insn);
}

/* Load or store one register of either file; size is log2 bytes (4 = a whole vector). */
static void load(Cpu *c, int vec, u32 rt, u64 a, int size, int sign, int to64, int el)
{
    if (vec) {
        u64 lo = rd(c, a, size > 3 ? 3 : size, el), hi = size > 3 ? rd(c, a + 8, 3, el) : 0;
        c->v[rt].d[0] = lo;
        c->v[rt].d[1] = hi;
    } else {
        u64 v = rd(c, a, size, el);
        if (sign) {
            v = sext(v, 8 << size);
            if (!to64)
                v &= M32;
        }
        c->x[rt] = v;
    }
}

static void store(Cpu *c, int vec, u32 rt, u64 a, int size, int el)
{
    if (vec) {
        if (size > 3) {
            wr(c, a, 3, el, c->v[rt].d[0]);
            wr(c, a + 8, 3, el, c->v[rt].d[1]);
        } else
            wr(c, a, size, el, c->v[rt].d[0]);
    } else
        wr(c, a, size, el, c->x[rt]);
}

static int fp_trap(Cpu *c)
{
    u32 en = (u32)(c->cpacr >> 20) & 3;
    if (en == 3 || (en == 1 && c->el))
        return 0;
    fault(c, (u64)0x07 << 26, 0, c->pc - 4);
    return 1;
}

static void loadstore(Cpu *c, u32 insn, u64 pc)
{
    u32 rt = RD, rn = RN, rt2 = RA;
    int vec = (int)(insn >> 26 & 1), size = (int)(insn >> 30), el = c->el;
    u64 a, base;
    if (vec && fp_trap(c))
        return;
    switch (insn >> 28 & 3) {
    case 0: {
        u32 o2 = insn >> 23 & 1, l = insn >> 22 & 1, o1 = insn >> 21 & 1, rs = RM;
        if (vec || (insn >> 24 & 1) || (o2 && o1))
            break;
        a = XSP(rn);
        if (o2) { /* ldar, stlr and friends */
            if (l)
                c->x[rt] = rd(c, a, size, el);
            else
                wr(c, a, size, el, c->x[rt]);
        } else if (l) {
            u64 v = rd(c, a, size, el), v2 = 0;
            if (o1)
                v2 = rd(c, a + ((u64)1 << size), size, el);
            c->x[rt] = v;
            if (o1)
                c->x[rt2] = v2;
            c->excl_valid = 1;
            c->excl_addr = a;
        } else {
            u64 v = c->x[rt], v2 = c->x[rt2];
            if (c->excl_valid && c->excl_addr == a) {
                wr(c, a, size, el, v);
                if (o1)
                    wr(c, a + ((u64)1 << size), size, el, v2);
                c->x[rs] = 0;
            } else
                c->x[rs] = 1;
            c->excl_valid = 0;
        }
        return;
    }
    case 1: {
        if (insn >> 24 & 1)
            break;
        a = pc + (sext(insn >> 5 & 0x7ffff, 19) << 2);
        if (vec)
            load(c, 1, rt, a, size + 2, 0, 0, el);
        else if (size == 3)
            ; /* prfm */
        else
            load(c, 0, rt, a, size == 1 ? 3 : 2, size == 2, 1, el);
        return;
    }
    case 2: {
        u32 mode = insn >> 23 & 3, l = insn >> 22 & 1;
        int sz = vec ? size + 2 : 2 + (size >> 1), sign = !vec && size == 1;
        u64 off = sext(insn >> 15 & 0x7f, 7) << sz;
        if ((vec && size == 3) || (!vec && (size == 3 || (size == 1 && !l))))
            break;
        base = XSP(rn);
        a = mode == 1 ? base : base + off;
        if (l) {
            if (vec) {
                u64 lo = rd(c, a, sz > 3 ? 3 : sz, el), hi = sz > 3 ? rd(c, a + 8, 3, el) : 0;
                load(c, 1, rt2, a + ((u64)1 << sz), sz, 0, 0, el);
                c->v[rt].d[0] = lo;
                c->v[rt].d[1] = hi;
            } else {
                u64 v = rd(c, a, sz, el), v2 = rd(c, a + ((u64)1 << sz), sz, el);
                if (sign) {
                    v = sext(v, 32);
                    v2 = sext(v2, 32);
                }
                c->x[rt] = v;
                c->x[rt2] = v2;
            }
        } else {
            store(c, vec, rt, a, sz, el);
            store(c, vec, rt2, a + ((u64)1 << sz), sz, el);
        }
        if (mode & 1) {
            if (rn == 31)
                c->sp = base + off;
            else
                c->x[rn] = base + off;
        }
        return;
    }
    default: {
        u32 opc = insn >> 22 & 3;
        int wb = 0, sign = 0, to64 = 0, isload;
        u64 newbase = 0;
        base = XSP(rn);
        if (insn >> 24 & 1)
            a = base + ((u64)(insn >> 10 & 0xfff) << (vec && (opc & 2) ? 4 : size));
        else if (insn >> 21 & 1) {
            u32 option = insn >> 13 & 7;
            if ((insn >> 10 & 3) != 2 || !(option & 2))
                break;
            a = base + extended(c->x[RM], option, insn >> 12 & 1 ? (u32)(vec && (opc & 2) ? 4 : size) : 0);
        } else {
            u64 off = sext(insn >> 12 & 0x1ff, 9);
            switch (insn >> 10 & 3) {
            case 0: a = base + off; break;
            case 1: a = base; wb = 1; newbase = base + off; break;
            case 2: a = base + off; el = 0; break;
            default: a = base + off; wb = 1; newbase = a;
            }
        }
        if (vec) {
            if (opc & 2) {
                if (size)
                    break;
                size = 4;
            }
            isload = (int)(opc & 1);
        } else {
            isload = opc != 0;
            if (opc >= 2) {
                if (size == 3) {
                    if (opc == 2)
                        return; /* prfm */
                    break;
                }
                if (opc == 3 && size == 2)
                    break;
                sign = 1;
                to64 = opc == 2;
            }
        }
        if (isload)
            load(c, vec, rt, a, size, sign, to64, el);
        else
            store(c, vec, rt, a, size, el);
        if (wb) {
            if (rn == 31)
                c->sp = newbase;
            else
                c->x[rn] = newbase;
        }
        return;
    }
    }
    undefined(c, insn);
}

static void dp_reg(Cpu *c, u32 insn)
{
    int sf = (int)(insn >> 31);
    u32 rd = RD, rn = RN, rm = RM, opc = insn >> 29 & 3, size = sf ? 64 : 32;
    u64 a, b, r;
    if (!(insn >> 28 & 1)) {
        if (!(insn >> 24 & 1)) { /* logical, shifted register */
            b = shifted(c->x[rm], insn >> 22 & 3, insn >> 10 & 63, sf);
            if (insn >> 21 & 1)
                b = ~b;
            a = c->x[rn];
            r = opc == 1 ? a | b : opc == 2 ? a ^ b : a & b;
            if (!sf)
                r &= M32;
            if (opc == 3)
                logic_flags(c, sf, r);
            c->x[rd] = r;
            return;
        }
        if (insn >> 21 & 1) { /* add/sub, extended register */
            a = XSP(rn);
            b = extended(c->x[rm], insn >> 13 & 7, insn >> 10 & 7);
        } else {
            a = c->x[rn];
            b = shifted(c->x[rm], insn >> 22 & 3, insn >> 10 & 63, sf);
        }
        if (opc & 1)
            c->x[rd] = adds(c, sf, a, opc & 2 ? ~b : b, opc >> 1);
        else {
            r = opc & 2 ? a - b : a + b;
            if (!sf)
                r &= M32;
            if (rd == 31 && (insn >> 21 & 1))
                c->sp = r;
            else
                c->x[rd] = r;
        }
        return;
    }
    if (insn >> 24 & 1) { /* multiply */
        u32 op31 = insn >> 21 & 7, sub = insn >> 15 & 1;
        a = c->x[rn];
        b = c->x[rm];
        switch (op31) {
        case 0: r = a * b; break;
        case 1: r = (u64)((s64)(s32)a * (s64)(s32)b); break;
        case 5: r = (a & M32) * (b & M32); break;
        case 2: c->x[rd] = umulh(a, b) - ((s64)a < 0 ? b : 0) - ((s64)b < 0 ? a : 0); return;
        case 6: c->x[rd] = umulh(a, b); return;
        default: undefined(c, insn); return;
        }
        r = sub ? c->x[RA] - r : c->x[RA] + r;
        c->x[rd] = sf ? r : r & M32;
        return;
    }
    switch (insn >> 21 & 15) {
    case 0: /* adc, sbc */
        a = c->x[rn];
        b = opc & 2 ? ~c->x[rm] : c->x[rm];
        if (opc & 1)
            c->x[rd] = adds(c, sf, a, b, c->nzcv >> 1 & 1);
        else {
            r = a + b + (c->nzcv >> 1 & 1);
            c->x[rd] = sf ? r : r & M32;
        }
        return;
    case 2: /* ccmp, ccmn */
        if (cond(c, insn >> 12 & 15)) {
            a = c->x[rn];
            b = insn >> 11 & 1 ? rm : c->x[rm];
            adds(c, sf, a, opc & 2 ? ~b : b, opc >> 1);
        } else
            c->nzcv = insn & 15;
        return;
    case 4: /* csel and friends */
        if (cond(c, insn >> 12 & 15))
            r = c->x[rn];
        else {
            r = c->x[rm];
            if (opc & 2)
                r = ~r;
            if (insn >> 10 & 1)
                r++;
        }
        c->x[rd] = sf ? r : r & M32;
        return;
    case 6:
        a = c->x[rn];
        if (!sf)
            a &= M32;
        if (insn >> 30 & 1) {
            switch (insn >> 10 & 63) {
            case 0: r = rbit(a, size); break;
            case 1: r = rev(a, 2); break;
            case 2: r = rev(a, 4); break;
            case 3: r = rev(a, 8); break;
            case 4: r = clz(a, size); break;
            case 5: r = clz(sext(a, (int)size) >> (size - 1) & 1 ? ~a & (sf ? ~(u64)0 : M32) : a, size) - 1; break;
            default: undefined(c, insn); return;
            }
        } else {
            b = c->x[rm];
            if (!sf)
                b &= M32;
            switch (insn >> 10 & 63) {
            case 2: r = b ? a / b : 0; break;
            case 3:
                if (!b)
                    r = 0;
                else if (sf)
                    r = b == ~(u64)0 ? 0 - a : (u64)((s64)a / (s64)b);
                else
                    r = b == M32 ? 0 - a : (u64)((s32)a / (s32)b);
                break;
            case 8: r = shifted(a, 0, (u32)b & (size - 1), sf); break;
            case 9: r = shifted(a, 1, (u32)b & (size - 1), sf); break;
            case 10: r = shifted(a, 2, (u32)b & (size - 1), sf); break;
            case 11: r = shifted(a, 3, (u32)b & (size - 1), sf); break;
            default: undefined(c, insn); return;
            }
        }
        c->x[rd] = sf ? r : r & M32;
        return;
    }
    undefined(c, insn);
}

/* ---- the loop ---- */

void cpu_reset(Cpu *c, Vm *vm, int id, u64 pc, u64 x0)
{
    memset(c, 0, sizeof *c);
    c->vm = vm;
    c->id = id;
    c->debug = vm->debug;
    c->pc = pc;
    c->x[0] = x0;
    c->el = 1;
    c->spsel = 1;
    c->daif = 0xf;
    c->sctlr = 0x00c50838;
    tlb_flush(c);
}

/* Run one CPU until its slice is used up, it goes idle or is switched off, or the machine stops. */
static void cpu_slice(Cpu *c)
{
    Vm *vm = c->vm;
    setjmp(c->jb);
    for (;;) {
        u64 pc;
        u32 insn;
        Tlb *e;
        if (c->icount >= c->next_event) {
            if (c->halted || vm->poweroff || vm->cpu_state[c->id] != 1 || c->icount >= c->slice_end)
                return;
            events(c);
        }
        pc = c->pc;
        e = &c->tlb[c->el][2][(pc >> 12) & (NTLB - 1)];
        if (e->tag == pc >> 12)
            insn = *(u32 *)(e->host + (pc & 0xffc));
        else
            insn = *(u32 *)resolve(c, pc & ~(u64)3, 2, c->el);
        c->pc = pc + 4;
        c->icount++;
        c->x[31] = 0;
        switch (insn >> 25 & 15) {
        case 8:
        case 9: dp_imm(c, insn, pc); break;
        case 10:
        case 11: branch(c, insn, pc); break;
        case 4:
        case 6:
        case 12:
        case 14: loadstore(c, insn, pc); break;
        case 5:
        case 13: dp_reg(c, insn); break;
        case 7:
        case 15:
            if (!fp_trap(c) && !fp_exec(c, insn))
                undefined(c, insn);
            break;
        default: undefined(c, insn);
        }
    }
}

/* The interpreter's machine: the CPUs take turns on this one thread, a slice each, and time is the
   number of instructions all of them have executed. */
void emu_run(Vm *vm)
{
    Cpu *cpus = vm->cpus;
    u64 now = 0, next_poll = 0, quantum = vm->ncpu > 1 ? 20000 : POLL;
    int i;
    while (!vm->poweroff) {
        int ran = 0, pending = 0;
        u64 d = 0;
        for (i = 0; i < vm->ncpu && !vm->poweroff; i++) {
            Cpu *c = &cpus[i];
            if (vm->cpu_state[i] == 2) {
                cpu_reset(c, vm, i, vm->start_pc[i], vm->start_x0[i]);
                vm->cpu_state[i] = 1;
            }
            if (vm->cpu_state[i] != 1)
                continue;
            c->toff = now - c->icount;
            if (c->halted) {
                timers(c);
                if (!vm->irq_out[i])
                    continue;
                c->halted = 0;
            }
            c->slice_end = c->icount + quantum;
            c->next_event = 0;
            cpu_slice(c);
            now = c->icount + c->toff;
            c->excl_valid = 0; /* another CPU may write before this one stores */
            ran = 1;
        }
        if (vm->poweroff)
            break;
        if (now >= next_poll || !ran) {
            uart_flush(vm);
            uart_input(vm);
            net_poll(vm);
            gpu_poll(vm);
            next_poll = now + POLL;
        }
        if (ran)
            continue;
        /* every CPU is idle: move to the next timer, in real time if someone outside may still act */
        for (i = 0; i < vm->ncpu; i++) {
            u64 w;
            if (vm->cpu_state[i] != 1)
                continue;
            cpus[i].toff = now - cpus[i].icount;
            timers(&cpus[i]);
            pending |= vm->irq_out[i];
            w = timer_wait(&cpus[i]);
            if (w && (!d || w < d))
                d = w;
        }
        if (pending || vm->poweroff)
            continue;
        if (vm_can_wake(vm)) {
            u64 t0 = now_usec(), el;
            vm_wait(vm, d ? (long)(d / (TIMER_HZ / 1000000) + 1) : 1000000);
            el = (now_usec() - t0) * (TIMER_HZ / 1000000);
            now += d && el >= d ? d : el;
        } else if (d)
            now += d;
        else {
            fprintf(stderr, "mvm: the guest sleeps and nothing can wake it\n");
            vm->status = 1;
            break;
        }
    }
    uart_flush(vm);
}
