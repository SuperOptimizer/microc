/* The machine around the CPU: GICv2, PL011, PSCI and the device tree that describes them. */
#include "vm.h"
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

u64 now_usec(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (u64)tv.tv_sec * 1000000 + (u64)tv.tv_usec;
}

/* ---- interrupt controller ---- */

#define BANK(cpu, i) ((i) < 32 ? (cpu) : 0)

static int gic_pending(Vm *vm, int cpu, int i)
{
    int b = BANK(cpu, i);
    if (i >= 32 && !(vm->irq_target[i] >> cpu & 1))
        return 0;
    return vm->irq_latch[b][i] || (vm->irq_level[b][i] && !vm->irq_edge[b][i]);
}

static int gic_best(Vm *vm, int cpu)
{
    int i, best = -1;
    unsigned limit = vm->gicc_pmr[cpu];
    for (i = 0; i < NIRQ; i++)
        if (vm->irq_active[cpu][i] && vm->irq_prio[BANK(cpu, i)][i] < limit)
            limit = vm->irq_prio[BANK(cpu, i)][i];
    for (i = 0; i < NIRQ; i++) {
        int b = BANK(cpu, i);
        if (vm->irq_enabled[b][i] && !vm->irq_active[b][i] && vm->irq_prio[b][i] < limit && gic_pending(vm, cpu, i)) {
            best = i;
            limit = vm->irq_prio[b][i];
        }
    }
    return best;
}

static void gic_update(Vm *vm)
{
    int c;
    for (c = 0; c < vm->ncpu; c++) {
        int out = (vm->gicd_ctlr & 1) && (vm->gicc_ctlr[c] & 1) && gic_best(vm, c) >= 0;
        if (out && !vm->irq_out[c]) {
            vm->irq_out[c] = 1;
            if (vm->kick)
                vm->kick(vm, c);
        } else
            vm->irq_out[c] = out;
    }
}

void gic_set_level(Vm *vm, int cpu, int irq, int level)
{
    int b = BANK(cpu, irq);
    level = level != 0;
    if (vm->irq_level[b][irq] == level)
        return;
    vm->irq_level[b][irq] = (u8)level;
    if (level && vm->irq_edge[b][irq])
        vm->irq_latch[b][irq] = 1;
    gic_update(vm);
}

/* one register of 32 bits, one bit per line: read it, or set the lines whose bits are in v to `to` */
static u32 gic_bits(Vm *vm, int cpu, u8 a[MAXCPU][NIRQ], u32 n)
{
    u32 v = 0, i;
    if (n * 32 >= NIRQ)
        return 0;
    for (i = 0; i < 32; i++)
        if (a[BANK(cpu, n * 32 + i)][n * 32 + i])
            v |= (u32)1 << i;
    (void)vm;
    return v;
}

static void gic_setbits(int cpu, u8 a[MAXCPU][NIRQ], u32 n, u32 v, int to)
{
    u32 i;
    if (n * 32 >= NIRQ)
        return;
    for (i = 0; i < 32; i++)
        if (v >> i & 1)
            a[BANK(cpu, n * 32 + i)][n * 32 + i] = (u8)to;
}

static u32 gicd_read(Vm *vm, int cpu, u32 off)
{
    u32 n = (off & 0x7f) / 4, i, v = 0;
    if (off == 0)
        return vm->gicd_ctlr;
    if (off == 4)
        return (NIRQ / 32 - 1) | (u32)(vm->ncpu - 1) << 5;
    if (off == 8)
        return 0x0200043b;
    if (off >= 0x100 && off < 0x200)
        return gic_bits(vm, cpu, vm->irq_enabled, n);
    if (off >= 0x200 && off < 0x300) {
        for (i = 0; i < 32 && n * 32 + i < NIRQ; i++)
            if (gic_pending(vm, cpu, (int)(n * 32 + i)))
                v |= (u32)1 << i;
        return v;
    }
    if (off >= 0x300 && off < 0x400)
        return gic_bits(vm, cpu, vm->irq_active, n);
    if (off >= 0x400 && off < 0x800) {
        for (i = 0; i < 4; i++)
            if (off - 0x400 + i < NIRQ)
                v |= (u32)vm->irq_prio[BANK(cpu, off - 0x400 + i)][off - 0x400 + i] << (8 * i);
        return v;
    }
    if (off >= 0x800 && off < 0xc00) {
        for (i = 0; i < 4; i++)
            if (off - 0x800 + i < 32)
                v |= ((u32)1 << cpu) << (8 * i);
            else if (off - 0x800 + i < NIRQ)
                v |= (u32)vm->irq_target[off - 0x800 + i] << (8 * i);
        return v;
    }
    if (off >= 0xc00 && off < 0xd00) {
        n = (off - 0xc00) / 4;
        for (i = 0; i < 16; i++)
            if (n * 16 + i < NIRQ && vm->irq_edge[BANK(cpu, n * 16 + i)][n * 16 + i])
                v |= (u32)2 << (2 * i);
        return v;
    }
    if (off == 0xfe8)
        return 0x2b; /* architecture revision 2 */
    return 0;
}

