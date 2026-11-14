/* The C side of the mvm test guest: prints what the instruction batches computed, then tries
   exceptions, the timer and interrupt controller, and EL0 under an MMU. */
typedef unsigned long u64;
typedef unsigned int u32;

extern u64 grp_arith(void), grp_logic(void), grp_mul(void), grp_mem(void), grp_branch(void), grp_fp(void), grp_simd(void);
extern void do_svc(void), do_brk(void), do_udf(void), set_tval(u64), set_ctl(u64), irq_on(void), irq_off(void), do_wfi(void);
extern u64 get_cntfrq(void), tpidr_test(u64, u64), run_user(u64, u64), do_ldtr(u64);
extern void mmu_on(u64 *, u64, u64), do_sttr(u64, u64);
extern char user_code[];

#define R32(a) (*(volatile u32 *)(a))
#define GICD 0x08000000UL
#define GICC 0x08010000UL

static void putch(int c) { *(volatile unsigned char *)0x09000000UL = (unsigned char)c; }

static void print(const char *s)
{
    while (*s)
        putch(*s++);
}

static void hex(u64 v)
{
    int i;
    for (i = 60; i >= 0; i -= 4)
        putch("0123456789abcdef"[v >> i & 15]);
}

static void line(const char *name, u64 v)
{
    print(name);
    putch(' ');
    hex(v);
    putch('\n');
}

static u64 log_esr[16], log_far[16];
static int nlog;
static volatile int ticks;
static u32 last_irq;
static u64 table[512] __attribute__((aligned(4096)));
static u64 user_stack[64];
u64 shared_word = 0x1122334455667788UL;

u64 sync_handler(u64 esr, u64 elr, u64 far, u64 from_el1)
{
    u64 ec = esr >> 26;
    if (nlog < 16) {
        log_esr[nlog] = esr;
        log_far[nlog] = ec == 0x24 || ec == 0x25 ? far : 0;
        nlog++;
    }
    if (ec == 0x15) /* svc: 2 from EL0 means "back to the kernel" */
        return !from_el1 && (esr & 0xffff) == 2 ? 0 : elr;
    return elr + 4; /* step over whatever faulted */
}

void irq_handler(void)
{
    u32 iar = R32(GICC + 0x0c);
    last_irq = iar;
    if (iar == 27) {
        ticks++;
        if (ticks < 3)
            set_tval(get_cntfrq() / 500);
        else
            set_ctl(2);
    } else if (iar == 5)
        ticks += 100;
    R32(GICC + 0x10) = iar;
}

static void show_log(const char *what)
{
    int i;
    print(what);
    putch('\n');
    for (i = 0; i < nlog; i++) {
        print("  esr ");
        hex(log_esr[i]);
        print(" far ");
        /* an address in shared_word is shown as if the word sat at the start of memory, so that what
           is printed does not depend on where the linker put it */
        hex(log_far[i] >= (u64)&shared_word && (log_far[i] & 0x3fffffffUL) == ((u64)&shared_word & 0x3fffffffUL)
                ? log_far[i] - ((u64)&shared_word & 0x3fffffffUL) : log_far[i]);
        putch('\n');
    }
    nlog = 0;
}

static double series(int n)
{
    double s = 0, t = 1;
    int i;
    for (i = 1; i <= n; i++) {
        t = t * 1.0001 + (double)i / 7;
        s += t / (double)(i * i + 1) - (float)i * 0.25f;
    }
    return s;
}

int main(void)
{
    u64 r;
    double d;
    print("mvm guest test\n");
    line("arith", grp_arith());
    line("logic", grp_logic());
    line("mul", grp_mul());
    /* exclusive loads and stores want real memory, which takes an MMU
       four 1 GB blocks: devices; RAM for EL1; the same RAM for EL0; the same RAM read-only for EL0 */
    table[0] = 0x0060000000000405UL;
    table[1] = 0x0040000040000701UL;
    table[2] = 0x0020000040000741UL;
    table[3] = 0x00600000400007c1UL;
    mmu_on(table, 25 | 1 << 8 | 1 << 10 | 3 << 12 | 1 << 23 | 2UL << 32, 0x04ff);
    print("mmu on\n");
    line("mem", grp_mem());
    line("branch", grp_branch());
    line("fp", grp_fp());
    line("simd", grp_simd());
    d = series(1000);
    line("series", *(u64 *)&d);
    line("series-int", (u64)(long)(d * 1000));
    line("sysregs", tpidr_test(0x123456789abcdefUL, 0xfedcba987654321UL));

    do_svc();
    do_brk();
    do_udf();
    show_log("exceptions at EL1");

    R32(GICD) = 1;
    R32(GICC + 4) = 0xff;
    R32(GICC) = 1;
    R32(GICD + 0x100) = 1U << 27 | 1U << 5;
    set_tval(get_cntfrq() / 500);
    set_ctl(1);
    irq_on();
    while (ticks < 3)
        do_wfi();
    line("timer ticks", (u64)ticks);
    line("last irq", last_irq);
    R32(GICD + 0xf00) = 0x02000005; /* a software interrupt to ourselves */
    while (ticks < 100)
        ;
    irq_off();
    line("after sgi", (u64)ticks);
    line("sgi irq", last_irq);

    r = run_user((u64)user_code + 0x40000000UL, (u64)&user_stack[64] + 0x40000000UL);
    line("user returned", r);
    show_log("exceptions from EL0");
    line("ldtr", do_ldtr((u64)&shared_word + 0x40000000UL));
    do_sttr((u64)&shared_word + 0x80000000UL, 1);
    r = do_ldtr((u64)&shared_word);
    show_log("unprivileged access from EL1");
    line("shared", shared_word);
    print("done\n");
    return 0;
}
