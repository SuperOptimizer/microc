/* mvm: a small aarch64 virtual machine, enough to boot Linux with a serial console.
   One machine model (RAM, GICv2, PL011, generic timers, PSCI) and a CPU that interprets A64. */
#ifndef VM_H
#define VM_H
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long u64; /* LP64 hosts only */
typedef long s64;

#define RAM_BASE 0x40000000UL
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL
#define UART_BASE 0x09000000UL
#define IRQ_VTIMER 27
#define IRQ_PTIMER 30
#define IRQ_UART 33
#define NIRQ 64
#define VIRTIO_BASE 0x0a000000UL /* slots of 0x200 bytes; slot n interrupts on line IRQ_VIRTIO + n */
#define IRQ_VIRTIO 48
#define NVIRTIO 8
#define TIMER_HZ 100000000UL /* one tick per interpreted instruction */

#define MAXCPU 8

struct Cpu;
struct Virtio;

typedef struct Vm {
    u8 *ram;
    u64 ram_size;
    int ncpu;
    /* interrupt controller: lines 0..31 are private to each CPU, the rest are shared (kept in row 0) */
    u32 gicd_ctlr, gicc_ctlr[MAXCPU], gicc_pmr[MAXCPU], gicc_bpr[MAXCPU];
    u8 irq_enabled[MAXCPU][NIRQ], irq_latch[MAXCPU][NIRQ], irq_level[MAXCPU][NIRQ], irq_active[MAXCPU][NIRQ];
    u8 irq_prio[MAXCPU][NIRQ], irq_edge[MAXCPU][NIRQ], irq_target[NIRQ];
    int irq_out[MAXCPU];                /* the line into each CPU */
    void (*kick)(struct Vm *, int cpu); /* told when a line rises, if the CPUs run on their own */
    /* CPUs as the firmware sees them: 0 off, 1 running, 2 asked to start at start_pc with start_x0 */
    int cpu_state[MAXCPU];
    u64 start_pc[MAXCPU], start_x0[MAXCPU];
    struct Cpu *cpus; /* the interpreter's */
    /* serial port */
    u32 uart_ris, uart_imsc, uart_cr, uart_lcrh, uart_ibrd, uart_fbrd, uart_ifls;
    u8 uart_fifo[256];
    int uart_rd, uart_cnt;
    char out[4096];
    int nout;
    int stdin_open, raw_tty, esc;
    int poweroff, status, debug;
    struct Virtio *virtio[NVIRTIO];
    int net; /* there is a network card (net.c) */
    u32 scr_w, scr_h; /* the screen (gpu.c): pixels are 0x00RRGGBB */
    u32 *scr;
    int scr_dirty, gui; /* gui: a window or a script may send input at any time */
    struct Virtio *vcon; /* the virtio console, which then gets the terminal's input */
} Vm;

/* A virtio device behind the MMIO transport (virtio.c). */
typedef struct VQ {
    u32 num, ready;
    u64 desc, avail, used; /* guest addresses of the three parts of the queue */
    u16 last;              /* the next entry of the avail ring to take */
} VQ;

typedef struct Virtio {
    int slot;
    u32 dev_id;
    u64 features, drv_features;
    u32 fsel, dsel, qsel, status, isr;
    VQ q[3];
    int nq;
    u8 config[256];
    void (*cfg_write)(Vm *, struct Virtio *); /* the guest wrote into config */
    void (*notify)(Vm *, struct Virtio *, int q); /* the guest put buffers on queue q */
    void *priv;
} Virtio;

typedef struct Iov { /* one piece of a request, in host memory */
    u8 *p;
    u32 len;
    int w; /* the device writes it */
} Iov;

typedef union V128 {
    u64 d[2];
    u32 w[4];
    u16 h[8];
    u8 b[16];
} V128;

#define NTLB 512
typedef struct Tlb {
    u64 tag; /* virtual page number; all ones when empty */
    u8 *host;
} Tlb;

