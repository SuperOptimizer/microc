#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int collatz(int n)
{
    int steps = 0;
    while (n != 1) {
        if (n % 2)
            n = 3 * n + 1;
        else
            n /= 2;
        steps++;
    }
    return steps;
}
int sw(int x)
{
    switch (x) {
    case 1: return 10;
    case 2:
    case 3: return 20;
    case 10: x++;
    case 11: return x;
    case -5: return 50;
    case 100: case 200: case 300: case 400: case 500: return x / 100;
    default: return -1;
    }
}
int gt(int n)
{
    int i = 0, s = 0;
again:
    if (i >= n)
        goto done;
    s += i;
    i++;
    goto again;
done:
    return s;
}
int main(void)
{
    int i, j, s = 0;
    CHECK(fib(15) == 610);
    CHECK(collatz(27) == 111);
    CHECK(sw(1) == 10 && sw(2) == 20 && sw(3) == 20 && sw(10) == 11 && sw(11) == 11);
    CHECK(sw(-5) == 50 && sw(300) == 3 && sw(500) == 5 && sw(7) == -1 && sw(501) == -1);
    CHECK(gt(10) == 45);
    for (i = 0; i < 10; i++) {
        if (i == 3)
            continue;
        if (i == 8)
            break;
        s += i;
    }
    CHECK(s == 0 + 1 + 2 + 4 + 5 + 6 + 7);
    s = 0;
    for (i = 0; i < 5; i++)
        for (j = 0; j < 5; j++) {
            if (j > i)
                break;
            s += j;
        }
    CHECK(s == 20);
    i = 0;
    do {
        i += 3;
    } while (i < 10);
    CHECK(i == 12);
    s = 0;
    for (;;) {
        if (++s == 100)
            break;
    }
    CHECK(s == 100);
    i = 5;
    CHECK((i > 3 && i < 10) == 1);
    CHECK((i > 30 || i < 10) == 1);
    CHECK((i > 30 || i < 1) == 0);
    j = 0;
    if (i == 5 || (j = 1))
        ;
    CHECK(j == 0);
    if (i == 6 && (j = 1))
        ;
    CHECK(j == 0);
    s = 0;
    switch (i) {
        int k;
    case 5:
        k = 2;
        s += k;
    case 6:
        s += 10;
        break;
    case 7:
        s += 100;
    }
    CHECK(s == 12);
    return 0;
}
