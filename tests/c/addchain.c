/* Sums p + constant of one p made from each other (opt.c add_chain): an interpreter's pc and sp, a reader that
   steps through bytes, a pointer read again after its sums, 32-bit sums that wrap, sums on both sides of a branch. */
#include <stdio.h>

enum { PUSH, ADD, SUB, DUP, SWAP, LOAD, STORE, JNZ, HALT };

static long run(const int *code, long *vars)
{
    long stack[32], *sp = stack, t;
    const int *pc = code;
    for (;;) {
        switch (*pc++) {
        case PUSH: *sp++ = *pc++; break;
        case ADD: sp--; sp[-1] += sp[0]; break;
        case SUB: sp--; sp[-1] -= sp[0]; break;
        case DUP: sp[0] = sp[-1]; sp++; break;
        case SWAP: t = sp[-1]; sp[-1] = sp[-2]; sp[-2] = t; break;
        case LOAD: *sp++ = vars[*pc++]; break;
        case STORE: vars[*pc++] = *--sp; break;
        case JNZ: t = *pc++; if (*--sp) pc = code + t; break;
        case HALT: return sp[-1];
        }
    }
}

static unsigned long words(const unsigned char *p, int n, const unsigned char **end)
{
    unsigned long s = 0;
    while (n-- > 0) {
        s = s * 31 + (p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24);
        p += 4;
    }
    *end = p;
    return s;
}

static long both(const long *p, int c)
{
    long a = p[1], r;
    if (c)
        r = p[2] + p[3] + (long)(p + 4 - p);
    else
        r = p[5] - p[2];
    return r + a + p[0]; /* p is read after its sums */
}

static unsigned wrap(unsigned x, int c)
{
    unsigned a = x + 0x7ffffff0u, b = x + 0xfffffff0u, d = x + 16;
    return c ? a ^ b : (a + d) ^ (b >> 3);
}

static const long *step(const long *p, long *out)
{
    const long *q = p + 2;
    out[0] = p[3];
    out[1] = p[1];
    if (p[0] & 1)
        q = p + 5;
    return q;
}

int main(void)
{
    static const int code[] = { LOAD, 1, LOAD, 0, ADD, PUSH, 7, SUB, STORE, 1, LOAD, 0, PUSH, 1, SUB, DUP, STORE, 0,
                                DUP, PUSH, 3, SWAP, SUB, STORE, 2, JNZ, 0, LOAD, 1, HALT };
    long vars[3], t[8], o[2];
    unsigned char bytes[64];
    const unsigned char *e;
    const long *q;
    int k;
    vars[0] = 1000;
    vars[1] = 5;
    vars[2] = 0;
    t[0] = run(code, vars);
    printf("%ld %ld\n", t[0], vars[2]);
    for (k = 0; k < 64; k++)
        bytes[k] = (unsigned char)(k * 37 + 11);
    {
        unsigned long w = words(bytes, 16, &e);
        printf("%lu %d\n", w, (int)(e - bytes));
    }
    for (k = 0; k < 8; k++)
        t[k] = k * k + 3;
    printf("%ld %ld\n", both(t, 0), both(t, 1));
    printf("%u %u %u\n", wrap(5, 0), wrap(5, 1), wrap(0xfffffff8u, 0));
    q = step(t, o);
    printf("%d %ld %ld\n", (int)(q - t), o[0], o[1]);
    q = step(t + 1, o);
    printf("%d %ld %ld\n", (int)(q - t), o[0], o[1]);
    return 0;
}
