/* x86-64 only: inline assembly (constraints, modifiers, register variables, clobbers) */
#include <stdio.h>
#include <string.h>
static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}
static unsigned long rdtsc(void)
{
    unsigned lo, hi;
    __asm__("rdtsc" : "=a"(lo), "=d"(hi));
    return (unsigned long)hi << 32 | lo;
}
static int add3(int a, int b, int c)
{
    int r;
    __asm__("leal (%1,%2), %0\n\taddl %3, %0" : "=&r"(r) : "r"(a), "r"(b), "r"(c));
    return r;
}
static unsigned bswap(unsigned x)
{
    __asm__("bswap %0" : "+r"(x));
    return x;
}
static int cas(int *p, int old, int new)
{
    unsigned char ok;
    __asm__ volatile("lock; cmpxchgl %3, %1\n\tsete %0" : "=q"(ok), "+m"(*p), "+a"(old) : "r"(new) : "memory", "cc");
    return ok;
}
static long named(long a, long b)
{
    long r;
    __asm__("movq %[x], %[out]; subq %[y], %[out]; addq $%c[k], %[out]; addq %[imm], %[out]" : [out] "=&r"(r) : [x] "r"(a), [y] "rm"(b), [k] "i"(100), [imm] "i"(7));
    return r;
}
static void cpuid(unsigned leaf, unsigned *r)
{
    __asm__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}
static unsigned short swab16(unsigned short x)
{
    __asm__("rolw $8, %0" : "+r"(x));
    return x;
}
static unsigned long mulhi(unsigned long a, unsigned long b)
{
    unsigned long hi, lo;
    __asm__("mulq %3" : "=a"(lo), "=d"(hi) : "a"(a), "r"(b) : "cc");
    (void)lo;
    return hi;
}
static char tab[16];
static void store8(long i, int v)
{
    __asm__("movb %b1, tab(%0)" : : "r"(i), "q"(v) : "memory");
}
int main(void)
{
    unsigned r[4];
    char vendor[13];
    int x = 5, c1;
    unsigned long t0 = rdtsc(), t1;
    sys3(1, 1, (long)"hello from syscall\n", 19);
    printf("%ld\n", sys6(1, 1, (long)"six\n", 4, 0, 0, 0));
    t1 = rdtsc();
    c1 = cas(&x, 5, 9);
    printf("%d %x %d %d %d\n", add3(1, 2, 3), bswap(0x11223344), t1 > t0, c1, x);
    c1 = cas(&x, 5, 7);
    printf("%d %d %ld %x\n", c1, x, named(50, 8), swab16(0x1234));
    cpuid(0, r);
    memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4); vendor[12] = 0;
    printf("%d %lx\n", strlen(vendor) == 12, mulhi(0x8000000000000000UL, 6));
    store8(3, 0x141);
    printf("%d\n", tab[3]);
    return 0;
}
