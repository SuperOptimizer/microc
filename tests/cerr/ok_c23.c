// ok
/* what C23 code does in place of the above: no false positives on auto, typeof, typedef names, _BitInt, unsigned alone,
   __builtin_ functions mcc knows, and functions declared after a block-scope declaration */
typedef long myint;
static unsigned u = 1;
int later(int);
int g(double d) {
    auto a = 3;
    typeof(a) b = 4;
    myint c = 5;
    signed s = 6;
    const long l = 7;
    extern int blockdecl(int);
    return (int)(a + b + c + s + l + u) + __builtin_popcount(9) + (int)__builtin_floor(d) + later(1) + blockdecl(2);
}
