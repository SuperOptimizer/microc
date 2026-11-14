#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
enum Color { RED, GREEN = 5, BLUE, LAST = BLUE + 10 };
typedef unsigned long size_t_;
typedef int (*binop)(int, int);
static int counter(void)
{
    static int n = 100;
    return n++;
}
int vsum(int n, ...)
{
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n--)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}
double vmix(const char *fmt, ...)
{
    va_list ap;
    double s = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt == 'i')
            s += va_arg(ap, int);
        else if (*fmt == 'l')
            s += va_arg(ap, long);
        else if (*fmt == 'd')
            s += va_arg(ap, double);
        else if (*fmt == 's')
            s += va_arg(ap, char *)[0];
    }
    va_end(ap);
    return s;
}
int many(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, long k, char l)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + (int)k + l;
}
int old(a, b, c)
int a;
char c;
long b;
{
    return a + (int)b + c;
}
int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int tentative;
int tentative;
extern int defined_later;
static int fwd(int);
int use_fwd(int x) { return fwd(x) + defined_later; }
static int fwd(int x) { return x * 3; }
int defined_later = 4;
int garr[3] = { 1 };
char gstr[10] = "ab";
int main(void)
{
    binop ops[2];
    int i, s;
    int la[5] = { 1, 2 };
    char ls[] = { 'x', 'y', 0 };
    int z[2][2] = { 1, 2, 3, 4 };
    CHECK(RED == 0 && GREEN == 5 && BLUE == 6 && LAST == 16);
    CHECK(counter() == 100 && counter() == 101 && counter() == 102);
    CHECK(vsum(0) == 0 && vsum(3, 1, 2, 3) == 6 && vsum(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);
    CHECK(vmix("idls", 1, 2.5, 3L, "A") == 71.5);
    CHECK(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11L, 12) == 385 + 23);
    CHECK(old(1, 2L, 3) == 6);
    ops[0] = add;
    ops[1] = mul;
    s = 0;
    for (i = 0; i < 2; i++)
        s += ops[i](3, 4);
    CHECK(s == 19);
    CHECK(tentative == 0);
    CHECK(use_fwd(5) == 19);
    CHECK(garr[0] == 1 && garr[1] == 0 && garr[2] == 0);
    CHECK(gstr[0] == 'a' && gstr[2] == 0 && gstr[9] == 0 && sizeof gstr == 10);
    CHECK(la[0] == 1 && la[1] == 2 && la[2] == 0 && la[4] == 0);
    CHECK(ls[1] == 'y' && sizeof ls == 3);
    CHECK(z[1][0] == 3 && z[1][1] == 4);
    CHECK(sizeof(size_t_) == 8 && sizeof(enum Color) == 4);
    CHECK('\n' == 10 && '\0' == 0 && '\x41' == 'A' && '\101' == 'A' && 'ab' == 24930);
    CHECK(sizeof(int[3]) == 12 && sizeof(struct { char a; int b; }) == 8);
    {
        int x = 1;
        {
            int x = 2;
            CHECK(x == 2);
        }
        CHECK(x == 1);
    }
    CHECK(undeclared(4) == 16);
    return 0;
}
int undeclared(int x) { return x * x; }
