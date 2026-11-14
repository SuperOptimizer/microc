/* ptxt FILE.ptx: load a PTX module and run the kernels its "//! run" lines name, through the engine's own
   interface (cuda/mc.h). A line reads

       //! run KERNEL GX GY GZ BX BY BZ SHARED NOUT

   A kernel takes up to three parameters: the address of NOUT zeroed 32-bit words, the address of 1024 words
   of input (word i holds i * 0x9E3779B1), and NOUT. What is printed: the result of the launch, the error text
   if it failed, and the output words in hexadecimal. A module that does not load prints the error only.
   A line "//! with OTHER.ptx" loads that module (of the same directory) before this one.
   ptxt -t FILE.ptx also says on stderr how many PTX instructions each launch ran and how fast. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mc.h"

static unsigned in[1024];

int main(int argc, char **argv)
{
    FILE *f;
    char *text, *s, *base;
    long n;
    McModule *m;
    int timing = 0;
    if (argc > 2 && strcmp(argv[1], "-t") == 0) {
        timing = 1;
        argv++;
        argc--;
    }
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "usage: ptxt FILE.ptx\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n)
        return 2;
    text[n] = 0;
    fclose(f);
    base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    for (n = 0; n < 1024; n++)
        in[n] = (unsigned)n * 0x9E3779B1U;
    /* "//! hw": the module's expectations are a GPU's, for mma too */
    if (strstr(text, "//! hw\n"))
        mc_mma_hw(1);
    /* "//! with OTHER.ptx": a module of the same directory to load first, for what this one declares .extern */
    for (s = text; (s = strstr(s, "//! with ")) != 0;) {
        char other[400], *q, *t2;
        long len = base - argv[1];
        FILE *g;
        s += 9;
        memcpy(other, argv[1], (size_t)len);
        for (q = other + len; *s && *s != '\n' && *s != ' ' && q < other + sizeof other - 1;)
            *q++ = *s++;
        *q = 0;
        if (!(g = fopen(other, "rb"))) {
            printf("cannot open %s\n", other + len);
            return 1;
        }
        fseek(g, 0, SEEK_END);
        n = ftell(g);
        fseek(g, 0, SEEK_SET);
        t2 = (char *)malloc((size_t)n + 1);
        if (fread(t2, 1, (size_t)n, g) != (size_t)n)
            return 2;
        t2[n] = 0;
        fclose(g);
        if (!mc_module_load(t2, other + len)) {
            printf("load: %s\n", mc_error());
            return 0;
        }
    }
    m = mc_module_load(text, base);
    if (!m) {
        printf("load: %s\n", mc_error());
        return 0;
    }
    for (s = text; (s = strstr(s, "//! run ")) != 0;) {
        char name[200];
        unsigned grid[3], block[3], shared, nout, i;
        unsigned *out;
        void *pout, *pin, *args[3];
        McFunc *k;
        int rc;
        s += 8;
        if (sscanf(s, "%199s %u %u %u %u %u %u %u %u", name, &grid[0], &grid[1], &grid[2], &block[0], &block[1], &block[2],
                   &shared, &nout) != 9) {
            printf("bad run line\n");
            return 1;
        }
        k = mc_module_func(m, name);
        if (!k) {
            printf("%s: no such kernel\n", name);
            continue;
        }
        out = (unsigned *)calloc(nout + 1, sizeof *out);
        pout = out;
        pin = in;
        args[0] = &pout;
        args[1] = &pin;
        args[2] = &nout;
        {
            unsigned long n0 = mc_instructions();
            clock_t c0 = clock();
            double sec;
            rc = mc_launch(k, grid, block, (unsigned long)shared, args);
            sec = (double)(clock() - c0) / CLOCKS_PER_SEC;
            if (timing)
                fprintf(stderr, "%s: %lu instructions in %.3f s, %.1f million a second\n", name, mc_instructions() - n0, sec,
                        sec > 0 ? (double)(mc_instructions() - n0) / sec / 1e6 : 0.0);
        }
        printf("%s: %d\n", name, rc);
        if (rc)
            printf("%s\n", mc_error());
        for (i = 0; i < nout; i++)
            printf("%08x%c", out[i], i % 8 == 7 || i == nout - 1 ? '\n' : ' ');
        free(out);
    }
    mc_module_free(m);
    return 0;
}
