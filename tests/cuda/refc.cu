#include <stdio.h>
struct f2 { float x, y; };
static int &pick(int c, int &a, int &b) { return c ? a : b; }
int main(int argc, char **argv) {
    f2 a = {1, 2}, b = {3, 4};
    for (int p = 0; p < 2; p++) { f2 &d = p ? b : a; d.x += 10; d.y += 20; }
    int i = 1, j = 2, k = 3;
    int &r = argc > 5 ? i : j; r += 100;
    int &q = argc > 5 ? i : argc > 0 ? k : j; q += 1000;
    pick(argc, i, j) += 7;
    const int &c = argc ? i : 5;
    printf("%g %g %g %g %d %d %d %d\n", a.x, a.y, b.x, b.y, i, j, k, c);
    return 0;
}
