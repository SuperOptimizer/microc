#ifndef _MALLOC_H
#define _MALLOC_H
#include <stdlib.h>
#define M_TRIM_THRESHOLD (-1)
#define M_TOP_PAD (-2)
#define M_MMAP_THRESHOLD (-3)
#define M_MMAP_MAX (-4)
#define M_ARENA_MAX (-8)
size_t malloc_usable_size(void *p);
int malloc_trim(size_t pad);
int mallopt(int param, int value); /* accepted, without effect */
void *pvalloc(size_t n);
/* what the allocator holds; this one keeps no count, so every field reads 0 */
struct mallinfo2 {
    size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};
struct mallinfo2 mallinfo2(void);
#endif
