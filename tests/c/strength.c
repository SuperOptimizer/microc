#include <stdio.h>
int d2(int x){return x/2;} int d8(int x){return x/8;} int r8(int x){return x%8;} long dl(long x){return x/1024;} long rl(long x){return x%4096;}
int m33(int x){return x*33;} int m7(int x){return x*7;} long m24(long x){return x*24;} unsigned m56(unsigned x){return x*56;} int m100(int x) {return x*100;}
int mneg(int x){return x*-3;}
int main(void){
  int v[] = {0,1,-1,2,-2,7,-7,8,-8,9,-9,1023,-1023,1024,-1025,2147483647,-2147483647-1, 123456789,-123456789};
  unsigned i;
  for (i=0;i<sizeof v/sizeof v[0];i++) {
    int x=v[i]; long l = (long)x * 1000003L;
    printf("%d %d %d %d %ld %ld %d %d %ld %u %d %d\n", x, d2(x), d8(x), r8(x), dl(l), rl(l), m33(x), m7(x), m24(l), m56((unsigned)x), m100(x), mneg(x));
  }
  return 0;
}
