#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
int id(int x) { return x; }
long lid(long x) { return x; }
unsigned uid(unsigned x) { return x; }
int main(void)
{
    int a = id(7), b = id(-3);
    unsigned u = uid(4000000000u);
    long l = lid(123456789012345L);
    signed char c = (signed char)id(200); /* (plain char is unsigned on aarch64 Linux) */
    unsigned char uc = (unsigned char)id(200);
    short s = (short)id(70000);
    CHECK(a + b == 4);
    CHECK(a - b == 10);
    CHECK(a * b == -21);
    CHECK(a / b == -2);
    CHECK(a % b == 1);
    CHECK(-a / 2 == -3);
    CHECK(-a % 2 == -1);
    CHECK((a & b) == 5);
    CHECK((a | b) == -1);
    CHECK((a ^ b) == -6);
    CHECK(~a == -8);
    CHECK(a << 3 == 56);
    CHECK(b >> 1 == -2);
    CHECK(u >> 1 == 2000000000u);
    CHECK(u / 3 == 1333333333u);
    CHECK(u % 7 == 4000000000u % 7);
    CHECK(u + u == 3705032704u);
    CHECK(u > 5);
    CHECK(-1 < 0);
    CHECK((unsigned)-1 > 0);
    CHECK(-1L < 0u + 0L);
    CHECK(l * 10 == 1234567890123450L);
    CHECK(l / 1000 == 123456789012L);
    CHECK(l % 1000 == 345);
    CHECK(l >> 20 == 117737568L);
    CHECK((int)l == -2045911175);
    CHECK(c == -56);
    CHECK(uc == 200);
    CHECK(s == 4464);
    CHECK((long)b == -3L);
    CHECK((unsigned long)(unsigned)b == 4294967293UL);
    CHECK((long)u == 4000000000L);
    CHECK(a > b && b < a && a >= 7 && a <= 7 && a != b && !(a == b));
    CHECK((a > 5 ? 1 : 2) == 1);
    CHECK((a < 5 ? 1 : 2) == 2);
    a += 5; CHECK(a == 12);
    a -= 2; CHECK(a == 10);
    a *= 3; CHECK(a == 30);
    a /= 4; CHECK(a == 7);
    a %= 4; CHECK(a == 3);
    a <<= 4; CHECK(a == 48);
    a >>= 2; CHECK(a == 12);
    a &= 10; CHECK(a == 8);
    a |= 3; CHECK(a == 11);
    a ^= 6; CHECK(a == 13);
    CHECK(a++ == 13 && a == 14);
    CHECK(++a == 15);
    CHECK(a-- == 15 && a == 14);
    CHECK(--a == 13);
    c = 127; c++; CHECK(c == -128);
    uc = 255; uc++; CHECK(uc == 0);
    CHECK(sizeof(char) == 1 && sizeof(short) == 2 && sizeof(int) == 4 && sizeof(long) == 8 && sizeof(void *) == 8);
    CHECK(0x7fffffff + id(0) == 2147483647);
    CHECK(id(1) << 31 == -2147483647 - 1);
    CHECK((id(5), id(6)) == 6);
    CHECK(lid(0x123456789abcdefL) == 0x123456789abcdefL);
    CHECK(lid(-1) == -1L && lid(0xffff0000ffffL) == 0xffff0000ffffL);
    CHECK((id(0xff00) & 0xff00) == 0xff00 && (id(0x12345) & 0xffff) == 0x2345 && (id(-1) & 0x7ffffff0) == 0x7ffffff0);
    return 0;
}
