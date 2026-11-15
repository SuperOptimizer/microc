// error: 'helper' undeclared
static int f(int x) { return helper(x) + 1; }
int g(int x) { return f(x); }
