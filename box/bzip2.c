/* bzip2: compression only. Run-length coding, the Burrows-Wheeler transform by prefix
   doubling, move-to-front, and up to six Huffman tables chosen per group of 50 symbols. */
#include "box.h"

static Buf bzout;
static unsigned long bzbits;
static int bzcount;

static void bw(int n, unsigned long v)
{
    while (n > 0) {
        int take = n > 8 ? 8 : n;
        bzbits = bzbits << take | ((v >> (n - take)) & ((1ul << take) - 1));
        bzcount += take;
        n -= take;
        while (bzcount >= 8) {
            bputc(&bzout, (int)(bzbits >> (bzcount - 8) & 255));
            bzcount -= 8;
        }
    }
}

static unsigned long bzcrc_table[256];

static unsigned long bzcrc(const unsigned char *p, size_t n)
{
    unsigned long c = 0xffffffffUL;
    size_t i;
    int k;
    if (!bzcrc_table[1])
        for (i = 0; i < 256; i++) {
            unsigned long v = (unsigned long)i << 24;
            for (k = 0; k < 8; k++)
                v = (v & 0x80000000UL ? (v << 1) ^ 0x04c11db7UL : v << 1) & 0xffffffffUL;
            bzcrc_table[i] = v;
        }
    for (i = 0; i < n; i++)
        c = ((c << 8) ^ bzcrc_table[((c >> 24) ^ p[i]) & 255]) & 0xffffffffUL;
    return ~c & 0xffffffffUL;
}

/* sorting the rotations of the block: after each pass the ranks order them by
   twice as many leading bytes */
static int *brank, *btmp;
static int bh, bn;

static int rot_cmp(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb, ra, rb;
    if (brank[a] != brank[b])
        return brank[a] < brank[b] ? -1 : 1;
    ra = brank[a + bh < bn ? a + bh : a + bh - bn];
    rb = brank[b + bh < bn ? b + bh : b + bh - bn];
    if (ra != rb)
        return ra < rb ? -1 : 1;
    return 0;
}

static void sort_rotations(const unsigned char *s, int n, int *sa)
{
    int i, h, count[257], pos[256];
    brank = xmalloc((size_t)n * sizeof *brank);
    btmp = xmalloc((size_t)n * sizeof *btmp);
    bn = n;
    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++)
        count[s[i] + 1]++;
    for (i = 0; i < 256; i++) {
        count[i + 1] += count[i];
        pos[i] = count[i];
    }
    for (i = 0; i < n; i++)
        sa[pos[s[i]]++] = i;
    for (i = 0; i < n; i++)
        brank[i] = count[s[i]];
    for (h = 1; h < n; h *= 2) {
        int start, distinct = 1;
        bh = h;
        for (start = 0; start < n;) {
            int end = start + 1;
            while (end < n && brank[sa[end]] == brank[sa[start]])
                end++;
            if (end - start > 1) {
                qsort(sa + start, (size_t)(end - start), sizeof *sa, rot_cmp);
                distinct = 0;
            }
            start = end;
        }
        if (distinct)
            break;
        btmp[sa[0]] = 0;
        for (i = 1; i < n; i++)
            btmp[sa[i]] = rot_cmp(&sa[i - 1], &sa[i]) ? i : btmp[sa[i - 1]];
        memcpy(brank, btmp, (size_t)n * sizeof *brank);
    }
    free(brank);
    free(btmp);
}

#define MAXGROUPS 6
#define GSIZE 50

static void assign_codes(const unsigned char *len, int n, unsigned *code)
{
    int l, i;
    unsigned v = 0;
    for (l = 1; l <= 20; l++) {
        for (i = 0; i < n; i++)
            if (len[i] == l)
                code[i] = v++;
        v <<= 1;
    }
}

