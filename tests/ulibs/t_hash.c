#include "t.h"
#include <openssl/evp.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
static int sha_is(const void *d, size_t n, const char *want)
{
    unsigned char h[EVP_MAX_MD_SIZE]; unsigned hn = 0; char hx[2 * EVP_MAX_MD_SIZE + 1];
    if (!EVP_Digest(d, n, h, &hn, EVP_sha256(), NULL) || hn != 32) return 0;
    hex(h, 32, hx);
    return !strcmp(hx, want);
}
static int xxh_is(const void *d, size_t n, const char *want)
{
    XXH128_hash_t h = XXH3_128bits(d, n); char hx[40];
    sprintf(hx, "%016llx%016llx", (unsigned long long)h.high64, (unsigned long long)h.low64);
    return !strcmp(hx, want);
}
int main(void)
{
    size_t n, i, k; unsigned char *plain = slurp("data/plain.bin", &n), h[EVP_MAX_MD_SIZE]; unsigned hn; char hx[130];
    EVP_MD_CTX *ctx; static char a1000[1000];
    /* FIPS 180-4 / NIST CAVS vectors */
    CHECK("sha256-empty", sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK("sha256-abc", sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK("sha256-448bits", sha_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK("sha256-896bits", sha_is("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    CHECK("sha256-file", sha_is(plain, n, PLAIN_SHA256));
    memset(a1000, 'a', 1000); ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    for (i = 0; i < 1000; i++) EVP_DigestUpdate(ctx, a1000, 1000);
    hn = 0; EVP_DigestFinal_ex(ctx, h, &hn); hex(h, 32, hx);
    CHECK("sha256-million-a", hn == 32 && !strcmp(hx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    /* the file in uneven pieces, with the context reused */
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    for (i = 0, k = 1; i < n; i += k, k = k * 3 % 251 + 1) EVP_DigestUpdate(ctx, plain + i, i + k > n ? n - i : k);
    EVP_DigestFinal_ex(ctx, h, &hn); hex(h, 32, hx); EVP_MD_CTX_free(ctx);
    CHECK("sha256-pieces", !strcmp(hx, PLAIN_SHA256));
    EVP_Digest("abc", 3, h, &hn, EVP_sha224(), NULL); hex(h, 28, hx);
    CHECK("sha224-abc", hn == 28 && !strcmp(hx, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"));
    CHECK("xxh3-128-empty", xxh_is("", 0, "99aa06d3014798d86001c324468d497f"));
    CHECK("xxh3-128-9", xxh_is(plain, 9, HEAD9_XXH128));
    CHECK("xxh3-128-100", xxh_is(plain, 100, HEAD100_XXH128));
    CHECK("xxh3-128-300", xxh_is(plain, 300, HEAD300_XXH128));
    CHECK("xxh3-128-file", xxh_is(plain, n, PLAIN_XXH128));
    return t_fail != 0;
}
