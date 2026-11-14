/* Complex functions. The compiler keeps a complex number as a pair of parts. */
#include <complex.h>
#include <math.h>

double creal(double _Complex z) { return __real__ z; }
double cimag(double _Complex z) { return __imag__ z; }
double cabs(double _Complex z) { return hypot(__real__ z, __imag__ z); }
double carg(double _Complex z) { return atan2(__imag__ z, __real__ z); }
double _Complex conj(double _Complex z) { return __builtin_complex(__real__ z, -__imag__ z); }
double _Complex cproj(double _Complex z) { return z; }

double _Complex cexp(double _Complex z)
{
    double e = exp(__real__ z), y = __imag__ z;
    if (y == 0)
        return __builtin_complex(e, y);
    return __builtin_complex(e * cos(y), e * sin(y));
}

double _Complex clog(double _Complex z)
{
    return __builtin_complex(log(cabs(z)), carg(z));
}

double _Complex csqrt(double _Complex z)
{
    double a = __real__ z, b = __imag__ z, r = hypot(a, b), s, t;
    if (r == 0)
        return __builtin_complex(0.0, b);
    if (a >= 0) {
        s = sqrt((r + a) * 0.5);
        t = b / (2 * s);
    } else {
        t = sqrt((r - a) * 0.5);
        if (b < 0)
            t = -t;
        s = b / (2 * t);
    }
    return __builtin_complex(s, t);
}

double _Complex cpow(double _Complex a, double _Complex b)
{
    if (__real__ a == 0 && __imag__ a == 0)
        return __builtin_complex(__real__ b == 0 && __imag__ b == 0 ? 1.0 : 0.0, 0.0);
    return cexp(b * clog(a));
}

float crealf(float _Complex z) { return __real__ z; }
float cimagf(float _Complex z) { return __imag__ z; }
float cabsf(float _Complex z) { return (float)hypot(__real__ z, __imag__ z); }
float cargf(float _Complex z) { return (float)atan2(__imag__ z, __real__ z); }
float _Complex conjf(float _Complex z) { return __builtin_complex(__real__ z, -__imag__ z); }
float _Complex cexpf(float _Complex z)
{
    double e = exp(__real__ z), y = __imag__ z;
    return __builtin_complex((float)(e * cos(y)), (float)(e * sin(y)));
}
