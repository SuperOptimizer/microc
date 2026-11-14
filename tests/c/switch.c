/* Switches of every shape: dense ones become jump tables, and must go where the comparisons went. */
#include <stdio.h>

static int dense(int x)
{
    switch (x) {
    case 0: return 10;
    case 1: return 11;
    case 2: return 12;
    case 3: return 13;
    case 4: return 14;
    case 5: return 15;
    case 6: return 16;
    case 7: return 17;
    }
    return -1;
}

static int negative(int x)
{
    int r = 0;
    switch (x) {
    case -3: r += 1;
    case -2: r += 2; break;
    case -1: r = 7; break;
    case 0: r = 9;
    case 1: r += 100; break;
    case 2: break;
    case 4: r = 44; break;
    default: r = -5;
    }
    return r;
}

static long holes(unsigned x, long a, long b)
{
    long r = a;
    switch (x) {
    case 10: r = a + b; break;
    case 11: r = a - b; break;
    case 13: r = a * b; break;
    case 14: r = a ^ b; break;
    case 17: r = a | b; break;
    case 18: r = a & b; break;
    case 19: r = -a; break;
    case 4000000000u: r = 1; break;
    }
    return r + (long)x;
}

static int wide(long x)
{
    switch (x) {
    case 0x100000000L: return 1;
    case 0x100000001L: return 2;
    case 0x100000002L: return 3;
    case 0x100000003L: return 4;
    case 0x100000005L: return 5;
    case 0x100000006L: return 6;
    case -1L: return 7;
    default: return 0;
    }
}

static unsigned char small(unsigned char c)
{
    switch (c) {
    case 'a': case 'e': case 'i': case 'o': case 'u': return 'V';
    case 'b': case 'c': case 'd': case 'f': case 'g': return 'C';
    case '0': case '1': case '2': case '3': return 'D';
    case 250: case 251: case 252: case 253: case 254: case 255: return 'H';
    }
    return '?';
}

static int at_limits(int x)
{
    switch (x) {
    case 2147483647: return 1;
    case 2147483646: return 2;
    case 2147483645: return 3;
    case 2147483644: return 4;
    case 2147483643: return 5;
    case 2147483641: return 6;
    case -2147483647 - 1: return 7;
    }
    return 0;
}

static int low_limits(int x)
{
    switch (x) {
    case -2147483647 - 1: return 1;
    case -2147483647: return 2;
    case -2147483646: return 3;
    case -2147483645: return 4;
    case -2147483643: return 5;
    case -2147483642: return 6;
    }
    return 0;
}

/* a state machine: the switch is in a loop and its arms change what it tests */
static int machine(const char *s)
{
    int state = 0, n = 0;
    for (; *s; s++) {
        switch (state) {
        case 0: state = *s == 'a' ? 1 : 0; break;
        case 1: state = *s == 'b' ? 2 : *s == 'a' ? 1 : 0; break;
        case 2: state = *s == 'c' ? 3 : 0; break;
        case 3: n++; state = *s == 'a' ? 1 : 4; break;
        case 4: state = 5; break;
        case 5: state = 0; n += 10; break;
        default: return -1;
        }
    }
    return n * 10 + state;
}

int main(void)
{
    int i;
    long s = 0;
    for (i = -3; i < 12; i++)
        printf("%d ", dense(i));
    printf("\n");
    for (i = -6; i < 8; i++)
        printf("%d ", negative(i));
    printf("\n");
    for (i = 0; i < 30; i++)
        printf("%ld ", holes((unsigned)i, 12, 5));
    printf("%ld %ld\n", holes(4000000000u, 1, 1), holes(4294967295u, 3, 4));
    for (i = -3; i < 10; i++)
        printf("%d", wide(0x100000000L + i));
    printf(" %d%d%d%d\n", wide(-1), wide(0), wide(1), wide(5));
    for (i = 0; i < 256; i++)
        s = s * 3 + small((unsigned char)i);
    printf("%ld\n", s);
    printf("%d%d%d%d%d%d%d%d%d\n", at_limits(2147483647), at_limits(2147483646), at_limits(2147483645),
           at_limits(2147483644), at_limits(2147483643), at_limits(2147483642), at_limits(2147483641),
           at_limits(-2147483647 - 1), at_limits(0));
    printf("%d%d%d%d%d%d%d%d%d\n", low_limits(-2147483647 - 1), low_limits(-2147483647), low_limits(-2147483646),
           low_limits(-2147483645), low_limits(-2147483644), low_limits(-2147483643), low_limits(-2147483642),
           low_limits(2147483647), low_limits(0));
    printf("%d %d\n", machine("xxabcabcaabcxyzabc"), machine("abcab"));
    return 0;
}
