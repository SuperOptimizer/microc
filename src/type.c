/* C types: construction, layout, conversions. LP64, char is signed. */
#include "cc.h"
#include <string.h>

Type *ty_void, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint, *ty_long, *ty_ulong,
    *ty_float, *ty_double, *ty_ldouble, *ty_bool, *ty_llong, *ty_ullong, *ty_int128, *ty_uint128, *ty_float16, *ty_bf16;

static Arena *ta;

static Type *new_type(int kind, u64 size, u32 align)
{
    Type *t = (Type *)arena_alloc(ta, sizeof(Type));
    t->kind = (u8)kind;
    t->size = size;
    t->align = align;
    t->complete = 1;
    t->canon = t;
    return t;
}

void type_init(Arena *a)
{
    ta = a;
    ty_void = new_type(TY_VOID, 1, 1);
    ty_void->complete = 0;
    ty_char = new_type(TY_CHAR, 1, 1);
    ty_schar = new_type(TY_SCHAR, 1, 1);
    ty_uchar = new_type(TY_UCHAR, 1, 1);
    ty_short = new_type(TY_SHORT, 2, 2);
    ty_ushort = new_type(TY_USHORT, 2, 2);
    ty_int = new_type(TY_INT, 4, 4);
    ty_uint = new_type(TY_UINT, 4, 4);
    ty_long = new_type(TY_LONG, 8, 8);
    ty_ulong = new_type(TY_ULONG, 8, 8);
    ty_float = new_type(TY_FLOAT, 4, 4);
    ty_double = new_type(TY_DOUBLE, 8, 8);
    ty_ldouble = new_type(TY_LDOUBLE, 8, 8);
    ty_bool = new_type(TY_BOOL, 1, 1);
    ty_llong = new_type(TY_LLONG, 8, 8);
    ty_ullong = new_type(TY_ULLONG, 8, 8);
    ty_int128 = new_type(TY_INT128, 16, 16);
    ty_uint128 = new_type(TY_UINT128, 16, 16);
    ty_float16 = new_type(TY_FLOAT16, 2, 2);
    ty_bf16 = new_type(TY_BF16, 2, 2);
}

Type *type_ptr(Type *base)
{
    Type *t;
    if (base->ptr_to)
        return base->ptr_to;
    t = new_type(TY_PTR, 8, 8);
    t->base = base;
    base->ptr_to = t;
    return t;
}

Type *type_ref(Type *base)
{
    Type *t = new_type(TY_REF, 8, 8);
    t->base = base;
    return t;
}

Type *type_tparam(u32 index)
{
    Type *t = new_type(TY_TPARAM, 1, 1);
    t->len = index;
    return t;
}

Type *type_array(Type *base, u64 len, int complete)
{
    Type *t = new_type(TY_ARRAY, type_size(base) * len, type_align(base));
    t->base = base;
    t->len = len;
    t->complete = (u8)complete;
    return t;
}

Type *type_func(Type *ret, Type **params, u32 *pnames, u32 nparams, int variadic, int oldstyle)
{
    Type *t = new_type(TY_FUNC, 1, 1);
    t->base = ret;
    t->params = params;
    t->pnames = pnames;
    t->nparams = nparams;
    t->variadic = (u8)variadic;
    t->oldstyle = (u8)oldstyle;
    return t;
}

Type *type_record(int kind, u32 tag)
{
    Type *t = new_type(kind, 0, 1);
    t->complete = 0;
    t->tag = tag;
    return t;
}

Type *type_qual(Type *t, int quals)
{
    Type *q;
    if ((t->quals | quals) == t->quals)
        return t;
    q = (Type *)arena_alloc(ta, sizeof(Type));
    *q = *t;
    q->quals |= (u8)quals;
    q->ptr_to = 0;
    return q;
}

Type *type_aligned(Type *t, u32 align)
{
    Type *q;
    if (type_align(t) == align)
        return t;
    q = (Type *)arena_alloc(ta, sizeof(Type));
    *q = *t;
    q->xalign = align;
    q->ptr_to = 0;
    return q;
}

