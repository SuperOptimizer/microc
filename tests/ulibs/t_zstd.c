#include "t.h"
#include <zstd.h>
int main(void)
{
    size_t n, cn, r, bl; unsigned char *plain = slurp("data/plain.bin", &n), *c, *out = malloc(PLAIN_LEN + 100), *b;
    int lv; char name[64]; ZSTD_DCtx *dc; ZSTD_CCtx *cc;
    bl = ZSTD_compressBound(n); b = malloc(bl);
    CHECK("zstd-version", ZSTD_versionNumber() == ZSTD_VERSION_NUMBER);
    for (lv = 1; lv <= 22; lv += (lv < 4 ? 2 : lv < 19 ? 7 : 3)) {       /* 1 3 5 12 19 22: every strategy family */
        sprintf(name, "zstd-roundtrip-%d", lv); memset(out, 0, n);
        r = ZSTD_compress(b, bl, plain, n, lv);
        CHECK(name, !ZSTD_isError(r) && r < n * 2 / 3 && ZSTD_decompress(out, n, b, r) == n && !memcmp(out, plain, n));
    }
    c = slurp("data/plain.zst", &cn); memset(out, 0, n);
    CHECK("zstd-decompress-cli19", ZSTD_getFrameContentSize(c, cn) == n && ZSTD_decompress(out, PLAIN_LEN + 100, c, cn) == n && !memcmp(out, plain, n));
    c[cn / 3] ^= 1;
    r = ZSTD_decompress(out, PLAIN_LEN + 100, c, cn);
    CHECK("zstd-corrupt-detected", ZSTD_isError(r) && ZSTD_getErrorName(r)[0]);
    c = slurp("data/plain.1.zst", &cn); memset(out, 0, n); dc = ZSTD_createDCtx();
    CHECK("zstd-decompressDCtx-cli1", dc && ZSTD_decompressDCtx(dc, out, PLAIN_LEN + 100, c, cn) == n && !memcmp(out, plain, n));
    CHECK("zstd-short-output", ZSTD_isError(ZSTD_decompressDCtx(dc, out, 100, c, cn)));
    ZSTD_freeDCtx(dc);
    /* what ufsm's phase-pack does: a level, a request for worker threads, compress2 */
    cc = ZSTD_createCCtx();
    CHECK("zstd-set-level", cc && !ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 9)));
    CHECK("zstd-set-nbWorkers", !ZSTD_isError(ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, 4)));
    r = ZSTD_compress2(cc, b, bl, plain, n); memset(out, 0, n);
    CHECK("zstd-compress2", !ZSTD_isError(r) && ZSTD_decompress(out, n, b, r) == n && !memcmp(out, plain, n));
    ZSTD_freeCCtx(cc);
    return t_fail != 0;
}
