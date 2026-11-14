#include "t.h"
#include <setjmp.h>
#include <jpeglib.h>
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } jerr_t;
static void on_error(j_common_ptr c) { longjmp(((jerr_t *)c->err)->jb, 1); }
static unsigned char *ppm(const char *path, int *w, int *h, int *nc)
{
    size_t n; unsigned char *p = slurp(path, &n); int max, off = 0; char kind;
    sscanf((char *)p, "P%c %d %d %d%n", &kind, w, h, &max, &off);
    *nc = kind == '6' ? 3 : 1;
    return p + off + 1;
}
/* decode with the stdio source (as ufsm's jpeg_read_rgb does) or from memory */
static unsigned char *decode(const char *path, const unsigned char *mem, unsigned long ml, int *w, int *h, J_COLOR_SPACE cs)
{
    struct jpeg_decompress_struct ci; jerr_t je; FILE *f = path ? fopen(path, "rb") : NULL; unsigned char *volatile px = NULL; int nc;
    ci.err = jpeg_std_error(&je.pub); je.pub.error_exit = on_error;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); if (f) fclose(f); free(px); return NULL; }
    jpeg_create_decompress(&ci);
    if (f) jpeg_stdio_src(&ci, f); else jpeg_mem_src(&ci, (unsigned char *)mem, ml);
    jpeg_read_header(&ci, TRUE);
    ci.out_color_space = cs; ci.dct_method = JDCT_ISLOW; jpeg_start_decompress(&ci);
    *w = ci.output_width; *h = ci.output_height; nc = ci.output_components;
    px = malloc((size_t)*w * *h * nc);
    while (ci.output_scanline < ci.output_height) { JSAMPROW r = px + (size_t)ci.output_scanline * *w * nc; jpeg_read_scanlines(&ci, &r, 1); }
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci);
    if (f) fclose(f);
    return px;
}
static int encode(FILE *f, unsigned char **mem, unsigned long *ml, const unsigned char *px, int w, int h, int nc, int q)
{
    struct jpeg_compress_struct ci; struct jpeg_error_mgr je;
    ci.err = jpeg_std_error(&je); jpeg_create_compress(&ci);
    if (f) jpeg_stdio_dest(&ci, f); else jpeg_mem_dest(&ci, mem, ml);
    ci.image_width = w; ci.image_height = h; ci.input_components = nc; ci.in_color_space = nc == 3 ? JCS_RGB : JCS_GRAYSCALE;
    jpeg_set_defaults(&ci); jpeg_set_quality(&ci, q, TRUE);
    if (ci.dct_method != JDCT_ISLOW || ci.optimize_coding || ci.progressive_mode) return 1;
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) { JSAMPROW r = (JSAMPROW)(px + (size_t)ci.next_scanline * w * nc); jpeg_write_scanlines(&ci, &r, 1); }
    jpeg_finish_compress(&ci); jpeg_destroy_compress(&ci);
    return 0;
}
static int maxdiff(const unsigned char *a, const unsigned char *b, size_t n, double *mean)
{
    size_t i; int m = 0; double s = 0;
    for (i = 0; i < n; i++) { int d = abs(a[i] - b[i]); if (d > m) m = d; s += d; }
    *mean = s / n;
    return m;
}
int main(void)
{
    int w, h, nc, dw, dh, m; double mean; size_t jn, n;
    unsigned char *src = ppm("data/img.ppm", &w, &h, &nc), *ref, *dec, *jhost = slurp("data/img.jpg", &jn), *mem = NULL, *gray;
    unsigned long ml = 0; FILE *f;
    CHECK("jpeg-test-image", w == IMG_W && h == IMG_H && nc == 3);
    /* a file that the Mac's cjpeg made, against what the Mac's djpeg decoded from it */
    ref = ppm("data/img.dec.ppm", &dw, &dh, &nc); dec = decode("data/img.jpg", NULL, 0, &dw, &dh, JCS_RGB);
    CHECK("jpeg-decode-host-file", dec && dw == w && dh == h && maxdiff(dec, ref, (size_t)w * h * 3, &mean) == 0);
    printf("# decode vs source: max %d mean %.2f\n", maxdiff(dec, src, (size_t)w * h * 3, &mean), mean);
    CHECK("jpeg-decode-close-to-source", dec && maxdiff(dec, src, (size_t)w * h * 3, &mean) < 160 && mean < 4.0);
    ref = ppm("data/gray.dec.pgm", &dw, &dh, &nc); dec = decode("data/gray.jpg", NULL, 0, &dw, &dh, JCS_GRAYSCALE);
    CHECK("jpeg-decode-progressive-gray", dec && nc == 1 && dw == w && dh == h && maxdiff(dec, ref, (size_t)w * h, &mean) == 0);
    CHECK("jpeg-decode-truncated-fails-cleanly", decode(NULL, jhost, 100, &dw, &dh, JCS_RGB) == NULL);
    /* encode: to memory and to a file; the same bytes as the Mac's cjpeg -quality 90 (same library family, same tables) */
    CHECK("jpeg-encode-mem", !encode(NULL, &mem, &ml, src, w, h, 3, 90) && ml > 1000 && mem[0] == 0xff && mem[1] == 0xd8);
    CHECK("jpeg-encode-same-as-host", ml == jn && !memcmp(mem, jhost, jn));
    f = fopen("out/t.jpg", "wb");
    CHECK("jpeg-encode-file", f && !encode(f, NULL, NULL, src, w, h, 3, 90) && !fclose(f) && (dec = slurp("out/t.jpg", &n)) && n == ml && !memcmp(dec, mem, ml));
    dec = decode("out/t.jpg", NULL, 0, &dw, &dh, JCS_RGB); m = dec ? maxdiff(dec, src, (size_t)w * h * 3, &mean) : 999;
    printf("# roundtrip: max %d mean %.2f\n", m, mean);
    CHECK("jpeg-roundtrip-pixels", dw == w && dh == h && m < 160 && mean < 4.0);
    gray = malloc((size_t)w * h);
    for (n = 0; n < (size_t)w * h; n++) gray[n] = (unsigned char)((src[3 * n] + src[3 * n + 1] + src[3 * n + 2]) / 3);
    free(mem); mem = NULL; ml = 0;
    dec = !encode(NULL, &mem, &ml, gray, w, h, 1, 95) ? decode(NULL, mem, ml, &dw, &dh, JCS_GRAYSCALE) : NULL;
    m = dec ? maxdiff(dec, gray, (size_t)w * h, &mean) : 999;
    CHECK("jpeg-roundtrip-gray", m < 24 && mean < 2.0);
    return t_fail != 0;
}
