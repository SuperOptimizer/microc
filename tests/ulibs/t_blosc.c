#include "t.h"
#include <blosc.h>
int main(void)
{
    static const char *names[] = { "lz4", "lz4hc", "zstd", "zlib", "blosclz", "raw" };
    static const char *comp[] = { "blosclz", "lz4", "lz4hc", "zlib", "zstd" };
    size_t n, cn, nb, cb, bs; unsigned char *plain = slurp("data/plain.bin", &n), *c, *out = malloc(PLAIN_LEN + 100), *b = malloc(PLAIN_LEN + BLOSC_MAX_OVERHEAD);
    int i, r; char name[64], path[64];
    for (i = 0; i < 6; i++) {            /* frames made by python-blosc on the Mac */
        sprintf(path, "data/plain.%s.blosc", names[i]); sprintf(name, "blosc-decompress-%s", names[i]);
        c = slurp(path, &cn); memset(out, 0, n);
        blosc_cbuffer_sizes(c, &nb, &cb, &bs);
        r = blosc_decompress_ctx(c, out, PLAIN_LEN, 1);
        CHECK(name, nb == n && cb == cn && r == (int)n && !memcmp(out, plain, n));
        free(c);
    }
    c = slurp("data/plain.zstd.blosc", &cn);
    CHECK("blosc-short-output", blosc_decompress_ctx(c, out, 1000, 1) < 0);
    CHECK("blosc-threads-asked", blosc_decompress_ctx(c, out, PLAIN_LEN, 4) == (int)n && !memcmp(out, plain, n));
    for (i = 0; i < 5; i++) {
        sprintf(name, "blosc-roundtrip-%s", comp[i]); memset(out, 0, n);
        r = blosc_compress_ctx(5, i & 1 ? BLOSC_SHUFFLE : BLOSC_BITSHUFFLE, 4, n, plain, b, PLAIN_LEN + BLOSC_MAX_OVERHEAD, comp[i], 0, 1);
        CHECK(name, r > 0 && r < (int)n && blosc_decompress_ctx(b, out, PLAIN_LEN, 1) == (int)n && !memcmp(out, plain, n));
    }
    /* many blocks (a frame of 4 KB blocks), decoded with one thread and with a request for four */
    r = blosc_compress_ctx(5, BLOSC_SHUFFLE, 4, n, plain, b, PLAIN_LEN + BLOSC_MAX_OVERHEAD, "zstd", 4096, 1);
    blosc_cbuffer_sizes(b, &nb, &cb, &bs); memset(out, 0, n);
    if (bs > 8192) printf("# blosc block size %zu\n", bs);
    CHECK("blosc-blocks", r > 0 && bs <= 8192 && blosc_decompress_ctx(b, out, PLAIN_LEN, 1) == (int)n && !memcmp(out, plain, n));
    memset(out, 0, n);
    CHECK("blosc-blocks-threads-asked", blosc_decompress_ctx(b, out, PLAIN_LEN, 4) == (int)n && !memcmp(out, plain, n));
    r = blosc_compress_ctx(5, BLOSC_SHUFFLE, 4, n, plain, b, PLAIN_LEN + BLOSC_MAX_OVERHEAD, "zstd", 4096, 4); memset(out, 0, n);
    CHECK("blosc-compress-threads-asked", r > 0 && blosc_decompress_ctx(b, out, PLAIN_LEN, 1) == (int)n && !memcmp(out, plain, n));
    blosc_init();
    r = blosc_compress(9, BLOSC_SHUFFLE, 2, n, plain, b, PLAIN_LEN + BLOSC_MAX_OVERHEAD); memset(out, 0, n);
    CHECK("blosc-global", r > 0 && blosc_decompress(b, out, PLAIN_LEN) == (int)n && !memcmp(out, plain, n));
    blosc_destroy();
    return t_fail != 0;
}