static void gicd_write(Vm *vm, int cpu, u32 off, u32 v, int size)
{
    u32 n = (off & 0x7f) / 4, i;
    int c;
    if (off == 0)
        vm->gicd_ctlr = v;
    else if (off >= 0x100 && off < 0x180)
        gic_setbits(cpu, vm->irq_enabled, n, v, 1);
    else if (off >= 0x180 && off < 0x200)
        gic_setbits(cpu, vm->irq_enabled, n, v, 0);
    else if (off >= 0x200 && off < 0x280)
        gic_setbits(cpu, vm->irq_latch, n, v, 1);
    else if (off >= 0x280 && off < 0x300)
        gic_setbits(cpu, vm->irq_latch, n, v, 0);
    else if (off >= 0x300 && off < 0x380)
        gic_setbits(cpu, vm->irq_active, n, v, 1);
    else if (off >= 0x380 && off < 0x400)
        gic_setbits(cpu, vm->irq_active, n, v, 0);
    else if (off >= 0x400 && off < 0x800) {
        for (i = 0; i < (u32)size; i++)
            if (off - 0x400 + i < NIRQ)
                vm->irq_prio[BANK(cpu, off - 0x400 + i)][off - 0x400 + i] = (u8)(v >> (8 * i));
    } else if (off >= 0x800 && off < 0xc00) {
        for (i = 0; i < (u32)size; i++)
            if (off - 0x800 + i >= 32 && off - 0x800 + i < NIRQ)
                vm->irq_target[off - 0x800 + i] = (u8)(v >> (8 * i));
    } else if (off >= 0xc00 && off < 0xd00) {
        n = (off - 0xc00) / 4;
        for (i = 0; i < 16; i++)
            if (n * 16 + i < NIRQ && n * 16 + i >= 16)
                vm->irq_edge[BANK(cpu, n * 16 + i)][n * 16 + i] = (u8)(v >> (2 * i + 1) & 1);
    } else if (off == 0xf00) { /* a software interrupt: to the CPUs listed, to all others, or to this one */
        u32 filter = v >> 24 & 3, list = v >> 16 & 0xff;
        for (c = 0; c < vm->ncpu; c++)
            if (filter == 0 ? (int)(list >> c & 1) : filter == 1 ? c != cpu : c == cpu)
                vm->irq_latch[c][v & 15] = 1;
    }
    gic_update(vm);
}

static u32 gicc_read(Vm *vm, int cpu, u32 off)
{
    int i;
    switch (off) {
    case 0x00: return vm->gicc_ctlr[cpu];
    case 0x04: return vm->gicc_pmr[cpu];
    case 0x08: return vm->gicc_bpr[cpu];
    case 0x0c:
        i = (vm->gicd_ctlr & 1) ? gic_best(vm, cpu) : -1;
        if (i < 0)
            return 1023;
        vm->irq_latch[BANK(cpu, i)][i] = 0;
        vm->irq_active[BANK(cpu, i)][i] = 1;
        vm->irq_active[cpu][i] = 1; /* what this CPU is busy with */
        gic_update(vm);
        return (u32)i;
    case 0x14:
        for (i = 0; i < NIRQ; i++)
            if (vm->irq_active[cpu][i])
                return vm->irq_prio[BANK(cpu, i)][i];
        return 0xff;
    case 0x18:
        i = gic_best(vm, cpu);
        return i < 0 ? 1023 : (u32)i;
    case 0xfc: return 0x0202043b;
    }
    return 0;
}

