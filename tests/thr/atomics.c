/* Atomic operations, one thread: every operation on every size must give
   what the plain operation gives. */
#include <stdatomic.h>
#include <stdio.h>

static unsigned long sum;
static void note(unsigned long v) { sum = sum * 31 + v; }

#define OPS(T, name, A, B)                                                                   \
    static void test_##name(void)                                                            \
    {                                                                                        \
        _Atomic T x = (T)(A);                                                                \
        T plain = (T)(A), e, old, y;                                                          \
        _Bool ok;                                                                            \
        sum = 0;                                                                             \
        note((unsigned long)atomic_load(&x));                                                \
        atomic_store(&x, (T)(B));                                                            \
        plain = (T)(B);                                                                      \
        old = atomic_exchange(&x, (T)(A));                                                   \
        if (old != (T)(B)) note(1001);                                                       \
        plain = (T)(A);                                                                      \
        old = atomic_fetch_add(&x, (T)(B)); if (old != plain) note(1002); plain += (T)(B);   \
        old = atomic_fetch_sub(&x, (T)3); if (old != plain) note(1003); plain -= (T)3;       \
        old = atomic_fetch_or(&x, (T)0x55); if (old != plain) note(1004); plain |= (T)0x55;  \
        old = atomic_fetch_and(&x, (T)~(T)0x11); if (old != plain) note(1005); plain &= (T)~(T)0x11; \
        old = atomic_fetch_xor(&x, (T)(B)); if (old != plain) note(1006); plain ^= (T)(B);   \
        if (atomic_load(&x) != plain) note(1007);                                            \
        note((unsigned long)plain);                                                          \
        e = (T)(plain + 1);                                                                  \
        ok = atomic_compare_exchange_strong(&x, &e, (T)9);                                   \
        if (ok || e != plain) note(1008);                                                    \
        ok = atomic_compare_exchange_strong(&x, &e, (T)9);                                   \
        if (!ok || e != plain || atomic_load(&x) != (T)9) note(1009);                        \
        e = 9;                                                                               \
        while (!atomic_compare_exchange_weak(&x, &e, (T)(A))) {}                             \
        plain = (T)(A);                                                                      \
        /* the operators */                                                                  \
        if (x++ != plain++) note(1010);                                                      \
        if (++x != ++plain) note(1011);                                                      \
        if (x-- != plain--) note(1012);                                                      \
        if (--x != --plain) note(1013);                                                      \
        if ((x += (T)(B)) != (plain += (T)(B))) note(1014);                                  \
        if ((x -= (T)7) != (plain -= (T)7)) note(1015);                                      \
        if ((x *= (T)3) != (plain *= (T)3)) note(1016);                                      \
        if ((x |= (T)0x30) != (plain |= (T)0x30)) note(1017);                                \
        if ((x &= (T)0x7f) != (plain &= (T)0x7f)) note(1018);                                \
        if ((x ^= (T)0x1f) != (plain ^= (T)0x1f)) note(1019);                                \
        if ((x <<= 1) != (plain <<= 1)) note(1020);                                          \
        if ((x >>= 2) != (plain >>= 2)) note(1021);                                          \
        if ((x /= (T)3) != (plain /= (T)3)) note(1022);                                      \
        if ((x %= (T)5) != (plain %= (T)5)) note(1023);                                      \
        x = (T)(B);                                                                          \
        plain = x;                                                                           \
        note((unsigned long)plain);                                                          \
        /* the builtins, on a plain object */                                                \
        y = plain;                                                                           \
        /* */                                                                   \
        if (__atomic_add_fetch(&y, (T)5, __ATOMIC_SEQ_CST) != (T)(plain + 5)) note(1030);    \
        if (__atomic_sub_fetch(&y, (T)5, __ATOMIC_RELAXED) != plain) note(1031);             \
        if (__atomic_or_fetch(&y, (T)3, __ATOMIC_SEQ_CST) != (T)(plain | 3)) note(1032);     \
        if (__atomic_and_fetch(&y, (T)0x3e, __ATOMIC_SEQ_CST) != (T)((plain | 3) & 0x3e)) note(1033); \
        if (__atomic_xor_fetch(&y, (T)0x3e, __ATOMIC_SEQ_CST) != (T)(((plain | 3) & 0x3e) ^ 0x3e)) note(1034); \
        __atomic_store_n(&y, plain, __ATOMIC_RELEASE);                                       \
        if (__atomic_fetch_nand(&y, (T)6, __ATOMIC_SEQ_CST) != plain) note(1035);            \
        if (__atomic_load_n(&y, __ATOMIC_ACQUIRE) != (T)~(plain & 6)) note(1036);            \
        if (__atomic_exchange_n(&y, plain, __ATOMIC_ACQ_REL) != (T)~(plain & 6)) note(1037); \
        if (__sync_fetch_and_add(&y, (T)2) != plain) note(1038);                             \
        if (__sync_sub_and_fetch(&y, (T)2) != plain) note(1039);                             \
        if (__sync_fetch_and_or(&y, (T)1) != plain) note(1040);                              \
        if (__sync_fetch_and_and(&y, (T)~(T)1) != (T)(plain | 1)) note(1041);                \
        if (__sync_fetch_and_sub(&y, (T)0) != (T)(plain & ~(T)1)) note(1042);                \
        y = plain;                                                                           \
        if (__sync_bool_compare_and_swap(&y, (T)(plain + 1), (T)4)) note(1043);              \
        if (!__sync_bool_compare_and_swap(&y, plain, (T)4)) note(1044);                      \
        if (__sync_val_compare_and_swap(&y, (T)4, (T)6) != (T)4) note(1045);                 \
        if (__sync_val_compare_and_swap(&y, (T)4, (T)8) != (T)6) note(1046);                 \
        if (__sync_lock_test_and_set(&y, (T)1) != (T)6) note(1047);                          \
        __sync_lock_release(&y);                                                             \
        if (y != 0) note(1048);                                                              \
        printf("%-6s %lu\n", #name, sum);                                                    \
    }

OPS(signed char, schar, -100, 77)
OPS(unsigned char, uchar, 200, 177)
OPS(short, sshort, -30000, 12345)
OPS(unsigned short, ushort, 60000, 54321)
OPS(int, sint, -2000000000, 1234567)
OPS(unsigned, uint, 4000000000U, 7654321)
OPS(long, slong, -9000000000000000000L, 123456789012345L)
OPS(unsigned long, ulong, 18000000000000000000UL, 98765432109876UL)

struct node {
    int v;
    _Atomic(struct node *) next;
    _Atomic int count;
    atomic_flag busy;
    _Atomic _Bool on;
    _Atomic double d;
    _Atomic float f;
};

static _Atomic(int *) gp;
static atomic_size_t gsize = 10;
static atomic_uint_fast64_t gfast;
static atomic_llong gll = -5;
static int cells[8];

int main(void)
{
    struct node a = {1}, b = {2}, *e;
    atomic_flag fl = ATOMIC_FLAG_INIT;
    int *ip;
    test_schar();
    test_uchar();
    test_sshort();
    test_ushort();
    test_sint();
    test_uint();
    test_slong();
    test_ulong();

    /* pointers */
    atomic_init(&a.next, &b);
    printf("next %d\n", atomic_load(&a.next)->v);
    e = &a;
    printf("cas %d", atomic_compare_exchange_strong(&a.next, &e, &a));
    printf(" %d\n", e->v);
    printf("cas %d %d\n", atomic_compare_exchange_strong(&a.next, &e, &a), a.next->v);
    printf("xchg %d %d\n", atomic_exchange(&a.next, &b)->v, a.next->v);
    gp = cells;
    gp++;
    gp += 3;
    ip = gp--;
    printf("ptr %ld %ld %ld\n", (long)(ip - cells), (long)(gp - cells), (long)(--gp - cells));
    gp -= 2;
    printf("ptr %ld\n", (long)(gp - cells));

    /* flags, booleans, floating point */
    printf("flag %d %d", atomic_flag_test_and_set(&fl), atomic_flag_test_and_set(&fl));
    atomic_flag_clear(&fl);
    printf(" %d\n", atomic_flag_test_and_set_explicit(&fl, memory_order_acquire));
    printf("busy %d %d\n", atomic_flag_test_and_set(&a.busy), atomic_flag_test_and_set(&a.busy));
    a.on = 5;
    printf("bool %d", a.on);
    a.on = 0;
    printf(" %d %d\n", a.on, atomic_exchange(&a.on, 1));
    a.d = 1.5;
    a.d += 2.25;
    a.d *= 2;
    a.d -= 0.5;
    a.f = 3;
    a.f /= 2;
    a.f++;
    printf("fp %g %g %g\n", a.d, (double)a.f, a.d + a.f);

    /* members, the wider typedefs, explicit orders */
    a.count = 40;
    a.count++;
    atomic_fetch_add_explicit(&a.count, 1, memory_order_relaxed);
    printf("count %d %d\n", a.count, atomic_load_explicit(&a.count, memory_order_relaxed));
    atomic_store_explicit(&gsize, 99, memory_order_release);
    gfast = 1;
    gfast <<= 40;
    gll *= 3;
    printf("wide %lu %lu %lld\n", (unsigned long)gsize, (unsigned long)atomic_fetch_or(&gfast, 1), gll);
    if (gll == -15 && gfast != 0)
        printf("compare ok\n");
    atomic_thread_fence(memory_order_seq_cst);
    __sync_synchronize();
    printf("sizes %d %d %d %d\n", (int)sizeof(atomic_int), (int)sizeof(atomic_flag), (int)sizeof(_Atomic(char *)),
           (int)sizeof(atomic_bool));
    return 0;
}
