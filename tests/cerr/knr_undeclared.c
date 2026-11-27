// error: parameter 'b' was not declared, defaults to 'int'
// flags: -std=c17
int f(a, b) int a; { return a + b; }
