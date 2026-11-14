#include <stdio.h>
#define P(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define Q(fmt, args...) printf(fmt, ## args)
#define R(...) printf(__VA_ARGS__)
#define CAT(a, b) a##b
#define ID(x) CAT(id_, x)
#define STR(...) #__VA_ARGS__
#define CNT __COUNTER__
// a line comment
int main(void)
{
    int ID(1) = CNT, ID(2) = CNT; /* 0, 1 */
    P("plain\n");
    P("%d %d\n", id_1, id_2);
    Q("q\n");
    Q("%s|%d\n", STR(a, b  c), 7);
    R("%d\n", __INCLUDE_LEVEL__);
    return 0;
}
