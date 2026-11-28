/* Switches that become jumps through a table (x64.c jt_plan): dense, with holes, from a negative case, on unsigned
   and 64-bit values, char and enum, with cases that fall through, a default in the middle of the range, values far
   outside the table on both sides, a switch round a loop back to itself, two switches on one value, a value kept in
   memory (many live values), and an if-chain of the same shape. Every one is tried over a range wider than its cases. */
#include <stdio.h>

enum Op { A, B, C, D, E, F, G, H, I, J };

__attribute__((noinline)) static int dense(int x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    case 6: return 77;
    case 7: return 88;
    case 8: return 99;
    }
    return -1;
}

__attribute__((noinline)) static int holes(int x)
{
    int r = 1;
    switch (x) {
    case -3: r += 5; /* falls through */
    case -2: r *= 3; break;
    case 0: r = 100; break;
    case 1: case 2: r = 7; break;
    case 5: r = x * 9; /* falls through */
    case 6: r -= 2; break;
    case 9: return 900;
    case 11: r = 1100; break;
    default: r = -r - x; break;
    case 13: r = 13;
    }
    return r + 1;
}

__attribute__((noinline)) static unsigned uns(unsigned x)
{
    switch (x) {
    case 10: return 1;
    case 11: return 2;
    case 12: return 3;
    case 13: return 5;
    case 14: return 8;
    case 15: return 13;
    case 16: return 21;
    case 17: return 34;
    }
    return x + 1000;
}

__attribute__((noinline)) static long wide(long x)
{
    switch (x) {
    case 100: return 1;
    case 101: return 2;
    case 102: return 4;
    case 103: return 8;
    case 104: return 16;
    case 105: return 32;
    case 107: return 64;
    case 108: return 128;
    }
    return x >> 3;
}

__attribute__((noinline)) static int chr(char c)
{
    switch (c) {
    case 'a': return 1;
    case 'b': return 2;
    case 'c': return 3;
    case 'd': return 4;
    case 'e': return 5;
    case 'f': return 6;
    case 'g': return 7;
    case 'h': return 8;
    case 'i': return 9;
    }
    return 0;
}

__attribute__((noinline)) static long machine(const unsigned char *p, int n)
{
    long acc = 0, t = 1;
    int i = 0, st = A;
    while (i < n) {
        switch (st) { /* comes round to itself from every case */
        case A: acc += p[i]; st = p[i] & 1 ? B : C; break;
        case B: acc ^= t; t += 3; st = D; break;
        case C: acc -= p[i] >> 1; st = p[i] & 2 ? E : F; break;
        case D: t *= 3; st = G; break;
        case E: acc += t; st = H; break;
        case F: acc *= 3; st = p[i] & 4 ? I : A; i++; break;
        case G: acc += i; st = J; break;
        case H: t ^= acc; st = A; i++; break;
        case I: acc -= 7; st = A; break;
        case J: st = p[i] & 8 ? A : C; i++; break;
        }
    }
    return acc + t;
}

__attribute__((noinline)) static int twice(int x, int y)
{
    int r = 0;
    switch (x) {
    case 1: r = 10; break;
    case 2: r = 20; break;
    case 3: r = 30; break;
    case 4: r = 40; break;
    case 5: r = 50; break;
    case 6: r = 60; break;
    }
    switch (x) {
    case 2: r += y; break;
    case 3: r -= y; break;
    case 4: r *= y; break;
    case 5: r ^= y; break;
    case 6: r |= y; break;
    case 7: r = y; break;
    }
    return r;
}

__attribute__((noinline)) static void nothing(void) { }

__attribute__((noinline)) static int sides(int x)
{
    if (x < 10) return -1; /* under and over the table go different ways */
    if (x > 17) return -2;
    if (x == 10) return 3;
    if (x == 11) return 1;
    if (x == 12) return 4;
    if (x == 13) return 1;
    if (x == 14) return 5;
    if (x == 15) return 9;
    if (x == 16) return 2;
    return 6;
}

__attribute__((noinline)) static long crowded(const long *v, int x)
{
    long a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5], g = v[6], h = v[7], i = v[8], j = v[9], k = v[10], l = v[11],
         m = v[12], n = v[13];
    nothing(); /* (a call: everything lives across it) */
    switch (x) {
    case 20: a += b; break;
    case 21: b += c; break;
    case 22: c += d; break;
    case 23: d += e; break;
    case 24: e += f; break;
    case 25: f += g; break;
    case 26: g += h; break;
    case 27: h += i; break;
    case 28: i += j; break;
    case 29: j += k; break;
    case 30: k += l; break;
    case 31: l += m; break;
    case 32: m += n; break;
    default: n += a;
    }
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k + 12 * l + 13 * m + 14 * n + x;
}

__attribute__((noinline)) static int chain(int x)
{
    if (x == 3) return 1;
    if (x == 4) return 2;
    if (x == 5) return 3;
    if (x == 6) return 5;
    if (x == 7) return 8;
    if (x == 8) return 13;
    if (x == 9) return 21;
    return x < 3 ? -1 : -2;
}

int main(void)
{
    static const int far[] = { -2147483647 - 1, -2147483647, -65536, -1000, 1000, 65536, 2147483646, 2147483647 };
    unsigned char buf[64];
    long v[14], s;
    unsigned u, us = 0;
    int x, k;
    for (x = -20, s = 0; x < 40; x++)
        s = s * 7 + dense(x) + 3 * holes(x) + 5 * chain(x) + 11 * twice(x, x + 9) + 13 * chr((char)(x + 'a' - 5)) + 17 * sides(x);
    for (k = 0; k < 8; k++)
        s = s * 7 + dense(far[k]) + 3 * holes(far[k]) + 5 * chain(far[k]) + twice(far[k], 3) + sides(far[k]);
    printf("%ld\n", s);
    for (u = 0; u < 30; u++)
        us = us * 31 + uns(u);
    us = us * 31 + uns(0xffffffffu) + uns(0x80000000u) + uns(0x8000000bu) + uns(0xfffffff6u);
    printf("%u\n", us);
    for (x = 90, s = 0; x < 120; x++)
        s = s * 5 + wide(x);
    s += wide(0x100000064L) + wide(-0xffffff9cL) + wide(0x7fffffffffffff00L) + wide(-0x7fffffffffffff00L);
    printf("%ld\n", s);
    for (k = 0; k < 64; k++)
        buf[k] = (unsigned char)(k * 37 + 5);
    printf("%ld\n", machine(buf, 64));
    for (k = 0; k < 14; k++)
        v[k] = k * k + 1;
    for (x = 15, s = 0; x < 38; x++)
        s = s * 3 + crowded(v, x);
    printf("%ld\n", s);
    return 0;
}
