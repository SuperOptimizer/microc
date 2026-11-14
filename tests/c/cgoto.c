/* GNU computed goto: a static table of label addresses, goto *, and labels whose address is taken later */
#include <stdio.h>

static int run(const unsigned char *code, int x)
{
    static const void *const table[] = { &&op_add, &&op_dbl, &&op_neg, &&op_end, [4 ... 6] = &&op_bad };
    int steps = 0;
next:
    steps++;
    goto *table[*code++];
op_add:
    x += 3;
    goto next;
op_dbl:
    x *= 2;
    goto next;
op_neg:
    x = -x;
    goto next;
op_bad:
    return -1000 - steps;
op_end:
    return x * 100 + steps;
}

static int later(int n)
{
    void *p, __attribute__((unused)) *q;
    int r = 0;
    p = n & 1 ? &&odd : &&even;
    goto *p;
odd:
    r += 1;
    p = &&done;
    goto *p;
even:
    r += 2;
done:
    return r + n;
}

int main(void)
{
    static const unsigned char a[] = { 0, 1, 0, 2, 1, 3 }, b[] = { 1, 5, 3 };
    printf("%d %d %d %d\n", run(a, 1), run(b, 7), later(4), later(7));
    return 0;
}
