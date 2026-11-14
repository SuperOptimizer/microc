/* Linker test, compiled by the host compiler. No libc. */
extern int counter;
extern int table[];
extern const char *names[];
int add(int a, int b);
int lib_twice(int x);
int common_var;
static int zeros[1000];
static int *ptrs[3] = { &zeros[1], &counter, &table[2] };

static int length(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int main(void)
{
    int r;
    zeros[1] = 5;
    counter += 2;                                /* 7 + 2 = 9 */
    r = add(*ptrs[0], *ptrs[1]);                 /* 5 + 9 = 14 */
    r += *ptrs[2];                               /* + 30 = 44 */
    r += length(names[0]) - length(names[1]);    /* + 3 - 5 = 42 */
    r += lib_twice(common_var);                  /* + 0 */
    return r;
}
