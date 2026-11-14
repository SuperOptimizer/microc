#define CHECK(x) if (!(x)) return __LINE__ % 200 + 1
struct P { int x, y; };
struct Big { long a[10]; char c; };
struct Node { int v; struct Node *next; };
union U { int i; char c[4]; long l; };
struct Bits { unsigned a : 3; int b : 5; unsigned c : 12; unsigned : 0; unsigned d : 1; };
struct Mix { char c; short s; int i; long l; char d; };
typedef struct { struct P p; int tag; } Wrap;
struct P gp = { 3, 4 };
struct Node n3 = { 3, 0 }, n2 = { 2, &n3 }, n1 = { 1, &n2 };
Wrap gw[2] = { { { 1, 2 }, 3 }, { { 4, 5 }, 6 } };
struct P mk(int x, int y)
{
    struct P p;
    p.x = x;
    p.y = y;
    return p;
}
int psum(struct P p) { return p.x + p.y; }
struct Big mkbig(int n)
{
    struct Big b;
    int i;
    for (i = 0; i < 10; i++)
        b.a[i] = n + i;
    b.c = 'z';
    return b;
}
long bigsum(struct Big b)
{
    long s = 0;
    int i;
    for (i = 0; i < 10; i++)
        s += b.a[i];
    b.a[0] = 999; /* must not affect the caller */
    return s;
}
int listsum(struct Node *n)
{
    int s = 0;
    for (; n; n = n->next)
        s += n->v;
    return s;
}
int main(void)
{
    struct P a, b, *pp = &a;
    struct Big big;
    union U u;
    struct Bits bits;
    Wrap w;
    struct P arr[3] = { { 1, 2 }, { 3, 4 } };
    a = mk(10, 20);
    b = a;
    b.x++;
    CHECK(a.x == 10 && b.x == 11 && b.y == 20);
    CHECK(psum(a) == 30 && psum(mk(1, 2)) == 3);
    CHECK(pp->x == 10 && (*pp).y == 20);
    pp->y = 7;
    CHECK(a.y == 7);
    big = mkbig(5);
    CHECK(big.a[9] == 14 && big.c == 'z');
    CHECK(bigsum(big) == 95 && big.a[0] == 5);
    CHECK(mkbig(1).a[3] == 4);
    CHECK(listsum(&n1) == 6);
    CHECK(gp.x == 3 && gp.y == 4);
    CHECK(gw[1].p.y == 5 && gw[0].tag == 3 && gw[1].tag == 6);
    u.l = 0;
    u.i = 0x01020304;
    CHECK(u.c[0] == 4 && u.c[3] == 1 && sizeof(u) == 8);
    bits.a = 5; bits.b = -3; bits.c = 4000; bits.d = 1;
    CHECK(bits.a == 5 && bits.b == -3 && bits.c == 4000 && bits.d == 1);
    bits.a = 9;
    CHECK(bits.a == 1 && bits.b == -3);
    bits.c++;
    CHECK(bits.c == 4001);
    CHECK(sizeof(struct Bits) == 8);
    CHECK(sizeof(struct Mix) == 24 && sizeof(struct P) == 8 && sizeof(struct Big) == 88);
    w.p = a;
    w.tag = 1;
    CHECK(w.p.x == 10);
    CHECK(arr[1].y == 4 && arr[2].x == 0 && arr[2].y == 0);
    {
        struct P c = a, d = mk(8, 9);
        CHECK(c.x == 10 && d.y == 9);
        c = d.x > 5 ? d : a;
        CHECK(c.x == 8);
    }
    return 0;
}
