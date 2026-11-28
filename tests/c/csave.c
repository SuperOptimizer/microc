/* Values alive across calls that are seldom made (x64.c cs_try: they keep caller-saved registers and are saved round
   the call): a table grown when full, a recursion behind a quick answer, a call whose result nothing reads, one with
   a variable number of arguments, one that returns two registers, a float alive beside them, a call in a loop's
   rare arm, setjmp, and arguments alive after the call that passes them on in another order. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>

typedef unsigned long U;
struct Two { U a, b; };
static U *tab;
static int ntab, captab, grown;
static jmp_buf jb;

__attribute__((noinline)) static void grow(void)
{
    captab = captab ? captab * 2 : 4;
    tab = realloc(tab, (size_t)captab * sizeof *tab);
    grown++;
}

__attribute__((noinline)) static U push(U a, U b, U c)
{
    U s = a * 3 + b, t = c ^ a;
    if (ntab == captab)
        grow(); /* seldom */
    tab[ntab++] = s + t * 5 + a + b + c;
    return s ^ t;
}

__attribute__((noinline)) static U deep(U n, U a, U b)
{
    U x = a + b, y = a * 3 - b;
    if (n < 3)
        return x + y + n; /* the usual way */
    return deep(n / 3, y, x) + deep(n - 3, x, y) * 2 + x * y + a;
}

__attribute__((noinline)) static U ignored(U a, U b) { return a * b; }
__attribute__((noinline)) static U sum(int n, ...)
{
    va_list ap;
    U s = 0;
    va_start(ap, n);
    while (n-- > 0)
        s += va_arg(ap, U);
    va_end(ap);
    return s;
}
__attribute__((noinline)) static struct Two two(U a, U b) { struct Two t; t.a = a + b; t.b = a - b; return t; }
__attribute__((noinline)) static U order(U a, U b, U c) { return a - 2 * b + 3 * c; }

__attribute__((noinline)) static U mixed(U a, U b, U c, double p, int rare)
{
    U x = a ^ (b << 3), y = c + a * b;
    double r = p * 2.5 + 1.5;
    struct Two t;
    t.a = t.b = 0;
    if (rare == 1)
        ignored(x, y); /* the result is not read: rax may hold one of ours */
    else if (rare == 2)
        x += sum(3, a, b, c);
    else if (rare == 3)
        t = two(y, x);
    else if (rare == 4)
        y += order(c, b, a);
    return x + 3 * y + t.a + 7 * t.b + (U)(r * 8) + a + b + c;
}

__attribute__((noinline)) static U looped(const U *v, int n)
{
    U s = 0, m = 1, hi = v[0];
    int i;
    for (i = 0; i < n; i++) {
        U x = v[i];
        s += x * m;
        m = m * 3 + 1;
        if (x > hi) hi = x;
        if ((x & 31) == 17)
            s += push(s, m, hi); /* the rare arm */
    }
    return s + m + hi * 5;
}

__attribute__((noinline)) static void thrower(int k) { if (k) longjmp(jb, k); }
__attribute__((noinline)) static U jumps(U a, U b, U c, int k)
{
    volatile U keep = a * b;
    U x = a + c, y = b - c;
    if (setjmp(jb))
        return keep + x * 2 + y * 3; /* x and y were not changed after setjmp */
    thrower(k);
    return keep - x - y;
}

int main(void)
{
    U v[200], s = 0;
    int k;
    for (k = 0; k < 50; k++)
        s = s * 31 + push((U)k, (U)k * 7 + 1, (U)k ^ 21);
    printf("%lu %d %d\n", s, ntab, grown);
    for (k = 0, s = 0; k < 50; k++)
        s += tab[k] * (U)(k + 1);
    printf("%lu\n", s);
    printf("%lu %lu %lu\n", deep(2, 5, 6), deep(30, 1, 2), deep(100, 3, 7));
    for (k = 0, s = 0; k < 20; k++)
        s = s * 7 + mixed((U)k + 1, (U)k * 2 + 3, 5 + (U)k, 0.25 * k, k % 5);
    printf("%lu\n", s);
    for (k = 0; k < 200; k++)
        v[k] = ((U)k * 2654435761u >> 7) % 1000;
    s = looped(v, 200);
    printf("%lu %d\n", s, ntab);
    printf("%lu %lu\n", jumps(3, 4, 5, 0), jumps(3, 4, 5, 2));
    return 0;
}
