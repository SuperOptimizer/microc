/* loads and stores whose offset does not fit the instruction, with the
   pointer's register reused for the result */
#include <stdio.h>

struct S {
    char pad[8000];
    unsigned char b;
    short h;
    int w;
    long q;
    char pad2[40000];
    struct S *next;
    double d;
};

static struct S a, c;

unsigned byte_of(struct S *s) { return s->b; }
int half_of(struct S *s) { return s->h; }
int word_of(struct S *s) { return s->w; }
long quad_of(struct S *s) { return s->q; }
struct S *next_of(struct S *s) { return s->next; }
double dbl_of(struct S *s) { return s->d; }
unsigned chain(struct S *s) { return s->next->next->b; }

int main(void)
{
    a.b = 200;
    a.h = -1234;
    a.w = 567890;
    a.q = 123456789012L;
    a.d = 2.5;
    a.next = &c;
    c.next = &a;
    c.b = 7;
    printf("%u %d %d %ld %d %.1f %u %u\n", byte_of(&a), half_of(&a), word_of(&a), quad_of(&a), next_of(&a) == &c,
           dbl_of(&a), chain(&a), chain(&c));
    return 0;
}
