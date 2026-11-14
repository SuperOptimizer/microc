/* _Generic taking its default inside a loop condition, which is read twice */
#include <stdio.h>
struct P { int x; };
struct I { const struct P *prog; };
#define ONCE(x) (*(const volatile __typeof__(_Generic((x), char: (char)0, default: (x))) *)&(x))
int main(void)
{
    struct P p[3] = {{1}, {2}, {3}};
    struct I it[4], *item = it;
    const struct P *prog;
    int n = 0, i;
    for (i = 0; i < 3; i++) it[i].prog = &p[i];
    it[3].prog = 0;
    while ((prog = ONCE(item->prog))) { n += prog->x; item++; }
    for (item = it; (prog = ({ ONCE(item->prog); })); item++) n += 10 * prog->x;
    printf("%d\n", n);
    return 0;
}
