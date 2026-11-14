/* t.h: what the tests in tests/ulibs share. Each check prints "ok NAME" or "FAIL NAME ..."; tests/ulibs.sh counts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "expect.h"
static int t_fail;
#define CHECK(name, cond) do { if (cond) printf("ok %s\n", name); else { printf("FAIL %s (%s:%d: %s)\n", name, __FILE__, __LINE__, #cond); t_fail++; } } while (0)
static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long len;
    if (!f) { printf("FAIL cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(len + 1);
    if (fread(p, 1, len, f) != (size_t)len) { printf("FAIL cannot read %s\n", path); exit(1); }
    fclose(f);
    *n = len;
    return p;
}
static void hex(const unsigned char *d, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}
