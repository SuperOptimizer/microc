/* mvm: boot an arm64 Linux Image.
   usage: mvm [-m megabytes] [-smp cpus] [-accel hvf|emu] -kernel Image [-initrd file] [-append "command line"]
              [-disk image]... [-net] [-fwd hostport:guestport]... [-share dir | -share-ro dir] [-rng] [-vcon]
              [-gpu] [-res WxH] [-headless] [-screenshot file] [-play script] [-d]
   The guest runs on the real CPU when the host lets it (hvf) and is interpreted otherwise (emu). */
#include "vm.h"
#include <sys/mman.h>

static Vm vm;
static Cpu cpus[MAXCPU];

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "mvm: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(2);
}

static u64 load(const char *path, u64 off)
{
    FILE *f = fopen(path, "rb");
    u64 n = 0;
    size_t k;
    if (!f)
        die("cannot open", path);
    /* in pieces: the system refuses to read 2 GB or more at once */
    while (off + n < vm.ram_size &&
           (k = fread(vm.ram + off + n, 1, vm.ram_size - off - n > 0x1000000 ? 0x1000000 : (size_t)(vm.ram_size - off - n), f)) > 0)
        n += k;
    if (off + n >= vm.ram_size)
        die("does not fit in memory", path);
    fclose(f);
    return n;
}

