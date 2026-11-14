#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
double dbl(double x) { return x; }
float flt(float x) { return x; }
double poly(double x) { return 3.0 * x * x - 2.5 * x + 1; }
double mixed(int a, double b, long c, float d, char e) { return a + b + c + d + e; }
double many(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j)
{
    return a + b + c + d + e + f + g + h + i * 100 + j * 1000;
}
double gd = 2.5;
float gf = 1.5f;
double tab[] = { 1.0, 2, 3.5e2, -0.25 };
int main(void)
{
    double a = dbl(1.5), b = dbl(-0.25);
    float f = flt(2.5f);
    int i = 7;
    unsigned u = 3000000000u;
    long l = 1L << 40;
    CHECK(a + b == 1.25);
    CHECK(a - b == 1.75);
    CHECK(a * b == -0.375);
    CHECK(a / b == -6.0);
    CHECK(-a == -1.5);
    CHECK(a > b && b < a && a >= 1.5 && a <= 1.5 && a != b);
    CHECK(f * 2 == 5.0f);
    CHECK((double)f == 2.5);
    CHECK((int)a == 1 && (int)dbl(-2.7) == -2 && (int)dbl(2.7) == 2);
    CHECK((double)i == 7.0 && i / 2 == 3 && i / 2.0 == 3.5);
    CHECK((double)u == 3000000000.0);
    CHECK((unsigned)dbl(3000000000.0) == 3000000000u);
    CHECK((double)l == 1099511627776.0);
    CHECK((long)dbl(1e15) == 1000000000000000L);
    CHECK(poly(2.0) == 8.0);
    CHECK(mixed(1, 2.5, 3L, 4.5f, 5) == 16.0);
    CHECK(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 36 + 900 + 10000);
    CHECK(gd == 2.5 && gf == 1.5f && tab[2] == 350.0 && tab[3] == -0.25 && tab[1] == 2.0);
    a += 1; CHECK(a == 2.5);
    a *= 2; CHECK(a == 5.0);
    a /= 4; CHECK(a == 1.25);
    a -= 0.25; CHECK(a == 1.0);
    a++; CHECK(a == 2.0);
    f = (float)a; CHECK(f == 2.0f);
    i = 3; i *= 1.5; CHECK(i == 4);
    CHECK(!dbl(0.0) && dbl(0.5));
    CHECK((a ? 1 : 2) == 1);
    CHECK(1e10 > 1e9 && 1.0e-3 < 1.0e-2 && .5 == 0.5 && 5. == 5.0);
    CHECK(sizeof(float) == 4 && sizeof(double) == 8);
    return 0;
}
