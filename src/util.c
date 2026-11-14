#include "mcc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("mcc: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

/* ---- arena ---- */

struct ArenaChunk {
    ArenaChunk *next;
    size_t size;
};

#define ARENA_CHUNK (1024 * 1024)

void *arena_alloc(Arena *a, size_t n)
{
    void *p = arena_alloc_raw(a, n);
    memset(p, 0, n);
    return p;
}

void arena_reset(Arena *a)
{
    ArenaChunk *c = a->chunks;
    size_t hdr = (sizeof(ArenaChunk) + 7) & ~(size_t)7;
    if (!c)
        return;
    while (c->next) {
        ArenaChunk *next = c->next;
        c->next = next->next;
        free(next);
    }
    a->p = (char *)c + hdr;
    a->end = (char *)c + c->size;
}

void *arena_alloc_raw(Arena *a, size_t n)
{
    char *p;
    n = (n + 7) & ~(size_t)7;
    if ((size_t)(a->end - a->p) < n) {
        size_t hdr = (sizeof(ArenaChunk) + 7) & ~(size_t)7;
        size_t size = n + hdr > ARENA_CHUNK ? n + hdr : ARENA_CHUNK;
        ArenaChunk *c = (ArenaChunk *)xmalloc(size);
        c->next = a->chunks;
        c->size = size;
        a->chunks = c;
        a->p = (char *)c + hdr;
        a->end = (char *)c + size;
    }
    p = a->p;
    a->p += n;
    return p;
}

void arena_free(Arena *a)
{
    ArenaChunk *c = a->chunks;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->chunks = 0;
    a->p = a->end = 0;
}

/* ---- growable byte buffer ---- */

void buf_reserve(Buf *b, size_t extra)
{
    if (b->cap - b->len < extra) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap - b->len < extra)
            cap *= 2;
        b->p = (u8 *)xrealloc(b->p, cap);
        b->cap = cap;
    }
}

void buf_put(Buf *b, const void *src, size_t n)
{
    buf_reserve(b, n);
    if (n)
        memcpy(b->p + b->len, src, n);
    b->len += n;
}

void buf_zero(Buf *b, size_t n)
{
    buf_reserve(b, n);
    memset(b->p + b->len, 0, n);
    b->len += n;
}

void buf_align(Buf *b, size_t align)
{
    size_t want = (size_t)align_up(b->len, align);
    buf_zero(b, want - b->len);
}

void buf_u8(Buf *b, u32 v)
{
    buf_reserve(b, 1);
    b->p[b->len++] = (u8)v;
}

void buf_u32(Buf *b, u32 v)
{
    buf_reserve(b, 4);
    put_u32le(b->p + b->len, v);
    b->len += 4;
}

void buf_u64(Buf *b, u64 v)
{
    buf_reserve(b, 8);
    put_u64le(b->p + b->len, v);
    b->len += 8;
}

void buf_uleb(Buf *b, u64 v)
{
    do {
        u32 byte = (u32)(v & 0x7f);
        v >>= 7;
        if (v)
            byte |= 0x80;
        buf_u8(b, byte);
    } while (v);
}

void buf_free(Buf *b)
{
    free(b->p);
    b->p = 0;
    b->len = b->cap = 0;
}

void put_u32le(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

void put_u64le(u8 *p, u64 v)
{
    put_u32le(p, (u32)v);
    put_u32le(p + 4, (u32)(v >> 32));
}

void put_u32be(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

void put_u64be(u8 *p, u64 v)
{
    put_u32be(p, (u32)(v >> 32));
    put_u32be(p + 4, (u32)v);
}

u32 get_u32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

u64 get_u64le(const u8 *p)
{
    return (u64)get_u32le(p) | ((u64)get_u32le(p + 4) << 32);
}

u64 align_up(u64 v, u64 align)
{
    return (v + align - 1) & ~(align - 1);
}

/* ---- string interning ---- */

static Arena intern_arena;
static const char **intern_strs;
static u32 *intern_lens;
static u32 intern_n, intern_cap;
static u32 *intern_tab; /* open addressing, holds ids, 0 = empty */
static u32 intern_tab_size;

static u32 hash_bytes(const char *s, size_t len)
{
    u32 h = HASH_INIT;
    size_t i;
    for (i = 0; i < len; i++)
        h = HASH_STEP(h, s[i]);
    return h;
}

static void intern_grow(void)
{
    u32 size = intern_tab_size ? intern_tab_size * 2 : 4096;
    u32 *tab = (u32 *)xmalloc(size * sizeof(u32));
    u32 i;
    memset(tab, 0, size * sizeof(u32));
    for (i = 1; i < intern_n; i++) {
        u32 h = hash_bytes(intern_strs[i], intern_lens[i]);
        h = (h ^ (h >> 15)) & (size - 1);
        while (tab[h])
            h = (h + 1) & (size - 1);
        tab[h] = i;
    }
    free(intern_tab);
    intern_tab = tab;
    intern_tab_size = size;
}

u32 intern(const char *s, size_t len)
{
    return intern_hashed(s, len, hash_bytes(s, len));
}

u32 intern_hashed(const char *s, size_t len, u32 h)
{
    char *copy;
    if (len == 0)
        return 0;
    if (intern_n * 2 >= intern_tab_size) {
        if (intern_n == 0)
            intern_n = 1; /* id 0 is the empty string */
        intern_grow();
    }
    h = (h ^ (h >> 15)) & (intern_tab_size - 1);
    while (intern_tab[h]) {
        u32 id = intern_tab[h];
        if (intern_lens[id] == len && memcmp(intern_strs[id], s, len) == 0)
            return id;
        h = (h + 1) & (intern_tab_size - 1);
    }
    if (intern_n >= intern_cap) {
        intern_cap = intern_cap ? intern_cap * 2 : 4096;
        intern_strs = (const char **)xrealloc((void *)intern_strs, intern_cap * sizeof(char *));
        intern_lens = (u32 *)xrealloc(intern_lens, intern_cap * sizeof(u32));
    }
    copy = (char *)arena_alloc(&intern_arena, len + 1);
    memcpy(copy, s, len);
    intern_strs[intern_n] = copy;
    intern_lens[intern_n] = (u32)len;
    intern_tab[h] = intern_n;
    return intern_n++;
}

u32 intern_cstr(const char *s)
{
    return intern(s, strlen(s));
}

const char *intern_str(u32 id)
{
    return id ? intern_strs[id] : "";
}

size_t intern_len(u32 id)
{
    return id ? intern_lens[id] : 0;
}

u32 intern_count(void)
{
    return intern_n ? intern_n : 1;
}
