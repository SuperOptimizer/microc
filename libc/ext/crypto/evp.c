/* evp.c: SHA-256 / SHA-224 (FIPS 180-4) behind the handful of OpenSSL EVP digest calls that ufsm makes.
   See openssl/evp.h. Installed by mkulibs.sh as libcrypto.a. */
#include <openssl/evp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct evp_md_st { int size; uint32_t iv[8]; };
struct evp_md_ctx_st {
    const EVP_MD *md;
    uint32_t h[8];
    uint64_t len;          /* bytes taken so far */
    unsigned char buf[64]; /* the len % 64 bytes not yet hashed */
};

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};
static const EVP_MD md256 = { 32, { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 } };
static const EVP_MD md224 = { 28, { 0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4 } };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t *h, const unsigned char *p)
{
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7], t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (ROR(x, 7) ^ ROR(x, 18) ^ (x >> 3)) + w[i - 7] + (ROR(y, 17) ^ ROR(y, 19) ^ (y >> 10));
    }
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

const EVP_MD *EVP_sha256(void) { return &md256; }
const EVP_MD *EVP_sha224(void) { return &md224; }
int EVP_MD_get_size(const EVP_MD *md) { return md ? md->size : -1; }
int EVP_MD_get_block_size(const EVP_MD *md) { return md ? 64 : -1; }

EVP_MD_CTX *EVP_MD_CTX_new(void) { return calloc(1, sizeof(EVP_MD_CTX)); }
void EVP_MD_CTX_free(EVP_MD_CTX *ctx) { free(ctx); }
int EVP_MD_CTX_reset(EVP_MD_CTX *ctx) { if (ctx) memset(ctx, 0, sizeof *ctx); return 1; }
int EVP_MD_CTX_copy_ex(EVP_MD_CTX *out, const EVP_MD_CTX *in) { if (!out || !in) return 0; *out = *in; return 1; }

int EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl)
{
    (void)impl;
    if (!ctx || (!type && !ctx->md))
        return 0;
    if (type)
        ctx->md = type;
    memcpy(ctx->h, ctx->md->iv, sizeof ctx->h);
    ctx->len = 0;
    return 1;
}
int EVP_DigestInit(EVP_MD_CTX *ctx, const EVP_MD *type) { EVP_MD_CTX_reset(ctx); return EVP_DigestInit_ex(ctx, type, 0); }

int EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt)
{
    const unsigned char *p = d;
    size_t have;
    if (!ctx || !ctx->md)
        return 0;
    have = (size_t)(ctx->len & 63);
    ctx->len += cnt;
    if (have) {
        size_t n = 64 - have;
        if (n > cnt)
            n = cnt;
        memcpy(ctx->buf + have, p, n);
        p += n; cnt -= n; have += n;
        if (have < 64)
            return 1;
        block(ctx->h, ctx->buf);
    }
    for (; cnt >= 64; p += 64, cnt -= 64)
        block(ctx->h, p);
    memcpy(ctx->buf, p, cnt);
    return 1;
}

int EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s)
{
    uint64_t bits;
    size_t have;
    int i;
    if (!ctx || !ctx->md)
        return 0;
    bits = ctx->len << 3;
    have = (size_t)(ctx->len & 63);
    ctx->buf[have++] = 0x80;
    if (have > 56) {
        memset(ctx->buf + have, 0, 64 - have);
        block(ctx->h, ctx->buf);
        have = 0;
    }
    memset(ctx->buf + have, 0, 56 - have);
    for (i = 0; i < 8; i++)
        ctx->buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    block(ctx->h, ctx->buf);
    for (i = 0; i < ctx->md->size; i++)
        md[i] = (unsigned char)(ctx->h[i >> 2] >> (24 - 8 * (i & 3)));
    if (s)
        *s = (unsigned)ctx->md->size;
    return 1;
}
int EVP_DigestFinal(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s) { return EVP_DigestFinal_ex(ctx, md, s); }

int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size, const EVP_MD *type, ENGINE *impl)
{
    EVP_MD_CTX c;
    c.md = 0;
    return EVP_DigestInit_ex(&c, type, impl) && EVP_DigestUpdate(&c, data, count) && EVP_DigestFinal_ex(&c, md, size);
}

unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md)
{
    static unsigned char own[32];
    if (!md)
        md = own;
    return EVP_Digest(d, n, md, 0, &md256, 0) ? md : 0;
}
