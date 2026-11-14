/* gzip, gunzip, zcat: deflate with hash-chain matching and per-block Huffman codes,
   and the matching inflate. */
#include "box.h"

static unsigned long crc_table[256];

static unsigned long crc32_of(const unsigned char *p, size_t n)
{
    unsigned long c = 0xffffffffUL;
    size_t i;
    int k;
    if (!crc_table[1])
        for (i = 0; i < 256; i++) {
            unsigned long v = (unsigned long)i;
            for (k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320UL ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return (c ^ 0xffffffffUL) & 0xffffffffUL;
}

/* ---- deflate */

static Buf zout;
static unsigned long bitbuf;
static int bitcnt;

static void putbits(unsigned v, int n)
{
    bitbuf |= (unsigned long)v << bitcnt;
    bitcnt += n;
    while (bitcnt >= 8) {
        bputc(&zout, (int)(bitbuf & 255));
        bitbuf >>= 8;
        bitcnt -= 8;
    }
}

static const unsigned short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                             4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                              769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                              9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const unsigned char clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* Huffman code lengths for the frequencies, none longer than `limit` */
void box_huff_lengths(const unsigned *freq0, int n, int limit, unsigned char *len)
{
    unsigned freq[288];
    int parent[2 * 288], alive[2 * 288];
    unsigned long weight[2 * 288];
    int i, used, nodes, a, b, shift = 0;
    for (;;) {
        int maxlen = 0;
        used = 0;
        for (i = 0; i < n; i++) {
            freq[i] = freq0[i] ? (freq0[i] >> shift) + 1 : 0;
            len[i] = 0;
            if (freq[i])
                used++;
        }
        if (used == 0)
            return;
        if (used == 1) {
            for (i = 0; i < n; i++)
                if (freq[i])
                    len[i] = 1;
            return;
        }
        nodes = n;
        for (i = 0; i < n; i++) {
            weight[i] = freq[i];
            alive[i] = freq[i] != 0;
            parent[i] = -1;
        }
        for (;;) {
            a = b = -1;
            for (i = 0; i < nodes; i++) {
                if (!alive[i])
                    continue;
                if (a < 0 || weight[i] < weight[a]) {
                    b = a;
                    a = i;
                } else if (b < 0 || weight[i] < weight[b])
                    b = i;
            }
            if (b < 0)
                break;
            weight[nodes] = weight[a] + weight[b];
            alive[nodes] = 1;
            parent[nodes] = -1;
            alive[a] = alive[b] = 0;
            parent[a] = parent[b] = nodes;
            nodes++;
        }
        for (i = 0; i < n; i++) {
            int d = 0, p;
            if (!freq[i])
                continue;
            for (p = parent[i]; p >= 0; p = parent[p])
                d++;
            len[i] = (unsigned char)d;
            if (d > maxlen)
                maxlen = d;
        }
        if (maxlen <= limit)
            return;
        shift++; /* flatten the distribution and try again */
    }
}

static void huff_codes(const unsigned char *len, int n, unsigned short *code)
{
    int count[16], next[16], i, bits;
    for (i = 0; i < 16; i++)
        count[i] = 0;
    for (i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    next[0] = 0;
    for (bits = 1; bits < 16; bits++)
        next[bits] = (next[bits - 1] + count[bits - 1]) << 1;
    for (i = 0; i < n; i++) {
        unsigned c, r = 0;
        int k;
        if (!len[i])
            continue;
        c = (unsigned)next[len[i]]++;
        for (k = 0; k < len[i]; k++) {
            r = r << 1 | (c & 1);
            c >>= 1;
        }
        code[i] = (unsigned short)r;
    }
}

typedef struct Token {
    unsigned short dist, lit; /* dist 0: a literal; else lit is the length */
} Token;

static int len_code(int len)
{
    int c;
    for (c = 28; len < len_base[c]; c--)
        ;
    return c;
}

static int dist_code(int dist)
{
    int c;
    for (c = 29; dist < dist_base[c]; c--)
        ;
    return c;
}

static void write_block(const Token *t, int n, int last)
{
    unsigned lfreq[288], dfreq[30], cfreq[19];
    unsigned char llen[288], dlen[30], clen[19], all[288 + 30], rle[288 + 30], rlex[288 + 30];
    unsigned short lcode[288], dcode[30], ccode[19];
    int i, nl, nd, nrle = 0, nc, total;
    memset(lfreq, 0, sizeof lfreq);
    memset(dfreq, 0, sizeof dfreq);
    memset(cfreq, 0, sizeof cfreq);
    for (i = 0; i < n; i++) {
        if (t[i].dist) {
            lfreq[257 + len_code(t[i].lit)]++;
            dfreq[dist_code(t[i].dist)]++;
        } else
            lfreq[t[i].lit]++;
    }
    lfreq[256] = 1;
    box_huff_lengths(lfreq, 286, 15, llen);
    box_huff_lengths(dfreq, 30, 15, dlen);
    for (nl = 286; nl > 257 && !llen[nl - 1]; nl--)
        ;
    for (nd = 30; nd > 1 && !dlen[nd - 1]; nd--)
        ;
    memcpy(all, llen, (size_t)nl);
    memcpy(all + nl, dlen, (size_t)nd);
    total = nl + nd;
    for (i = 0; i < total;) {
        int run = 1, v = all[i];
        while (i + run < total && all[i + run] == v)
            run++;
        if (v == 0 && run >= 3) {
            if (run > 138)
                run = 138;
            rle[nrle] = (unsigned char)(run <= 10 ? 17 : 18);
            rlex[nrle++] = (unsigned char)(run <= 10 ? run - 3 : run - 11);
        } else if (run >= 4) {
            run--;
            if (run > 6)
                run = 6;
            rle[nrle] = (unsigned char)v;
            rlex[nrle++] = 0;
            rle[nrle] = 16;
            rlex[nrle++] = (unsigned char)(run - 3);
            run++;
        } else {
            run = 1;
            rle[nrle] = (unsigned char)v;
            rlex[nrle++] = 0;
        }
        i += run;
    }
    for (i = 0; i < nrle; i++)
        cfreq[rle[i]]++;
    box_huff_lengths(cfreq, 19, 7, clen);
    for (nc = 19; nc > 4 && !clen[clen_order[nc - 1]]; nc--)
        ;
    huff_codes(llen, 286, lcode);
    huff_codes(dlen, 30, dcode);
    huff_codes(clen, 19, ccode);
    putbits((unsigned)last, 1);
    putbits(2, 2);
    putbits((unsigned)(nl - 257), 5);
    putbits((unsigned)(nd - 1), 5);
    putbits((unsigned)(nc - 4), 4);
    for (i = 0; i < nc; i++)
        putbits(clen[clen_order[i]], 3);
    for (i = 0; i < nrle; i++) {
        putbits(ccode[rle[i]], clen[rle[i]]);
        if (rle[i] == 16)
            putbits(rlex[i], 2);
        else if (rle[i] == 17)
            putbits(rlex[i], 3);
        else if (rle[i] == 18)
            putbits(rlex[i], 7);
    }
    for (i = 0; i < n; i++) {
        if (t[i].dist) {
            int lc = len_code(t[i].lit), dc = dist_code(t[i].dist);
            putbits(lcode[257 + lc], llen[257 + lc]);
            if (len_extra[lc])
                putbits((unsigned)(t[i].lit - len_base[lc]), len_extra[lc]);
            putbits(dcode[dc], dlen[dc]);
            if (dist_extra[dc])
                putbits((unsigned)(t[i].dist - dist_base[dc]), dist_extra[dc]);
        } else
            putbits(lcode[t[i].lit], llen[t[i].lit]);
    }
    putbits(lcode[256], llen[256]);
}

#define WSIZE 32768
#define HSIZE 65536
#define BLOCK 65536

static void deflate(const unsigned char *in, size_t n, int level)
{
    static int head[HSIZE], prev[WSIZE];
    Token *tok = xmalloc(BLOCK * sizeof *tok);
    int ntok = 0, maxchain = level >= 8 ? 1024 : level >= 5 ? 128 : 16, i, final = 0;
    size_t pos = 0;
    for (i = 0; i < HSIZE; i++)
        head[i] = -1;
    bitbuf = 0;
    bitcnt = 0;
    while (pos < n) {
        int best = 0, bestdist = 0;
        if (pos + 3 <= n) {
            unsigned h = ((unsigned)in[pos] << 8 ^ (unsigned)in[pos + 1] << 4 ^ (unsigned)in[pos + 2] * 251u) & (HSIZE - 1);
            int cand = head[h], chain = maxchain;
            int maxlen = n - pos < 258 ? (int)(n - pos) : 258;
            while (cand >= 0 && pos - (size_t)cand <= WSIZE - 262 && chain-- > 0) {
                const unsigned char *a = in + pos, *b = in + cand;
                if (b[best] == a[best] && b[0] == a[0]) {
                    int l = 0;
                    while (l < maxlen && a[l] == b[l])
                        l++;
                    if (l > best) {
                        best = l;
                        bestdist = (int)(pos - (size_t)cand);
                        if (l == maxlen)
                            break;
                    }
                }
                if (prev[cand & (WSIZE - 1)] >= cand)
                    break;
                cand = prev[cand & (WSIZE - 1)];
            }
            prev[pos & (WSIZE - 1)] = head[h];
            head[h] = (int)pos;
        }
        if (best >= 3 && !(best == 3 && bestdist > 4096)) {
            int k;
            tok[ntok].dist = (unsigned short)bestdist;
            tok[ntok++].lit = (unsigned short)best;
            for (k = 1; k < best; k++) {
                size_t q = pos + (size_t)k;
                if (q + 3 <= n) {
                    unsigned h = ((unsigned)in[q] << 8 ^ (unsigned)in[q + 1] << 4 ^ (unsigned)in[q + 2] * 251u)
                                 & (HSIZE - 1);
                    prev[q & (WSIZE - 1)] = head[h];
                    head[h] = (int)q;
                }
            }
            pos += (size_t)best;
        } else {
            tok[ntok].dist = 0;
            tok[ntok++].lit = in[pos++];
        }
        if (ntok == BLOCK) {
            write_block(tok, ntok, pos >= n);
            ntok = 0;
            final = pos >= n;
        }
    }
    if (!final)
        write_block(tok, ntok, 1);
    if (bitcnt)
        putbits(0, 8 - bitcnt);
    free(tok);
}

/* ---- inflate */

static const unsigned char *zin;
static size_t zn, zpos;
static unsigned long ibuf;
static int icnt;

static void zfail(const char *msg)
{
    fflush(stdout);
    fprintf(stderr, "gzip: %s\n", msg);
    exit(1);
}

static unsigned getbits(int n)
{
    unsigned v;
    while (icnt < n) {
        if (zpos >= zn)
            zfail("unexpected end of data");
        ibuf |= (unsigned long)zin[zpos++] << icnt;
        icnt += 8;
    }
    v = (unsigned)(ibuf & ((1ul << n) - 1));
    ibuf >>= n;
    icnt -= n;
    return v;
}

typedef struct Huff {
    short count[16], symbol[288];
} Huff;

static void huff_build(Huff *h, const unsigned char *len, int n)
{
    short offs[16];
    int i;
    for (i = 0; i < 16; i++)
        h->count[i] = 0;
    for (i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (i = 1; i < 15; i++)
        offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (short)i;
}

static int huff_decode(const Huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int count = h->count[len];
        code |= (int)getbits(1);
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    zfail("bad compressed data");
    return 0;
}

static void inflate(Buf *out)
{
    int last, type, i;
    ibuf = 0;
    icnt = 0;
    do {
        last = (int)getbits(1);
        type = (int)getbits(2);
        if (type == 0) {
            unsigned len;
            ibuf = 0;
            icnt = 0;
            if (zpos + 4 > zn)
                zfail("unexpected end of data");
            len = zin[zpos] | (unsigned)zin[zpos + 1] << 8;
            zpos += 4;
            if (zpos + len > zn)
                zfail("unexpected end of data");
            bput(out, (const char *)zin + zpos, len);
            zpos += len;
        } else if (type == 1 || type == 2) {
            Huff lh, dh;
            unsigned char len[320];
            if (type == 1) {
                for (i = 0; i < 288; i++)
                    len[i] = (unsigned char)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                huff_build(&lh, len, 288);
                for (i = 0; i < 30; i++)
                    len[i] = 5;
                huff_build(&dh, len, 30);
            } else {
                int nl = (int)getbits(5) + 257, nd = (int)getbits(5) + 1, nc = (int)getbits(4) + 4;
                Huff ch;
                unsigned char cl[19];
                memset(cl, 0, sizeof cl);
                for (i = 0; i < nc; i++)
                    cl[clen_order[i]] = (unsigned char)getbits(3);
                huff_build(&ch, cl, 19);
                for (i = 0; i < nl + nd;) {
                    int sym = huff_decode(&ch), rep = 0, v = 0;
                    if (sym < 16) {
                        len[i++] = (unsigned char)sym;
                        continue;
                    }
                    if (sym == 16) {
                        if (!i)
                            zfail("bad compressed data");
                        v = len[i - 1];
                        rep = 3 + (int)getbits(2);
                    } else if (sym == 17)
                        rep = 3 + (int)getbits(3);
                    else
                        rep = 11 + (int)getbits(7);
                    if (i + rep > nl + nd)
                        zfail("bad compressed data");
                    while (rep--)
                        len[i++] = (unsigned char)v;
                }
                huff_build(&lh, len, nl);
                huff_build(&dh, len + nl, nd);
            }
            for (;;) {
                int sym = huff_decode(&lh);
                if (sym < 256)
                    bputc(out, sym);
                else if (sym == 256)
                    break;
                else {
                    int l, d, ds;
                    size_t from;
                    sym -= 257;
                    if (sym >= 29)
                        zfail("bad compressed data");
                    l = len_base[sym] + (int)getbits(len_extra[sym]);
                    ds = huff_decode(&dh);
                    if (ds >= 30)
                        zfail("bad compressed data");
                    d = dist_base[ds] + (int)getbits(dist_extra[ds]);
                    if ((size_t)d > out->n)
                        zfail("bad compressed data");
                    from = out->n - (size_t)d;
                    while (l--)
                        bputc(out, out->s[from++]);
                }
            }
        } else
            zfail("bad compressed data");
    } while (!last);
}

static void put32(Buf *b, unsigned long v)
{
    int i;
    for (i = 0; i < 4; i++)
        bputc(b, (int)(v >> (8 * i) & 255));
}

static int gz_stream(int in, int out, int decompress, int level)
{
    size_t n, off = 0;
    unsigned char *data = (unsigned char *)readall(in, &n);
    Buf o;
    memset(&o, 0, sizeof o);
    if (decompress) {
        int flags;
        if (n < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8)
            zfail("not in gzip format");
        flags = data[3];
        zpos = 10;
        if (flags & 4)
            zpos += 2 + (data[zpos] | (size_t)data[zpos + 1] << 8);
        if (flags & 8)
            while (zpos < n && data[zpos++])
                ;
        if (flags & 16)
            while (zpos < n && data[zpos++])
                ;
        if (flags & 2)
            zpos += 2;
        zin = data;
        zn = n;
        inflate(&o);
    } else {
        memset(&zout, 0, sizeof zout);
        bputc(&zout, 0x1f);
        bputc(&zout, 0x8b);
        bputc(&zout, 8);
        bputc(&zout, 0);
        put32(&zout, 0);
        bputc(&zout, level >= 9 ? 2 : level <= 1 ? 4 : 0);
        bputc(&zout, 3);
        deflate(data, n, level);
        put32(&zout, crc32_of(data, n));
        put32(&zout, (unsigned long)(n & 0xffffffffUL));
        o = zout;
    }
    while (off < o.n) {
        long w = write(out, o.s + off, o.n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        off += (size_t)w;
    }
    free(o.s);
    free(data);
    return 0;
}

int gzip_main(int argc, char **argv)
{
    int i, decompress = 0, tostdout = 0, keep = 0, level = 6, st = 0, nfiles = 0;
    if (!strcmp(argv[0], "gunzip"))
        decompress = 1;
    if (!strcmp(argv[0], "zcat"))
        decompress = tostdout = 1;
    for (i = 1; i < argc; i++) {
        const char *p;
        if (argv[i][0] != '-' || !argv[i][1]) {
            nfiles++;
            continue;
        }
        if (argv[i][1] == '-')
            continue;
        for (p = argv[i] + 1; *p; p++) {
            if (*p == 'd')
                decompress = 1;
            else if (*p == 'c')
                tostdout = 1;
            else if (*p == 'k')
                keep = 1;
            else if (isdigit((unsigned char)*p))
                level = *p - '0';
        }
    }
    fflush(stdout);
    if (!nfiles)
        return gz_stream(0, 1, decompress, level);
    for (i = 1; i < argc; i++) {
        const char *name = argv[i];
        char *oname;
        int in, out;
        size_t n;
        struct stat sb;
        if (name[0] == '-' && name[1])
            continue;
        if (!strcmp(name, "-")) {
            st |= gz_stream(0, 1, decompress, level);
            continue;
        }
        in = open(name, O_RDONLY);
        if (in < 0 || fstat(in, &sb) < 0) {
            warn("%s: %s", name, strerror(errno));
            st = 1;
            continue;
        }
        if (tostdout) {
            st |= gz_stream(in, 1, decompress, level);
            close(in);
            continue;
        }
        n = strlen(name);
        oname = xmalloc(n + 4);
        strcpy(oname, name);
        if (decompress) {
            if (n < 4 || strcmp(name + n - 3, ".gz")) {
                warn("%s: unknown suffix", name);
                st = 1;
                close(in);
                continue;
            }
            oname[n - 3] = 0;
        } else
            strcat(oname, ".gz");
        out = open(oname, O_WRONLY | O_CREAT | O_TRUNC, sb.st_mode & 0777);
        if (out < 0) {
            warn("%s: %s", oname, strerror(errno));
            st = 1;
            close(in);
            continue;
        }
        if (gz_stream(in, out, decompress, level))
            st = 1;
        else if (!keep)
            unlink(name);
        close(in);
        close(out);
    }
    return st;
}
