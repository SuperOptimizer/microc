#include <stdio.h>
#include <stdlib.h>
void st(int *g, int *t, int w, int h)
{
    int x, y;
        for (y = 1; y < h - 1; y++)
            for (x = 1; x < w - 1; x++)
                t[y * w + x] = (g[y * w + x] * 4 + g[y * w + x - 1] + g[y * w + x + 1]
                                + g[(y - 1) * w + x] + g[(y + 1) * w + x]) >> 3;
}
void st2(unsigned char *g, unsigned char *t, long w, long h)
{
    long x, y;
        for (y = 1; y < h - 1; y++)
            for (x = 1; x < w - 1; x++)
                t[y * w + x] = (unsigned char)((g[y * w + x] * 4 + g[y * w + x - 1] + g[y * w + x + 1]) >> 3);
}
int main(void)
{
    int w, h, k; unsigned long s = 0;
    for (w = 1; w < 40; w++) for (h = 1; h < 7; h++) for (k = 0; k < 3; k++) {
        int n = w * h + 64, i;
        int *a = malloc(n * 4 * 2), *b = k == 0 ? a + n : k == 1 ? a + 1 : a;
        unsigned char *c = malloc(n * 2), *d = k == 0 ? c + n : k == 1 ? c + 1 : c + w;
        for (i = 0; i < n * 2; i++) a[i] = i * 7 % 101, c[i] = i * 13;
        st(a, b, w, h); st2(c, d, w, h);
        for (i = 0; i < n * 2; i++) s = s * 31 + a[i] + c[i];
        free(a); free(c);
    }
    printf("%lu\n", s);
    return 0;
}