void type_layout(Type *t, Field *fields, int packed, u32 minalign)
{
    Field *f;
    u64 bitpos = 0, size = 0;
    u32 align = 1;
    t = t->canon;
    for (f = fields; f; f = f->next) {
        u64 fsize = type_size(f->ty);
        u32 falign = type_align(f->ty);
        int pk = packed || f->packed;
        if (pk)
            falign = 1;
        if (f->align && (pk || f->align > falign))
            falign = f->align;
        if (pk && (f->bitwidth || f->bitoff == 255)) {
            /* packed bit-fields follow each other bit by bit */
            if (t->kind == TY_UNION)
                bitpos = 0;
            if (f->bitoff == 255) {
                bitpos = align_up(bitpos, 8);
                f->bitoff = 0;
                continue;
            }
            if (bitpos % 8 + f->bitwidth > fsize * 8)
                bitpos = align_up(bitpos, 8); /* would not fit the access unit */
            f->off = (u32)(bitpos / 8);
            f->bitoff = (u8)(bitpos % 8);
            if (t->kind == TY_UNION) {
                if ((u64)(f->bitwidth + 7) / 8 > size)
                    size = (u64)(f->bitwidth + 7) / 8;
            } else {
                bitpos += f->bitwidth;
                size = (bitpos + 7) / 8;
            }
            continue;
        }
        if (f->bitwidth || f->bitoff == 255) {
            /* bit-field; bitoff == 255 marks a zero-width field */
            u64 unit = fsize * 8;
            if (t->kind == TY_UNION)
                bitpos = 0;
            if (f->bitoff == 255) {
                bitpos = align_up(bitpos, unit);
                f->bitoff = 0;
                continue;
            }
            if (bitpos / unit != (bitpos + f->bitwidth - 1) / unit)
                bitpos = align_up(bitpos, unit);
            f->off = (u32)(bitpos / unit * fsize);
            f->bitoff = (u8)(bitpos % unit);
            if (t->kind == TY_UNION) {
                if (fsize > size)
                    size = fsize;
            } else {
                bitpos += f->bitwidth;
                size = (bitpos + 7) / 8;
            }
        } else if (t->kind == TY_UNION) {
            f->off = 0;
            if (fsize > size)
                size = fsize;
        } else {
            u64 off = align_up((bitpos + 7) / 8, falign);
            f->off = (u32)off;
            bitpos = (off + fsize) * 8;
            size = off + fsize;
        }
        if (falign > align)
            align = falign;
    }
    if (minalign > align)
        align = minalign;
    t->fields = fields;
    t->align = align;
    t->size = align_up(size, align);
    t->complete = 1;
}

Field *type_field(Type *t, u32 name)
{
    Field *f;
    for (f = t->canon->fields; f; f = f->next)
        if (f->name == name)
            return f;
    return 0;
}

Field *type_member(Type *t, u32 name, u64 *off)
{
    Field *f, *g;
    for (f = t->canon->fields; f; f = f->next) {
        if (f->name == name) {
            *off += f->off;
            return f;
        }
        if (!f->name && type_is_record(f->ty)) {
            u64 o = *off + f->off;
            g = type_member(f->ty, name, &o);
            if (g) {
                *off = o;
                return g;
            }
        }
    }
    return 0;
}

int type_is_int(Type *t)
{
    return t->kind >= TY_BOOL && t->kind <= TY_UINT128;
}

int type_is_float(Type *t)
{
    return t->kind >= TY_BF16 && t->kind <= TY_LDOUBLE;
}

int type_is_arith(Type *t)
{
    return t->kind >= TY_BOOL && t->kind <= TY_LDOUBLE;
}

int type_is_scalar(Type *t)
{
    return (t->kind >= TY_BOOL && t->kind <= TY_LDOUBLE) || t->kind == TY_PTR;
}

int type_is_record(Type *t)
{
    return t->kind == TY_STRUCT || t->kind == TY_UNION;
}

int type_is_unsigned(Type *t)
{
    return t->kind == TY_UCHAR || t->kind == TY_USHORT || t->kind == TY_UINT || t->kind == TY_ULONG ||
           t->kind == TY_ULLONG || t->kind == TY_UINT128 || t->kind == TY_BOOL || t->kind == TY_PTR ||
           (t->kind == TY_CHAR && opt_target == TARGET_LINUX && opt_arch != ARCH_X64); /* plain char is unsigned on aarch64 Linux */
}

int type_complete(Type *t)
{
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        return t->canon->complete;
    if (t->kind == TY_ARRAY)
        return t->complete && type_complete(t->base);
    return t->complete;
}

u64 type_size(Type *t)
{
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        return t->canon->size;
    if (t->kind == TY_ARRAY)
        return type_size(t->base) * t->len;
    return t->size;
}

u32 type_align(Type *t)
{
    if (t->xalign)
        return t->xalign;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        return t->canon->align;
    if (t->kind == TY_ARRAY)
        return type_align(t->base);
    return t->align;
}