static void gicc_write(Vm *vm, int cpu, u32 off, u32 v)
{
    switch (off) {
    case 0x00: vm->gicc_ctlr[cpu] = v; break;
    case 0x04: vm->gicc_pmr[cpu] = v & 0xff; break;
    case 0x08: vm->gicc_bpr[cpu] = v & 7; break;
    case 0x10:
    case 0x1000:
        if ((v & 1023) < NIRQ) {
            vm->irq_active[BANK(cpu, v & 1023)][v & 1023] = 0;
            vm->irq_active[cpu][v & 1023] = 0;
        }
        break;
    }
    gic_update(vm);
}

/* ---- serial port ---- */

void uart_flush(Vm *vm)
{
    int done = 0;
    while (done < vm->nout) {
        long n = write(1, vm->out + done, (size_t)(vm->nout - done));
        if (n <= 0)
            break;
        done += (int)n;
    }
    vm->nout = 0;
}

void con_out(Vm *vm, int c)
{
    if (vm->nout == (int)sizeof vm->out)
        uart_flush(vm);
    vm->out[vm->nout++] = (char)c;
}

static void uart_irq(Vm *vm) { gic_set_level(vm, 0, IRQ_UART, (vm->uart_ris & vm->uart_imsc) != 0); }

/* Take what the terminal has, without waiting. */
void uart_input(Vm *vm)
{
    fd_set fds;
    struct timeval tv;
    u8 buf[64];
    long n, i;
    int room = vm->vcon ? vcon_room(vm) : (int)sizeof vm->uart_fifo - vm->uart_cnt, k = 0;
    if (!vm->stdin_open || room <= 0)
        return;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(1, &fds, 0, 0, &tv) <= 0)
        return;
    n = read(0, buf, room < (int)sizeof buf ? (size_t)room : sizeof buf);
    if (n <= 0) {
        vm->stdin_open = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        if (vm->raw_tty) { /* ctrl-a x leaves, ctrl-a ctrl-a sends one ctrl-a */
            if (vm->esc) {
                vm->esc = 0;
                if (buf[i] == 'x') {
                    vm->poweroff = 1;
                    vm->status = 0;
                    return;
                }
                if (buf[i] != 1)
                    continue;
            } else if (buf[i] == 1) {
                vm->esc = 1;
                continue;
            }
        }
        if (vm->vcon)
            buf[k++] = buf[i];
        else
            vm->uart_fifo[(vm->uart_rd + vm->uart_cnt++) % (int)sizeof vm->uart_fifo] = buf[i];
    }
    if (vm->vcon) {
        vcon_input(vm, buf, k);
        return;
    }
    if (vm->uart_cnt)
        vm->uart_ris |= 0x50;
    uart_irq(vm);
}

int net_fdset(Vm *vm, fd_set *r, fd_set *w);

int vm_can_wake(Vm *vm) { return vm->stdin_open || vm->gui || net_active(vm); }

/* Sleep until the terminal has something to read, or for usec; 1 if something came. */
int vm_wait(Vm *vm, long usec)
{
    fd_set fds, w;
    struct timeval tv;
    int room = vm->vcon ? vcon_room(vm) : (int)sizeof vm->uart_fifo - vm->uart_cnt, n = 0, k;
    FD_ZERO(&fds);
    FD_ZERO(&w);
    if (vm->stdin_open && room > 0) {
        FD_SET(0, &fds);
        n = 1;
    }
    k = net_fdset(vm, &fds, &w);
    if (k > n)
        n = k;
    if (vm->net && usec > 100000)
        usec = 100000; /* its timers */
    if (vm->gui && usec > 10000)
        usec = 10000;
    tv.tv_sec = usec / 1000000;
    tv.tv_usec = (int)(usec % 1000000);
    return select(n, &fds, &w, 0, &tv) > 0;
}

