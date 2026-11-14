/* gpuptx FILE.ptx: what ptxt.c does with a PTX test module, on a real GPU through NVIDIA's driver interface
   (cuda.h; built with gcc on the machine that has the GPU, see gpuptx.sh): load the module, run the kernels
   its "//! run" lines name with the same arguments (the address of NOUT zeroed words, the address of 1024
   words of input, NOUT), and print the result and the output words in the same form, so that the output can
   be compared with FILE.exp. What cannot be the same: the text of an error (the driver's name of it is
   printed instead), and a kernel that our engine diagnoses and stops may hang on a device: a run is given
   SECS seconds (default 120, which is mostly the assembler's). The runs are made in one context, one after the
   other with the module as the one before left it, as ptxt.c makes them; but in a process apart, and after
   a fault, which takes the context with it, or a run that does not end, a new one starts with the next run
   and the module as new. A line "//! with OTHER.ptx" links that module in. */
#include <cuda.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char *slurp(const char *name, long *len)
{
    FILE *f = fopen(name, "rb");
    long n;
    char *p;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    p = malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n)
        return 0;
    p[n] = 0;
    fclose(f);
    if (len)
        *len = n;
    return p;
}

static char log_[1 << 16];

static void say_log(void)
{
    char *e;
    for (e = strtok(log_, "\n"); e; e = strtok(0, "\n"))
        printf("    %.300s\n", e);
}

