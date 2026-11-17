/* a static object whose address the unit never hands out is not what a pointer points to (x86-64: not read again
   after a store through one); one whose address is taken anywhere in the unit - after the function that reads it,
   in an initialiser, by an array's name - is, and must be read again */
#include <stdio.h>
#include <string.h>

static int n_priv, late, in_init, arr[4];
static unsigned char *buf_priv;
static int *p_init = &in_init;
static unsigned char store[64];

/* reads late, in_init and arr[1] each trip: the bytes written may be theirs */
static int sum_through(unsigned char *w, int n)
{
    int k, s = 0;
    for (k = 0; k < n; k++) {
        w[k] = (unsigned char)(k + 1);
        s += late + in_init + arr[1] + n_priv;
    }
    return s;
}

static int fill_priv(void)
{
    int k, s = 0;
    for (k = 0; k < n_priv; k++) {
        buf_priv[k] = (unsigned char)(k * 3);
        s += buf_priv[k] + n_priv;
    }
    return s;
}

/* the addresses are taken only here, below their readers */
static unsigned char *addr_late(void) { return (unsigned char *)&late; }
static unsigned char *addr_arr(int i) { return (unsigned char *)(arr + i); }

int main(void)
{
    volatile int four = 4;
    n_priv = 9;
    buf_priv = store;
    late = 0, in_init = 0, arr[1] = 0;
    printf("%d\n", sum_through(store + 16, four));
    printf("%d\n", sum_through(addr_late(), four));
    late = 0;
    printf("%d\n", sum_through((unsigned char *)p_init, four));
    in_init = 0;
    printf("%d\n", sum_through(addr_arr(1), four));
    printf("%d %d\n", fill_priv(), store[8]);
    /* the private pointer's own bytes are not what it points to, but what it points to may hold n_priv's value */
    memset(store, 0, sizeof store);
    n_priv = 5;
    printf("%d\n", fill_priv());
    return 0;
}
