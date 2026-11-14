#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
int g[5] = { 1, 2, 3, 4, 5 };
int *gp = &g[2];
char *msg = "hello";
char arr[] = "world";
const char *names[] = { "zero", "one", "two" };
int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
int slen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (int)(p - s);
}
void swap(int *a, int *b)
{
    int t = *a;
    *a = *b;
    *b = t;
}
int sum(int *p, int n)
{
    int s = 0;
    while (n--)
        s += *p++;
    return s;
}
int apply(int (*f)(int *, int), int *p, int n) { return f(p, n); }
int add1(int x) { return x + 1; }
int twice(int x) { return x * 2; }
int main(void)
{
    int a = 3, b = 4, i;
    int loc[10];
    int *p = loc, **pp = &p;
    int (*fn[2])(int);
    char buf[8];
    long big[3];
    CHECK(*gp == 3 && gp[-1] == 2 && gp[1] == 4);
    CHECK(slen(msg) == 5 && slen(arr) == 5 && sizeof(arr) == 6);
    CHECK(names[2][1] == 'w' && slen(names[0]) == 4);
    CHECK(m[1][2] == 6 && m[0][1] == 2 && sizeof(m) == 24 && sizeof(m[0]) == 12);
    swap(&a, &b);
    CHECK(a == 4 && b == 3);
    for (i = 0; i < 10; i++)
        loc[i] = i * i;
    CHECK(sum(loc, 10) == 285);
    CHECK(apply(sum, g, 5) == 15);
    CHECK(*(p + 3) == 9 && p[4] == 16 && 5[p] == 25);
    CHECK(**pp == 0);
    (*pp)++;
    CHECK(*p == 1 && p - loc == 1);
    p += 3;
    CHECK(*p == 16);
    CHECK(&loc[9] - &loc[2] == 7);
    CHECK(p > loc && p < loc + 10 && p != 0);
    fn[0] = add1;
    fn[1] = &twice;
    CHECK(fn[0](5) == 6 && (*fn[1])(5) == 10);
    for (i = 0; i < 7; i++)
        buf[i] = (char)('a' + i);
    buf[7] = 0;
    CHECK(slen(buf) == 7 && buf[3] == 'd');
    big[0] = 1; big[1] = 2; big[2] = 0x100000000L;
    CHECK(big[2] / big[1] == 0x80000000L);
    CHECK(*"abc" == 'a' && "abc"[2] == 'c' && sizeof("abc") == 4);
    {
        char *s = arr;
        *s = 'W';
        s[4] = 'D';
        CHECK(arr[0] == 'W' && arr[4] == 'D');
    }
    {
        void *v = &a;
        int *q = v;
        CHECK(*q == 4);
        CHECK((char *)v + 4 == (char *)(&a + 1));
    }
    return 0;
}