static void compress_block(const unsigned char *blk, int n, unsigned long crc)
{
    int *sa = xmalloc((size_t)n * sizeof *sa);
    unsigned char *bwt = xmalloc((size_t)n), yy[256], unseq[256];
    unsigned short *mtfv = xmalloc(((size_t)n + 2) * sizeof *mtfv);
    unsigned char len[MAXGROUPS][258], *sel;
    unsigned code[MAXGROUPS][258], freq[258], rfreq[MAXGROUPS][258];
    int inuse[256], ninuse = 0, i, j, t, orig = 0, nmtf = 0, zpend = 0, eob, alpha, ngroups, nsel, iter;
    sort_rotations(blk, n, sa);
    for (i = 0; i < n; i++) {
        if (sa[i] == 0)
            orig = i;
        bwt[i] = blk[sa[i] ? sa[i] - 1 : n - 1];
    }
    memset(inuse, 0, sizeof inuse);
    for (i = 0; i < n; i++)
        inuse[blk[i]] = 1;
    for (i = 0; i < 256; i++)
        if (inuse[i])
            unseq[i] = (unsigned char)ninuse++;
    eob = ninuse + 1;
    alpha = ninuse + 2;
    memset(freq, 0, sizeof freq);
    for (i = 0; i < ninuse; i++)
        yy[i] = (unsigned char)i;
    for (i = 0; i <= n; i++) {
        int c = 0;
        if (i < n) {
            c = unseq[bwt[i]];
            for (j = 0; yy[j] != c; j++)
                ;
            if (j == 0) {
                zpend++;
                continue;
            }
        }
        if (zpend > 0) {
            zpend--;
            for (;;) {
                mtfv[nmtf] = (unsigned short)(zpend & 1);
                freq[mtfv[nmtf++]]++;
                if (zpend < 2)
                    break;
                zpend = (zpend - 2) / 2;
            }
            zpend = 0;
        }
        if (i == n)
            break;
        memmove(yy + 1, yy, (size_t)j);
        yy[0] = (unsigned char)c;
        mtfv[nmtf++] = (unsigned short)(j + 1);
        freq[j + 1]++;
    }
    mtfv[nmtf++] = (unsigned short)eob;
    freq[eob]++;
    ngroups = nmtf < 200 ? 2 : nmtf < 600 ? 3 : nmtf < 1200 ? 4 : nmtf < 2400 ? 5 : 6;
    nsel = (nmtf + GSIZE - 1) / GSIZE;
    sel = xmalloc((size_t)nsel + 1);
    /* start with tables that each favour a slice of the alphabet */
    {
        int remaining = nmtf, gs = 0, ge;
        for (t = ngroups; t > 0; t--) {
            int target = remaining / t, sum = 0;
            ge = gs - 1;
            while (sum < target && ge < alpha - 1)
                sum += (int)freq[++ge];
            if (ge > gs && t != ngroups && t != 1 && ((ngroups - t) % 2 == 1))
                sum -= (int)freq[ge--];
            for (i = 0; i < alpha; i++)
                len[t - 1][i] = (unsigned char)(i >= gs && i <= ge ? 0 : 15);
            gs = ge + 1;
            remaining -= sum;
        }
    }
    for (iter = 0; iter < 4; iter++) {
        memset(rfreq, 0, sizeof rfreq);
        for (i = 0, j = 0; i < nmtf; i += GSIZE, j++) {
            int end = i + GSIZE < nmtf ? i + GSIZE : nmtf, best = 0, k;
            unsigned long bestcost = ~0ul;
            for (t = 0; t < ngroups; t++) {
                unsigned long cost = 0;
                for (k = i; k < end; k++)
                    cost += len[t][mtfv[k]];
                if (cost < bestcost) {
                    bestcost = cost;
                    best = t;
                }
            }
            sel[j] = (unsigned char)best;
            for (k = i; k < end; k++)
                rfreq[best][mtfv[k]]++;
        }
        for (t = 0; t < ngroups; t++) {
            for (i = 0; i < alpha; i++)
                rfreq[t][i]++;
            box_huff_lengths(rfreq[t], alpha, 17, len[t]);
        }
    }
    for (t = 0; t < ngroups; t++)
        assign_codes(len[t], alpha, code[t]);
    /* the block */
    bw(24, 0x314159);
    bw(24, 0x265359);
    bw(32, crc);
    bw(1, 0);
    bw(24, (unsigned long)orig);
    for (i = 0; i < 16; i++) {
        int any = 0;
        for (j = 0; j < 16; j++)
            any |= inuse[i * 16 + j];
        bw(1, (unsigned long)any);
    }
    for (i = 0; i < 16; i++) {
        int any = 0;
        for (j = 0; j < 16; j++)
            any |= inuse[i * 16 + j];
        if (any)
            for (j = 0; j < 16; j++)
                bw(1, (unsigned long)inuse[i * 16 + j]);
    }
    bw(3, (unsigned long)ngroups);
    bw(15, (unsigned long)nsel);
    {
        unsigned char order[MAXGROUPS];
        for (t = 0; t < ngroups; t++)
            order[t] = (unsigned char)t;
        for (i = 0; i < nsel; i++) {
            for (j = 0; order[j] != sel[i]; j++)
                ;
            for (t = 0; t < j; t++)
                bw(1, 1);
            bw(1, 0);
            memmove(order + 1, order, (size_t)j);
            order[0] = sel[i];
        }
    }
    for (t = 0; t < ngroups; t++) {
        int cur = len[t][0];
        bw(5, (unsigned long)cur);
        for (i = 0; i < alpha; i++) {
            while (cur < len[t][i]) {
                bw(2, 2);
                cur++;
            }
            while (cur > len[t][i]) {
                bw(2, 3);
                cur--;
            }
            bw(1, 0);
        }
    }
    for (i = 0; i < nmtf; i++) {
        t = sel[i / GSIZE];
        bw(len[t][mtfv[i]], code[t][mtfv[i]]);
    }
    free(sel);
    free(mtfv);
    free(bwt);
    free(sa);
}

