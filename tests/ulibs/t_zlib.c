#include "t.h"
#include <zlib.h>
int main(void)
{
    size_t n, cn; unsigned char *plain = slurp("data/plain.bin", &n), *c, *out = malloc(PLAIN_LEN + 100);
    int lv; uLongf dl; z_stream st; char name[64]; gzFile g; int r;
    CHECK("zlib-version", !strcmp(zlibVersion(), ZLIB_VERSION));
    CHECK("zlib-crc32", crc32(crc32(0, Z_NULL, 0), plain, n) == PLAIN_CRC32);
    CHECK("zlib-adler32", adler32(adler32(0, Z_NULL, 0), plain, n) == PLAIN_ADLER32);
    for (lv = 0; lv <= 9; lv += 3) {
        uLongf bl = compressBound(n); unsigned char *b = malloc(bl);
        sprintf(name, "zlib-roundtrip-%d", lv);
        r = compress2(b, &bl, plain, n, lv); dl = PLAIN_LEN + 100;
        CHECK(name, r == Z_OK && uncompress(out, &dl, b, bl) == Z_OK && dl == n && !memcmp(out, plain, n) && (lv == 0 || bl < n / 2));
        free(b);
    }
    c = slurp("data/plain.zz", &cn); dl = PLAIN_LEN + 100; memset(out, 0, n);
    CHECK("zlib-uncompress-python", uncompress(out, &dl, c, cn) == Z_OK && dl == n && !memcmp(out, plain, n));
    c[cn / 2] ^= 0x10; dl = PLAIN_LEN + 100;
    CHECK("zlib-corrupt-detected", uncompress(out, &dl, c, cn) != Z_OK);
    /* gzip member through inflateInit2, fed in small pieces, as ufsm reads zarr gzip chunks */
    c = slurp("data/plain.gz", &cn); memset(&st, 0, sizeof st); memset(out, 0, n);
    r = inflateInit2(&st, 16 + MAX_WBITS);
    st.next_out = out; st.avail_out = PLAIN_LEN + 100;
    { size_t off = 0; while (r == Z_OK && off < cn) { size_t k = cn - off < 1000 ? cn - off : 1000; st.next_in = c + off; st.avail_in = k; r = inflate(&st, Z_NO_FLUSH); off += k - st.avail_in; } }
    CHECK("zlib-inflate-gzip", r == Z_STREAM_END && st.total_out == n && !memcmp(out, plain, n));
    inflateEnd(&st);
    c = slurp("data/plain.deflate", &cn); memset(&st, 0, sizeof st); memset(out, 0, n);
    r = inflateInit2(&st, -15); st.next_in = c; st.avail_in = cn; st.next_out = out; st.avail_out = PLAIN_LEN + 100;
    CHECK("zlib-inflate-raw", r == Z_OK && inflate(&st, Z_FINISH) == Z_STREAM_END && st.total_out == n && !memcmp(out, plain, n));
    inflateEnd(&st);
    memset(&st, 0, sizeof st); r = inflateInit(&st); c = slurp("data/plain.zz", &cn);
    st.next_in = c; st.avail_in = cn; st.next_out = out; st.avail_out = PLAIN_LEN + 100;
    CHECK("zlib-inflateInit", r == Z_OK && inflate(&st, Z_FINISH) == Z_STREAM_END && st.total_out == n && inflateEnd(&st) == Z_OK);
    g = gzopen("data/plain.gz", "rb"); memset(out, 0, n);
    CHECK("zlib-gzread", g && gzread(g, out, PLAIN_LEN + 100) == (int)n && !memcmp(out, plain, n) && gzclose(g) == Z_OK);
    g = gzopen("out/t.gz", "wb9");
    r = g && gzwrite(g, plain, n) == (int)n && gzprintf(g, "%s %d\n", "tail", 42) == 8 && gzclose(g) == Z_OK;
    g = gzopen("out/t.gz", "rb"); memset(out, 0, n + 8);
    CHECK("zlib-gzwrite", r && g && gzread(g, out, PLAIN_LEN + 100) == (int)n + 8 && !memcmp(out, plain, n) && !memcmp(out + n, "tail 42\n", 8) && gzclose(g) == Z_OK);
    return t_fail != 0;
}
