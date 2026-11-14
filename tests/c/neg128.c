/* unary minus and complement of 128-bit integers act on both halves */
#include <stdio.h>
#define show(e) do { __int128 x_ = (e); printf("%llx %llx\n", (unsigned long long)(x_ >> 64), (unsigned long long)x_); } while (0)
int main(int argc, char **argv)
{
    unsigned __int128 w = (unsigned __int128)(argc + 6) << 100 | 12345;
    __int128 z = argc - 1, one = argc;
    (void)argv;
    show(-(__int128)w);
    show((__int128)-w);
    show(~(__int128)w);
    show(-z);
    show(~z);
    show(-one);
    show(-(one << 64));
    return 0;
}