static u32 uart_read(Vm *vm, u32 off)
{
    static const u8 id[8] = { 0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };
    u32 v;
    switch (off) {
    case 0x00:
        if (!vm->uart_cnt)
            return 0;
        v = vm->uart_fifo[vm->uart_rd];
        vm->uart_rd = (vm->uart_rd + 1) % (int)sizeof vm->uart_fifo;
        if (--vm->uart_cnt == 0)
            vm->uart_ris &= ~(u32)0x50;
        uart_irq(vm);
        return v;
    case 0x18: return 0x80 | (vm->uart_cnt ? 0 : 0x10) | (vm->uart_cnt == (int)sizeof vm->uart_fifo ? 0x40 : 0);
    case 0x24: return vm->uart_ibrd;
    case 0x28: return vm->uart_fbrd;
    case 0x2c: return vm->uart_lcrh;
    case 0x30: return vm->uart_cr;
    case 0x34: return vm->uart_ifls;
    case 0x38: return vm->uart_imsc;
    case 0x3c: return vm->uart_ris;
    case 0x40: return vm->uart_ris & vm->uart_imsc;
    }
    if (off >= 0xfe0 && off < 0x1000)
        return id[(off - 0xfe0) / 4];
    return 0;
}

static void uart_write(Vm *vm, u32 off, u32 v)
{
    switch (off) {
    case 0x00:
        con_out(vm, (int)v);
        vm->uart_ris |= 0x20;
        break;
    case 0x24: vm->uart_ibrd = v; break;
    case 0x28: vm->uart_fbrd = v; break;
    case 0x2c: vm->uart_lcrh = v; break;
    case 0x30: vm->uart_cr = v; break;
    case 0x34: vm->uart_ifls = v; break;
    case 0x38: vm->uart_imsc = v; break;
    case 0x44: vm->uart_ris &= ~v; break;
    }
    uart_irq(vm);
}

static struct termios saved_tio;

void term_raw(Vm *vm)
{
    struct termios t;
    if (!isatty(0) || tcgetattr(0, &saved_tio) < 0)
        return;
    t = saved_tio;
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) == 0)
        vm->raw_tty = 1;
}

void term_restore(Vm *vm)
{
    uart_flush(vm);
    if (vm->raw_tty)
        tcsetattr(0, TCSANOW, &saved_tio);
    vm->raw_tty = 0;
}

/* ---- bus ---- */

u64 vm_mmio_read(Vm *vm, int cpu, u64 pa, int size)
{
    if (pa >= GICD_BASE && pa < GICD_BASE + 0x1000) {
        u32 off = (u32)(pa - GICD_BASE), v = gicd_read(vm, cpu, off & ~(u32)3);
        return size == 1 ? (v >> (8 * (off & 3))) & 0xff : v;
    }
    if (pa >= GICC_BASE && pa < GICC_BASE + 0x2000)
        return gicc_read(vm, cpu, (u32)(pa - GICC_BASE));
    if (pa >= UART_BASE && pa < UART_BASE + 0x1000)
        return uart_read(vm, (u32)(pa - UART_BASE));
    if (pa >= VIRTIO_BASE && pa < VIRTIO_BASE + NVIRTIO * 0x200 && vm->virtio[(pa - VIRTIO_BASE) / 0x200]) {
        u32 off = (u32)(pa - VIRTIO_BASE) % 0x200;
        u64 v = virtio_read(vm, vm->virtio[(pa - VIRTIO_BASE) / 0x200], off & ~(u32)3);
        return size == 1 ? v >> (8 * (off & 3)) & 0xff : size == 2 ? v >> (8 * (off & 3)) & 0xffff : v;
    }
    return 0;
}

void vm_mmio_write(Vm *vm, int cpu, u64 pa, int size, u64 val)
{
    if (pa >= GICD_BASE && pa < GICD_BASE + 0x1000)
        gicd_write(vm, cpu, (u32)(pa - GICD_BASE), (u32)val, size);
    else if (pa >= GICC_BASE && pa < GICC_BASE + 0x2000)
        gicc_write(vm, cpu, (u32)(pa - GICC_BASE), (u32)val);
    else if (pa >= UART_BASE && pa < UART_BASE + 0x1000)
        uart_write(vm, (u32)(pa - UART_BASE), (u32)val);
    else if (pa >= VIRTIO_BASE && pa < VIRTIO_BASE + NVIRTIO * 0x200 && vm->virtio[(pa - VIRTIO_BASE) / 0x200]) {
        Virtio *d = vm->virtio[(pa - VIRTIO_BASE) / 0x200];
        u32 off = (u32)(pa - VIRTIO_BASE) % 0x200, k;
        if (off >= 0x100) { /* its own registers, byte by byte */
            for (k = 0; k < (u32)size && off - 0x100 + k < sizeof d->config; k++)
                d->config[off - 0x100 + k] = (u8)(val >> (8 * k));
            if (d->cfg_write)
                d->cfg_write(vm, d);
        } else
            virtio_write(vm, d, off, (u32)val);
    }
}

