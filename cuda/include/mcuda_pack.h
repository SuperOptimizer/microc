/* mcuda_pack.h: the packed form of a unit's PTX text in its module record (struct __mcuda_module .ptx).
   mcc packs the text (src/ptx.c ptx_pack; --cuda-ptx-raw keeps it plain): PTX is long and repetitive (the
   instantiations of one template differ in a few constants), a whole-unit window finds those repeats, and a
   program that links every unit (a test linking every kernel family) carries ~5x fewer bytes, so it is linked,
   written and paged in that much faster. The run time unpacks a record's text once, before it first reads it
   (mcuda_ptx_plain below); the result is the exact text, so the JIT cache key (the text) is unchanged.
   The declarations ahead of the first function body (the prologue: all that mcudahw's scan of every unit reads at
   the first launch) stay plain, so a process unpacks the bodies of only the units whose kernels it runs.
   Layout: "\0MP" and a letter for the form (a PTX text never starts with a NUL), the unpacked size n (8 bytes,
   little endian, the terminating NUL included), the packed size m (8 bytes), the prologue size P (8 bytes), the P
   bytes of the prologue, then m bytes that make the remaining n - P bytes (a copy may reach back into the prologue).
   Two forms of those m bytes:
   - "\0MPZ" (mcc --cuda-ptx-pack=1; ~5x on ufsm): tokens
       varint L, L literal bytes, [varint M - MCUDA_PACK_MINM, varint D: copy M bytes from D bytes back]
     repeated; the last token has its literals only. varint: 7 bits a byte, low first, high bit = more.
   - "\0MPY" (mcc's default, --cuda-ptx-pack=2 / 3; ~10x / ~13x on ufsm): sequences of literals and copies with
     three repeat offsets, entropy-coded. varint S (the number of sequences); the code lengths of four canonical
     prefix codes as nibbles, low nibble first, each table a whole number of bytes: LIT (256 symbols: the literal
     bytes), LL (88: literal counts), ML (88: copy lengths - MCUDA_MPY_MINM), OF (91: offsets); then one bit stream,
     least significant bit first, codes stored bit-reversed (the first bit of a code is the stream's next bit).
     Each sequence: LL, that many LIT, ML, OF; OF 0..2 takes repeat offset 0..2 (newest first) and moves it to the
     front, OF 3 + c is a new offset (bucket code c) pushed in front; the repeats start as 1, 2, 3. After the last
     sequence, LIT up to n. Counts, lengths and offsets are bucket codes: v < 16 is the code itself; else, with
     b = floor(log2 v), the code is 16 + 2 (b - 4) + the bit below the top one, followed by the low b - 1 bits of v
     plain. Codes are at most MCUDA_MPY_MAXL bits.
   - "\0MPF" (mcc --cuda-ptx-file, with -c -o X.o): the packed record (MPZ or MPY) is in the file X.o.ptxz, not in
     the object; here m is that file's size, and the prologue is followed by two 64-bit FNV-1a hashes of the file
     (seeds MCUDA_PACK_FNV1, MCUDA_PACK_FNV2) and its absolute path, NUL-terminated. A build that links one unit into
     many programs (a test suite) then holds its PTX once. The file is read when the bodies are first wanted; it
     must have the hashes, the same n and P and the same prologue. MCUDA_PTXDIR=DIR: DIR/(the file's name) first
     (a build directory that moved). */
#ifndef MCUDA_PACK_H
#define MCUDA_PACK_H
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MCUDA_PACK_MINM 6
#define MCUDA_PACK_HDR 28

/* ---- the MPY form's decoder ---- */
#define MCUDA_MPY_MINM 3
#define MCUDA_MPY_MAXL 11
#define MCUDA_MPY_NLIT 256
#define MCUDA_MPY_NLEN 88
#define MCUDA_MPY_NOF 91

typedef struct {
    const unsigned char *p, *e; /* the next byte to load, the end */
    unsigned long long b;       /* bits, the next one lowest */
    int n;                      /* how many b holds */
    int bad;                    /* no code here */
    int pad;                    /* zero bits put in past the end (a stream that reads them is damaged) */
} mcuda_bits;

static inline void mcuda_bits_fill(mcuda_bits *s)
{
    if (s->p + 8 <= s->e) {
        unsigned long long v;
        int k = (63 - s->n) >> 3;
        memcpy(&v, s->p, 8);
        s->b |= v << s->n;
        s->p += k;
        s->n += k * 8;
        s->b &= s->n >= 64 ? ~0ull : (1ull << s->n) - 1;
    } else
        while (s->n <= 56) {
            if (s->p < s->e)
                s->b |= (unsigned long long)*s->p++ << s->n;
            else
                s->pad += 8;
            s->n += 8;
        }
}

static inline unsigned long long mcuda_bits_get(mcuda_bits *s, int k)
{
    unsigned long long v;
    if (k == 0)
        return 0;
    if (s->n < k)
        mcuda_bits_fill(s);
    v = s->b & ((1ull << k) - 1);
    s->b >>= k;
    s->n -= k;
    return v;
}

