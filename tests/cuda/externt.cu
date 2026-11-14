#include <stdio.h>
template <int N, class T> T addn(T x) { return x + (T)N; }
template <class T> T twice(T x);
template <class T> T twice(T x) { return x * 2; }
extern template int addn<3, int>(int x);
extern template float twice<float>(float x);
extern template int twice<int>(int x);
int use(int v) { return addn<3, int>(v) + (int)twice<float>(1.5f) + twice<int>(v) + addn<4, int>(1); }
template int twice<int>(int x);

template int addn<3, int>(int x);
template float twice<float>(float x);
int main() { printf("%d\n", use(10)); return 0; }

