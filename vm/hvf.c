/* Hardware virtualization on macOS: the guest runs on the real CPUs through Hypervisor.framework (the
   counterpart of Linux's KVM), one host thread per guest CPU, and comes back here for devices, firmware
   calls and idling. The device models are shared, so one lock covers everything outside the guest.
   The binary needs the com.apple.security.hypervisor entitlement. */
#include "vm.h"

typedef struct HvExit {
    u32 reason;
    u32 pad;
    u64 syndrome, va, pa;
} HvExit;

extern int hv_vm_create(void *config);
extern int hv_vm_map(void *addr, u64 ipa, u64 size, u64 flags);
extern int hv_vcpu_create(u64 *vcpu, HvExit **exit, void *config);
extern int hv_vcpu_run(u64 vcpu);
extern int hv_vcpus_exit(u64 *vcpus, u32 count);
extern int hv_vcpu_get_reg(u64 vcpu, u32 reg, u64 *value);
extern int hv_vcpu_set_reg(u64 vcpu, u32 reg, u64 value);
extern int hv_vcpu_get_sys_reg(u64 vcpu, u32 reg, u64 *value);
extern int hv_vcpu_set_sys_reg(u64 vcpu, u32 reg, u64 value);
extern int hv_vcpu_set_pending_interrupt(u64 vcpu, u32 type, int pending);
extern int hv_vcpu_set_vtimer_mask(u64 vcpu, int masked);
extern int hv_vcpu_set_vtimer_offset(u64 vcpu, u64 offset);
extern u64 mach_absolute_time(void);
extern int mach_timebase_info(u32 *info);
/* threads come from the system's library too */
extern int pthread_create(void **thread, void *attr, void *(*fn)(void *), void *arg);
extern int pthread_mutex_init(long *m, void *attr);
extern int pthread_mutex_lock(long *m);
extern int pthread_mutex_unlock(long *m);
extern int pthread_cond_init(long *c, void *attr);
extern int pthread_cond_signal(long *c);
extern int pthread_cond_wait(long *c, long *m);
extern int pthread_cond_timedwait_relative_np(long *c, long *m, long *timespec);

#define REG_PC 31
#define REG_CPSR 34
#define CNTV_CTL 0xdf19  /* 3 3 14 3 1 */
#define CNTV_CVAL 0xdf1a /* 3 3 14 3 2 */
#define SCTLR_EL1 0xc080
#define MPIDR_EL1 0xc005

typedef struct HvCpu {
    u64 vcpu;
    HvExit *ex;
    int id, created, masked;
    long cond[6];
} HvCpu;

static Vm *gvm;
static HvCpu hc[MAXCPU];
static long big[8];  /* the lock around everything that is not guest code */
static int cur_cpu;  /* the CPU whose thread holds the lock, -1 for the I/O thread */
static u64 offset, nexits;
static u32 tb[2];

static void check(int r, const char *what)
{
    if (r) {
        fprintf(stderr, "mvm: %s failed (%x)\n", what, (unsigned)r);
        exit(2);
    }
}

static void *vcpu_main(void *arg);

/* A CPU has an interrupt or a start order waiting (called with the lock held). */
static void kick(Vm *vm, int cpu)
{
    HvCpu *h = &hc[cpu];
    void *t;
    if (!h->created) {
        if (vm->cpu_state[cpu] == 2 && h->id < 0) {
            h->id = cpu;
            pthread_cond_init(h->cond, 0);
            check(pthread_create(&t, 0, vcpu_main, h), "pthread_create");
        }
        return;
    }
    pthread_cond_signal(h->cond);
    if (cpu != cur_cpu)
        hv_vcpus_exit(&h->vcpu, 1);
}