static void bz_compress(const unsigned char *in, size_t n, int level)
{
    size_t max = (size_t)level * 100000 - 19, pos = 0;
    unsigned char *blk = xmalloc(max + 8);
    unsigned long combined = 0;
    memset(&bzout, 0, sizeof bzout);
    bzbits = 0;
    bzcount = 0;
    bw(8, 'B');
    bw(8, 'Z');
    bw(8, 'h');
    bw(8, (unsigned long)('0' + level));
    while (pos < n) {
        size_t start = pos, bn_ = 0;
        unsigned long crc;
        while (pos < n && bn_ + 5 <= max) {
            size_t run = 1;
            int c = in[pos];
            while (run < 255 && pos + run < n && in[pos + run] == c)
                run++;
            if (run >= 4) {
                memset(blk + bn_, c, 4);
                blk[bn_ + 4] = (unsigned char)(run - 4);
                bn_ += 5;
            } else {
                memset(blk + bn_, c, run);
                bn_ += run;
            }
            pos += run;
        }
        crc = bzcrc(in + start, pos - start);
        combined = ((combined << 1 | combined >> 31) ^ crc) & 0xffffffffUL;
        compress_block(blk, (int)bn_, crc);
    }
    bw(24, 0x177245);
    bw(24, 0x385090);
    bw(32, combined);
    if (bzcount)
        bw(8 - bzcount, 0);
    free(blk);
}

int bzip2_main(int argc, char **argv)
{
    int i, level = 9, tostdout = 0, keep = 0, st = 0, nfiles = 0;
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
                die("decompression is not supported");
            else if (*p == 'c')
                tostdout = 1;
            else if (*p == 'k')
                keep = 1;
            else if (*p >= '1' && *p <= '9')
                level = *p - '0';
        }
    }
    fflush(stdout);
    for (i = nfiles ? 1 : 0; i < argc; i++) {
        const char *name = argv[i];
        unsigned char *data;
        size_t n, off = 0;
        int in = 0, out = 1;
        char *oname = 0;
        if (nfiles) {
            if (name[0] == '-' && name[1])
                continue;
            if (strcmp(name, "-") && (in = open(name, O_RDONLY)) < 0) {
                warn("%s: %s", name, strerror(errno));
                st = 1;
                continue;
            }
            if (in && !tostdout) {
                oname = xmalloc(strlen(name) + 5);
                sprintf(oname, "%s.bz2", name);
                out = open(oname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (out < 0) {
                    warn("%s: %s", oname, strerror(errno));
                    st = 1;
                    close(in);
                    continue;
                }
            }
        }
        data = (unsigned char *)readall(in, &n);
        bz_compress(data, n, level);
        while (off < bzout.n) {
            long w = write(out, bzout.s + off, bzout.n - off);
            if (w < 0 && errno != EINTR) {
                st = 1;
                break;
            }
            if (w > 0)
                off += (size_t)w;
        }
        free(bzout.s);
        free(data);
        if (in)
            close(in);
        if (oname) {
            close(out);
            if (!keep && !st)
                unlink(name);
        }
        if (!nfiles)
            break;
    }
    return st;
}
