int counter;
static int helper(int x) { counter++; return x + x; }
int add(int a, int b) { return a + b; }
int twice(int x) { return helper(x); }
int unused_b(int x) { return x - 1; }
int (*fp)(int) = twice;