/* a decoding table: (symbol << 4 | length) by the next MAXL bits; 0 where no code is */
typedef unsigned short mcuda_htab[1 << MCUDA_MPY_MAXL];

/* the table of nsym code lengths len[]: -1 when they are not a prefix code */
static inline int mcuda_htab_make(mcuda_htab t, const unsigned char *len, int nsym)
{
    int cnt[MCUDA_MPY_MAXL + 1], next[MCUDA_MPY_MAXL + 1], s, l, code = 0, used = 0;
    memset(t, 0, sizeof(mcuda_htab));
    memset(cnt, 0, sizeof cnt);
    for (s = 0; s < nsym; s++)
        cnt[len[s]]++;
    cnt[0] = 0;
    for (l = 1; l <= MCUDA_MPY_MAXL; l++) {
        code = (code + cnt[l - 1]) << 1;
        next[l] = code;
        used += cnt[l] << (MCUDA_MPY_MAXL - l);
    }
    if (used > (1 << MCUDA_MPY_MAXL))
        return -1;
    for (s = 0; s < nsym; s++) {
        int c, r = 0, k, step;
        l = len[s];
        if (!l)
            continue;
        c = next[l]++;
        for (k = 0; k < l; k++) /* (the stream is LSB first: the code reversed) */
            r |= ((c >> k) & 1) << (l - 1 - k);
        for (step = 1 << l, k = r; k < (1 << MCUDA_MPY_MAXL); k += step)
            t[k] = (unsigned short)(s << 4 | l);
    }
    return 0;
}

static inline int mcuda_hget(mcuda_bits *s, const mcuda_htab t)
{
    unsigned e;
    if (s->n < MCUDA_MPY_MAXL)
        mcuda_bits_fill(s);
    e = t[s->b & ((1u << MCUDA_MPY_MAXL) - 1)];
    if (!e) {
        s->bad = 1;
        return 0;
    }
    s->b >>= e & 15;
    s->n -= (int)(e & 15);
    return (int)(e >> 4);
}

static inline unsigned long long mcuda_bucket_get(mcuda_bits *s, int code)
{
    int b, half;
    if (code < 16)
        return (unsigned long long)code;
    b = 4 + (code - 16) / 2;
    half = (code - 16) & 1;
    return 1ull << b | (unsigned long long)half << (b - 1) | mcuda_bits_get(s, b - 1);
}

/* the nibbles of nsym code lengths at *pp (< e), into len[]; -1 if short */
static inline int mcuda_mpy_lens(const unsigned char **pp, const unsigned char *e, unsigned char *len, int nsym)
{
    int k;
    const unsigned char *p = *pp;
    if ((size_t)(e - p) < (size_t)(nsym + 1) / 2)
        return -1;
    for (k = 0; k < nsym; k++)
        len[k] = (unsigned char)((p[k >> 1] >> ((k & 1) * 4)) & 15);
    *pp = p + (nsym + 1) / 2;
    for (k = 0; k < nsym; k++)
        if (len[k] > MCUDA_MPY_MAXL)
            return -1;
    return 0;
}

/* Decodes an MPY body (m bytes) into out[pl .. n) after the prologue out[0 .. pl) (already there). -1 on a damaged body. */
static inline int mcuda_mpy_body(const unsigned char *body, unsigned long long m, char *outc, unsigned long long pl,
                                   unsigned long long n)
{
    static const int nsym[4] = {MCUDA_MPY_NLIT, MCUDA_MPY_NLEN, MCUDA_MPY_NLEN, MCUDA_MPY_NOF};
    const unsigned char *p = body, *e = body + m;
    unsigned char *o = (unsigned char *)outc + pl, *oe = (unsigned char *)outc + n, *st = (unsigned char *)outc;
    unsigned char len[256];
    unsigned long long nseq = 0, rep[3] = {1, 2, 3};
    int s = 0, k;
    mcuda_bits B;
    mcuda_htab *T;
    int r = -1;

    while (p < e && (*p & 128) && s < 63)
        nseq |= (unsigned long long)(*p++ & 127) << s, s += 7;
    if (p >= e || s >= 63)
        return -1;
    nseq |= (unsigned long long)*p++ << s;
    if (!(T = (mcuda_htab *)malloc(4 * sizeof(mcuda_htab)))) /* (16 KB: not for a thread's stack) */
        return -1;
    for (k = 0; k < 4; k++)
        if (mcuda_mpy_lens(&p, e, len, nsym[k]) || mcuda_htab_make(T[k], len, nsym[k]))
            goto out;
    B.p = p, B.e = e, B.b = 0, B.n = 0, B.bad = 0, B.pad = 0;
    while (nseq--) {
        unsigned long long ll = mcuda_bucket_get(&B, mcuda_hget(&B, T[1])), ml, off;
        int oc;
        if (B.bad || ll > (unsigned long long)(oe - o))
            goto out;
        while (ll--)
            *o++ = (unsigned char)mcuda_hget(&B, T[0]);
        ml = mcuda_bucket_get(&B, mcuda_hget(&B, T[2])) + MCUDA_MPY_MINM;
        oc = mcuda_hget(&B, T[3]);
        if (oc < 3) {
            off = rep[oc];
            for (; oc > 0; oc--)
                rep[oc] = rep[oc - 1];
            rep[0] = off;
        } else {
            off = mcuda_bucket_get(&B, oc - 3);
            rep[2] = rep[1], rep[1] = rep[0], rep[0] = off;
        }
        if (B.bad || ml > (unsigned long long)(oe - o) || off == 0 || off > (unsigned long long)(o - st))
            goto out;
        {
            const unsigned char *f = o - off;
            if (off >= 8)
                while (ml >= 8) {
                    memcpy(o, f, 8);
                    o += 8, f += 8, ml -= 8;
                }
            while (ml--)
                *o++ = *f++;
        }
    }
    while (o < oe && !B.bad)
        *o++ = (unsigned char)mcuda_hget(&B, T[0]);
    if (!B.bad && B.n >= B.pad && o == oe && oe[-1] == 0)
        r = 0;
out:
    free(T);
    return r;
}