static u64 le64(const u8 *p)
{
    u64 v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

int main(int argc, char **argv)
{
    const char *kernel = 0, *initrd = 0, *append = "", *disks[4];
    const char *share = 0, *shot = 0, *playfile = 0;
    unsigned gw = 0, gh = 0;
    int headless = 0;
    int share_ro = 0;
    int ndisks = 0, rng = 0, vcon = 0, net = 0, nfwd = 0;
    unsigned fwd[8][2];
    u64 mb = 256, text_off, ksize, isize = 0, ioff = 0, doff, t0;
    int i, debug = 0, accel = 0, ncpu = 1; /* 0 whichever works, 1 hvf, 2 emu */
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-kernel") && i + 1 < argc)
            kernel = argv[++i];
        else if (!strcmp(a, "-initrd") && i + 1 < argc)
            initrd = argv[++i];
        else if (!strcmp(a, "-append") && i + 1 < argc)
            append = argv[++i];
        else if (!strcmp(a, "-m") && i + 1 < argc)
            mb = (u64)atol(argv[++i]);
        else if (!strcmp(a, "-accel") && i + 1 < argc) {
            a = argv[++i];
            accel = !strcmp(a, "hvf") ? 1 : !strcmp(a, "emu") || !strcmp(a, "tcg") ? 2 : -1;
            if (accel < 0)
                die("unknown accelerator", a);
        } else if (!strcmp(a, "-d"))
            debug++;
        else if (!strcmp(a, "-disk") && i + 1 < argc && ndisks < 4)
            disks[ndisks++] = argv[++i];
        else if (!strcmp(a, "-net"))
            net = 1;
        else if (!strcmp(a, "-fwd") && i + 1 < argc && nfwd < 8 && sscanf(argv[i + 1], "%u:%u", &fwd[nfwd][0], &fwd[nfwd][1]) == 2)
            net = 1, nfwd++, i++;
        else if ((!strcmp(a, "-share") || !strcmp(a, "-share-ro")) && i + 1 < argc)
            share_ro = a[6] != 0, share = argv[++i];
        else if (!strcmp(a, "-gpu"))
            gw = 1024, gh = 768;
        else if (!strcmp(a, "-res") && i + 1 < argc && sscanf(argv[i + 1], "%ux%u", &gw, &gh) == 2 && gw >= 64 && gh >= 64 && gw <= 4096 && gh <= 4096)
            i++;
        else if (!strcmp(a, "-headless"))
            headless = 1;
        else if (!strcmp(a, "-screenshot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(a, "-play") && i + 1 < argc)
            playfile = argv[++i];
        else if (!strcmp(a, "-rng"))
            rng = 1;
        else if (!strcmp(a, "-vcon"))
            vcon = 1;
        else if (!strcmp(a, "-smp") && i + 1 < argc)
            ncpu = atoi(argv[++i]);
        else if ((!strcmp(a, "-M") || !strcmp(a, "-cpu")) && i + 1 < argc)
            i++; /* accepted for people used to qemu */
        else if (!strcmp(a, "-nographic") || !strcmp(a, "-no-reboot"))
            ;
        else
            die("unknown option", a);
    }
    if (!kernel)
        die("usage: mvm [-m megabytes] [-smp cpus] [-accel hvf|emu] -kernel Image [-initrd file] [-append cmdline]"
            " [-disk image]... [-net] [-fwd hostport:guestport]... [-share dir | -share-ro dir] [-rng] [-vcon]"
            " [-gpu] [-res WxH] [-headless] [-screenshot file] [-play script] [-d]", 0);
    if (ncpu < 1 || ncpu > MAXCPU)
        die("-smp takes 1..8", 0);
    if (mb < 32 || mb > 16384)
        die("memory size must be 32..16384 megabytes", 0);
    vm.ram_size = mb << 20;
    vm.ram = (u8 *)mmap(0, (size_t)vm.ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (vm.ram == (u8 *)MAP_FAILED)
        die("out of memory", 0);
    vm.stdin_open = 1;
    vm.ncpu = ncpu;
    vm.debug = debug;
    vm.cpus = cpus;
    memset(vm.irq_target, 1, sizeof vm.irq_target);
    ksize = load(kernel, 0);
    if (ksize < 64 || memcmp(vm.ram + 56, "ARM\x64", 4) != 0)
        die("not an arm64 Linux Image", kernel);
    text_off = le64(vm.ram + 16) ? le64(vm.ram + 8) : 0x80000;
    if (text_off) {
        memset(vm.ram, 0, (size_t)ksize);
        load(kernel, text_off);
    }
    for (i = 0; i < ndisks; i++)
        if (blk_add(&vm, disks[i], 0) < 0)
            die("cannot open", disks[i]);
    if (rng && rng_add(&vm) < 0)
        die("cannot open", "/dev/urandom");
    if (vcon)
        vcon_add(&vm);
    if (share && p9_add(&vm, share, share_ro) < 0)
        die("not a directory:", share);
    if (gw)
        gpu_add(&vm, gw, gh);
    if (gw && !headless && gui_open(&vm) < 0)
        fprintf(stderr, "mvm: no window for the screen\n");
    if (playfile && gpu_play(&vm, playfile) < 0)
        die("cannot open", playfile);
    if (net)
        net_add(&vm);
    for (i = 0; i < nfwd; i++)
        if (net_forward(&vm, fwd[i][0], fwd[i][1]) < 0)
            die("cannot listen on the port given to", "-fwd");
    doff = vm.ram_size - 0x100000;
    if (initrd) {
        ioff = vm.ram_size / 2;
        isize = load(initrd, ioff);
        if (ioff + isize > doff)
            die("does not fit in memory", initrd);
    }
    fdt_build(&vm, vm.ram + doff, append, RAM_BASE + ioff, isize);
    vm.cpu_state[0] = 2;
    vm.start_pc[0] = RAM_BASE + text_off;
    vm.start_x0[0] = RAM_BASE + doff;
    term_raw(&vm);
    t0 = now_usec();
    if (accel == 2 || hvf_run(&vm, debug) < 0) {
        if (accel == 1) {
            term_restore(&vm);
            die("the hypervisor is not available", 0);
        }
        emu_run(&vm);
        accel = 2;
    }
    term_restore(&vm);
    if (shot && screenshot(&vm, shot) < 0)
        fprintf(stderr, "mvm: no picture of the screen written to %s\n", shot);
    if (debug && accel == 2) {
        u64 us = now_usec() - t0, n = 0;
        for (i = 0; i < ncpu; i++)
            n += cpus[i].icount;
        fprintf(stderr, "mvm: %lu instructions in %lu.%03lu s (%lu MIPS)\n", n, us / 1000000, us / 1000 % 1000,
                us ? n / us : 0);
    }
    return vm.status;
}
