// ok
// flags: -std=gnu89
f(a, b) int a; { return a + b; }
static x = 3;
int g(void) { register r = 2; return f(r, x); }