static void device_access(Vm *vm, HvCpu *h, u64 syn, u64 pc)
{
    u32 rt = (u32)(syn >> 16) & 31;
    int size = 1 << (syn >> 22 & 3);
    u64 v;
    if (!(syn >> 24 & 1)) {
        fprintf(stderr, "mvm: device access at %lx by an instruction that cannot be decoded (pc %lx)\n", h->ex->pa, pc);
        exit(2);
    }
    if (syn >> 6 & 1) {
        v = 0;
        if (rt != 31)
            hv_vcpu_get_reg(h->vcpu, rt, &v);
        vm_mmio_write(vm, h->id, h->ex->pa, size, v);
    } else {
        v = vm_mmio_read(vm, h->id, h->ex->pa, size);
        if ((syn >> 21 & 1) && size < 8)
            v = (u64)((s64)(v << (64 - 8 * size)) >> (64 - 8 * size));
        if (!(syn >> 15 & 1))
            v &= 0xffffffffUL;
        if (rt != 31)
            hv_vcpu_set_reg(h->vcpu, rt, v);
    }
    hv_vcpu_set_reg(h->vcpu, REG_PC, pc + 4);
}

/* wfi: sleep until the timer is due or somebody kicks this CPU */
static void idle(Vm *vm, HvCpu *h)
{
    u64 ctl, cval, now, usec = 1000000;
    long ts[2];
    if (vm->irq_out[h->id])
        return;
    hv_vcpu_get_sys_reg(h->vcpu, CNTV_CTL, &ctl);
    hv_vcpu_get_sys_reg(h->vcpu, CNTV_CVAL, &cval);
    now = mach_absolute_time() - offset;
    uart_flush(vm);
    if ((ctl & 3) == 1) {
        if (cval <= now)
            return;
        usec = (cval - now) * tb[0] / tb[1] / 1000;
        if (usec > 1000000)
            usec = 1000000;
    } else if (vm->ncpu == 1 && !vm_can_wake(vm)) {
        fprintf(stderr, "mvm: the guest sleeps and nothing can wake it\n");
        vm->status = 1;
        vm->poweroff = 1;
        return;
    }
    ts[0] = (long)(usec / 1000000);
    ts[1] = (long)(usec % 1000000) * 1000;
    pthread_cond_timedwait_relative_np(h->cond, big, ts);
}