/* ---- firmware calls ---- */

u64 psci_call(Vm *vm, int cpu, u64 fn, u64 a1, u64 a2, u64 a3)
{
    int t;
    switch ((u32)fn) {
    case 0x84000000: return 2; /* version 0.2 */
    case 0x84000006: return 2; /* no migration needed */
    case 0x84000002:           /* CPU_OFF */
        vm->cpu_state[cpu] = 0;
        return 0;
    case 0x84000003:
    case 0xc4000003: /* CPU_ON: target, entry point, context */
        t = (int)(a1 & 0xff);
        if ((a1 & ~(u64)0xff) || t >= vm->ncpu)
            return (u64)-2;
        if (vm->cpu_state[t])
            return (u64)-4;
        vm->start_pc[t] = a2;
        vm->start_x0[t] = a3;
        vm->cpu_state[t] = 2;
        if (vm->kick)
            vm->kick(vm, t);
        return 0;
    case 0x84000004:
    case 0xc4000004: /* AFFINITY_INFO */
        t = (int)(a1 & 0xff);
        return t < vm->ncpu ? (u64)(vm->cpu_state[t] == 0) : (u64)-2;
    case 0x84000008:
    case 0x84000009:
        vm->poweroff = 1;
        vm->status = 0;
        return 0;
    }
    return (u64)-1; /* not supported */
}

/* ---- device tree ---- */

static u8 *fs, *fstr;
static u32 nfs, nfstr;

static void f32(u32 v)
{
    fs[nfs++] = (u8)(v >> 24);
    fs[nfs++] = (u8)(v >> 16);
    fs[nfs++] = (u8)(v >> 8);
    fs[nfs++] = (u8)v;
}

static void fbytes(const void *p, u32 n)
{
    memcpy(fs + nfs, p, n);
    nfs += n;
    while (nfs & 3)
        fs[nfs++] = 0;
}

static void fnode(const char *name)
{
    f32(1);
    fbytes(name, (u32)strlen(name) + 1);
}

static void fprop(const char *name, const void *data, u32 len)
{
    u32 off = 0;
    while (off < nfstr && strcmp((char *)fstr + off, name) != 0)
        off += (u32)strlen((char *)fstr + off) + 1;
    if (off == nfstr) {
        strcpy((char *)fstr + nfstr, name);
        nfstr += (u32)strlen(name) + 1;
    }
    f32(3);
    f32(len);
    f32(off);
    fbytes(data, len);
}

static void fcells(const char *name, int n, const u32 *v)
{
    u8 b[64];
    int i;
    for (i = 0; i < n; i++) {
        b[4 * i] = (u8)(v[i] >> 24);
        b[4 * i + 1] = (u8)(v[i] >> 16);
        b[4 * i + 2] = (u8)(v[i] >> 8);
        b[4 * i + 3] = (u8)v[i];
    }
    fprop(name, b, (u32)n * 4);
}

static void fcell(const char *name, u32 v) { fcells(name, 1, &v); }
static void fstring(const char *name, const char *s) { fprop(name, s, (u32)strlen(s) + 1); }

