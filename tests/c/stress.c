/* Register pressure, spilling, large frames, deep expressions. */
#include <stdio.h>
#include <string.h>

int id(int x) { return x; }
double did(double x) { return x; }

int pressure(int s)
{
    int a = id(s + 1), b = id(s + 2), c = id(s + 3), d = id(s + 4), e = id(s + 5), f = id(s + 6), g = id(s + 7);
    int h = id(s + 8), i = id(s + 9), j = id(s + 10), k = id(s + 11), l = id(s + 12), m = id(s + 13), n = id(s + 14);
    int o = id(s + 15), p = id(s + 16), q = id(s + 17), r = id(s + 18), t = id(s + 19), u = id(s + 20), v = id(s + 21);
    int w = id(s + 22), x = id(s + 23), y = id(s + 24), z = id(s + 25), aa = id(s + 26), bb = id(s + 27), cc = id(s + 28);
    int sum = 0, n2;
    for (n2 = 0; n2 < 3; n2++) {
        sum += a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13 +
               n * 14 + o * 15 + p * 16 + q * 17 + r * 18 + t * 19 + u * 20 + v * 21 + w * 22 + x * 23 + y * 24 + z * 25 +
               aa * 26 + bb * 27 + cc * 28;
        sum += id(n2);
        a++; cc--; m += 2;
    }
    return sum + a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + t + u + v + w + x + y + z + aa +
           bb + cc;
}

double fpressure(double s)
{
    double a = did(s + 1), b = did(s + 2), c = did(s + 3), d = did(s + 4), e = did(s + 5), f = did(s + 6), g = did(s + 7);
    double h = did(s + 8), i = did(s + 9), j = did(s + 10), k = did(s + 11), l = did(s + 12), m = did(s + 13);
    double n = did(s + 14), o = did(s + 15), p = did(s + 16), q = did(s + 17), r = did(s + 18), t = did(s + 19);
    double u = did(s + 20), v = did(s + 21), w = did(s + 22), x = did(s + 23), y = did(s + 24);
    double all = a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13;
    all += did(1.0);
    return all + n + o + p + q + r + t + u + v + w + x + y + a * b - c / d + (e - f) * (g + h) - i * j + k / l;
}

long big_frame(int n)
{
    long big[5000];
    char pad[70000];
    int i;
    long s = 0;
    for (i = 0; i < 5000; i++)
        big[i] = (long)i * n;
    memset(pad, 1, sizeof pad);
    for (i = 0; i < 5000; i += 7)
        s += big[i] + pad[i * 13];
    return s + id(pad[69999]);
}

int deep(int x)
{
    return ((((x + 1) * (x + 2)) - ((x + 3) * (x + 4))) + (((x + 5) * (x + 6)) - ((x + 7) * (x + 8)))) *
           ((((x - 1) * (x - 2)) + ((x - 3) * (x - 4))) - (((x - 5) * (x - 6)) + ((x - 7) * (x - 8)))) +
           (id(x) ? id(x + 1) ? id(x + 2) ? 7 : 8 : 9 : 10);
}

int args12(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l)
{
    return a - b + c - d + e - f + g - h + i - j + k - l;
}

double dargs(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j, int k,
             long l, char m)
{
    return a + b + c + d + e + f + g + h + i * 2 + j * 3 + k + l + m;
}

unsigned long mix(unsigned long x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdUL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53UL;
    x ^= x >> 33;
    return x;
}

int swap_loop(int n)
{
    int a = 1, b = 2, c = 3, t, i;
    for (i = 0; i < n; i++) {
        t = a;
        a = b;
        b = c;
        c = t;
    }
    return a * 100 + b * 10 + c;
}

int main(void)
{
    char buf[100];
    printf("%d\n", pressure(10));
    printf("%.3f\n", fpressure(1.5));
    printf("%ld\n", big_frame(3));
    printf("%d %d\n", deep(3), deep(0));
    printf("%d\n", args12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
    printf("%.1f\n", dargs(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12L, 13));
    printf("%lx %lx\n", mix(1), mix(0x123456789abcdefUL));
    printf("%d %d %d\n", swap_loop(0), swap_loop(1), swap_loop(5));
    sprintf(buf, "%5.2f|%-8s|%08.3f|%+d|%x|%o|%c|%%|%10.4e|%g", 3.14159, "ab", -2.5, 7, 0xbeef, 8, 'z', 12345.6789, 1e-5);
    puts(buf);
    return 0;
}