static inline int mcuda_packed(const char *p)
{
    return p && p[0] == 0 && p[1] == 'M' && p[2] == 'P' && (p[3] == 'Z' || p[3] == 'Y' || p[3] == 'F');
}

#define MCUDA_PACK_FNV1 0xcbf29ce484222325ull
#define MCUDA_PACK_FNV2 0x84222325cbf29ce4ull
static inline unsigned long long mcuda_pack_fnv(unsigned long long h, const unsigned char *p, unsigned long long n)
{
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
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
static inline int mcuda_unpack_mpz(const char *p, char *out);
static inline int mcuda_unpack_body(const char *p, char *out);

/* an MPF record's file name (0 for the other forms) */
static inline const char *mcuda_pack_file(const char *p)
{
    return p[3] == 'F' ? p + MCUDA_PACK_HDR + mcuda_pack_p(p) + 16 : 0;
}

/* the bodies of an MPF record from its file */
static inline int mcuda_unpack_file(const char *p, char *out)
{
    unsigned long long n = mcuda_pack_n(p), m = mcuda_pack_m(p), pl = mcuda_pack_p(p);
    const char *path = mcuda_pack_file(p), *dir = getenv("MCUDA_PTXDIR"), *base = strrchr(path, '/');
    const unsigned char *hs = (const unsigned char *)p + MCUDA_PACK_HDR + pl;
    char *buf = 0;
    FILE *f = 0;
    int r = -1;
    if (m < MCUDA_PACK_HDR + pl || m > (1ull << 40))
        goto out;
    if (dir && dir[0] && strlen(dir) < 4000 && strlen(base ? base : path) < 4000) {
        char alt[8200];
        sprintf(alt, "%s/%s", dir, base ? base + 1 : path);
        f = fopen(alt, "rb");
    }
    if (!f && !(f = fopen(path, "rb")))
        goto out;
    if (!(buf = (char *)malloc((size_t)m)) || fread(buf, 1, (size_t)m, f) != (size_t)m || fgetc(f) != EOF)
        goto out;
    if (mcuda_pack_fnv(MCUDA_PACK_FNV1, (const unsigned char *)buf, m) != mcuda_pack_rd8(hs) ||
        mcuda_pack_fnv(MCUDA_PACK_FNV2, (const unsigned char *)buf, m) != mcuda_pack_rd8(hs + 8))
        goto out;
    if (!mcuda_packed(buf) || buf[3] == 'F' || mcuda_pack_n(buf) != n || mcuda_pack_p(buf) != pl ||
        MCUDA_PACK_HDR + pl + mcuda_pack_m(buf) != m || memcmp(buf + MCUDA_PACK_HDR, p + MCUDA_PACK_HDR, (size_t)pl))
        goto out;
    r = mcuda_unpack_body(buf, out);
out:
    if (f)
        fclose(f);
    free(buf);
    if (r)
        out[pl] = 0;
    return r;
}

static inline int mcuda_unpack_body(const char *p, char *out)
{
    unsigned long long n = mcuda_pack_n(p), m = mcuda_pack_m(p), pl = mcuda_pack_p(p);
    if (p[3] == 'F')
        return mcuda_unpack_file(p, out);
    if (p[3] != 'Y')
        return mcuda_unpack_mpz(p, out);
    if (n < 1 || n > (1ull << 40) || pl >= n || m > (1ull << 40) ||
        mcuda_mpy_body((const unsigned char *)p + MCUDA_PACK_HDR + pl, m, out, pl, n)) {
        out[pl] = 0;
        return -1;
    }
    return 0;
}
static inline int mcuda_unpack_mpz(const char *p, char *out)
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
