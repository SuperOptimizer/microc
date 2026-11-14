#include <stdio.h>
int add(int a, int b);
int twice(int x);
int bump(int by);
extern int (*fp)(int);
extern int counter;
static int helper(int x) { return x * 3; }
int unused_a(int x) { return helper(x) + 1; }
int main(void)
{
    int i, s = 0;
    for (i = 0; i < 10; i++)
        s = add(s, twice(i));
    printf("%d %d %d\n", s, counter, helper(2));
    printf("%d %d\n", bump(5), fp(4));
    return 0;
}