/* the module, alone or linked with the ones it names */
static int load(CUmodule *m, char *text, long len, const char *path, const char *base)
{
    CUjit_option opt[2] = {CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
    void *val[2];
    CUresult r;
    char *s;
    val[0] = log_;
    val[1] = (void *)(size_t)sizeof log_;
    log_[0] = 0;
    if (!strstr(text, "//! with ")) {
        r = cuModuleLoadDataEx(m, text, 2, opt, val);
        if (r) {
            const char *nm = 0;
            cuGetErrorName(r, &nm);
            printf("load: %s\n", nm ? nm : "?");
            say_log();
            return -1;
        }
        return 0;
    } else {
        CUlinkState ls;
        void *img;
        size_t imglen;
        if ((r = cuLinkCreate(2, opt, val, &ls)) != 0) {
            printf("load: cuLinkCreate %d\n", (int)r);
            return -1;
        }
        for (s = text; (s = strstr(s, "//! with ")) != 0;) {
            char other[400], *q, *t2;
            long l2, dl = base - path;
            s += 9;
            memcpy(other, path, (size_t)dl);
            for (q = other + dl; *s && *s != '\n' && *s != ' ' && q < other + sizeof other - 1;)
                *q++ = *s++;
            *q = 0;
            t2 = slurp(other, &l2);
            if (!t2) {
                printf("cannot open %s\n", other + dl);
                return -1;
            }
            if ((r = cuLinkAddData(ls, CU_JIT_INPUT_PTX, t2, (size_t)l2 + 1, other + dl, 0, 0, 0)) != 0)
                break;
        }
        if (!r)
            r = cuLinkAddData(ls, CU_JIT_INPUT_PTX, text, (size_t)len + 1, base, 0, 0, 0);
        if (!r)
            r = cuLinkComplete(ls, &img, &imglen);
        if (!r)
            r = cuModuleLoadData(m, img);
        if (r) {
            const char *nm = 0;
            cuGetErrorName(r, &nm);
            printf("load: %s\n", nm ? nm : "?");
            say_log();
            return -1;
        }
        return 0;
    }
}

/* The runs from text + at on, in a context of our own; after each, where the next starts goes down the pipe.
   Ends after a run that failed (the context is no good any more) or after the last. */
static void runs(char *text, long len, const char *path, const char *base, long at, int secs, int fd)
{
    CUdevice dev;
    CUcontext ctx;
    CUmodule m;
    char *s = at < 0 ? 0 : text + at;
    long next;
    alarm((unsigned)secs);
    if (cuInit(0) || cuDeviceGet(&dev, 0) || cuCtxCreate(&ctx, 0, 0, dev)) {
        printf("no device\n");
        fflush(stdout);
        _exit(2);
    }
    /* recursion and alloca: the driver cannot size the stack of a kernel that calls itself */
    cuCtxSetLimit(CU_LIMIT_STACK_SIZE, 16384);
    if (load(&m, text, len, path, base) < 0) {
        fflush(stdout);
        _exit(3);
    }
    while (s) {
        char name[200];
        unsigned grid[3], block[3], shared = 0, nout = 0, i;
        CUfunction fn;
        CUdeviceptr dout, din;
        CUresult r;
        unsigned *out, *in;
        void *args[3];
        s += 8;
        if (sscanf(s, "%199s %u %u %u %u %u %u %u %u", name, &grid[0], &grid[1], &grid[2], &block[0], &block[1], &block[2], &shared,
                   &nout) != 9) {
            printf("bad run line\n");
            fflush(stdout);
            _exit(2);
        }
        s = strstr(s, "//! run ");
        next = s ? s - text : -1;
        alarm((unsigned)secs);
        if (cuModuleGetFunction(&fn, m, name)) {
            printf("%s: no such kernel\n", name);
            fflush(stdout);
            if (write(fd, &next, sizeof next) < 0)
                _exit(2);
            continue;
        }
        out = calloc(nout + 1, sizeof *out);
        in = malloc(1024 * sizeof *in);
        for (i = 0; i < 1024; i++)
            in[i] = i * 0x9E3779B1U;
        if (cuMemAlloc(&dout, (size_t)(nout + 1) * 4) || cuMemAlloc(&din, 4096) || cuMemcpyHtoD(dout, out, (size_t)(nout + 1) * 4) ||
            cuMemcpyHtoD(din, in, 4096)) {
            printf("%s: no memory on the device\n", name);
            fflush(stdout);
            _exit(2);
        }
        args[0] = &dout;
        args[1] = &din;
        args[2] = &nout;
        r = cuLaunchKernel(fn, grid[0], grid[1], grid[2], block[0], block[1], block[2], shared, 0, args, 0);
        if (!r)
            r = cuCtxSynchronize();
        printf("%s: %d\n", name, (int)r);
        if (r) {
            const char *nm = 0;
            cuGetErrorName(r, &nm);
            printf("%s\n", nm ? nm : "?");
        } else if (cuMemcpyDtoH(out, dout, (size_t)(nout + 1) * 4))
            printf("cannot read the output\n");
        for (i = 0; i < nout; i++)
            printf("%08x%c", out[i], i % 8 == 7 || i == nout - 1 ? '\n' : ' ');
        fflush(stdout);
        if (write(fd, &next, sizeof next) < 0)
            _exit(2);
        if (r)
            _exit(0);
        cuMemFree(dout);
        cuMemFree(din);
        free(out);
        free(in);
    }
    _exit(0);
}

int main(int argc, char **argv)
{
    char *text, *s, *base;
    long len, at, next;
    int secs = getenv("SECS") ? atoi(getenv("SECS")) : 120;
    if (argc < 2 || !(text = slurp(argv[1], &len))) {
        fprintf(stderr, "usage: gpuptx FILE.ptx\n");
        return 2;
    }
    base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    s = strstr(text, "//! run ");
    at = s ? s - text : -1;
    for (;;) {
        int fd[2], st = 0;
        pid_t pid;
        long done = at;
        fflush(stdout);
        if (pipe(fd))
            return 2;
        pid = fork();
        if (pid == 0) {
            close(fd[0]);
            runs(text, len, argv[1], base, at, secs, fd[1]);
        }
        close(fd[1]);
        while (read(fd[0], &next, sizeof next) == (long)sizeof next)
            done = next;
        close(fd[0]);
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st)) {
            /* the run at done did not end (or the load, if there is none) */
            char name[200];
            name[0] = 0;
            if (done >= 0)
                sscanf(text + done + 8, "%199s", name);
            printf("%s: no answer in %d seconds (signal %d)\n", name[0] ? name : "load", secs, WTERMSIG(st));
            if (done < 0)
                return 0;
            s = strstr(text + done + 8, "//! run ");
            done = s ? s - text : -1;
        } else if (WEXITSTATUS(st) == 3)
            return 0; /* the module does not load: said */
        else if (WEXITSTATUS(st) == 2)
            return 1;
        if (done < 0 || at < 0)
            return 0;
        at = done;
    }
}
