// error: call to undeclared function 'strlen' (include <string.h>)
// flags: -std=c99
unsigned long g(const char *s) { return strlen(s); }