typedef struct Cpu {
    int id, halted;
    u64 slice_end;
    u64 x[32]; /* x[31] reads as zero; the stack pointer lives in sp */
    u64 pc, sp;
    u32 nzcv, daif; /* N=8 Z=4 C=2 V=1; D=8 A=4 I=2 F=1 */
    int el, spsel;
    u64 sp_bank[2];
    V128 v[32];
    u32 fpcr, fpsr;
    u64 icount, next_event, next_poll, toff;
    u64 sctlr, ttbr0, ttbr1, tcr, mair, vbar, esr, far, elr, spsr, tpidr_el0, tpidr_el1, tpidrro, contextidr;
    u64 cpacr, par, cntkctl, afsr0, afsr1, amair, mdscr, csselr, actlr;
    u64 cntv_ctl, cntv_cval, cntp_ctl, cntp_cval;
    int excl_valid;
    u64 excl_addr;
    u64 mmio_pa;
    int debug;
    Vm *vm;
    jmp_buf jb;
    Tlb tlb[2][3][NTLB];
} Cpu;

/* dev.c */
u64 vm_mmio_read(Vm *vm, int cpu, u64 pa, int size);
void vm_mmio_write(Vm *vm, int cpu, u64 pa, int size, u64 val);
void gic_set_level(Vm *vm, int cpu, int irq, int level);
void uart_flush(Vm *vm);
void uart_input(Vm *vm);         /* take what the terminal has, without waiting */
int vm_wait(Vm *vm, long usec);  /* sleep until the terminal or the network has something, or for usec */
int vm_can_wake(Vm *vm);         /* is there anything outside that could still wake an idle guest? */
void term_raw(Vm *vm);
void term_restore(Vm *vm);
u64 psci_call(Vm *vm, int cpu, u64 fn, u64 a1, u64 a2, u64 a3);
u32 fdt_build(Vm *vm, u8 *out, const char *cmdline, u64 initrd, u64 initrd_size);
u64 now_usec(void);
/* virtio.c */
Virtio *virtio_add(Vm *vm, u32 dev_id, u64 features, int nq, void (*notify)(Vm *, Virtio *, int));
int vq_pop(Vm *vm, Virtio *d, int q, Iov *iov, int max, u16 *head); /* number of pieces; 0 when the queue is empty */
void vq_push(Vm *vm, Virtio *d, int q, u16 head, u32 written);
u64 virtio_read(Vm *vm, Virtio *d, u32 off);
void virtio_write(Vm *vm, Virtio *d, u32 off, u32 v);
int blk_add(Vm *vm, const char *path, int readonly);
int rng_add(Vm *vm);
void vcon_add(Vm *vm);
int vcon_room(Vm *vm);
void vcon_input(Vm *vm, const u8 *buf, int n);
void con_out(Vm *vm, int c); /* dev.c: a character for the terminal */
/* gpu.c: the screen, the keyboard and the pointer */
void gpu_add(Vm *vm, u32 w, u32 h);
void gpu_poll(Vm *vm);
int gpu_play(Vm *vm, const char *file); /* a script of input events and screenshots */
int screenshot(Vm *vm, const char *file);
void input_key(Vm *vm, int code, int down);          /* Linux key codes */
void input_pointer(Vm *vm, int x, int y);            /* 0..32767 across the screen */
void input_button(Vm *vm, int code, int down);
void input_wheel(Vm *vm, int steps);
/* gui.c */
int gui_open(Vm *vm);
void gui_poll(Vm *vm);
/* p9.c */
int p9_add(Vm *vm, const char *dir, int readonly);
/* net.c */
void net_add(Vm *vm);
int net_forward(Vm *vm, u32 hport, u32 gport); /* connections to hport of this machine go to gport of the guest */
void net_poll(Vm *vm);
int net_active(Vm *vm);
/* cpu.c */
void cpu_reset(Cpu *c, Vm *vm, int id, u64 pc, u64 x0);
void emu_run(Vm *vm);
/* hvf.c */
int hvf_run(Vm *vm, int debug);
/* fp.c */
int fp_exec(Cpu *c, u32 insn);
#endif
