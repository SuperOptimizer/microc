/* mcc's <cpuid.h>: the processor's identification leaves */
#ifndef _CPUID_H_INCLUDED
#define _CPUID_H_INCLUDED
#define __cpuid(level, a, b, c, d) __cpuid_count(level, 0, a, b, c, d)
#define __cpuid_count(level, count, a, b, c, d) \
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "0"(level), "2"(count))
static __inline__ unsigned int __get_cpuid_max(unsigned int __ext, unsigned int *__sig)
{
    unsigned int __a, __b, __c, __d;
    __cpuid(__ext, __a, __b, __c, __d);
    if (__sig)
        *__sig = __b;
    return __a;
}
static __inline__ int __get_cpuid(unsigned int __leaf, unsigned int *__a, unsigned int *__b, unsigned int *__c,
                                  unsigned int *__d)
{
    if (__get_cpuid_max(__leaf & 0x80000000u, 0) < __leaf)
        return 0;
    __cpuid(__leaf, *__a, *__b, *__c, *__d);
    return 1;
}
static __inline__ int __get_cpuid_count(unsigned int __leaf, unsigned int __sub, unsigned int *__a, unsigned int *__b,
                                        unsigned int *__c, unsigned int *__d)
{
    if (__get_cpuid_max(__leaf & 0x80000000u, 0) < __leaf)
        return 0;
    __cpuid_count(__leaf, __sub, *__a, *__b, *__c, *__d);
    return 1;
}
#endif
