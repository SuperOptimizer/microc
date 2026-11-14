#include "tls.h"

thread_local int other_value = 1000;
_Thread_local __attribute__((aligned(64))) long other_aligned;

void other_add(int n) { other_value += n; }
int other_get(void) { return other_value; }
int *other_addr(void) { return &other_value; }
int other_counter(void) { return shared_counter; }