Type *type_promote(Type *t)
{
    if (t->kind >= TY_BOOL && t->kind < TY_INT)
        return ty_int;
    if (t->quals || t->xalign)
        switch (t->kind) {
        case TY_INT: return ty_int;
        case TY_UINT: return ty_uint;
        case TY_LONG: return ty_long;
        case TY_ULONG: return ty_ulong;
        case TY_LLONG: return ty_llong;
        case TY_ULLONG: return ty_ullong;
        case TY_INT128: return ty_int128;
        case TY_UINT128: return ty_uint128;
        case TY_FLOAT16: return ty_float16;
        case TY_BF16: return ty_bf16;
        case TY_FLOAT: return ty_float;
        case TY_DOUBLE: return ty_double;
        case TY_LDOUBLE: return ty_ldouble;
        }
    return t;
}

Type *type_usual(Type *a, Type *b)
{
    a = type_promote(a);
    b = type_promote(b);
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
        return ty_ldouble;
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
        return ty_double;
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return ty_float;
    if (a->kind == TY_FLOAT16 || b->kind == TY_FLOAT16)
        return ty_float16;
    if (a->kind == TY_BF16 || b->kind == TY_BF16)
        return ty_bf16;
    if (a->kind == TY_UINT128 || b->kind == TY_UINT128)
        return ty_uint128;
    if (a->kind == TY_INT128 || b->kind == TY_INT128)
        return ty_int128;
    if (a->kind == TY_ULLONG || b->kind == TY_ULLONG)
        return ty_ullong;
    if (a->kind == TY_LLONG || b->kind == TY_LLONG)
        return a->kind == TY_ULONG || b->kind == TY_ULONG ? ty_ullong : ty_llong;
    if (a->kind == TY_ULONG || b->kind == TY_ULONG)
        return ty_ulong;
    if (a->kind == TY_LONG || b->kind == TY_LONG)
        return ty_long;
    if (a->kind == TY_UINT || b->kind == TY_UINT)
        return ty_uint;
    return ty_int;
}

int type_compatible(Type *a, Type *b)
{
    u32 i;
    if (a == b)
        return 1;
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case TY_PTR:
    case TY_REF:
        return a->base->quals == b->base->quals && type_compatible(a->base, b->base);
    case TY_TPARAM:
        return a->len == b->len;
    case TY_ARRAY:
        if (a->complete && b->complete && a->len != b->len)
            return 0;
        return type_compatible(a->base, b->base);
    case TY_STRUCT:
    case TY_UNION:
        return a->canon == b->canon;
    case TY_FUNC:
        if (!type_compatible(a->base, b->base))
            return 0;
        if (a->oldstyle || b->oldstyle)
            return 1;
        if (a->nparams != b->nparams || a->variadic != b->variadic)
            return 0;
        for (i = 0; i < a->nparams; i++)
            if (!type_compatible(a->params[i], b->params[i]))
                return 0;
        return 1;
    }
    return 1;
}

int type_ir(Type *t)
{
    switch (t->kind) {
    case TY_LONG:
    case TY_ULONG:
    case TY_LLONG:
    case TY_ULLONG:
    case TY_INT128:
    case TY_UINT128:
    case TY_PTR:
    case TY_REF:
    case TY_ARRAY:
    case TY_FUNC:
    case TY_STRUCT:
    case TY_UNION:
        return IT_I64;
    case TY_BF16:
    case TY_FLOAT16: /* held as the float of the same value */
    case TY_FLOAT:
        return IT_F32;
    case TY_DOUBLE:
    case TY_LDOUBLE:
        return IT_F64;
    case TY_VOID:
        return IT_VOID;
    }
    return IT_I32;
}

int type_mem(Type *t)
{
    switch (t->kind) {
    case TY_CHAR:
        return opt_target == TARGET_LINUX && opt_arch != ARCH_X64 ? M_8U : M_8S;
    case TY_SCHAR:
        return M_8S;
    case TY_UCHAR:
    case TY_BOOL:
        return M_8U;
    case TY_SHORT:
        return M_16S;
    case TY_USHORT:
    case TY_BF16:
    case TY_FLOAT16: /* its bits; parse.c converts */
        return M_16U;
    case TY_INT:
    case TY_UINT:
        return M_32;
    case TY_FLOAT:
        return M_F32;
    case TY_DOUBLE:
    case TY_LDOUBLE:
        return M_F64;
    }
    return M_64;
}
