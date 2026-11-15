/* mcuda_pack.h: the packed form of a unit's PTX text in its module record (struct __mcuda_module .ptx).
   mcc packs the text (src/ptx.c ptx_pack; --cuda-ptx-raw keeps it plain): PTX is long and repetitive (the
   instantiations of one template differ in a few constants), a whole-unit window finds those repeats, and a
   program that links every unit (a test linking every kernel family) carries ~5x fewer bytes, so it is linked,
   written and paged in that much faster. The run time unpacks a record's text once, before it first reads it
   (mcuda_ptx_plain below); the result is the exact text, so the JIT cache key (the text) is unchanged.
   The declarations ahead of the first function body (the prologue: all that mcudahw's scan of every unit reads at
   the first launch) stay plain, so a process unpacks the bodies of only the units whose kernels it runs.
   Layout: "\0MPZ" (a PTX text never starts with a NUL), the unpacked size n (8 bytes, little endian, the
   terminating NUL included), the packed size m (8 bytes), the prologue size P (8 bytes), the P bytes of the
   prologue, then m bytes of tokens that make the remaining n - P bytes (a copy may reach back into the prologue):
     varint L, L literal bytes, [varint M - MCUDA_PACK_MINM, varint D: copy M bytes from D bytes back]
   repeated; the last token has its literals only. varint: 7 bits a byte, low first, high bit = more. */
#ifndef MCUDA_PACK_H
#define MCUDA_PACK_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MCUDA_PACK_MINM 6
#define MCUDA_PACK_HDR 28

static inline int mcuda_packed(const char *p)
{
    return p && p[0] == 0 && p[1] == 'M' && p[2] == 'P' && p[3] == 'Z';
}

static inline unsigned long long mcuda_pack_rd8(const unsigned char *p)
{
    unsigned long long v = 0;
    int k;
    for (k = 7; k >= 0; k--)
        v = v << 8 | p[k];
    return v;
}

static inline const unsigned char *mcuda_pack_var(const unsigned char *i, const unsigned char *e, unsigned long long *v)
{
    unsigned long long r = 0;
    int s = 0;
    while (i < e && (*i & 128) && s < 63) {
        r |= (unsigned long long)(*i++ & 127) << s;
        s += 7;
    }
    if (i >= e || (*i & 128))
        return 0;
    *v = r | (unsigned long long)*i++ << s;
    return i;
}

/* sizes of a packed record: n (the text with its NUL), m (the tokens), P (the plain prologue) */
static inline unsigned long long mcuda_pack_n(const char *p) { return mcuda_pack_rd8((const unsigned char *)p + 4); }
static inline unsigned long long mcuda_pack_m(const char *p) { return mcuda_pack_rd8((const unsigned char *)p + 12); }
static inline unsigned long long mcuda_pack_p(const char *p) { return mcuda_pack_rd8((const unsigned char *)p + 20); }

/* the plain prologue of packed record p into out (which has n + 8 bytes), NUL-terminated there: the declarations
   alone, a text that mcudahw can scan before the bodies are unpacked. -1 on damaged sizes. */
static inline int mcuda_unpack_prologue(const char *p, char *out)
{
    unsigned long long n = mcuda_pack_n(p), pl = mcuda_pack_p(p);
    if (n < 1 || n > (1ull << 40) || pl >= n || mcuda_pack_m(p) > (1ull << 40))
        return -1;
    memcpy(out, p + MCUDA_PACK_HDR, (size_t)pl);
    out[pl] = 0;
    return 0;
}

/* Unpacks the bodies of packed record p into out, after the prologue (out as mcuda_unpack_prologue left it), so
   that out holds the whole text. -1 on a damaged record (sizes or a copy out of range, a text that does not end
   in its NUL): out then holds the prologue alone. */
static inline int mcuda_unpack_body(const char *p, char *out)
{
    unsigned long long n = mcuda_pack_n(p), m = mcuda_pack_m(p), pl = mcuda_pack_p(p), l, ml, d;
    const unsigned char *i = (const unsigned char *)p + MCUDA_PACK_HDR + pl, *e = i + m;
    unsigned char *o = (unsigned char *)out + pl, *oe = (unsigned char *)out + n, *st = (unsigned char *)out;
    while (i < e) {
        if (!(i = mcuda_pack_var(i, e, &l)) || l > (unsigned long long)(e - i) || l > (unsigned long long)(oe - o))
            goto bad;
        memcpy(o, i, (size_t)l);
        o += l, i += l;
        if (i == e)
            break;
        if (!(i = mcuda_pack_var(i, e, &ml)) || !(i = mcuda_pack_var(i, e, &d)))
            goto bad;
        ml += MCUDA_PACK_MINM;
        if (d < 1 || d > (unsigned long long)(o - st) || ml > (unsigned long long)(oe - o))
            goto bad;
        if (d >= 8) {
            const unsigned char *s = o - d;
            unsigned long long k;
            for (k = 0; k < ml; k += 8) /* (may write up to 7 bytes past the copy: inside the 8 spare at the end) */
                memcpy(o + k, s + k, 8);
        } else {
            unsigned long long k;
            for (k = 0; k < ml; k++)
                o[k] = o[k - d];
        }
        o += ml;
    }
    if (o != oe || oe[-1] != 0)
        goto bad;
    return 0;
bad:
    out[pl] = 0;
    return -1;
}

/* Unpacks packed text p into a new buffer (malloc, n + 8 bytes; the caller keeps it for the life of the program).
   0 on a damaged record. */
static inline char *mcuda_unpack(const char *p)
{
    unsigned long long n = mcuda_pack_n(p);
    char *out;
    if (n < 1 || n > (1ull << 40) || !(out = (char *)malloc((size_t)n + 8)))
        return 0;
    if (mcuda_unpack_prologue(p, out) || mcuda_unpack_body(p, out)) {
        free(out);
        return 0;
    }
    return out;
}

/* Makes *ptx (a module record's .ptx) plain text: a packed one is unpacked and *ptx then points at the copy (kept
   for the life of the program). Call it with the record's lock held, before the text is first read. 0 when the
   text is plain now, -1 when a packed record is damaged (*ptx is then 0: no module). */
static inline int mcuda_ptx_plain(const char **ptx)
{
    if (!mcuda_packed(*ptx))
        return 0;
    *ptx = mcuda_unpack(*ptx);
    return *ptx ? 0 : -1;
}
#endif
