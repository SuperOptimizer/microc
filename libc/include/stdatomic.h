#ifndef _STDATOMIC_H
#define _STDATOMIC_H
/* C11 atomics. Objects of 1, 2, 4 and 8 bytes are lock-free; the compiler does the operations in line. */

typedef enum memory_order {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5
} memory_order;

#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5
#endif

typedef _Atomic _Bool atomic_bool;
typedef _Atomic char atomic_char;
typedef _Atomic signed char atomic_schar;
typedef _Atomic unsigned char atomic_uchar;
typedef _Atomic short atomic_short;
typedef _Atomic unsigned short atomic_ushort;
typedef _Atomic int atomic_int;
typedef _Atomic unsigned int atomic_uint;
typedef _Atomic long atomic_long;
typedef _Atomic unsigned long atomic_ulong;
typedef _Atomic long long atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic unsigned short atomic_char16_t;
typedef _Atomic unsigned int atomic_char32_t;
typedef _Atomic int atomic_wchar_t;
typedef _Atomic signed char atomic_int_least8_t;
typedef _Atomic unsigned char atomic_uint_least8_t;
typedef _Atomic short atomic_int_least16_t;
typedef _Atomic unsigned short atomic_uint_least16_t;
typedef _Atomic int atomic_int_least32_t;
typedef _Atomic unsigned int atomic_uint_least32_t;
typedef _Atomic long atomic_int_least64_t;
typedef _Atomic unsigned long atomic_uint_least64_t;
typedef _Atomic signed char atomic_int_fast8_t;
typedef _Atomic unsigned char atomic_uint_fast8_t;
typedef _Atomic long atomic_int_fast16_t;
typedef _Atomic unsigned long atomic_uint_fast16_t;
typedef _Atomic long atomic_int_fast32_t;
typedef _Atomic unsigned long atomic_uint_fast32_t;
typedef _Atomic long atomic_int_fast64_t;
typedef _Atomic unsigned long atomic_uint_fast64_t;
typedef _Atomic long atomic_intptr_t;
typedef _Atomic unsigned long atomic_uintptr_t;
typedef _Atomic unsigned long atomic_size_t;
typedef _Atomic long atomic_ptrdiff_t;
typedef _Atomic long atomic_intmax_t;
typedef _Atomic unsigned long atomic_uintmax_t;

#define ATOMIC_BOOL_LOCK_FREE 2
#define ATOMIC_CHAR_LOCK_FREE 2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE 2
#define ATOMIC_SHORT_LOCK_FREE 2
#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

#define ATOMIC_VAR_INIT(value) (value)
#define atomic_init(obj, value) __atomic_store_n((obj), (value), __ATOMIC_RELAXED)
#define kill_dependency(y) (y)
#define atomic_is_lock_free(obj) (sizeof *(obj) <= 8)

#define atomic_store_explicit(obj, value, order) __atomic_store_n((obj), (value), (order))
#define atomic_store(obj, value) __atomic_store_n((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_load_explicit(obj, order) __atomic_load_n((obj), (order))
#define atomic_load(obj) __atomic_load_n((obj), __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(obj, value, order) __atomic_exchange_n((obj), (value), (order))
#define atomic_exchange(obj, value) __atomic_exchange_n((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, success, failure) \
    __atomic_compare_exchange_n((obj), (expected), (desired), 0, (success), (failure))
#define atomic_compare_exchange_strong(obj, expected, desired) \
    __atomic_compare_exchange_n((obj), (expected), (desired), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(obj, expected, desired, success, failure) \
    __atomic_compare_exchange_n((obj), (expected), (desired), 1, (success), (failure))
#define atomic_compare_exchange_weak(obj, expected, desired) \
    __atomic_compare_exchange_n((obj), (expected), (desired), 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(obj, value, order) __atomic_fetch_add((obj), (value), (order))
#define atomic_fetch_add(obj, value) __atomic_fetch_add((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(obj, value, order) __atomic_fetch_sub((obj), (value), (order))
#define atomic_fetch_sub(obj, value) __atomic_fetch_sub((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(obj, value, order) __atomic_fetch_or((obj), (value), (order))
#define atomic_fetch_or(obj, value) __atomic_fetch_or((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(obj, value, order) __atomic_fetch_xor((obj), (value), (order))
#define atomic_fetch_xor(obj, value) __atomic_fetch_xor((obj), (value), __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(obj, value, order) __atomic_fetch_and((obj), (value), (order))
#define atomic_fetch_and(obj, value) __atomic_fetch_and((obj), (value), __ATOMIC_SEQ_CST)

typedef struct atomic_flag {
    _Atomic unsigned char __v;
} atomic_flag;
#define ATOMIC_FLAG_INIT {0}
#define atomic_flag_test_and_set_explicit(flag, order) __atomic_test_and_set(&(flag)->__v, (order))
#define atomic_flag_test_and_set(flag) __atomic_test_and_set(&(flag)->__v, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(flag, order) __atomic_clear(&(flag)->__v, (order))
#define atomic_flag_clear(flag) __atomic_clear(&(flag)->__v, __ATOMIC_SEQ_CST)

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)
#endif
