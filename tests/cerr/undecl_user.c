// error: call to undeclared function 'helper'; ISO C99 and later do not support implicit function declarations
static int f(int x) { return helper(x) + 1; }
int g(int x) { return f(x); }
