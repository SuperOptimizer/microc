/* The device library's functions on memory, which the compiler calls for copies and zeroing of objects and
   device code by name, and the 128-bit division (libc/src/int128.c). Where the pointers allow it, eight or
   four bytes move at a time. */
#include "int128.c"
#define OUT __device__ __attribute__((noinline))
typedef unsigned long W8;
typedef unsigned int W4;
typedef unsigned char B;

__device__ static __forceinline__ void mcd_copy(B *d, const B *s, unsigned long n)
{
    unsigned long a = (unsigned long)d | (unsigned long)s;
    if (!(a & 7)) {
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(W8 *)d = *(const W8 *)s;
    } else if (!(a & 3)) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(W4 *)d = *(const W4 *)s;
    }
    for (; n; n--)
        *d++ = *s++;
}
__device__ static __forceinline__ void mcd_fill(B *d, unsigned c, unsigned long n)
{
    W8 w = (c & 0xff) * 0x0101010101010101UL;
    for (; n && ((unsigned long)d & 7); n--)
        *d++ = (B)c;
    for (; n >= 8; n -= 8, d += 8)
        *(W8 *)d = w;
    for (; n; n--)
        *d++ = (B)c;
}
OUT void *mcdw_memcpy(void *d, const void *s, unsigned long n)
{
    mcd_copy((B *)d, (const B *)s, n);
    return d;
}
OUT void *mcdw_memmove(void *dst, const void *src, unsigned long n)
{
    B *d = (B *)dst;
    const B *s = (const B *)src;
    if (d <= s || d >= s + n)
        mcd_copy(d, s, n);
    else
        while (n) {
            n--;
            d[n] = s[n];
        }
    return dst;
}
OUT void *mcdw_memset(void *d, int c, unsigned long n)
{
    mcd_fill((B *)d, (unsigned)c, n);
    return d;
}
OUT void mcdw_bzero(void *d, unsigned long n) { mcd_fill((B *)d, 0, n); }
OUT int mcdw_memcmp(const void *a, const void *b, unsigned long n)
{
    const B *p = (const B *)a, *q = (const B *)b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return (int)*p - (int)*q;
    return 0;
}
OUT unsigned long mcdw_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}
OUT void mcdw___mcc_divmod128(unsigned long *q, unsigned long alo, unsigned long ahi, unsigned long blo, unsigned long bhi, int is_signed)
{
    __mcc_divmod128(q, alo, ahi, blo, bhi, is_signed);
}

__global__ void mcd_all_mem(void *a, void *b, unsigned long *q)
{
    mcdw_memcpy(a, b, q[0]);
    mcdw_memmove(a, b, q[0]);
    mcdw_memset(a, 1, q[0]);
    mcdw_bzero(a, q[0]);
    q[1] = (unsigned long)mcdw_memcmp(a, b, q[0]) + mcdw_strlen((const char *)a);
    mcdw___mcc_divmod128(q, q[0], q[1], q[2], q[3], 1);
}
extern "C" void mcd_unit_mem(void);
void mcd_unit_mem(void) { mcd_all_mem<<<1, 1>>>((void *)0, (void *)0, (unsigned long *)0); }
