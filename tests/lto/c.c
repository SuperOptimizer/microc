/* an archive member: one function used, one not */
extern int counter;
int bump(int by) { counter += by; return counter; }
int unused_c(void) { return 7; }
