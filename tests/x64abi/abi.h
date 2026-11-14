/* Calling convention test for x86-64: abi_a.c calls, abi_b.c is called; tests/x64abi.sh builds one with
   mcc and the other with the C compiler of the x86-64 machine, both ways round. */
typedef struct { int a; } S1;
typedef struct { long a, b; } S2;
typedef struct { long a, b, c; } S3;
typedef struct { double a; } S4;
typedef struct { double a, b; } S5;
typedef struct { float a, b; } S6;
typedef struct { float a, b, c; } S7;
typedef struct { int a; float b; } S8;
typedef struct { double a; int b; } S9;
typedef struct { long a; double b; } S10;
typedef struct { char a[3]; } S11;
typedef struct { char a[17]; } S12;
typedef struct { float a; int b; double c; } S13;
typedef struct { struct { float x, y; } p; float a; } S14;
typedef union { double a; long b; } S15;
typedef struct { char a; short b; char c; } S16;
typedef struct { float a, b, c, d; } S17;
typedef struct { double a, b, c; } S18;
typedef struct { char a[9]; } S19;
typedef struct { int a : 5; unsigned b : 20; long c : 40; } S20;
typedef struct { float a; double b; } S21;
typedef struct { short a[5]; } S22;
typedef struct { long a; char b; } S23;
typedef struct { double a; float b[2]; } S24;

#define TYPES(X) X(S1) X(S2) X(S3) X(S4) X(S5) X(S6) X(S7) X(S8) X(S9) X(S10) X(S11) X(S12) X(S13) X(S14) \
    X(S15) X(S16) X(S17) X(S18) X(S19) X(S20) X(S21) X(S22) X(S23) X(S24)

/* a value of the type made from a seed, and a number that depends on every byte that matters */
#define DECL(T) \
    T make_##T(long seed); \
    long sum_##T(T s); \
    long take0_##T(T s); \
    long take5_##T(int a, int b, int c, int d, int e, T s, long z); \
    long take7d_##T(double a, double b, double c, double d, double e, double f, double g, T s, double z); \
    long takemix_##T(T s, int a, double b, T t, long c, T u, float d, T v, T w); \
    long takestk_##T(long a, long b, long c, long d, long e, long f, double g, double h, double i, double j, double k, double l, double m, double n, T s, char y, T t); \
    T ret_##T(long seed); \
    T pass_##T(int a, T s, double b); \
    long var_##T(int n, ...);
TYPES(DECL)

long var_mixed(const char *fmt, ...);
long many(char a, short b, int c, long d, unsigned char e, unsigned short f, unsigned g, unsigned long h, float i, double j,
          signed char k, long l, long m, float n, double o, double p, double q, double r, double s, double t, int u, float v);
long add128(__int128 a, long b, __int128 c, int d, int e, int f, __int128 g);
void fill(void *p, int size, long seed);
long hash(const void *p, int size);