static void *vcpu_main(void *arg)
{
    HvCpu *h = (HvCpu *)arg;
    Vm *vm = gvm;
    int id = h->id, irq;
    u64 v, pc, syn, ec;
    check(hv_vcpu_create(&h->vcpu, &h->ex, 0), "hv_vcpu_create");
    hv_vcpu_set_sys_reg(h->vcpu, MPIDR_EL1, 0x80000000UL | (u64)id);
    hv_vcpu_set_vtimer_offset(h->vcpu, offset);
    pthread_mutex_lock(big);
    h->created = 1;
    for (;;) {
        while (vm->cpu_state[id] == 0 && !vm->poweroff)
            pthread_cond_wait(h->cond, big);
        if (vm->poweroff)
            break;
        if (vm->cpu_state[id] == 2) { /* (re)start: EL1h, interrupts masked, MMU and caches off */
            hv_vcpu_set_reg(h->vcpu, REG_PC, vm->start_pc[id]);
            hv_vcpu_set_reg(h->vcpu, 0, vm->start_x0[id]);
            hv_vcpu_set_reg(h->vcpu, REG_CPSR, 0x3c5);
            hv_vcpu_get_sys_reg(h->vcpu, SCTLR_EL1, &v);
            hv_vcpu_set_sys_reg(h->vcpu, SCTLR_EL1, v & ~(u64)0x1005);
            vm->cpu_state[id] = 1;
        }
        if (h->masked) { /* the timer interrupt stays up until the guest silences its timer */
            hv_vcpu_get_sys_reg(h->vcpu, CNTV_CTL, &v);
            if ((v & 7) != 5) {
                gic_set_level(vm, id, IRQ_VTIMER, 0);
                hv_vcpu_set_vtimer_mask(h->vcpu, 0);
                h->masked = 0;
            }
        }
        if (vm->nout && vm->out[vm->nout - 1] == '\n')
            uart_flush(vm);
        irq = vm->irq_out[id];
        cur_cpu = -1;
        pthread_mutex_unlock(big);
        hv_vcpu_set_pending_interrupt(h->vcpu, 0, irq);
        check(hv_vcpu_run(h->vcpu), "hv_vcpu_run");
        pthread_mutex_lock(big);
        cur_cpu = id;
        nexits++;
        if (h->ex->reason == 2) {
            gic_set_level(vm, id, IRQ_VTIMER, 1);
            h->masked = 1;
            continue;
        }
        if (h->ex->reason != 1)
            continue;
        syn = h->ex->syndrome;
        ec = syn >> 26 & 63;
        hv_vcpu_get_reg(h->vcpu, REG_PC, &pc);
        if (ec == 0x24)
            device_access(vm, h, syn, pc);
        else if (ec == 0x16) {
            u64 a[4];
            int i;
            for (i = 0; i < 4; i++)
                hv_vcpu_get_reg(h->vcpu, (u32)i, &a[i]);
            hv_vcpu_set_reg(h->vcpu, 0, psci_call(vm, id, a[0], a[1], a[2], a[3]));
        } else if (ec == 0x17) {
            hv_vcpu_set_reg(h->vcpu, 0, (u64)-1);
            hv_vcpu_set_reg(h->vcpu, REG_PC, pc + 4);
        } else if (ec == 0x18) { /* a system register the hypervisor does not provide: reads as zero */
            if ((syn & 1) && (syn >> 5 & 31) != 31)
                hv_vcpu_set_reg(h->vcpu, (u32)(syn >> 5) & 31, 0);
            if (vm->debug > 1)
                fprintf(stderr, "mvm: system register access trapped, syndrome %lx at %lx\n", syn, pc);
            hv_vcpu_set_reg(h->vcpu, REG_PC, pc + 4);
        } else if (ec == 0x01) {
            hv_vcpu_set_reg(h->vcpu, REG_PC, pc + 4);
            if (!(syn & 1))
                idle(vm, h);
        } else {
            fprintf(stderr, "mvm: unexpected exit from the guest: syndrome %lx, pc %lx, address %lx\n", syn, pc, h->ex->va);
            vm->status = 2;
            vm->poweroff = 1;
        }
    }
    pthread_mutex_unlock(big);
    return 0;
}

/* -1 when this machine or this binary may not use the hypervisor */
int hvf_run(Vm *vm, int debug)
{
    int i;
    if (hv_vm_create(0))
        return -1;
    check(hv_vm_map(vm->ram, RAM_BASE, vm->ram_size, 7), "hv_vm_map");
    gvm = vm;
    offset = mach_absolute_time();
    mach_timebase_info(tb);
    pthread_mutex_init(big, 0);
    for (i = 0; i < MAXCPU; i++)
        hc[i].id = -1;
    cur_cpu = -1;
    vm->kick = kick;
    pthread_mutex_lock(big);
    kick(vm, 0); /* cpu_state[0] is 2: this starts its thread */
    while (!vm->poweroff) {
        pthread_mutex_unlock(big);
        vm_wait(vm, 20000);
        pthread_mutex_lock(big);
        cur_cpu = -1;
        uart_input(vm);
        net_poll(vm);
        gpu_poll(vm);
        if (vm->nout)
            uart_flush(vm);
    }
    for (i = 0; i < vm->ncpu; i++)
        if (hc[i].created) {
            pthread_cond_signal(hc[i].cond);
            hv_vcpus_exit(&hc[i].vcpu, 1);
        }
    uart_flush(vm);
    pthread_mutex_unlock(big);
    if (debug)
        fprintf(stderr, "mvm: %lu exits from the guest\n", nexits);
    return 0;
}
