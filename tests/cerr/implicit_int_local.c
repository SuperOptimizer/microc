// error: type specifier missing, defaults to 'int'
// flags: -std=c11
int g(void) { register r = 2; const c = 3; return r + c; }