u32 fdt_build(Vm *vm, u8 *out, const char *cmdline, u64 initrd, u64 initrd_size)
{
    static u8 sbuf[8192], strbuf[1024];
    u32 c[16], total, off_struct = 40 + 16, off_str, ppi = 4 | (((u32)1 << vm->ncpu) - 1) << 8;
    int i;
    fs = sbuf;
    fstr = strbuf;
    nfs = nfstr = 0;
    fnode("");
    fcell("#address-cells", 2);
    fcell("#size-cells", 2);
    fcell("interrupt-parent", 1);
    fstring("compatible", "linux,dummy-virt");
    fstring("model", "mcc-vm");
    fnode("chosen");
    fstring("bootargs", cmdline);
    fstring("stdout-path", "/pl011@9000000");
    if (initrd_size) {
        c[0] = (u32)(initrd >> 32);
        c[1] = (u32)initrd;
        fcells("linux,initrd-start", 2, c);
        c[0] = (u32)((initrd + initrd_size) >> 32);
        c[1] = (u32)(initrd + initrd_size);
        fcells("linux,initrd-end", 2, c);
    }
    f32(2);
    fnode("memory@40000000");
    fstring("device_type", "memory");
    c[0] = 0;
    c[1] = (u32)RAM_BASE;
    c[2] = (u32)(vm->ram_size >> 32);
    c[3] = (u32)vm->ram_size;
    fcells("reg", 4, c);
    f32(2);
    fnode("cpus");
    fcell("#address-cells", 1);
    fcell("#size-cells", 0);
    for (i = 0; i < vm->ncpu; i++) {
        char name[16];
        sprintf(name, "cpu@%d", i);
        fnode(name);
        fstring("device_type", "cpu");
        fstring("compatible", "arm,armv8");
        fstring("enable-method", "psci");
        fcell("reg", (u32)i);
        f32(2);
    }
    f32(2);
    fnode("psci");
    fprop("compatible", "arm,psci-0.2\0arm,psci", 22);
    fstring("method", "hvc");
    f32(2);
    fnode("intc@8000000");
    fstring("compatible", "arm,cortex-a15-gic");
    fcell("#interrupt-cells", 3);
    fprop("interrupt-controller", "", 0);
    c[0] = 0, c[1] = (u32)GICD_BASE, c[2] = 0, c[3] = 0x10000;
    c[4] = 0, c[5] = (u32)GICC_BASE, c[6] = 0, c[7] = 0x10000;
    fcells("reg", 8, c);
    fcell("phandle", 1);
    f32(2);
    fnode("timer");
    fstring("compatible", "arm,armv8-timer");
    c[0] = 1, c[1] = 13, c[2] = ppi, c[3] = 1, c[4] = 14, c[5] = ppi;
    c[6] = 1, c[7] = 11, c[8] = ppi, c[9] = 1, c[10] = 10, c[11] = ppi;
    fcells("interrupts", 12, c);
    fprop("always-on", "", 0);
    f32(2);
    fnode("apb-pclk");
    fstring("compatible", "fixed-clock");
    fcell("#clock-cells", 0);
    fcell("clock-frequency", 24000000);
    fstring("clock-output-names", "clk24mhz");
    fcell("phandle", 2);
    f32(2);
    fnode("pl011@9000000");
    fprop("compatible", "arm,pl011\0arm,primecell", 24);
    c[0] = 0, c[1] = (u32)UART_BASE, c[2] = 0, c[3] = 0x1000;
    fcells("reg", 4, c);
    c[0] = 0, c[1] = IRQ_UART - 32, c[2] = 4;
    fcells("interrupts", 3, c);
    c[0] = 2, c[1] = 2;
    fcells("clocks", 2, c);
    fprop("clock-names", "uartclk\0apb_pclk", 17);
    f32(2);
    for (i = 0; i < NVIRTIO; i++)
        if (vm->virtio[i]) {
            char name[32];
            sprintf(name, "virtio_mmio@%lx", VIRTIO_BASE + (u64)i * 0x200);
            fnode(name);
            fstring("compatible", "virtio,mmio");
            c[0] = 0, c[1] = (u32)(VIRTIO_BASE + (u64)i * 0x200), c[2] = 0, c[3] = 0x200;
            fcells("reg", 4, c);
            c[0] = 0, c[1] = (u32)(IRQ_VIRTIO + i - 32), c[2] = 4;
            fcells("interrupts", 3, c);
            f32(2);
        }
    f32(2);
    f32(9);
    off_str = off_struct + nfs;
    total = off_str + nfstr;
    memset(out, 0, off_struct);
    memcpy(out + off_struct, sbuf, nfs);
    memcpy(out + off_str, strbuf, nfstr);
    fs = out;
    nfs = 0;
    f32(0xd00dfeed);
    f32(total);
    f32(off_struct);
    f32(off_str);
    f32(40);
    f32(17);
    f32(16);
    f32(0);
    f32(nfstr);
    f32(off_str - off_struct);
    return total;
}
