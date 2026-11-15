_Thread_local int b_tls = 3;
int b_shared = 17;
static const char *names[] = {"lto", "glibc"};
static int helper(int x) { return x + x; }
int b_twice(int x) { return helper(x); }
const char *b_name(void) { return names[b_shared & 1]; }
/* weak here, strong in c.c (an ordinary object): the system's linker must take c.c's, so -flto may not inline this one */
__attribute__((weak)) int b_pick(void) { return 1; }
int b_picked(void) { return b_pick(); }
