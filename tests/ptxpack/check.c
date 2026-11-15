/* check.c: linked with one packed unit (mcc -c X.cu -Dmain=unit_main) and mcudahw, and run as  check PLAIN.ptx  (that unit's --ptx-out text). Walks the
   mcuda_modules section as the run time does, unpacks the record with mcuda_pack.h and compares it with the plain text;
   then damages copies of the record (a cut, a flipped token byte, a wrong size) and requires the unpacker to refuse or to
   return a NUL-terminated text, never to read or write out of bounds (run under the glibc allocator's checks). */
#include <mcuda.h>
#include <mcuda_pack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct __mcuda_module *__start_mcuda_modules[] __attribute__((weak));
extern struct __mcuda_module *__stop_mcuda_modules[] __attribute__((weak));
static unsigned long long rd8(const unsigned char *p) { return mcuda_pack_rd8(p); }

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    char *want, *got;
    const char *ptx;
    unsigned long long un, pm, k;
    unsigned char *copy;
    int bad = 0;
    if (argc != 2 || !(f = fopen(argv[1], "rb")))
        return printf("usage: check PLAIN.ptx\n"), 2;
    fseek(f, 0, SEEK_END), n = ftell(f), fseek(f, 0, SEEK_SET);
    want = malloc((size_t)n + 1);
    if (fread(want, 1, (size_t)n, f) != (size_t)n)
        return printf("check: cannot read %s\n", argv[1]), 2;
    want[n] = 0;
    fclose(f);
    if (__stop_mcuda_modules - __start_mcuda_modules != 1)
        return printf("check: %d module records, not 1\n", (int)(__stop_mcuda_modules - __start_mcuda_modules)), 1;
    ptx = __start_mcuda_modules[0]->ptx;
    if (!mcuda_packed(ptx))
        return printf("check: the record is not packed\n"), 1;
    un = rd8((const unsigned char *)ptx + 4), pm = rd8((const unsigned char *)ptx + 12);
    if (un != (unsigned long long)n + 1)
        return printf("check: unpacked size %llu, the text has %ld + 1\n", un, n), 1;
    got = mcuda_unpack(ptx);
    if (!got || strcmp(got, want) != 0)
        return printf("check: the unpacked text differs from --ptx-out\n"), 1;
    free(got);
    /* damaged copies: each must be refused or give some NUL-terminated text of the stated size */
    copy = malloc((size_t)(MCUDA_PACK_HDR + pm));
    for (k = 0; k < 64 && k < pm; k++) {
        unsigned long long at = MCUDA_PACK_HDR + (k * 7919) % pm;
        memcpy(copy, ptx, (size_t)(MCUDA_PACK_HDR + pm));
        copy[at] ^= (unsigned char)(1 + k % 255);
        got = mcuda_unpack((const char *)copy);
        if (got && strlen(got) + 1 > un)
            bad++;
        free(got);
    }
    memcpy(copy, ptx, (size_t)(MCUDA_PACK_HDR + pm));
    copy[12] = (unsigned char)(pm / 2), copy[13] = (unsigned char)(pm / 512), copy[14] = 0, copy[15] = 0; /* cut short */
    memset(copy + 16, 0, 4);
    if ((got = mcuda_unpack((const char *)copy)) != 0)
        bad++, free(got);
    memcpy(copy, ptx, (size_t)(MCUDA_PACK_HDR + pm));
    copy[4] ^= 1; /* a wrong unpacked size */
    if ((got = mcuda_unpack((const char *)copy)) != 0)
        bad++, free(got);
    free(copy);
    if (bad)
        return printf("check: %d damaged records not refused\n", bad), 1;
    printf("ok %ld bytes -> %llu (%.2fx)\n", n, pm + MCUDA_PACK_HDR, (double)n / (double)(pm + MCUDA_PACK_HDR));
    return 0;
}
