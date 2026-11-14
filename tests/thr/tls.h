extern _Thread_local int shared_counter;
extern __thread long zero_long;
extern thread_local int other_value;
extern _Thread_local __attribute__((aligned(64))) long other_aligned;
void other_add(int n);
int other_get(void);
int *other_addr(void);
int other_counter(void);
