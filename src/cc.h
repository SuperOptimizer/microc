/* C front end: types and declarations shared by type.c and parse.c. */
#ifndef CC_H
#define CC_H

#include "ir.h"

enum {
    TY_VOID, TY_BOOL, TY_CHAR, TY_SCHAR, TY_UCHAR, TY_SHORT, TY_USHORT, TY_INT, TY_UINT, TY_LONG, TY_ULONG,
    TY_LLONG, TY_ULLONG, TY_INT128, TY_UINT128,
    TY_BF16, /* the 16-bit "brain" float: the top half of a float */
    TY_FLOAT16, TY_FLOAT, TY_DOUBLE, TY_LDOUBLE, TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION,
    TY_REF,   /* C++: a reference to base; only in declarations, never the type of an expression */
    TY_TPARAM /* C++: a template's type parameter (number len), while its declaration is studied */
};

#define Q_CONST 1
#define Q_VOLATILE 2
#define Q_ATOMIC 4 /* reads and writes of the object are atomic operations */

typedef struct Type Type;

typedef struct Field {
    u32 name;
    Type *ty;
    u32 off;
    u8 bitoff;
    u8 bitwidth; /* 0: not a bit-field */
    u8 packed;
    u32 align;   /* from an attribute; 0: that of the type */
    struct Field *next;
} Field;

struct Type {
    u8 kind;
    u8 quals;
    u8 complete;
    u8 variadic;
    u8 oldstyle; /* function declared without a prototype */
    u8 vkind;    /* a struct that is really 1: a GNU vector, 2: a complex number; base = element type, len = count */
    u32 align;
    u32 xalign; /* alignment given by an attribute, overriding the natural one */
    u32 vslot;  /* a variable length array: 1 + the stack slot that holds its size in bytes */
    u64 size;
    Type *base;     /* pointer target, array element, function return */
    u64 len;        /* array length */
    Type **params;  /* function parameter types */
    u32 *pnames;    /* function parameter names (0 if unnamed) */
    u32 nparams;
    Field *fields;
    u32 tag;
    Type *canon;    /* struct/union: the type that owns the definition */
    Type *ptr_to;   /* cached pointer-to-this */
    void *lam;      /* C++: the lambda this (empty) record stands for */
    u8 guard;       /* C++: a lock guard: made from a mutex, which it releases at the end of its scope */
    u8 transparent; /* a union with __attribute__((transparent_union)): as a parameter it takes any of its members */
    u32 lname;      /* C++: an unnamed record's name in symbols: that of its first typedef */
    void *ctm, *cti; /* C++: a class template's instance: the template and the instance (Itanium names: name I args E) */
};

extern Type *ty_void, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint, *ty_long,
    *ty_ulong, *ty_float, *ty_double, *ty_ldouble, *ty_bool, *ty_llong, *ty_ullong, *ty_int128, *ty_uint128, *ty_float16, *ty_bf16;

void type_init(Arena *a);
Type *type_ptr(Type *base);
Type *type_ref(Type *base);
Type *type_tparam(u32 index);
Type *type_array(Type *base, u64 len, int complete);
Type *type_func(Type *ret, Type **params, u32 *pnames, u32 nparams, int variadic, int oldstyle);
Type *type_record(int kind, u32 tag);
Type *type_qual(Type *t, int quals);
void type_layout(Type *t, Field *fields, int packed, u32 minalign); /* completes a struct or union */
Field *type_field(Type *t, u32 name);
Field *type_member(Type *t, u32 name, u64 *off); /* also looks inside unnamed members; *off from the start of t */
Type *type_aligned(Type *t, u32 align);
int type_is_int(Type *t);
#define type_is_128(t) ((t)->kind == TY_INT128 || (t)->kind == TY_UINT128)
int type_is_float(Type *t);
int type_is_arith(Type *t);
int type_is_scalar(Type *t);
int type_is_record(Type *t);
int type_is_unsigned(Type *t);
int type_complete(Type *t);
u64 type_size(Type *t);
u32 type_align(Type *t);
Type *type_promote(Type *t);
Type *type_usual(Type *a, Type *b);
int type_compatible(Type *a, Type *b);
int type_ir(Type *t);  /* IT_* of a scalar */
int type_mem(Type *t); /* M_* of a scalar */

/* parse.c */
void cc_compile(const char *path, Obj *o, int dump_ir);

#endif
